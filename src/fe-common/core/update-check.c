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
#include <irssi/src/core/session.h>
#ifdef HAVE_CAPSICUM
#include <irssi/src/core/capsicum.h>
#endif
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/module-formats.h>
#include <irssi/src/fe-common/core/update-check.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/file.h>
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
#define CAPSICUM_REASON "not possible in capability mode (Capsicum): erssi cannot start " \
	"the installer there - use erssi --check-update from a shell"
/* a line of the installer's output longer than this is cut */
#define UPDATE_LINE_MAX 4096
/* what /UPDATE INSTALL shows of the installer at most: its steps and
   warnings, and the lines of its error */
#define INSTALL_SHOWN_MAX 30
#define INSTALL_ERROR_LINES 8

/* ── answers ─────────────────────────────────────────────────────────── */

void update_answer_clear(UPDATE_ANSWER *answer)
{
	g_free(answer->installed);
	g_free(answer->newest);
	g_free(answer->reason);
	g_free(answer->summary);
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

gboolean update_check_newer(const char *a, const char *b)
{
	if (!update_check_version_valid(a) || !update_check_version_valid(b))
		return FALSE;
	while (*a != '\0' || *b != '\0') {
		/* at most 9 digits each: no overflow */
		long x = strtol(a, (char **) &a, 10), y = strtol(b, (char **) &b, 10);

		if (x != y)
			return x > y;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
	return FALSE;
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

char *update_check_clean_summary(const char *text, gsize len)
{
	const char *p = text, *end = text + len;
	GString *str;
	gboolean space = FALSE;

	str = g_string_sized_new(MIN(len, UPDATE_SUMMARY_LINE) + 1);
	while (p < end) {
		gunichar c = g_utf8_get_char_validated(p, end - p);
		char buf[6];
		int n;

		if (*p == '\0') {
			p++;
			continue;
		}
		if (c == (gunichar) -1 || c == (gunichar) -2) {
			c = '?';
			p++;
		} else {
			p = g_utf8_next_char(p);
		}
		if (c == ' ' || c == '\t') {
			space = str->len > 0;
			continue;
		}
		/* escape codes, bidi overrides, zero-width and line separators */
		if (g_unichar_iscntrl(c) || g_unichar_type(c) == G_UNICODE_FORMAT ||
		    g_unichar_type(c) == G_UNICODE_LINE_SEPARATOR ||
		    g_unichar_type(c) == G_UNICODE_PARAGRAPH_SEPARATOR)
			continue;
		n = g_unichar_to_utf8(c, buf);
		if (str->len + (space ? 1 : 0) + n > UPDATE_SUMMARY_LINE)
			break;
		if (space)
			g_string_append_c(str, ' ');
		space = FALSE;
		g_string_append_len(str, buf, n);
	}
	if (str->len == 0) {
		g_string_free(str, TRUE);
		return NULL;
	}
	return g_string_free(str, FALSE);
}

/* the lines after the first one, FROM to END (each ended by '\n'): only
   "summary TEXT" after "update"; FALSE for anything else */
static gboolean parse_summary(const char *from, const char *end, UPDATE_ANSWER *answer)
{
	GString *summary = NULL;
	int lines = 0;

	while (from < end) {
		const char *nl = memchr(from, '\n', end - from);
		char *clean;

		if (answer->kind != UPDATE_ANSWER_UPDATE || nl - from < 8 ||
		    strncmp(from, "summary ", 8) != 0) {
			if (summary != NULL)
				g_string_free(summary, TRUE);
			return FALSE;
		}
		clean = lines < UPDATE_SUMMARY_LINES ?
		            update_check_clean_summary(from + 8, nl - from - 8) :
		            NULL;
		if (clean != NULL) {
			if (summary == NULL)
				summary = g_string_new(clean);
			else
				g_string_append_printf(summary, "\n%s", clean);
			lines++;
			g_free(clean);
		}
		from = nl + 1;
	}
	if (summary != NULL)
		answer->summary = g_string_free(summary, FALSE);
	return TRUE;
}

static void parse_answer(const char *out, gsize len, int wait_status, UPDATE_ANSWER *answer)
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
	if (nl == NULL || nl == out || out[len - 1] != '\n' || memchr(out, '\0', len) != NULL) {
		/* no line, an empty first one, or not ended */
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
	/* more lines: only the summary of an update */
	if (!parse_summary(nl + 1, out + len, answer))
		answer_error(answer,
		             g_strdup_printf("unexpected answer from the installer (exit code %d)", code));
	g_strfreev(words);
	g_free(line);
}

void update_check_parse(const char *out, gsize len, int wait_status, UPDATE_ANSWER *answer)
{
	parse_answer(out, len, wait_status, answer);
	answer->exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
}

/* ── state ───────────────────────────────────────────────────────────── */

void update_state_clear(UPDATE_STATE *state)
{
	g_free(state->notified);
	g_free(state->available);
	g_free(state->summary);
	g_free(state->unmanaged);
	g_free(state->last_error);
	memset(state, 0, sizeof(*state));
}

void update_state_parse(const char *text, UPDATE_STATE *state)
{
	char **lines, **l;
	int summary_lines = 0;

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
		} else if (strcmp(*l, "summary") == 0 && summary_lines < UPDATE_SUMMARY_LINES) {
			char *clean = update_check_clean_summary(value, strlen(value));

			if (clean != NULL) {
				char *joined = state->summary == NULL ?
				                   g_strdup(clean) :
				                   g_strconcat(state->summary, "\n", clean, NULL);

				g_free(state->summary);
				state->summary = joined;
				summary_lines++;
			}
			g_free(clean);
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
	if (state->summary != NULL) {
		char **lines = g_strsplit(state->summary, "\n", UPDATE_SUMMARY_LINES + 1);
		int i;

		for (i = 0; lines[i] != NULL && i < UPDATE_SUMMARY_LINES; i++)
			g_string_append_printf(str, "summary=%s\n", lines[i]);
		g_strfreev(lines);
	}
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
	guint out_tag, err_tag, poll_tag, timeout_tag, kill_tag;
	GString *out, *err;
	gboolean exited, lost, timed_out, stopping;
	int status, timeout_ms;
	UPDATE_JOB_FUNC done;
	UPDATE_JOB_RAW_FUNC raw;
	void *data;
	/* update_job_start_logged: the log, and the partial last line of each
	   stream for LINE */
	int log_fd;
	UPDATE_LINE_FUNC line;
	GString *out_line, *err_line;
	/* in one of its callbacks (LINE, DONE, RAW) now: a callback that
	   cancels the job (a print handler that enters Capsicum mode) leaves
	   it CANCELLED, freed from the main loop once the callback is over */
	int busy;
	gboolean cancelled;
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
	if (job->poll_tag != 0)
		g_source_remove(job->poll_tag);
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
	g_string_free(job->out_line, TRUE);
	g_string_free(job->err_line, TRUE);
	g_free(job);
}

static gboolean job_free_later(gpointer data)
{
	job_free(data);
	return G_SOURCE_REMOVE;
}

static void job_finish(UPDATE_JOB *job)
{
	UPDATE_ANSWER answer;

	if (job->raw != NULL) {
		char *failure = job->timed_out ?
		                g_strdup_printf("timed out after %d s", job->timeout_ms / 1000) :
		                job->lost ? g_strdup("the installer could not be waited for") : NULL;

		job->busy++;
		job->raw(failure, job->status, job->out->str, job->err->str, job->data);
		job->busy--;
		g_free(failure);
		if (!job->cancelled)
			job_free(job);
		return;
	}
	if (job->timed_out || job->lost) {
		memset(&answer, 0, sizeof(answer));
		answer.kind = UPDATE_ANSWER_ERROR;
		answer.exit_code = -1;
		answer.reason = job->timed_out ?
		                g_strdup_printf("timed out after %d s", job->timeout_ms / 1000) :
		                g_strdup("the installer could not be waited for");
	} else {
		update_check_parse(job->out->str, job->out->len, job->status, &answer);
	}
	job->busy++;
	job->done(&answer, job->err->str, job->data);
	job->busy--;
	update_answer_clear(&answer);
	if (!job->cancelled)
		job_free(job);
}

/* LEN bytes of the output into the log (a write that fails ends the log:
   the install goes on without it) */
static void job_log(UPDATE_JOB *job, const char *buf, gsize len)
{
	while (job->log_fd != -1 && len > 0) {
		ssize_t n = write(job->log_fd, buf, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			job->log_fd = -1;
			break;
		}
		buf += n;
		len -= n;
	}
}

/* the whole lines in LINE given to the job's LINE; at the end (FLUSH) the
   last one without its '\n' too. A line too long is given cut. */
static void job_lines(UPDATE_JOB *job, GString *line, gboolean from_stderr, gboolean flush)
{
	char *nl;

	/* LINE is NULL once a line cancelled the job */
	while (job->line != NULL && (nl = memchr(line->str, '\n', line->len)) != NULL) {
		*nl = '\0';
		job->busy++;
		job->line(line->str, from_stderr, job->data);
		job->busy--;
		g_string_erase(line, 0, nl - line->str + 1);
	}
	if (job->line != NULL && (line->len >= UPDATE_LINE_MAX || (flush && line->len > 0))) {
		job->busy++;
		job->line(line->str, from_stderr, job->data);
		job->busy--;
		g_string_truncate(line, 0);
	}
}

/* reads what the job's stdout (FROM_STDERR: stderr) has now; at its end
   (or an error) closes it and sets it to -1: FALSE then */
static gboolean job_read(UPDATE_JOB *job, gboolean from_stderr)
{
	int *fd = from_stderr ? &job->err_fd : &job->out_fd;
	GString *buf = from_stderr ? job->err : job->out;
	GString *line = from_stderr ? job->err_line : job->out_line;
	char tmp[4096];
	ssize_t n;

	for (;;) {
		n = read(*fd, tmp, sizeof(tmp));
		if (n > 0) {
			if (buf->len < UPDATE_OUTPUT_MAX)
				g_string_append_len(buf, tmp, MIN((gsize) n, UPDATE_OUTPUT_MAX - buf->len));
			job_log(job, tmp, n);
			if (job->line != NULL) {
				g_string_append_len(line, tmp, n);
				job_lines(job, line, from_stderr, FALSE);
			}
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return TRUE;
		break; /* end of file, or an error */
	}
	if (job->line != NULL)
		job_lines(job, line, from_stderr, TRUE);
	close(*fd);
	*fd = -1;
	return FALSE;
}

static gboolean job_out(gint fd, GIOCondition cond, gpointer data)
{
	UPDATE_JOB *job = data;

	(void) fd;
	(void) cond;
	if (job_read(job, FALSE))
		return G_SOURCE_CONTINUE;
	job->out_tag = 0;
	return G_SOURCE_REMOVE;
}

static gboolean job_err(gint fd, GIOCondition cond, gpointer data)
{
	UPDATE_JOB *job = data;

	(void) fd;
	(void) cond;
	if (job_read(job, TRUE))
		return G_SOURCE_CONTINUE;
	job->err_tag = 0;
	return G_SOURCE_REMOVE;
}

/* The job's process is reaped here, by waitpid() on its own pid, and by
   nobody else (no GLib child watch, whose worker may reap it before its
   callback runs). A signal goes to its process group only after a
   waitpid() said it is still there: its number, which is the group's,
   cannot have been given to another process by then. A job that is
   stopped (timed out or cancelled) has what is left of its group killed
   when its process has exited, before it is reaped: as a zombie it still
   holds its number. TRUE once it is reaped (or, with SIGCHLD ignored as
   in Capsicum mode, gone). */
static gboolean job_reap(UPDATE_JOB *job)
{
	siginfo_t info;
	pid_t r;
	int status, w;

	if (job->exited)
		return TRUE;
	if (job->timed_out || job->stopping) {
		memset(&info, 0, sizeof(info));
		do
			w = waitid(P_PID, job->pid, &info, WEXITED | WNOHANG | WNOWAIT);
		while (w == -1 && errno == EINTR);
		if (w == 0 && info.si_pid == 0)
			return FALSE; /* still running */
		if (w == 0)
			kill(-job->pid, SIGKILL); /* a child it left behind */
	}
	do
		r = waitpid(job->pid, &status, WNOHANG);
	while (r == -1 && errno == EINTR);
	if (r == 0)
		return FALSE;
	job->exited = TRUE;
	if (r == job->pid)
		job->status = status;
	else
		job->lost = TRUE;
	return TRUE;
}

static void job_signal(UPDATE_JOB *job, int sig)
{
	if (!job_reap(job))
		kill(-job->pid, sig);
}

static gboolean job_poll(gpointer data)
{
	UPDATE_JOB *job = data;

	if (!job_reap(job))
		return G_SOURCE_CONTINUE;
	job->poll_tag = 0;
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
		job_read(job, FALSE);
	if (job->err_fd != -1 && !job->cancelled)
		job_read(job, TRUE);
	if (!job->cancelled)
		job_finish(job); /* closes what is still open */
	return G_SOURCE_REMOVE;
}

static gboolean job_kill(gpointer data)
{
	UPDATE_JOB *job = data;

	job->kill_tag = 0;
	job_signal(job, SIGKILL);
	return G_SOURCE_REMOVE;
}

static gboolean job_timeout(gpointer data)
{
	UPDATE_JOB *job = data;

	job->timeout_tag = 0;
	job->timed_out = TRUE;
	job_signal(job, SIGTERM);
	if (!job->exited)
		job->kill_tag = g_timeout_add(UPDATE_CHECK_KILL_GRACE_MS, job_kill, job);
	return G_SOURCE_REMOVE;
}

UPDATE_JOB *update_job_start(const char *const *argv, int timeout_ms, UPDATE_JOB_FUNC done,
                             void *data, GError **error)
{
	return update_job_start_logged(argv, timeout_ms, -1, NULL, done, data, error);
}

UPDATE_JOB *update_job_start_logged(const char *const *argv, int timeout_ms, int log_fd,
                                    UPDATE_LINE_FUNC line, UPDATE_JOB_FUNC done, void *data,
                                    GError **error)
{
	/* git rev-parse --local-env-vars, and GIT_NAMESPACE */
	static const char *const git_env[] = {
		"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_CONFIG", "GIT_CONFIG_PARAMETERS",
		"GIT_CONFIG_COUNT", "GIT_OBJECT_DIRECTORY", "GIT_DIR", "GIT_WORK_TREE",
		"GIT_IMPLICIT_WORK_TREE", "GIT_GRAFT_FILE", "GIT_INDEX_FILE",
		"GIT_NO_REPLACE_OBJECTS", "GIT_REPLACE_REF_BASE", "GIT_PREFIX", "GIT_SHALLOW_FILE",
		"GIT_COMMON_DIR", "GIT_NAMESPACE"
	};
	UPDATE_JOB *job;
	char **envp;
	int null_fd;
	gsize i;
	gboolean ok;

	null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (null_fd == -1) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot open /dev/null: %s", g_strerror(errno));
		return NULL;
	}
	/* git never asks for a password, no colours, plain ASCII messages;
	   no repository of erssi's environment (erssi started in a git
	   checkout or a hook): git would take it before -C */
	envp = g_get_environ();
	for (i = 0; i < G_N_ELEMENTS(git_env); i++)
		envp = g_environ_unsetenv(envp, git_env[i]);
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
	job->log_fd = log_fd;
	job->line = line;
	job->out_line = g_string_new(NULL);
	job->err_line = g_string_new(NULL);
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
	/* a check runs a few seconds a day, an install a few minutes now and
	   then: polling waitpid() 10 times a second costs nothing then */
	job->poll_tag = g_timeout_add(100, job_poll, job);
	job->timeout_tag = g_timeout_add(timeout_ms, job_timeout, job);
	return job;
}

UPDATE_JOB *update_job_start_raw(const char *const *argv, int timeout_ms, UPDATE_JOB_RAW_FUNC done,
                                 void *data, GError **error)
{
	UPDATE_JOB *job = update_job_start(argv, timeout_ms, NULL, data, error);

	/* it finishes from the main loop only, after this */
	if (job != NULL)
		job->raw = done;
	return job;
}

void update_job_cancel(UPDATE_JOB *job)
{
	int i, status;

	if (job->cancelled)
		return;
	/* the installer removes its temporary files on SIGTERM, once the
	   step it runs has ended: the same grace as after a timeout */
	job->stopping = TRUE;
	job_signal(job, SIGTERM);
	for (i = 0; i < UPDATE_CHECK_KILL_GRACE_MS / 50 && !job_reap(job); i++)
		g_usleep(50000);
	if (!job->exited) {
		kill(-job->pid, SIGKILL); /* not reaped just above */
		while (waitpid(job->pid, &status, 0) == -1 && errno == EINTR)
			;
		job->exited = TRUE;
	}
	if (job->busy == 0) {
		job_free(job);
		return;
	}
	/* from inside one of its callbacks: no more of them, no more
	   sources; the memory stays until the callback has returned */
	job->cancelled = TRUE;
	job->line = NULL;
	job->done = NULL;
	job->raw = NULL;
	job->log_fd = -1;
	if (job->out_tag != 0)
		g_source_remove(job->out_tag);
	if (job->err_tag != 0)
		g_source_remove(job->err_tag);
	if (job->poll_tag != 0)
		g_source_remove(job->poll_tag);
	if (job->timeout_tag != 0)
		g_source_remove(job->timeout_tag);
	if (job->kill_tag != 0)
		g_source_remove(job->kill_tag);
	job->out_tag = job->err_tag = job->poll_tag = job->timeout_tag = job->kill_tag = 0;
	g_idle_add(job_free_later, job);
}

int update_job_pid(UPDATE_JOB *job)
{
	return (int) job->pid;
}

/* ── the module ──────────────────────────────────────────────────────── */

/* who started the check that runs: the timer, /UPDATE CHECK, or /UPDATE
   INSTALL, whose install starts when it found a newer release */
enum { CHECK_AUTO, CHECK_MANUAL, CHECK_INSTALL };

static UPDATE_JOB *job;
static int job_mode;
static guint tick_tag;
static gboolean check_on, started, capability_mode;
static char *available;
static UPDATE_STATE cached;

/* /UPDATE INSTALL, from its start to its end (install_end): the log,
   open and locked (flock) all the while, so that a second erssi with this
   home does not run one into the same log; the installer's own run, once
   the check found a release; the lines of its error */
static gboolean installing, install_upgrade;
static UPDATE_JOB *install_job;
static int install_fd = -1;
static char *install_log, *install_newest;
static gint64 install_started;
static int install_shown;
static GPtrArray *install_errors;
static gboolean install_in_error;
static guint upgrade_tag;
static char *upgrade_binary;

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

/* VERSION, unless this erssi is that release or a newer one already (it
   was updated and /UPGRADEd since the check that found it) */
static void set_available(const char *version)
{
	if (!update_check_newer(version, PACKAGE_VERSION))
		version = NULL;
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

/* the summary of the release, a line each, under the notice */
static void print_summary(const char *summary)
{
	char **lines, **l;

	if (summary == NULL)
		return;
	lines = g_strsplit(summary, "\n", UPDATE_SUMMARY_LINES + 1);
	for (l = lines; *l != NULL && l - lines < UPDATE_SUMMARY_LINES; l++)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_SUMMARY, *l);
	g_strfreev(lines);
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

char *update_check_installer(void)
{
	gboolean test;

	return installer_path(&test);
}

/* NULL when a check can run here, else why not */
static char *unavailable_reason(void)
{
	char *installer, **confs, **c, *reason = NULL;
	const char *foreign = NULL;
	gboolean test, found = FALSE;

#ifdef HAVE_CAPSICUM
	if (capsicum_enabled())
		capability_mode = TRUE;
#endif
	if (capability_mode)
		return g_strdup(CAPSICUM_REASON);
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

char *update_install_binary(const char *prefix, const char *current)
{
	char *parent, *base, *top, *binary, *real_new, *real_cur;
	gboolean same;

	parent = g_path_get_dirname(prefix);
	base = g_path_get_basename(parent);
	top = strcmp(base, "releases") == 0 && strcmp(parent, prefix) != 0 ?
	          g_path_get_dirname(parent) :
	          g_strdup(prefix);
	binary = g_build_filename(top, "bin", "erssi", NULL);
	g_free(parent);
	g_free(base);
	g_free(top);
	if (current == NULL)
		return binary;
	real_new = realpath(binary, NULL);
	real_cur = realpath(current, NULL);
	same = real_new != NULL && real_cur != NULL && strcmp(real_new, real_cur) == 0;
	free(real_new);
	free(real_cur);
	if (same) {
		g_free(binary);
		return NULL;
	}
	return binary;
}

/* ── /UPDATE INSTALL ─────────────────────────────────────────────────── */

static void install_log_line(const char *text)
{
	if (install_fd != -1 && write(install_fd, text, strlen(text)) < 0) {
		/* the log is for reading afterwards; the install goes on */
	}
}

/* the end of an install, whatever it was: the log closed (its lock goes) */
static void install_end(void)
{
	installing = FALSE;
	install_job = NULL;
	if (install_fd != -1)
		close(install_fd);
	install_fd = -1;
	g_free(install_newest);
	install_newest = NULL;
	if (install_errors != NULL)
		g_ptr_array_free(install_errors, TRUE);
	install_errors = NULL;
	install_in_error = FALSE;
	install_shown = 0;
}

static void install_failed(const char *reason)
{
	char *line = g_strdup_printf("# /UPDATE INSTALL failed: %s\n", reason);
	const char *first = reason;
	guint i = 0;

	install_log_line(line);
	g_free(line);
	/* the installer's own error first, when it said one */
	if (install_errors != NULL && install_errors->len > 0) {
		first = g_ptr_array_index(install_errors, 0);
		i = 1;
	}
	printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_INSTALL_FAILED, first, install_log);
	for (; install_errors != NULL && i < install_errors->len; i++)
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_INSTALL_PROGRESS,
		            (char *) g_ptr_array_index(install_errors, i));
	if (first != reason)
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_INSTALL_PROGRESS, reason);
	install_end();
}

/* The installer's output, a line at a time, all of it in the log already:
   shown are its steps ("  [3/6] Building  ok 2m 04s") and warnings
   ("        ! ..."), at most INSTALL_SHOWN_MAX lines, and kept for the end
   is its error ("  error: ..." on stderr, up to the empty line after it).
   Nothing else - the build itself writes to the installer's own log. */
static void install_line(const char *line, gboolean from_stderr, void *data)
{
	const char *p = line;
	char *clean;

	(void) data;
	while (*p == ' ')
		p++;
	if (install_in_error) {
		if (*p == '\0') {
			install_in_error = FALSE;
			return;
		}
		clean = update_check_clean_summary(p, strlen(p));
		if (clean != NULL && install_errors->len < INSTALL_ERROR_LINES)
			g_ptr_array_add(install_errors, clean);
		else
			g_free(clean);
		return;
	}
	if (from_stderr && strncmp(p, "error:", 6) == 0) {
		install_in_error = TRUE;
		p += 6;
		clean = update_check_clean_summary(p, strlen(p));
		if (clean != NULL && install_errors->len < INSTALL_ERROR_LINES)
			g_ptr_array_add(install_errors, clean);
		else
			g_free(clean);
		return;
	}
	if (!((p - line == 2 && *p == '[' && g_ascii_isdigit(p[1])) ||
	      (p - line == 8 && p[0] == '!' && p[1] == ' ')))
		return;
	if (install_shown++ >= INSTALL_SHOWN_MAX)
		return;
	clean = update_check_clean_summary(p, strlen(p));
	if (clean != NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_PROGRESS, clean);
	g_free(clean);
}

/* VERSION= of the conf the installer wrote (for a binary install the one
   of the layout's top first: the prefix of this erssi is its release's);
   with a stand-in installer the conf next to it. NULL: none readable. */
static char *installed_version(void)
{
	char **confs, *installer, *version = NULL;
	gboolean test;
	int i;

	installer = installer_path(&test);
	if (test) {
		char *dir = g_path_get_dirname(installer);

		confs = g_new0(char *, 2);
		confs[0] = g_build_filename(dir, "shellter-install.conf", NULL);
		g_free(dir);
	} else {
		confs = update_check_conf_paths(get_install_prefix());
	}
	g_free(installer);
	for (i = (int) g_strv_length(confs) - 1; i >= 0 && version == NULL; i--) {
		char *data = NULL, **lines, **l;
		gsize len;

		if (!safe_file_read(confs[i], SAFE_FILE_OWN | SAFE_FILE_QUIET, 65536, &data, &len))
			continue;
		lines = g_strsplit(data, "\n", -1);
		for (l = lines; *l != NULL; l++) {
			if (strncmp(*l, "VERSION=", 8) == 0 && update_check_version_valid(*l + 8)) {
				g_free(version);
				version = g_strdup(*l + 8);
			}
		}
		g_strfreev(lines);
		g_free(data);
	}
	g_strfreev(confs);
	return version;
}

static gboolean upgrade_now(gpointer data)
{
	char *binary = upgrade_binary;

	(void) data;
	upgrade_tag = 0;
	upgrade_binary = NULL;
	signal_emit("command upgrade", 3, binary == NULL ? "" : binary, NULL, NULL);
	g_free(binary);
	return G_SOURCE_REMOVE;
}

static void install_done(const UPDATE_ANSWER *answer, const char *err_text, void *data)
{
	char *version, *binary, *line;
	gboolean test;

	(void) err_text; /* in the log */
	(void) data;
	install_job = NULL;
	if (answer->exit_code != 0) {
		char *reason = answer->exit_code > 0 ?
		                   g_strdup_printf("the installer ended with exit code %d",
		                                   answer->exit_code) :
		                   g_strdup(answer->reason);

		install_failed(reason);
		g_free(reason);
		return;
	}
	version = installed_version();
	if (version == NULL)
		version = g_strdup(install_newest);
	if (!update_check_newer(version, PACKAGE_VERSION)) {
		line = g_strdup_printf("the installer ended without an error, but erssi %s is "
		                       "installed, not a newer one",
		                       version);
		install_failed(line);
		g_free(line);
		g_free(version);
		return;
	}
	line = g_strdup_printf("# /UPDATE INSTALL: erssi %s is installed\n", version);
	install_log_line(line);
	g_free(line);

	g_free(installer_path(&test));
	/* the binary /UPGRADE runs without an argument is the one erssi was
	   started as: the new release when that is the install's bin/erssi
	   (or the ~/.local/bin link to it), else it is named */
	binary = test ? NULL : update_install_binary(get_install_prefix(), irssi_binary);
	if (install_upgrade) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_UPGRADING, version,
		            install_log);
		g_free(upgrade_binary);
		upgrade_binary = binary;
		binary = NULL;
		/* after this job is gone: /UPGRADE ends the main loop */
		if (upgrade_tag == 0)
			upgrade_tag = g_idle_add(upgrade_now, NULL);
	} else if (binary == NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALLED, version,
		            install_log);
	} else {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALLED_PATH, version,
		            install_log, binary);
	}
	g_free(binary);
	g_free(version);
	install_end();
}

/* the check of /UPDATE INSTALL found ANSWER: a newer release is installed
   by the installer with --update, in the background, else it ends */
static void install_after_check(const UPDATE_ANSWER *answer)
{
	const char *argv[6];
	GError *error = NULL;
	char *installer, *line;
	gboolean test;

	switch (answer->kind) {
	case UPDATE_ANSWER_CURRENT:
		install_log_line("# /UPDATE INSTALL: nothing newer\n");
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_CURRENT,
		            answer->installed);
		install_end();
		return;
	case UPDATE_ANSWER_UNMANAGED:
		/* the installer would run that tool: not from inside erssi */
		line = g_strdup_printf("this erssi is %s - erssi does not run that; update it "
		                       "that way",
		                       answer->reason);
		install_failed(line);
		g_free(line);
		return;
	case UPDATE_ANSWER_ERROR:
		install_failed(answer->reason);
		return;
	case UPDATE_ANSWER_UPDATE:
		break;
	}
	install_newest = g_strdup(answer->newest);
	installer = installer_path(&test);
	argv[0] = "/bin/sh";
	argv[1] = installer;
	argv[2] = "--update";
	argv[3] = "--prefix";
	argv[4] = get_install_prefix();
	argv[5] = NULL;
	line = g_strdup_printf("# /UPDATE INSTALL: erssi %s over %s: %s --update --prefix %s\n",
	                       answer->newest, answer->installed, installer, argv[4]);
	install_log_line(line);
	g_free(line);
	/* stdin /dev/null and no terminal (a session of its own): the
	   installer asks nothing - as root it stops for missing packages
	   instead of asking, as without --yes */
	install_job = update_job_start_logged(argv, UPDATE_INSTALL_TIMEOUT_MS, install_fd,
	                                      install_line, install_done, NULL, &error);
	g_free(installer);
	if (install_job == NULL) {
		install_failed(error->message);
		g_error_free(error);
		return;
	}
	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_START, answer->installed,
	            answer->newest, install_log);
	print_summary(answer->summary);
}

/* ── the check ───────────────────────────────────────────────────────── */

static void tick_schedule(gint64 seconds);

/* An installer before 2026.10.9 (one installed after this erssi started:
   a rollback) does not know --summary and exits 2 for it. Once it did,
   this process asks without it (until a restart or /UPGRADE), and the
   check is run again at once without it: the one line, no summary. */
static gboolean no_summary, job_summary, retrying;

static void check_start(int mode);

static void check_done(const UPDATE_ANSWER *answer, const char *err_text, void *data)
{
	int mode = GPOINTER_TO_INT(data);
	gboolean manual = mode == CHECK_MANUAL;

	(void) err_text; /* the steps; erssi --check-update shows them */
	job = NULL;
	if (job_summary && answer->exit_code == 2) {
		no_summary = TRUE;
		if (mode != CHECK_INSTALL || installing) {
			retrying = TRUE;
			check_start(mode);
			retrying = FALSE;
		}
		return;
	}
	/* another erssi with this home may have written it meanwhile */
	state_reload();
	switch (answer->kind) {
	case UPDATE_ANSWER_UPDATE:
		cached.failures = 0;
		g_free(cached.unmanaged);
		cached.unmanaged = NULL;
		g_free(cached.available);
		cached.available = g_strdup(answer->newest);
		g_free(cached.summary);
		cached.summary = g_strdup(answer->summary);
		if (manual || (mode == CHECK_AUTO && g_strcmp0(cached.notified, answer->newest) != 0)) {
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_AVAILABLE,
			            answer->installed, answer->newest);
			print_summary(answer->summary);
		}
		g_free(cached.notified);
		cached.notified = g_strdup(answer->newest);
		break;
	case UPDATE_ANSWER_CURRENT:
		cached.failures = 0;
		g_free(cached.unmanaged);
		cached.unmanaged = NULL;
		g_free(cached.available);
		cached.available = NULL;
		g_free(cached.summary);
		cached.summary = NULL;
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_CURRENT,
			            answer->installed);
		break;
	case UPDATE_ANSWER_UNMANAGED:
		if (manual || (mode == CHECK_AUTO && g_strcmp0(cached.unmanaged, answer->reason) != 0))
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE,
			            answer->reason);
		g_free(cached.unmanaged);
		cached.unmanaged = g_strdup(answer->reason);
		g_free(cached.available);
		cached.available = NULL;
		g_free(cached.summary);
		cached.summary = NULL;
		break;
	case UPDATE_ANSWER_ERROR:
		cached.failures++;
		g_free(cached.last_error);
		cached.last_error = g_strdup(answer->reason);
		/* a moved release tag is for the user to report at once (an
		   install says why it failed itself) */
		if (manual || (mode == CHECK_AUTO && strstr(answer->reason, "report it") != NULL))
			printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_FAILED,
			            answer->reason);
		else if (mode == CHECK_AUTO && cached.failures == UPDATE_CHECK_FAILURES_SAY)
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
	/* a print handler may have stopped the install meanwhile (Capsicum):
	   then there is no log and nothing to start */
	if (mode == CHECK_INSTALL && installing)
		install_after_check(answer);
	if (check_on)
		tick_schedule(UPDATE_TICK_MAX);
}

static void check_start(int mode)
{
	const char *argv[9];
	GError *error = NULL;
	char *installer, *reason;
	gboolean test, manual = mode == CHECK_MANUAL;
	int n = 5;

	if (job != NULL || (installing && mode != CHECK_INSTALL)) {
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_BUSY);
		return;
	}
	reason = unavailable_reason();
	if (reason != NULL) {
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_UNAVAILABLE, reason);
		if (mode == CHECK_INSTALL)
			install_failed(reason);
		g_free(reason);
		return;
	}
	installer = installer_path(&test);
	argv[0] = "/bin/sh";
	argv[1] = installer;
	argv[2] = "--update";
	argv[3] = "--check";
	argv[4] = "--porcelain";
	/* the summary lines only when asked: an erssi before 2026.10.9 that
	   runs a newer installer takes exactly one line */
	if (!no_summary)
		argv[n++] = "--summary";
	argv[n++] = "--prefix";
	argv[n++] = get_install_prefix();
	argv[n] = NULL;
	job_summary = !no_summary;

	/* the start is what counts: a check cut short by /UPGRADE or /QUIT
	   is not run again right after the next start */
	state_reload();
	cached.last_check = (gint64) time(NULL);
	state_store();

	if (manual && !retrying)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_CHECKING);
	job_mode = mode;
	job = update_job_start_logged(argv, UPDATE_CHECK_TIMEOUT_MS,
	                              mode == CHECK_INSTALL ? install_fd : -1, NULL, check_done,
	                              GINT_TO_POINTER(mode), &error);
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
		if (mode == CHECK_INSTALL)
			install_failed(error->message);
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
	if (installing) {
		/* again once the install is over */
		tick_schedule(UPDATE_TICK_MAX);
		return G_SOURCE_REMOVE;
	}
	due = update_check_due_in((gint64) time(NULL), cached.last_check);
	if (due == 0) {
		/* another erssi with this home may have checked meanwhile: then
		   what it found is shown here too */
		state_reload();
		set_available(cached.available);
		due = update_check_due_in((gint64) time(NULL), cached.last_check);
	}
	/* also after an "unmanaged" answer: the conf may name no other tool
	   any more (an install with shellter-install.sh again) */
	if (due == 0)
		check_start(CHECK_AUTO);
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

/* an install stopped (Capsicum, /QUIT, /UPGRADE): said in
   the log; the installer removes what it had not finished on SIGTERM */
static void install_stop(const char *why)
{
	char *line;

	if (!installing)
		return;
	line = g_strdup_printf("# /UPDATE INSTALL stopped: %s\n", why);
	install_log_line(line);
	g_free(line);
	if (install_job != NULL)
		update_job_cancel(install_job);
	install_job = NULL;
	install_end();
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
		/* the check of /UPDATE INSTALL is the user's, not the timer's */
		if (job != NULL && job_mode != CHECK_INSTALL) {
			update_job_cancel(job);
			job = NULL;
		}
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

/* Capsicum: SIGCHLD is ignored from here on, so a running check would never
   be waited for (and its group cannot be signalled in capability mode):
   it is stopped and reaped now, an install too */
static void sig_capability_entering(void)
{
	if (job != NULL)
		update_job_cancel(job);
	job = NULL;
	install_stop("erssi entered capability mode (Capsicum)");
}

static void sig_capability_enabled(void)
{
	capability_mode = TRUE;
	if (check_on)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE,
		            CAPSICUM_REASON);
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
	if (installing) {
		GDateTime *dt = g_date_time_new_from_unix_local(install_started);

		when = dt == NULL ? g_strdup("a while ago") : g_date_time_format(dt, "%H:%M");
		if (dt != NULL)
			g_date_time_unref(dt);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_RUNNING, when,
		            install_log);
		g_free(when);
	} else if (job != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_BUSY);
	}
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
	if (cached.unmanaged != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_UNAVAILABLE,
		            cached.unmanaged);
	} else if (update_check_newer(cached.available, PACKAGE_VERSION)) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_AVAILABLE, PACKAGE_VERSION,
		            cached.available);
		print_summary(cached.summary);
	}
}

void update_check_tick_now(void)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
	tick(NULL);
}

gboolean update_check_running(void)
{
	return job != NULL;
}

gboolean update_install_running(void)
{
	return installing;
}

/* SYNTAX: UPDATE CHECK */
static void cmd_update_check(void)
{
	check_start(CHECK_MANUAL);
}

/* the log of the install, made empty and locked; -1 with ERRNO when it
   cannot be (EWOULDBLOCK: another erssi with this home runs one) */
static int install_log_open(const char *path)
{
	struct stat st;
	int fd, err;

	fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC, 0600);
	if (fd == -1)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
		close(fd);
		errno = EPERM;
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0 || fchmod(fd, 0600) != 0 || ftruncate(fd, 0) != 0) {
		err = errno;
		close(fd);
		errno = err;
		return -1;
	}
	return fd;
}

/* SYNTAX: UPDATE INSTALL [-upgrade] */
static void cmd_update_install(const char *data)
{
	GHashTable *optlist;
	void *free_arg;
	gboolean upgrade;
	char *reason, *line, *when;
	GDateTime *dt;

	if (!cmd_get_params(data, &free_arg, PARAM_FLAG_OPTIONS, "update install", &optlist))
		return;
	upgrade = g_hash_table_lookup(optlist, "upgrade") != NULL;
	cmd_params_free(free_arg);

	g_free(install_log);
	install_log = g_build_filename(get_irssi_dir(), UPDATE_INSTALL_LOG, NULL);
	if (installing) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_BUSY, install_log);
		return;
	}
	if (job != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_REFUSED,
		            "an update check is running - try again when it is done");
		return;
	}
	reason = unavailable_reason();
	if (reason != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_INSTALL_REFUSED, reason);
		g_free(reason);
		return;
	}
	install_fd = install_log_open(install_log);
	if (install_fd == -1) {
		reason = errno == EWOULDBLOCK || errno == EAGAIN ?
		             g_strdup_printf("another erssi with this home is installing an update "
		                             "(log: %s)",
		                             install_log) :
		             g_strdup_printf("cannot write %s: %s", install_log, g_strerror(errno));
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_UPDATE_INSTALL_REFUSED, reason);
		g_free(reason);
		return;
	}
	installing = TRUE;
	install_upgrade = upgrade;
	install_started = (gint64) time(NULL);
	install_errors = g_ptr_array_new_with_free_func(g_free);
	install_in_error = FALSE;
	install_shown = 0;
	dt = g_date_time_new_from_unix_local(install_started);
	when = dt == NULL ? g_strdup("?") : g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
	if (dt != NULL)
		g_date_time_unref(dt);
	line = g_strdup_printf("# /UPDATE INSTALL%s by erssi %s, %s\n", upgrade ? " -upgrade" : "",
	                       PACKAGE_VERSION, when);
	install_log_line(line);
	g_free(line);
	g_free(when);
	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_UPDATE_INSTALL_CHECKING, install_log);
	check_start(CHECK_INSTALL);
}

void update_check_init(void)
{
	settings_add_bool("misc", "update_check", FALSE);
	check_on = started = FALSE;
	memset(&cached, 0, sizeof(cached));

	command_bind("update", NULL, (SIGNAL_FUNC) cmd_update);
	command_bind("update check", NULL, (SIGNAL_FUNC) cmd_update_check);
	command_bind("update install", NULL, (SIGNAL_FUNC) cmd_update_install);
	command_set_options("update install", "upgrade");
	signal_add_last("irssi init finished", (SIGNAL_FUNC) sig_init_finished);
	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
	signal_add("capability mode entering", (SIGNAL_FUNC) sig_capability_entering);
	signal_add("capability mode enabled", (SIGNAL_FUNC) sig_capability_enabled);
}

void update_check_deinit(void)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
	if (upgrade_tag != 0)
		g_source_remove(upgrade_tag);
	upgrade_tag = 0;
	g_free(upgrade_binary);
	upgrade_binary = NULL;
	/* /UPGRADE and /QUIT: the check stops and is reaped here, so the new
	   process gets no zombie; its start is in the state file. An install
	   stops too: the installer leaves the release in use as it was */
	if (job != NULL)
		update_job_cancel(job);
	job = NULL;
	install_stop("erssi quit or ran /UPGRADE");
	g_free(install_log);
	install_log = NULL;
	g_free(available);
	available = NULL;
	update_state_clear(&cached);

	command_unbind("update", (SIGNAL_FUNC) cmd_update);
	command_unbind("update check", (SIGNAL_FUNC) cmd_update_check);
	command_unbind("update install", (SIGNAL_FUNC) cmd_update_install);
	signal_remove("irssi init finished", (SIGNAL_FUNC) sig_init_finished);
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);
	signal_remove("capability mode entering", (SIGNAL_FUNC) sig_capability_entering);
	signal_remove("capability mode enabled", (SIGNAL_FUNC) sig_capability_enabled);
}
