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
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char *dir_new(void)
{
	char *dir = g_dir_make_tmp("home-lock-XXXXXX", NULL);

	g_assert_nonnull(dir);
	/* a hang, not a slow machine: meson's own timeout is 30 s */
	alarm(20);
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

/* a child holds the exclusive lock (as --restore does while it swaps the
 * home) for HOLD_MS, then lets go; returns its pid once it holds it */
/* the write end of the holder's life line: it holds the lock until
   HOLD_MS are over or this end is closed - also when the test dies on a
   failed assertion, so no holder is left behind */
static int holder_life = -1;

static pid_t hold_exclusive(const char *dir, int hold_ms)
{
	char *path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	int fd, pipefd[2], life[2];
	pid_t pid;
	char c;

	fd = open(path, O_RDWR | O_CREAT, 0600);
	g_free(path);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(pipe(pipefd), ==, 0);
	g_assert_cmpint(pipe(life), ==, 0);
	pid = fork();
	g_assert_cmpint(pid, >=, 0);
	if (pid == 0) {
		struct pollfd pfd;

		close(life[1]);
		if (flock(fd, LOCK_EX) != 0)
			_exit(1);
		if (write(pipefd[1], "x", 1) != 1)
			_exit(1);
		pfd.fd = life[0];
		pfd.events = POLLIN;
		while (poll(&pfd, 1, hold_ms) < 0 && errno == EINTR)
			;
		_exit(0);
	}
	close(fd);
	close(pipefd[1]);
	close(life[0]);
	holder_life = life[1];
	g_assert_cmpint(read(pipefd[0], &c, 1), ==, 1);
	close(pipefd[0]);
	return pid;
}

static void holder_gone(void)
{
	if (holder_life != -1)
		close(holder_life);
	holder_life = -1;
}

/* the holder of a lock the test no longer waits for */
static void kill_reap(pid_t pid)
{
	int status;

	holder_gone();
	kill(pid, SIGKILL);
	g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
}

static void reap(pid_t pid)
{
	int status;

	g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
	holder_gone();
	g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_waits_for_restore(void)
{
	char *dir = dir_new();
	gint64 t0;
	pid_t pid;
	int fd;

	/* held for 300 ms: a start waits and gets it. The time is taken
	 * before the child holds it, so a slow machine cannot make the wait
	 * look shorter; there are tries for 10 s (it takes about 0.3) */
	t0 = g_get_monotonic_time();
	pid = hold_exclusive(dir, 300);
	g_assert_cmpint(home_lock_take(dir), ==, -1);
	g_assert_true(errno == EWOULDBLOCK || errno == EAGAIN);
	fd = home_lock_take_wait(dir, 100, 100);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(g_get_monotonic_time() - t0, >=, 300 * 1000);
	close(fd);
	reap(pid);

	/* held longer than it waits: it gives up, with EWOULDBLOCK. The
	 * holder keeps it until it is killed: 3 tries of 100 ms give up
	 * however slow the machine is, and do not wait for the holder */
	pid = hold_exclusive(dir, 60 * 1000);
	t0 = g_get_monotonic_time();
	g_assert_cmpint(home_lock_take_wait(dir, 3, 100), ==, -1);
	g_assert_true(errno == EWOULDBLOCK || errno == EAGAIN);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 10 * 1000 * 1000);
	kill_reap(pid);
	g_free(dir);
}

static void test_no_wait_for_other_failures(void)
{
	char *dir = dir_new(), *path;
	gint64 t0;

	path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	/* one try of 1 s would show: it does not wait at all */
	t0 = g_get_monotonic_time();
	g_assert_cmpint(home_lock_take_wait(dir, 20, 1000), ==, -1);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 1000 * 1000);
	g_free(path);
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
	g_test_add_func("/home-lock/waits-for-restore", test_waits_for_restore);
	g_test_add_func("/home-lock/no-wait-for-other-failures", test_no_wait_for_other_failures);

	return g_test_run();
}
