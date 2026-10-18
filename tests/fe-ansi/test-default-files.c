/*
 test-default-files.c : the update of the files erssi ships into ~/.erssi

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

/* Every test works in its own temporary directory (never in the home):
 * D stands for ~/.erssi, S is the file erssi ships. */

#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/default-files.h>

#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#define SHIPPED "shipped v2\n"
#define OLD_V1_SHA256 "443454f499ac3a53541d9059e33780a4627613e23caebb9d7b2137736e198583"

/* the versions erssi ever shipped: "old v1\n" under t.theme */
static const char *const KNOWN[][2] = {
	{ "t.theme", OLD_V1_SHA256 },
	{ NULL, NULL }
};

typedef struct {
	char *root;	/* removed after the test */
	char *dir;	/* D, ~/.erssi */
	char *shipped;	/* S */
} Fixture;

/* The hash list must never exist with another mode, not even between a
 * write and a chmod(): every chmod() of the unit comes through here. */
static gboolean hash_list_had_other_mode;

int chmod(const char *path, mode_t mode)
{
	struct stat st;

	if (g_str_has_suffix(path, "/default-files.sha256") && stat(path, &st) == 0 &&
	    (st.st_mode & 07777) != 0600)
		hash_list_had_other_mode = TRUE;
	return fchmodat(AT_FDCWD, path, mode, 0);
}

/* Every file the unit replaces is a temporary file in the same directory,
 * written out to the disk, then renamed: its rename() (and on Linux its
 * fsync()) come through here while watch_renames is set. */
static gboolean watch_renames;
static int renames, bad_renames, syncs;
#ifdef __linux__
static gboolean fail_fsync; /* fsync() fails with EIO */

int fsync(int fd)
{
	syncs++;
	if (fail_fsync) {
		errno = EIO;
		return -1;
	}
	return syscall(SYS_fsync, fd);
}
#endif

int rename(const char *oldpath, const char *newpath)
{
	if (watch_renames) {
		char *olddir = g_path_get_dirname(oldpath);
		char *newdir = g_path_get_dirname(newpath);

		renames++;
		if (strcmp(olddir, newdir) != 0)
			bad_renames++;
#ifdef __linux__
		if (syncs == 0)
			bad_renames++;
		syncs = 0;
#endif
		g_free(olddir);
		g_free(newdir);
	}
	return renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath);
}

static void watch_start(void)
{
	watch_renames = TRUE;
	renames = bad_renames = syncs = 0;
}

static void watch_stop(int expected_renames)
{
	watch_renames = FALSE;
	g_assert_cmpint(renames, ==, expected_renames);
	g_assert_cmpint(bad_renames, ==, 0);
}

static void remove_tree(const char *path)
{
	struct stat st;
	GDir *dir;
	const char *name;

	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		g_chmod(path, 0700);
		dir = g_dir_open(path, 0, NULL);
		if (dir != NULL) {
			while ((name = g_dir_read_name(dir)) != NULL) {
				char *child = g_build_filename(path, name, NULL);

				remove_tree(child);
				g_free(child);
			}
			g_dir_close(dir);
		}
		g_rmdir(path);
	} else {
		g_unlink(path);
	}
}

static char *path_in(const char *dir, const char *name)
{
	return g_build_filename(dir, name, NULL);
}

static void write_file(const char *path, const char *data)
{
	GError *error = NULL;

	g_file_set_contents(path, data, -1, &error);
	g_assert_no_error(error);
}

/* the file's contents, or NULL; free with g_free() */
static char *read_file(const char *path)
{
	char *data;

	if (!g_file_get_contents(path, &data, NULL, NULL))
		return NULL;
	return data;
}

static char *sha256(const char *data)
{
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, data, -1);
}

static int file_mode(const char *path)
{
	struct stat st;

	g_assert_cmpint(lstat(path, &st), ==, 0);
	return st.st_mode & 07777;
}

static gint cmp_names(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

/* D's entries, sorted and joined by spaces - a left temporary file shows */
static char *dir_entries(const char *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	const char *name;
	char *out;

	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_dir_close(dir);
	g_ptr_array_sort(names, cmp_names);
	g_ptr_array_add(names, NULL);
	out = g_strjoinv(" ", (char **) names->pdata);
	g_ptr_array_free(names, TRUE);
	return out;
}

static void fixture_dir_setup(Fixture *f, const char *subdir)
{
	char *shipped_dir;

	f->root = g_dir_make_tmp("default-files-XXXXXX", NULL);
	g_assert_nonnull(f->root);
	if (subdir == NULL) {
		f->dir = g_strdup(f->root);
	} else {
		f->dir = path_in(f->root, subdir);
		g_assert_cmpint(g_mkdir(f->dir, 0700), ==, 0);
	}
	shipped_dir = g_strconcat(f->root, ".shipped", NULL);
	g_assert_cmpint(g_mkdir(shipped_dir, 0755), ==, 0);
	f->shipped = path_in(shipped_dir, "t.theme");
	write_file(f->shipped, SHIPPED);
	g_free(shipped_dir);
}

static void fixture_setup(Fixture *f, gconstpointer data)
{
	fixture_dir_setup(f, data);
}

static void fixture_teardown(Fixture *f, gconstpointer data)
{
	char *shipped_dir = g_path_get_dirname(f->shipped);

	remove_tree(f->root);
	remove_tree(shipped_dir);
	g_free(shipped_dir);
	g_free(f->root);
	g_free(f->dir);
	g_free(f->shipped);
}

static void test_user_changed_kept(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char *content;

	write_file(dst, "mine\n");
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_KEPT);
	content = read_file(dst);
	g_assert_cmpstr(content, ==, "mine\n");
	g_free(content);
	g_assert_cmpuint(g_slist_length(rec->notes), ==, 1);
	g_assert_nonnull(strstr(rec->notes->data, "kept as you changed it"));
	g_assert_nonnull(strstr(rec->notes->data, dst));
	g_assert_nonnull(strstr(rec->notes->data, f->shipped));
	g_assert_false(rec->themes_changed);
	default_files_close(rec);

	/* same shipped version again: no new note */
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_KEPT);
	g_assert_null(rec->notes);
	default_files_close(rec);
	content = read_file(dst);
	g_assert_cmpstr(content, ==, "mine\n");
	g_free(content);

	/* a known hash counts only under its own name */
	g_free(dst);
	dst = path_in(f->dir, "u.theme");
	write_file(dst, "old v1\n");
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "u.theme", TRUE), ==,
	                DEFAULT_FILE_KEPT);
	default_files_close(rec);
	content = read_file(dst);
	g_assert_cmpstr(content, ==, "old v1\n");
	g_free(content);
	g_free(dst);
}

static void test_missing_copied_0600(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char *list = path_in(f->dir, "default-files.sha256");
	char *content, *line, *entries, *hash;
	mode_t old_umask;

	g_chmod(f->shipped, 0644);
	old_umask = umask(0);
	hash_list_had_other_mode = FALSE;
	watch_start();
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_COPIED);
	g_assert_true(rec->hashes_changed);
	g_assert_null(rec->notes);
	default_files_close(rec);
	watch_stop(2);	/* t.theme, the hash list */
	umask(old_umask);

	content = read_file(dst);
	g_assert_cmpstr(content, ==, SHIPPED);
	g_free(content);
	g_assert_cmpint(file_mode(dst), ==, 0600);

	hash = sha256(SHIPPED);
	line = g_strdup_printf("%s  t.theme\n", hash);
	content = read_file(list);
	g_assert_cmpstr(content, ==, line);
	g_assert_cmpint(file_mode(list), ==, 0600);
	g_assert_false(hash_list_had_other_mode);

	entries = dir_entries(f->dir);
	g_assert_cmpstr(entries, ==, "default-files.sha256 t.theme");

	g_free(entries);
	g_free(content);
	g_free(line);
	g_free(hash);
	g_free(list);
	g_free(dst);
}

/* a hash list that cannot be replaced (a directory there): the copy is made,
 * the directory stays and no temporary file is left behind */
static void test_hash_list_write_fails_cleanly(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *list = path_in(f->dir, "default-files.sha256");
	char *entries;

	g_assert_cmpint(g_mkdir(list, 0700), ==, 0);
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_COPIED);
	default_files_close(rec);
	g_assert_true(g_file_test(list, G_FILE_TEST_IS_DIR));
	entries = dir_entries(f->dir);
	g_assert_cmpstr(entries, ==, "default-files.sha256 t.theme");
	g_free(entries);
	g_free(list);
}

/* D/t.theme unchanged since erssi recorded it ("recorded v0\n") */
static void write_recorded(Fixture *f)
{
	char *dst = path_in(f->dir, "t.theme");
	char *list = path_in(f->dir, "default-files.sha256");
	char *hash = sha256("recorded v0\n");
	char *line = g_strdup_printf("%s  t.theme\n", hash);

	write_file(dst, "recorded v0\n");
	write_file(list, line);
	g_free(line);
	g_free(hash);
	g_free(list);
	g_free(dst);
}

static void assert_file(const char *dir, const char *name, const char *expected)
{
	char *path = path_in(dir, name);
	char *content = read_file(path);

	g_assert_cmpstr(content, ==, expected);
	g_free(content);
	g_free(path);
}

/* the replacing write fails (D not writable): nothing is recorded, so the
 * old file is not taken for the user's own at the next start */
static void test_replace_fails_nothing_recorded(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *list = path_in(f->dir, "default-files.sha256");
	char *list_before, *entries;

	if (geteuid() == 0) {
		g_test_skip("root writes into any directory");
		g_free(list);
		return;
	}
	write_recorded(f);
	list_before = read_file(list);
	g_assert_cmpint(g_chmod(f->dir, 0500), ==, 0);
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_SKIPPED);
	g_assert_false(rec->hashes_changed);
	g_assert_false(rec->themes_changed);
	g_assert_null(rec->notes);
	default_files_close(rec);
	g_assert_cmpint(g_chmod(f->dir, 0700), ==, 0);

	assert_file(f->dir, "t.theme", "recorded v0\n");
	assert_file(f->dir, "default-files.sha256", list_before);
	entries = dir_entries(f->dir);
	g_assert_cmpstr(entries, ==, "default-files.sha256 t.theme");
	g_free(entries);
	g_free(list_before);
	g_free(list);
}

/* a failing fsync(): the file is not replaced, no temporary file is left */
static void test_fsync_fails_not_replaced(Fixture *f, gconstpointer data)
{
#ifdef __linux__
	DEFAULT_FILES_REC *rec;
	char *entries;

	write_recorded(f);
	rec = default_files_open(f->dir, KNOWN);
	fail_fsync = TRUE;
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_SKIPPED);
	fail_fsync = FALSE;
	g_assert_false(rec->hashes_changed);
	g_assert_false(rec->themes_changed);
	default_files_close(rec);

	assert_file(f->dir, "t.theme", "recorded v0\n");
	entries = dir_entries(f->dir);
	g_assert_cmpstr(entries, ==, "default-files.sha256 t.theme");
	g_free(entries);
#else
	g_test_skip("fsync() is observed on Linux only");
#endif
}

/* a theme replaced, then startup (not a theme) replaced: the themes are
 * still to be reloaded */
static void test_themes_changed_sticks(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *shipped_dir = g_path_get_dirname(f->shipped);
	char *shipped_startup = path_in(shipped_dir, "startup");
	char *list = path_in(f->dir, "default-files.sha256");
	char *theme_hash = sha256("recorded v0\n");
	char *startup_hash = sha256("startup v1\n");
	char *lines = g_strdup_printf("%s  startup\n%s  t.theme\n", startup_hash, theme_hash);
	char *dst;

	write_file(shipped_startup, "startup v2\n");
	dst = path_in(f->dir, "t.theme");
	write_file(dst, "recorded v0\n");
	g_free(dst);
	dst = path_in(f->dir, "startup");
	write_file(dst, "startup v1\n");
	g_free(dst);
	write_file(list, lines);

	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_UPDATED);
	g_assert_cmpint(default_files_update(rec, shipped_startup, "startup", FALSE), ==,
	                DEFAULT_FILE_UPDATED);
	g_assert_true(rec->themes_changed);
	default_files_close(rec);
	assert_file(f->dir, "startup", "startup v2\n");
	assert_file(f->dir, "t.theme", SHIPPED);

	g_free(lines);
	g_free(startup_hash);
	g_free(theme_hash);
	g_free(list);
	g_free(shipped_startup);
	g_free(shipped_dir);
}

/* D/t.theme matches a hash the list records, but the list is not read
 * (a link, FIFO or too large): no record, so the file is kept with a note,
 * and the list written at the end is a regular 0600 file */
static void assert_list_not_read(Fixture *f)
{
	DEFAULT_FILES_REC *rec;
	char *list = path_in(f->dir, "default-files.sha256");
	char *hash = sha256(SHIPPED);
	char *line = g_strdup_printf("%s  t.theme\n", hash);
	struct stat st;

	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpuint(g_hash_table_size(rec->hashes), ==, 0);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_KEPT);
	g_assert_cmpuint(g_slist_length(rec->notes), ==, 1);
	default_files_close(rec);
	assert_file(f->dir, "t.theme", "recorded v0\n");

	g_assert_cmpint(lstat(list, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpint(st.st_mode & 07777, ==, 0600);
	assert_file(f->dir, "default-files.sha256", line);
	g_free(line);
	g_free(hash);
	g_free(list);
}

static void test_list_symlink_no_record(Fixture *f, gconstpointer data)
{
	char *list = path_in(f->dir, "default-files.sha256");
	char *target = path_in(f->dir, "elsewhere");
	char *hash = sha256("recorded v0\n");
	char *line = g_strdup_printf("%s  t.theme\n", hash);

	write_recorded(f);
	g_assert_cmpint(g_rename(list, target), ==, 0);
	g_assert_cmpint(symlink(target, list), ==, 0);
	assert_list_not_read(f);
	/* the link was replaced, not written through */
	assert_file(f->dir, "elsewhere", line);
	g_free(line);
	g_free(hash);
	g_free(target);
	g_free(list);
}

static void test_list_fifo_no_record(Fixture *f, gconstpointer data)
{
	if (g_test_subprocess()) {
		char *list = path_in(f->dir, "default-files.sha256");

		write_recorded(f);
		g_assert_cmpint(g_unlink(list), ==, 0);
		g_assert_cmpint(mkfifo(list, 0600), ==, 0);
		assert_list_not_read(f);
		g_free(list);
		return;
	}
	g_test_trap_subprocess(NULL, 2 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
}

/* the list with its record, padded with comment lines to size bytes */
static void write_padded_list(Fixture *f, gsize size)
{
	char *list = path_in(f->dir, "default-files.sha256");
	char *hash = sha256("recorded v0\n");
	GString *out = g_string_new(NULL);

	g_string_append_printf(out, "%s  t.theme\n", hash);
	while (out->len < size)
		g_string_append_c(out, out->len % 100 == 99 ? '\n' : '#');
	g_assert_true(g_file_set_contents(list, out->str, out->len, NULL));
	g_string_free(out, TRUE);
	g_free(hash);
	g_free(list);
}

static void test_list_oversized_no_record(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;

	/* at the limit: read */
	write_recorded(f);
	write_padded_list(f, DEFAULT_FILES_LIST_MAX);
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpuint(g_hash_table_size(rec->hashes), ==, 1);
	default_files_close(rec);

	/* one byte more: not read */
	write_padded_list(f, DEFAULT_FILES_LIST_MAX + 1);
	assert_list_not_read(f);
}

static void test_current_untouched(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char *list = path_in(f->dir, "default-files.sha256");
	char *hash = sha256(SHIPPED);
	char *line = g_strdup_printf("%s  t.theme\n", hash);
	struct utimbuf old = { 1000000000, 1000000000 };
	struct stat before, after, list_before, list_after;

	write_file(dst, SHIPPED);
	write_file(list, line);
	g_assert_cmpint(utime(dst, &old), ==, 0);
	g_assert_cmpint(utime(list, &old), ==, 0);
	g_assert_cmpint(stat(dst, &before), ==, 0);
	g_assert_cmpint(stat(list, &list_before), ==, 0);

	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_CURRENT);
	g_assert_false(rec->hashes_changed);
	g_assert_false(rec->themes_changed);
	g_assert_null(rec->notes);
	default_files_close(rec);

	g_assert_cmpint(stat(dst, &after), ==, 0);
	g_assert_cmpuint(after.st_ino, ==, before.st_ino);
	g_assert_cmpint(after.st_mtime, ==, before.st_mtime);
	/* nothing changed: the hash list is not written either */
	g_assert_cmpint(stat(list, &list_after), ==, 0);
	g_assert_cmpuint(list_after.st_ino, ==, list_before.st_ino);
	g_assert_cmpint(list_after.st_mtime, ==, list_before.st_mtime);

	g_free(line);
	g_free(hash);
	g_free(list);
	g_free(dst);
}

static void test_recorded_unchanged_updated(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char *list = path_in(f->dir, "default-files.sha256");
	char *old_hash = sha256("recorded v0\n");
	char *new_hash = sha256(SHIPPED);
	char *line = g_strdup_printf("%s  t.theme\n", old_hash);
	char *content, *entries;
	mode_t old_umask;

	write_file(dst, "recorded v0\n");
	g_assert_cmpint(g_chmod(dst, 0640), ==, 0);
	write_file(list, line);
	g_free(line);

	/* the user's mode is kept, whatever the umask */
	old_umask = umask(077);
	watch_start();
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_UPDATED);
	g_assert_true(rec->themes_changed);
	g_assert_true(rec->hashes_changed);
	g_assert_null(rec->notes);
	default_files_close(rec);
	watch_stop(2);
	umask(old_umask);

	content = read_file(dst);
	g_assert_cmpstr(content, ==, SHIPPED);
	g_free(content);
	g_assert_cmpint(file_mode(dst), ==, 0640);

	line = g_strdup_printf("%s  t.theme\n", new_hash);
	content = read_file(list);
	g_assert_cmpstr(content, ==, line);
	g_assert_cmpint(file_mode(list), ==, 0600);
	entries = dir_entries(f->dir);
	g_assert_cmpstr(entries, ==, "default-files.sha256 t.theme");

	g_free(entries);
	g_free(content);
	g_free(line);
	g_free(new_hash);
	g_free(old_hash);
	g_free(list);
	g_free(dst);
}

static void test_known_hash_updated(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char *content;

	write_file(dst, "old v1\n");
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", FALSE), ==,
	                DEFAULT_FILE_UPDATED);
	/* not a theme: no theme to reload */
	g_assert_false(rec->themes_changed);
	g_assert_null(rec->notes);
	default_files_close(rec);

	content = read_file(dst);
	g_assert_cmpstr(content, ==, SHIPPED);
	g_free(content);
	g_free(dst);
}

static void test_note_escapes_percent(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char **parts, *unescaped;

	g_assert_nonnull(strstr(f->dir, "/a%b"));
	write_file(dst, "mine\n");
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_KEPT);
	g_assert_cmpuint(g_slist_length(rec->notes), ==, 1);
	g_assert_nonnull(strstr(rec->notes->data, "/a%%b/t.theme: kept as you changed it"));

	/* every % is doubled: none is left alone */
	parts = g_strsplit(rec->notes->data, "%%", -1);
	unescaped = g_strjoinv("", parts);
	g_assert_null(strchr(unescaped, '%'));
	g_free(unescaped);
	g_strfreev(parts);

	default_files_close(rec);
	g_free(dst);
}

static void test_symlink_skipped(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char *target = path_in(f->dir, "target");
	char *list = path_in(f->dir, "default-files.sha256");
	char *hash = sha256("recorded v0\n");
	char *line = g_strdup_printf("%s  t.theme\n", hash);
	char *content;
	struct stat st;

	write_file(target, "recorded v0\n");
	write_file(list, line);
	g_assert_cmpint(symlink(target, dst), ==, 0);

	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_SKIPPED);
	g_assert_false(rec->themes_changed);
	g_assert_null(rec->notes);
	default_files_close(rec);

	g_assert_cmpint(lstat(dst, &st), ==, 0);
	g_assert_true(S_ISLNK(st.st_mode));
	content = read_file(target);
	g_assert_cmpstr(content, ==, "recorded v0\n");
	g_free(content);

	g_free(line);
	g_free(hash);
	g_free(list);
	g_free(target);
	g_free(dst);
}

static void test_directory_skipped(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");

	g_assert_cmpint(g_mkdir(dst, 0700), ==, 0);
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_SKIPPED);
	g_assert_null(rec->notes);
	default_files_close(rec);
	g_assert_true(g_file_test(dst, G_FILE_TEST_IS_DIR));
	g_free(dst);
}

static void test_unreadable_skipped(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");
	char *content;

	if (geteuid() == 0) {
		g_test_skip("root reads any file");
		g_free(dst);
		return;
	}
	write_file(dst, "old v1\n");
	g_assert_cmpint(g_chmod(dst, 0), ==, 0);
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_SKIPPED);
	g_assert_null(rec->notes);
	default_files_close(rec);
	g_assert_cmpint(file_mode(dst), ==, 0);
	g_assert_cmpint(g_chmod(dst, 0600), ==, 0);
	content = read_file(dst);
	g_assert_cmpstr(content, ==, "old v1\n");
	g_free(content);
	g_free(dst);
}

/* a FIFO where the file was must not hang the start */
static void test_fifo_skipped(Fixture *f, gconstpointer data)
{
	char *dst = path_in(f->dir, "t.theme");

	g_assert_cmpint(mkfifo(dst, 0600), ==, 0);
	if (g_test_subprocess()) {
		DEFAULT_FILES_REC *rec = default_files_open(f->dir, KNOWN);

		g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
		                DEFAULT_FILE_SKIPPED);
		default_files_close(rec);
		g_free(dst);
		return;
	}
	g_test_trap_subprocess(NULL, 2 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
	g_free(dst);
}

/* the shipped file is a FIFO (in an installation of the user's own): it is
 * not read - opening it would wait for a writer and hang the start */
static void test_shipped_fifo_skipped(Fixture *f, gconstpointer data)
{
	char *dst = path_in(f->dir, "t.theme");

	g_assert_cmpint(g_unlink(f->shipped), ==, 0);
	g_assert_cmpint(mkfifo(f->shipped, 0600), ==, 0);
	if (g_test_subprocess()) {
		DEFAULT_FILES_REC *rec = default_files_open(f->dir, KNOWN);

		g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
		                DEFAULT_FILE_SKIPPED);
		default_files_close(rec);
		g_assert_false(g_file_test(dst, G_FILE_TEST_EXISTS));
		g_free(dst);
		return;
	}
	g_test_trap_subprocess(NULL, 2 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
	g_free(dst);
}

/* themes installed as links (stow, a package of the user's own): the
 * shipped file is read through the link */
static void test_shipped_symlink_followed(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *real = g_strconcat(f->shipped, ".real", NULL);

	g_assert_cmpint(g_rename(f->shipped, real), ==, 0);
	g_assert_cmpint(symlink(real, f->shipped), ==, 0);
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_COPIED);
	default_files_close(rec);
	assert_file(f->dir, "t.theme", SHIPPED);
	g_free(real);
}

/* a link to /dev/null as the shipped file: nothing is installed */
static void test_shipped_dev_null_skipped(Fixture *f, gconstpointer data)
{
	DEFAULT_FILES_REC *rec;
	char *dst = path_in(f->dir, "t.theme");

	g_assert_cmpint(g_unlink(f->shipped), ==, 0);
	g_assert_cmpint(symlink("/dev/null", f->shipped), ==, 0);
	rec = default_files_open(f->dir, KNOWN);
	g_assert_cmpint(default_files_update(rec, f->shipped, "t.theme", TRUE), ==,
	                DEFAULT_FILE_SKIPPED);
	default_files_close(rec);
	g_assert_false(g_file_test(dst, G_FILE_TEST_EXISTS));
	g_free(dst);
}

#define ADD(name, func, subdir) \
	g_test_add(name, Fixture, subdir, fixture_setup, func, fixture_teardown)

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("/default-files/user-changed-kept", test_user_changed_kept, NULL);
	ADD("/default-files/missing-copied-0600", test_missing_copied_0600, NULL);
	ADD("/default-files/hash-list-write-fails-cleanly", test_hash_list_write_fails_cleanly, NULL);
	ADD("/default-files/replace-fails-nothing-recorded",
	    test_replace_fails_nothing_recorded, NULL);
	ADD("/default-files/fsync-fails-not-replaced", test_fsync_fails_not_replaced, NULL);
	ADD("/default-files/themes-changed-sticks", test_themes_changed_sticks, NULL);
	ADD("/default-files/list-symlink-no-record", test_list_symlink_no_record, NULL);
	ADD("/default-files/list-fifo-no-record", test_list_fifo_no_record, NULL);
	ADD("/default-files/list-oversized-no-record", test_list_oversized_no_record, NULL);
	ADD("/default-files/current-untouched", test_current_untouched, NULL);
	ADD("/default-files/recorded-unchanged-updated", test_recorded_unchanged_updated, NULL);
	ADD("/default-files/known-hash-updated", test_known_hash_updated, NULL);
	ADD("/default-files/note-escapes-percent", test_note_escapes_percent, "a%b");
	ADD("/default-files/symlink-skipped", test_symlink_skipped, NULL);
	ADD("/default-files/directory-skipped", test_directory_skipped, NULL);
	ADD("/default-files/unreadable-skipped", test_unreadable_skipped, NULL);
	ADD("/default-files/fifo-skipped", test_fifo_skipped, NULL);
	ADD("/default-files/shipped-fifo-skipped", test_shipped_fifo_skipped, NULL);
	ADD("/default-files/shipped-symlink-followed", test_shipped_symlink_followed, NULL);
	ADD("/default-files/shipped-dev-null-skipped", test_shipped_dev_null_skipped, NULL);

	return g_test_run();
}
