/*
 backup-auto.c : automatic backups of the home (backup_auto, /BACKUP)

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
#include <irssi/src/fe-common/core/backup-auto.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define BACKUP_REASON_MAX 200
#define BACKUP_FILE_MAX 4096
#define BACKUP_STATE_FILE "backup-auto.state"
#define BACKUP_STATE_MAX 16384
/* the tick re-reads the clock at least this often (s), as the update
   check's does: a suspended laptop does not count the time it slept */
#define BACKUP_TICK_MAX 900
#define BACKUP_NAME_PREFIX "erssi-auto-"
#define BACKUP_NAME_SUFFIX ".tar.gz"
#define CAPSICUM_REASON "not possible in capability mode (Capsicum): erssi cannot start " \
	"the installer there - use erssi --backup from a shell"

/* ── timing and names ────────────────────────────────────────────────── */

gint64 backup_auto_interval(BACKUP_AUTO_MODE mode)
{
	switch (mode) {
	case BACKUP_AUTO_DAILY:
		return 24 * 60 * 60;
	case BACKUP_AUTO_WEEKLY:
		return 7 * 24 * 60 * 60;
	default:
		return 0;
	}
}

gint64 backup_auto_due_in(gint64 now, BACKUP_AUTO_MODE mode, gint64 last_success,
                          gint64 last_attempt)
{
	gint64 interval = backup_auto_interval(mode), next;

	if (interval == 0)
		return -1;
	next = now;
	if (last_success > 0 && last_success <= now)
		next = last_success + interval;
	/* a failed (or cut short) try since: not again before the retry delay */
	if (last_attempt > last_success && last_attempt <= now)
		next = MAX(next, last_attempt + BACKUP_AUTO_RETRY);
	return next > now ? next - now : 0;
}

char *backup_auto_name(gint64 t)
{
	GDateTime *dt = g_date_time_new_from_unix_local(t);
	char *name;

	if (dt == NULL)
		return g_strdup(BACKUP_NAME_PREFIX "19700101-000000" BACKUP_NAME_SUFFIX);
	name = g_date_time_format(dt, BACKUP_NAME_PREFIX "%Y%m%d-%H%M%S" BACKUP_NAME_SUFFIX);
	g_date_time_unref(dt);
	return name;
}

gboolean backup_auto_name_valid(const char *name)
{
	/* the part between prefix and suffix: YYYYMMDD-HHMMSS */
	static const char shape[] = "dddddddd-dddddd";
	gsize plen = strlen(BACKUP_NAME_PREFIX), i;

	if (name == NULL || strlen(name) != plen + strlen(shape) + strlen(BACKUP_NAME_SUFFIX))
		return FALSE;
	if (strncmp(name, BACKUP_NAME_PREFIX, plen) != 0 ||
	    strcmp(name + plen + strlen(shape), BACKUP_NAME_SUFFIX) != 0)
		return FALSE;
	for (i = 0; shape[i] != '\0'; i++) {
		char c = name[plen + i];

		if (shape[i] == 'd' ? !g_ascii_isdigit(c) : c != shape[i])
			return FALSE;
	}
	return TRUE;
}

static int name_newer_first(gconstpointer a, gconstpointer b)
{
	return -strcmp(*(const char *const *) a, *(const char *const *) b);
}

char **backup_auto_rotate(const char *const *names, int keep, const char *newest)
{
	GPtrArray *found = g_ptr_array_new(), *out = g_ptr_array_new();
	gboolean has_newest = backup_auto_name_valid(newest);
	guint i, skip;

	for (; names != NULL && *names != NULL && keep >= 1; names++) {
		if (backup_auto_name_valid(*names) && g_strcmp0(*names, newest) != 0)
			g_ptr_array_add(found, (char *) *names);
	}
	g_ptr_array_sort(found, name_newer_first);
	skip = keep >= 1 ? (guint) keep - (has_newest ? 1 : 0) : found->len;
	for (i = skip; i < found->len; i++) {
		/* two equal names cannot be in one directory: none twice */
		g_ptr_array_add(out, g_strdup(found->pdata[i]));
	}
	g_ptr_array_free(found, TRUE);
	g_ptr_array_add(out, NULL);
	return (char **) g_ptr_array_free(out, FALSE);
}

/* ── the backup directory ────────────────────────────────────────────── */

/* PATH (absolute, canonical) with the links followed as far as its
   directories exist: what is still missing is added as it is named */
static char *resolve(const char *path)
{
	char *real = realpath(path, NULL), *parent, *base, *top, *res;

	if (real != NULL) {
		res = g_strdup(real);
		free(real);
		return res;
	}
	parent = g_path_get_dirname(path);
	if (strcmp(parent, path) == 0) {
		g_free(parent);
		return g_strdup(path);
	}
	base = g_path_get_basename(path);
	top = resolve(parent);
	res = g_build_filename(top, base, NULL);
	g_free(parent);
	g_free(base);
	g_free(top);
	return res;
}

static gboolean path_within(const char *path, const char *dir)
{
	gsize len = strlen(dir);

	if (strcmp(dir, "/") == 0)
		return TRUE;
	return strncmp(path, dir, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

char *backup_auto_dir_reason(const char *dir, const char *home)
{
	char *path, *canon_dir, *canon_home, *real_dir, *real_home, *reason = NULL;

	if (dir == NULL || *dir == '\0')
		return g_strdup("backup_dir is empty - /SET backup_dir ~/erssi-backups");
	path = convert_home(dir);
	if (!g_path_is_absolute(path)) {
		reason = g_strdup_printf("backup_dir %s is not an absolute path - use one that "
		                         "starts with / or ~/, e.g. ~/erssi-backups",
		                         dir);
		g_free(path);
		return reason;
	}
	canon_dir = g_canonicalize_filename(path, "/");
	canon_home = g_canonicalize_filename(home, "/");
	real_dir = resolve(canon_dir);
	real_home = resolve(canon_home);
	if (path_within(canon_dir, canon_home) || path_within(real_dir, real_home) ||
	    path_within(canon_dir, real_home) || path_within(real_dir, canon_home))
		reason = g_strdup_printf("backup_dir %s is inside the erssi home %s: each backup "
		                         "would hold the ones before it - choose a directory "
		                         "outside it",
		                         dir, home);
	g_free(path);
	g_free(canon_dir);
	g_free(canon_home);
	g_free(real_dir);
	g_free(real_home);
	return reason;
}

/* ── what the installer said ─────────────────────────────────────────── */

/* printable ASCII, at most BACKUP_REASON_MAX bytes */
static char *clean_reason(const char *text, gsize len)
{
	GString *str = g_string_sized_new(MIN(len, BACKUP_REASON_MAX) + 1);
	gsize i;

	for (i = 0; i < len && str->len < BACKUP_REASON_MAX; i++) {
		unsigned char c = text[i];

		g_string_append_c(str, c >= 0x20 && c < 0x7f ? (char) c : '?');
	}
	return g_string_free(str, FALSE);
}

/* a file name as it was given: no control characters (a line of the
   state file), at most BACKUP_FILE_MAX bytes */
static char *clean_file(const char *text)
{
	GString *str = g_string_new(NULL);
	const unsigned char *p;

	for (p = (const unsigned char *) text; *p != '\0' && str->len < BACKUP_FILE_MAX; p++)
		g_string_append_c(str, *p >= 0x20 && *p != 0x7f ? (char) *p : '?');
	return g_string_free(str, FALSE);
}

char *backup_auto_error(const char *stderr_text, int wait_status)
{
	char **lines, **l;
	const char *found = NULL;
	char *reason;

	/* the installer's die(): "  error: <what>" on stderr */
	lines = g_strsplit(stderr_text != NULL ? stderr_text : "", "\n", -1);
	for (l = lines; *l != NULL; l++) {
		const char *s = *l;

		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, "error: ", 7) == 0 && s[7] != '\0')
			found = s + 7;
	}
	if (found != NULL)
		reason = clean_reason(found, strlen(found));
	else if (WIFEXITED(wait_status))
		reason = g_strdup_printf("the installer stopped with exit code %d",
		                         WEXITSTATUS(wait_status));
	else
		reason = g_strdup_printf("the installer was stopped by signal %d",
		                         WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0);
	g_strfreev(lines);
	return reason;
}

/* ── state ───────────────────────────────────────────────────────────── */

void backup_state_clear(BACKUP_STATE *state)
{
	g_free(state->last_file);
	g_free(state->last_error);
	memset(state, 0, sizeof(*state));
}

static gboolean parse_time(const char *value, gint64 *out)
{
	char *end;
	gint64 num = g_ascii_strtoll(value, &end, 10);

	if (*value == '\0' || *end != '\0' || num <= 0)
		return FALSE;
	*out = num;
	return TRUE;
}

void backup_state_parse(const char *text, BACKUP_STATE *state)
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
		if (strcmp(*l, "last_success") == 0) {
			parse_time(value, &state->last_success);
		} else if (strcmp(*l, "last_attempt") == 0) {
			parse_time(value, &state->last_attempt);
		} else if (strcmp(*l, "failures") == 0) {
			num = g_ascii_strtoll(value, &end, 10);
			if (*value != '\0' && *end == '\0' && num >= 0 && num < 1000000)
				state->failures = (int) num;
		} else if (strcmp(*l, "last_file") == 0 && *value == '/') {
			g_free(state->last_file);
			state->last_file = clean_file(value);
		} else if (strcmp(*l, "last_error") == 0 && *value != '\0') {
			g_free(state->last_error);
			state->last_error = clean_reason(value, strlen(value));
		}
	}
	g_strfreev(lines);
}

char *backup_state_format(const BACKUP_STATE *state)
{
	GString *str = g_string_new("# erssi backup_auto (/HELP backup_auto)\n");

	g_string_append_printf(str, "last_success=%" G_GINT64_FORMAT "\n", state->last_success);
	g_string_append_printf(str, "last_attempt=%" G_GINT64_FORMAT "\n", state->last_attempt);
	g_string_append_printf(str, "failures=%d\n", state->failures);
	if (state->last_file != NULL)
		g_string_append_printf(str, "last_file=%s\n", state->last_file);
	if (state->last_error != NULL)
		g_string_append_printf(str, "last_error=%s\n", state->last_error);
	return g_string_free(str, FALSE);
}

gboolean backup_state_load(const char *path, BACKUP_STATE *state)
{
	char *data = NULL;
	gsize len;

	if (!safe_file_read(path, SAFE_FILE_OWN | SAFE_FILE_QUIET, BACKUP_STATE_MAX, &data, &len)) {
		backup_state_parse(NULL, state);
		return FALSE;
	}
	backup_state_parse(data, state);
	g_free(data);
	return TRUE;
}

gboolean backup_state_save(const char *path, const BACKUP_STATE *state, GError **error)
{
	char *text = backup_state_format(state);
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

/* ── the module ──────────────────────────────────────────────────────── */

typedef struct {
	gboolean manual;
	gint64 started;
	char *dir, *file;
} BACKUP_RUN;

static UPDATE_JOB *job;
static BACKUP_RUN *running; /* the job's data */
static guint tick_tag;
static gboolean started, capability_mode;
static BACKUP_AUTO_MODE mode_on;
static BACKUP_STATE cached;
/* what was last said about backup_dir or the installer, so that a tick
   does not say it again every 15 minutes */
static char *said;

static char *state_path(void)
{
	return g_build_filename(get_irssi_dir(), BACKUP_STATE_FILE, NULL);
}

static void state_reload(void)
{
	char *path = state_path();

	backup_state_clear(&cached);
	backup_state_load(path, &cached);
	g_free(path);
}

static void state_store(void)
{
	GError *error = NULL;
	char *path = state_path();

	/* not said: at worst one backup more after a restart */
	if (!backup_state_save(path, &cached, &error))
		g_error_free(error);
	g_free(path);
}

static BACKUP_AUTO_MODE setting_mode(void)
{
	int mode = settings_get_choice("backup_auto");

	return mode == BACKUP_AUTO_DAILY || mode == BACKUP_AUTO_WEEKLY ? mode : BACKUP_AUTO_OFF;
}

/* ~/x for a path in the user's home directory */
static char *tilde(const char *path)
{
	const char *home = g_get_home_dir();

	if (home != NULL && *home != '\0' && strcmp(home, "/") != 0 && path_within(path, home))
		return g_strconcat("~", path + strlen(home), NULL);
	return g_strdup(path);
}

static char *format_time(gint64 t)
{
	GDateTime *dt = g_date_time_new_from_unix_local(t);
	char *text;

	if (dt == NULL)
		return g_strdup("at an unknown time");
	text = g_date_time_format(dt, "%Y-%m-%d %H:%M");
	g_date_time_unref(dt);
	return text;
}

/* NULL when the installer can be run here, else why not */
static char *unavailable_reason(void)
{
	char *installer, *reason = NULL;

#ifdef HAVE_CAPSICUM
	if (capsicum_enabled())
		capability_mode = TRUE;
#endif
	if (capability_mode)
		return g_strdup(CAPSICUM_REASON);
	installer = update_check_installer();
	if (access(installer, R_OK) != 0)
		reason = g_strdup_printf("this erssi was not installed with shellter-install.sh "
		                         "(no %s), which writes the backups - copy %s yourself",
		                         installer, get_irssi_dir());
	g_free(installer);
	return reason;
}

/* why no automatic backup can be made, said once while it stays so */
static void say_once(const char *reason)
{
	if (g_strcmp0(said, reason) == 0)
		return;
	g_free(said);
	said = g_strdup(reason);
	if (reason != NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_UNAVAILABLE, reason);
}

/* the automatic backups of DIR beyond backup_keep removed: regular files
   of the automatic name only, never one behind a link */
static void rotate(const char *dir, const char *newest)
{
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	char **doomed, **r;
	const char *name;
	GDir *d;

	d = g_dir_open(dir, 0, NULL);
	if (d == NULL) {
		g_ptr_array_free(names, TRUE);
		return;
	}
	while ((name = g_dir_read_name(d)) != NULL) {
		if (backup_auto_name_valid(name))
			g_ptr_array_add(names, g_strdup(name));
	}
	g_dir_close(d);
	g_ptr_array_add(names, NULL);
	doomed = backup_auto_rotate((const char *const *) names->pdata,
	                            settings_get_int("backup_keep"), newest);
	for (r = doomed; *r != NULL; r++) {
		char *path = g_build_filename(dir, *r, NULL);
		struct stat st;

		if (lstat(path, &st) == 0 && S_ISREG(st.st_mode))
			unlink(path);
		g_free(path);
	}
	g_strfreev(doomed);
	g_ptr_array_free(names, TRUE);
}

static void tick_schedule(gint64 seconds);

static void run_free(BACKUP_RUN *run)
{
	g_free(run->dir);
	g_free(run->file);
	g_free(run);
}

static void backup_failed(gboolean manual, const char *reason)
{
	gboolean again;

	state_reload();
	again = cached.failures > 0 && g_strcmp0(cached.last_error, reason) == 0;
	cached.failures++;
	g_free(cached.last_error);
	cached.last_error = g_strdup(reason);
	state_store();
	/* an automatic one that fails the same way again is not said again */
	if (manual)
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_BACKUP_FAILED, reason);
	else if (!again)
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_BACKUP_FAILED_RETRY, reason);
}

static void backup_done(const char *failure, int wait_status, const char *out_text,
                        const char *err_text, void *data)
{
	BACKUP_RUN *run = data;
	struct stat st;
	char *reason = NULL;

	(void) out_text; /* the steps; erssi --backup shows them */
	job = NULL;
	running = NULL;
	if (failure != NULL)
		reason = g_strdup(failure);
	else if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0)
		reason = backup_auto_error(err_text, wait_status);
	else if (lstat(run->file, &st) != 0 || !S_ISREG(st.st_mode))
		reason = g_strdup("the installer said it was done, but wrote no file");

	if (reason != NULL) {
		backup_failed(run->manual, reason);
		g_free(reason);
	} else {
		char *base = g_path_get_basename(run->file), *shown = tilde(run->file);
		char *size = g_format_size_full((guint64) st.st_size, G_FORMAT_SIZE_DEFAULT);

		/* another erssi with this home may have written it meanwhile */
		state_reload();
		cached.last_success = run->started;
		cached.failures = 0;
		g_free(cached.last_error);
		cached.last_error = NULL;
		g_free(cached.last_file);
		cached.last_file = clean_file(run->file);
		state_store();
		rotate(run->dir, base);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_DONE, shown, size);
		g_free(base);
		g_free(shown);
		g_free(size);
	}
	run_free(run);
	if (mode_on != BACKUP_AUTO_OFF)
		tick_schedule(BACKUP_TICK_MAX);
}

static void backup_start(gboolean manual)
{
	const char *argv[12];
	GError *error = NULL;
	BACKUP_RUN *run;
	char *installer, *reason, *dir, *name;
	gint64 now;
	int i;

	if (job != NULL) {
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_BUSY);
		return;
	}
	reason = unavailable_reason();
	if (reason == NULL)
		reason = backup_auto_dir_reason(settings_get_str("backup_dir"), get_irssi_dir());
	if (reason != NULL) {
		if (manual)
			printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_BACKUP_FAILED, reason);
		else
			say_once(reason);
		g_free(reason);
		return;
	}
	say_once(NULL);

	now = (gint64) time(NULL);
	dir = convert_home(settings_get_str("backup_dir"));
	/* the start counts: a failed one is not tried again before the retry
	   delay, also after a restart */
	state_reload();
	cached.last_attempt = now;
	state_store();

	if (g_mkdir_with_parents(dir, 0700) != 0) {
		reason = g_strdup_printf("cannot create %s: %s", dir, g_strerror(errno));
		backup_failed(manual, reason);
		g_free(reason);
		g_free(dir);
		return;
	}
	/* made now: what it is with the links followed is checked again */
	reason = backup_auto_dir_reason(dir, get_irssi_dir());
	if (reason != NULL) {
		backup_failed(manual, reason);
		g_free(reason);
		g_free(dir);
		return;
	}

	run = g_new0(BACKUP_RUN, 1);
	run->manual = manual;
	run->started = now;
	run->dir = dir;
	/* two in one second (/BACKUP NOW twice): the next second's name */
	for (i = 0; i < 60; i++) {
		struct stat st;

		name = backup_auto_name(now + i);
		g_free(run->file);
		run->file = g_build_filename(dir, name, NULL);
		g_free(name);
		if (lstat(run->file, &st) != 0)
			break;
	}

	installer = update_check_installer();
	argv[0] = "/bin/sh";
	argv[1] = installer;
	argv[2] = "--backup";
	argv[3] = run->file;
	argv[4] = "--home";
	argv[5] = get_irssi_dir();
	argv[6] = "--prefix";
	argv[7] = get_install_prefix();
	argv[8] = "--erssi-version";
	argv[9] = PACKAGE_VERSION;
	argv[10] = NULL;

	if (manual) {
		char *shown = tilde(dir);

		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_RUNNING, shown);
		g_free(shown);
	}
	job = update_job_start_raw(argv, BACKUP_AUTO_TIMEOUT_MS, backup_done, run, &error);
	g_free(installer);
	if (job != NULL) {
		running = run;
	} else {
		backup_failed(manual, error->message);
		g_error_free(error);
		run_free(run);
	}
}

static gint64 due_now(void)
{
	return backup_auto_due_in((gint64) time(NULL), mode_on, cached.last_success,
	                          cached.last_attempt);
}

static gboolean tick(gpointer data)
{
	gint64 due;

	(void) data;
	tick_tag = 0;
	if (mode_on == BACKUP_AUTO_OFF || job != NULL)
		return G_SOURCE_REMOVE;
	due = due_now();
	if (due == 0) {
		/* another erssi with this home may have made one meanwhile */
		state_reload();
		due = due_now();
	}
	if (due == 0)
		backup_start(FALSE);
	if (job == NULL) {
		/* the start may have failed: the state says when to try again */
		due = due_now();
		tick_schedule(due > 0 ? due : BACKUP_TICK_MAX);
	}
	return G_SOURCE_REMOVE;
}

static void tick_schedule(gint64 seconds)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = g_timeout_add_seconds((guint) CLAMP(seconds, 1, BACKUP_TICK_MAX), tick, NULL);
}

/* on: the first backup a minute after the start (or when the last one is
   due, if later); why none can be made, said once */
static void mode_changed(void)
{
	char *reason;

	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
	if (mode_on == BACKUP_AUTO_OFF) {
		say_once(NULL);
		return;
	}
	reason = unavailable_reason();
	if (reason == NULL)
		reason = backup_auto_dir_reason(settings_get_str("backup_dir"), get_irssi_dir());
	say_once(reason);
	g_free(reason);
	state_reload();
	tick_schedule(MAX(BACKUP_AUTO_FIRST_DELAY, due_now()));
}

static void read_settings(void)
{
	BACKUP_AUTO_MODE mode = setting_mode();
	char *reason;

	/* before the start is complete, sig_init_finished takes it */
	if (!started)
		return;
	if (mode != mode_on) {
		mode_on = mode;
		mode_changed();
		return;
	}
	if (mode_on == BACKUP_AUTO_OFF)
		return;
	/* backup_dir changed: said at once when it cannot be used */
	reason = unavailable_reason();
	if (reason == NULL)
		reason = backup_auto_dir_reason(settings_get_str("backup_dir"), get_irssi_dir());
	say_once(reason);
	g_free(reason);
}

static void sig_init_finished(void)
{
	started = TRUE;
	mode_on = setting_mode();
	if (mode_on != BACKUP_AUTO_OFF)
		mode_changed();
}

/* Capsicum: as for the update check, a running backup is stopped and
   reaped before SIGCHLD is ignored */
static void job_stop(void)
{
	if (job != NULL)
		update_job_cancel(job); /* without calling backup_done */
	job = NULL;
	if (running != NULL)
		run_free(running);
	running = NULL;
}

static void sig_capability_entering(void)
{
	job_stop();
}

static void sig_capability_enabled(void)
{
	capability_mode = TRUE;
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
	if (mode_on != BACKUP_AUTO_OFF)
		say_once(CAPSICUM_REASON);
}

/* SYNTAX: BACKUP */
static void cmd_backup(const char *data, SERVER_REC *server, void *item)
{
	static const char *const modes[] = { "off", "daily", "weekly" };
	BACKUP_AUTO_MODE mode = setting_mode();
	char *reason, *when, *dir;

	if (*data != '\0') {
		command_runsub("backup", data, server, item);
		return;
	}
	dir = tilde(settings_get_str("backup_dir"));
	if (mode == BACKUP_AUTO_OFF) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_OFF);
	} else {
		char *keep = settings_get_int("backup_keep") < 1 ?
		             g_strdup("all") : g_strdup_printf("%d", settings_get_int("backup_keep"));

		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_ON, modes[mode], dir, keep);
		g_free(keep);
	}
	g_free(dir);
	reason = unavailable_reason();
	if (reason == NULL)
		reason = backup_auto_dir_reason(settings_get_str("backup_dir"), get_irssi_dir());
	if (reason != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_UNAVAILABLE, reason);
		g_free(reason);
	}
	state_reload();
	if (job != NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_BUSY);
	if (cached.last_success > 0) {
		char *shown = cached.last_file == NULL ? NULL : tilde(cached.last_file);
		char *file = shown == NULL ? g_strdup("") : g_strconcat(": ", shown, NULL);

		when = format_time(cached.last_success);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_LAST, when, file);
		g_free(when);
		g_free(file);
		g_free(shown);
	} else {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_NONE);
	}
	if (cached.last_error != NULL) {
		when = format_time(cached.last_attempt);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_LAST_FAILED, when,
		            cached.last_error, cached.failures);
		g_free(when);
	}
	if (mode != BACKUP_AUTO_OFF && job == NULL) {
		gint64 now = (gint64) time(NULL);
		gint64 due = backup_auto_due_in(now, mode, cached.last_success, cached.last_attempt);

		/* not before the first delay after a start */
		when = due == 0 ? g_strdup("now") : format_time(now + due);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_BACKUP_NEXT, when);
		g_free(when);
	}
}

/* SYNTAX: BACKUP NOW */
static void cmd_backup_now(void)
{
	backup_start(TRUE);
}

void backup_auto_tick_now(void)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
	tick(NULL);
}

gboolean backup_auto_running(void)
{
	return job != NULL;
}

void backup_auto_init(void)
{
	settings_add_choice("misc", "backup_auto", BACKUP_AUTO_OFF, "off;daily;weekly");
	settings_add_str("misc", "backup_dir", "~/erssi-backups");
	settings_add_int("misc", "backup_keep", 7);
	started = FALSE;
	mode_on = BACKUP_AUTO_OFF;
	memset(&cached, 0, sizeof(cached));

	command_bind("backup", NULL, (SIGNAL_FUNC) cmd_backup);
	command_bind("backup now", NULL, (SIGNAL_FUNC) cmd_backup_now);
	signal_add_last("irssi init finished", (SIGNAL_FUNC) sig_init_finished);
	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
	signal_add("capability mode entering", (SIGNAL_FUNC) sig_capability_entering);
	signal_add("capability mode enabled", (SIGNAL_FUNC) sig_capability_enabled);
}

void backup_auto_deinit(void)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
	/* /UPGRADE and /QUIT: the backup stops (the installer removes its
	   half-written file) and is reaped here; the next try waits for the
	   retry delay */
	job_stop();
	g_free(said);
	said = NULL;
	backup_state_clear(&cached);

	command_unbind("backup", (SIGNAL_FUNC) cmd_backup);
	command_unbind("backup now", (SIGNAL_FUNC) cmd_backup_now);
	signal_remove("irssi init finished", (SIGNAL_FUNC) sig_init_finished);
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);
	signal_remove("capability mode entering", (SIGNAL_FUNC) sig_capability_entering);
	signal_remove("capability mode enabled", (SIGNAL_FUNC) sig_capability_enabled);
}
