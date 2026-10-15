/*
 * mainwindows_get_line() always ends: a main window whose columns are the
 * wrong way round (last before first) once made find_left/find_right
 * match the window itself, or two such windows each other, and erssi hung
 * (the side panels reserved their columns once for every main window).
 * Each case runs in a subprocess with a time limit, so a hang fails the
 * test instead of hanging it.
 */
#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/mainwindows.h>

#include <string.h>

#define MODULE_NAME "tests"

static MAIN_WINDOW_REC wins[4];

static void set_win(int i, int first_line, int last_line, int first_column, int last_column)
{
	memset(&wins[i], 0, sizeof(wins[i]));
	wins[i].first_line = first_line;
	wins[i].last_line = last_line;
	wins[i].first_column = first_column;
	wins[i].last_column = last_column;
	wins[i].width = last_column - first_column + 1;
	wins[i].height = last_line - first_line + 1;
	mainwindows = g_slist_append(mainwindows, &wins[i]);
}

static void reset(void)
{
	g_slist_free(mainwindows);
	mainwindows = NULL;
}

/* a row of three windows, side by side: the line is all of them, from
   the left, from whichever window it is asked */
static void test_row_order(void)
{
	GSList *line;
	int i;

	set_win(0, 1, 20, 0, 29);
	set_win(1, 1, 20, 31, 59);
	set_win(2, 1, 20, 61, 99);
	for (i = 0; i < 3; i++) {
		line = mainwindows_get_line(&wins[i]);
		g_assert_cmpint(g_slist_length(line), ==, 3);
		g_assert_true(g_slist_nth_data(line, 0) == &wins[0]);
		g_assert_true(g_slist_nth_data(line, 1) == &wins[1]);
		g_assert_true(g_slist_nth_data(line, 2) == &wins[2]);
		g_slist_free(line);
	}
	reset();
}

static void inverted_self(void)
{
	GSList *line;

	set_win(0, 1, 20, 50, 10); /* last column before the first one */
	line = mainwindows_get_line(&wins[0]);
	g_assert_cmpint(g_slist_length(line), >=, 1);
	g_assert_cmpint(g_slist_length(line), <=, 1);
	g_slist_free(line);
	reset();
}

static void inverted_pair(void)
{
	GSList *line;

	/* each one's last column is before the other one's first */
	set_win(0, 1, 20, 50, 10);
	set_win(1, 1, 20, 30, 5);
	line = mainwindows_get_line(&wins[0]);
	g_assert_cmpint(g_slist_length(line), <=, 2);
	g_slist_free(line);
	line = mainwindows_get_line(&wins[1]);
	g_assert_cmpint(g_slist_length(line), <=, 2);
	g_slist_free(line);
	reset();
}

static void test_inverted_self(void)
{
	if (g_test_subprocess()) {
		inverted_self();
		return;
	}
	g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
}

static void test_inverted_pair(void)
{
	if (g_test_subprocess()) {
		inverted_pair();
		return;
	}
	g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mainwindows/get_line/row_order", test_row_order);
	g_test_add_func("/mainwindows/get_line/inverted_self", test_inverted_self);
	g_test_add_func("/mainwindows/get_line/inverted_pair", test_inverted_pair);

	return g_test_run();
}
