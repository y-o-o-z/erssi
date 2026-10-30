/*
 * The update notice as a module (update-check.c with core and fe-common
 * running, a temporary --home): what the "update" item and /UPDATE show
 * from the state file, the timer's work (update_check_tick_now) and the
 * installer it starts - a stand-in named by ERSSI_TEST_UPDATE_INSTALLER
 * that answers a check with the lines in installer.answer, runs
 * installer.install for an --update without --check (/UPDATE INSTALL),
 * and logs each run.
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
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../../test-home.h"

#define MODULE_NAME "tests"

static GString *printed;
static char *answer_file, *runs_file, *state_file, *install_file, *conf_file, *log_file;
static GString *upgraded;
static int upgrades;

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

/* /UPGRADE is not run: what it would run is kept */
static void sig_command_upgrade(const char *data)
{
	upgrades++;
	g_string_assign(upgraded, data);
	signal_stop();
}

/* the stand-in answers LINE (it may hold more, each ended by \n but the
   last) with exit code CODE; "hang" makes it wait */
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

/* the stand-in's --update (no --check) runs BODY, a shell script */
static void install_body(const char *body)
{
	g_assert_true(g_file_set_contents(install_file, body, -1, NULL));
}

/* until the install is over (10 s at most) */
static void wait_install(void)
{
	gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (update_install_running() && g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE), g_usleep(10000);
	g_assert_false(update_install_running());
	/* the idle /UPGRADE of -upgrade */
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* the runs whose arguments have WHAT */
static int runs_with(const char *what)
{
	char *text = NULL, **lines, **l;
	int n = 0;

	if (!g_file_get_contents(runs_file, &text, NULL, NULL))
		return 0;
	lines = g_strsplit(text, "\n", -1);
	for (l = lines; *l != NULL; l++)
		n += strstr(*l, what) != NULL;
	g_strfreev(lines);
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
	unlink(install_file);
	unlink(conf_file);
	answer("current 1.0", 0);
	g_string_truncate(printed, 0);
	g_string_truncate(upgraded, 0);
	upgrades = 0;
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

/* The summary of a new release: under the notice, kept in the state file
 * for /UPDATE, its control characters gone; without one, none. */
static void test_summary(void)
{
	char *state = NULL;

	reset();
	answer("update 2026.10.6 9999.1.0\n"
	       "summary Inline images \033[2Jand \xe2\x80\xae/UPDATE INSTALL.\n"
	       "summary 100% \a" "done $0 %r",
	       0);
	command("/update check");
	wait_check();
	g_assert_nonnull(strstr(printed->str, "erssi 9999.1.0 is available (signed). Install it: "
	                                      "/update install"));
	g_assert_nonnull(strstr(printed->str, "  Inline images [2Jand /UPDATE INSTALL.\n"));
	g_assert_nonnull(strstr(printed->str, "  100% done $0 %r\n"));
	g_assert_null(strchr(printed->str, '\033'));
	g_assert_true(g_file_get_contents(state_file, &state, NULL, NULL));
	g_assert_nonnull(strstr(state, "\nsummary=Inline images [2Jand /UPDATE INSTALL.\n"
	                               "summary=100% done $0 %r\n"));
	g_free(state);
	/* /UPDATE shows it again, from the state */
	g_string_truncate(printed, 0);
	command("/update");
	g_assert_nonnull(strstr(printed->str, "erssi 9999.1.0 is available (signed)"));
	g_assert_nonnull(strstr(printed->str, "  Inline images [2Jand /UPDATE INSTALL.\n"));
	/* an escape code put into the state file is not printed either */
	write_state("last_check=1\navailable=9999.1.0\nsummary=x\033]0;owned\007y\n");
	g_string_truncate(printed, 0);
	command("/update");
	g_assert_nonnull(strstr(printed->str, "  x]0;ownedy\n"));
	/* none newer: no summary left */
	answer("current 9999.1.0", 0);
	command("/update check");
	wait_check();
	g_assert_true(g_file_get_contents(state_file, &state, NULL, NULL));
	g_assert_null(strstr(state, "summary="));
	g_free(state);
	reset();
}

static void assert_log_private(void)
{
	struct stat st;

	g_assert_cmpint(lstat(log_file, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpint(st.st_mode & 07777, ==, 0600);
}

/* /UPDATE INSTALL: the check first, then the installer with --update; its
 * steps shown, the build's output not, all of it in the log; at the end the
 * release installed and how /UPGRADE loads it. */
static void test_install(void)
{
	char *log = NULL;

	reset();
	answer("update 2026.10.6 9999.1.0\nsummary What it brings.", 0);
	install_body("echo '  erssi Shellter Edition - update'\n"
	             "echo '  [1/5] Checking the system  ok debian13-amd64'\n"
	             "echo 'compiler noise that is not shown'\n"
	             "echo '        ! a warning of the installer'\n"
	             "echo '  [5/5] Installing           ok ~/.local/opt/erssi'\n"
	             "printf '\\n  Updated: 2026.10.6 -> 9999.1.0\\n'\n"
	             "echo VERSION=9999.1.0 > \"$(dirname \"$0\")/shellter-install.conf\"\n"
	             "exit 0\n");
	command("/update install");
	g_assert_true(update_install_running());
	wait_install();
	g_assert_cmpint(runs_with("--update --check --porcelain --summary --prefix"), ==, 1);
	g_assert_cmpint(runs_with("--update --prefix"), ==, 1);
	g_assert_nonnull(strstr(printed->str, "Update install: checking for the newest signed release"));
	g_assert_nonnull(strstr(printed->str, "Update install: installing erssi 9999.1.0 over 2026.10.6 "
	                                      "in the background"));
	g_assert_nonnull(strstr(printed->str, "  What it brings.\n"));
	g_assert_nonnull(strstr(printed->str, "Update install: [1/5] Checking the system ok "
	                                      "debian13-amd64"));
	g_assert_nonnull(strstr(printed->str, "Update install: ! a warning of the installer"));
	g_assert_null(strstr(printed->str, "compiler noise"));
	g_assert_nonnull(strstr(printed->str, "erssi 9999.1.0 is installed: /upgrade loads it and "
	                                      "keeps your windows and channels (TLS connections "
	                                      "reconnect once)"));
	g_assert_nonnull(strstr(printed->str, log_file));
	g_assert_null(strstr(printed->str, "without disconnecting"));
	g_assert_cmpint(upgrades, ==, 0);
	assert_log_private();
	g_assert_true(g_file_get_contents(log_file, &log, NULL, NULL));
	g_assert_nonnull(strstr(log, "compiler noise that is not shown"));
	g_assert_nonnull(strstr(log, "update 2026.10.6 9999.1.0"));
	g_assert_nonnull(strstr(log, "# /UPDATE INSTALL: erssi 9999.1.0 is installed"));
	g_free(log);

	/* -upgrade: /UPGRADE runs once it is installed */
	reset();
	answer("update 2026.10.6 9999.1.0", 0);
	install_body("echo VERSION=9999.1.0 > \"$(dirname \"$0\")/shellter-install.conf\"\n");
	command("/update install -upgrade");
	wait_install();
	g_assert_cmpint(upgrades, ==, 1);
	g_assert_cmpstr(upgraded->str, ==, "");
	g_assert_nonnull(strstr(printed->str, "running /upgrade to load it (TLS connections "
	                                      "reconnect once)"));
	reset();
}

/* nothing newer, another tool, a failing installer, one at a time */
static void test_install_refused(void)
{
	char *log = NULL;

	/* up to date: nothing installed */
	reset();
	answer("current 2026.10.6", 0);
	install_body("touch \"$(dirname \"$0\")/installed\"\n");
	command("/update install -upgrade");
	wait_install();
	g_assert_cmpint(runs_with("--update --prefix"), ==, 0);
	g_assert_nonnull(strstr(printed->str, "Update install: erssi 2026.10.6 is the newest signed "
	                                      "release - nothing to install"));
	g_assert_cmpint(upgrades, ==, 0);

	/* another tool updates it: not run from erssi */
	reset();
	answer("unmanaged updated with: pkg upgrade erssi", 3);
	command("/update install");
	wait_install();
	g_assert_cmpint(runs_with("--update --prefix"), ==, 0);
	g_assert_nonnull(strstr(printed->str, "Update install failed: this erssi is updated with: pkg "
	                                      "upgrade erssi - erssi does not run that"));

	/* the check fails */
	reset();
	answer("error cannot reach https://github.com/y-o-o-z/erssi.git", 1);
	command("/update install");
	wait_install();
	g_assert_nonnull(strstr(printed->str, "Update install failed: cannot reach "
	                                      "https://github.com/y-o-o-z/erssi.git"));

	/* the installer fails: its error shown, not the end of its build log */
	reset();
	answer("update 2026.10.6 9999.1.0", 0);
	install_body("echo '  [1/6] Checking the system  ok'\n"
	             "printf '\\n  error: another installer is using /p (if not,\\n"
	             "    remove /p/.install.lock)\\n\\n    build log tail\\n' >&2\n"
	             "exit 1\n");
	command("/update install -upgrade");
	wait_install();
	g_assert_nonnull(strstr(printed->str, "Update install failed: another installer is using /p "
	                                      "(if not, (full output: "));
	g_assert_nonnull(strstr(printed->str, "Update install: remove /p/.install.lock)"));
	g_assert_nonnull(strstr(printed->str, "the installer ended with exit code 1"));
	g_assert_null(strstr(printed->str, "build log tail"));
	g_assert_null(strstr(printed->str, "is installed"));
	g_assert_cmpint(upgrades, ==, 0);
	g_assert_true(g_file_get_contents(log_file, &log, NULL, NULL));
	g_assert_nonnull(strstr(log, "build log tail"));
	g_free(log);

	/* exit 0, but the release is not there */
	reset();
	answer("update 2026.10.6 9999.1.0", 0);
	install_body("echo VERSION=1.0 > \"$(dirname \"$0\")/shellter-install.conf\"\n");
	command("/update install");
	wait_install();
	g_assert_nonnull(strstr(printed->str, "Update install failed: the installer ended without an "
	                                      "error, but erssi 1.0 is installed"));

	/* one at a time; no check meanwhile */
	reset();
	answer("update 2026.10.6 9999.1.0", 0);
	install_body("sleep 1\necho VERSION=9999.1.0 > \"$(dirname \"$0\")/shellter-install.conf\"\n");
	command("/update install");
	g_assert_true(update_install_running());
	g_string_truncate(printed, 0);
	command("/update install");
	g_assert_nonnull(strstr(printed->str, "Update install: one is running (full output: "));
	wait_check();
	command("/update check");
	g_assert_false(update_check_running());
	g_assert_nonnull(strstr(printed->str, "Update check: one is running"));
	command("/update");
	g_assert_nonnull(strstr(printed->str, "Update install: running since "));
	wait_install();
	g_assert_cmpint(runs_with("--update --prefix"), ==, 1);
	g_assert_cmpint(runs_with("--check"), ==, 1);
	assert_log_private();

	/* the log is locked while one runs: another erssi with this home
	   cannot start one into it */
	{
		int fd = open(log_file, O_RDWR);

		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpint(flock(fd, LOCK_EX | LOCK_NB), ==, 0);
		reset();
		command("/update install");
		g_assert_false(update_install_running());
		g_assert_nonnull(strstr(printed->str, "another erssi with this home is installing an update"));
		g_assert_cmpint(runs(), ==, 0);
		close(fd);
	}
	reset();
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
	install_file = g_build_filename(test_home_dir, "installer.install", NULL);
	conf_file = g_build_filename(test_home_dir, "shellter-install.conf", NULL);
	installer = g_build_filename(test_home_dir, "shellter-install.sh", NULL);
	text = g_strdup_printf("#!/bin/sh\n"
	                       "echo \"$$ $*\" >> '%s'\n"
	                       "case \" $* \" in *' --check '*) ;; *)\n"
	                       "    [ -f '%s' ] || exit 1\n"
	                       "    . '%s'; exit ;;\n"
	                       "esac\n"
	                       "line=$(sed -n 1p '%s'); code=$(tail -n 1 '%s')\n"
	                       "if [ \"$line\" = hang ]; then sleep 300; fi\n"
	                       "sed '$d' '%s'\n"
	                       "exit \"$code\"\n",
	                       runs_file, install_file, install_file, answer_file, answer_file,
	                       answer_file);
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
	log_file = g_build_filename(get_irssi_dir(), "update-install.log", NULL);

	printed = g_string_new("");
	upgraded = g_string_new("");
	signal_add("print text", (SIGNAL_FUNC) sig_print_text);
	signal_add_first("command upgrade", (SIGNAL_FUNC) sig_command_upgrade);

	g_test_add_func("/update_check/module/newer", test_newer);
	g_test_add_func("/update_check/module/running_version_not_available",
	                test_running_version_not_available);
	g_test_add_func("/update_check/module/other_erssi_checked", test_other_erssi_checked);
	g_test_add_func("/update_check/module/unmanaged_checked_daily",
	                test_unmanaged_checked_daily);
	g_test_add_func("/update_check/module/summary", test_summary);
	g_test_add_func("/update_check/module/install", test_install);
	g_test_add_func("/update_check/module/install_refused", test_install_refused);
	/* last: capability mode stays on */
	g_test_add_func("/update_check/module/capability_mode", test_capability_mode);
	return g_test_run();
}
