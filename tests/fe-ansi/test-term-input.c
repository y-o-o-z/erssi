/*
 * Focus reports (ESC [ I, ESC [ O) are taken out of the terminal input by
 * term_gets() before the keys are read. A report split over two reads
 * (a slow link, ssh) must still be one report, not the keys ESC [ I.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/term-ansi.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../test-home.h"

#define MODULE_NAME "tests"

static int focus_changes, last_focus;

static void sig_focus(gpointer focus)
{
	focus_changes++;
	last_focus = GPOINTER_TO_INT(focus);
}

static int pipe_in;

/* writes the chunk, then reads it the way the input handler does */
static GString *read_chunk(const char *chunk)
{
	GArray *keys = g_array_new(FALSE, FALSE, sizeof(unichar));
	GString *str = g_string_new(NULL);
	int lines = 0;
	guint i;

	g_assert_cmpint(write(pipe_in, chunk, strlen(chunk)), ==, (int) strlen(chunk));
	term_gets(keys, &lines);
	for (i = 0; i < keys->len; i++)
		g_string_append_unichar(str, g_array_index(keys, unichar, i));
	g_array_free(keys, TRUE);
	return str;
}

static void check_split(const char *first, const char *second, const char *keys1,
                        const char *keys2, int focus)
{
	GString *got;

	focus_changes = 0;
	got = read_chunk(first);
	g_assert_cmpstr(got->str, ==, keys1);
	g_string_free(got, TRUE);
	got = read_chunk(second);
	g_assert_cmpstr(got->str, ==, keys2);
	g_string_free(got, TRUE);
	g_assert_cmpint(focus_changes, ==, 1);
	g_assert_cmpint(last_focus, ==, focus);
}

static void test_focus_report_split(void)
{
	int fds[2];

	g_assert_cmpint(pipe(fds), ==, 0);
	g_assert_cmpint(dup2(fds[0], STDIN_FILENO), ==, STDIN_FILENO);
	close(fds[0]);
	pipe_in = fds[1];

	ansi_term = g_new0(ANSI_TERM, 1);
	ansi_term->out = fopen("/dev/null", "w");
	g_assert_nonnull(ansi_term->out);
	term_set_input_type(TERM_TYPE_UTF8);
	term_set_focus_reporting(TRUE);
	signal_add("terminal focus changed", (SIGNAL_FUNC) sig_focus);

	/* split after ESC [ and after ESC: lost focus, got it back */
	check_split("ab\033[", "Ocd", "ab", "cd", 0);
	check_split("x\033", "[Iy", "x", "y", 1);
	/* whole, as before */
	focus_changes = 0;
	g_string_free(read_chunk("p\033[Oq"), TRUE);
	g_assert_cmpint(focus_changes, ==, 1);
	/* other sequences come through whole, split or not */
	{
		GString *a = read_chunk("\033[");
		GString *b = read_chunk("A");

		g_assert_cmpstr(a->str, ==, "");
		g_assert_cmpstr(b->str, ==, "\033[A");
		g_string_free(a, TRUE);
		g_string_free(b, TRUE);
	}

	signal_remove("terminal focus changed", (SIGNAL_FUNC) sig_focus);
	term_set_focus_reporting(FALSE);
	/* without reporting nothing is held: ESC [ I is a key */
	{
		GString *a = read_chunk("\033[");
		GString *b = read_chunk("I");

		g_assert_cmpstr(a->str, ==, "\033[");
		g_assert_cmpstr(b->str, ==, "I");
		g_string_free(a, TRUE);
		g_string_free(b, TRUE);
	}
	fclose(ansi_term->out);
	g_free(ansi_term);
	ansi_term = NULL;
	close(pipe_in);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/term/input/focus-report-split", test_focus_report_split);

	test_home_setup();

	core_preinit(argv[0]);
	core_init();

	return g_test_run();
}
