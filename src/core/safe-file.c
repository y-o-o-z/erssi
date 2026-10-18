/*
 safe-file.c : opening files at paths others may influence (see safe-file.h)

    Copyright (C) 2026 the Shellter team

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

#include <irssi/src/common.h>
#include <irssi/src/core/safe-file.h>
#ifdef HAVE_CAPSICUM
#include <irssi/src/core/capsicum.h>
#endif

#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

static int open_path(const char *path, int oflags, int mode)
{
#ifdef HAVE_CAPSICUM
	/* in capability mode only the log directory can be opened, through
	 * its descriptor */
	return capsicum_open_wrapper(path, oflags, mode);
#else
	return open(path, oflags, mode);
#endif
}

/* fd is a regular file: then it is made blocking again (O_NONBLOCK was
 * only there for open()); otherwise it is closed and errno says why */
static int check_regular(int fd, struct stat *st)
{
	int err, fl;

	if (fstat(fd, st) < 0) {
		err = errno;
	} else if (!S_ISREG(st->st_mode)) {
		err = S_ISDIR(st->st_mode) ? EISDIR : EINVAL;
	} else if ((fl = fcntl(fd, F_GETFL)) < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) {
		err = errno;
	} else {
		return fd;
	}
	close(fd);
	errno = err;
	return -1;
}

int safe_file_open_read(const char *path, int flags, struct stat *st)
{
	struct stat buf;
	int fd;

	g_return_val_if_fail(path != NULL, -1);

	fd = open_path(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC |
	               ((flags & SAFE_FILE_FOLLOW) != 0 ? 0 : O_NOFOLLOW), 0);
	if (fd < 0)
		return -1;
	return check_regular(fd, st != NULL ? st : &buf);
}

FILE *safe_file_fopen_read(const char *path, int flags)
{
	FILE *f;
	int fd, err;

	fd = safe_file_open_read(path, flags, NULL);
	if (fd < 0)
		return NULL;
	f = fdopen(fd, "r");
	if (f == NULL) {
		err = errno;
		close(fd);
		errno = err;
	}
	return f;
}

gboolean safe_file_read(const char *path, int flags, gsize max, char **data, gsize *len)
{
	struct stat st;
	GString *buf;
	char chunk[8192];
	ssize_t n;
	int fd, err;

	g_return_val_if_fail(data != NULL, FALSE);

	fd = safe_file_open_read(path, flags, &st);
	if (fd < 0)
		return FALSE;
	if ((guint64) st.st_size > max) {
		close(fd);
		errno = EFBIG;
		return FALSE;
	}
	buf = g_string_sized_new(st.st_size + 1);
	/* the size is checked again while reading: the file may grow */
	while ((n = read(fd, chunk, sizeof(chunk))) > 0 || (n < 0 && errno == EINTR)) {
		if (n <= 0)
			continue;
		if (buf->len + n > max) {
			n = -1;
			errno = EFBIG;
			break;
		}
		g_string_append_len(buf, chunk, n);
	}
	err = errno;
	close(fd);
	if (n < 0) {
		g_string_free(buf, TRUE);
		errno = err;
		return FALSE;
	}
	if (len != NULL)
		*len = buf->len;
	*data = g_string_free(buf, FALSE);
	return TRUE;
}

/* a regular file of the user's own for writing; truncated only once it
 * is known to be one (O_TRUNC in open() would empty a file of someone
 * else's before the check) */
static int open_write(const char *path, int mode, gboolean append)
{
	struct stat st;
	int fd, err;

	g_return_val_if_fail(path != NULL, -1);

	fd = open_path(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY |
	               O_CLOEXEC | (append ? O_APPEND : 0), mode);
	if (fd < 0)
		return -1;
	fd = check_regular(fd, &st);
	if (fd < 0)
		return -1;
	if (st.st_uid != geteuid()) {
		/* put there by someone else (in a shared directory): what is
		 * written would be theirs to read */
		err = EPERM;
	} else if ((st.st_mode & 07777) != (mode_t) mode && fchmod(fd, mode) < 0) {
		/* a file made with the umask's mode (an older erssi, by hand) */
		err = errno;
	} else if (!append && ftruncate(fd, 0) < 0) {
		err = errno;
	} else {
		return fd;
	}
	close(fd);
	errno = err;
	return -1;
}

int safe_file_open_append(const char *path, int mode)
{
	return open_write(path, mode, TRUE);
}

int safe_file_open_truncate(const char *path, int mode)
{
	return open_write(path, mode, FALSE);
}

gboolean safe_file_write(const char *path, const char *data, gsize len, int mode)
{
	char *tmp;
	gsize done = 0;
	int fd, ok, err;

	g_return_val_if_fail(path != NULL, FALSE);
	g_return_val_if_fail(data != NULL || len == 0, FALSE);

	/* the new file has its mode from the start: no moment with the
	 * umask's mode; the fchmod() gives it exactly, whatever the umask */
	tmp = g_strconcat(path, ".XXXXXX", NULL);
	fd = g_mkstemp_full(tmp, O_WRONLY | O_CLOEXEC, mode);
	if (fd < 0) {
		err = errno;
		g_free(tmp);
		errno = err;
		return FALSE;
	}
	ok = fchmod(fd, mode) == 0;
	while (ok && done < len) {
		ssize_t n = write(fd, data + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n == 0)
			errno = EIO;
		ok = n > 0;
		if (ok)
			done += n;
	}
	ok = ok && fsync(fd) == 0;
	err = errno;
	if (close(fd) != 0 && ok) {
		ok = FALSE;
		err = errno;
	}
	if (ok && rename(tmp, path) != 0) {
		ok = FALSE;
		err = errno;
	}
	if (!ok) {
		unlink(tmp);
		errno = err;
	}
	g_free(tmp);
	return ok;
}

const char *safe_file_strerror(int err)
{
	switch (err) {
	case ELOOP:
#if defined(EMLINK)
	case EMLINK:
#endif
#if defined(EFTYPE)
	case EFTYPE:
#endif
		return "a symbolic link (not followed)";
	case EINVAL:
		return "not a regular file";
	case EPERM:
		return "not your own file";
	case EFBIG:
		return "too large";
	default:
		return g_strerror(err);
	}
}
