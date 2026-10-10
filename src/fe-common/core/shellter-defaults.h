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

/* Migrates mainconfig and saves it at once, unless the config file had
   errors (the part not read would be lost) or does not exist yet. */
void shellter_defaults_init(void);

/* The note printed after the banner (printtext text, a % in the path is
   doubled), or NULL when nothing was written. The caller frees it. */
char *shellter_defaults_note(void);

#endif
