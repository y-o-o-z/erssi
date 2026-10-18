#ifndef IRSSI_CORE_SAFE_FILE_H
#define IRSSI_CORE_SAFE_FILE_H

/* Files at a path that a setting, the user or another local user may
 * influence (logs, caches, keys, files erssi installs into ~/.erssi).
 *
 * Reading: the file is opened once with O_NONBLOCK, so a FIFO or a device
 * put in its place cannot hang erssi in open(), and checked on that
 * descriptor with fstat(): anything but a regular file is refused (an
 * earlier stat() of the path could be answered by another file). A
 * symbolic link as the last component is refused unless the caller passes
 * SAFE_FILE_FOLLOW - for files the user may well keep as a link, such as
 * help pages or the themes of an installation. Links in the directories
 * above are always followed: a log or help directory that is a link to
 * another disk keeps working. The descriptor is returned blocking again.
 *
 * Writing private data: appended to (or written from the start of) a
 * regular file of the user's own with no other hard link, never through
 * a symbolic link; or
 * replaced as a whole by a new file in the same directory, written out to
 * the disk and renamed over the old one, so a crash leaves the old or the
 * new file and a link at the path is replaced, not followed.
 *
 * On failure every function returns -1, NULL or FALSE with errno set:
 * ELOOP (EMLINK or EFTYPE on BSDs) for a link that is not followed, EISDIR
 * for a directory, EINVAL for another file that is not a regular one,
 * EPERM for a file that is not the user's own or (to write) has other
 * hard links, EFBIG for a file over the size limit. safe_file_strerror()
 * names these refusals; the same errno for another reason is printed as
 * it is. */

#include <glib.h>
#include <stdio.h>
#include <sys/stat.h>

/* a symbolic link as the last component of the path is followed */
#define SAFE_FILE_FOLLOW (1 << 0)
/* reading: only a file of the user's own (the effective uid) */
#define SAFE_FILE_OWN (1 << 1)
/* with Capsicum: a plain open(), which fails quietly in capability mode
 * (for files that are looked for in several places, such as help pages) */
#define SAFE_FILE_QUIET (1 << 2)

/* a regular file opened for reading (O_CLOEXEC); st, when not NULL, gets
 * its fstat() */
int safe_file_open_read(const char *path, int flags, struct stat *st);
/* the same as a stdio stream, for libraries that read a FILE * */
FILE *safe_file_fopen_read(const char *path, int flags);
/* the whole file, NUL-terminated; more than max bytes is EFBIG */
gboolean safe_file_read(const char *path, int flags, gsize max, char **data, gsize *len);

/* a regular file of the user's own, with no other hard link, opened for
 * appending, created with mode when missing; a file with other permission
 * bits is set to mode */
int safe_file_open_append(const char *path, int mode);
/* the same, emptied: for a file written in place from its start, such as
 * a download (data that is not kept when a write fails) */
int safe_file_open_truncate(const char *path, int mode);
/* path replaced by data (see above), the new file with mode */
gboolean safe_file_write(const char *path, const char *data, gsize len, int mode);

/* errno from the last call above as text: its own refusals are named ("a
 * symbolic link (not followed)", "not a regular file", ...), anything
 * else is g_strerror() */
const char *safe_file_strerror(int err);

#endif
