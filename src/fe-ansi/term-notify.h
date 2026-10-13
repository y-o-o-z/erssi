#ifndef IRSSI_FE_ANSI_TERM_NOTIFY_H
#define IRSSI_FE_ANSI_TERM_NOTIFY_H

#include <glib.h>

/* Pure helpers for desktop notifications through the terminal (OSC 9 /
 * OSC 777) and for the focus reports (CSI I / CSI O) a terminal sends
 * once focus reporting is switched on. Nothing here touches the terminal
 * or any global state. Text that ends up in an escape sequence comes from
 * remote IRC users, so it always goes through term_notify_clean(). */

typedef enum {
	TERM_NOTIFY_NONE,
	TERM_NOTIFY_OSC9,
	TERM_NOTIFY_OSC777
} TermNotifyProto;

/* The environment variables that identify the terminal; NULL when unset. */
typedef struct {
	const char *term, *term_program, *lc_terminal, *kitty_window_id, *ghostty_resources_dir,
	    *wezterm_pane, *tmux, *sty;
} TermNotifyEnv;

/* Fill env from the process environment. The strings belong to the
 * environment, do not free them. */
void term_notify_env_from_process(TermNotifyEnv *env);

/* Which protocol the terminal understands; NONE under screen (STY) and
 * for unknown terminals. */
TermNotifyProto term_notify_detect(const TermNotifyEnv *env);

/* A copy of text that is safe inside an escape sequence: no byte below
 * 0x20, no 0x7f, no U+0080..U+009F, no invalid UTF-8. With no_semicolon
 * ';' becomes ','. Cut at a character boundary to at most max_bytes, and
 * the ellipsis (U+2026, 3 bytes) is appended when something was cut. */
char *term_notify_clean(const char *text, gsize max_bytes, gboolean no_semicolon);

/* The complete sequence to write to the terminal, or NULL for NONE. With
 * tmux it is wrapped in the tmux passthrough. Free with g_free(). */
char *term_notify_encode(TermNotifyProto proto, gboolean tmux, const char *title,
                         const char *body);

/* Remove every complete ESC [ I and ESC [ O from buf in place and return
 * the new length. *focus is set to 1 (I) or 0 (O) for the last one seen
 * and left alone if there was none. */
int term_focus_filter(unsigned char *buf, int len, int *focus);

/* TRUE, and *last_ms = now_ms, when *last_ms is 0 or at least min_ms have
 * passed since it. */
gboolean term_notify_throttle(gint64 now_ms, gint64 *last_ms, int min_ms);

#endif
