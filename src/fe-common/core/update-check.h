#ifndef IRSSI_FE_COMMON_CORE_UPDATE_CHECK_H
#define IRSSI_FE_COMMON_CORE_UPDATE_CHECK_H

/* The update notice (update_check): once a day the installer installed with
 * erssi (<PKGDATADIR>/shellter-install.sh --update --check --porcelain) is
 * run in the background. It finds the newest release signed by the key it
 * carries, exactly as `erssi --check-update` does, and answers in one line;
 * erssi says once per new version that it is there and shows it in the
 * "update" statusbar item. Nothing is ever installed from here. */

#include <glib.h>

/* the first check after a start, and the time between checks */
#define UPDATE_CHECK_FIRST_DELAY 60
#define UPDATE_CHECK_INTERVAL (24 * 60 * 60)
/* a check that takes longer is stopped (SIGTERM, then SIGKILL) */
#define UPDATE_CHECK_TIMEOUT_MS 120000
#define UPDATE_CHECK_KILL_GRACE_MS 5000
/* failed checks in a row before erssi says so once */
#define UPDATE_CHECK_FAILURES_SAY 3

typedef enum {
	UPDATE_ANSWER_ERROR,
	UPDATE_ANSWER_UPDATE,
	UPDATE_ANSWER_CURRENT,
	UPDATE_ANSWER_UNMANAGED
} UPDATE_ANSWER_KIND;

/* installed and newest are versions (update_check_version_valid), reason a
 * printable ASCII text; what a kind does not have is NULL */
typedef struct {
	UPDATE_ANSWER_KIND kind;
	char *installed, *newest, *reason;
} UPDATE_ANSWER;

void update_answer_clear(UPDATE_ANSWER *answer);

/* "2026.11.1": 1 to 4 numbers of 1 to 9 digits, separated by dots */
gboolean update_check_version_valid(const char *version);

/* ANSWER from what the installer printed on stdout (OUT, LEN bytes) and its
 * wait status: exactly one line, "update A B" or "current A" with exit 0,
 * "error R" with exit 1, "unmanaged R" with exit 3. Anything else -
 * garbage, another exit code, no line, a signal - is an error with a
 * reason that says so. A reason is cut to 200 bytes; bytes that are not
 * printable ASCII become '?'. */
void update_check_parse(const char *out, gsize len, int wait_status, UPDATE_ANSWER *answer);

/* What ~/.erssi/update-check.state keeps between runs (and /upgrade):
 * when the last check started, the version last announced, the newest
 * version found (NULL: none newer), the failed checks in a row, the
 * reason of an "unmanaged" answer and why the last check failed (NULL:
 * it did not). */
typedef struct {
	gint64 last_check;
	char *notified;
	char *available;
	int failures;
	char *unmanaged;
	char *last_error;
} UPDATE_STATE;

void update_state_clear(UPDATE_STATE *state);
/* key=value lines; unknown keys and invalid values are ignored */
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

UPDATE_JOB *update_job_start(const char *const *argv, int timeout_ms, UPDATE_JOB_FUNC done,
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
 * or by /UPDATE CHECK - else NULL (for the statusbar item; "update check
 * changed" is emitted when it changes) */
const char *update_check_available(void);
/* TRUE when update_check can work in this installation (for the first-run
 * setup, which offers it only then) */
gboolean update_check_possible(void);

void update_check_init(void);
void update_check_deinit(void);

#endif
