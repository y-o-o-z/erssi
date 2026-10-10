/*
 * Tests for the join-time names rule (fe-channels.c): when a join prints
 * only the count line instead of the nick table.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/channels.h>
#include <irssi/src/fe-common/core/fe-channels.h>

static void test_join_count_only(void)
{
	/* limit, nicks, show_alone -> count line only? */
	g_assert_false(fe_channels_join_count_only(18, 1, TRUE));
	g_assert_true(fe_channels_join_count_only(18, 1, FALSE));
	g_assert_true(fe_channels_join_count_only(18, 0, FALSE));
	g_assert_false(fe_channels_join_count_only(18, 18, TRUE));
	g_assert_true(fe_channels_join_count_only(18, 19, TRUE));
	g_assert_true(fe_channels_join_count_only(1, 1, FALSE));
	g_assert_true(fe_channels_join_count_only(1, 5, TRUE));
	g_assert_true(fe_channels_join_count_only(0, 1, FALSE));
	g_assert_false(fe_channels_join_count_only(0, 300, TRUE));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fe_channels/join_count_only", test_join_count_only);

	return g_test_run();
}
