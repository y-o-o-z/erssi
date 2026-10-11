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
int fsync(int fd)
{
	syncs++;
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

static void test_read_regular_reads_file(Fixture *f, gconstpointer data)
{
	char *path = path_in(f->dir, "plain");
	char *content = NULL;
	gsize len = 0;

	/* also a file with a NUL and larger than one read() */
	GString *big = g_string_new(NULL);
	int i;

	for (i = 0; i < 3000; i++)
		g_string_append(big, "0123456789");
	g_string_append_c(big, '\0');
	g_string_append(big, "tail");
	g_assert_true(g_file_set_contents(path, big->str, big->len, NULL));

	g_assert_true(default_files_read_regular(path, &content, &len));
	g_assert_cmpuint(len, ==, big->len);
	g_assert_cmpint(memcmp(content, big->str, len), ==, 0);
	g_free(content);
	g_string_free(big, TRUE);
	g_free(path);
}

static void test_read_regular_refuses_link(Fixture *f, gconstpointer data)
{
	char *target = path_in(f->dir, "target");
	char *link = path_in(f->dir, "link");
	char *content = NULL;
	gsize len = 0;

	write_file(target, "through a link\n");
	g_assert_cmpint(symlink(target, link), ==, 0);
	g_assert_false(default_files_read_regular(link, &content, &len));
	g_assert_null(content);
	g_free(link);
	g_free(target);
}

static void test_read_regular_fifo_does_not_block(Fixture *f, gconstpointer data)
{
	char *fifo = path_in(f->dir, "fifo");

	if (g_test_subprocess()) {
		char *content = NULL;
		gsize len = 0;

		g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
		g_assert_false(default_files_read_regular(fifo, &content, &len));
		g_assert_null(content);
		g_free(fifo);
		return;
	}
	g_test_trap_subprocess(NULL, 2 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
	g_free(fifo);
}

#define ADD(name, func, subdir) \
	g_test_add(name, Fixture, subdir, fixture_setup, func, fixture_teardown)

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("/default-files/user-changed-kept", test_user_changed_kept, NULL);
	ADD("/default-files/missing-copied-0600", test_missing_copied_0600, NULL);
	ADD("/default-files/hash-list-write-fails-cleanly", test_hash_list_write_fails_cleanly, NULL);
	ADD("/default-files/current-untouched", test_current_untouched, NULL);
	ADD("/default-files/recorded-unchanged-updated", test_recorded_unchanged_updated, NULL);
	ADD("/default-files/known-hash-updated", test_known_hash_updated, NULL);
	ADD("/default-files/note-escapes-percent", test_note_escapes_percent, "a%b");
	ADD("/default-files/symlink-skipped", test_symlink_skipped, NULL);
	ADD("/default-files/directory-skipped", test_directory_skipped, NULL);
	ADD("/default-files/unreadable-skipped", test_unreadable_skipped, NULL);
	ADD("/default-files/fifo-skipped", test_fifo_skipped, NULL);
	ADD("/default-files/read-regular-reads-file", test_read_regular_reads_file, NULL);
	ADD("/default-files/read-regular-refuses-link", test_read_regular_refuses_link, NULL);
	ADD("/default-files/read-regular-fifo-does-not-block",
	    test_read_regular_fifo_does_not_block, NULL);

	return g_test_run();
}
