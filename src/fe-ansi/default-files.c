/*
 default-files.c : the files erssi ships into ~/.erssi (themes, startup)

    Copyright (C) 2026 the Shellter team

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
#include <irssi/src/core/safe-file.h>
#include <irssi/src/fe-ansi/default-files.h>

#include <glib/gstdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_FILES_SHA256 "default-files.sha256"

DEFAULT_FILES_REC *default_files_open(const char *dir, const char *const known[][2])
{
	DEFAULT_FILES_REC *rec;
	char *path, *contents, **lines, **line;
	gsize len;

	rec = g_new0(DEFAULT_FILES_REC, 1);
	rec->dir = g_strdup(dir);
	rec->known = known;
	rec->hashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	path = g_strdup_printf("%s/" DEFAULT_FILES_SHA256, dir);
	/* like the files: not through a link, not a FIFO, and not a huge file;
	 * a list that is not read is no record (files are then kept, and the
	 * list written at the end replaces it) */
	if (safe_file_read(path, 0, DEFAULT_FILES_LIST_MAX, &contents, &len)) {
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

/* reads fd to its end into sum, then closes it */
static gboolean hash_all(int fd, GChecksum *sum)
{
	char chunk[8192];
	ssize_t n;

	while ((n = read(fd, chunk, sizeof(chunk))) > 0 ||
	       (n < 0 && errno == EINTR)) {
		if (n > 0)
			g_checksum_update(sum, (guchar *) chunk, n);
	}
	close(fd);
	return n == 0;
}

/* the sha256 of a regular file in ~/.erssi, never through a symbolic link
 * (one the user made is the user's own version, left alone), streamed: a
 * large file is not held in memory; NULL when it is not read */
static char *hash_regular(const char *path)
{
	GChecksum *sum;
	char *hash = NULL;
	int fd;

	fd = safe_file_open_read(path, 0, NULL);
	if (fd < 0)
		return NULL;
	sum = g_checksum_new(G_CHECKSUM_SHA256);
	if (hash_all(fd, sum))
		hash = g_strdup(g_checksum_get_string(sum));
	g_checksum_free(sum);
	return hash;
}

DEFAULT_FILE_RESULT default_files_update(DEFAULT_FILES_REC *rec, const char *shipped_path,
                                         const char *name, gboolean theme)
{
	DEFAULT_FILE_RESULT result = DEFAULT_FILE_SKIPPED;
	char *shipped, *dst_path, *shipped_hash, *current_hash;
	const char *recorded;
	gsize shipped_len;
	struct stat st;

	/* the installation's file: a link is followed (themes installed as
	 * links, by stow or a package of the user's own), a FIFO or device is
	 * not read */
	if (!safe_file_read(shipped_path, SAFE_FILE_FOLLOW, DEFAULT_FILES_SHIPPED_MAX,
	                    &shipped, &shipped_len))
		return DEFAULT_FILE_SKIPPED;
	shipped_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *) shipped, shipped_len);
	recorded = g_hash_table_lookup(rec->hashes, name);
	dst_path = g_strdup_printf("%s/%s", rec->dir, name);

	if (g_lstat(dst_path, &st) != 0) {
		if (errno == ENOENT && safe_file_write(dst_path, shipped, shipped_len, 0600)) {
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
			if (safe_file_write(dst_path, shipped, shipped_len, st.st_mode & 07777)) {
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
		safe_file_write(path, out->str, out->len, 0600);
		g_free(path);
		g_string_free(out, TRUE);
	}
	g_hash_table_destroy(rec->hashes);
	g_slist_free_full(rec->notes, g_free);
	g_free(rec->dir);
	g_free(rec);
}
