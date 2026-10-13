#include <glib.h>
#include <string.h>
#include <irssi/src/fe-ansi/term-notify.h>

static void test_detect(void)
{
	static const struct {
		TermNotifyEnv env;
		TermNotifyProto want;
	} rows[] = {
	    {{.term_program = "iTerm.app"}, TERM_NOTIFY_OSC9},
	    {{.lc_terminal = "iTerm2", .term_program = "tmux", .tmux = "/t,1,0"}, TERM_NOTIFY_OSC9},
	    {{.term_program = "WezTerm"}, TERM_NOTIFY_OSC777},
	    {{.wezterm_pane = "3"}, TERM_NOTIFY_OSC777},
	    {{.term_program = "ghostty"}, TERM_NOTIFY_OSC777},
	    {{.ghostty_resources_dir = "/x"}, TERM_NOTIFY_OSC777},
	    {{.term = "xterm-ghostty"}, TERM_NOTIFY_OSC777},
	    {{.term = "foot"}, TERM_NOTIFY_OSC777},
	    {{.term = "foot-extra"}, TERM_NOTIFY_OSC777},
	    {{.kitty_window_id = "1"}, TERM_NOTIFY_OSC9},
	    {{.term = "xterm-kitty"}, TERM_NOTIFY_OSC9},
	    {{.term = "xterm-256color"}, TERM_NOTIFY_NONE},
	    {{.term_program = "iTerm.app", .sty = "1.x"}, TERM_NOTIFY_NONE},
	    {{.term_program = "Apple_Terminal"}, TERM_NOTIFY_NONE},
	    {{.term_program = "tmux"}, TERM_NOTIFY_NONE},
	    {{0}, TERM_NOTIFY_NONE},
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++)
		g_assert_cmpint(term_notify_detect(&rows[i].env), ==, rows[i].want);
}

static void test_encode_osc9(void)
{
	char *s = term_notify_encode(TERM_NOTIFY_OSC9, FALSE, "erssi", "PM bob: hi");

	g_assert_cmpstr(s, ==, "\033]9;erssi: PM bob: hi\007");
	g_free(s);
}

static void test_encode_osc777(void)
{
	char *s = term_notify_encode(TERM_NOTIFY_OSC777, FALSE, "erssi", "#test bob: a;b");

	g_assert_cmpstr(s, ==, "\033]777;notify;erssi;#test bob: a,b\007");
	g_free(s);
}

static void test_encode_tmux(void)
{
	char *s = term_notify_encode(TERM_NOTIFY_OSC9, TRUE, "erssi", "hi");

	g_assert_cmpstr(s, ==, "\033Ptmux;\033\033]9;erssi: hi\007\033\\");
	g_free(s);
}

static void test_encode_none(void)
{
	g_assert_null(term_notify_encode(TERM_NOTIFY_NONE, FALSE, "erssi", "hi"));
	g_assert_null(term_notify_encode(TERM_NOTIFY_NONE, TRUE, "erssi", "hi"));
}

static void test_encode_hostile(void)
{
	/* an attacker must not be able to end the sequence early or open a new one */
	char *s = term_notify_encode(TERM_NOTIFY_OSC9, FALSE, "t\033\\x",
	                             "a\007\033]52;c;Zm9v\007b\x9b");
	const char *p;

	g_assert_true(g_str_has_prefix(s, "\033]9;"));
	g_assert_true(g_str_has_suffix(s, "\007"));
	for (p = s + 1; p < s + strlen(s) - 1; p++)
		g_assert_true((guchar) *p >= 0x20 && *p != 0x7f);
	g_free(s);
}

static void assert_safe(const char *s)
{
	const char *p;

	g_assert_true(g_utf8_validate(s, -1, NULL));
	for (p = s; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		g_assert_true(c >= 0x20 && c != 0x7f && !(c >= 0x80 && c <= 0x9f));
	}
}

static void test_clean_strips_controls(void)
{
	char *s = term_notify_clean("evil\033]52;c;Zm9v\007end\xc2\x9b"
	                            "2J\r\n",
	                            200, FALSE);

	g_assert_cmpstr(s, ==, "evil]52;c;Zm9vend2J");
	assert_safe(s);
	g_free(s);
}

static void test_clean_invalid_utf8(void)
{
	char *s = term_notify_clean("a\xff" "b", 200, FALSE);

	g_assert_cmpstr(s, ==, "ab");
	g_free(s);
	s = term_notify_clean("ab\xe2\x82", 200, FALSE);
	g_assert_cmpstr(s, ==, "ab");
	g_free(s);
	g_assert_cmpstr(s = term_notify_clean(NULL, 200, FALSE), ==, "");
	g_free(s);
}

static void test_clean_cut(void)
{
	GString *in = g_string_new(NULL);
	char *s;
	guint i;

	for (i = 0; i < 300; i++)
		g_string_append(in, "\xc5\xbc");
	s = term_notify_clean(in->str, 200, FALSE);
	g_assert_cmpuint(strlen(s), <=, 203);
	g_assert_true(g_utf8_validate(s, -1, NULL));
	g_assert_true(g_str_has_suffix(s, "\xe2\x80\xa6"));
	g_free(s);
	g_string_free(in, TRUE);
}

static void test_clean_cut_mixed(void)
{
	/* invalid bytes and controls around the cut boundary, 3-byte chars */
	GString *in = g_string_new(NULL);
	char *s;
	guint i;

	for (i = 0; i < 100; i++)
		g_string_append(in, "\xe2\x82\xac\xff\033\x7f\xc2\x85\xf0\x9f");
	s = term_notify_clean(in->str, 200, TRUE);
	g_assert_cmpuint(strlen(s), <=, 203);
	assert_safe(s);
	g_assert_true(g_str_has_suffix(s, "\xe2\x80\xa6"));
	g_free(s);

	/* only dropped bytes after the limit: nothing is cut, no suffix */
	s = term_notify_clean("abc\xff\033\x7f", 3, FALSE);
	g_assert_cmpstr(s, ==, "abc");
	g_free(s);
	g_string_free(in, TRUE);
}

static void test_focus_filter_out_in(void)
{
	unsigned char buf[] = "ab\033[Ocd\033[Ief";
	int focus = -1;
	int n = term_focus_filter(buf, sizeof(buf) - 1, &focus);

	g_assert_cmpint(n, ==, 6);
	g_assert_cmpint(memcmp(buf, "abcdef", 6), ==, 0);
	g_assert_cmpint(focus, ==, 1);
}

static void test_focus_filter_none(void)
{
	unsigned char buf[] = "\033[A";
	int focus = -1;

	g_assert_cmpint(term_focus_filter(buf, 3, &focus), ==, 3);
	g_assert_cmpint(memcmp(buf, "\033[A", 3), ==, 0);
	g_assert_cmpint(focus, ==, -1);
	g_assert_cmpint(term_focus_filter(buf, 0, &focus), ==, 0);
	g_assert_cmpint(focus, ==, -1);
}

static void test_focus_filter_partial(void)
{
	unsigned char buf[] = "x\033[";
	int focus = -1;

	g_assert_cmpint(term_focus_filter(buf, 3, &focus), ==, 3);
	g_assert_cmpint(memcmp(buf, "x\033[", 3), ==, 0);
	g_assert_cmpint(focus, ==, -1);
}

static void test_focus_filter_back_to_back(void)
{
	unsigned char buf[] = "\033[I\033[O";
	int focus = -1;

	g_assert_cmpint(term_focus_filter(buf, 6, &focus), ==, 0);
	g_assert_cmpint(focus, ==, 0);
}

static void test_focus_filter_at_end(void)
{
	unsigned char buf[] = "ab\033[I";
	int focus = -1;

	g_assert_cmpint(term_focus_filter(buf, 5, &focus), ==, 2);
	g_assert_cmpint(focus, ==, 1);
}

static void test_focus_filter_partial_heap(void)
{
	/* exactly sized heap buffers: reading past the end trips ASan */
	int focus = -1;
	unsigned char *buf = g_memdup2("x\033[", 3);

	g_assert_cmpint(term_focus_filter(buf, 3, &focus), ==, 3);
	g_assert_cmpint(focus, ==, -1);
	g_free(buf);

	buf = g_memdup2("\033[", 2);
	g_assert_cmpint(term_focus_filter(buf, 2, &focus), ==, 2);
	g_assert_cmpint(focus, ==, -1);
	g_free(buf);

	buf = g_memdup2("\033", 1);
	g_assert_cmpint(term_focus_filter(buf, 1, &focus), ==, 1);
	g_free(buf);
}

static void test_encode_osc777_title_semicolon(void)
{
	char *s = term_notify_encode(TERM_NOTIFY_OSC777, FALSE, "a;b", "c");

	g_assert_cmpstr(s, ==, "\033]777;notify;a,b;c\007");
	g_free(s);
}

static void test_throttle(void)
{
	gint64 last = 0;

	g_assert_true(term_notify_throttle(1000, &last, 3000));
	g_assert_cmpint(last, ==, 1000);
	g_assert_false(term_notify_throttle(1500, &last, 3000));
	g_assert_cmpint(last, ==, 1000);
	g_assert_true(term_notify_throttle(4001, &last, 3000));
	g_assert_cmpint(last, ==, 4001);
	/* exactly min_ms later is allowed, one ms earlier is not */
	g_assert_false(term_notify_throttle(7000, &last, 3000));
	g_assert_true(term_notify_throttle(7001, &last, 3000));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/term_notify/detect", test_detect);
	g_test_add_func("/term_notify/encode_osc9", test_encode_osc9);
	g_test_add_func("/term_notify/encode_osc777", test_encode_osc777);
	g_test_add_func("/term_notify/encode_tmux", test_encode_tmux);
	g_test_add_func("/term_notify/encode_none", test_encode_none);
	g_test_add_func("/term_notify/encode_hostile", test_encode_hostile);
	g_test_add_func("/term_notify/clean_strips_controls", test_clean_strips_controls);
	g_test_add_func("/term_notify/clean_invalid_utf8", test_clean_invalid_utf8);
	g_test_add_func("/term_notify/clean_cut", test_clean_cut);
	g_test_add_func("/term_notify/clean_cut_mixed", test_clean_cut_mixed);
	g_test_add_func("/term_focus/filter_out_in", test_focus_filter_out_in);
	g_test_add_func("/term_focus/filter_none", test_focus_filter_none);
	g_test_add_func("/term_focus/filter_partial", test_focus_filter_partial);
	g_test_add_func("/term_focus/filter_back_to_back", test_focus_filter_back_to_back);
	g_test_add_func("/term_focus/filter_at_end", test_focus_filter_at_end);
	g_test_add_func("/term_focus/filter_partial_heap", test_focus_filter_partial_heap);
	g_test_add_func("/term_notify/encode_osc777_title_semicolon",
	                test_encode_osc777_title_semicolon);
	g_test_add_func("/term_notify/throttle", test_throttle);
	return g_test_run();
}
