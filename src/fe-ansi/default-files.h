#ifndef IRSSI_FE_ANSI_DEFAULT_FILES_H
#define IRSSI_FE_ANSI_DEFAULT_FILES_H

/* Files erssi ships into ~/.erssi (themes, startup) are updated like
 * configuration files of a package: replaced by the new version only while
 * the user has not changed them - their sha256 is the one erssi recorded
 * when it installed them (<dir>/default-files.sha256) or one of a version
 * erssi ever shipped under that name (known). A changed file is never
 * touched; when a new version ships, one note says where it is. Symbolic
 * links and anything that is not a regular file are left alone. */

#include <glib.h>

/* a larger <dir>/default-files.sha256 is not read (as if there were none) */
#define DEFAULT_FILES_LIST_MAX (64 * 1024)

typedef enum {
	DEFAULT_FILE_COPIED,   /* was missing */
	DEFAULT_FILE_CURRENT,  /* already the shipped version */
	DEFAULT_FILE_UPDATED,  /* unchanged by the user: replaced */
	DEFAULT_FILE_KEPT,     /* changed by the user: kept (note added once per shipped version) */
	DEFAULT_FILE_SKIPPED   /* a link, directory, FIFO or unreadable */
} DEFAULT_FILE_RESULT;

typedef struct {
	char *dir;
	GHashTable *hashes;                 /* name -> recorded sha256 */
	const char *const (*known)[2];      /* { name, sha256 } ..., { NULL, NULL } */
	gboolean hashes_changed, themes_changed;
	GSList *notes;                      /* char *, '%' doubled for printtext */
} DEFAULT_FILES_REC;

/* reads <dir>/default-files.sha256; known lists every shipped version */
DEFAULT_FILES_REC *default_files_open(const char *dir, const char *const known[][2]);
/* <dir>/<name> from shipped_path: copied when missing, replaced while
 * unchanged by the user, otherwise kept */
DEFAULT_FILE_RESULT default_files_update(DEFAULT_FILES_REC *rec, const char *shipped_path,
                                         const char *name, gboolean theme);
/* writes <dir>/default-files.sha256 (mode 0600 from the start) when changed; frees rec and its notes */
void default_files_close(DEFAULT_FILES_REC *rec);
/* a regular file's contents, never through a link: O_NOFOLLOW | O_NONBLOCK, checked with fstat */
gboolean default_files_read_regular(const char *path, char **data, gsize *len);

#endif
