#ifndef IRSSI_FE_COMMON_CORE_DISK_SPACE_H
#define IRSSI_FE_COMMON_CORE_DISK_SPACE_H

/* A warning in the Notices window when little room is left for the erssi
 * home (disk_warn, default 20M; 0 = never): the free space of its file
 * system for the user, or what the user's disk quota there leaves, the
 * smaller one. Looked at after the start and every DISK_SPACE_PERIOD; told
 * when it gets low, and again every DISK_SPACE_REPEAT while it stays so.
 * A full disk or quota is what makes logs, the E2E keyring and backups
 * fail to save on a shell account. */

#include <glib.h>

#define DISK_SPACE_FIRST_DELAY 5
#define DISK_SPACE_PERIOD (10 * 60)
#define DISK_SPACE_REPEAT (6 * 60 * 60)

typedef enum {
	DISK_LEFT_UNKNOWN, /* neither could be read */
	DISK_LEFT_FS,      /* the file system's free space */
	DISK_LEFT_QUOTA    /* the user's quota */
} DISK_LEFT_KIND;

/* the bytes left: the file system's (FS_OK, FS_AVAIL) or the quota's
 * (QUOTA_OK, the limit LIMIT, 0 = none, and USED), the smaller; *KIND
 * says which */
guint64 disk_space_left(gboolean fs_ok, guint64 fs_avail, gboolean quota_ok, guint64 limit,
                        guint64 used, DISK_LEFT_KIND *kind);

/* the bytes free for the user of a file system of BLOCKS blocks of
 * FRSIZE bytes, BAVAIL of them available: none when BAVAIL is more than
 * the whole (FreeBSD's UFS reports it negative once the reserve is used
 * too, and statvfs copies that into its unsigned field) */
guint64 disk_space_fs_avail(guint64 bavail, guint64 blocks, guint64 frsize);

/* whether to warn now: LOW now, and either not low at the last look
 * (WAS_LOW) or the last warning (LAST_WARNED, 0 = none) DISK_SPACE_REPEAT
 * ago or more */
gboolean disk_space_warn_due(gint64 now, gboolean low, gboolean was_low, gint64 last_warned);

/* one look now, as the timer does */
void disk_space_check_now(void);

void disk_space_init(void);
void disk_space_deinit(void);

#endif
