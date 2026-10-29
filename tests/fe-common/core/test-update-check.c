/* The update notice (src/fe-common/core/update-check.c): the parser of the
 * installer's --porcelain line, the state file, the timing, where the
 * install's conf is looked for, and the background job against stand-in
 * installers - one for each answer, one that prints garbage, one that
 * floods, one that hangs (stopped after the timeout, with its child) and
 * one that ignores SIGTERM (killed after the grace time). */

#include <irssi/src/common.h>
#include <irssi/src/fe-common/core/update-check.h>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <glib/gstdio.h>

#include "../../test-tmpdir.h"

/* exit codes as waitpid() gives them */
#define EXITED(code) ((code) << 8)

static void parse(const char *out, int status, UPDATE_ANSWER *a)
{
	update_check_parse(out, out == NULL ? 0 : strlen(out), status, a);
}

static void test_version_valid(void)
{
	g_assert_true(update_check_version_valid("2026.11.1"));
	g_assert_true(update_check_version_valid("1.3.10"));
	g_assert_true(update_check_version_valid("0"));
	g_assert_true(update_check_version_valid("1.2.3.4"));
	g_assert_false(update_check_version_valid(NULL));
	g_assert_false(update_check_version_valid(""));
	g_assert_false(update_check_version_valid("1.2.3.4.5"));
	g_assert_false(update_check_version_valid("1..2"));
	g_assert_false(update_check_version_valid(".1"));
	g_assert_false(update_check_version_valid("1."));
	g_assert_false(update_check_version_valid("v1.2"));
	g_assert_false(update_check_version_valid("1.2-rc1"));
	g_assert_false(update_check_version_valid("1234567890.1"));
	g_assert_false(update_check_version_valid("1.2\033[2J"));
}

static void test_parse_answers(void)
{
	UPDATE_ANSWER a;

	parse("update 2026.10.6 2026.11.1\n", EXITED(0), &a);
	g_assert_cmpint(a.kind, ==, UPDATE_ANSWER_UPDATE);
	g_assert_cmpstr(a.installed, ==, "2026.10.6");
	g_assert_cmpstr(a.newest, ==, "2026.11.1");
	g_assert_null(a.reason);
	update_answer_clear(&a);

	parse("current 2026.11.1\n", EXITED(0), &a);
	g_assert_cmpint(a.kind, ==, UPDATE_ANSWER_CURRENT);
	g_assert_cmpstr(a.installed, ==, "2026.11.1");
	g_assert_null(a.newest);
	update_answer_clear(&a);

	parse("error cannot reach https://github.com/y-o-o-z/erssi.git\n", EXITED(1), &a);
	g_assert_cmpint(a.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpstr(a.reason, ==, "cannot reach https://github.com/y-o-o-z/erssi.git");
	update_answer_clear(&a);

	parse("unmanaged updated with: pkg upgrade erssi\n", EXITED(3), &a);
	g_assert_cmpint(a.kind, ==, UPDATE_ANSWER_UNMANAGED);
	g_assert_cmpstr(a.reason, ==, "updated with: pkg upgrade erssi");
	update_answer_clear(&a);

	parse("error\n", EXITED(1), &a);
	g_assert_cmpint(a.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpstr(a.reason, ==, "no reason given");
	update_answer_clear(&a);
}

static void expect_error(const char *out, gsize len, int status, const char *reason)
{
	UPDATE_ANSWER a;

	update_check_parse(out, len, status, &a);
	g_assert_cmpint(a.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpstr(a.reason, ==, reason);
	g_assert_null(a.installed);
	g_assert_null(a.newest);
	update_answer_clear(&a);
}

#define ERR(s, status, reason) expect_error(s, strlen(s), status, reason)

static void test_parse_garbage(void)
{
	const char *odd0 = "unexpected answer from the installer (exit code 0)";
	char buf[] = "current 1.0\n";

	/* the right line with the wrong exit code */
	ERR("update 1.0 2.0\n", EXITED(1), "unexpected answer from the installer (exit code 1)");
	ERR("current 1.0\n", EXITED(3), "unexpected answer from the installer (exit code 3)");
	ERR("error x\n", EXITED(0), odd0);
	ERR("unmanaged x\n", EXITED(1), "unexpected answer from the installer (exit code 1)");
	/* not one line */
	ERR("", EXITED(0), "no answer from the installer (exit code 0)");
	ERR("", EXITED(1), "no answer from the installer (exit code 1)");
	ERR("current 1.0", EXITED(0), odd0);
	ERR("current 1.0\ncurrent 1.0\n", EXITED(0), odd0);
	ERR("\ncurrent 1.0\n", EXITED(0), odd0);
	ERR("current 1.0\nx", EXITED(0), odd0);
	buf[7] = '\0';
	expect_error(buf, sizeof(buf) - 1, EXITED(0), odd0);
	/* not the words */
	ERR("Update available: 1.0 -> 2.0\n", EXITED(0), odd0);
	ERR("update 1.0\n", EXITED(0), odd0);
	ERR("update 1.0 2.0 3.0\n", EXITED(0), odd0);
	ERR("update 1.0  2.0\n", EXITED(0), odd0);
	ERR("update 1.0 v2.0\n", EXITED(0), odd0);
	ERR("update 1.0 2.0\033[2J\n", EXITED(0), odd0);
	ERR("current\n", EXITED(0), odd0);
	ERR("current 1.0 \n", EXITED(0), odd0);
	ERR("CURRENT 1.0\n", EXITED(0), odd0);
	ERR("current 1.0\r\n", EXITED(0), odd0);
	/* killed */
	ERR("current 1.0\n", SIGKILL, "the installer was stopped by signal 9");
	ERR("", SIGTERM, "the installer was stopped by signal 15");
}

static void test_parse_reason_cleaned(void)
{
	UPDATE_ANSWER a;
	GString *long_line;
	char *reason;

	parse("error bad \033[2J\tthing \xc3\xa9\n", EXITED(1), &a);
	g_assert_cmpstr(a.reason, ==, "bad ?[2J?thing ??");
	update_answer_clear(&a);

	long_line = g_string_new("error ");
	while (long_line->len < 1000)
		g_string_append_c(long_line, 'x');
	g_string_append_c(long_line, '\n');
	parse(long_line->str, EXITED(1), &a);
	reason = g_strnfill(200, 'x');
	g_assert_cmpstr(a.reason, ==, reason);
	g_free(reason);
	update_answer_clear(&a);
	g_string_free(long_line, TRUE);
}

static void test_state_roundtrip(void)
{
	UPDATE_STATE st, back;
	char *text;

	memset(&st, 0, sizeof(st));
	st.last_check = 1793000000;
	st.failures = 2;
	st.notified = g_strdup("2026.11.1");
	st.available = g_strdup("2026.11.1");
	st.unmanaged = g_strdup("updated with: pkg upgrade erssi");
	st.last_error = g_strdup("cannot reach https://github.com/y-o-o-z/erssi.git");
	text = update_state_format(&st);
	update_state_parse(text, &back);
	g_assert_cmpint(back.last_check, ==, 1793000000);
	g_assert_cmpint(back.failures, ==, 2);
	g_assert_cmpstr(back.notified, ==, "2026.11.1");
	g_assert_cmpstr(back.available, ==, "2026.11.1");
	g_assert_cmpstr(back.unmanaged, ==, "updated with: pkg upgrade erssi");
	g_assert_cmpstr(back.last_error, ==, "cannot reach https://github.com/y-o-o-z/erssi.git");
	g_free(text);
	update_state_clear(&back);
	g_assert_null(back.notified);

	g_free(st.available);
	st.available = NULL;
	g_free(st.unmanaged);
	st.unmanaged = NULL;
	g_free(st.last_error);
	st.last_error = NULL;
	text = update_state_format(&st);
	g_assert_null(strstr(text, "available="));
	g_assert_null(strstr(text, "unmanaged="));
	g_assert_null(strstr(text, "last_error="));
	update_state_parse(text, &back);
	g_assert_null(back.available);
	g_free(text);
	update_state_clear(&back);
	update_state_clear(&st);
}

static void test_state_invalid(void)
{
	UPDATE_STATE st;

	update_state_parse("last_check=-5\nfailures=x\nnotified=v1\navailable=1.2.3.4.5\n"
	                   "unmanaged=\nlast_error=\nother=1\nnoequals\n",
	                   &st);
	g_assert_null(st.last_error);
	g_assert_cmpint(st.last_check, ==, 0);
	g_assert_cmpint(st.failures, ==, 0);
	g_assert_null(st.notified);
	g_assert_null(st.available);
	g_assert_null(st.unmanaged);
	update_state_clear(&st);

	update_state_parse("last_check=12abc\nfailures=99999999\nnotified=1.0\nnotified=2.0\n"
	                   "last_error=bad\033[2J\n",
	                   &st);
	g_assert_cmpstr(st.last_error, ==, "bad?[2J");
	g_assert_cmpint(st.last_check, ==, 0);
	g_assert_cmpint(st.failures, ==, 0);
	g_assert_cmpstr(st.notified, ==, "2.0");
	update_state_clear(&st);

	update_state_parse(NULL, &st);
	g_assert_cmpint(st.last_check, ==, 0);
	update_state_clear(&st);
}

static void test_state_file(void)
{
	char *dir = g_build_filename(test_tmpdir, "state", NULL);
	char *path = g_build_filename(dir, "update-check.state", NULL);
	char *link = g_build_filename(dir, "link.state", NULL);
	UPDATE_STATE st, back;
	GError *error = NULL;
	struct stat sb;

	g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
	g_assert_false(update_state_load(path, &back));
	g_assert_cmpint(back.last_check, ==, 0);
	update_state_clear(&back);

	memset(&st, 0, sizeof(st));
	st.last_check = 1793000000;
	st.notified = g_strdup("2026.11.1");
	g_assert_true(update_state_save(path, &st, &error));
	g_assert_no_error(error);
	g_assert_cmpint(stat(path, &sb), ==, 0);
	g_assert_cmpint(sb.st_mode & 07777, ==, 0600);
	g_assert_true(update_state_load(path, &back));
	g_assert_cmpint(back.last_check, ==, 1793000000);
	g_assert_cmpstr(back.notified, ==, "2026.11.1");
	update_state_clear(&back);

	/* a link at the path is not read */
	g_assert_cmpint(symlink(path, link), ==, 0);
	g_assert_false(update_state_load(link, &back));
	update_state_clear(&back);
	update_state_clear(&st);

	unlink(link);
	unlink(path);
	g_rmdir(dir);
	g_free(link);
	g_free(path);
	g_free(dir);
}

static void test_due_in(void)
{
	const gint64 now = 1793000000;

	/* never checked, or the clock went back: now */
	g_assert_cmpint(update_check_due_in(now, 0), ==, 0);
	g_assert_cmpint(update_check_due_in(now, now + 3600), ==, 0);
	/* a check just made (also by the erssi before /UPGRADE): in 24 h */
	g_assert_cmpint(update_check_due_in(now, now), ==, UPDATE_CHECK_INTERVAL);
	g_assert_cmpint(update_check_due_in(now, now - 3600), ==, UPDATE_CHECK_INTERVAL - 3600);
	g_assert_cmpint(update_check_due_in(now, now - UPDATE_CHECK_INTERVAL + 1), ==, 1);
	g_assert_cmpint(update_check_due_in(now, now - UPDATE_CHECK_INTERVAL), ==, 0);
	g_assert_cmpint(update_check_due_in(now, now - 10 * UPDATE_CHECK_INTERVAL), ==, 0);
}

static void test_conf_paths(void)
{
	char **p;

	p = update_check_conf_paths("/home/u/.local/opt/erssi");
	g_assert_cmpuint(g_strv_length(p), ==, 1);
	g_assert_cmpstr(p[0], ==, "/home/u/.local/opt/erssi/share/irssi/shellter-install.conf");
	g_strfreev(p);

	p = update_check_conf_paths("/home/u/.local/opt/erssi/releases/2026.11.0");
	g_assert_cmpuint(g_strv_length(p), ==, 2);
	g_assert_cmpstr(p[0], ==,
	                "/home/u/.local/opt/erssi/releases/2026.11.0/share/irssi/shellter-install.conf");
	g_assert_cmpstr(p[1], ==, "/home/u/.local/opt/erssi/share/irssi/shellter-install.conf");
	g_strfreev(p);

	p = update_check_conf_paths("/usr");
	g_assert_cmpuint(g_strv_length(p), ==, 1);
	g_strfreev(p);
	p = update_check_conf_paths("/");
	g_assert_cmpuint(g_strv_length(p), ==, 1);
	g_strfreev(p);
	p = update_check_conf_paths("/releases/x");
	g_assert_cmpuint(g_strv_length(p), ==, 2);
	g_strfreev(p);
}

/* ── the job ─────────────────────────────────────────────────────────── */

typedef struct {
	gboolean done;
	UPDATE_ANSWER answer;
	char *err;
	GMainLoop *loop;
} RESULT;

static void job_done(const UPDATE_ANSWER *answer, const char *err, void *data)
{
	RESULT *r = data;

	g_assert_false(r->done);
	r->done = TRUE;
	r->answer.kind = answer->kind;
	r->answer.installed = g_strdup(answer->installed);
	r->answer.newest = g_strdup(answer->newest);
	r->answer.reason = g_strdup(answer->reason);
	r->err = g_strdup(err);
	g_main_loop_quit(r->loop);
}

static gboolean loop_guard(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static char *script_dir;

/* a stand-in installer: sh SCRIPT --update --check --porcelain --prefix P */
static char *script(const char *name, const char *body)
{
	char *path = g_build_filename(script_dir, name, NULL);
	char *text = g_strconcat("#!/bin/sh\n", body, NULL);

	g_assert_true(g_file_set_contents(path, text, -1, NULL));
	g_free(text);
	return path;
}

/* runs it as erssi does; the pid of the job in *PID */
static void run(const char *body, int timeout_ms, RESULT *r, int *pid)
{
	char *path = script("installer.sh", body);
	const char *argv[] = { "/bin/sh", path, "--update", "--check", "--porcelain",
		               "--prefix", script_dir, NULL };
	UPDATE_JOB *job;
	GError *error = NULL;
	guint guard;

	memset(r, 0, sizeof(*r));
	r->loop = g_main_loop_new(NULL, FALSE);
	job = update_job_start(argv, timeout_ms, job_done, r, &error);
	g_assert_no_error(error);
	g_assert_nonnull(job);
	*pid = update_job_pid(job);
	guard = g_timeout_add(timeout_ms + UPDATE_CHECK_KILL_GRACE_MS + 10000, loop_guard, r->loop);
	g_main_loop_run(r->loop);
	g_source_remove(guard);
	g_assert_true(r->done);
	g_main_loop_unref(r->loop);
	g_free(path);
}

static void result_clear(RESULT *r)
{
	update_answer_clear(&r->answer);
	g_free(r->err);
}

/* reaped: no zombie is left */
static void assert_reaped(int pid)
{
	errno = 0;
	g_assert_cmpint(waitpid(pid, NULL, WNOHANG), ==, -1);
	g_assert_cmpint(errno, ==, ECHILD);
}

/* the process named in FILE is gone (a child of the installer: reparented,
   so it may take a moment until it is reaped by init) */
static void assert_gone(const char *file)
{
	char *text = NULL;
	int pid, i;

	g_assert_true(g_file_get_contents(file, &text, NULL, NULL));
	pid = atoi(text);
	g_free(text);
	g_assert_cmpint(pid, >, 1);
	for (i = 0; i < 100 && kill(pid, 0) == 0; i++)
		g_usleep(20000);
	g_assert_cmpint(kill(pid, 0), ==, -1);
}

static void test_job_answers(void)
{
	RESULT r;
	int pid;

	run("echo 'update 2026.10.6 2026.11.1'\n", 10000, &r, &pid);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_UPDATE);
	g_assert_cmpstr(r.answer.newest, ==, "2026.11.1");
	assert_reaped(pid);
	result_clear(&r);

	/* the steps on stderr come back for the debug log */
	run("echo '[1/1] Finding the release' >&2\necho 'current 2026.11.1'\n", 10000, &r, &pid);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_CURRENT);
	g_assert_cmpstr(r.answer.installed, ==, "2026.11.1");
	g_assert_cmpstr(r.err, ==, "[1/1] Finding the release\n");
	assert_reaped(pid);
	result_clear(&r);

	run("echo 'error cannot reach https://github.com/y-o-o-z/erssi.git'\nexit 1\n", 10000, &r,
	    &pid);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpstr(r.answer.reason, ==, "cannot reach https://github.com/y-o-o-z/erssi.git");
	result_clear(&r);

	run("echo 'unmanaged updated with: pkg upgrade erssi'\nexit 3\n", 10000, &r, &pid);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_UNMANAGED);
	g_assert_cmpstr(r.answer.reason, ==, "updated with: pkg upgrade erssi");
	result_clear(&r);

	/* the arguments erssi gives, and no terminal: stdin is /dev/null,
	   git asks for no password, no repository of erssi's environment
	   reaches git */
	g_setenv("GIT_DIR", "/nonexistent/.git", TRUE);
	g_setenv("GIT_WORK_TREE", "/nonexistent", TRUE);
	g_setenv("GIT_INDEX_FILE", "/nonexistent/.git/index", TRUE);
	g_setenv("GIT_OBJECT_DIRECTORY", "/nonexistent/.git/objects", TRUE);
	g_setenv("GIT_COMMON_DIR", "/nonexistent/.git", TRUE);
	g_setenv("GIT_NAMESPACE", "x", TRUE);
	run("[ \"$*\" = \"--update --check --porcelain --prefix $(dirname \"$0\")\" ] || exit 9\n"
	    "if read x; then exit 8; fi\n"
	    "[ \"$GIT_TERMINAL_PROMPT\" = 0 ] || exit 7\n"
	    "env | grep -Eq '^GIT_(DIR|WORK_TREE|INDEX_FILE|OBJECT_DIRECTORY|COMMON_DIR|NAMESPACE)=' &&"
	    " exit 5\n"
	    "[ ! -t 0 ] && [ ! -t 1 ] && [ ! -t 2 ] || exit 6\n"
	    "echo 'current 1.0'\n",
	    10000, &r, &pid);
	g_assert_cmpstr(r.answer.reason, ==, NULL);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_CURRENT);
	result_clear(&r);
	g_unsetenv("GIT_DIR");
	g_unsetenv("GIT_WORK_TREE");
	g_unsetenv("GIT_INDEX_FILE");
	g_unsetenv("GIT_OBJECT_DIRECTORY");
	g_unsetenv("GIT_COMMON_DIR");
	g_unsetenv("GIT_NAMESPACE");
}

static void test_job_garbage(void)
{
	RESULT r;
	int pid;

	run("echo 'Update available: 1.0 -> 2.0'\necho 'Run: erssi --update'\n", 10000, &r, &pid);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpstr(r.answer.reason, ==, "unexpected answer from the installer (exit code 0)");
	assert_reaped(pid);
	result_clear(&r);

	run("exit 42\n", 10000, &r, &pid);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpstr(r.answer.reason, ==, "no answer from the installer (exit code 42)");
	result_clear(&r);

	/* a flood: kept to 64 KiB, read without blocking, an error */
	run("i=0; while [ $i -lt 3000 ]; do\n"
	    "echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
	    "echo yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy >&2; i=$((i + 1)); done\n",
	    10000, &r, &pid);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpuint(strlen(r.err), ==, 65536);
	assert_reaped(pid);
	result_clear(&r);

	/* killed by a signal */
	run("kill -KILL $$\n", 10000, &r, &pid);
	g_assert_cmpstr(r.answer.reason, ==, "the installer was stopped by signal 9");
	result_clear(&r);
}

static void test_job_timeout(void)
{
	char *child = g_build_filename(script_dir, "child.pid", NULL);
	char *body;
	gint64 t0;
	RESULT r;
	int pid;

	/* hangs in a child (as in git): both stopped, the job reaped */
	body = g_strdup_printf("sleep 300 &\necho $! > '%s'\nwait\necho 'current 1.0'\n", child);
	t0 = g_get_monotonic_time();
	run(body, 500, &r, &pid);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 3 * G_USEC_PER_SEC);
	g_assert_cmpint(r.answer.kind, ==, UPDATE_ANSWER_ERROR);
	g_assert_cmpstr(r.answer.reason, ==, "timed out after 0 s");
	assert_reaped(pid);
	assert_gone(child);
	result_clear(&r);
	g_free(body);

	/* SIGTERM ignored: SIGKILL after the grace time */
	body = g_strdup_printf("trap '' TERM\nsleep 300 &\necho $! > '%s'\nwait\nwait\n", child);
	t0 = g_get_monotonic_time();
	run(body, 1000, &r, &pid);
	g_assert_cmpint(g_get_monotonic_time() - t0, >=,
	                (1000 + UPDATE_CHECK_KILL_GRACE_MS) * (gint64) 1000);
	g_assert_cmpstr(r.answer.reason, ==, "timed out after 1 s");
	assert_reaped(pid);
	assert_gone(child);
	result_clear(&r);
	g_free(body);
	unlink(child);
	g_free(child);
}

/* /UPGRADE and /QUIT while a check runs: stopped and reaped at once */
static void test_job_cancel(void)
{
	char *child = g_build_filename(script_dir, "child.pid", NULL);
	char *path, *body;
	const char *argv[3];
	UPDATE_JOB *job;
	RESULT r;
	int pid;

	body = g_strdup_printf("sleep 300 &\necho $! > '%s'\nwait\n", child);
	path = script("hang.sh", body);
	argv[0] = "/bin/sh";
	argv[1] = path;
	argv[2] = NULL;
	memset(&r, 0, sizeof(r));
	unlink(child);
	job = update_job_start(argv, 60000, job_done, &r, NULL);
	g_assert_nonnull(job);
	pid = update_job_pid(job);
	while (!g_file_test(child, G_FILE_TEST_EXISTS))
		g_usleep(10000);
	update_job_cancel(job);
	g_assert_false(r.done);
	assert_reaped(pid);
	assert_gone(child);
	/* nothing of it is left in the main loop */
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_false(r.done);
	unlink(child);
	unlink(path);
	g_free(path);
	g_free(body);
	g_free(child);
}

/* a job whose process has exited, but was not reaped yet: cancelling it
   reaps it and sends its process group no signal - once reaped, its
   number may belong to another process. A child it left behind in that
   group lives on. */
static void test_job_cancel_after_exit(void)
{
	char *child = g_build_filename(script_dir, "stray.pid", NULL);
	char *got = g_build_filename(script_dir, "stray.term", NULL);
	char *path, *body, *text = NULL;
	const char *argv[3];
	UPDATE_JOB *job;
	RESULT r;
	int pid, stray, i;

	unlink(child);
	unlink(got);
	/* the child says when a signal reaches it */
	body = g_strdup_printf("sh -c 'trap \"echo TERM > %s; exit\" TERM; sleep 300 & wait' "
	                       "> /dev/null 2>&1 &\necho $! > '%s'\n",
	                       got, child);
	path = script("exits.sh", body);
	argv[0] = "/bin/sh";
	argv[1] = path;
	argv[2] = NULL;
	memset(&r, 0, sizeof(r));
	job = update_job_start(argv, 60000, job_done, &r, NULL);
	g_assert_nonnull(job);
	pid = update_job_pid(job);
	for (i = 0; i < 500 && !g_file_get_contents(child, &text, NULL, NULL); i++)
		g_usleep(10000);
	g_assert_nonnull(text);
	stray = atoi(text);
	g_free(text);
	g_assert_cmpint(stray, >, 1);
	/* the script has exited by now (no main loop ran: nothing reaped it) */
	g_usleep(500000);
	update_job_cancel(job);
	g_assert_false(r.done);
	assert_reaped(pid);
	g_usleep(300000);
	g_assert_false(g_file_test(got, G_FILE_TEST_EXISTS));
	g_assert_cmpint(kill(stray, 0), ==, 0);
	kill(-pid, SIGKILL); /* the stray and its sleep, the group still exists */
	unlink(got);
	g_free(got);
	unlink(child);
	unlink(path);
	g_free(path);
	g_free(body);
	g_free(child);
}

static void test_job_spawn_fails(void)
{
	const char *argv[] = { "/nonexistent/sh", NULL };
	GError *error = NULL;
	RESULT r;

	memset(&r, 0, sizeof(r));
	g_assert_null(update_job_start(argv, 1000, job_done, &r, &error));
	g_assert_nonnull(error);
	g_error_free(error);
	g_assert_false(r.done);
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	test_tmpdir_setup();
	script_dir = test_tmpdir;

	g_test_add_func("/update_check/version_valid", test_version_valid);
	g_test_add_func("/update_check/parse_answers", test_parse_answers);
	g_test_add_func("/update_check/parse_garbage", test_parse_garbage);
	g_test_add_func("/update_check/parse_reason_cleaned", test_parse_reason_cleaned);
	g_test_add_func("/update_check/state_roundtrip", test_state_roundtrip);
	g_test_add_func("/update_check/state_invalid", test_state_invalid);
	g_test_add_func("/update_check/state_file", test_state_file);
	g_test_add_func("/update_check/due_in", test_due_in);
	g_test_add_func("/update_check/conf_paths", test_conf_paths);
	g_test_add_func("/update_check/job_answers", test_job_answers);
	g_test_add_func("/update_check/job_garbage", test_job_garbage);
	g_test_add_func("/update_check/job_timeout", test_job_timeout);
	g_test_add_func("/update_check/job_cancel", test_job_cancel);
	g_test_add_func("/update_check/job_cancel_after_exit", test_job_cancel_after_exit);
	g_test_add_func("/update_check/job_spawn_fails", test_job_spawn_fails);
	ret = g_test_run();
	return ret;
}
