/*
 * The disk space warning (src/fe-common/core/disk-space.c): what is left
 * of the file system's free space and the user's quota, and when to warn.
 */
#include <irssi/src/common.h>
#include <irssi/src/fe-common/core/disk-space.h>

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

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/disk-space/left-fs-only", test_left_fs_only);
	g_test_add_func("/disk-space/left-quota-smaller", test_left_quota_smaller);
	g_test_add_func("/disk-space/left-unknown", test_left_unknown);
	g_test_add_func("/disk-space/warn-due", test_warn_due);
	return g_test_run();
}
