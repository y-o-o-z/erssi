#ifndef IRSSI_CORE_HOME_LOCK_H
#define IRSSI_CORE_HOME_LOCK_H

/* "<home>/erssi.lock": every erssi holds a shared flock() on it for as
 * long as it runs, so that a tool that must not work on a home in use
 * (shellter-install.sh --restore) can tell: its exclusive lock fails
 * while any erssi holds the shared one. Several erssi on one home all
 * hold it. The descriptor is close-on-exec: /upgrade drops the lock for
 * the moment of the exec and the new process takes it again. The kernel
 * drops the lock when the process dies, so a file left by a killed erssi
 * locks nothing; the file is never removed. */

#define HOME_LOCK_FILE "erssi.lock"

/* Takes the shared lock in DIR. Returns the descriptor, or -1 (errno
 * set) when the file cannot be opened - a link or anything but a regular
 * file of the user's own in its place - or locked (a file system without
 * locks). Never waits. */
int home_lock_take(const char *dir);

void home_lock_init(void);
void home_lock_deinit(void);

#endif
