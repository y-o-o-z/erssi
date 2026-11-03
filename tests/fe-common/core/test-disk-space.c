/*
 * The disk space warning (src/fe-common/core/disk-space.c): what is left
 * of the file system's free space and the user's quota, and when to warn.
 */
#include <irssi/src/common.h>
#include <irssi/src/fe-common/core/disk-space.h>

#ifdef HAVE_QUOTACTL_FD
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#endif

static void test_left_fs_only(void)
{
	DISK_LEFT_KIND kind;

	g_assert_cmpuint(disk_space_left(TRUE, 5000, FALSE, 0, 0, &kind), ==, 5000);
	g_assert_cmpint(kind, ==, DISK_LEFT_FS);
	/* a quota without a limit counts as none */
	g_assert_cmpuint(disk_space_left(TRUE, 5000, TRUE, 0, 100, &kind), ==, 5000);
	g_assert_cmpint(kind, ==, DISK_LEFT_FS);
}

static void test_left_quota_smaller(void)
{
	DISK_LEFT_KIND kind;

	g_assert_cmpuint(disk_space_left(TRUE, 1000000, TRUE, 50000, 30000, &kind), ==, 20000);
	g_assert_cmpint(kind, ==, DISK_LEFT_QUOTA);
	/* the file system smaller than what the quota leaves */
	g_assert_cmpuint(disk_space_left(TRUE, 1000, TRUE, 50000, 30000, &kind), ==, 1000);
	g_assert_cmpint(kind, ==, DISK_LEFT_FS);
	/* over the quota (past a soft limit, in its grace time): nothing */
	g_assert_cmpuint(disk_space_left(TRUE, 1000000, TRUE, 50000, 60000, &kind), ==, 0);
	g_assert_cmpint(kind, ==, DISK_LEFT_QUOTA);
	/* the quota only */
	g_assert_cmpuint(disk_space_left(FALSE, 0, TRUE, 50000, 10000, &kind), ==, 40000);
	g_assert_cmpint(kind, ==, DISK_LEFT_QUOTA);
}

static void test_left_unknown(void)
{
	DISK_LEFT_KIND kind;

	disk_space_left(FALSE, 0, FALSE, 0, 0, &kind);
	g_assert_cmpint(kind, ==, DISK_LEFT_UNKNOWN);
	disk_space_left(FALSE, 0, TRUE, 0, 0, &kind);
	g_assert_cmpint(kind, ==, DISK_LEFT_UNKNOWN);
}

/* FreeBSD's UFS reports negative available blocks once the reserve is
   used too; statvfs copies them into its unsigned field */
static void test_fs_avail(void)
{
	g_assert_cmpuint(disk_space_fs_avail(100, 1000, 4096), ==, 409600);
	g_assert_cmpuint(disk_space_fs_avail(1000, 1000, 512), ==, 512000);
	g_assert_cmpuint(disk_space_fs_avail((guint64) -5, 1000, 4096), ==, 0);
	g_assert_cmpuint(disk_space_fs_avail(1001, 1000, 4096), ==, 0);
}

static void test_warn_due(void)
{
	gint64 now = 1800000000;

	/* not low: never */
	g_assert_false(disk_space_warn_due(now, FALSE, FALSE, 0));
	g_assert_false(disk_space_warn_due(now, FALSE, TRUE, now - 10));
	/* low now, not before: at once */
	g_assert_true(disk_space_warn_due(now, TRUE, FALSE, 0));
	g_assert_true(disk_space_warn_due(now, TRUE, FALSE, now - 10));
	/* still low: again only after DISK_SPACE_REPEAT */
	g_assert_false(disk_space_warn_due(now, TRUE, TRUE, now - 10));
	g_assert_false(disk_space_warn_due(now, TRUE, TRUE, now - DISK_SPACE_REPEAT + 1));
	g_assert_true(disk_space_warn_due(now, TRUE, TRUE, now - DISK_SPACE_REPEAT));
	/* low before but never told (warnings were off) */
	g_assert_true(disk_space_warn_due(now, TRUE, TRUE, 0));
}

#ifdef HAVE_QUOTACTL_FD
/* in a child under a seccomp filter that kills on the calls NR1 and NR2:
   the quota read (or not), erssi alive; whether the quota calls were
   found allowed, -1 when no filter can be set here */
static int quota_allowed_under(long nr1, long nr2)
{
	pid_t pid;
	int status;

	pid = fork();
	g_assert_cmpint(pid, !=, -1);
	if (pid == 0) {
		struct sock_filter code[] = {
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr1, 2, 0),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr2, 1, 0),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
		};
		struct sock_fprog prog = { G_N_ELEMENTS(code), code };
		guint64 limit = 0, used = 0;

		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
		    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
			_exit(77);
		disk_space_read_quota(".", &limit, &used);
		_exit(disk_space_quota_calls_allowed() ? 10 : 11);
	}
	g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
	g_assert_false(WIFSIGNALED(status));
	g_assert_true(WIFEXITED(status));
	if (WEXITSTATUS(status) == 77)
		return -1;
	g_assert_cmpint(WEXITSTATUS(status), >=, 10);
	g_assert_cmpint(WEXITSTATUS(status), <=, 11);
	return WEXITSTATUS(status) == 10;
}

/* a filter that kills on the quota calls, as systemd's
   SystemCallFilter=@system-service does (they are in @privileged): the
   quota is not read, and erssi lives */
static void test_quota_under_killing_seccomp(void)
{
	int allowed = quota_allowed_under(SYS_quotactl_fd, SYS_quotactl);

	if (allowed == -1) {
		g_test_skip("no seccomp filter here");
		return;
	}
	g_assert_cmpint(allowed, ==, 0);
}

/* a filter that lets them through: the quota is still read */
static void test_quota_under_other_seccomp(void)
{
	int allowed;

	if (!disk_space_quota_calls_allowed()) {
		g_test_skip("the quota calls are filtered here already");
		return;
	}
	allowed = quota_allowed_under(SYS_acct, SYS_reboot);
	if (allowed == -1) {
		g_test_skip("no seccomp filter here");
		return;
	}
	g_assert_cmpint(allowed, ==, 1);
}

/* QCMD(Q_GETQUOTA, USRQUOTA) without its shift out of an int */
static void test_quota_cmd(void)
{
	g_assert_cmpuint(disk_space_quota_cmd(), ==, 0x80000700u);
}
#endif

/* the quota of the current directory: none, unlimited (0) or some,
   never a fault (under UBSan too) */
static void test_read_quota_here(void)
{
	guint64 limit = 0, used = 0;

	disk_space_read_quota(".", &limit, &used);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/disk-space/left-fs-only", test_left_fs_only);
	g_test_add_func("/disk-space/left-quota-smaller", test_left_quota_smaller);
	g_test_add_func("/disk-space/left-unknown", test_left_unknown);
	g_test_add_func("/disk-space/fs-avail", test_fs_avail);
	g_test_add_func("/disk-space/warn-due", test_warn_due);
	g_test_add_func("/disk-space/read-quota-here", test_read_quota_here);
#ifdef HAVE_QUOTACTL_FD
	g_test_add_func("/disk-space/quota-cmd", test_quota_cmd);
	g_test_add_func("/disk-space/quota-under-killing-seccomp", test_quota_under_killing_seccomp);
	g_test_add_func("/disk-space/quota-under-other-seccomp", test_quota_under_other_seccomp);
#endif
	return g_test_run();
}
