#ifndef IRSSI_FE_COMMON_CORE_SHELLTER_DEFAULTS_H
#define IRSSI_FE_COMMON_CORE_SHELLTER_DEFAULTS_H

#include <irssi/src/lib-config/iconfig.h>

/* erssi/defaults_version of a config that already has the Shellter Edition
   values of the settings whose built-in defaults changed in 2026.10.0 */
#define SHELLTER_DEFAULTS_VERSION 1

/* Writes those values into `config', only for the keys it lacks, and sets
   the marker; nothing when the marker is already SHELLTER_DEFAULTS_VERSION
   or newer. Returns the number of settings written. */
int shellter_defaults_migrate(CONFIG_REC *config);

/* a former default: the setting `key' of `module' gets `value' in a config
   whose erssi/defaults_version is lower than `since' */
typedef struct {
	int since;
	const char *module;
	const char *key;
	const char *value;
} SHELLTER_DEFAULT_REC;

/* shellter_defaults_migrate() with the `rows' (`rows_count' of them) of a
   defaults version `version'. Exposed for the tests. */
int shellter_defaults_migrate_rows(CONFIG_REC *config, const SHELLTER_DEFAULT_REC *rows,
                                   int rows_count, int version);

/* erssi/statusbar_version of a config whose info bar got the items added
   to the shipped one since 2026.10.0 (the e2e item), when it was the
   shipped one */
#define SHELLTER_STATUSBAR_VERSION 1

/* Adds the item e2e after window to statusbar.default.info.items when that
   list is exactly one erssi shipped in irssi.conf (a copy of the default
   config written into the file); a list the user changed, and every other
   bar, are left alone. Sets the marker; nothing when the marker is already
   SHELLTER_STATUSBAR_VERSION or newer. TRUE when the item was added. */
int shellter_statusbar_migrate(CONFIG_REC *config);

/* shellter_statusbar_migrate() up to the statusbar version `version'.
   Exposed for the tests. */
int shellter_statusbar_migrate_to(CONFIG_REC *config, int version);

/* what shellter_defaults_init() did with the written values */
enum {
	SHELLTER_DEFAULTS_SAVED,	/* saved in the config file */
	SHELLTER_DEFAULTS_BROKEN,	/* the file has errors: this run only */
	SHELLTER_DEFAULTS_SAVE_FAILED,	/* writing the file failed: until /SAVE */
	SHELLTER_DEFAULTS_NO_FILE	/* no file of the user's yet: in memory,
					   no note, not written at quit */
};

/* settings_save() of mainconfig at start, by a migration, that leaves no
   error in mainconfig: a failed write is no parse error (erssi --setup and
   the other migrations check config_last_error()). Call it only when
   config_last_error(mainconfig) is NULL. A write that lib-config reports
   as done but with an error set (a failed rename) counts as failed.
   Returns TRUE when the file was written; else `reason' (when not NULL)
   gets why, to be freed by the caller. */
int shellter_save_at_start(char **reason);

/* Migrates mainconfig (the settings, then the info bar) and saves it at
   once, unless the config file had errors (the part not read would be
   lost) or does not exist yet. A failed write leaves no error in
   mainconfig (config_last_error() stays for parse errors). */
void shellter_defaults_init(void);

/* The note printed after the banner (as the argument of printtext "%s",
   which is not expanded), or NULL when nothing was written. The caller
   frees it. */
char *shellter_defaults_note(void);

/* The note for `count' settings (NULL when 0, or in the state NO_FILE) in
   the `state' above, about the config file `fname'; `reason' is why the
   write failed (SAVE_FAILED). Exposed for the tests. */
char *shellter_defaults_format_note(int count, int state, const char *fname,
                                    const char *reason);

#endif
