/*
 * Automatic backups (src/fe-common/core/backup-auto.c): when one is due,
 * the names of automatic backups and which of them rotation removes, the
 * check that backup_dir is outside the home, the reason of a failure and
 * the state file; then as a module (core and fe-common running, a
 * temporary --home): the timer's work (backup_auto_tick_now) against a
 * stand-in installer named by ERSSI_TEST_UPDATE_INSTALLER, which writes
 * the file it is given, fails or hangs as backup.mode says, and logs each
 * run, and /BACKUP.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/backup-auto.h>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../../test-tmpdir.h"
#include "../../test-home.h"

#define MODULE_NAME "tests"
#define DAY (24 * 60 * 60)
#define EXITED(code) ((code) << 8)

static GString *printed;
static char *mode_file, *runs_file, *state_file, *backups, *home_id;

/* ── the pure parts ──────────────────────────────────────────────────── */

static void test_due(void)
{
	gint64 now = 1800000000;

	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_OFF, 0, 0), ==, -1);
	/* never made: now */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, 0, 0), ==, 0);
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_WEEKLY, 0, 0), ==, 0);
	/* one interval after the last one that worked */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, now - 3600, now - 3600), ==,
	                DAY - 3600);
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_WEEKLY, now - DAY, now - DAY), ==,
	                6 * DAY);
	/* a missed day: now (also when the start of that one is older) */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, now - 3 * DAY, now - 3 * DAY),
	                ==, 0);
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, now - DAY, now - DAY), ==, 0);
	/* the clock went back: now */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, now + 3600, now + 3600), ==, 0);
	/* a failed try 10 minutes ago: the retry delay after it */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, now - 2 * DAY, now - 600), ==,
	                BACKUP_AUTO_RETRY - 600);
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, 0, now - 600), ==,
	                BACKUP_AUTO_RETRY - 600);
	/* ... and one that failed long enough ago: now */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, now - 2 * DAY,
	                                   now - BACKUP_AUTO_RETRY), ==, 0);
	/* a failed try does not make a later due time sooner */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_WEEKLY, now - DAY, now - 600), ==,
	                6 * DAY);
	/* a try in the future (the clock went back) holds nothing off */
	g_assert_cmpint(backup_auto_due_in(now, BACKUP_AUTO_DAILY, now - 2 * DAY, now + 600), ==,
	                0);
}

#define ID "0123abcd"
#define OTHER "fedc9876"

static void test_names(void)
{
	char *name = backup_auto_name(ID, 1800000000), *id, *link, *dir, *sum;

	g_assert_true(g_str_has_prefix(name, "erssi-auto-" ID "-20"));
	g_assert_true(g_str_has_suffix(name, ".tar.gz"));
	g_assert_true(backup_auto_name_valid(name, ID));
	g_assert_true(backup_auto_name_valid(name, NULL));
	g_assert_false(backup_auto_name_valid(name, OTHER));
	g_free(name);
	g_assert_true(backup_auto_name_valid("erssi-auto-" ID "-20261030-120000.tar.gz", ID));
	g_assert_true(backup_auto_name_valid("erssi-auto-" OTHER "-20261030-120000.tar.gz", NULL));
	g_assert_false(backup_auto_name_valid("erssi-auto-" OTHER "-20261030-120000.tar.gz", ID));
	g_assert_false(backup_auto_name_valid(NULL, ID));
	g_assert_false(backup_auto_name_valid("", ID));
	/* the names without an id (never released) are not automatic ones */
	g_assert_false(backup_auto_name_valid("erssi-auto-20261030-120000.tar.gz", NULL));
	g_assert_false(backup_auto_name_valid("erssi-auto-0123ABCD-20261030-120000.tar.gz", NULL));
	g_assert_false(backup_auto_name_valid("erssi-auto-0123abcg-20261030-120000.tar.gz", NULL));
	g_assert_false(backup_auto_name_valid("erssi-auto-0123abc-020261030-120000.tar.gz", NULL));
	g_assert_false(backup_auto_name_valid("erssi-backup-" ID "-20261030-120000.tar.gz", ID));
	g_assert_false(backup_auto_name_valid("erssi-auto-" ID "-20261030-120000.tar.xz", ID));
	g_assert_false(backup_auto_name_valid("erssi-auto-" ID "-20261030-120000.tar.gz~", ID));
	g_assert_false(backup_auto_name_valid("xerssi-auto-" ID "-20261030-120000.tar.gz", ID));
	g_assert_false(backup_auto_name_valid("erssi-auto-" ID "-2026103-1200000.tar.gz", ID));
	g_assert_false(backup_auto_name_valid("erssi-auto-" ID "-2026103a-120000.tar.gz", ID));
	g_assert_false(backup_auto_name_valid("erssi-auto-" ID "-20261030_120000.tar.gz", ID));
	g_assert_false(backup_auto_name_valid(".erssi-backup.AbC123", NULL));

	/* the id of a home: 8 hex digits of the SHA-1 of its real path, the
	   same through a link */
	dir = g_build_filename(test_tmpdir, "idhome", NULL);
	link = g_build_filename(test_tmpdir, "idlink", NULL);
	g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
	g_assert_cmpint(symlink(dir, link), ==, 0);
	name = realpath(dir, NULL);
	g_assert_nonnull(name);
	sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, name, -1);
	free(name);
	id = backup_auto_home_id(dir);
	g_assert_cmpint(strlen(id), ==, BACKUP_AUTO_ID_LEN);
	g_assert_true(strncmp(sum, id, BACKUP_AUTO_ID_LEN) == 0);
	g_free(id);
	id = backup_auto_home_id(link);
	g_assert_true(strncmp(sum, id, BACKUP_AUTO_ID_LEN) == 0);
	g_free(id);
	g_free(sum);
	/* another home, another id */
	id = backup_auto_home_id(test_tmpdir);
	sum = backup_auto_home_id(dir);
	g_assert_cmpstr(id, !=, sum);
	g_free(id);
	g_free(sum);
	g_free(dir);
	g_free(link);
}

static void assert_strv(char **got, const char *const *want)
{
	guint i;

	for (i = 0; want[i] != NULL; i++) {
		g_assert_nonnull(got[i]);
		g_assert_cmpstr(got[i], ==, want[i]);
	}
	g_assert_null(got[i]);
	g_strfreev(got);
}

#define NEWEST "erssi-auto-" ID "-20261030-120000.tar.gz"

static void test_rotate(void)
{
	static const char *const names[] = {
		"erssi-auto-" ID "-20261025-120000.tar.gz",
		"notes.txt",
		"erssi-auto-" ID "-20261029-120000.tar.gz",
		"erssi-backup-20200101-000000.tar.gz",
		"erssi-auto-" ID "-20261027-120000.tar.gz",
		"erssi-auto-" ID "-20261030-120000.tar.gz",
		"erssi-auto-" ID "-20261028-120000.tar.gz",
		"my-erssi-auto-" ID "-20200101-000000.tar.gz",
		/* another home's, and the old names without an id */
		"erssi-auto-" OTHER "-20200101-000000.tar.gz",
		"erssi-auto-20200101-000000.tar.gz",
		"erssi-auto-" ID "-20261026-120000.tar.gz",
		NULL
	};
	static const char *const keep3[] = {
		"erssi-auto-" ID "-20261027-120000.tar.gz",
		"erssi-auto-" ID "-20261026-120000.tar.gz",
		"erssi-auto-" ID "-20261025-120000.tar.gz",
		NULL
	};
	static const char *const keep4_no_newest[] = {
		"erssi-auto-" ID "-20261026-120000.tar.gz",
		"erssi-auto-" ID "-20261025-120000.tar.gz",
		NULL
	};
	static const char *const keep1_old_newest[] = {
		"erssi-auto-" ID "-20261030-120000.tar.gz",
		"erssi-auto-" ID "-20261029-120000.tar.gz",
		"erssi-auto-" ID "-20261028-120000.tar.gz",
		"erssi-auto-" ID "-20261027-120000.tar.gz",
		"erssi-auto-" ID "-20261026-120000.tar.gz",
		NULL
	};
	static const char *const none[] = { NULL };

	/* the newest three, the one just made among them */
	assert_strv(backup_auto_rotate(names, ID, 3, NEWEST), keep3);
	/* nothing just made: the newest four by name */
	assert_strv(backup_auto_rotate(names, ID, 4, NULL), keep4_no_newest);
	/* a clock that went back gave the new one the oldest name: it stays,
	   and the others go */
	assert_strv(backup_auto_rotate(names, ID, 1, "erssi-auto-" ID "-20261025-120000.tar.gz"),
	            keep1_old_newest);
	assert_strv(backup_auto_rotate(names, ID, 7, NEWEST), none);
	assert_strv(backup_auto_rotate(names, ID, 6, NEWEST), none);
	/* 0 or less: nothing is removed */
	assert_strv(backup_auto_rotate(names, ID, 0, NEWEST), none);
	assert_strv(backup_auto_rotate(names, ID, -1, NULL), none);
	assert_strv(backup_auto_rotate(NULL, ID, 3, NULL), none);
	assert_strv(backup_auto_rotate(names, NULL, 1, NULL), none);
	/* another home: its one backup is the newest of its own */
	assert_strv(backup_auto_rotate(names, OTHER, 1, NULL), none);
}

static void test_dir_reason(void)
{
	char *top = g_build_filename(test_tmpdir, "dirs", NULL);
	char *home = g_build_filename(top, "home", NULL);
	char *inside = g_build_filename(home, "backups", NULL);
	char *outside = g_build_filename(top, "out", "new", NULL);
	char *link = g_build_filename(top, "link", NULL);
	char *dots = g_build_filename(home, "..", "out", NULL);
	char *dots_in = g_build_filename(top, "out", "..", "home", "x", NULL);
	char *via_link = g_build_filename(link, "sub", NULL);
	char *r;

	g_assert_cmpint(g_mkdir_with_parents(home, 0700), ==, 0);
	g_assert_cmpint(symlink(home, link), ==, 0);

	g_assert_null(backup_auto_dir_reason(outside, home));
	g_assert_null(backup_auto_dir_reason(dots, home));
	/* a name that only starts like the home is not in it */
	r = g_strconcat(home, "-backups", NULL);
	g_assert_null(backup_auto_dir_reason(r, home));
	g_free(r);

	r = backup_auto_dir_reason(home, home);
	g_assert_nonnull(strstr(r, "is inside the erssi home"));
	g_free(r);
	r = backup_auto_dir_reason(inside, home);
	g_assert_nonnull(strstr(r, "is inside the erssi home"));
	g_free(r);
	r = backup_auto_dir_reason(dots_in, home);
	g_assert_nonnull(r);
	g_free(r);
	/* through a link into the home, also where nothing exists yet */
	r = backup_auto_dir_reason(link, home);
	g_assert_nonnull(r);
	g_free(r);
	r = backup_auto_dir_reason(via_link, home);
	g_assert_nonnull(r);
	g_free(r);
	/* a home named through the link */
	r = backup_auto_dir_reason(inside, link);
	g_assert_nonnull(r);
	g_free(r);
	/* ~ is HOME, which is the test's --home */
	r = backup_auto_dir_reason("~/erssi-backups", test_home_dir);
	g_assert_nonnull(strstr(r, "~/erssi-backups is inside the erssi home"));
	g_free(r);
	r = backup_auto_dir_reason("backups", home);
	g_assert_nonnull(strstr(r, "not an absolute path"));
	g_free(r);
	r = backup_auto_dir_reason("", home);
	g_assert_nonnull(strstr(r, "empty"));
	g_free(r);

	g_free(top);
	g_free(home);
	g_free(inside);
	g_free(outside);
	g_free(link);
	g_free(dots);
	g_free(dots_in);
	g_free(via_link);
}

static void test_error(void)
{
	char *r;

	r = backup_auto_error("\n  erssi Shellter Edition - backup\n\n  error: there is no erssi "
	                      "home ~/.erssi to back up\n",
	                      EXITED(1));
	g_assert_cmpstr(r, ==, "there is no erssi home ~/.erssi to back up");
	g_free(r);
	/* the last one */
	r = backup_auto_error("  error: first\n  error: second\n", EXITED(1));
	g_assert_cmpstr(r, ==, "second");
	g_free(r);
	r = backup_auto_error("tar: something\n", EXITED(2));
	g_assert_cmpstr(r, ==, "the installer stopped with exit code 2");
	g_free(r);
	r = backup_auto_error(NULL, SIGKILL);
	g_assert_cmpstr(r, ==, "the installer was stopped by signal 9");
	g_free(r);
	r = backup_auto_error("  error: bad \033[1m\n", EXITED(1));
	g_assert_cmpstr(r, ==, "bad ?[1m");
	g_free(r);
}

static void test_state(void)
{
	BACKUP_STATE s, t;
	char *text;

	backup_state_parse("# comment\nlast_success=100\nlast_attempt=200\nfailures=2\n"
	                   "last_file=/home/u/erssi-backups/erssi-auto-x.tar.gz\n"
	                   "last_error=disk \001full\nunknown=1\n",
	                   &s);
	g_assert_cmpint(s.last_success, ==, 100);
	g_assert_cmpint(s.last_attempt, ==, 200);
	g_assert_cmpint(s.failures, ==, 2);
	g_assert_cmpstr(s.last_file, ==, "/home/u/erssi-backups/erssi-auto-x.tar.gz");
	g_assert_cmpstr(s.last_error, ==, "disk ?full");
	text = backup_state_format(&s);
	backup_state_parse(text, &t);
	g_assert_cmpint(t.last_success, ==, 100);
	g_assert_cmpint(t.last_attempt, ==, 200);
	g_assert_cmpint(t.failures, ==, 2);
	g_assert_cmpstr(t.last_file, ==, s.last_file);
	g_assert_cmpstr(t.last_error, ==, s.last_error);
	g_free(text);
	backup_state_clear(&s);
	backup_state_clear(&t);

	/* invalid values are left out */
	backup_state_parse("last_success=-5\nlast_attempt=x\nfailures=-1\nlast_file=relative\n", &s);
	g_assert_cmpint(s.last_success, ==, 0);
	g_assert_cmpint(s.last_attempt, ==, 0);
	g_assert_cmpint(s.failures, ==, 0);
	g_assert_null(s.last_file);
	backup_state_clear(&s);
}

/* ── the module ──────────────────────────────────────────────────────── */

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

static void set(const char *key, const char *value)
{
	if (strcmp(key, "backup_auto") == 0)
		g_assert_true(settings_set_choice(key, value));
	else if (strcmp(key, "backup_keep") == 0)
		settings_set_int(key, atoi(value));
	else
		settings_set_str(key, value);
	signal_emit("setup changed", 0);
}

/* the stand-in writes the file (ok), fails (fail) or waits (hang) */
static void installer_mode(const char *mode)
{
	g_assert_true(g_file_set_contents(mode_file, mode, -1, NULL));
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

static int count(const char *text, const char *what)
{
	int n = 0;

	while ((text = strstr(text, what)) != NULL) {
		n++;
		text += strlen(what);
	}
	return n;
}

static void wait_backup(void)
{
	gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (backup_auto_running() && g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE), g_usleep(10000);
	g_assert_false(backup_auto_running());
}

static void write_state(const char *text)
{
	g_assert_true(g_file_set_contents(state_file, text, -1, NULL));
}

static void touch(const char *name)
{
	char *path = g_build_filename(backups, name, NULL);

	g_assert_true(g_file_set_contents(path, "old", -1, NULL));
	g_free(path);
}

/* the automatic name of STAMP (YYYYMMDD-HHMMSS) of the home ID */
static const char *of(const char *id, const char *stamp)
{
	static char buf[4][128];
	static int n;

	n = (n + 1) % 4;
	g_snprintf(buf[n], sizeof(buf[n]), "erssi-auto-%s-%s.tar.gz", id, stamp);
	return buf[n];
}

static gboolean exists(const char *name)
{
	char *path = g_build_filename(backups, name, NULL);
	struct stat st;
	gboolean ok = lstat(path, &st) == 0;

	g_free(path);
	return ok;
}

/* the regular files of the automatic name in the backup directory */
static int auto_files(void)
{
	GDir *d = g_dir_open(backups, 0, NULL);
	const char *name;
	int n = 0;

	g_assert_nonnull(d);
	while ((name = g_dir_read_name(d)) != NULL) {
		char *path = g_build_filename(backups, name, NULL);
		struct stat st;

		if (backup_auto_name_valid(name, home_id) && lstat(path, &st) == 0 &&
		    S_ISREG(st.st_mode))
			n++;
		g_free(path);
	}
	g_dir_close(d);
	return n;
}

static void reset(void)
{
	set("backup_auto", "off");
	set("backup_dir", backups);
	set("backup_keep", "3");
	unlink(state_file);
	unlink(runs_file);
	installer_mode("ok");
	g_string_truncate(printed, 0);
}

/* Due at the start (no backup yet): the timer runs the installer with
 * --backup <backup_dir>/erssi-auto-<id>-*.tar.gz --home <home>, one line
 * says so, and rotation keeps the newest backup_keep automatic backups of
 * this home - nothing else in the directory, no link, and none of another
 * home that shares it. The next tick within the day runs nothing, also
 * after the setting went off and on (a restart). */
static void test_scheduled_and_rotation(void)
{
	char *runs_text = NULL, *link, *state = NULL, *expect;

	reset();
	touch(of(home_id, "20200101-000000"));
	touch(of(home_id, "20200102-000000"));
	touch(of(home_id, "20200103-000000"));
	touch(of(home_id, "20200104-000000"));
	/* another erssi home with this backup_dir: older and newer ones */
	touch(of("fedc9876", "20190101-000000"));
	touch(of("fedc9876", "20200102-000000"));
	touch(of("fedc9876", "20300101-000000"));
	touch(of("fedc9876", "20300102-000000"));
	touch("erssi-backup-20200101-000000.tar.gz");
	touch("hand.tar.gz");
	link = g_build_filename(backups, of(home_id, "19990101-000000"), NULL);
	g_assert_cmpint(symlink("hand.tar.gz", link), ==, 0);
	g_free(link);

	set("backup_auto", "daily");
	g_assert_cmpint(runs(), ==, 0); /* not at once: a minute after */
	backup_auto_tick_now();
	g_assert_true(backup_auto_running());
	wait_backup();
	g_assert_cmpint(runs(), ==, 1);
	g_assert_true(g_file_get_contents(runs_file, &runs_text, NULL, NULL));
	expect = g_strdup_printf("--backup %s/erssi-auto-%s-", backups, home_id);
	g_assert_nonnull(strstr(runs_text, expect));
	g_free(expect);
	expect = g_strdup_printf(" --home %s ", get_irssi_dir());
	g_assert_nonnull(strstr(runs_text, expect));
	g_free(expect);
	g_assert_nonnull(strstr(runs_text, " --erssi-version "));
	g_free(runs_text);

	g_assert_cmpint(count(printed->str, "Backup: "), ==, 1);
	g_assert_nonnull(strstr(printed->str, "/erssi-auto-"));
	/* the new one and the two newest old ones */
	g_assert_cmpint(auto_files(), ==, 3);
	g_assert_true(exists(of(home_id, "20200104-000000")));
	g_assert_true(exists(of(home_id, "20200103-000000")));
	g_assert_false(exists(of(home_id, "20200102-000000")));
	g_assert_false(exists(of(home_id, "20200101-000000")));
	g_assert_true(exists("erssi-backup-20200101-000000.tar.gz"));
	g_assert_true(exists("hand.tar.gz"));
	g_assert_true(exists(of(home_id, "19990101-000000")));
	/* the other home's: all four, though older and more than backup_keep */
	g_assert_true(exists(of("fedc9876", "20190101-000000")));
	g_assert_true(exists(of("fedc9876", "20200102-000000")));
	g_assert_true(exists(of("fedc9876", "20300101-000000")));
	g_assert_true(exists(of("fedc9876", "20300102-000000")));

	g_assert_true(g_file_get_contents(state_file, &state, NULL, NULL));
	g_assert_nonnull(strstr(state, "last_file="));
	g_assert_nonnull(strstr(state, "failures=0"));
	g_assert_null(strstr(state, "last_error"));
	g_free(state);

	backup_auto_tick_now();
	g_assert_false(backup_auto_running());
	set("backup_auto", "off");
	set("backup_auto", "weekly");
	backup_auto_tick_now();
	g_assert_false(backup_auto_running());
	g_assert_cmpint(runs(), ==, 1);

	g_string_truncate(printed, 0);
	command("/backup");
	g_assert_nonnull(strstr(printed->str, "automatic backups weekly into"));
	g_assert_nonnull(strstr(printed->str, "the newest 3 kept"));
	g_assert_nonnull(strstr(printed->str, "Backup: last one "));
	g_assert_nonnull(strstr(printed->str, "Backup: next one "));
	reset();
}

/* A missed day runs at the next tick; a failure is said once, and the
 * same failure again is not; it is tried again only after the retry
 * delay, and /BACKUP shows why it failed. */
static void test_failure_retry(void)
{
	char *state;
	gint64 now = (gint64) time(NULL);

	reset();
	state = g_strdup_printf("last_success=%" G_GINT64_FORMAT "\nlast_attempt=%" G_GINT64_FORMAT
	                        "\n", now - 2 * DAY, now - 2 * DAY);
	write_state(state);
	g_free(state);
	installer_mode("fail");
	set("backup_auto", "daily");
	backup_auto_tick_now();
	wait_backup();
	g_assert_cmpint(runs(), ==, 1);
	g_assert_cmpint(count(printed->str, "Backup failed: no space left on the disk - erssi "
	                                    "tries again in an hour"),
	                ==, 1);
	/* not again at once */
	backup_auto_tick_now();
	g_assert_false(backup_auto_running());
	g_assert_cmpint(runs(), ==, 1);
	/* an hour later: again, and the same failure is not said again */
	state = g_strdup_printf("last_success=%" G_GINT64_FORMAT "\nlast_attempt=%" G_GINT64_FORMAT
	                        "\nfailures=1\nlast_error=no space left on the disk\n",
	                        now - 2 * DAY, now - BACKUP_AUTO_RETRY - 1);
	write_state(state);
	g_free(state);
	/* read again as at a restart */
	set("backup_auto", "off");
	set("backup_auto", "daily");
	backup_auto_tick_now();
	wait_backup();
	g_assert_cmpint(runs(), ==, 2);
	g_assert_cmpint(count(printed->str, "Backup failed"), ==, 1);
	g_string_truncate(printed, 0);
	command("/backup");
	g_assert_nonnull(strstr(printed->str, "failed: no space left on the disk (2 in a row)"));
	/* /BACKUP NOW says it every time */
	g_string_truncate(printed, 0);
	command("/backup now");
	wait_backup();
	g_assert_cmpint(runs(), ==, 3);
	g_assert_nonnull(strstr(printed->str, "Backup failed: no space left on the disk"));
	/* one that works again clears it */
	installer_mode("ok");
	g_string_truncate(printed, 0);
	command("/backup now");
	wait_backup();
	g_assert_cmpint(runs(), ==, 4);
	command("/backup");
	g_assert_null(strstr(printed->str, "failed"));
	reset();
}

/* backup_dir inside the home: said once when set, and no backup runs;
 * the default ~/erssi-backups is in the home here (HOME is the --home) */
static void test_dir_inside_home(void)
{
	reset();
	set("backup_dir", "~/erssi-backups");
	set("backup_auto", "daily");
	g_assert_cmpint(count(printed->str, "is inside the erssi home"), ==, 1);
	backup_auto_tick_now();
	backup_auto_tick_now();
	g_assert_false(backup_auto_running());
	g_assert_cmpint(runs(), ==, 0);
	g_assert_cmpint(count(printed->str, "is inside the erssi home"), ==, 1);
	command("/backup now");
	g_assert_false(backup_auto_running());
	g_assert_cmpint(runs(), ==, 0);
	g_assert_cmpint(count(printed->str, "Backup failed: backup_dir ~/erssi-backups is inside"),
	                ==, 1);
	g_assert_false(g_file_test(state_file, G_FILE_TEST_EXISTS));
	reset();
}

/* /BACKUP NOW also with backup_auto off; it is the last backup then, and
 * a second while one runs is refused */
static void test_now_while_off(void)
{
	reset();
	installer_mode("hang");
	command("/backup now");
	g_assert_true(backup_auto_running());
	command("/backup now");
	g_assert_nonnull(strstr(printed->str, "Backup: one is running"));
	/* stopped as at /QUIT, the next one runs */
	signal_emit("capability mode entering", 0);
	g_assert_false(backup_auto_running());
	installer_mode("ok");
	g_string_truncate(printed, 0);
	command("/backup now");
	wait_backup();
	g_assert_nonnull(strstr(printed->str, "Backup: writing one into"));
	g_assert_cmpint(count(printed->str, "/erssi-auto-"), ==, 1);
	g_string_truncate(printed, 0);
	command("/backup");
	g_assert_nonnull(strstr(printed->str, "automatic backups are off"));
	g_assert_nonnull(strstr(printed->str, "Backup: last one "));
	g_assert_null(strstr(printed->str, "next one"));
	reset();
}

int main(int argc, char **argv)
{
	char *installer, *text;

	g_test_init(&argc, &argv, NULL);
	/* the home inside the temporary directory, the backups next to it */
	test_tmpdir_setup();
	test_home_setup();
	signal(SIGABRT, test_tmpdir_signal);
	signal(SIGALRM, test_tmpdir_signal);
	signal(SIGTERM, test_tmpdir_signal);
	backups = g_build_filename(test_tmpdir, "backups", NULL);
	g_assert_cmpint(g_mkdir(backups, 0700), ==, 0);

	mode_file = g_build_filename(test_tmpdir, "backup.mode", NULL);
	runs_file = g_build_filename(test_tmpdir, "backup.runs", NULL);
	installer = g_build_filename(test_tmpdir, "shellter-install.sh", NULL);
	text = g_strdup_printf("#!/bin/sh\n"
	                       "echo \"$*\" >> '%s'\n"
	                       "mode=$(cat '%s')\n"
	                       "[ \"$mode\" != hang ] || sleep 300\n"
	                       "if [ \"$mode\" = fail ]; then\n"
	                       "  printf '\\n  error: no space left on the disk\\n' >&2; exit 1\n"
	                       "fi\n"
	                       "echo backup > \"$2\"\n",
	                       runs_file, mode_file);
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
	state_file = g_build_filename(get_irssi_dir(), "backup-auto.state", NULL);
	home_id = backup_auto_home_id(get_irssi_dir());

	printed = g_string_new("");
	signal_add("print text", (SIGNAL_FUNC) sig_print_text);

	g_test_add_func("/backup_auto/due", test_due);
	g_test_add_func("/backup_auto/names", test_names);
	g_test_add_func("/backup_auto/rotate", test_rotate);
	g_test_add_func("/backup_auto/dir_reason", test_dir_reason);
	g_test_add_func("/backup_auto/error", test_error);
	g_test_add_func("/backup_auto/state", test_state);
	g_test_add_func("/backup_auto/module/scheduled_and_rotation", test_scheduled_and_rotation);
	g_test_add_func("/backup_auto/module/failure_retry", test_failure_retry);
	g_test_add_func("/backup_auto/module/dir_inside_home", test_dir_inside_home);
	g_test_add_func("/backup_auto/module/now_while_off", test_now_while_off);
	return g_test_run();
}
