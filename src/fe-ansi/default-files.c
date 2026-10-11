/*
 default-files.c : the files erssi ships into ~/.erssi (themes, startup)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/default-files.h>

#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_FILES_SHA256 "default-files.sha256"

DEFAULT_FILES_REC *default_files_open(const char *dir, const char *const known[][2])
{
	DEFAULT_FILES_REC *rec;
	char *path, *contents, **lines, **line;

	rec = g_new0(DEFAULT_FILES_REC, 1);
	rec->dir = g_strdup(dir);
	rec->known = known;
	rec->hashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	path = g_strdup_printf("%s/" DEFAULT_FILES_SHA256, dir);
	if (g_file_get_contents(path, &contents, NULL, NULL)) {
		/* "<sha256>  <name>", as sha256sum writes it */
		lines = g_strsplit(contents, "\n", -1);
		for (line = lines; *line != NULL; line++) {
			if (strlen(*line) > 66 && (*line)[64] == ' ' && (*line)[65] == ' ')
				g_hash_table_replace(rec->hashes, g_strdup(*line + 66),
				                     g_strndup(*line, 64));
		}
		g_strfreev(lines);
		g_free(contents);
	}
	g_free(path);
	return rec;
}

static void default_hash_record(DEFAULT_FILES_REC *rec, const char *name, const char *hash)
{
	const char *old = g_hash_table_lookup(rec->hashes, name);

	if (old == NULL || strcmp(old, hash) != 0) {
		g_hash_table_replace(rec->hashes, g_strdup(name), g_strdup(hash));
		rec->hashes_changed = TRUE;
	}
}

static gboolean default_hash_known(DEFAULT_FILES_REC *rec, const char *name, const char *hash)
{
	int i;

	for (i = 0; rec->known[i][0] != NULL; i++) {
		if (strcmp(rec->known[i][0], name) == 0 &&
		    strcmp(rec->known[i][1], hash) == 0)
			return TRUE;
	}
	return FALSE;
}

/* A regular file opened for reading, never through a symbolic link:
 * opened once with O_NOFOLLOW and checked on that descriptor, so a link
 * swapped in after the lstat() is not followed; O_NONBLOCK, so a FIFO
 * swapped in does not hang the start (it is refused by the fstat()) */
static int open_regular(const char *path)
{
	struct stat st;
	int fd;

	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	return fd;
}

/* reads fd to its end into buf and/or sum, then closes it */
static gboolean read_all(int fd, GString *buf, GChecksum *sum)
{
	char chunk[8192];
	ssize_t n;

	while ((n = read(fd, chunk, sizeof(chunk))) > 0 ||
	       (n < 0 && errno == EINTR)) {
		if (n > 0 && buf != NULL)
			g_string_append_len(buf, chunk, n);
		if (n > 0 && sum != NULL)
			g_checksum_update(sum, (guchar *) chunk, n);
	}
	close(fd);
	return n == 0;
}

gboolean default_files_read_regular(const char *path, char **data, gsize *len)
{
	GString *buf;
	int fd;

	fd = open_regular(path);
	if (fd < 0)
		return FALSE;
	buf = g_string_new(NULL);
	if (!read_all(fd, buf, NULL)) {
		g_string_free(buf, TRUE);
		return FALSE;
	}
	*len = buf->len;
	*data = g_string_free(buf, FALSE);
	return TRUE;
}

/* the sha256 of a regular file (as default_files_read_regular() reads it),
 * streamed: a large file is not held in memory; NULL when it is not read */
static char *hash_regular(const char *path)
{
	GChecksum *sum;
	char *hash = NULL;
	int fd;

	fd = open_regular(path);
	if (fd < 0)
		return NULL;
	sum = g_checksum_new(G_CHECKSUM_SHA256);
	if (read_all(fd, NULL, sum))
		hash = g_strdup(g_checksum_get_string(sum));
	g_checksum_free(sum);
	return hash;
}

/* Replaces path with data: a new file in the same directory with the given
 * mode from the start (no window with the umask's mode), written out to the
 * disk, then renamed over the old one - never a half-written file under
 * path, and a link at path is replaced, not followed */
static gboolean write_file_mode(const char *path, const char *data, gsize len, int mode)
{
	char *tmp = g_strconcat(path, ".XXXXXX", NULL);
	gsize done = 0;
	int fd, ok;

	fd = g_mkstemp_full(tmp, O_WRONLY | O_CLOEXEC, mode);
	if (fd < 0) {
		g_free(tmp);
		return FALSE;
	}
	ok = fchmod(fd, mode) == 0;
	while (ok && done < len) {
		ssize_t n = write(fd, data + done, len - done);
		if (n < 0 && errno == EINTR)
			continue;
		ok = n > 0;
		if (ok)
			done += n;
	}
	ok = ok && fsync(fd) == 0;
	ok = close(fd) == 0 && ok;
	ok = ok && rename(tmp, path) == 0;
	if (!ok)
		unlink(tmp);
	g_free(tmp);
	return ok;
}

DEFAULT_FILE_RESULT default_files_update(DEFAULT_FILES_REC *rec, const char *shipped_path,
                                         const char *name, gboolean theme)
{
	DEFAULT_FILE_RESULT result = DEFAULT_FILE_SKIPPED;
	char *shipped, *dst_path, *shipped_hash, *current_hash;
	const char *recorded;
	gsize shipped_len;
	struct stat st;

	if (!g_file_get_contents(shipped_path, &shipped, &shipped_len, NULL))
		return DEFAULT_FILE_SKIPPED;
	shipped_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *) shipped, shipped_len);
	recorded = g_hash_table_lookup(rec->hashes, name);
	dst_path = g_strdup_printf("%s/%s", rec->dir, name);

	if (g_lstat(dst_path, &st) != 0) {
		if (errno == ENOENT && write_file_mode(dst_path, shipped, shipped_len, 0600)) {
			default_hash_record(rec, name, shipped_hash);
			result = DEFAULT_FILE_COPIED;
		}
	} else if (S_ISREG(st.st_mode) &&
	           (current_hash = hash_regular(dst_path)) != NULL) {
		if (strcmp(current_hash, shipped_hash) == 0) {
			default_hash_record(rec, name, shipped_hash);
			result = DEFAULT_FILE_CURRENT;
		} else if ((recorded != NULL && strcmp(current_hash, recorded) == 0) ||
		           default_hash_known(rec, name, current_hash)) {
			/* not changed since erssi installed it: the new version */
			if (write_file_mode(dst_path, shipped, shipped_len, st.st_mode & 07777)) {
				default_hash_record(rec, name, shipped_hash);
				rec->themes_changed |= theme;
				result = DEFAULT_FILE_UPDATED;
			}
		} else {
			/* the user's own version: kept, with a note once per
			 * shipped version (also the first time, without a record) */
			if (recorded == NULL || strcmp(recorded, shipped_hash) != 0) {
				char *note = g_strdup_printf(
					"%s: kept as you changed it; the version shipped "
					"with erssi is %s", dst_path, shipped_path);
				char **parts = g_strsplit(note, "%", -1);

				/* printed as text: a % in a path is not a colour code */
				rec->notes = g_slist_append(rec->notes, g_strjoinv("%%", parts));
				g_strfreev(parts);
				g_free(note);
			}
			default_hash_record(rec, name, shipped_hash);
			result = DEFAULT_FILE_KEPT;
		}
		g_free(current_hash);
	}
	g_free(dst_path);
	g_free(shipped_hash);
	g_free(shipped);
	return result;
}

void default_files_close(DEFAULT_FILES_REC *rec)
{
	GString *out;
	GList *names, *tmp;
	char *path;

	if (rec->hashes_changed) {
		out = g_string_new(NULL);
		names = g_list_sort(g_hash_table_get_keys(rec->hashes), (GCompareFunc) strcmp);
		for (tmp = names; tmp != NULL; tmp = tmp->next) {
			g_string_append_printf(out, "%s  %s\n",
			                       (char *) g_hash_table_lookup(rec->hashes, tmp->data),
			                       (char *) tmp->data);
		}
		g_list_free(names);
		/* 0600 from the start: no moment with the umask's mode */
		path = g_strdup_printf("%s/" DEFAULT_FILES_SHA256, rec->dir);
		write_file_mode(path, out->str, out->len, 0600);
		g_free(path);
		g_string_free(out, TRUE);
	}
	g_hash_table_destroy(rec->hashes);
	g_slist_free_full(rec->notes, g_free);
	g_free(rec->dir);
	g_free(rec);
}
