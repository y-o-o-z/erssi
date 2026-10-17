/*
 * Tests for the nick column expandos of fe-expandos.c: $nickalign pads the
 * mode and the nick to nick_column_width terminal cells, $nicktrunc cuts a
 * nick that does not fit on a character boundary and adds "+", and
 * $nickcolored shows that nick also when nick hash colouring is off.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/recode.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/special-vars.h>
#include <irssi/src/core/utf8.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/fe-expandos.h>

#include <string.h>
#include "../../test-home.h"

/* $expando with the nick and mode of a message, as printed */
static char *expand(const char *nick, const char *mode, const char *what)
{
	char *ret;

	update_nick_context(nick, mode);
	ret = parse_special_string(what, NULL, NULL, "", NULL, 0);
	clear_nick_context();
	return ret;
}

static void set_charset(const char *charset)
{
	settings_set_str("term_charset", charset);
	recode_update_charset();
}

static void set_column(gboolean on, int width)
{
	settings_set_bool("nick_column_enabled", on);
	settings_set_int("nick_column_width", width);
}

typedef struct {
	const char *what;
	const char *nick;
	const char *mode;
	int width;
	const char *trunc; /* $nicktrunc */
	int pad;           /* spaces of $nickalign */
} COLUMN_CASE;

#define CJK10 "日本語のニックネーム" /* 10 characters, 20 cells */
#define E_ACUTE "e\xcc\x81"         /* e + COMBINING ACUTE ACCENT: one cell */

static const COLUMN_CASE COLUMN_CASES[] = {
	{ "ascii", "alice", "@", 13, "alice", 7 },
	{ "ascii, no mode", "alice", "", 13, "alice", 7 },
	{ "ascii, exactly the width", "abcdefghijkl", "@", 13, "abcdefghijkl", 0 },
	{ "ascii, one too long", "abcdefghijklm", "@", 13, "abcdefghijk+", 0 },
	{ "nick@host of a duplicate nick", "bob@example", "", 13, "bob@example", 1 },
	{ "Polish letters", "Łukasz", "@", 13, "Łukasz", 6 },
	{ "Polish, exactly the width", "żółćżółćżółć", "+", 13, "żółćżółćżółć", 0 },
	{ "Polish, cut", "żółćżółćżółćż", "+", 13, "żółćżółćżół+", 0 },
	{ "wide characters that fit", "日本語の", "@", 13, "日本語の", 4 },
	/* 11 cells after the mode for the nick: 5 wide characters and the
	 * "+", one cell left for $nickalign */
	{ "wide characters, cut", CJK10, "@", 13, "日本語のニ+", 1 },
	{ "wide characters, cut even", CJK10, "@", 12, "日本語のニ+", 0 },
	{ "combining marks", E_ACUTE E_ACUTE "x", "@", 13, E_ACUTE E_ACUTE "x", 9 },
	{ "combining marks, cut",
	  E_ACUTE E_ACUTE E_ACUTE E_ACUTE E_ACUTE E_ACUTE E_ACUTE, "@", 6,
	  E_ACUTE E_ACUTE E_ACUTE E_ACUTE "+", 0 },
	/* the text view draws a byte that is not UTF-8 in one cell (U+FFFD) */
	{ "invalid UTF-8", "ab\xff" "cd", "@", 13, "ab\xff" "cd", 7 },
	{ "invalid UTF-8 with Polish", "ą\xff" "b", "@", 13, "ą\xff" "b", 9 },
	{ "invalid UTF-8, cut", "\xff\xfe\xfd\xfc\xfb\xfa", "@", 5, "\xff\xfe\xfd+", 0 },
	{ "mode alone fills the column", "alice", "@", 1, "+", 0 },
	{ "no room for the nick", "alice", "@", 2, "+", 0 },
};

static void test_column_cases(void)
{
	int i;

	set_charset("UTF-8");
	for (i = 0; i < (int) G_N_ELEMENTS(COLUMN_CASES); i++) {
		const COLUMN_CASE *c = &COLUMN_CASES[i];
		char *trunc, *align;

		set_column(TRUE, c->width);
		trunc = expand(c->nick, c->mode, "$nicktrunc");
		align = expand(c->nick, c->mode, "$nickalign");
		if (g_strcmp0(trunc, c->trunc) != 0 || (int) strlen(align) != c->pad)
			g_test_message("%s: \"%s\" (%d spaces), expected \"%s\" (%d)", c->what,
			               trunc, (int) strlen(align), c->trunc, c->pad);
		g_assert_cmpstr(trunc, ==, c->trunc);
		g_assert_cmpint(strlen(align), ==, c->pad);
		g_assert_cmpint(strspn(align, " "), ==, strlen(align));
		g_free(trunc);
		g_free(align);
	}
}

/* every width: the padding, the mode and the (cut) nick fill the column
 * exactly, and a cut nick is a prefix of whole characters */
static void test_every_boundary(void)
{
	static const char *const nicks[] = {
		"alice_in_wonderland", "żółćżółćżółćżółć", CJK10, "a日b本c語d",
		E_ACUTE "a" E_ACUTE "b" E_ACUTE "c" E_ACUTE "d" E_ACUTE "e" E_ACUTE,
		"🦊fox🦊fox🦊fox",
	};
	int i, width;

	set_charset("UTF-8");
	for (i = 0; i < (int) G_N_ELEMENTS(nicks); i++) {
		int nick_width = string_width(nicks[i], TREAT_STRING_AS_UTF8);

		for (width = 2; width <= nick_width + 3; width++) {
			char *trunc, *align;
			int cells;

			set_column(TRUE, width);
			trunc = expand(nicks[i], "@", "$nicktrunc");
			align = expand(nicks[i], "@", "$nickalign");
			g_assert_true(g_utf8_validate(trunc, -1, NULL));
			cells = strlen(align) + 1 + string_width(trunc, TREAT_STRING_AS_UTF8);
			if (cells != width)
				g_test_message("%s, width %d: \"%s\" + %d spaces = %d cells",
				               nicks[i], width, trunc, (int) strlen(align), cells);
			g_assert_cmpint(cells, ==, width);
			if (1 + nick_width <= width) {
				g_assert_cmpstr(trunc, ==, nicks[i]);
			} else {
				size_t len = strlen(trunc) - 1;

				g_assert_cmpint(trunc[len], ==, '+');
				g_assert_cmpint(strncmp(trunc, nicks[i], len), ==, 0);
				/* the cut is at a character, not inside one or
				 * before a combining mark */
				g_assert_true(is_utf8_leading((unsigned char) nicks[i][len]));
				g_assert_cmpint(strncmp(nicks[i] + len, "\xcc\x81", 2), !=, 0);
			}
			g_free(trunc);
			g_free(align);
		}
	}
}

/* a terminal that is not UTF-8: one byte is one cell */
static void test_not_utf8_terminal(void)
{
	char *trunc, *align;

	set_charset("ISO-8859-2");
	set_column(TRUE, 13);
	trunc = expand("\xa3ukasz", "@", "$nicktrunc"); /* Łukasz in ISO-8859-2 */
	align = expand("\xa3ukasz", "@", "$nickalign");
	g_assert_cmpstr(trunc, ==, "\xa3ukasz");
	g_assert_cmpint(strlen(align), ==, 6);
	g_free(trunc);
	g_free(align);

	set_column(TRUE, 5);
	trunc = expand("\xb1\xe6\xea\xb3\xf1", "@", "$nicktrunc");
	g_assert_cmpstr(trunc, ==, "\xb1\xe6\xea+");
	g_free(trunc);
	set_charset("UTF-8");
}

/* with the column off the nick is whole and not padded */
static void test_column_off(void)
{
	char *trunc, *align;

	set_charset("UTF-8");
	set_column(FALSE, 5);
	trunc = expand(CJK10, "@", "$nicktrunc");
	align = expand(CJK10, "@", "$nickalign");
	g_assert_cmpstr(trunc, ==, CJK10);
	g_assert_cmpstr(align, ==, "");
	g_free(trunc);
	g_free(align);
}

/* $nickcolored is the nick of $nicktrunc, in a colour only while nick hash
 * colouring is on: every combination of the two settings shows the nick */
static void test_nickcolored(void)
{
	static const char *const nicks[] = { "alice", "a_rather_long_nick", "żółćżółćżółćżółć" };
	int i, hash, column;

	set_charset("UTF-8");
	for (hash = 0; hash < 2; hash++) {
		for (column = 0; column < 2; column++) {
			settings_set_bool("nick_hash_color_enabled", hash);
			set_column(column, 13);
			for (i = 0; i < (int) G_N_ELEMENTS(nicks); i++) {
				char *colored, *trunc, *plain;

				colored = expand(nicks[i], "@", "$nickcolored");
				trunc = expand(nicks[i], "@", "$nicktrunc");
				plain = strip_codes(colored);
				if (g_strcmp0(plain, trunc) != 0)
					g_test_message("hash %d, column %d: \"%s\" gave \"%s\"", hash,
					               column, nicks[i], plain);
				g_assert_cmpstr(plain, ==, trunc);
				g_assert_cmpstr(plain, !=, "");
				/* coloured only with hashing on */
				if (hash)
					g_assert_cmpstr(colored, !=, plain);
				else
					g_assert_cmpstr(colored, ==, plain);
				g_free(colored);
				g_free(trunc);
				g_free(plain);
			}
		}
	}
	settings_set_bool("nick_hash_color_enabled", TRUE);
}

/* outside a message the expandos are empty, not the nick of the last one */
static void test_outside_message(void)
{
	char *ret;

	set_column(TRUE, 13);
	update_nick_context("alice", "@");
	clear_nick_context();
	ret = parse_special_string("[$nicktrunc][$nickalign][$nickmode][$nickcolored]", NULL,
	                           NULL, "", NULL, 0);
	g_assert_cmpstr(ret, ==, "[][][][]");
	g_free(ret);

	set_column(FALSE, 13);
	settings_set_bool("nick_hash_color_enabled", FALSE);
	ret = parse_special_string("[$nicktrunc][$nickcolored]", NULL, NULL, "", NULL, 0);
	g_assert_cmpstr(ret, ==, "[][]");
	settings_set_bool("nick_hash_color_enabled", TRUE);
	g_free(ret);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* report every case that fails, not only the first one */
	g_test_set_nonfatal_assertions();

	g_test_add_func("/nick_column/cases", test_column_cases);
	g_test_add_func("/nick_column/every_boundary", test_every_boundary);
	g_test_add_func("/nick_column/not_utf8_terminal", test_not_utf8_terminal);
	g_test_add_func("/nick_column/column_off", test_column_off);
	g_test_add_func("/nick_column/nickcolored", test_nickcolored);
	g_test_add_func("/nick_column/outside_message", test_outside_message);

	test_home_setup();
	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	args_execute(0, NULL);
	core_init();
	fe_common_core_init();
	signal_emit("irssi init finished", 0);

	return g_test_run();
}
