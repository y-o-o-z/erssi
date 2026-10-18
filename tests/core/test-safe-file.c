/*
 test-safe-file.c : opening files at paths others may influence

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

/* Every test works with real files, links, FIFOs and directories in its
 * own temporary directory. An open() that waits on a FIFO would hang the
 * test: alarm() ends it instead, and the test fails. */

#include <irssi/src/common.h>
#include <irssi/src/core/safe-file.h>

#include "../test-tmpdir.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define HANG_SECONDS 5

typedef struct {
	char *dir;
} Fixture;

static void fixture_setup(Fixture *f, gconstpointer data)
{
	f->dir = g_dir_make_tmp("safe-file-XXXXXX", NULL);
	g_assert_nonnull(f->dir);
	alarm(HANG_SECONDS);
}

static void remove_tree(const char *path)
{
	struct stat st;
	GDir *dir;
	const char *name;

	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
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

static void fixture_teardown(Fixture *f, gconstpointer data)
{
	alarm(0);
	remove_tree(f->dir);
	g_free(f->dir);
}

static char *path_in(Fixture *f, const char *name)
{
	return g_build_filename(f->dir, name, NULL);
}

static char *make_file(Fixture *f, const char *name, const char *data)
{
	char *path = path_in(f, name);

	g_assert_true(g_file_set_contents(path, data, -1, NULL));
	return path;
}

static char *make_link(Fixture *f, const char *name, const char *target)
{
	char *path = path_in(f, name);

	g_assert_cmpint(symlink(target, path), ==, 0);
	return path;
}

static char *make_fifo(Fixture *f, const char *name)
{
	char *path = path_in(f, name);

	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	return path;
}

static char *file_contents(const char *path)
{
	char *data = NULL;

	g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
	return data;
}

/* a link that is not followed: ELOOP, or EMLINK / EFTYPE on BSDs */
static void assert_link_refused(void)
{
	g_assert_true(errno == ELOOP || errno == EMLINK
#ifdef EFTYPE
	              || errno == EFTYPE
#endif
	              );
}

static void test_read_regular(Fixture *f, gconstpointer data)
{
	char *path = make_file(f, "a", "hello\n"), *got;
	struct stat st;
	gsize len;
	int fd, fl;

	g_assert_true(safe_file_read(path, 0, 64, &got, &len));
	g_assert_cmpstr(got, ==, "hello\n");
	g_assert_cmpuint(len, ==, 6);
	g_free(got);

	/* the descriptor comes back blocking, and closed on exec */
	fd = safe_file_open_read(path, 0, &st);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(st.st_size, ==, 6);
	fl = fcntl(fd, F_GETFL);
	g_assert_cmpint(fl & O_NONBLOCK, ==, 0);
	g_assert_cmpint(fcntl(fd, F_GETFD) & FD_CLOEXEC, ==, FD_CLOEXEC);
	close(fd);
	g_free(path);
}

/* a file with a NUL in it, larger than one read() */
static void test_read_large(Fixture *f, gconstpointer data)
{
	char *path = path_in(f, "big"), *got;
	GString *big = g_string_new(NULL);
	gsize len;
	int i;

	for (i = 0; i < 3000; i++)
		g_string_append(big, "0123456789");
	g_string_append_c(big, '\0');
	g_string_append(big, "tail");
	g_assert_true(g_file_set_contents(path, big->str, big->len, NULL));
	g_assert_true(safe_file_read(path, 0, big->len, &got, &len));
	g_assert_cmpuint(len, ==, big->len);
	g_assert_cmpint(memcmp(got, big->str, len), ==, 0);
	g_free(got);
	g_string_free(big, TRUE);
	g_free(path);
}

static void test_read_size_limit(Fixture *f, gconstpointer data)
{
	char *path = make_file(f, "a", "0123456789"), *got = NULL;
	gsize len;

	g_assert_true(safe_file_read(path, 0, 10, &got, &len));
	g_free(got);
	errno = 0;
	g_assert_false(safe_file_read(path, 0, 9, &got, &len));
	g_assert_cmpint(errno, ==, EFBIG);
	g_free(path);
}

static void test_read_symlink(Fixture *f, gconstpointer data)
{
	char *target = make_file(f, "target", "secret");
	char *link = make_link(f, "link", target), *got;
	FILE *fp;
	gsize len;

	errno = 0;
	g_assert_false(safe_file_read(link, 0, 64, &got, &len));
	assert_link_refused();
	g_assert_null(safe_file_fopen_read(link, 0));

	/* followed only when asked for */
	g_assert_true(safe_file_read(link, SAFE_FILE_FOLLOW, 64, &got, &len));
	g_assert_cmpstr(got, ==, "secret");
	g_free(got);
	fp = safe_file_fopen_read(link, SAFE_FILE_FOLLOW);
	g_assert_nonnull(fp);
	fclose(fp);
	g_free(target);
	g_free(link);
}

/* a FIFO with no writer: open() would wait for one */
static void test_read_fifo(Fixture *f, gconstpointer data)
{
	char *fifo = make_fifo(f, "fifo"), *got;
	char *link = make_link(f, "link", fifo);
	gsize len;

	errno = 0;
	g_assert_false(safe_file_read(fifo, 0, 64, &got, &len));
	g_assert_cmpint(errno, ==, EINVAL);
	g_assert_cmpint(safe_file_open_read(fifo, SAFE_FILE_FOLLOW, NULL), ==, -1);
	/* a followed link to a FIFO: the descriptor is checked, not the link */
	errno = 0;
	g_assert_false(safe_file_read(link, SAFE_FILE_FOLLOW, 64, &got, &len));
	g_assert_cmpint(errno, ==, EINVAL);
	g_free(fifo);
	g_free(link);
}

static void test_read_directory(Fixture *f, gconstpointer data)
{
	char *sub = path_in(f, "sub"), *got;
	gsize len;

	g_assert_cmpint(g_mkdir(sub, 0700), ==, 0);
	errno = 0;
	g_assert_false(safe_file_read(sub, SAFE_FILE_FOLLOW, 64, &got, &len));
	g_assert_cmpint(errno, ==, EISDIR);
	g_free(sub);
}

/* a link to /dev/null (a device): refused, followed or not */
static void test_read_dev_null_link(Fixture *f, gconstpointer data)
{
	char *link = make_link(f, "null", "/dev/null"), *got;
	gsize len;

	g_assert_false(safe_file_read(link, 0, 64, &got, &len));
	errno = 0;
	g_assert_false(safe_file_read(link, SAFE_FILE_FOLLOW, 64, &got, &len));
	g_assert_cmpint(errno, ==, EINVAL);
	g_free(link);
}

/* a directory above that is a link (a log directory on another disk) */
static void test_read_linked_directory(Fixture *f, gconstpointer data)
{
	char *real = path_in(f, "real"), *link, *path, *file, *got;
	gsize len;

	g_assert_cmpint(g_mkdir(real, 0700), ==, 0);
	file = g_build_filename(real, "a", NULL);
	g_assert_true(g_file_set_contents(file, "x", -1, NULL));
	link = make_link(f, "linked", real);
	path = g_build_filename(link, "a", NULL);
	g_assert_true(safe_file_read(path, 0, 64, &got, &len));
	g_assert_cmpstr(got, ==, "x");
	g_free(got);
	g_free(real);
	g_free(link);
	g_free(path);
	g_free(file);
}

static void write_fd(int fd, const char *s)
{
	g_assert_cmpint(write(fd, s, strlen(s)), ==, (gssize) strlen(s));
}

static void test_append_creates(Fixture *f, gconstpointer data)
{
	char *path = path_in(f, "log"), *got;
	struct stat st;
	int fd;

	fd = safe_file_open_append(path, 0600);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(fcntl(fd, F_GETFL) & O_NONBLOCK, ==, 0);
	write_fd(fd, "one\n");
	close(fd);
	fd = safe_file_open_append(path, 0600);
	g_assert_cmpint(fd, >=, 0);
	write_fd(fd, "two\n");
	close(fd);
	got = file_contents(path);
	g_assert_cmpstr(got, ==, "one\ntwo\n");
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
	g_free(got);
	g_free(path);
}

static void test_append_tightens_mode(Fixture *f, gconstpointer data)
{
	char *path = make_file(f, "log", "old\n");
	struct stat st;
	int fd;

	g_assert_cmpint(chmod(path, 0644), ==, 0);
	fd = safe_file_open_append(path, 0600);
	g_assert_cmpint(fd, >=, 0);
	close(fd);
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
	g_free(path);
}

/* a link planted where the log goes: the file it points to stays as it is */
static void test_append_symlink(Fixture *f, gconstpointer data)
{
	char *target = make_file(f, "target", "keep");
	char *link = make_link(f, "log", target), *got;
	struct stat st;

	g_assert_cmpint(chmod(target, 0644), ==, 0);
	errno = 0;
	g_assert_cmpint(safe_file_open_append(link, 0600), ==, -1);
	assert_link_refused();
	got = file_contents(target);
	g_assert_cmpstr(got, ==, "keep");
	g_assert_cmpint(stat(target, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0644);
	g_free(got);
	g_free(target);
	g_free(link);
}

static void test_append_fifo(Fixture *f, gconstpointer data)
{
	char *fifo = make_fifo(f, "log");
	int rd;

	/* no reader: open() fails at once (ENXIO) instead of waiting */
	g_assert_cmpint(safe_file_open_append(fifo, 0600), ==, -1);
	/* with a reader: opened, but refused as no regular file */
	rd = open(fifo, O_RDONLY | O_NONBLOCK);
	g_assert_cmpint(rd, >=, 0);
	errno = 0;
	g_assert_cmpint(safe_file_open_append(fifo, 0600), ==, -1);
	g_assert_cmpint(errno, ==, EINVAL);
	close(rd);
	g_free(fifo);
}

static void test_append_directory(Fixture *f, gconstpointer data)
{
	g_assert_cmpint(safe_file_open_append(f->dir, 0600), ==, -1);
}

static void test_append_dev_null_link(Fixture *f, gconstpointer data)
{
	char *link = make_link(f, "log", "/dev/null");

	g_assert_cmpint(safe_file_open_append(link, 0600), ==, -1);
	g_free(link);
}

/* written from the start: emptied, but only once it is known to be a
 * regular file of the user's own */
static void test_truncate(Fixture *f, gconstpointer data)
{
	char *path = make_file(f, "dl", "old contents"), *got;
	char *target = make_file(f, "target", "keep");
	char *link = make_link(f, "link", target);
	char *fifo = make_fifo(f, "fifo");
	int fd;

	fd = safe_file_open_truncate(path, 0600);
	g_assert_cmpint(fd, >=, 0);
	write_fd(fd, "new");
	close(fd);
	got = file_contents(path);
	g_assert_cmpstr(got, ==, "new");
	g_free(got);

	errno = 0;
	g_assert_cmpint(safe_file_open_truncate(link, 0600), ==, -1);
	assert_link_refused();
	got = file_contents(target);
	g_assert_cmpstr(got, ==, "keep");
	g_free(got);
	g_assert_cmpint(safe_file_open_truncate(fifo, 0600), ==, -1);
	g_free(path);
	g_free(target);
	g_free(link);
	g_free(fifo);
}

/* a hard link to another file of the user's (made by someone else where
 * the system allows it): nothing is appended to it, its mode stays */
static void test_append_hard_link(Fixture *f, gconstpointer data)
{
	char *target = make_file(f, "profile", "keep");
	char *log = path_in(f, "log"), *got;
	struct stat st;

	g_assert_cmpint(chmod(target, 0644), ==, 0);
	g_assert_cmpint(link(target, log), ==, 0);
	errno = 0;
	g_assert_cmpint(safe_file_open_append(log, 0600), ==, -1);
	g_assert_cmpint(errno, ==, EPERM);
	g_assert_cmpstr(safe_file_strerror(errno), ==, "a file with other hard links");
	g_assert_cmpint(safe_file_open_truncate(log, 0600), ==, -1);
	got = file_contents(target);
	g_assert_cmpstr(got, ==, "keep");
	g_assert_cmpint(stat(target, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0644);
	g_free(got);
	g_free(target);
	g_free(log);
}

/* the names are for the refusals of the helper only: the same errno for
 * another reason is printed as it is */
static void test_strerror(Fixture *f, gconstpointer data)
{
	char *a = path_in(f, "a"), *b = path_in(f, "b"), *loop, *got;
	char *fifo = make_fifo(f, "fifo");
	gsize len;

	g_assert_false(safe_file_read(fifo, 0, 64, &got, &len));
	g_assert_cmpstr(safe_file_strerror(errno), ==, "not a regular file");
	g_assert_cmpstr(safe_file_strerror(EPERM), ==, g_strerror(EPERM));

	/* a loop of links in the directories above: ELOOP, but not a link
	 * that was refused */
	g_assert_cmpint(symlink(b, a), ==, 0);
	g_assert_cmpint(symlink(a, b), ==, 0);
	loop = g_build_filename(a, "file", NULL);
	errno = 0;
	g_assert_false(safe_file_read(loop, 0, 64, &got, &len));
	g_assert_cmpint(errno, ==, ELOOP);
	g_assert_cmpstr(safe_file_strerror(errno), ==, g_strerror(ELOOP));
	g_free(loop);
	g_free(a);
	g_free(b);
	g_free(fifo);
}

/* SAFE_FILE_OWN: the user's own file is read (a file of someone else's
 * cannot be made without root, see the check in check_regular()) */
static void test_read_own(Fixture *f, gconstpointer data)
{
	char *path = make_file(f, "own", "mine"), *got;
	gsize len;

	g_assert_true(safe_file_read(path, SAFE_FILE_OWN, 64, &got, &len));
	g_assert_cmpstr(got, ==, "mine");
	g_free(got);
	g_free(path);
}

static void test_write_replaces(Fixture *f, gconstpointer data)
{
	char *path = make_file(f, "f", "old"), *got;
	struct stat st;
	GDir *dir;
	int entries = 0;

	g_assert_true(safe_file_write(path, "new", 3, 0600));
	got = file_contents(path);
	g_assert_cmpstr(got, ==, "new");
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
	g_free(got);

	/* no temporary file is left behind */
	dir = g_dir_open(f->dir, 0, NULL);
	while (g_dir_read_name(dir) != NULL)
		entries++;
	g_dir_close(dir);
	g_assert_cmpint(entries, ==, 1);
	g_free(path);
}

/* a link at the path is replaced by the file; what it pointed to stays */
static void test_write_symlink(Fixture *f, gconstpointer data)
{
	char *target = make_file(f, "target", "keep");
	char *link = make_link(f, "f", target), *got;
	struct stat st;

	g_assert_true(safe_file_write(link, "new", 3, 0600));
	g_assert_cmpint(lstat(link, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	got = file_contents(target);
	g_assert_cmpstr(got, ==, "keep");
	g_free(got);
	got = file_contents(link);
	g_assert_cmpstr(got, ==, "new");
	g_free(got);
	g_free(target);
	g_free(link);
}

/* a FIFO at the path is replaced, never opened */
static void test_write_fifo(Fixture *f, gconstpointer data)
{
	char *fifo = make_fifo(f, "f"), *got;

	g_assert_true(safe_file_write(fifo, "new", 3, 0600));
	got = file_contents(fifo);
	g_assert_cmpstr(got, ==, "new");
	g_free(got);
	g_free(fifo);
}

static void test_write_fails_cleanly(Fixture *f, gconstpointer data)
{
	char *path = g_build_filename(f->dir, "missing", "f", NULL);

	errno = 0;
	g_assert_false(safe_file_write(path, "x", 1, 0600));
	g_assert_cmpint(errno, ==, ENOENT);
	g_free(path);
}

#define ADD(name, func) \
	g_test_add(name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* every temporary file in one directory, removed also after a failure */
	test_tmpdir_setup();

	ADD("/safe-file/read/regular", test_read_regular);
	ADD("/safe-file/read/large", test_read_large);
	ADD("/safe-file/read/size-limit", test_read_size_limit);
	ADD("/safe-file/read/symlink", test_read_symlink);
	ADD("/safe-file/read/fifo", test_read_fifo);
	ADD("/safe-file/read/directory", test_read_directory);
	ADD("/safe-file/read/dev-null-link", test_read_dev_null_link);
	ADD("/safe-file/read/linked-directory", test_read_linked_directory);
	ADD("/safe-file/append/creates", test_append_creates);
	ADD("/safe-file/append/tightens-mode", test_append_tightens_mode);
	ADD("/safe-file/append/symlink", test_append_symlink);
	ADD("/safe-file/append/fifo", test_append_fifo);
	ADD("/safe-file/append/directory", test_append_directory);
	ADD("/safe-file/append/dev-null-link", test_append_dev_null_link);
	ADD("/safe-file/truncate", test_truncate);
	ADD("/safe-file/append/hard-link", test_append_hard_link);
	ADD("/safe-file/strerror", test_strerror);
	ADD("/safe-file/read/own", test_read_own);
	ADD("/safe-file/write/replaces", test_write_replaces);
	ADD("/safe-file/write/symlink", test_write_symlink);
	ADD("/safe-file/write/fifo", test_write_fifo);
	ADD("/safe-file/write/fails-cleanly", test_write_fails_cleanly);

	return g_test_run();
}
