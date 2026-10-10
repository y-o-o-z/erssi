/*
 * Tests for the defaults of the terminal colour settings: the C default of
 * colors_ansi_24bit is erssi's (off), a new home gets 24-bit colours from
 * the built-in irssi.conf and an older config from the Shellter defaults
 * migration, also when its only copy of the setting is the ignored
 * "fe-text" one.
 */
#include <irssi/src/common.h>
#include <glib/gstdio.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/lib-config/iconfig.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/shellter-defaults.h>
#include <irssi/src/fe-ansi/term.h>

/* the home of the test, made by the parent for the child process */
#define TEST_HOME_ENV "ERSSI_TEST_HOME"

static void remove_tree(const char *path)
{
	GDir *dir;
	const char *name;

	if (g_file_test(path, G_FILE_TEST_IS_DIR) &&
	    !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
		dir = g_dir_open(path, 0, NULL);
		if (dir != NULL) {
			while ((name = g_dir_read_name(dir)) != NULL) {
				char *child = g_build_filename(path, name, NULL);
				remove_tree(child);
				g_free(child);
			}
			g_dir_close(dir);
		}
	}
	g_remove(path);
}

/* In the parent: makes a home with `config' (none when NULL), runs the
   test in a child process with it, removes the home (also when the child
   failed) and returns FALSE. In the child: returns TRUE. */
static gboolean run_in_child(const char *config)
{
	GError *error = NULL;
	char *home, *fname;

	if (g_test_subprocess())
		return TRUE;

	home = g_dir_make_tmp("erssi-defaults-XXXXXX", &error);
	g_assert_no_error(error);
	if (config != NULL) {
		fname = g_build_filename(home, "config", NULL);
		g_file_set_contents(fname, config, -1, &error);
		g_free(fname);
		if (error != NULL)
			remove_tree(home);
		g_assert_no_error(error);
	}

	g_setenv(TEST_HOME_ENV, home, TRUE);
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_unsetenv(TEST_HOME_ENV);
	remove_tree(home);
	g_free(home);
	g_test_trap_assert_passed();
	return FALSE;
}

/* in the child: starts erssi's core in the home of the test; returns
   the home */
static char *start_in_home(void)
{
	const char *home;
	char *argv[] = { "test-term-defaults", "--home", NULL, NULL };

	home = g_getenv(TEST_HOME_ENV);
	g_assert_nonnull(home);
	argv[2] = (char *) home;
	core_register_options();
	fe_common_core_register_options();
	args_execute(3, argv);
	core_preinit(argv[0]);
	core_init();
	return g_strdup(home);
}

static void test_upstream_and_shipped(void)
{
	SETTINGS_REC *rec;
	char *home;

	if (!g_test_subprocess() &&
	    g_file_test(SYSCONFDIR "/" IRSSI_GLOBAL_CONFIG, G_FILE_TEST_EXISTS)) {
		g_test_skip(SYSCONFDIR "/" IRSSI_GLOBAL_CONFIG " exists: a home without a "
		            "config reads it, not the built-in irssi.conf");
		return;
	}
	if (!run_in_child(NULL))
		return;

	home = start_in_home();
	/* term_common_init reads term_charset of fe-common/core */
	fe_common_core_init();
	term_common_init();

	rec = settings_get_record("colors_ansi_24bit");
	g_assert_nonnull(rec);
	g_assert_cmpint(rec->type, ==, SETTING_TYPE_BOOLEAN);
	g_assert_false(rec->default_value.v_bool);
	/* the "fe-ansi" block of the built-in irssi.conf, not the migration */
	g_assert_null(shellter_defaults_note());
	g_assert_true(settings_get_bool("colors_ansi_24bit"));

	rec = settings_get_record("term_truecolor");
	g_assert_nonnull(rec);
	g_assert_cmpint(rec->default_value.v_int, ==, 0);

	g_free(home);
}

/* before 2026.10.0 the setting could be saved in "fe-text" (the module's old
   name), which is not read: the C default applied. It was on then, so
   24-bit colours stay on through the migration. */
static void test_legacy_fe_text(void)
{
	char *home;

	if (!run_in_child("settings = { \"fe-text\" = { colors_ansi_24bit = \"no\"; }; };\n"))
		return;

	home = start_in_home();
	g_assert_cmpint(shellter_defaults_migrate(mainconfig), ==, 14);
	/* term_common_init reads term_charset of fe-common/core */
	fe_common_core_init();
	term_common_init();

	g_assert_true(settings_get_bool("colors_ansi_24bit"));

	g_free(home);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/term_defaults/upstream_and_shipped", test_upstream_and_shipped);
	g_test_add_func("/term_defaults/legacy_fe_text", test_legacy_fe_text);

	return g_test_run();
}
