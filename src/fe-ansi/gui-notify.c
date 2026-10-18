/*
 gui-notify.c : desktop notifications for mentions through the terminal

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "module.h"
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-ansi/gui-notify.h>
#include <irssi/src/fe-ansi/term-notify.h>
#include <irssi/src/fe-ansi/term.h>

#define NOTIFY_TITLE "erssi"
/* at most one notification in this many milliseconds */
#define NOTIFY_INTERVAL_MS 3000
/* where and nick come from the server too: keep them short */
#define NOTIFY_NAME_MAX 64
#define NOTIFY_TEXT_MAX 200

/* the order of the term_notify choices */
enum { NOTIFY_OFF, NOTIFY_AUTO, NOTIFY_OSC9, NOTIFY_OSC777 };

static TermNotifyProto proto;
static gboolean in_tmux;
static int focused = TRUE;
static gint64 last_ms;

/* A copy of the cleaned (valid UTF-8) text with every non-ASCII
 * character as '?'. For a terminal that is not UTF-8: it would read the
 * bytes 0x80-0x9f of UTF-8 as C1 controls (C3 9B is "A~" and CSI). */
static char *ascii_only(const char *text)
{
	GString *out = g_string_new(NULL);
	const char *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p))
		g_string_append_c(out, (guchar) *p < 0x80 ? *p : '?');
	return g_string_free(out, FALSE);
}

static void sig_mention(void *server, const char *kind, const char *where, const char *nick,
                        const char *text)
{
	char *w, *n, *t, *body, *seq;

	if (proto == TERM_NOTIFY_NONE || focused)
		return;
	if (!term_notify_throttle(g_get_monotonic_time() / 1000, &last_ms, NOTIFY_INTERVAL_MS))
		return;

	/* everything here is remote text: each part is cleaned on its own,
	 * term_notify_encode() cleans the whole again */
	w = term_notify_clean(where, NOTIFY_NAME_MAX, FALSE);
	n = term_notify_clean(nick, NOTIFY_NAME_MAX, FALSE);
	if (settings_get_bool("term_notify_text")) {
		t = term_notify_clean(text, NOTIFY_TEXT_MAX, FALSE);
		body = g_strdup_printf("%s %s: %s", w, n, t);
		g_free(t);
	} else {
		body = g_strdup_printf("%s %s", w, n);
	}
	if (term_type != TERM_TYPE_UTF8) {
		char *ascii = ascii_only(body);

		g_free(body);
		body = ascii;
	}

	seq = term_notify_encode(proto, in_tmux, NOTIFY_TITLE, body);
	if (seq != NULL)
		term_write_raw(seq);

	g_free(seq);
	g_free(body);
	g_free(n);
	g_free(w);
}

static void sig_focus_changed(gpointer focusp)
{
	focused = GPOINTER_TO_INT(focusp);
}

static void read_settings(void)
{
	TermNotifyEnv env;
	TermNotifyProto new_proto;

	term_notify_env_from_process(&env);
	switch (settings_get_choice("term_notify")) {
	case NOTIFY_AUTO:
		new_proto = term_notify_detect(&env);
		break;
	case NOTIFY_OSC9:
		new_proto = TERM_NOTIFY_OSC9;
		break;
	case NOTIFY_OSC777:
		new_proto = TERM_NOTIFY_OSC777;
		break;
	default:
		new_proto = TERM_NOTIFY_NONE;
		break;
	}
	/* under screen (STY) a forced protocol is sent as it is */
	in_tmux = env.tmux != NULL && *env.tmux != '\0';

	/* focus reports are asked for only when they are used: with an
	 * unknown terminal ESC [ I stays a key */
	if ((new_proto != TERM_NOTIFY_NONE) != (proto != TERM_NOTIFY_NONE)) {
		term_set_focus_reporting(new_proto != TERM_NOTIFY_NONE);
		focused = TRUE;
	}
	proto = new_proto;
}

void gui_notify_init(void)
{
	settings_add_choice("lookandfeel", "term_notify", NOTIFY_OFF, "off;auto;osc9;osc777");
	settings_add_bool("lookandfeel", "term_notify_text", TRUE);

	proto = TERM_NOTIFY_NONE;
	focused = TRUE;
	last_ms = 0;
	read_settings();

	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
	signal_add("terminal focus changed", (SIGNAL_FUNC) sig_focus_changed);
	signal_add("mention", (SIGNAL_FUNC) sig_mention);
}

void gui_notify_deinit(void)
{
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);
	signal_remove("terminal focus changed", (SIGNAL_FUNC) sig_focus_changed);
	signal_remove("mention", (SIGNAL_FUNC) sig_mention);

	if (proto != TERM_NOTIFY_NONE)
		term_set_focus_reporting(FALSE);
	proto = TERM_NOTIFY_NONE;
}
