/*
 * OSC 8 links (term-hyperlink.c, term_set_hyperlink() and the text view):
 * which addresses become links, that nothing from the network can end
 * the sequence early or bring its own escape sequences, the ids, what
 * term_hyperlinks auto decides, and the exact bytes the text view writes
 * for an address that wraps over several lines of a narrow window.
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
#include <irssi/src/fe-ansi/gui-windows.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/term-ansi.h>
#include <irssi/src/fe-ansi/term-hyperlink.h>
#include <irssi/src/fe-ansi/textbuffer.h>
#include <irssi/src/fe-ansi/textbuffer-view.h>
#include <irssi/src/fe-ansi/sidepanels-text.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../test-home.h"

#define MODULE_NAME "tests"

/* the URIs term_link_scan() finds in text, joined with a space */
static char *scan_uris(const char *text, int charset)
{
	TermLinkList *list = term_link_scan(text, charset);
	GString *out = g_string_new(NULL);
	int i;

	for (i = 0; list != NULL && i < list->count; i++) {
		const TermLinkSpan *s = &list->spans[i];

		if (i > 0)
			g_string_append_c(out, ' ');
		g_string_append(out, s->uri);
		/* the span covers exactly the shown address */
		g_assert_cmpuint(s->start, <, s->end);
		g_assert_true(term_link_uri_is_safe(s->uri));
	}
	term_link_list_free(list);
	return g_string_free(out, FALSE);
}

typedef struct {
	const char *text;
	const char *uris; /* "" for none */
} SCAN_ROW;

static const SCAN_ROW SCAN[] = {
	{ "no address here", "" },
	{ "see https://example.org/a", "https://example.org/a" },
	{ "HTTP://Example.org/x", "HTTP://Example.org/x" },
	{ "two: http://a.example/ and ftp://b.example/f.txt.", "http://a.example/ ftp://b.example/f.txt" },
	{ "gopher://g.example gemini://m.example/ irc://irc.example/#chan ircs://irc.example:6697/x",
	  "gopher://g.example gemini://m.example/ irc://irc.example/#chan ircs://irc.example:6697/x" },
	/* not these schemes */
	{ "file:///etc/passwd javascript://x data://x mailto:a@example.org ssh://h.example", "" },
	/* the scheme starts a word */
	{ "xhttps://example.org/ 9http://example.org/", "" },
	{ "(https://example.org/)", "https://example.org/" },
	{ "<https://example.org/>", "https://example.org/" },
	{ "\"https://example.org/\"", "https://example.org/" },
	{ "'https://example.org/'", "https://example.org/" },
	{ "`https://example.org/`", "https://example.org/" },
	/* sentence punctuation is not part of it */
	{ "go to https://example.org/a, https://example.org/b; https://example.org/c!",
	  "https://example.org/a https://example.org/b https://example.org/c" },
	{ "https://example.org/?q=1.", "https://example.org/?q=1" },
	{ "https://example.org/x:", "https://example.org/x" },
	/* brackets that belong to it, and those that do not */
	{ "https://en.wikipedia.org/wiki/Foo_(bar)", "https://en.wikipedia.org/wiki/Foo_(bar)" },
	{ "(see https://en.wikipedia.org/wiki/Foo_(bar))", "https://en.wikipedia.org/wiki/Foo_(bar)" },
	{ "(see https://example.org/x).", "https://example.org/x" },
	{ "http://[::1]:8080/a", "http://[::1]:8080/a" },
	{ "[https://example.org/]", "https://example.org/" },
	/* no host */
	{ "http:// https:///x http://", "" },
	/* spaces and controls end it */
	{ "https://example.org/a b", "https://example.org/a" },
	{ "https://example.org/a\tb", "https://example.org/a" },
	/* non-ASCII: percent-encoded in the target, the text shows it as it is */
	{ "https://example.org/zażółć", "https://example.org/za%C5%BC%C3%B3%C5%82%C4%87" },
	{ "https://example.org/a|b{c}^", "https://example.org/a%7Cb%7Bc%7D%5E" },
	{ "https://example.org/100%", "https://example.org/100%25" },
	{ "https://example.org/%41", "https://example.org/%41" },
	/* closing punctuation of other scripts */
	{ "「https://example.org/x」", "https://example.org/x" },
	/* format sequences in between are not part of the target; bold,
	 * underline, italic and monospace do not end it */
	{ "\004" "a" "https://exa\004" "cmple.org/\004" "e", "https://example.org/" },
	{ "https://e\004" "b" "x\004" "f" "a\004" "i" "mple.org/", "https://example.org/" },
	/* a colour change ends it: the rest could be drawn invisible, as
	 * the user part of an address that goes elsewhere */
	{ "https://paypal.example\004" "00" "@evil.example/login", "https://paypal.example" },
	{ "https://paypal.example\004" "#" "\x21\x21\x22\x20" "\004" "#" "\x21\x21\x21\x21" "@evil.example/",
	  "https://paypal.example" },
	{ "https://paypal.example\004" ".!" "@evil.example/", "https://paypal.example" },
	{ "https://paypal.example\004" "1/" "@evil.example/", "https://paypal.example" },
	{ "https://paypal.example\004" "d" "@evil.example/", "https://paypal.example" },
	{ "https://paypal.example\004" "g" "@evil.example/", "https://paypal.example" },
	/* a character the text view draws whole (under 16 bytes) is part of
	 * it; a longer one the view draws as its first code point only, so
	 * it ends the address (here x and 8 or 7 combining acute accents) */
	{ "https://example.org/a" "x\314\201\314\201\314\201\314\201\314\201\314\201\314\201\314\201" "b",
	  "https://example.org/a" },
	{ "https://example.org/a" "x\314\201\314\201\314\201\314\201\314\201\314\201\314\201" "b",
	  "https://example.org/ax%CC%81%CC%81%CC%81%CC%81%CC%81%CC%81%CC%81b" },
	/* lines of one entry */
	{ "https://a.example/\nhttps://b.example/", "https://a.example/ https://b.example/" },
};

static void test_scan(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(SCAN); i++) {
		char *got = scan_uris(SCAN[i].text, TERM_TYPE_UTF8);

		if (strcmp(got, SCAN[i].uris) != 0)
			g_error("row %u \"%s\": got \"%s\", expected \"%s\"", i,
				g_strescape(SCAN[i].text, NULL), got, SCAN[i].uris);
		g_free(got);
	}
}

/* the span is where the shown address is, format sequences included */
static void test_scan_offsets(void)
{
	const char *text = "x \004" "chttps://example.org/\004" "e.";
	TermLinkList *list = term_link_scan(text, TERM_TYPE_UTF8);

	g_assert_nonnull(list);
	g_assert_cmpint(list->count, ==, 1);
	g_assert_cmpuint(list->spans[0].start, ==, 4);
	g_assert_cmpuint(list->spans[0].end, ==, strlen("x \004" "chttps://example.org/"));
	term_link_list_free(list);
}

/* An 8-bit or Big5 line: an address is ASCII, a byte of 0x80 or more ends
 * it. */
static void test_scan_8bit(void)
{
	char *got = scan_uris("https://example.org/a\xe9" "b", TERM_TYPE_8BIT);

	g_assert_cmpstr(got, ==, "https://example.org/a");
	g_free(got);
	/* after a Latin-1 letter, an address still starts */
	got = scan_uris("\xe9 https://example.org/", TERM_TYPE_8BIT);
	g_assert_cmpstr(got, ==, "https://example.org/");
	g_free(got);
	/* Big5: the second byte of a character (here 'h') is not the start
	 * of an address */
	got = scan_uris("\xa4https://example.org/ \xa4@ https://example.org/b",
			TERM_TYPE_BIG5);
	g_assert_cmpstr(got, ==, "https://example.org/b");
	g_free(got);
}

/* Addresses that try to end the sequence or start one of their own: the
 * link stops before what cannot be part of it, and its target never has
 * a byte outside 0x21..0x7e. */
static const char *const EVIL[] = {
	"https://example.org/\033]52;c;SGk=\007",
	"https://example.org/\033\\\033]8;;https://evil.example/\033\\",
	"https://example.org/\007x",
	"https://example.org/\302\234x",       /* U+009C, ST */
	"https://example.org/\302\235x",       /* U+009D, OSC */
	"https://example.org/\302\233" "2J",   /* U+009B, CSI */
	"https://example.org/\234x",           /* a raw 0x9C, invalid UTF-8 */
	"https://example.org/\x7fx",
	"https://example.org/\342\200\256gpj.exe", /* U+202E, right-to-left override */
	"https://example.org/\342\200\213x",   /* U+200B, zero width space */
	"https://example.org/\343\200\200x",   /* U+3000, ideographic space */
	"https://example.org/\357\277\275x",   /* U+FFFD */
};

static void test_scan_evil(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(EVIL); i++) {
		TermLinkList *list = term_link_scan(EVIL[i], TERM_TYPE_UTF8);

		g_assert_nonnull(list);
		/* the first link is the harmless part, nothing after it */
		if (strcmp(list->spans[0].uri, "https://example.org/") != 0)
			g_error("row %u: link \"%s\"", i, g_strescape(list->spans[0].uri, NULL));
		g_assert_cmpuint(list->spans[0].end, ==, strlen("https://example.org/"));
		term_link_list_free(list);
	}
}

static void test_encode_uri(void)
{
	static const char *const REFUSED[] = {
		"https://example.org/\033", "https://example.org/\007", "https://example.org/ x",
		"https://example.org/\x7f", "https://example.org/<", "https://example.org/>",
		"https://example.org/\"", "https://example.org/\n",
	};
	char *uri, *longer;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(REFUSED); i++)
		g_assert_null(term_link_encode_uri(REFUSED[i], strlen(REFUSED[i])));

	/* a C1 byte or code point is encoded, never written as it is */
	uri = term_link_encode_uri("https://e.example/\302\234\234", 21);
	g_assert_cmpstr(uri, ==, "https://e.example/%C2%9C%9C");
	g_free(uri);

	/* the length limit, after encoding */
	longer = g_strnfill(TERM_LINK_URI_MAX - strlen("https://e.example/"), 'a');
	uri = g_strconcat("https://e.example/", longer, NULL);
	g_free(longer);
	longer = term_link_encode_uri(uri, strlen(uri));
	g_assert_cmpstr(longer, ==, uri);
	g_free(longer);
	longer = g_strconcat(uri, "b", NULL);
	g_assert_null(term_link_encode_uri(longer, strlen(longer)));
	g_free(longer);
	longer = g_strconcat(uri, "b see https://ok.example/ ", uri, NULL);
	g_free(uri);
	/* too long as a link: plain text, and the next address still is one;
	 * one of exactly the limit is a link */
	uri = scan_uris(longer, TERM_TYPE_UTF8);
	g_assert_true(g_str_has_prefix(uri, "https://ok.example/ https://e.example/aaa"));
	g_assert_cmpuint(strlen(uri), ==, strlen("https://ok.example/ ") + TERM_LINK_URI_MAX);
	g_free(uri);
	g_free(longer);

	g_assert_false(term_link_uri_is_safe("https://e.example/\033"));
	g_assert_false(term_link_uri_is_safe("https://e.example/\302\234"));
	g_assert_false(term_link_uri_is_safe(""));
	g_assert_true(term_link_uri_is_safe("https://e.example/;x"));
	g_assert_false(term_link_id_is_safe("a:b"));
	g_assert_false(term_link_id_is_safe("a;b"));
	g_assert_false(term_link_id_is_safe(""));
	g_assert_true(term_link_id_is_safe("e0123abc"));
}

static void test_make_id(void)
{
	char a[TERM_LINK_ID_SIZE], b[TERM_LINK_ID_SIZE], c[TERM_LINK_ID_SIZE];
	const char *p;

	term_link_set_salt(0);
	term_link_make_id(1234, 0, a);
	term_link_make_id(1234, 0, b);
	g_assert_cmpstr(a, ==, b);
	g_assert_cmpuint(strlen(a), ==, TERM_LINK_ID_SIZE - 1);
	for (p = a; *p != '\0'; p++)
		g_assert_true(g_ascii_isalnum(*p));
	g_assert_true(term_link_id_is_safe(a));

	term_link_make_id(1234, 1, b);
	g_assert_cmpstr(a, !=, b);
	term_link_make_id(1235, 0, b);
	g_assert_cmpstr(a, !=, b);

	term_link_set_salt(99);
	term_link_make_id(1234, 0, c);
	g_assert_cmpstr(a, !=, c);
	term_link_set_salt(0);
}

typedef struct {
	const char *env; /* "VAR=value, VAR=value" */
	gboolean on;
} DETECT_ROW;

static const DETECT_ROW DETECT[] = {
	{ "TERM=xterm-kitty", TRUE },
	{ "TERM=xterm-256color, KITTY_WINDOW_ID=1", TRUE },
	{ "TERM=xterm-256color, TERM_PROGRAM=iTerm.app", TRUE },
	{ "TERM=xterm-256color, LC_TERMINAL=iTerm2", TRUE },
	{ "TERM=xterm-256color, TERM_PROGRAM=WezTerm", TRUE },
	{ "TERM=xterm-ghostty", TRUE },
	{ "TERM=foot", TRUE },
	{ "TERM=alacritty", TRUE },
	{ "TERM=xterm-256color, VTE_VERSION=7600", TRUE },
	{ "TERM=xterm-256color, VTE_VERSION=4800", FALSE },
	{ "TERM=xterm-256color, KONSOLE_VERSION=230800", TRUE },
	{ "TERM=xterm-256color, WT_SESSION=x", TRUE },
	{ "TERM=xterm-256color, TERM_PROGRAM=vscode", TRUE },
	/* tmux: 3.4 hands it on, older ones drop it */
	{ "TERM=tmux-256color, TMUX=/tmp/t,1,0", TRUE },
	{ "TERM=screen-256color, TMUX=/tmp/t,1,0", TRUE },
	/* GNU screen, also over ssh where only TERM tells */
	{ "TERM=screen-256color, STY=1.pts", FALSE },
	{ "TERM=xterm-kitty, STY=1.pts", FALSE },
	{ "TERM=tmux-256color, TMUX=/tmp/t,1,0, STY=1.pts", FALSE },
	{ "TERM=screen.xterm-256color", FALSE },
	/* consoles, dumb terminals, Emacs */
	{ "TERM=linux", FALSE },
	{ "TERM=linux, TMUX=/tmp/t,1,0", FALSE },
	{ "TERM=xterm, TTY=/dev/ttyv0", FALSE },
	{ "TERM=xterm-kitty, TTY=/dev/tty1", FALSE },
	{ "TERM=xterm-256color, TTY=/dev/pts/3, KITTY_WINDOW_ID=1", TRUE },
	{ "TERM=cons25", FALSE },
	{ "TERM=vt100", FALSE },
	{ "TERM=dumb", FALSE },
	{ "", FALSE },
	{ "TERM=xterm-256color, INSIDE_EMACS=29.1,eat", FALSE },
	/* not known */
	{ "TERM=xterm-256color", FALSE },
	{ "TERM=xterm", FALSE },
	{ "TERM=rxvt-unicode-256color", FALSE },
};

static void test_detect(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(DETECT); i++) {
		TermLinkEnv env;
		char **pairs = g_strsplit(DETECT[i].env, ", ", -1);
		int k;

		memset(&env, 0, sizeof(env));
		for (k = 0; pairs[k] != NULL; k++) {
			char *eq = strchr(pairs[k], '='), *value;

			if (eq == NULL)
				continue;
			*eq = '\0';
			value = eq + 1;
			if (strcmp(pairs[k], "TERM") == 0) env.term = value;
			else if (strcmp(pairs[k], "TERM_PROGRAM") == 0) env.term_program = value;
			else if (strcmp(pairs[k], "LC_TERMINAL") == 0) env.lc_terminal = value;
			else if (strcmp(pairs[k], "KITTY_WINDOW_ID") == 0) env.kitty_window_id = value;
			else if (strcmp(pairs[k], "VTE_VERSION") == 0) env.vte_version = value;
			else if (strcmp(pairs[k], "KONSOLE_VERSION") == 0) env.konsole_version = value;
			else if (strcmp(pairs[k], "WT_SESSION") == 0) env.wt_session = value;
			else if (strcmp(pairs[k], "INSIDE_EMACS") == 0) env.inside_emacs = value;
			else if (strcmp(pairs[k], "TMUX") == 0) env.tmux = value;
			else if (strcmp(pairs[k], "STY") == 0) env.sty = value;
			else if (strcmp(pairs[k], "TTY") == 0) env.tty = value;
			else g_error("row %u: unknown variable %s", i, pairs[k]);
		}
		if (term_link_detect(&env) != DETECT[i].on)
			g_error("row %u (%s): %s, expected %s", i, DETECT[i].env,
				DETECT[i].on ? "off" : "on", DETECT[i].on ? "on" : "off");
		g_strfreev(pairs);
	}
}

/* ------------------------------------------------------------------------
 * the terminal and the text view
 * ------------------------------------------------------------------------ */

static char *buf;
static size_t buf_len;

static void term_open(int width, int height)
{
	term_type = TERM_TYPE_UTF8;
	term_width = width;
	term_height = height;
	ansi_term = g_new0(ANSI_TERM, 1);
	ansi_term->out = open_memstream(&buf, &buf_len);
	g_assert_nonnull(ansi_term->out);
	ansi_term->lines_empty = g_new0(char, term_height);
	ansi_term->last_fg = ansi_term->last_bg = UINT_MAX;
	ansi_term->crealx = ansi_term->crealy = -1;
	ansi_term->cforcemove = TRUE;
	root_window = term_window_create(0, 0, width, height);
	term_use_colors = 1;
	term_use_colors24 = 0;
}

static const char *term_output(void)
{
	fflush(ansi_term->out);
	return buf != NULL ? buf : "";
}

static void term_reset_output(void)
{
	fclose(ansi_term->out);
	free(buf);
	buf = NULL;
	ansi_term->out = open_memstream(&buf, &buf_len);
	g_assert_nonnull(ansi_term->out);
}

static void term_close(void)
{
	term_set_hyperlink(NULL, NULL, NULL);
	g_assert_null(ansi_term->link_uri);
	fclose(ansi_term->out);
	g_free(ansi_term->lines_empty);
	g_free(ansi_term);
	ansi_term = NULL;
	term_window_destroy(root_window);
	root_window = NULL;
	free(buf);
	buf = NULL;
}

/* Every OSC 8 in the output: each one is well formed, opened only when
 * none is open and closed before the next one; returns the open ones as
 * "id uri" lines. Fails when one is left open at the end. */
static char *osc8_opens(const char *out)
{
	GString *opens = g_string_new(NULL);
	gboolean open = FALSE;
	const char *p = out;

	while ((p = strstr(p, "\033]8;")) != NULL) {
		const char *end = strstr(p, "\033\\");
		const char *params, *uri;
		char *seq;

		g_assert_nonnull(end);
		seq = g_strndup(p + 4, end - p - 4);
		/* nothing inside but printable ASCII */
		for (params = seq; *params != '\0'; params++)
			g_assert_true(*params > 0x20 && *params < 0x7f);
		uri = strchr(seq, ';');
		g_assert_nonnull(uri);
		uri++;
		if (*uri == '\0') {
			g_assert_true(open);
			open = FALSE;
		} else {
			g_assert_false(open);
			g_assert_true(g_str_has_prefix(seq, "id="));
			open = TRUE;
			g_string_append_len(opens, seq + 3, uri - 1 - (seq + 3));
			g_string_append_printf(opens, " %s\n", uri);
		}
		g_free(seq);
		p = end + 2;
	}
	g_assert_false(open);
	return g_string_free(opens, FALSE);
}

/* term_set_hyperlink() itself: lazy open, close at once, closed by moves
 * and clears; refused when off or when the URI or id is not safe */
static void test_term_primitives(void)
{
	term_open(80, 3);
	term_use_hyperlinks = TRUE;

	term_move(root_window, 0, 0);
	term_addstr(root_window, "a");
	term_set_hyperlink(root_window, "https://example.org/", "e1");
	g_assert_cmpstr(term_output(), ==, "\033[1;1Ha");
	term_addstr(root_window, "b");
	term_addstr(root_window, "c");
	term_set_hyperlink(root_window, "https://example.org/", "e1");
	term_addstr(root_window, "d");
	term_set_hyperlink(root_window, NULL, NULL);
	term_addstr(root_window, "e");
	g_assert_cmpstr(term_output(), ==,
			"\033[1;1Ha\033]8;id=e1;https://example.org/\033\\bcd\033]8;;\033\\e");

	/* a move ends it; the colours inside it do not */
	term_reset_output();
	term_set_hyperlink(root_window, "https://example.org/", "e1");
	term_addstr(root_window, "x");
	term_set_color(root_window, ATTR_RESET | ATTR_BOLD);
	term_addstr(root_window, "y");
	term_move(root_window, 0, 1);
	term_addstr(root_window, "z");
	g_assert_cmpstr(term_output(), ==,
			"\033]8;id=e1;https://example.org/\033\\x\033[1my\033]8;;\033\\\033[2;1Hz");

	/* clearing to the end of the line is never part of it */
	term_reset_output();
	term_set_color(root_window, ATTR_RESET);
	term_reset_output();
	term_set_hyperlink(root_window, "https://example.org/", "e1");
	term_addstr(root_window, "q");
	term_clrtoeol(root_window);
	term_addstr(root_window, "r");
	g_assert_cmpstr(term_output(), ==,
			"\033]8;id=e1;https://example.org/\033\\q\033]8;;\033\\\033[Kr");

	/* refused: not safe, or links off */
	term_reset_output();
	term_set_hyperlink(root_window, "https://example.org/\033]52;c;x\007", "e1");
	term_addstr(root_window, "s");
	term_set_hyperlink(root_window, "https://example.org/\302\234", "e1");
	term_addstr(root_window, "t");
	term_set_hyperlink(root_window, "https://example.org/", "e;1");
	term_addstr(root_window, "u");
	term_use_hyperlinks = FALSE;
	term_set_hyperlink(root_window, "https://example.org/", "e1");
	term_addstr(root_window, "v");
	g_assert_cmpstr(term_output(), ==, "stuv");

	term_close();
}

/* the output with every OSC 8 sequence taken out */
static char *without_osc8(const char *out)
{
	GString *str = g_string_new(NULL);
	const char *p;

	while ((p = strstr(out, "\033]8;")) != NULL) {
		g_string_append_len(str, out, p - out);
		p = strstr(p, "\033\\");
		g_assert_nonnull(p);
		out = p + 2;
	}
	g_string_append(str, out);
	return g_string_free(str, FALSE);
}

static WINDOW_REC fake_window;
static GUI_WINDOW_REC fake_gui;

static TEXT_BUFFER_VIEW_REC *view_open(int width, int height, gboolean scroll)
{
	TEXT_BUFFER_REC *buffer;
	TEXT_BUFFER_VIEW_REC *view;

	memset(&fake_window, 0, sizeof(fake_window));
	memset(&fake_gui, 0, sizeof(fake_gui));
	fake_window.gui_data = &fake_gui;
	buffer = textbuffer_create(&fake_window);
	view = textbuffer_view_create(buffer, width, height, scroll, TRUE);
	fake_gui.view = view;
	return view;
}

static LINE_REC *view_add(TEXT_BUFFER_VIEW_REC *view, const char *text)
{
	LINE_INFO_REC info = { 0 };
	GString *data;
	LINE_REC *line;

	info.level = MSGLEVEL_CLIENTCRAP;
	info.time = 1000;
	data = g_string_new(text);
	g_string_append_c(data, 0);
	g_string_append_c(data, LINE_CMD_EOL);
	line = textbuffer_insert(view->buffer, textbuffer_line_last(view->buffer),
				 (const unsigned char *) data->str, data->len, &info);
	g_assert_nonnull(line);
	g_string_free(data, TRUE);
	textbuffer_view_insert_line(view, line);
	return line;
}

/* the buffer goes with its last view */
static void view_close(TEXT_BUFFER_VIEW_REC *view)
{
	textbuffer_view_destroy(view);
}

static void line_id(LINE_REC *line, int index, char id[TERM_LINK_ID_SIZE])
{
	term_link_make_id((guint64) GPOINTER_TO_SIZE(line) ^ ((guint64) line->info.time << 32),
			  index, id);
}

#define LONG_URL "https://example.org/aaaaaaaaaabbbbbbbbbbcc"

/* A long address in a window 20 columns wide, at the right of a 30 column
 * terminal (a side panel on the left): three pieces, each one opened at
 * its first character and closed after its last, all with the same id and
 * the whole address as the target. */
static void test_view_wrapped(void)
{
	TEXT_BUFFER_VIEW_REC *view;
	TERM_WINDOW *win;
	LINE_REC *line;
	char id[TERM_LINK_ID_SIZE], *expected, *open, *opens, *on;
	const char *out;

	term_open(30, 6);
	term_use_hyperlinks = TRUE;
	win = term_window_create(10, 0, 20, 4);
	view = view_open(20, 4, FALSE);
	line = view_add(view, "see " LONG_URL " ok");
	line_id(line, 0, id);

	textbuffer_view_set_window(view, win);
	term_reset_output();
	textbuffer_view_redraw(view);
	out = term_output();

	open = g_strdup_printf("\033]8;id=%s;" LONG_URL "\033\\", id);
	/* the address is a long word: it starts after "see " and goes on
	 * at the start of the next lines; the empty last line is cleared,
	 * then the cursor goes back */
	expected = g_strconcat(
		"\033[1;11Hsee ", open, "https://example.", "\033]8;;\033\\",
		"\033[2;11H", open, "org/aaaaaaaaaabbbbbb", "\033]8;;\033\\",
		"\033[3;11H", open, "bbbbcc", "\033]8;;\033\\", " ok\033[K",
		"\033[4;11H\033[K\033[1;1H\033[?25h",
		NULL);
	if (strcmp(out, expected) != 0)
		g_error("output\n%s\nexpected\n%s", g_strescape(out, NULL),
			g_strescape(expected, NULL));
	g_free(expected);

	opens = osc8_opens(out);
	expected = g_strdup_printf("%s %s\n%s %s\n%s %s\n", id, LONG_URL, id, LONG_URL, id,
				   LONG_URL);
	g_assert_cmpstr(opens, ==, expected);
	g_free(expected);
	g_free(opens);
	g_free(open);

	/* off: exactly the same output without the OSC 8: the links take
	 * no room and move nothing */
	on = without_osc8(out);
	term_use_hyperlinks = FALSE;
	term_reset_output();
	ansi_term->curs_visible = FALSE; /* as at the first redraw */
	textbuffer_view_redraw(view);
	g_assert_null(strstr(term_output(), "\033]8"));
	g_assert_cmpstr(term_output(), ==, on);
	g_free(on);

	view_close(view);
	term_window_destroy(win);
	term_close();
}

/* Only the bottom line of the window is drawn (a scroll, a partial
 * redraw): its piece of the link is opened and closed on its own. */
static void test_view_partial(void)
{
	TEXT_BUFFER_VIEW_REC *view;
	TERM_WINDOW *win;
	LINE_REC *line;
	char id[TERM_LINK_ID_SIZE], *opens, *expected, *shown;

	term_open(20, 4);
	term_use_hyperlinks = TRUE;
	win = term_window_create(0, 0, 20, 2);
	view = view_open(20, 2, TRUE);
	textbuffer_view_set_window(view, win);
	textbuffer_view_redraw(view);
	/* a line of four pieces in a window of two lines that follows the
	 * end: only the last two are on the screen, drawn as the line comes */
	term_reset_output();
	line = view_add(view, LONG_URL "dddddddddddddddddddd" "ee");
	line_id(line, 0, id);
	opens = osc8_opens(term_output());
	/* each piece drawn has its own open and close, same id, same URI
	 * (the text view may draw the end of the window more than once) */
	expected = g_strdup_printf("%s %s\n", id, LONG_URL "dddddddddddddddddddd" "ee");
	g_assert_true(g_str_has_prefix(opens, expected));
	{
		char **lines = g_strsplit(opens, "\n", -1);
		int k;

		g_strchomp(expected);
		for (k = 0; lines[k] != NULL && *lines[k] != '\0'; k++)
			g_assert_cmpstr(lines[k], ==, expected);
		g_assert_cmpint(k % 2, ==, 0);
		g_strfreev(lines);
	}
	/* the text drawn: the last two pieces only */
	shown = without_osc8(term_output());
	g_assert_null(strstr(shown, "https://"));
	g_assert_null(strstr(shown, "aaaa"));
	g_assert_nonnull(strstr(shown, "ccdddddddddddddddddd"));
	g_assert_nonnull(strstr(shown, "\033[2;1Hddee"));
	g_free(shown);
	g_free(opens);
	g_free(expected);

	view_close(view);
	term_window_destroy(win);
	term_close();
}

/* A window as wide as the terminal, a long word that goes on at the start
 * of the next line: the text view lets the terminal wrap there instead of
 * moving the cursor. The link is closed at the last column all the same
 * and opened again on the next line. */
static void test_view_full_width(void)
{
	TEXT_BUFFER_VIEW_REC *view;
	TERM_WINDOW *win;
	LINE_REC *line;
	char id[TERM_LINK_ID_SIZE], *opens, *expected, *on;

	term_open(20, 4);
	term_use_hyperlinks = TRUE;
	win = term_window_create(0, 0, 20, 4);
	view = view_open(20, 4, FALSE);
	view->longword_noindent = TRUE;
	line = view_add(view, "x " LONG_URL);
	line_id(line, 0, id);
	textbuffer_view_set_window(view, win);
	term_reset_output();
	textbuffer_view_redraw(view);

	opens = osc8_opens(term_output());
	expected = g_strdup_printf("%s %s\n%s %s\n%s %s\n", id, LONG_URL, id, LONG_URL, id,
				   LONG_URL);
	g_assert_cmpstr(opens, ==, expected);
	g_free(opens);
	g_free(expected);

	on = without_osc8(term_output());
	term_use_hyperlinks = FALSE;
	term_reset_output();
	/* the terminal as at the first redraw */
	ansi_term->curs_visible = FALSE;
	ansi_term->crealx = ansi_term->crealy = -1;
	ansi_term->cforcemove = TRUE;
	memset(ansi_term->lines_empty, 0, term_height);
	textbuffer_view_redraw(view);
	g_assert_cmpstr(term_output(), ==, on);
	g_free(on);

	view_close(view);
	term_window_destroy(win);
	term_close();
}

/* A colour change in the middle of an address keeps the link open, and
 * the colour; malicious bytes the sender put there are drawn as text and
 * end the link. */
static void test_view_colour_and_evil(void)
{
	TEXT_BUFFER_VIEW_REC *view;
	TERM_WINDOW *win;
	LINE_REC *line;
	char id[TERM_LINK_ID_SIZE], *opens, *expected, *shown;
	const char *out, *p;

	term_open(80, 3);
	term_use_hyperlinks = TRUE;
	win = term_window_create(0, 0, 80, 3);
	view = view_open(80, 3, FALSE);
	textbuffer_view_set_window(view, win);
	line = view_add(view, "https://exa\002mple.org/\002x https://e.example/\033]8;;https://evil.example/\007");
	line_id(line, 0, id);
	term_reset_output();
	textbuffer_view_redraw(view);
	out = term_output();

	opens = osc8_opens(out);
	line_id(line, 1, id);
	expected = NULL;
	{
		char id0[TERM_LINK_ID_SIZE];

		line_id(line, 0, id0);
		/* the ESC is not shown (the text view drops it), so it is
		 * not in the target either: the target is what is shown */
		expected = g_strdup_printf("%s https://example.org/x\n"
					   "%s https://e.example/]8;;https://evil.example/\n",
					   id0, id);
	}
	g_assert_cmpstr(opens, ==, expected);
	g_free(opens);
	g_free(expected);
	/* bold inside the first link */
	g_assert_nonnull(strstr(out, "https://exa\033[1mmple.org/"));
	/* no ESC from the sender: each ESC is one of erssi's own sequences */
	for (p = out; (p = strchr(p, '\033')) != NULL; p++)
		g_assert_true(p[1] == '[' || (p[1] == ']' && p[2] == '8') || p[1] == '\\');
	shown = without_osc8(out);
	g_assert_nonnull(strstr(shown, "https://e.example/]8;;https://evil.example/"));
	g_free(shown);
	g_assert_null(strchr(out, '\007'));

	view_close(view);
	term_window_destroy(win);
	term_close();
}

/* A sender colours the rest of an address like the background (mIRC
 * colour 1 on 1): only what is visible before it is the link. */
static void test_view_colour_ends_link(void)
{
	TEXT_BUFFER_VIEW_REC *view;
	TERM_WINDOW *win;
	LINE_REC *line;
	char id[TERM_LINK_ID_SIZE], *opens, *expected;

	term_open(80, 3);
	term_use_hyperlinks = TRUE;
	win = term_window_create(0, 0, 80, 3);
	view = view_open(80, 3, FALSE);
	textbuffer_view_set_window(view, win);
	line = view_add(view, "https://paypal.example\00301,01@evil.example/login");
	line_id(line, 0, id);
	term_reset_output();
	textbuffer_view_redraw(view);

	opens = osc8_opens(term_output());
	expected = g_strdup_printf("%s https://paypal.example\n", id);
	g_assert_cmpstr(opens, ==, expected);
	g_free(opens);
	g_free(expected);

	view_close(view);
	term_window_destroy(win);
	term_close();
}

/* What the side panels draw goes through term_add_unichar() after a
 * term_move(): never part of a link, also when a link was open. */
static gboolean add_unichar(unichar chr, int width, int color, unsigned int fg24,
			    unsigned int bg24, void *data)
{
	term_add_unichar(NULL, chr);
	return TRUE;
}

static void test_side_panel_no_link(void)
{
	term_open(100, 3);
	term_use_hyperlinks = TRUE;
	term_move(root_window, 0, 0);
	term_set_hyperlink(root_window, "https://example.org/", "e1");
	term_addstr(root_window, "h");
	/* the panel draws its row */
	term_move(root_window, 60, 0);
	sidepanel_text_walk("#https://example.org/", TERM_TYPE_UTF8, add_unichar, NULL);
	g_assert_nonnull(strstr(term_output(), "\033]8;;\033\\\033[1;61H#https://example.org/"));
	g_assert_cmpstr(strstr(term_output(), "\033[1;61H"), ==, "\033[1;61H#https://example.org/");
	term_close();
}

/* term_hyperlinks: auto follows the detection, on and off override it */
static void test_setting(void)
{
	term_open(80, 3);
	ansi_term->caps.has_hyperlinks = FALSE;
	settings_set_choice("term_hyperlinks", "auto");
	signal_emit("setup changed", 0);
	g_assert_false(term_use_hyperlinks);
	ansi_term->caps.has_hyperlinks = TRUE;
	signal_emit("setup changed", 0);
	g_assert_true(term_use_hyperlinks);
	settings_set_choice("term_hyperlinks", "off");
	signal_emit("setup changed", 0);
	g_assert_false(term_use_hyperlinks);
	ansi_term->caps.has_hyperlinks = FALSE;
	settings_set_choice("term_hyperlinks", "on");
	signal_emit("setup changed", 0);
	g_assert_true(term_use_hyperlinks);
	settings_set_choice("term_hyperlinks", "auto");
	signal_emit("setup changed", 0);
	g_assert_false(term_use_hyperlinks);
	term_close();
}

/* the detection is filled from the environment */
static void test_detect_capabilities(void)
{
	ANSI_TERM t;

	g_unsetenv("STY");
	g_unsetenv("TMUX");
	g_setenv("TERM", "xterm-kitty", TRUE);
	memset(&t, 0, sizeof(t));
	ansi_detect_capabilities(&t);
	/* the test may run on a console: then the tty says no */
	if (!isatty(0) || !g_str_has_prefix(ttyname(0), "/dev/tty"))
		g_assert_true(t.caps.has_hyperlinks);
	g_setenv("TERM", "linux", TRUE);
	memset(&t, 0, sizeof(t));
	ansi_detect_capabilities(&t);
	g_assert_false(t.caps.has_hyperlinks);
	g_setenv("TERM", "xterm-kitty", TRUE);
	g_setenv("STY", "1.x", TRUE);
	memset(&t, 0, sizeof(t));
	ansi_detect_capabilities(&t);
	g_assert_false(t.caps.has_hyperlinks);
	g_unsetenv("STY");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/term-hyperlink/scan", test_scan);
	g_test_add_func("/term-hyperlink/scan-offsets", test_scan_offsets);
	g_test_add_func("/term-hyperlink/scan-8bit", test_scan_8bit);
	g_test_add_func("/term-hyperlink/scan-evil", test_scan_evil);
	g_test_add_func("/term-hyperlink/encode-uri", test_encode_uri);
	g_test_add_func("/term-hyperlink/make-id", test_make_id);
	g_test_add_func("/term-hyperlink/detect", test_detect);
	g_test_add_func("/term-hyperlink/detect-capabilities", test_detect_capabilities);
	g_test_add_func("/term-hyperlink/term-primitives", test_term_primitives);
	g_test_add_func("/term-hyperlink/view-wrapped", test_view_wrapped);
	g_test_add_func("/term-hyperlink/view-partial", test_view_partial);
	g_test_add_func("/term-hyperlink/view-full-width", test_view_full_width);
	g_test_add_func("/term-hyperlink/view-colour-and-evil", test_view_colour_and_evil);
	g_test_add_func("/term-hyperlink/view-colour-ends-link", test_view_colour_ends_link);
	g_test_add_func("/term-hyperlink/side-panel-no-link", test_side_panel_no_link);
	g_test_add_func("/term-hyperlink/setting", test_setting);

	test_home_setup();

	core_preinit(argv[0]);
	core_init();
	fe_common_core_init();
	term_common_init();

	return g_test_run();
}
