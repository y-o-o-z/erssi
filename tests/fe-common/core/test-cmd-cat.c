/*
 * Tests for /CAT (fe-core-commands.c): a file that cannot be opened prints
 * the open error and frees what the command parsed (LeakSanitizer fails the
 * test in the ASan build otherwise).
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/printtext.h>

#include <string.h>
#include "../../test-home.h"

#define MODULE_NAME "tests"

static GString *printed;

static void sig_print_text(TEXT_DEST_REC *dest, const char *text, const char *stripped)
{
	g_string_append(printed, stripped);
	g_string_append_c(printed, '\n');
}

static void run_cat(const char *args)
{
	char *cmd = g_strdup_printf("/cat %s", args);

	g_string_truncate(printed, 0);
	signal_emit("send command", 3, cmd, NULL, NULL);
	g_free(cmd);
}

static void test_missing_file(void)
{
	char *path = g_build_filename(test_home_dir, "no-such-file", NULL);

	run_cat(path);
	g_assert_nonnull(strstr(printed->str, "No such file or directory"));
	g_free(path);
}

static void test_missing_file_window_option(void)
{
	char *path = g_build_filename(test_home_dir, "no-such-file", NULL);
	char *args = g_strdup_printf("-window %s 5", path);

	run_cat(args);
	g_assert_nonnull(strstr(printed->str, "No such file or directory"));
	g_free(args);
	g_free(path);
}

static void test_existing_file(void)
{
	char *path = g_build_filename(test_home_dir, "present", NULL);

	g_assert_true(g_file_set_contents(path, "line one\n", -1, NULL));
	run_cat(path);
	g_assert_nonnull(strstr(printed->str, "line one"));
	g_free(path);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/cmd-cat/missing-file", test_missing_file);
	g_test_add_func("/cmd-cat/missing-file-window-option", test_missing_file_window_option);
	g_test_add_func("/cmd-cat/existing-file", test_existing_file);

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	args_execute(0, NULL);
	core_init();
	fe_common_core_init();
	signal_emit("irssi init finished", 0);

	printed = g_string_new("");
	signal_add("print text", (SIGNAL_FUNC) sig_print_text);

	return g_test_run();
}
