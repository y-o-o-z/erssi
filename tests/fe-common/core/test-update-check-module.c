/*
 * The update notice as a module (update-check.c with core and fe-common
 * running, a temporary --home): what the "update" item and /UPDATE show
 * from the state file, the timer's work (update_check_tick_now) and the
 * installer it starts - a stand-in named by ERSSI_TEST_UPDATE_INSTALLER
 * that answers with the line in installer.answer and logs each run.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/update-check.h>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../../test-home.h"

#define MODULE_NAME "tests"

static GString *printed;
static char *answer_file, *runs_file, *state_file;

static void sig_print_text(TEXT_DEST_REC *dest, const char *text, const char *stripped)
{
	(void) dest;
	(void) text;
	g_string_append(printed, stripped);
	g_string_append_c(printed, '\n');
}

static void command(const char *cmd)
{
	signal_emit("send command", 3, cmd, NULL, NULL);
}

static void set_check(gboolean on)
{
	settings_set_bool("update_check", on);
	signal_emit("setup changed", 0);
}

static void write_state(const char *text)
{
	g_assert_true(g_file_set_contents(state_file, text, -1, NULL));
}

/* the stand-in answers LINE with exit code CODE; "hang" makes it wait */
static void answer(const char *line, int code)
{
	char *text = g_strdup_printf("%s\n%d\n", line, code);

	g_assert_true(g_file_set_contents(answer_file, text, -1, NULL));
	g_free(text);
}

static int runs(void)
{
	char *text = NULL;
	int n = 0;
	char *p;

	if (!g_file_get_contents(runs_file, &text, NULL, NULL))
		return 0;
	for (p = text; *p != '\0'; p++)
		n += *p == '\n';
	g_free(text);
	return n;
}

/* until the running check is done (10 s at most) */
static void wait_check(void)
{
	gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (update_check_running() && g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE), g_usleep(10000);
	g_assert_false(update_check_running());
}

/* off, no state, nothing printed */
static void reset(void)
{
	set_check(FALSE);
	unlink(state_file);
	unlink(runs_file);
	answer("current 1.0", 0);
	g_string_truncate(printed, 0);
}

/* The release this erssi is: once it was updated and /UPGRADEd, the
 * version an earlier check found is no longer "available". */
static void test_running_version_not_available(void)
{
	char *state;

	reset();
	state = g_strdup_printf("last_check=%" G_GINT64_FORMAT "\nnotified=%s\navailable=%s\n",
	                        (gint64) time(NULL), PACKAGE_VERSION, PACKAGE_VERSION);
	write_state(state);
	g_free(state);
	set_check(TRUE);
	g_assert_null(update_check_available());
	command("/update");
	g_assert_null(strstr(printed->str, "is available"));
	set_check(FALSE);

	/* an older one neither */
	write_state("last_check=1\navailable=1.0\n");
	set_check(TRUE);
	g_assert_null(update_check_available());
	set_check(FALSE);

	/* a newer one is */
	write_state("last_check=1\navailable=9999.1.0\n");
	set_check(TRUE);
	g_assert_cmpstr(update_check_available(), ==, "9999.1.0");
	g_string_truncate(printed, 0);
	command("/update");
	g_assert_nonnull(strstr(printed->str, "erssi 9999.1.0 is available (signed)"));
	reset();
	g_assert_null(update_check_available());
}

/* Two erssi with one home: the one whose timer finds that the other has
 * checked meanwhile shows what that check found, and does not check. */
static void test_other_erssi_checked(void)
{
	char *state;

	reset();
	write_state("last_check=1\n");
	set_check(TRUE);
	g_assert_null(update_check_available());
	/* the other erssi checks */
	state = g_strdup_printf("last_check=%" G_GINT64_FORMAT "\nnotified=9999.1.0\n"
	                        "available=9999.1.0\n",
	                        (gint64) time(NULL));
	write_state(state);
	g_free(state);
	update_check_tick_now();
	g_assert_false(update_check_running());
	g_assert_cmpint(runs(), ==, 0);
	g_assert_cmpstr(update_check_available(), ==, "9999.1.0");
	/* it said so, this one does not again */
	g_assert_null(strstr(printed->str, "is available"));
	reset();
}

static int count(const char *text, const char *what)
{
	int n = 0;

	while ((text = strstr(text, what)) != NULL) {
		n++;
		text += strlen(what);
	}
	return n;
}

/* "unmanaged" (another tool updates this erssi) is said once, and the
 * daily check goes on: once the conf names no other tool any more, the
 * check finds releases again. */
static void test_unmanaged_checked_daily(void)
{
	reset();
	write_state("last_check=1\nunmanaged=updated with: pkg upgrade erssi\n");
	answer("unmanaged updated with: pkg upgrade erssi", 3);
	set_check(TRUE);
	g_assert_cmpint(count(printed->str, "updated with: pkg upgrade erssi"), ==, 1);
	update_check_tick_now();
	wait_check();
	g_assert_cmpint(runs(), ==, 1);
	/* the same answer is not said again */
	g_assert_cmpint(count(printed->str, "updated with: pkg upgrade erssi"), ==, 1);
	/* the next day (the state read again) the installer answers again */
	set_check(FALSE);
	write_state("last_check=1\nunmanaged=updated with: pkg upgrade erssi\n");
	set_check(TRUE);
	answer("update 2026.10.6 9999.2.0", 0);
	update_check_tick_now();
	wait_check();
	g_assert_cmpint(runs(), ==, 2);
	g_assert_cmpstr(update_check_available(), ==, "9999.2.0");
	g_assert_nonnull(strstr(printed->str, "erssi 9999.2.0 is available (signed)"));
	reset();
}

/* the pid of the stand-in's last run */
static int last_pid(void)
{
	char *text = NULL, *line;
	int pid;

	g_assert_true(g_file_get_contents(runs_file, &text, NULL, NULL));
	line = strrchr(g_strchomp(text), '\n');
	pid = atoi(line == NULL ? text : line + 1);
	g_free(text);
	return pid;
}

/* Capsicum: SIGCHLD is ignored once the mode is entered, so a running
 * check is stopped and reaped before ("capability mode entering"), and
 * after it erssi says why there is no check. Run last: the mode stays. */
static void test_capability_mode(void)
{
	int pid, i;

	reset();
	set_check(TRUE);
	answer("hang", 0);
	command("/update check");
	g_assert_true(update_check_running());
	for (i = 0; i < 500 && runs() == 0; i++)
		g_usleep(10000);
	pid = last_pid();
	g_assert_cmpint(pid, >, 1);
	signal_emit("capability mode entering", 0);
	g_assert_false(update_check_running());
	errno = 0;
	g_assert_cmpint(waitpid(pid, NULL, WNOHANG), ==, -1);
	g_assert_cmpint(errno, ==, ECHILD);
	g_string_truncate(printed, 0);
	signal_emit("capability mode enabled", 0);
	g_assert_nonnull(strstr(printed->str, "not possible in capability mode (Capsicum)"));
	/* no check runs in it */
	g_string_truncate(printed, 0);
	command("/update check");
	g_assert_false(update_check_running());
	g_assert_cmpint(runs(), ==, 1);
	g_assert_nonnull(strstr(printed->str, "not possible in capability mode (Capsicum)"));
	update_check_tick_now();
	g_assert_false(update_check_running());
	g_assert_cmpint(runs(), ==, 1);
}

static void test_newer(void)
{
	g_assert_true(update_check_newer("2026.11.0", "2026.10.6"));
	g_assert_true(update_check_newer("2026.10.10", "2026.10.9"));
	g_assert_true(update_check_newer("2026.10.0", "1.3.10"));
	g_assert_true(update_check_newer("2026.10.0.1", "2026.10.0"));
	g_assert_false(update_check_newer("2026.10.0", "2026.10.0"));
	g_assert_false(update_check_newer("2026.10", "2026.10.0"));
	g_assert_false(update_check_newer("2026.10.6", "2026.11.0"));
	g_assert_false(update_check_newer(NULL, "1.0"));
	g_assert_false(update_check_newer("1.0", NULL));
	g_assert_false(update_check_newer("2.0-rc1", "1.0"));
}

int main(int argc, char **argv)
{
	char *installer, *text;

	g_test_init(&argc, &argv, NULL);
	test_home_setup();

	answer_file = g_build_filename(test_home_dir, "installer.answer", NULL);
	runs_file = g_build_filename(test_home_dir, "installer.runs", NULL);
	installer = g_build_filename(test_home_dir, "shellter-install.sh", NULL);
	text = g_strdup_printf("#!/bin/sh\n"
	                       "echo \"$$ $*\" >> '%s'\n"
	                       "line=$(sed -n 1p '%s'); code=$(sed -n 2p '%s')\n"
	                       "if [ \"$line\" = hang ]; then sleep 300; fi\n"
	                       "echo \"$line\"\n"
	                       "exit \"$code\"\n",
	                       runs_file, answer_file, answer_file);
	g_assert_true(g_file_set_contents(installer, text, -1, NULL));
	g_free(text);
	g_setenv("ERSSI_TEST_UPDATE_INSTALLER", installer, TRUE);
	g_free(installer);

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	args_execute(0, NULL);
	core_init();
	fe_common_core_init();
	signal_emit("irssi init finished", 0);
	state_file = g_build_filename(get_irssi_dir(), "update-check.state", NULL);

	printed = g_string_new("");
	signal_add("print text", (SIGNAL_FUNC) sig_print_text);

	g_test_add_func("/update_check/module/newer", test_newer);
	g_test_add_func("/update_check/module/running_version_not_available",
	                test_running_version_not_available);
	g_test_add_func("/update_check/module/other_erssi_checked", test_other_erssi_checked);
	g_test_add_func("/update_check/module/unmanaged_checked_daily",
	                test_unmanaged_checked_daily);
	/* last: capability mode stays on */
	g_test_add_func("/update_check/module/capability_mode", test_capability_mode);
	return g_test_run();
}
