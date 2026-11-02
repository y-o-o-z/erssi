#define MODULE_NAME "test-gui-mouse"
#include <irssi/src/common.h>
#include <irssi/src/core/modules.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-ansi/gui-mouse.h>

/* the keys that reach the input line: the ones the parser re-emits, and the
   ones it does not consume */
static GString *typed;
static GuiMouseEvent events[8];
static int nevents;

static void sig_key(gpointer keyp)
{
	g_string_append_unichar(typed, GPOINTER_TO_INT(keyp));
}

static gboolean on_mouse(const GuiMouseEvent *event, gpointer user_data)
{
	(void) user_data;
	if (nevents < (int) G_N_ELEMENTS(events))
		events[nevents] = *event;
	nevents++;
	return TRUE;
}

static void feed(const char *keys)
{
	const char *p;

	for (p = keys; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar key = g_utf8_get_char(p);

		if (!gui_mouse_try_parse_key(key))
			g_string_append_unichar(typed, key);
	}
}

static void feed_byte(unsigned char key)
{
	if (!gui_mouse_try_parse_key(key))
		g_string_append_unichar(typed, key);
}

static void setup(void)
{
	gui_mouse_init();
	gui_mouse_add_handler(on_mouse, NULL);
	g_string_truncate(typed, 0);
	nevents = 0;
}

static void teardown(void)
{
	gui_mouse_remove_handler(on_mouse, NULL);
}

static void test_sgr_click(void)
{
	setup();
	feed("\033[<0;5;3M");
	g_assert_cmpint(nevents, ==, 1);
	g_assert_cmpint(events[0].button, ==, MOUSE_BUTTON_LEFT);
	g_assert_true(events[0].press);
	g_assert_cmpint(events[0].x, ==, 4);
	g_assert_cmpint(events[0].y, ==, 2);
	g_assert_cmpstr(typed->str, ==, "");
	feed("hi");
	g_assert_cmpstr(typed->str, ==, "hi");
	teardown();
}

/* a report ended before its two ';' is dropped, and typing goes on */
static void test_sgr_malformed_final(void)
{
	setup();
	feed("\033[<12M");
	feed("hi\r");
	g_assert_cmpint(nevents, ==, 0);
	g_assert_cmpstr(typed->str, ==, "hi\r");
	teardown();
}

/* a report cut off by the link: what is typed after it is not swallowed */
static void test_sgr_truncated_then_typing(void)
{
	setup();
	feed("\033[<35;10");
	feed("ab\r");
	g_assert_cmpint(nevents, ==, 0);
	g_assert_cmpstr(typed->str, ==, "ab\r");
	teardown();
}

/* an endless report fills no buffer for good */
static void test_sgr_overflow(void)
{
	int i;

	setup();
	feed("\033[<");
	for (i = 0; i < 200; i++)
		feed("1");
	feed("x");
	feed("\033[<0;1;1M");
	g_assert_cmpstr(typed->str, ==, "x");
	g_assert_cmpint(nevents, ==, 1);
	teardown();
}

/* ESC [ M Cb Cx Cy, from a terminal or screen without SGR (1006) */
static void test_x10_report(void)
{
	setup();
	feed("\033[M");
	feed_byte(32 + 0);
	feed_byte(32 + 5);
	feed_byte(32 + 3);
	g_assert_cmpint(nevents, ==, 1);
	g_assert_cmpint(events[0].button, ==, MOUSE_BUTTON_LEFT);
	g_assert_true(events[0].press);
	g_assert_cmpint(events[0].x, ==, 4);
	g_assert_cmpint(events[0].y, ==, 2);
	g_assert_cmpint(events[0].raw_button, ==, 0);

	feed("\033[M");
	feed_byte(32 + 64);
	feed_byte(32 + 1);
	feed_byte(32 + 1);
	g_assert_cmpint(nevents, ==, 2);
	g_assert_cmpint(events[1].button, ==, MOUSE_WHEEL_UP);
	g_assert_cmpint(events[1].raw_button, ==, 64);

	/* a release names no button */
	feed("\033[M");
	feed_byte(32 + 3);
	feed_byte(32 + 5);
	feed_byte(32 + 3);
	g_assert_cmpint(nevents, ==, 3);
	g_assert_false(events[2].press);
	g_assert_cmpstr(typed->str, ==, "");
	teardown();
}

static void test_keys_pass_through(void)
{
	setup();
	feed("\033[A");
	feed("\033[1;5C");
	feed("\033OP");
	g_assert_cmpint(nevents, ==, 0);
	g_assert_cmpstr(typed->str, ==, "\033[A\033[1;5C\033OP");
	teardown();
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	modules_init();
	signals_init();
	typed = g_string_new(NULL);
	signal_add_first("gui key pressed", (SIGNAL_FUNC) sig_key);

	g_test_add_func("/gui-mouse/sgr-click", test_sgr_click);
	g_test_add_func("/gui-mouse/sgr-malformed-final", test_sgr_malformed_final);
	g_test_add_func("/gui-mouse/sgr-truncated-then-typing", test_sgr_truncated_then_typing);
	g_test_add_func("/gui-mouse/sgr-overflow", test_sgr_overflow);
	g_test_add_func("/gui-mouse/x10-report", test_x10_report);
	g_test_add_func("/gui-mouse/keys-pass-through", test_keys_pass_through);
	ret = g_test_run();

	signal_remove("gui key pressed", (SIGNAL_FUNC) sig_key);
	g_string_free(typed, TRUE);
	signals_deinit();
	modules_deinit();
	return ret;
}
