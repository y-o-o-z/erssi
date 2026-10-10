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

/* what shellter_defaults_init() did with the written values */
enum {
	SHELLTER_DEFAULTS_SAVED,	/* saved in the config file */
	SHELLTER_DEFAULTS_BROKEN,	/* the file has errors: this run only */
	SHELLTER_DEFAULTS_UNSAVED	/* no file yet, or the save failed */
};

/* Migrates mainconfig and saves it at once, unless the config file had
   errors (the part not read would be lost) or does not exist yet. */
void shellter_defaults_init(void);

/* The note printed after the banner (printtext text, a % in the path is
   doubled), or NULL when nothing was written. The caller frees it. */
char *shellter_defaults_note(void);

/* The note for `count' settings (NULL when 0) in the `state' above, about
   the config file `fname'. Exposed for the tests. */
char *shellter_defaults_format_note(int count, int state, const char *fname);

#endif
