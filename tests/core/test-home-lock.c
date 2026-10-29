/*
 test-home-lock.c : the lock that says an erssi runs on a home

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

/* shellter-install.sh --restore asks for an exclusive lock on the file
 * (perl's flock, flock(1) or lockf(1)): here the same with flock(). A
 * FIFO in place of the file would hang open(): alarm() ends the test. */

#include <irssi/src/common.h>
#include <irssi/src/core/home-lock.h>

#include "../test-tmpdir.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static char *dir_new(void)
{
	char *dir = g_dir_make_tmp("home-lock-XXXXXX", NULL);

	g_assert_nonnull(dir);
	alarm(5);
	return dir;
}

/* what --restore does: 0 when it gets the exclusive lock */
static int exclusive(const char *dir)
{
	char *path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	int fd, ret;

	fd = open(path, O_RDONLY | O_NONBLOCK);
	g_free(path);
	g_assert_cmpint(fd, >=, 0);
	ret = flock(fd, LOCK_EX | LOCK_NB);
	close(fd);
	return ret;
}

static void test_shared(void)
{
	char *dir = dir_new(), *path;
	struct stat st;
	int a, b;

	a = home_lock_take(dir);
	g_assert_cmpint(a, >=, 0);
	path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	g_assert_cmpint(lstat(path, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	/* not inherited by a program /upgrade or a script starts */
	g_assert_true(fcntl(a, F_GETFD) & FD_CLOEXEC);

	/* a second erssi on the same home gets it too */
	b = home_lock_take(dir);
	g_assert_cmpint(b, >=, 0);
	g_assert_cmpint(exclusive(dir), !=, 0);
	close(a);
	g_assert_cmpint(exclusive(dir), !=, 0);
	close(b);
	/* nobody runs: --restore may go on, and the file stays */
	g_assert_cmpint(exclusive(dir), ==, 0);
	g_assert_cmpint(lstat(path, &st), ==, 0);
	g_free(path);
	g_free(dir);
}

static void test_symlink_refused(void)
{
	char *dir = dir_new(), *path, *target;
	struct stat st;

	path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	target = g_build_filename(dir, "elsewhere", NULL);
	g_assert_cmpint(symlink(target, path), ==, 0);
	g_assert_cmpint(home_lock_take(dir), ==, -1);
	/* nothing made where the link points */
	g_assert_cmpint(lstat(target, &st), ==, -1);
	g_free(target);
	g_free(path);
	g_free(dir);
}

static void test_fifo_refused(void)
{
	char *dir = dir_new(), *path;

	path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	g_assert_cmpint(home_lock_take(dir), ==, -1);
	g_assert_cmpint(errno, ==, EINVAL);
	g_free(path);
	g_free(dir);
}

static void test_directory_refused(void)
{
	char *dir = dir_new(), *path;

	path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	g_assert_cmpint(mkdir(path, 0700), ==, 0);
	g_assert_cmpint(home_lock_take(dir), ==, -1);
	g_free(path);
	g_free(dir);
}

static void test_no_home(void)
{
	char *dir = dir_new(), *missing;

	missing = g_build_filename(dir, "missing", NULL);
	g_assert_cmpint(home_lock_take(missing), ==, -1);
	g_assert_cmpint(errno, ==, ENOENT);
	g_free(missing);
	g_free(dir);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	test_tmpdir_setup();

	g_test_add_func("/home-lock/shared", test_shared);
	g_test_add_func("/home-lock/symlink", test_symlink_refused);
	g_test_add_func("/home-lock/fifo", test_fifo_refused);
	g_test_add_func("/home-lock/directory", test_directory_refused);
	g_test_add_func("/home-lock/no-home", test_no_home);

	return g_test_run();
}
