/*
 * A test or fuzz harness that runs core_init() must not touch the real home:
 * the core reads and writes ~/.erssi/config. test_home_setup() gives the
 * process a temporary --home (and a config inside it) and removes the tree
 * at exit; HOME points there too, as "~" paths (autolog) follow HOME.
 * Call it before core_preinit().
 */
#ifndef ERSSI_TESTS_TEST_HOME_H
#define ERSSI_TESTS_TEST_HOME_H

#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>

#include <glib/gstdio.h>
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

static void test_home_setup(void)
{
	char *argv[] = { "test", "--home", NULL, NULL };

	test_home_dir = g_dir_make_tmp("erssi-test-home-XXXXXX", NULL);
	if (test_home_dir == NULL) {
		g_printerr("cannot make a temporary home\n");
		exit(1);
	}
	atexit(test_home_cleanup);

	/* "~" in paths (autolog, /LOG) expands from the home directory, not
	 * from --home */
	g_setenv("HOME", test_home_dir, TRUE);
	argv[2] = test_home_dir;
	core_register_options();
	args_execute(3, argv);
}

#endif
