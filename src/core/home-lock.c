/*
 home-lock.c : the lock that says an erssi runs on a home

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
#include <irssi/src/core/home-lock.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int home_lock_fd = -1;

int home_lock_take(const char *dir)
{
	struct stat st;
	char *path;
	int fd, err;

	path = g_build_filename(dir, HOME_LOCK_FILE, NULL);
	/* O_NONBLOCK: a FIFO put there must not hang erssi in open() */
	fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY,
	          0600);
	err = errno;
	g_free(path);
	if (fd < 0) {
		errno = err;
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
		err = errno;
		close(fd);
		errno = err;
		return -1;
	}
	return fd;
}

void home_lock_init(void)
{
	/* nothing to say when it fails: the lock only helps --restore, which
	 * also looks for erssi processes */
	if (home_lock_fd < 0)
		home_lock_fd = home_lock_take(get_irssi_dir());
}

void home_lock_deinit(void)
{
	if (home_lock_fd >= 0)
		close(home_lock_fd);
	home_lock_fd = -1;
}
