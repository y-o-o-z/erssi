/*
 update-check.c : the update notice (update_check, /UPDATE)

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

#include "module.h"
#include <irssi/src/core/signals.h>
#include <irssi/src/core/commands.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/relocate.h>
#include <irssi/src/core/safe-file.h>
#ifdef HAVE_CAPSICUM
#include <irssi/src/core/capsicum.h>
#endif
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/module-formats.h>
#include <irssi/src/fe-common/core/update-check.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <glib-unix.h>

#define UPDATE_OUTPUT_MAX 65536
#define UPDATE_REASON_MAX 200
/* the tick re-reads the clock at least this often (s): a suspended laptop
   does not count the time it slept in a GLib timeout */
#define UPDATE_TICK_MAX 900
#define UPDATE_STATE_FILE "update-check.state"
#define UPDATE_STATE_MAX 4096

/* ── answers ─────────────────────────────────────────────────────────── */

void update_answer_clear(UPDATE_ANSWER *answer)
{
	g_free(answer->installed);
	g_free(answer->newest);
	g_free(answer->reason);
	memset(answer, 0, sizeof(*answer));
}

gboolean update_check_version_valid(const char *version)
{
	int parts = 0, digits = 0;
	const char *p;

	if (version == NULL || *version == '\0')
		return FALSE;
	for (p = version;; p++) {
		if (*p >= '0' && *p <= '9') {
			if (++digits > 9)
				return FALSE;
		} else if (*p == '.' || *p == '\0') {
			if (digits == 0 || ++parts > 4)
				return FALSE;
			digits = 0;
			if (*p == '\0')
				return TRUE;
		} else {
			return FALSE;
		}
	}
}

/* printable ASCII, at most UPDATE_REASON_MAX bytes */
static char *clean_reason(const char *text, gsize len)
{
	GString *str;
	gsize i;

	str = g_string_sized_new(MIN(len, UPDATE_REASON_MAX) + 1);
	for (i = 0; i < len && str->len < UPDATE_REASON_MAX; i++) {
		unsigned char c = text[i];

		g_string_append_c(str, c >= 0x20 && c < 0x7f ? (char) c : '?');
	}
	return g_string_free(str, FALSE);
}

static void answer_error(UPDATE_ANSWER *answer, char *reason)
{
	update_answer_clear(answer);
	answer->kind = UPDATE_ANSWER_ERROR;
	answer->reason = reason;
}

void update_check_parse(const char *out, gsize len, int wait_status, UPDATE_ANSWER *answer)
{
	const char *nl, *rest;
	char *line, **words;
	int code;
	guint n;

	memset(answer, 0, sizeof(*answer));
	if (!WIFEXITED(wait_status)) {
		answer_error(answer,
		             g_strdup_printf("the installer was stopped by signal %d",
		                             WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0));
		return;
	}
	code = WEXITSTATUS(wait_status);
	nl = out == NULL ? NULL : memchr(out, '\n', len);
	if (nl == NULL || (gsize) (nl - out) + 1 != len || memchr(out, '\0', len) != NULL) {
		/* no line, more than one, or not ended */
		answer_error(answer, len == 0 ?
		             g_strdup_printf("no answer from the installer (exit code %d)", code) :
		             g_strdup_printf("unexpected answer from the installer (exit code %d)", code));
		return;
	}
	line = g_strndup(out, nl - out);
	rest = strchr(line, ' ');
	words = g_strsplit(line, " ", -1);
	n = g_strv_length(words);

	if (strcmp(words[0], "update") == 0 && n == 3 && code == 0 &&
	    update_check_version_valid(words[1]) && update_check_version_valid(words[2])) {
		answer->kind = UPDATE_ANSWER_UPDATE;
		answer->installed = g_strdup(words[1]);
		answer->newest = g_strdup(words[2]);
	} else if (strcmp(words[0], "current") == 0 && n == 2 && code == 0 &&
	           update_check_version_valid(words[1])) {
		answer->kind = UPDATE_ANSWER_CURRENT;
		answer->installed = g_strdup(words[1]);
	} else if ((strcmp(words[0], "error") == 0 && code == 1) ||
	           (strcmp(words[0], "unmanaged") == 0 && code == 3)) {
		rest = rest == NULL ? "" : rest + 1;
		answer->kind = *words[0] == 'e' ? UPDATE_ANSWER_ERROR : UPDATE_ANSWER_UNMANAGED;
		answer->reason = *rest == '\0' ? g_strdup("no reason given") :
		                                 clean_reason(rest, strlen(rest));
	} else {
		answer_error(answer,
		             g_strdup_printf("unexpected answer from the installer (exit code %d)", code));
	}
	g_strfreev(words);
	g_free(line);
}

/* ── state ───────────────────────────────────────────────────────────── */

void update_state_clear(UPDATE_STATE *state)
{
	g_free(state->notified);
	g_free(state->available);
	g_free(state->unmanaged);
	g_free(state->last_error);
	memset(state, 0, sizeof(*state));
}

void update_state_parse(const char *text, UPDATE_STATE *state)
{
	char **lines, **l;

	memset(state, 0, sizeof(*state));
	if (text == NULL)
		return;
	lines = g_strsplit(text, "\n", -1);
	for (l = lines; *l != NULL; l++) {
		char *eq = strchr(*l, '='), *value, *end;
		gint64 num;

		if (eq == NULL)
			continue;
		*eq = '\0';
		value = eq + 1;
		if (strcmp(*l, "last_check") == 0) {
			num = g_ascii_strtoll(value, &end, 10);
			if (*value != '\0' && *end == '\0' && num > 0)
				state->last_check = num;
		} else if (strcmp(*l, "failures") == 0) {
			num = g_ascii_strtoll(value, &end, 10);
			if (*value != '\0' && *end == '\0' && num >= 0 && num < 1000000)
				state->failures = (int) num;
		} else if (strcmp(*l, "notified") == 0 && update_check_version_valid(value)) {
			g_free(state->notified);
			state->notified = g_strdup(value);
		} else if (strcmp(*l, "available") == 0 && update_check_version_valid(value)) {
			g_free(state->available);
			state->available = g_strdup(value);
		} else if (strcmp(*l, "unmanaged") == 0 && *value != '\0') {
			g_free(state->unmanaged);
			state->unmanaged = clean_reason(value, strlen(value));
		} else if (strcmp(*l, "last_error") == 0 && *value != '\0') {
			g_free(state->last_error);
			state->last_error = clean_reason(value, strlen(value));
		}
	}
	g_strfreev(lines);
}

char *update_state_format(const UPDATE_STATE *state)
{
	GString *str = g_string_new("# erssi update_check (/HELP update_check)\n");

	g_string_append_printf(str, "last_check=%" G_GINT64_FORMAT "\n", state->last_check);
	g_string_append_printf(str, "failures=%d\n", state->failures);
	if (state->notified != NULL)
		g_string_append_printf(str, "notified=%s\n", state->notified);
	if (state->available != NULL)
		g_string_append_printf(str, "available=%s\n", state->available);
	if (state->unmanaged != NULL)
		g_string_append_printf(str, "unmanaged=%s\n", state->unmanaged);
	if (state->last_error != NULL)
		g_string_append_printf(str, "last_error=%s\n", state->last_error);
	return g_string_free(str, FALSE);
}

gboolean update_state_load(const char *path, UPDATE_STATE *state)
{
	char *data = NULL;
	gsize len;

	if (!safe_file_read(path, SAFE_FILE_OWN | SAFE_FILE_QUIET, UPDATE_STATE_MAX, &data, &len)) {
		update_state_parse(NULL, state);
		return FALSE;
	}
	update_state_parse(data, state);
	g_free(data);
	return TRUE;
}

gboolean update_state_save(const char *path, const UPDATE_STATE *state, GError **error)
{
	char *text = update_state_format(state);
	gboolean ok;
	int err;

	ok = safe_file_write(path, text, strlen(text), 0600);
	err = errno;
	g_free(text);
	if (!ok)
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err), "cannot write %s: %s",
		            path, safe_file_strerror(err));
	return ok;
}

gint64 update_check_due_in(gint64 now, gint64 last_check)
{
	if (last_check <= 0 || last_check > now)
		return 0;
	if (now - last_check >= UPDATE_CHECK_INTERVAL)
		return 0;
	return last_check + UPDATE_CHECK_INTERVAL - now;
}

char **update_check_conf_paths(const char *prefix)
{
	GPtrArray *paths = g_ptr_array_new();
	char *parent, *top;

	g_ptr_array_add(paths, g_build_filename(prefix, "share", "irssi", "shellter-install.conf", NULL));
	/* <top>/releases/<version> of a binary install */
	parent = g_path_get_dirname(prefix);
	top = g_path_get_dirname(parent);
	if (strcmp(parent, prefix) != 0 && strcmp(top, parent) != 0) {
		char *base = g_path_get_basename(parent);

		if (strcmp(base, "releases") == 0)
			g_ptr_array_add(paths, g_build_filename(top, "share", "irssi",
			                                        "shellter-install.conf", NULL));
		g_free(base);
	}
	g_free(parent);
	g_free(top);
	g_ptr_array_add(paths, NULL);
	return (char **) g_ptr_array_free(paths, FALSE);
}

/* ── the background job ──────────────────────────────────────────────── */

struct _UPDATE_JOB {
	GPid pid;
	int out_fd, err_fd;
	guint out_tag, err_tag, child_tag, timeout_tag, kill_tag;
	GString *out, *err;
	gboolean exited, timed_out;
	int status, timeout_ms;
	UPDATE_JOB_FUNC done;
	void *data;
};

/* in the child, between fork and exec: a session of its own (no terminal,
   its own process group to stop as a whole), the default action for the
   signals erssi handles or ignores - an ignored SIGPIPE or SIGTERM would
   stay ignored in sh - and no blocked signal */
static void job_child_setup(gpointer data)
{
	static const int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGCHLD,
		                    SIGALRM, SIGUSR1, SIGUSR2, SIGWINCH, SIGTSTP, SIGTTIN, SIGTTOU };
	sigset_t none;
	gsize i;

	(void) data;
	setsid();
	for (i = 0; i < G_N_ELEMENTS(sigs); i++)
		signal(sigs[i], SIG_DFL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
}

static void job_free(UPDATE_JOB *job)
{
	if (job->out_tag != 0)
		g_source_remove(job->out_tag);
	if (job->err_tag != 0)
		g_source_remove(job->err_tag);
	if (job->child_tag != 0)
		g_source_remove(job->child_tag);
	if (job->timeout_tag != 0)
		g_source_remove(job->timeout_tag);
	if (job->kill_tag != 0)
		g_source_remove(job->kill_tag);
	if (job->out_fd != -1)
		close(job->out_fd);
	if (job->err_fd != -1)
		close(job->err_fd);
	g_string_free(job->out, TRUE);
	g_string_free(job->err, TRUE);
	g_free(job);
}

static void job_finish(UPDATE_JOB *job)
{
	UPDATE_ANSWER answer;

	if (job->timed_out) {
		memset(&answer, 0, sizeof(answer));
		answer.kind = UPDATE_ANSWER_ERROR;
		answer.reason = g_strdup_printf("timed out after %d s", job->timeout_ms / 1000);
	} else {
		update_check_parse(job->out->str, job->out->len, job->status, &answer);
	}
	job->done(&answer, job->err->str, job->data);
	update_answer_clear(&answer);
	job_free(job);
}

/* reads what FD has now; at its end (or an error) closes it and sets it
   to -1: FALSE then */
static gboolean job_read(int *fd, GString *buf)
{
	char tmp[4096];
	ssize_t n;

	for (;;) {
		n = read(*fd, tmp, sizeof(tmp));
		if (n > 0) {
			if (buf->len < UPDATE_OUTPUT_MAX)
				g_string_append_len(buf, tmp, MIN((gsize) n, UPDATE_OUTPUT_MAX - buf->len));
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return TRUE;
		break; /* end of file, or an error */
	}
	close(*fd);
	*fd = -1;
	return FALSE;
}

static gboolean job_out(gint fd, GIOCondition cond, gpointer data)
{
	UPDATE_JOB *job = data;

	(void) fd;
	(void) cond;
	if (job_read(&job->out_fd, job->out))
		return G_SOURCE_CONTINUE;
	job->out_tag = 0;
	return G_SOURCE_REMOVE;
}

static gboolean job_err(gint fd, GIOCondition cond, gpointer data)
{
	UPDATE_JOB *job = data;

	(void) fd;
	(void) cond;
	if (job_read(&job->err_fd, job->err))
		return G_SOURCE_CONTINUE;
	job->err_tag = 0;
	return G_SOURCE_REMOVE;
}

static void job_exited(GPid pid, gint status, gpointer data)
{
	UPDATE_JOB *job = data;

	(void) pid;
	job->child_tag = 0;
	job->exited = TRUE;
	job->status = status;
	/* reaped: its number may be reused, no signal goes to it any more */
	if (job->kill_tag != 0) {
		g_source_remove(job->kill_tag);
		job->kill_tag = 0;
	}
	if (job->timeout_tag != 0) {
		g_source_remove(job->timeout_tag);
		job->timeout_tag = 0;
	}
	/* what is in the pipes now was written before the exit; a pipe that a
	   left-over child still holds open is not waited for */
	if (job->out_fd != -1)
		job_read(&job->out_fd, job->out);
	if (job->err_fd != -1)
		job_read(&job->err_fd, job->err);
	job_finish(job); /* closes what is still open */
}

static gboolean job_kill(gpointer data)
{
	UPDATE_JOB *job = data;

	job->kill_tag = 0;
	if (!job->exited)
		kill(-job->pid, SIGKILL);
	return G_SOURCE_REMOVE;
}

static gboolean job_timeout(gpointer data)
{
	UPDATE_JOB *job = data;

	job->timeout_tag = 0;
	job->timed_out = TRUE;
	if (!job->exited) {
		kill(-job->pid, SIGTERM);
		job->kill_tag = g_timeout_add(UPDATE_CHECK_KILL_GRACE_MS, job_kill, job);
	}
	return G_SOURCE_REMOVE;
}

UPDATE_JOB *update_job_start(const char *const *argv, int timeout_ms, UPDATE_JOB_FUNC done,
                             void *data, GError **error)
{
	UPDATE_JOB *job;
	char **envp;
	int null_fd;
	gboolean ok;

	null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (null_fd == -1) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot open /dev/null: %s", g_strerror(errno));
		return NULL;
	}
	/* git never asks for a password, no colours, plain ASCII messages */
	envp = g_get_environ();
	envp = g_environ_setenv(envp, "GIT_TERMINAL_PROMPT", "0", TRUE);
	envp = g_environ_setenv(envp, "NO_COLOR", "1", TRUE);
	envp = g_environ_setenv(envp, "LC_ALL", "C", TRUE);

	job = g_new0(UPDATE_JOB, 1);
	job->out_fd = job->err_fd = -1;
	job->out = g_string_new(NULL);
	job->err = g_string_new(NULL);
	job->timeout_ms = timeout_ms;
	job->done = done;
	job->data = data;
	ok = g_spawn_async_with_pipes_and_fds(NULL, argv, (const char *const *) envp,
	                                      G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES,
	                                      job_child_setup, NULL, null_fd, -1, -1, NULL, NULL,
	                                      0, &job->pid, NULL, &job->out_fd, &job->err_fd,
	                                      error);
	close(null_fd);
	g_strfreev(envp);
	if (!ok) {
		job->out_fd = job->err_fd = -1;
		job_free(job);
		return NULL;
	}
	g_unix_set_fd_nonblocking(job->out_fd, TRUE, NULL);
	g_unix_set_fd_nonblocking(job->err_fd, TRUE, NULL);
	job->out_tag = g_unix_fd_add(job->out_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, job_out, job);
	job->err_tag = g_unix_fd_add(job->err_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, job_err, job);
	job->child_tag = g_child_watch_add(job->pid, job_exited, job);
	job->timeout_tag = g_timeout_add(timeout_ms, job_timeout, job);
	return job;
}

void update_job_cancel(UPDATE_JOB *job)
{
	int i, status;

	if (job->child_tag != 0) {
		/* not reaped yet: the number is still ours */
		g_source_remove(job->child_tag);
		job->child_tag = 0;
		kill(-job->pid, SIGTERM);
		/* the installer removes its temporary files on SIGTERM */
		for (i = 0; i < 40; i++) {
			if (waitpid(job->pid, &status, WNOHANG) != 0)
				break;
			g_usleep(50000);
		}
		if (i == 40) {
			kill(-job->pid, SIGKILL);
			while (waitpid(job->pid, &status, 0) == -1 && errno == EINTR)
				;
		}
	}
	job_free(job);
}

int update_job_pid(UPDATE_JOB *job)
{
	return (int) job->pid;
}

/* ── the module ──────────────────────────────────────────────────────── */

static UPDATE_JOB *job;
static guint tick_tag;
static gboolean check_on, started;
static char *available;
static UPDATE_STATE cached;

static char *state_path(void)
{
	return g_build_filename(get_irssi_dir(), UPDATE_STATE_FILE, NULL);
}

static void state_reload(void)
{
	char *path = state_path();

	update_state_clear(&cached);
	update_state_load(path, &cached);
	g_free(path);
}

static void state_store(void)
{
	GError *error = NULL;
	char *path = state_path();

	/* not said: the next check is then due again after a restart */
	if (!update_state_save(path, &cached, &error))
		g_error_free(error);
	g_free(path);
}

static void set_available(const char *version)
{
	if (g_strcmp0(available, version) == 0)
		return;
	g_free(available);
	available = g_strdup(version);
	signal_emit("update check changed", 0);
}

const char *update_check_available(void)
{
	return available;
}

/* the installer installed with this erssi; ERSSI_TEST_UPDATE_INSTALLER
   (for the tests) names another one, taken as an install it made */
static char *installer_path(gboolean *test)
{
	const char *env = g_getenv("ERSSI_TEST_UPDATE_INSTALLER");

	*test = env != NULL && *env != '\0';
	if (*test)
		return g_strdup(env);
	return g_build_filename(get_install_path(PKGDATADIR), "shellter-install.sh", NULL);
}

/* NULL when a check can run here, else why not */
static char *unavailable_reason(void)
{
	char *installer, **confs, **c, *reason = NULL;
	const char *foreign = NULL;
	gboolean test, found = FALSE;

#ifdef HAVE_CAPSICUM
	if (capsicum_enabled())
		return g_strdup("not possible in capability mode (Capsicum): erssi cannot start "
		                "the installer there - use erssi --check-update from a shell");
#endif
	installer = installer_path(&test);
	if (access(installer, R_OK) != 0) {
		reason = g_strdup_printf("this erssi was not installed with shellter-install.sh "
		                         "(no %s) - update it the way it was installed",
		                         installer);
		g_free(installer);
		return reason;
	}
	g_free(installer);
	if (test)
		return NULL;
	/* the installer updates only an install of the user's own (a system
	   install made by root is root's to update) */
	confs = update_check_conf_paths(get_install_prefix());
	for (c = confs; *c != NULL && !found; c++) {
		struct stat st;

		if (stat(*c, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (st.st_uid == geteuid())
			found = TRUE;
		else if (foreign == NULL)
			foreign = *c;
	}
	if (!found && foreign != NULL)
		reason = g_strdup_printf("this erssi was installed by another user (%s is not "
		                         "yours) - they update it",
		                         foreign);
	else if (!found)
		reason = g_strdup_printf("this erssi was not installed with shellter-install.sh "
		                         "(no %s) - update it the way it was installed",
		                         confs[0]);
	g_strfreev(confs);
	return reason;
}

gboolean update_check_possible(void)
{
	char *reason = unavailable_reason();

	g_free(reason);
	return reason == NULL;
}

static void tick_schedule(gint64 seconds);

static void check_done(const UPDATE_ANSWER *answer, const char *err_text, void *data)
{
	gboolean manual = GPOINTER_TO_INT(data);

	(void) err_text; /* the steps; erssi --check-update shows them */
	job = NULL;
	/* another erssi with this home may have written it meanwhile */
	state_reload();
	switch (answer->kind) {
	case UPDATE_ANSWER_UPDATE:
		cached.failures = 0;
		g_free(cached.unmanaged);
		cached.unmanaged = NULL;
		g_free(cached.available);
		cached.available = g_strdup(answer->newest);
		if (manual || g_strcmp0(cached.notified, answer->newest) != 0)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_AVAILABLE,
			            answer->installed, answer->newest);
		g_free(cached.notified);
		cached.notified = g_strdup(answer->newest);
		break;
	case UPDATE_ANSWER_CURRENT:
		cached.failures = 0;
		g_free(cached.unmanaged);
		cached.unmanaged = NULL;
		g_free(cached.available);
		cached.available = NULL;
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_CURRENT,
			            answer->installed);
		break;
	case UPDATE_ANSWER_UNMANAGED:
		if (manual || g_strcmp0(cached.unmanaged, answer->reason) != 0)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE,
			            answer->reason);
		g_free(cached.unmanaged);
		cached.unmanaged = g_strdup(answer->reason);
		g_free(cached.available);
		cached.available = NULL;
		break;
	case UPDATE_ANSWER_ERROR:
		cached.failures++;
		g_free(cached.last_error);
		cached.last_error = g_strdup(answer->reason);
		/* a moved release tag is for the user to report at once */
		if (manual || strstr(answer->reason, "report it") != NULL)
			printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_FAILED,
			            answer->reason);
		else if (cached.failures == UPDATE_CHECK_FAILURES_SAY)
			printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, TXT_UPDATE_FAILED_AGAIN,
			            answer->reason, cached.failures);
		break;
	}
	if (answer->kind != UPDATE_ANSWER_ERROR) {
		g_free(cached.last_error);
		cached.last_error = NULL;
	}
	state_store();
	set_available(cached.available);
	if (check_on)
		tick_schedule(UPDATE_TICK_MAX);
}

static void check_start(gboolean manual)
{
	const char *argv[8];
	GError *error = NULL;
	char *installer, *reason;
	gboolean test;

	if (job != NULL) {
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_BUSY);
		return;
	}
	reason = unavailable_reason();
	if (reason != NULL) {
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_UNAVAILABLE, reason);
		g_free(reason);
		return;
	}
	installer = installer_path(&test);
	argv[0] = "/bin/sh";
	argv[1] = installer;
	argv[2] = "--update";
	argv[3] = "--check";
	argv[4] = "--porcelain";
	argv[5] = "--prefix";
	argv[6] = get_install_prefix();
	argv[7] = NULL;

	/* the start is what counts: a check cut short by /UPGRADE or /QUIT
	   is not run again right after the next start */
	state_reload();
	cached.last_check = (gint64) time(NULL);
	state_store();

	if (manual)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_CHECKING);
	job = update_job_start(argv, UPDATE_CHECK_TIMEOUT_MS, check_done, GINT_TO_POINTER(manual),
	                       &error);
	g_free(installer);
	if (job == NULL) {
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_FAILED,
			            error->message);
		state_reload();
		cached.failures++;
		g_free(cached.last_error);
		cached.last_error = g_strdup(error->message);
		state_store();
		g_error_free(error);
	}
}

static gboolean tick(gpointer data)
{
	gint64 due;

	(void) data;
	tick_tag = 0;
	if (!check_on || job != NULL)
		return G_SOURCE_REMOVE;
	due = update_check_due_in((gint64) time(NULL), cached.last_check);
	if (due == 0) {
		/* another erssi with this home may have checked meanwhile */
		state_reload();
		due = update_check_due_in((gint64) time(NULL), cached.last_check);
	}
	if (due == 0 && cached.unmanaged == NULL)
		check_start(FALSE);
	if (job == NULL)
		tick_schedule(due > 0 ? due : UPDATE_TICK_MAX);
	return G_SOURCE_REMOVE;
}

static void tick_schedule(gint64 seconds)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = g_timeout_add_seconds((guint) CLAMP(seconds, 1, UPDATE_TICK_MAX), tick, NULL);
}

static void read_settings(void)
{
	gboolean on = settings_get_bool("update_check");
	char *reason;

	/* before the start is complete, sig_init_finished takes it */
	if (!started || on == check_on)
		return;
	check_on = on;
	if (!on) {
		if (tick_tag != 0)
			g_source_remove(tick_tag);
		tick_tag = 0;
		if (job != NULL)
			update_job_cancel(job);
		job = NULL;
		set_available(NULL);
		return;
	}
	/* turned on: why it cannot work here, once */
	reason = unavailable_reason();
	if (reason != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE, reason);
		g_free(reason);
		return;
	}
	state_reload();
	if (cached.unmanaged != NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE,
		            cached.unmanaged);
	set_available(cached.available);
	tick_schedule(MAX(UPDATE_CHECK_FIRST_DELAY,
	                  update_check_due_in((gint64) time(NULL), cached.last_check)));
}

/* the first check a minute after the start (or when the last one is 24 h
   old), and what the last one found shown at once */
static void sig_init_finished(void)
{
	started = TRUE;
	check_on = settings_get_bool("update_check");
	if (!check_on)
		return;
	state_reload();
	set_available(cached.available);
	if (!update_check_possible())
		return;
	tick_schedule(MAX(UPDATE_CHECK_FIRST_DELAY,
	                  update_check_due_in((gint64) time(NULL), cached.last_check)));
}

/* SYNTAX: UPDATE */
static void cmd_update(const char *data, SERVER_REC *server, void *item)
{
	char *reason, *when;

	if (*data != '\0') {
		command_runsub("update", data, server, item);
		return;
	}
	if (!settings_get_bool("update_check"))
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_OFF);
	reason = unavailable_reason();
	if (reason != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE, reason);
		g_free(reason);
		return;
	}
	state_reload();
	if (job != NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_BUSY);
	if (cached.last_check > 0) {
		GDateTime *dt = g_date_time_new_from_unix_local(cached.last_check);

		when = dt == NULL ? g_strdup("at an unknown time") :
		                    g_date_time_format(dt, "%Y-%m-%d %H:%M");
		if (dt != NULL)
			g_date_time_unref(dt);
		if (cached.last_error != NULL)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_LAST_FAILED, when,
			            cached.last_error, cached.failures);
		else
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_LAST, when);
		g_free(when);
	}
	if (cached.unmanaged != NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE,
		            cached.unmanaged);
	else if (cached.available != NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_AVAILABLE, PACKAGE_VERSION,
		            cached.available);
}

/* SYNTAX: UPDATE CHECK */
static void cmd_update_check(void)
{
	check_start(TRUE);
}

void update_check_init(void)
{
	settings_add_bool("misc", "update_check", FALSE);
	check_on = started = FALSE;
	memset(&cached, 0, sizeof(cached));

	command_bind("update", NULL, (SIGNAL_FUNC) cmd_update);
	command_bind("update check", NULL, (SIGNAL_FUNC) cmd_update_check);
	signal_add_last("irssi init finished", (SIGNAL_FUNC) sig_init_finished);
	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
}

void update_check_deinit(void)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
	/* /UPGRADE and /QUIT: the check stops and is reaped here, so the new
	   process gets no zombie; its start is in the state file */
	if (job != NULL)
		update_job_cancel(job);
	job = NULL;
	g_free(available);
	available = NULL;
	update_state_clear(&cached);

	command_unbind("update", (SIGNAL_FUNC) cmd_update);
	command_unbind("update check", (SIGNAL_FUNC) cmd_update_check);
	signal_remove("irssi init finished", (SIGNAL_FUNC) sig_init_finished);
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);
}
