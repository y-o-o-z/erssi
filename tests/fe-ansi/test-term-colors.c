/*
 * Tests for truecolor detection (ansi_detect_capabilities), the term_truecolor
 * setting and the 256-colour fallback of term_set_color2().
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/term-ansi.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../test-home.h"

#define MODULE_NAME "tests"

typedef struct {
	const char *term;          /* NULL: unset */
	const char *extra;         /* "VAR=value, VAR=value" */
	int max_colors;
} DETECT_ROW;

static const DETECT_ROW DETECT[] = {
	{ "xterm-256color", NULL, 16777216 },
	{ "xterm-256color", "STY=1", 256 },
	{ "linux", NULL, 256 },
	{ "xterm-16color", NULL, 256 },
	{ "vt100", NULL, 256 },
	{ "dumb", NULL, 256 },
	{ NULL, NULL, 256 },
	{ "rxvt-unicode-256color", NULL, 256 },
	{ "rxvt-unicode-24bit", NULL, 16777216 },
	{ "xterm-256color", "TERM_PROGRAM=Apple_Terminal", 256 },
	{ "xterm-256color", "TERM_PROGRAM=Apple_Terminal, COLORTERM=truecolor", 16777216 },
	{ "tmux-256color", "TMUX=/tmp/x", 16777216 },
	{ "xterm-kitty", "KITTY_PID=1, STY=1", 256 },
};

static const char *const DETECT_VARS[] = {
	"TERM", "COLORTERM", "TERM_PROGRAM", "TMUX", "STY", "KITTY_PID",
	"GHOSTTY_RESOURCES_DIR", NULL
};

static void set_env(const DETECT_ROW *row)
{
	int i;

	for (i = 0; DETECT_VARS[i] != NULL; i++)
		g_unsetenv(DETECT_VARS[i]);
	if (row->term != NULL)
		g_setenv("TERM", row->term, TRUE);
	if (row->extra != NULL) {
		char **pairs = g_strsplit(row->extra, ", ", -1);

		for (i = 0; pairs[i] != NULL; i++) {
			char *eq = strchr(pairs[i], '=');

			g_assert_nonnull(eq);
			*eq = '\0';
			g_setenv(pairs[i], eq + 1, TRUE);
		}
		g_strfreev(pairs);
	}
}

static void test_detect(void)
{
	ANSI_TERM t;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(DETECT); i++) {
		memset(&t, 0, sizeof(t));
		set_env(&DETECT[i]);
		ansi_detect_capabilities(&t);
		if (t.caps.max_colors != DETECT[i].max_colors)
			g_error("row %u (TERM=%s %s): max_colors %d, expected %d", i,
				DETECT[i].term != NULL ? DETECT[i].term : "(unset)",
				DETECT[i].extra != NULL ? DETECT[i].extra : "",
				t.caps.max_colors, DETECT[i].max_colors);
	}
	for (i = 0; DETECT_VARS[i] != NULL; i++)
		g_unsetenv(DETECT_VARS[i]);
}

/* an ANSI_TERM that writes into a memory buffer */
static char *buf;
static size_t buf_len;

static void term_open(void)
{
	ansi_term = g_new0(ANSI_TERM, 1);
	ansi_term->out = open_memstream(&buf, &buf_len);
	g_assert_nonnull(ansi_term->out);
	ansi_term->last_fg = ansi_term->last_bg = UINT_MAX;
	term_use_colors = 1;
}

static void term_close(void)
{
	fclose(ansi_term->out);
	g_free(ansi_term);
	ansi_term = NULL;
	free(buf);
	buf = NULL;
}

static void check_output(const char *expected)
{
	fflush(ansi_term->out);
	g_assert_cmpstr(buf, ==, expected);
}

static void test_fallback_256_fg(void)
{
	term_open();
	term_use_colors24 = 0;
	term_set_color2(NULL, ATTR_FGCOLOR24 | ATTR_RESETBG, 0xFF0000, 0);
	check_output("\033[38;5;196m");
	term_close();
}

static void test_fallback_256_bg(void)
{
	term_open();
	term_use_colors24 = 0;
	term_set_color2(NULL, ATTR_BGCOLOR24 | ATTR_RESETFG, 0, 0x0000FF);
	check_output("\033[48;5;21m");
	term_close();
}

static void test_fallback_black24(void)
{
	term_open();
	term_use_colors24 = 0;
	term_set_color2(NULL, ATTR_FGCOLOR24 | ATTR_RESETBG, 0, 0);
	check_output("\033[38;5;16m");
	term_close();
}

static void test_truecolor_fg(void)
{
	term_open();
	term_use_colors24 = 1;
	term_set_color2(NULL, ATTR_FGCOLOR24 | ATTR_RESETBG, 0xFF0000, 0);
	check_output("\033[38;2;255;0;0m");
	term_close();
}

/* term_window_clear() leaves the default colours, on the terminal and in
   what term_set_color2() knows of it: the same colour set again after it
   is sent again (before, only SGR 0 was sent and the colour was taken as
   still set, so the next text was drawn without it) */
static void test_window_clear_resets_colour(void)
{
	TERM_WINDOW *win;
	char *colour;

	term_open();
	term_use_colors24 = 0;
	term_width = 80;
	term_height = 24;
	ansi_term->lines_empty = g_new0(char, term_height);
	term_set_color(NULL, 4 | ATTR_RESETBG);
	fflush(ansi_term->out);
	colour = g_strdup(buf);
	g_assert_cmpstr(colour, !=, "");

	win = term_window_create(0, 2, 10, 1);
	term_window_clear(win);
	term_window_destroy(win);
	term_set_color(NULL, 4 | ATTR_RESETBG);
	fflush(ansi_term->out);
	if (!g_str_has_suffix(buf, colour))
		g_error("the colour was not set again after the clear: %s", g_strescape(buf, NULL));

	g_free(colour);
	g_free(ansi_term->lines_empty);
	term_close();
}

static void test_term_truecolor_setting(void)
{
	settings_add_choice("lookandfeel", "term_truecolor", 0, "auto;on;off");
	term_open();

	ansi_term->caps.max_colors = 256;
	settings_set_str("term_truecolor", "auto");
	g_assert_false(term_truecolor_capable());
	settings_set_str("term_truecolor", "on");
	g_assert_true(term_truecolor_capable());

	ansi_term->caps.max_colors = 16777216;
	settings_set_str("term_truecolor", "auto");
	g_assert_true(term_truecolor_capable());
	settings_set_str("term_truecolor", "off");
	g_assert_false(term_truecolor_capable());

	/* back to the default */
	settings_set_str("term_truecolor", "auto");
	term_close();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/term/detect-truecolor", test_detect);
	g_test_add_func("/term/fallback-256-fg", test_fallback_256_fg);
	g_test_add_func("/term/fallback-256-bg", test_fallback_256_bg);
	g_test_add_func("/term/fallback-black24", test_fallback_black24);
	g_test_add_func("/term/truecolor-fg", test_truecolor_fg);
	g_test_add_func("/term/term-truecolor-setting", test_term_truecolor_setting);
	g_test_add_func("/term/window-clear-resets-colour", test_window_clear_resets_colour);

	test_home_setup();

	core_preinit(argv[0]);
	core_init();

	return g_test_run();
}
