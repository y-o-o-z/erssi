/*
 term-hyperlink.c : finding addresses in a line and making OSC 8 links of them

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <irssi/src/common.h>
#include <irssi/src/core/utf8.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/textbuffer-unformat.h>
#include <irssi/src/fe-ansi/term-hyperlink.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the schemes that become links: the web, and what IRC people share.
 * No file:, javascript:, data: or mailto: */
static const char *const SCHEMES[] = {
	"https", "http", "ftp", "gopher", "gemini", "ircs", "irc", NULL
};

/* one character of the line as it is shown, or a break (a newline, a
 * control, a character that is not text) */
typedef struct {
	gsize start, end;
	gunichar chr;
	gboolean brk;
} UNIT;

static guint64 link_salt;

/* A code point that never belongs to an address: controls (also the bidi
 * and other format characters, Cf), spaces and separators, surrogates,
 * private use, unassigned. */
static gboolean bad_unichar(gunichar c)
{
	switch (g_unichar_type(c)) {
	case G_UNICODE_CONTROL:
	case G_UNICODE_FORMAT:
	case G_UNICODE_UNASSIGNED:
	case G_UNICODE_PRIVATE_USE:
	case G_UNICODE_SURROGATE:
	case G_UNICODE_SPACE_SEPARATOR:
	case G_UNICODE_LINE_SEPARATOR:
	case G_UNICODE_PARAGRAPH_SEPARATOR:
		return TRUE;
	default:
		return c == 0xfffd;
	}
}

/* Every code point of a character the text view draws as one (a grapheme
 * cluster) must be fine, not only the first. */
static gboolean cluster_is_ok(const unsigned char *p, const unsigned char *end)
{
	while (p < end) {
		gunichar c = g_utf8_get_char_validated((const char *) p, end - p);

		if (c == (gunichar) -1 || c == (gunichar) -2 || bad_unichar(c))
			return FALSE;
		p = (const unsigned char *) g_utf8_next_char(p);
	}
	return TRUE;
}

/* A format sequence that leaves the colours alone: the text after it
 * looks like the text before it, so an address can go on through it. */
static gboolean format_keeps_colours(unsigned char c)
{
	return c == FORMAT_STYLE_BOLD || c == FORMAT_STYLE_UNDERLINE ||
	       c == FORMAT_STYLE_ITALIC || c == FORMAT_STYLE_MONOSPACE ||
	       c == FORMAT_STYLE_BLINK || c == FORMAT_STYLE_INDENT;
}

/* The line as the text view walks it (view_line_draw()): format
 * sequences are skipped, each character is one unit. A sequence that
 * changes a colour (also reverse and the defaults) is a break: the
 * sender could draw the rest of an address in the background colour
 * (or one next to it) and make a link whose target is not what anyone
 * sees. */
static GArray *line_units(const char *text, int charset)
{
	GArray *units = g_array_new(FALSE, FALSE, sizeof(UNIT));
	const unsigned char *base = (const unsigned char *) text;
	const unsigned char *p = base, *next;
	int color = 0, width;
	unsigned int fg24 = 0, bg24 = 0;

	while (*p != '\0') {
		UNIT u;

		if (*p == 4) {
			p++;
			if (*p == '\0')
				break;
			if (!format_keeps_colours(*p)) {
				u.start = u.end = p - 1 - base;
				u.chr = 0;
				u.brk = TRUE;
				g_array_append_val(units, u);
			}
			if (*p == FORMAT_STYLE_INDENT)
				p++;
			else
				textbuffer_unformat(&p, &color, &fg24, &bg24);
			continue;
		}

		u.start = p - base;
		u.brk = FALSE;
		if (*p < 0x20 || *p == 0x7f) {
			u.chr = *p;
			u.brk = TRUE;
			next = p + 1;
		} else if (*p < 0x80 && charset != TERM_TYPE_UTF8) {
			u.chr = *p;
			next = p + 1;
		} else if (charset != TERM_TYPE_UTF8) {
			/* an address in an 8-bit charset is ASCII; a Big5
			 * character is two bytes, as the text view draws it */
			u.chr = *p;
			u.brk = TRUE;
			next = p + (charset == TERM_TYPE_BIG5 && p[1] != '\0' &&
				    is_big5(p[0], p[1]) ? 2 : 1);
		} else {
			/* one character as the text view reads it: a grapheme
			 * cluster. One it does not draw whole is no part of an
			 * address. */
			u.chr = textbuffer_read_unichar(p, &next, &width);
			if (next <= p)
				next = p + 1;
			if (next - p >= TERM_LINK_CLUSTER_MAX || !cluster_is_ok(p, next))
				u.brk = TRUE;
		}
		u.end = next - base;
		g_array_append_val(units, u);
		p = next;
	}
	return units;
}

static gboolean unit_is_ascii(const UNIT *u, char c)
{
	return !u->brk && u->chr == (gunichar) (unsigned char) c;
}

/* May this character be part of an address? Trailing punctuation is
 * handled afterwards. */
static gboolean unit_in_url(const UNIT *u)
{
	if (u->brk)
		return FALSE;
	if (u->chr < 0x80)
		return u->chr > 0x20 && u->chr < 0x7f && u->chr != '<' && u->chr != '>' &&
		       u->chr != '"' && u->chr != '`';
	return TRUE;
}

/* the scheme starting at units[i] followed by "://": its length, or 0 */
static gsize scheme_at(const UNIT *units, guint count, guint i)
{
	int s;

	if (i > 0 && !units[i - 1].brk && units[i - 1].chr < 0x80 &&
	    g_ascii_isalnum(units[i - 1].chr))
		return 0;

	for (s = 0; SCHEMES[s] != NULL; s++) {
		const char *name = SCHEMES[s];
		gsize len = strlen(name), k;

		if (i + len + 3 > count)
			continue;
		for (k = 0; k < len; k++) {
			const UNIT *u = &units[i + k];

			if (u->brk || u->chr >= 0x80 || g_ascii_tolower(u->chr) != name[k])
				break;
		}
		if (k < len)
			continue;
		if (unit_is_ascii(&units[i + len], ':') && unit_is_ascii(&units[i + len + 1], '/') &&
		    unit_is_ascii(&units[i + len + 2], '/'))
			return len;
	}
	return 0;
}

/* Where the address units[from..] really ends: without the punctuation
 * that ends the sentence around it, and without a closing bracket that
 * has no opening one in it ("(see https://example.org/)"). The brackets
 * are counted once and then as they are cut off: linear also for a line
 * of nothing but ")". */
static guint trim_end(const UNIT *units, guint from, guint to)
{
	static const char OPEN[] = "([{", CLOSE[] = ")]}";
	guint opened[3] = { 0, 0, 0 }, closed[3] = { 0, 0, 0 }, i;

	for (i = from; i < to; i++) {
		const char *b;

		if (units[i].brk || units[i].chr >= 0x80 || units[i].chr == 0)
			continue;
		if ((b = strchr(OPEN, (char) units[i].chr)) != NULL)
			opened[b - OPEN]++;
		else if ((b = strchr(CLOSE, (char) units[i].chr)) != NULL)
			closed[b - CLOSE]++;
	}

	while (to > from) {
		const UNIT *last = &units[to - 1];

		if (last->chr < 0x80) {
			char c = (char) last->chr;
			const char *b;

			if (c != '\0' && strchr(".,;:!?'*", c) != NULL) {
				to--;
				continue;
			}
			if (c != '\0' && (b = strchr(CLOSE, c)) != NULL &&
			    closed[b - CLOSE] > opened[b - CLOSE]) {
				closed[b - CLOSE]--;
				to--;
				continue;
			}
			break;
		}
		switch (g_unichar_type(last->chr)) {
		case G_UNICODE_OTHER_PUNCTUATION:
		case G_UNICODE_CLOSE_PUNCTUATION:
		case G_UNICODE_FINAL_PUNCTUATION:
		case G_UNICODE_INITIAL_PUNCTUATION:
			to--;
			continue;
		default:
			break;
		}
		break;
	}
	return to;
}

char *term_link_encode_uri(const char *text, gsize len)
{
	static const char hex[] = "0123456789ABCDEF";
	GString *out;
	gsize i;

	out = g_string_sized_new(len);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) text[i];

		if (c <= 0x20 || c == 0x7f || c == '<' || c == '>' || c == '"') {
			g_string_free(out, TRUE);
			return NULL;
		}
		if (c >= 0x80 || strchr("\\^`{|}", c) != NULL ||
		    (c == '%' && (i + 2 >= len || !g_ascii_isxdigit(text[i + 1]) ||
		                  !g_ascii_isxdigit(text[i + 2])))) {
			g_string_append_c(out, '%');
			g_string_append_c(out, hex[c >> 4]);
			g_string_append_c(out, hex[c & 15]);
		} else {
			g_string_append_c(out, c);
		}
		if (out->len > TERM_LINK_URI_MAX) {
			g_string_free(out, TRUE);
			return NULL;
		}
	}
	return g_string_free(out, FALSE);
}

gboolean term_link_uri_is_safe(const char *uri)
{
	const unsigned char *p;

	if (uri == NULL || *uri == '\0' || strlen(uri) > TERM_LINK_URI_MAX)
		return FALSE;
	for (p = (const unsigned char *) uri; *p != '\0'; p++)
		if (*p <= 0x20 || *p >= 0x7f)
			return FALSE;
	return TRUE;
}

gboolean term_link_id_is_safe(const char *id)
{
	const char *p;

	if (id == NULL || *id == '\0' || strlen(id) >= TERM_LINK_ID_SIZE)
		return FALSE;
	for (p = id; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p))
			return FALSE;
	return TRUE;
}

TermLinkList *term_link_scan(const char *text, int charset)
{
	GArray *units, *spans;
	TermLinkList *list;
	const UNIT *u;
	guint i, count;

	/* the common case, a line without an address */
	if (text == NULL || strstr(text, "://") == NULL)
		return NULL;

	units = line_units(text, charset);
	u = (const UNIT *) units->data;
	count = units->len;
	spans = g_array_new(FALSE, TRUE, sizeof(TermLinkSpan));

	for (i = 0; i < count;) {
		gsize len = scheme_at(u, count, i);
		guint first = i, body, end, k;
		GString *shown;
		TermLinkSpan span;

		if (len == 0) {
			i++;
			continue;
		}
		body = i + len + 3;
		for (end = body; end < count && unit_in_url(&u[end]); end++)
			;
		/* the next address starts after this one, whatever it is */
		k = end;
		end = trim_end(u, body, end);
		/* a host: a letter or digit, or [ of an IPv6 address */
		if (end == body || !(u[body].chr == '[' || g_unichar_isalnum(u[body].chr))) {
			i = k > i ? k : i + 1;
			continue;
		}

		shown = g_string_new(NULL);
		for (; i < end; i++)
			g_string_append_len(shown, text + u[i].start, u[i].end - u[i].start);
		memset(&span, 0, sizeof(span));
		span.uri = term_link_encode_uri(shown->str, shown->len);
		g_string_free(shown, TRUE);
		if (span.uri != NULL) {
			span.start = u[first].start;
			span.end = u[end - 1].end;
			g_array_append_val(spans, span);
		}
		i = end;
	}
	g_array_free(units, TRUE);

	if (spans->len == 0) {
		g_array_free(spans, TRUE);
		return NULL;
	}
	list = g_malloc0(sizeof(TermLinkList) + sizeof(TermLinkSpan) * (spans->len - 1));
	list->count = spans->len;
	memcpy(list->spans, spans->data, sizeof(TermLinkSpan) * spans->len);
	g_array_free(spans, TRUE);
	return list;
}

void term_link_list_free(TermLinkList *list)
{
	int i;

	if (list == NULL)
		return;
	for (i = 0; i < list->count; i++)
		g_free(list->spans[i].uri);
	g_free(list);
}

void term_link_set_salt(guint64 salt)
{
	link_salt = salt;
}

static guint64 fnv1a(guint64 hash, guint64 value)
{
	int i;

	for (i = 0; i < 8; i++) {
		hash ^= (value >> (i * 8)) & 0xff;
		hash *= G_GUINT64_CONSTANT(0x100000001b3);
	}
	return hash;
}

void term_link_make_id(guint64 key, guint index, char id[TERM_LINK_ID_SIZE])
{
	guint64 hash = G_GUINT64_CONSTANT(0xcbf29ce484222325);

	hash = fnv1a(hash, link_salt);
	hash = fnv1a(hash, key);
	hash = fnv1a(hash, index);
	g_snprintf(id, TERM_LINK_ID_SIZE, "e%016" G_GINT64_MODIFIER "x", hash);
}

void term_link_env_from_process(TermLinkEnv *env)
{
	env->term = g_getenv("TERM");
	env->term_program = g_getenv("TERM_PROGRAM");
	env->lc_terminal = g_getenv("LC_TERMINAL");
	env->kitty_window_id = g_getenv("KITTY_WINDOW_ID");
	env->ghostty_resources_dir = g_getenv("GHOSTTY_RESOURCES_DIR");
	env->wezterm_pane = g_getenv("WEZTERM_PANE");
	env->vte_version = g_getenv("VTE_VERSION");
	env->konsole_version = g_getenv("KONSOLE_VERSION");
	env->wt_session = g_getenv("WT_SESSION");
	env->inside_emacs = g_getenv("INSIDE_EMACS");
	env->tmux = g_getenv("TMUX");
	env->sty = g_getenv("STY");
	env->tty = isatty(0) ? ttyname(0) : NULL;
}

static gboolean is_set(const char *s)
{
	return s != NULL && *s != '\0';
}

static gboolean is_str(const char *s, const char *value)
{
	return s != NULL && strcmp(s, value) == 0;
}

static gboolean has_prefix(const char *s, const char *prefix)
{
	return s != NULL && g_str_has_prefix(s, prefix);
}

/* vt100, vt220, vt52...: the DEC terminals and what emulates only them;
 * not vte-256color, the TERM of VTE itself */
static gboolean is_vt(const char *term)
{
	return has_prefix(term, "vt") && (term[2] == '\0' || g_ascii_isdigit(term[2]));
}

/* The text console of the system: the Linux console (/dev/tty1), FreeBSD
 * syscons and vt (/dev/ttyv0, which say TERM=xterm), wscons of NetBSD and
 * OpenBSD (/dev/ttyC0, /dev/ttyE0), and serial lines. None of them knows
 * OSC; the Linux console and FreeBSD print the text of the sequence. */
static gboolean tty_is_console(const char *tty)
{
	const char *name;

	if (tty == NULL)
		return FALSE;
	if (strcmp(tty, "/dev/console") == 0)
		return TRUE;
	if (!g_str_has_prefix(tty, "/dev/tty"))
		return FALSE;
	name = tty + strlen("/dev/tty");
	return g_ascii_isdigit(name[0]) || name[0] == 'v' || name[0] == 'C' || name[0] == 'E' ||
	       name[0] == 'S' || name[0] == 'u' || g_str_has_prefix(name, "USB") ||
	       g_str_has_prefix(name, "AMA");
}

gboolean term_link_detect(const TermLinkEnv *env)
{
	const char *term = env->term;

	/* GNU screen: it does not pass OSC 8 on (screen 4 swallows it, and
	 * the cells would not carry it on a redraw); over ssh from inside
	 * screen only TERM says so */
	if (is_set(env->sty))
		return FALSE;
	if (!is_set(env->tmux) && has_prefix(term, "screen"))
		return FALSE;

	if (!is_set(term) || is_str(term, "dumb") || has_prefix(term, "linux") ||
	    has_prefix(term, "cons") || is_vt(term) || has_prefix(term, "ansi") ||
	    has_prefix(term, "eterm") || has_prefix(term, "emacs") || has_prefix(term, "wsvt") ||
	    has_prefix(term, "pccon") || is_set(env->inside_emacs))
		return FALSE;
	if (tty_is_console(env->tty))
		return FALSE;

	/* tmux reads the sequence itself: 3.4 and later keep the link with
	 * the cells and give it to the outer terminal when that one has the
	 * hyperlinks feature (terminal-features), older versions drop it */
	if (is_set(env->tmux))
		return TRUE;

	if (is_str(env->term_program, "iTerm.app") || is_str(env->lc_terminal, "iTerm2") ||
	    is_str(env->term_program, "WezTerm") || is_set(env->wezterm_pane) ||
	    is_str(env->term_program, "ghostty") || is_set(env->ghostty_resources_dir) ||
	    is_str(term, "xterm-ghostty") || has_prefix(term, "foot") ||
	    is_set(env->kitty_window_id) || is_str(term, "xterm-kitty") ||
	    has_prefix(term, "alacritty") || has_prefix(term, "contour") ||
	    is_str(env->term_program, "vscode") || is_set(env->wt_session) ||
	    is_set(env->konsole_version))
		return TRUE;

	/* GNOME Terminal, Tilix, Terminator, xfce4-terminal...: VTE knows
	 * OSC 8 from 0.50 */
	if (is_set(env->vte_version) && atoi(env->vte_version) >= 5000)
		return TRUE;

	/* not known: xterm, PuTTY, rxvt and most others ignore it, but not
	 * every terminal that calls itself xterm does (the FreeBSD console) */
	return FALSE;
}
