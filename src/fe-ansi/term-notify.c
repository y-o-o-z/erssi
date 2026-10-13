#include "term-notify.h"

#include <string.h>

#define NOTIFY_FIELD_MAX 200

void term_notify_env_from_process(TermNotifyEnv *env)
{
	env->term = g_getenv("TERM");
	env->term_program = g_getenv("TERM_PROGRAM");
	env->lc_terminal = g_getenv("LC_TERMINAL");
	env->kitty_window_id = g_getenv("KITTY_WINDOW_ID");
	env->ghostty_resources_dir = g_getenv("GHOSTTY_RESOURCES_DIR");
	env->wezterm_pane = g_getenv("WEZTERM_PANE");
	env->tmux = g_getenv("TMUX");
	env->sty = g_getenv("STY");
}

static gboolean is_set(const char *s)
{
	return s != NULL && *s != '\0';
}

static gboolean is_str(const char *s, const char *value)
{
	return s != NULL && strcmp(s, value) == 0;
}

/* Characters that change the direction of the text around them, or
 * start a new line: a nick could be made to look like another one. */
static gboolean is_layout_control(gunichar c)
{
	return c == 0x061c || c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202e) ||
	       (c >= 0x2066 && c <= 0x2069) || c == 0x2028 || c == 0x2029;
}

TermNotifyProto term_notify_detect(const TermNotifyEnv *env)
{
	/* screen swallows or mangles the sequences */
	if (is_set(env->sty))
		return TERM_NOTIFY_NONE;

	if (is_str(env->term_program, "iTerm.app") || is_str(env->lc_terminal, "iTerm2"))
		return TERM_NOTIFY_OSC9;

	if (is_str(env->term_program, "WezTerm") || is_set(env->wezterm_pane))
		return TERM_NOTIFY_OSC777;

	if (is_str(env->term_program, "ghostty") || is_set(env->ghostty_resources_dir) ||
	    is_str(env->term, "xterm-ghostty"))
		return TERM_NOTIFY_OSC777;

	if (env->term != NULL && g_str_has_prefix(env->term, "foot"))
		return TERM_NOTIFY_OSC777;

	/* kitty documents the legacy iTerm2 OSC 9 protocol */
	if (is_set(env->kitty_window_id) || is_str(env->term, "xterm-kitty"))
		return TERM_NOTIFY_OSC9;

	return TERM_NOTIFY_NONE;
}

char *term_notify_clean(const char *text, gsize max_bytes, gboolean no_semicolon)
{
	GString *out = g_string_new(NULL);
	gboolean cut = FALSE;
	gsize left;

	if (text == NULL)
		return g_string_free(out, FALSE);

	left = strlen(text);
	while (left > 0) {
		gunichar c = g_utf8_get_char_validated(text, left);
		gsize len;
		char buf[6];

		if (c == (gunichar) -1 || c == (gunichar) -2) {
			/* invalid or truncated sequence: drop one byte */
			text++;
			left--;
			continue;
		}
		len = g_utf8_skip[(guchar) *text];
		text += len;
		left -= len;

		if (c < 0x20 || c == 0x7f || (c >= 0x80 && c <= 0x9f) || is_layout_control(c))
			continue;
		if (c == ';' && no_semicolon)
			c = ',';

		len = g_unichar_to_utf8(c, buf);
		if (out->len + len > max_bytes) {
			cut = TRUE;
			break;
		}
		g_string_append_len(out, buf, len);
	}

	if (cut)
		g_string_append(out, "\xe2\x80\xa6");
	return g_string_free(out, FALSE);
}

char *term_notify_encode(TermNotifyProto proto, gboolean tmux, const char *title,
                         const char *body)
{
	char *t, *b, *seq;
	GString *out;
	const char *p;

	if (proto != TERM_NOTIFY_OSC9 && proto != TERM_NOTIFY_OSC777)
		return NULL;

	t = term_notify_clean(title, NOTIFY_FIELD_MAX, proto == TERM_NOTIFY_OSC777);
	b = term_notify_clean(body, NOTIFY_FIELD_MAX, proto == TERM_NOTIFY_OSC777);
	if (proto == TERM_NOTIFY_OSC9)
		seq = g_strconcat("\033]9;", t, ": ", b, "\007", NULL);
	else
		seq = g_strconcat("\033]777;notify;", t, ";", b, "\007", NULL);
	g_free(t);
	g_free(b);

	if (!tmux)
		return seq;

	out = g_string_new("\033Ptmux;");
	for (p = seq; *p != '\0'; p++) {
		if (*p == '\033')
			g_string_append_c(out, '\033');
		g_string_append_c(out, *p);
	}
	g_string_append(out, "\033\\");
	g_free(seq);
	return g_string_free(out, FALSE);
}

int term_focus_filter(unsigned char *buf, int len, int *focus)
{
	int in, out = 0;

	for (in = 0; in < len;) {
		if (len - in >= 3 && buf[in] == '\033' && buf[in + 1] == '[' &&
		    (buf[in + 2] == 'I' || buf[in + 2] == 'O')) {
			*focus = buf[in + 2] == 'I' ? 1 : 0;
			in += 3;
			continue;
		}
		buf[out++] = buf[in++];
	}
	return out;
}

gboolean term_notify_throttle(gint64 now_ms, gint64 *last_ms, int min_ms)
{
	if (*last_ms != 0 && now_ms - *last_ms < min_ms)
		return FALSE;
	*last_ms = now_ms;
	return TRUE;
}
