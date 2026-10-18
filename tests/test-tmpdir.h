/*
 * The temporary files of a test, in one directory that is removed when the
 * test ends - also when an assertion fails (abort) or a hang ends it
 * (alarm(), meson's SIGTERM). test_tmpdir_setup() makes it and points
 * TMPDIR there, so g_dir_make_tmp() and g_get_tmp_dir() use it; call it
 * before either is used.
 */
#ifndef ERSSI_TESTS_TEST_TMPDIR_H
#define ERSSI_TESTS_TEST_TMPDIR_H

#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>

static char *test_tmpdir;

static void test_tmpdir_remove(const char *path)
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

				test_tmpdir_remove(child);
				g_free(child);
			}
			g_dir_close(dir);
		}
		g_rmdir(path);
	} else {
		g_unlink(path);
	}
}

static void test_tmpdir_cleanup(void)
{
	if (test_tmpdir != NULL) {
		test_tmpdir_remove(test_tmpdir);
		g_free(test_tmpdir);
		test_tmpdir = NULL;
	}
}

static void test_tmpdir_signal(int sig)
{
	test_tmpdir_cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void test_tmpdir_setup(void)
{
	/* not g_dir_make_tmp(): it makes GLib remember the temporary
	 * directory before TMPDIR points into this tree (see test-home.h) */
	test_tmpdir = g_build_filename(g_getenv("TMPDIR") != NULL ? g_getenv("TMPDIR") : "/tmp",
	                               "erssi-test-XXXXXX", NULL);
	if (g_mkdtemp(test_tmpdir) == NULL) {
		g_printerr("cannot make a temporary directory\n");
		exit(1);
	}
	atexit(test_tmpdir_cleanup);
	signal(SIGABRT, test_tmpdir_signal);
	signal(SIGALRM, test_tmpdir_signal);
	signal(SIGTERM, test_tmpdir_signal);
	g_setenv("TMPDIR", test_tmpdir, TRUE);
}

#endif
