#ifndef IRSSI_FE_COMMON_CORE_BACKUP_AUTO_H
#define IRSSI_FE_COMMON_CORE_BACKUP_AUTO_H

/* Automatic backups (backup_auto, backup_dir, backup_keep, /BACKUP): once
 * a day or a week the installer installed with erssi
 * (<PKGDATADIR>/shellter-install.sh --backup FILE --home <home>) is run in
 * the background, the same backup as `erssi --backup FILE`, into
 * backup_dir/erssi-auto-YYYYMMDD-HHMMSS.tar.gz. After a backup that
 * worked, the newest backup_keep files of that name are kept and older
 * ones of that name removed; nothing else in backup_dir is touched. The
 * job is the one of the update check (update-check.h). */

#include <glib.h>

/* the first backup after a start, a retry after a failed one, and the
 * longest one may take (then SIGTERM, then SIGKILL) */
#define BACKUP_AUTO_FIRST_DELAY 60
#define BACKUP_AUTO_RETRY (60 * 60)
#define BACKUP_AUTO_TIMEOUT_MS (15 * 60 * 1000)

typedef enum {
	BACKUP_AUTO_OFF,
	BACKUP_AUTO_DAILY,
	BACKUP_AUTO_WEEKLY
} BACKUP_AUTO_MODE;

/* the seconds between two backups of MODE (0 for off) */
gint64 backup_auto_interval(BACKUP_AUTO_MODE mode);

/* Seconds until the next automatic backup is due, 0 = now, -1 = never
 * (off). Due one interval after the last backup that worked; never one
 * yet, or one in the future (the clock went back), is due now. A try
 * since then that did not work (LAST_ATTEMPT newer than LAST_SUCCESS)
 * holds the next one off until BACKUP_AUTO_RETRY after it. */
gint64 backup_auto_due_in(gint64 now, BACKUP_AUTO_MODE mode, gint64 last_success,
                          gint64 last_attempt);

/* "erssi-auto-YYYYMMDD-HHMMSS.tar.gz" of the local time T */
char *backup_auto_name(gint64 t);
/* TRUE for exactly that pattern (digits where the date and time are) */
gboolean backup_auto_name_valid(const char *name);

/* Of the file NAMES of a directory, those to remove so that KEEP
 * automatic backups stay: only names that are backup_auto_name_valid,
 * the newest by name kept. NEWEST (the backup just made, or NULL) is
 * always kept and is one of the KEEP, even when a clock that went back
 * gave it an older name. KEEP < 1 removes nothing. NULL-terminated,
 * g_strfreev, newest first. */
char **backup_auto_rotate(const char *const *names, int keep, const char *newest);

/* NULL when DIR (absolute, after ~ is expanded) can take backups of the
 * erssi home HOME, else why not: empty, relative, or HOME itself or
 * inside it (links followed as far as the directories exist) */
char *backup_auto_dir_reason(const char *dir, const char *home);

/* why a backup failed, from the installer's stderr (its last "error:"
 * line) and wait status; printable, 200 bytes at most */
char *backup_auto_error(const char *stderr_text, int wait_status);

/* What ~/.erssi/backup-auto.state keeps between runs (and /upgrade):
 * when the last backup that worked was made and its file, when the last
 * try started, the failed tries in a row and why the last one failed
 * (NULL: it did not). */
typedef struct {
	gint64 last_success;
	gint64 last_attempt;
	char *last_file;
	int failures;
	char *last_error;
} BACKUP_STATE;

void backup_state_clear(BACKUP_STATE *state);
/* key=value lines; unknown keys and invalid values are ignored */
void backup_state_parse(const char *text, BACKUP_STATE *state);
char *backup_state_format(const BACKUP_STATE *state);
/* a missing or unreadable file gives an empty state (FALSE) */
gboolean backup_state_load(const char *path, BACKUP_STATE *state);
/* replaced as a whole, mode 0600 */
gboolean backup_state_save(const char *path, const BACKUP_STATE *state, GError **error);

/* for the tests: the timer's work at once, and whether a backup runs */
void backup_auto_tick_now(void);
gboolean backup_auto_running(void);

void backup_auto_init(void);
void backup_auto_deinit(void);

#endif
