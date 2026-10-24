/*
 * The text output primitives (term_add_unichar, term_addstr, term_addch)
 * must never hand the terminal a control byte, whatever the terminal type:
 * channel names and nicks come from the network, and the side panels draw
 * them code point by code point. An ESC or a C1 byte in the output starts an
 * escape sequence; OSC 52 writes the user's clipboard.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/term-ansi.h>
#include <irssi/src/fe-ansi/sidepanels-text.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../test-home.h"

#define MODULE_NAME "tests"

static char *buf;
static size_t buf_len;

/* an ANSI_TERM that writes into a memory buffer, cursor already in place so
 * that the output holds only the text */
static void term_open(int type)
{
	term_type = type;
	term_width = 1000;
	term_height = 2;
	ansi_term = g_new0(ANSI_TERM, 1);
	ansi_term->out = open_memstream(&buf, &buf_len);
	g_assert_nonnull(ansi_term->out);
	ansi_term->lines_empty = g_new0(char, term_height);
	ansi_term->last_fg = ansi_term->last_bg = UINT_MAX;
}

static const char *term_output(void)
{
	fflush(ansi_term->out);
	return buf;
}

static void term_close(void)
{
	fclose(ansi_term->out);
	g_free(ansi_term->lines_empty);
	g_free(ansi_term);
	ansi_term = NULL;
	free(buf);
	buf = NULL;
	term_type = TERM_TYPE_UTF8;
}

static const char *type_name(int type)
{
	return type == TERM_TYPE_UTF8 ? "utf8" : type == TERM_TYPE_BIG5 ? "big5" : "8bit";
}

/* no C0, no DEL, no C1 in the output, as the terminal of that type reads it */
static void assert_no_control(int type, const char *what, const char *name)
{
	const unsigned char *out = (const unsigned char *) term_output();
	size_t i;

	for (i = 0; i < buf_len; i++) {
		if (out[i] < 0x20 || out[i] == 0x7f ||
		    (type != TERM_TYPE_UTF8 && out[i] >= 0x80 && out[i] <= 0x9f))
			g_error("%s, %s terminal, name %s: control byte 0x%02x at %zu",
				what, type_name(type), g_strescape(name, NULL), out[i], i);
	}
	if (type == TERM_TYPE_UTF8) {
		const char *p;

		if (!g_utf8_validate((const char *) out, buf_len, NULL))
			g_error("%s, utf8 terminal, name %s: invalid UTF-8 in the output",
				what, g_strescape(name, NULL));
		for (p = (const char *) out; *p != '\0'; p = g_utf8_next_char(p)) {
			gunichar c = g_utf8_get_char(p);

			if (c >= 0x80 && c <= 0x9f)
				g_error("%s, utf8 terminal, name %s: C1 U+%04X in the output",
					what, g_strescape(name, NULL), c);
		}
	}
}

/* what the side panel does with each character (draw_formatted_char()) */
static gboolean add_unichar(unichar chr, int width, int color, unsigned int fg24,
                            unsigned int bg24, void *data)
{
	term_add_unichar(NULL, chr);
	return TRUE;
}

/* Channel names that hide an OSC 52 (set clipboard) sequence, each aimed at
 * one way a code point or byte could turn into ESC / OSC / ST / BEL:
 * the low byte of a code point, a UTF-8 encoded C1, a raw C1 byte, a raw
 * C0, a Big5 lead byte in front of a control, a Big5 pair with a C1 trail. */
static const char *const OSC52_NAMES[] = {
	"#\xd0\x9b]52;c;SGk=\xd0\x9b\\",     /* U+041B: low byte 0x1B */
	"#\xc6\x9d" "52;c;SGk=\xc6\x9c",     /* U+019D/U+019C: 0x9D OSC, 0x9C ST */
	"#\xc2\x9d" "52;c;SGk=\xc2\x9c",     /* U+009D/U+009C themselves */
	"#\x9d" "52;c;SGk=\x9c",             /* raw C1 bytes */
	"#\x1b]52;c;SGk=\x07",               /* raw ESC and BEL */
	"#\x1b]52;c;SGk=\x1b\\",             /* raw ESC, ESC \ */
	"#\x7f\x1b]52;c;SGk=\x07",           /* DEL too */
	"#\xa4\x1b]52;c;SGk=\xa4\x07",       /* Big5 lead byte before ESC */
	"#\xa4\x9d" "52;c;SGk=\xa4\x9c",     /* Big5 pairs with a C1 trail */
	"#\x9d\x9d" "52;c;SGk=\x9c\x9c",     /* C1 as a Big5 lead byte */
	"#\xc4\x9b]52;c;SGk=\xc4\x9b\\",     /* U+011B: low byte 0x1B */
};

static const int TYPES[] = { TERM_TYPE_UTF8, TERM_TYPE_8BIT, TERM_TYPE_BIG5 };

static void test_osc52_side_panel(void)
{
	guint t, i;

	for (t = 0; t < G_N_ELEMENTS(TYPES); t++) {
		for (i = 0; i < G_N_ELEMENTS(OSC52_NAMES); i++) {
			term_open(TYPES[t]);
			sidepanel_text_walk(OSC52_NAMES[i], TYPES[t], add_unichar, NULL);
			assert_no_control(TYPES[t], "side panel", OSC52_NAMES[i]);
			g_assert_null(strstr(term_output(), "\033]"));
			term_close();
		}
	}
}

/* the side panel's walk is only one caller: term_add_unichar() itself must
 * refuse, so that no other caller can get it wrong */
static void test_osc52_unichar(void)
{
	static const unichar CHARS[] = {
		0x1b, 0x07, 0x7f, 0x9b, 0x9c, 0x9d, 0x90, 0x41b, 0x11b, 0x19d, 0x19c,
		0x1001b, 0xa41b, 0xa49d, 0x9d9d, 0x110000, 0xd800, 0x7fffffff,
	};
	guint t, i;

	for (t = 0; t < G_N_ELEMENTS(TYPES); t++) {
		term_open(TYPES[t]);
		for (i = 0; i < G_N_ELEMENTS(CHARS); i++)
			term_add_unichar(NULL, CHARS[i]);
		assert_no_control(TYPES[t], "term_add_unichar", "(code points)");
		term_close();
	}
}

/* term_addstr() (status bar, prompts, anything printed outside a window)
 * and term_addch() get raw strings */
static void test_osc52_addstr_addch(void)
{
	guint t, i;
	const char *p;

	for (t = 0; t < G_N_ELEMENTS(TYPES); t++) {
		for (i = 0; i < G_N_ELEMENTS(OSC52_NAMES); i++) {
			term_open(TYPES[t]);
			term_addstr(NULL, OSC52_NAMES[i]);
			assert_no_control(TYPES[t], "term_addstr", OSC52_NAMES[i]);
			term_close();

			term_open(TYPES[t]);
			for (p = OSC52_NAMES[i]; *p != '\0'; p++)
				term_addch(NULL, *p);
			assert_no_control(TYPES[t], "term_addch", OSC52_NAMES[i]);
			term_close();
		}
	}
}

/* printable text still comes out exactly as it went in */
static void check_unchanged(int type, const char *text)
{
	const char *p;

	term_open(type);
	sidepanel_text_walk(text, type, add_unichar, NULL);
	g_assert_cmpstr(term_output(), ==, text);
	term_close();

	term_open(type);
	term_addstr(NULL, text);
	g_assert_cmpstr(term_output(), ==, text);
	term_close();

	/* term_addch() writes single-byte characters, that is not UTF-8 */
	if (type == TERM_TYPE_UTF8)
		return;
	term_open(type);
	for (p = text; *p != '\0'; p++)
		term_addch(NULL, *p);
	g_assert_cmpstr(term_output(), ==, text);
	term_close();
}

static void test_valid_output(void)
{
	/* UTF-8: Cyrillic, Polish, CJK, an emoji */
	check_unchanged(TERM_TYPE_UTF8, "#\xd0\x9b\xd1\x91\xd0\xbd \xc5\xbc\xc3\xb3\xc5\x82w "
	                "\xe4\xb8\xad \xf0\x9f\x98\x80");
	/* Latin-1 / Latin-2 bytes on an 8-bit terminal: "#\xe9t\xe9", "\xbf\xf3\xb3w" */
	check_unchanged(TERM_TYPE_8BIT, "#\xe9t\xe9 \xbf\xf3\xb3w \xa0\xff");
	/* Big5 "zhong wen" */
	check_unchanged(TERM_TYPE_BIG5, "#\xa4\xa4\xa4\xe5 abc");
}

/* the replacement keeps the column count: one '?' for one control */
static void test_replacement_width(void)
{
	term_open(TERM_TYPE_UTF8);
	g_assert_cmpint(term_addstr(NULL, "a\033b\xc2\x9b" "c\xff" "d"), ==, 7);
	g_assert_cmpstr(term_output(), ==, "a?b?c?d");
	g_assert_cmpint(ansi_term->vcx, ==, 7);
	term_close();

	term_open(TERM_TYPE_8BIT);
	g_assert_cmpint(term_addstr(NULL, "a\033b\x9b" "c"), ==, 5);
	g_assert_cmpstr(term_output(), ==, "a?b?c");
	term_add_unichar(NULL, 0x41b);
	g_assert_cmpstr(term_output(), ==, "a?b?c?");
	g_assert_cmpint(ansi_term->vcx, ==, 6);
	term_close();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/term/output/osc52-side-panel", test_osc52_side_panel);
	g_test_add_func("/term/output/osc52-unichar", test_osc52_unichar);
	g_test_add_func("/term/output/osc52-addstr-addch", test_osc52_addstr_addch);
	g_test_add_func("/term/output/valid", test_valid_output);
	g_test_add_func("/term/output/replacement-width", test_replacement_width);

	test_home_setup();

	core_preinit(argv[0]);
	core_init();

	return g_test_run();
}
