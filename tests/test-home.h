/*
 * A test or fuzz harness that runs core_init() must not touch the real home:
 * the core reads and writes ~/.erssi/config. test_home_setup() gives the
 * process a temporary --home (and a config inside it) and removes the tree
 * at exit, also when an assertion fails or a hang ends the test; HOME
 * points there too, as "~" paths (autolog) follow HOME.
 * Call it before core_preinit().
 */
#ifndef ERSSI_TESTS_TEST_HOME_H
#define ERSSI_TESTS_TEST_HOME_H

#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>

#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>

static char *test_home_dir;

static void test_home_remove_tree(const char *path)
{
	GDir *dir;
	const char *name;

	if (g_file_test(path, G_FILE_TEST_IS_DIR) &&
	    !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
		dir = g_dir_open(path, 0, NULL);
		if (dir != NULL) {
			while ((name = g_dir_read_name(dir)) != NULL) {
				char *child = g_build_filename(path, name, NULL);

				test_home_remove_tree(child);
				g_free(child);
			}
			g_dir_close(dir);
		}
	}
	g_remove(path);
}

static void test_home_cleanup(void)
{
	if (test_home_dir != NULL) {
		test_home_remove_tree(test_home_dir);
		g_free(test_home_dir);
		test_home_dir = NULL;
	}
}

/* A failed assertion aborts, and a hang ends with alarm() or the SIGTERM
 * of meson's timeout: atexit() does not run then, the tree is removed
 * here before the signal ends the process as it would have. */
static void test_home_signal(int sig)
{
	test_home_cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void test_home_setup(void)
{
	char *argv[] = { "test", "--home", NULL, NULL };

	/* not g_dir_make_tmp(): it makes GLib remember the temporary
	 * directory before TMPDIR points into this tree, and later
	 * g_dir_make_tmp() calls of the test would land outside it */
	test_home_dir = g_build_filename(g_getenv("TMPDIR") != NULL ? g_getenv("TMPDIR") : "/tmp",
	                                 "erssi-test-home-XXXXXX", NULL);
	if (g_mkdtemp(test_home_dir) == NULL) {
		g_printerr("cannot make a temporary home\n");
		exit(1);
	}
	atexit(test_home_cleanup);
	signal(SIGABRT, test_home_signal);
	signal(SIGALRM, test_home_signal);
	signal(SIGTERM, test_home_signal);
	/* the temporary files of the test, and the homes of its child
	 * processes (g_test_trap_subprocess(), whose failed assertions end
	 * them with _exit(), without any cleanup), are made in here: they go
	 * with this tree */
	g_setenv("TMPDIR", test_home_dir, TRUE);

	/* "~" in paths (autolog, /LOG) expands from the home directory, not
	 * from --home */
	g_setenv("HOME", test_home_dir, TRUE);
	argv[2] = test_home_dir;
	core_register_options();
	args_execute(3, argv);
}

#endif
