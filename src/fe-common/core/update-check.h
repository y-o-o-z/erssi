#ifndef IRSSI_FE_COMMON_CORE_UPDATE_CHECK_H
#define IRSSI_FE_COMMON_CORE_UPDATE_CHECK_H

/* The update notice (update_check): once a day the installer installed with
 * erssi (<PKGDATADIR>/shellter-install.sh --update --check --porcelain
 * --summary) is
 * run in the background. It finds the newest release signed by the key it
 * carries, exactly as `erssi --check-update` does, and answers in one line;
 * erssi says once per new version that it is there (with the summary of
 * the release, when its tag has one) and shows it in the "update"
 * statusbar item. Nothing is installed by the check: only /UPDATE INSTALL
 * does, by running the same installer with --update in the background. */

#include <glib.h>

/* the first check after a start, and the time between checks */
#define UPDATE_CHECK_FIRST_DELAY 60
#define UPDATE_CHECK_INTERVAL (24 * 60 * 60)
/* a check that takes longer is stopped (SIGTERM, then SIGKILL) */
#define UPDATE_CHECK_TIMEOUT_MS 120000
#define UPDATE_CHECK_KILL_GRACE_MS 5000
/* failed checks in a row before erssi says so once */
#define UPDATE_CHECK_FAILURES_SAY 3
/* /UPDATE INSTALL: a source build with its tests can take 20 minutes and
   more on a slow shell box; one that takes longer is stopped */
#define UPDATE_INSTALL_TIMEOUT_MS (60 * 60 * 1000)
/* its whole output goes to this file in the erssi home (mode 0600) */
#define UPDATE_INSTALL_LOG "update-install.log"
/* the summary of a release: at most this many lines of at most
   UPDATE_SUMMARY_LINE bytes */
#define UPDATE_SUMMARY_LINES 3
#define UPDATE_SUMMARY_LINE 200

typedef enum {
	UPDATE_ANSWER_ERROR,
	UPDATE_ANSWER_UPDATE,
	UPDATE_ANSWER_CURRENT,
	UPDATE_ANSWER_UNMANAGED
} UPDATE_ANSWER_KIND;

/* installed and newest are versions (update_check_version_valid), reason a
 * printable ASCII text; what a kind does not have is NULL. summary (only
 * with "update", NULL without) is what the new release brings, at most
 * UPDATE_SUMMARY_LINES lines joined by '\n' (update_check_clean_summary).
 * exit_code is the installer's, -1 when it did not exit (a signal, the
 * timeout). */
typedef struct {
	UPDATE_ANSWER_KIND kind;
	char *installed, *newest, *reason, *summary;
	int exit_code;
} UPDATE_ANSWER;

void update_answer_clear(UPDATE_ANSWER *answer);

/* "2026.11.1": 1 to 4 numbers of 1 to 9 digits, separated by dots */
gboolean update_check_version_valid(const char *version);

/* TRUE when the valid version A is newer than the valid version B, number
 * by number ("2026.10.10" > "2026.10.9", "1.3.10" < "2026.10.0"; a
 * missing number is 0); FALSE when either is not valid */
gboolean update_check_newer(const char *a, const char *b);

/* ANSWER from what the installer printed on stdout (OUT, LEN bytes) and its
 * wait status: one line, "update A B" or "current A" with exit 0, "error
 * R" with exit 1, "unmanaged R" with exit 3; after "update A B" lines
 * "summary TEXT" may follow (more than UPDATE_SUMMARY_LINES are left out,
 * empty ones skipped). Anything else - garbage, another exit code, no
 * line, other lines, a signal - is an error with a reason that says so.
 * A reason is cut to 200 bytes; bytes that are not printable ASCII become
 * '?'. */
void update_check_parse(const char *out, gsize len, int wait_status, UPDATE_ANSWER *answer);

/* TEXT (LEN bytes) as one line of a summary, safe to print: UTF-8 (an
 * invalid byte becomes '?'), without control and format characters (C0,
 * DEL, C1, bidi and zero-width marks: removed), runs of spaces as one,
 * no space at either end, at most UPDATE_SUMMARY_LINE bytes (whole
 * characters). NULL when nothing is left. */
char *update_check_clean_summary(const char *text, gsize len);

/* What ~/.erssi/update-check.state keeps between runs (and /upgrade):
 * when the last check started, the version last announced, the newest
 * version found (NULL: none newer) and its summary (NULL: none), the
 * failed checks in a row, the reason of an "unmanaged" answer and why the
 * last check failed (NULL: it did not). */
typedef struct {
	gint64 last_check;
	char *notified;
	char *available;
	char *summary;
	int failures;
	char *unmanaged;
	char *last_error;
} UPDATE_STATE;

void update_state_clear(UPDATE_STATE *state);
/* key=value lines; unknown keys and invalid values are ignored. The
 * summary is one "summary=" line for each of its lines, cleaned again
 * when read (at most UPDATE_SUMMARY_LINES). */
void update_state_parse(const char *text, UPDATE_STATE *state);
char *update_state_format(const UPDATE_STATE *state);
/* a missing or unreadable file gives an empty state (FALSE) */
gboolean update_state_load(const char *path, UPDATE_STATE *state);
/* replaced as a whole, mode 0600 */
gboolean update_state_save(const char *path, const UPDATE_STATE *state, GError **error);

/* seconds until the next check is due, 0 = now: LAST_CHECK + 24 h. No
 * check yet, or one in the future (the clock went back), is due now. */
gint64 update_check_due_in(gint64 now, gint64 last_check);

/* the shellter-install.conf files that show that the installer made the
 * install of PREFIX: <prefix>/share/irssi/shellter-install.conf, and for
 * the binary layout <top>/releases/<version> also
 * <top>/share/irssi/shellter-install.conf. NULL-terminated, g_strfreev. */
char **update_check_conf_paths(const char *prefix);

/* One check running in the background: ARGV run in a session of its own,
 * stdin /dev/null, stdout and stderr read without blocking (64 KiB kept
 * of each). DONE is called once, from the main loop, after the process
 * exited and its output ended, or when it took longer than TIMEOUT_MS:
 * then it is sent SIGTERM, and SIGKILL after UPDATE_CHECK_KILL_GRACE_MS
 * (its whole process group), and DONE gets an error "timed out" once it
 * was reaped. The job frees itself after DONE. */
typedef struct _UPDATE_JOB UPDATE_JOB;
typedef void (*UPDATE_JOB_FUNC)(const UPDATE_ANSWER *answer, const char *stderr_text, void *data);
/* a whole line of the job's output, without its '\n' (a last one without
 * it too, at the end); FROM_STDERR tells which */
typedef void (*UPDATE_LINE_FUNC)(const char *line, gboolean from_stderr, void *data);

UPDATE_JOB *update_job_start(const char *const *argv, int timeout_ms, UPDATE_JOB_FUNC done,
                             void *data, GError **error);
/* the same, and all of its output (stdout and stderr as they come) also
 * written to LOG_FD (-1: none; not closed by the job) and given to LINE
 * line by line (NULL: not), with the same DATA */
UPDATE_JOB *update_job_start_logged(const char *const *argv, int timeout_ms, int log_fd,
                                    UPDATE_LINE_FUNC line, UPDATE_JOB_FUNC done, void *data,
                                    GError **error);
/* the same, for a program that is not the update check: DONE gets the
 * wait status and what it printed (NUL-terminated, 64 KiB at most of
 * each), or with FAILURE set ("timed out after N s", "could not be
 * waited for") no status to go by */
typedef void (*UPDATE_JOB_RAW_FUNC)(const char *failure, int wait_status, const char *stdout_text,
                                    const char *stderr_text, void *data);
UPDATE_JOB *update_job_start_raw(const char *const *argv, int timeout_ms, UPDATE_JOB_RAW_FUNC done,
                                 void *data, GError **error);
/* the process group stopped (SIGTERM, up to 2 s, then SIGKILL) while
 * its process is still there, and the process reaped here; DONE is not
 * called, the job is freed. The job's process is reaped only by the job
 * (waitpid on its pid, never a GLib child watch), and its group is sent
 * a signal only while waitpid says it has not exited, so the number can
 * never be another process's by then. */
void update_job_cancel(UPDATE_JOB *job);
/* the pid of the job's process (for the tests) */
int update_job_pid(UPDATE_JOB *job);

/* the newest version found - by the last check while update_check is on,
 * or by /UPDATE CHECK - when it is newer than this erssi, else NULL (for
 * the statusbar item; "update check changed" is emitted when it changes) */
const char *update_check_available(void);
/* the installer installed with this erssi (<PKGDATADIR>/shellter-install.sh,
 * or ERSSI_TEST_UPDATE_INSTALLER for the tests), whether it is there or not */
char *update_check_installer(void);
/* TRUE when update_check can work in this installation (for the first-run
 * setup, which offers it only then) */
gboolean update_check_possible(void);

/* the binary that /UPGRADE runs to load the release installed into the
 * install PREFIX (get_install_prefix()): <top>/bin/erssi, top being PREFIX
 * of a source install, or the <top> of <top>/releases/<version> of a
 * binary install. NULL when /UPGRADE without an argument runs it already:
 * CURRENT (the binary erssi was started as; NULL: unknown) resolves to the
 * same file. g_free. */
char *update_install_binary(const char *prefix, const char *current);

/* for the tests: the timer's work at once, and whether a check or an
 * install runs */
void update_check_tick_now(void);
gboolean update_check_running(void);
gboolean update_install_running(void);

void update_check_init(void);
void update_check_deinit(void);

#endif
