/*
 * Tests for the queue of fe-mentions.c: mentions that arrive before
 * "irssi init finished" wait (at most 100, the oldest dropped) and are
 * printed when the Mentions window is made; the "mention" signal is not
 * delayed; a deinit drops what is still queued.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/themes.h>
#include <irssi/src/fe-common/irc/fe-mentions.h>
#include <irssi/src/fe-common/irc/module-formats.h>

#include <string.h>
#include "../../test-home.h"

#define MODULE_NAME "tests"

static GPtrArray *printed; /* stripped lines printed to the Mentions window */
static int signalled;      /* "mention" emissions */

static void sig_print_text(TEXT_DEST_REC *dest, const char *text, const char *stripped)
{
	WINDOW_REC *window = window_find_name("Mentions");

	(void) text;
	if (window != NULL && dest->window == window)
		g_ptr_array_add(printed, g_strdup(stripped));
}

static void sig_mention(SERVER_REC *server, const char *kind, const char *where,
                        const char *nick, const char *text)
{
	(void) server;
	(void) kind;
	(void) where;
	(void) nick;
	(void) text;
	signalled++;
}

static void record_n(int first, int count)
{
	int i;

	for (i = first; i < first + count; i++) {
		char *text = g_strdup_printf("n%d", i);

		mentions_record(NULL, "dcc", "DCC", "DCC", "bob", text);
		g_free(text);
	}
}

/* what unloading the module does: deinit, then its settings go */
static void unload(void)
{
	fe_mentions_deinit();
	settings_remove_module("fe-common/irc");
}

static void reset_counts(void)
{
	g_ptr_array_set_size(printed, 0);
	signalled = 0;
}

static void test_held_until_init(void)
{
	reset_counts();
	g_assert_false(irssi_init_finished);

	record_n(0, 101);
	/* the signal is not delayed, the window does not exist yet */
	g_assert_cmpint(signalled, ==, 101);
	g_assert_null(window_find_name("Mentions"));
	g_assert_null(mentions_window(FALSE));
	g_assert_cmpuint(printed->len, ==, 0);

	signal_emit("irssi init finished", 0);
	g_assert_nonnull(window_find_name("Mentions"));
	g_assert_true(mentions_window(FALSE) == window_find_name("Mentions"));

	/* 100 kept, the oldest (n0) dropped, in order */
	g_assert_cmpuint(printed->len, ==, 100);
	g_assert_true(g_str_has_suffix(g_ptr_array_index(printed, 0), ": n1"));
	g_assert_true(g_str_has_suffix(g_ptr_array_index(printed, 99), ": n100"));

	/* from now on an entry is printed at once */
	record_n(101, 1);
	g_assert_cmpuint(printed->len, ==, 101);
	g_assert_true(g_str_has_suffix(g_ptr_array_index(printed, 100), ": n101"));
}

static void test_deinit_drops_queue(void)
{
	WINDOW_REC *window;

	/* back to the state before init finished, without a window */
	unload();
	window = window_find_name("Mentions");
	g_assert_nonnull(window);
	window_set_immortal(window, FALSE);
	window_destroy(window);
	irssi_init_finished = FALSE;

	fe_mentions_init();
	reset_counts();
	record_n(200, 3);
	g_assert_cmpint(signalled, ==, 3);
	g_assert_null(window_find_name("Mentions"));

	/* what was queued is dropped, not printed by the next instance */
	unload();
	fe_mentions_init();
	signal_emit("irssi init finished", 0);
	g_assert_nonnull(window_find_name("Mentions"));
	g_assert_cmpuint(printed->len, ==, 0);
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mentions_queue/held_until_init", test_held_until_init);
	g_test_add_func("/mentions_queue/deinit_drops_queue", test_deinit_drops_queue);

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	args_execute(0, NULL);
	core_init();
	fe_common_core_init();
	theme_register_module("fe-common/irc", fecommon_irc_formats);
	fe_mentions_init();

	printed = g_ptr_array_new_with_free_func(g_free);
	signal_add("print text", (SIGNAL_FUNC) sig_print_text);
	signal_add("mention", (SIGNAL_FUNC) sig_mention);

	ret = g_test_run();

	signal_remove("print text", (SIGNAL_FUNC) sig_print_text);
	signal_remove("mention", (SIGNAL_FUNC) sig_mention);
	unload();
	g_ptr_array_free(printed, TRUE);
	return ret;
}
