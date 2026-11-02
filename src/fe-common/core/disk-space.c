/*
 disk-space.c : a warning when little room is left for the erssi home (disk_warn)

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
#include "module-formats.h"
#include <irssi/src/core/settings.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/disk-space.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/statvfs.h>

#ifdef HAVE_QUOTACTL_FD
#include <linux/quota.h>
#include <sys/syscall.h>
#endif
#ifdef HAVE_UFS_QUOTACTL
#include <sys/param.h>
#include <ufs/ufs/quota.h>
#endif

static guint tick_tag;
static gboolean was_low;
static gint64 last_warned;

guint64 disk_space_left(gboolean fs_ok, guint64 fs_avail, gboolean quota_ok, guint64 limit,
                        guint64 used, DISK_LEFT_KIND *kind)
{
	gboolean quota = quota_ok && limit > 0;
	guint64 qleft = quota ? (used < limit ? limit - used : 0) : 0;

	if (quota && (!fs_ok || qleft <= fs_avail)) {
		*kind = DISK_LEFT_QUOTA;
		return qleft;
	}
	if (fs_ok) {
		*kind = DISK_LEFT_FS;
		return fs_avail;
	}
	*kind = DISK_LEFT_UNKNOWN;
	return 0;
}

gboolean disk_space_warn_due(gint64 now, gboolean low, gboolean was_low_before, gint64 last)
{
	if (!low)
		return FALSE;
	if (!was_low_before || last == 0)
		return TRUE;
	return now - last >= DISK_SPACE_REPEAT;
}

/* the user's quota on the file system of DIR: its limit (the soft one if
   set: past it the grace time runs out) and what is used, in bytes */
static gboolean read_quota(const char *dir, guint64 *limit, guint64 *used)
{
#if defined(HAVE_QUOTACTL_FD)
	struct if_dqblk q;
	int fd, rc;

	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd == -1)
		return FALSE;
	rc = syscall(SYS_quotactl_fd, fd, QCMD(Q_GETQUOTA, USRQUOTA), (int) getuid(), &q);
	close(fd);
	if (rc != 0 || (q.dqb_valid & (QIF_BLIMITS | QIF_SPACE)) != (QIF_BLIMITS | QIF_SPACE))
		return FALSE;
	*limit = (q.dqb_bsoftlimit != 0 ? q.dqb_bsoftlimit : q.dqb_bhardlimit) * QIF_DQBLKSIZE;
	*used = q.dqb_curspace;
	return TRUE;
#elif defined(HAVE_UFS_QUOTACTL)
	struct dqblk q;

	if (quotactl(dir, QCMD(Q_GETQUOTA, USRQUOTA), (int) getuid(), &q) != 0)
		return FALSE;
	*limit = (guint64) (q.dqb_bsoftlimit != 0 ? q.dqb_bsoftlimit : q.dqb_bhardlimit) * DEV_BSIZE;
	*used = (guint64) q.dqb_curblocks * DEV_BSIZE;
	return TRUE;
#else
	(void) dir;
	(void) limit;
	(void) used;
	return FALSE;
#endif
}

/* what is left for the home; ERSSI_TEST_DISK_LEFT (bytes, ":quota" after
   them for the quota's) stands in for both in the tests */
static guint64 home_left(DISK_LEFT_KIND *kind)
{
	const char *test = g_getenv("ERSSI_TEST_DISK_LEFT");
	const char *dir = get_irssi_dir();
	struct statvfs vfs;
	guint64 limit = 0, used = 0;
	gboolean fs_ok, quota_ok;

	if (test != NULL && *test != '\0') {
		char *end;
		guint64 n = g_ascii_strtoull(test, &end, 10);

		*kind = g_strcmp0(end, ":quota") == 0 ? DISK_LEFT_QUOTA : DISK_LEFT_FS;
		return n;
	}
	fs_ok = statvfs(dir, &vfs) == 0;
	quota_ok = read_quota(dir, &limit, &used);
	return disk_space_left(fs_ok, fs_ok ? (guint64) vfs.f_bavail * vfs.f_frsize : 0, quota_ok,
	                       limit, used, kind);
}

void disk_space_check_now(void)
{
	DISK_LEFT_KIND kind;
	guint64 left;
	gint64 threshold, now;
	gboolean low;
	char *size, *where;

	threshold = settings_get_size("disk_warn");
	if (threshold <= 0) {
		was_low = FALSE;
		return;
	}
	left = home_left(&kind);
	low = kind != DISK_LEFT_UNKNOWN && left < (guint64) threshold;
	now = g_get_real_time() / G_USEC_PER_SEC;
	if (disk_space_warn_due(now, low, was_low, last_warned)) {
		size = g_format_size(left);
		where = kind == DISK_LEFT_QUOTA ?
		            g_strdup("of your disk quota") :
		            g_strdup_printf("on the disk of %s", get_irssi_dir());
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_DISK_LOW, size, where);
		g_free(size);
		g_free(where);
		last_warned = now;
	}
	was_low = low;
}

static gboolean tick(gpointer data)
{
	disk_space_check_now();
	if (GPOINTER_TO_INT(data) == 0) {
		/* the first look, soon after the start: then every period */
		tick_tag = g_timeout_add_seconds(DISK_SPACE_PERIOD, tick, GINT_TO_POINTER(1));
		return FALSE;
	}
	return TRUE;
}

void disk_space_init(void)
{
	settings_add_size("misc", "disk_warn", "20M");
	was_low = FALSE;
	last_warned = 0;
	tick_tag = g_timeout_add_seconds(DISK_SPACE_FIRST_DELAY, tick, GINT_TO_POINTER(0));
}

void disk_space_deinit(void)
{
	if (tick_tag != 0)
		g_source_remove(tick_tag);
	tick_tag = 0;
}
