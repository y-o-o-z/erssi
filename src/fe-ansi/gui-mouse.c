/*
 * gui-mouse.c : Mouse event handling and SGR protocol parsing for irssi
 *
 * Copyright (C) 2025 Evolved Irssi Team
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <irssi/src/common.h>
#include <irssi/src/core/signals.h>
#include "gui-mouse.h"

/* Mouse event handler chain */
typedef struct GuiMouseHandler {
	GuiMouseEventCallback callback;
	gpointer user_data;
	struct GuiMouseHandler *next;
} GuiMouseHandler;

/* Mouse tracking state */
static gboolean mouse_tracking_enabled = FALSE;
static GuiMouseHandler *mouse_handlers = NULL;

/* SGR mouse parser state */
static int mouse_state = 0; /* 0 idle, >0 reading sequence */
static char mouse_buf[64];
static int mouse_len = 0;
static gboolean mouse_overflow = FALSE; /* an SGR report longer than mouse_buf */
static int esc_pending = 0;
static int esc_timeout_tag = -1;
static int reemit_guard = 0;

/* Forward declarations */
static gboolean esc_timeout_callback(gpointer data);
static void fire_mouse_event(const GuiMouseEvent *event);

/* ESC sequence timeout handler */
static gboolean esc_timeout_callback(gpointer data)
{
	(void) data;
	if (esc_pending && !reemit_guard) {
		reemit_guard = 1;
		signal_emit("gui key pressed", 1, GINT_TO_POINTER(0x1b));
		reemit_guard = 0;
		esc_pending = 0;
	}
	esc_timeout_tag = -1;
	mouse_state = 0;
	mouse_len = 0;
	return FALSE;
}

/* Initialize mouse system */
void gui_mouse_init(void)
{
	mouse_tracking_enabled = FALSE;
	mouse_handlers = NULL;
	mouse_state = 0;
	mouse_len = 0;
	mouse_overflow = FALSE;
	esc_pending = 0;
	reemit_guard = 0;
	if (esc_timeout_tag != -1) {
		g_source_remove(esc_timeout_tag);
		esc_timeout_tag = -1;
	}
}

/* Cleanup mouse system */
void gui_mouse_deinit(void)
{
	GuiMouseHandler *handler, *next;
	
	/* Disable tracking */
	if (mouse_tracking_enabled) {
		gui_mouse_disable_tracking();
	}
	
	/* Clean up handler chain */
	for (handler = mouse_handlers; handler; handler = next) {
		next = handler->next;
		g_free(handler);
	}
	mouse_handlers = NULL;
	
	/* Clean up timeouts */
	if (esc_timeout_tag != -1) {
		g_source_remove(esc_timeout_tag);
		esc_timeout_tag = -1;
	}
}

/* Enable mouse tracking */
void gui_mouse_enable_tracking(void)
{
	fputs("\x1b[?1000h", stdout);
	fputs("\x1b[?1002h", stdout);  /* Enable button event tracking for gestures */
	fputs("\x1b[?1006h", stdout);
	fflush(stdout);
	mouse_tracking_enabled = TRUE;
}

/* Disable mouse tracking */
void gui_mouse_disable_tracking(void)
{
	fputs("\x1b[?1006l", stdout);
	fputs("\x1b[?1002l", stdout);  /* Disable button event tracking */
	fputs("\x1b[?1000l", stdout);
	fflush(stdout);
	mouse_tracking_enabled = FALSE;
}

/* Check if mouse tracking is enabled */
gboolean gui_mouse_is_tracking_enabled(void)
{
	return mouse_tracking_enabled;
}

/* Add mouse event handler to chain */
void gui_mouse_add_handler(GuiMouseEventCallback callback, gpointer user_data)
{
	GuiMouseHandler *handler;
	
	if (!callback) return;
	
	handler = g_new0(GuiMouseHandler, 1);
	handler->callback = callback;
	handler->user_data = user_data;
	handler->next = mouse_handlers;
	mouse_handlers = handler;
}

/* Remove mouse event handler from chain */
void gui_mouse_remove_handler(GuiMouseEventCallback callback, gpointer user_data)
{
	GuiMouseHandler **handler_ptr;
	GuiMouseHandler *handler;
	
	for (handler_ptr = &mouse_handlers; *handler_ptr; handler_ptr = &(*handler_ptr)->next) {
		handler = *handler_ptr;
		if (handler->callback == callback && handler->user_data == user_data) {
			*handler_ptr = handler->next;
			g_free(handler);
			break;
		}
	}
}

/* Fire mouse event through handler chain */
static void fire_mouse_event(const GuiMouseEvent *event)
{
	GuiMouseHandler *handler;
	
	for (handler = mouse_handlers; handler; handler = handler->next) {
		if (handler->callback(event, handler->user_data)) {
			/* Event consumed by handler */
			break;
		}
	}
}

/* Check if raw button indicates scroll event */
gboolean gui_mouse_is_scroll_event(int raw_button)
{
	return (raw_button & 64) != 0;
}

/* Check if raw button indicates drag/motion event */
gboolean gui_mouse_is_drag_event(int raw_button)
{
	return (raw_button & 32) != 0;
}

/* back to idle, the sequence read so far dropped */
static void mouse_reset(void)
{
	mouse_state = 0;
	mouse_len = 0;
	mouse_overflow = FALSE;
	esc_pending = 0;
	if (esc_timeout_tag != -1) {
		g_source_remove(esc_timeout_tag);
		esc_timeout_tag = -1;
	}
}

/* "btn;x;y", three numbers of up to 5 digits each; FALSE for anything else */
static gboolean sgr_fields(const char *s, int *btn, int *x, int *y)
{
	int *out[3] = { btn, x, y };
	int i, n, digits;

	for (i = 0; i < 3; i++) {
		n = 0;
		for (digits = 0; *s >= '0' && *s <= '9'; s++, digits++) {
			if (digits == 5)
				return FALSE;
			n = n * 10 + (*s - '0');
		}
		if (digits == 0 || *s != (i < 2 ? ';' : '\0'))
			return FALSE;
		*out[i] = n;
		if (i < 2)
			s++;
	}
	return TRUE;
}

/* x and y 0-based */
static void fire_parsed_event(int braw, int x, int y, gboolean press)
{
	GuiMouseEvent event;

	memset(&event, 0, sizeof(event));
	event.x = x;
	event.y = y;
	event.press = press;
	event.valid = TRUE;
	event.raw_button = braw;
	if (gui_mouse_is_scroll_event(braw))
		event.button = (braw == 64) ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN;
	else
		event.button = (braw & 3) + 1;
	fire_mouse_event(&event);
}

/* Main SGR mouse parsing function */
gboolean gui_mouse_try_parse_key(gunichar key)
{
	char *s;
	int braw;
	int x;
	int y;
	gboolean press;

	if (reemit_guard)
		return FALSE; /* Don't process re-emitted keys */
		
	if (mouse_state == 0) {
		if (key == 0x1b) {
			mouse_state = 1;
			mouse_len = 0;
			esc_pending = 1;
			/* Start timeout to distinguish fast mouse ESC from user ESC+key */
			if (esc_timeout_tag != -1)
				g_source_remove(esc_timeout_tag);
			esc_timeout_tag = g_timeout_add(50, esc_timeout_callback, NULL);
			return TRUE;
		}
		return FALSE;
	} else if (mouse_state == 1) {
		if (key == '[') {
			/* Cancel timeout - might be mouse sequence or arrow keys */
			if (esc_timeout_tag != -1) {
				g_source_remove(esc_timeout_tag);
				esc_timeout_tag = -1;
			}
			mouse_state = 2;
			esc_pending = 0; /* clear pending since we'll handle this */
			return TRUE;
		}
		if (key == 'O') {
			/* This is ESC O - application mode arrow keys, re-emit immediately */
			if (esc_timeout_tag != -1) {
				g_source_remove(esc_timeout_tag);
				esc_timeout_tag = -1;
			}
			mouse_state = 3; /* special state for ESC O sequences */
			esc_pending = 0; /* clear pending since we'll handle this immediately */
			return TRUE;
		}
		/* Not SGR - cancel timeout and re-emit ESC */
		if (esc_timeout_tag != -1) {
			g_source_remove(esc_timeout_tag);
			esc_timeout_tag = -1;
		}
		mouse_state = 0;
		mouse_len = 0;
		if (esc_pending && !reemit_guard) {
			reemit_guard = 1;
			signal_emit("gui key pressed", 1, GINT_TO_POINTER(0x1b));
			reemit_guard = 0;
			esc_pending = 0;
		}
		return FALSE;
	} else if (mouse_state == 3) {
		/* ESC O sequence - re-emit ESC O and current key */
		mouse_state = 0;
		mouse_len = 0;
		esc_pending = 0;
		/* Cancel any pending timeout */
		if (esc_timeout_tag != -1) {
			g_source_remove(esc_timeout_tag);
			esc_timeout_tag = -1;
		}
		if (!reemit_guard) {
			reemit_guard = 1;
			signal_emit("gui key pressed", 1, GINT_TO_POINTER(0x1b));
			signal_emit("gui key pressed", 1, GINT_TO_POINTER('O'));
			signal_emit("gui key pressed", 1, GINT_TO_POINTER(key));
			reemit_guard = 0;
		}
		return TRUE;
	} else if (mouse_state == 4) {
		/* ESC [ M Cb Cx Cy: the report of a terminal without SGR mode
		   (GNU screen 4); each of the three bytes is its value + 32 */
		if (key < 32 || key > 255 || mouse_len >= 3) {
			mouse_reset();
			return FALSE;
		}
		mouse_buf[mouse_len++] = (char) (key - 32);
		if (mouse_len < 3)
			return TRUE;
		braw = (unsigned char) mouse_buf[0];
		x = (unsigned char) mouse_buf[1] - 1;
		y = (unsigned char) mouse_buf[2] - 1;
		mouse_reset();
		/* a release names no button: it is taken as the left one,
		   the one the gestures follow */
		press = gui_mouse_is_scroll_event(braw) || (braw & 3) != 3;
		if (!press)
			braw &= ~3;
		if (x >= 0 && y >= 0)
			fire_parsed_event(braw, x, y, press);
		return TRUE;
	} else if (mouse_state >= 2) {
		/* If we receive ESC while parsing, abort current sequence and start new one */
		if (key == 0x1b) {
			/* Discard incomplete sequence, start fresh */
			mouse_state = 1;
			mouse_len = 0;
			mouse_overflow = FALSE;
			esc_pending = 1;
			if (esc_timeout_tag != -1)
				g_source_remove(esc_timeout_tag);
			esc_timeout_tag = g_timeout_add(50, esc_timeout_callback, NULL);
			return TRUE;
		}

		if (mouse_len == 0 && key == 'M') {
			mouse_state = 4;
			return TRUE;
		}

		/* SGR mouse report: ESC [ < btn ; x ; y M (press) or m (release),
		   nothing but digits and ';' between */
		if (mouse_len > 0 && mouse_buf[0] == '<') {
			if ((key >= '0' && key <= '9') || key == ';') {
				if (mouse_len < (int) sizeof(mouse_buf) - 1)
					mouse_buf[mouse_len++] = (char) key;
				else
					mouse_overflow = TRUE;
				return TRUE;
			}
			if (key != 'M' && key != 'm') {
				/* not a report, or one cut off: dropped, and the
				   key is typed as usual */
				mouse_reset();
				return FALSE;
			}
			mouse_buf[mouse_len] = '\0';
			if (!mouse_overflow && sgr_fields(mouse_buf + 1, &braw, &x, &y)) {
				mouse_reset();
				fire_parsed_event(braw, x - 1, y - 1, key == 'M');
			} else {
				mouse_reset();
			}
			return TRUE;
		}

		/* a CSI sequence that fills mouse_buf ends below, so it
		   always has room for one more */
		if (mouse_len < 0 || mouse_len > (int) sizeof(mouse_buf) - 2)
			mouse_len = 0;
		mouse_buf[mouse_len++] = (char) key;
		mouse_buf[mouse_len] = '\0';
		s = mouse_buf;
		
		/* Check if this is arrow keys (A/B/C/D) or other ESC sequences */
		/* But NOT if first char is '<' (SGR mouse prefix) */
		if (mouse_len == 1 && mouse_buf[0] != '<' &&
		    (key == 'A' || key == 'B' || key == 'C' || key == 'D' ||
		     key == 'H' || key == 'F' || key == '1' || key == '2' ||
		     key == '3' || key == '4' || key == '5' || key == '6')) {
			/* This is arrow key or function key - re-emit ESC[ and current key */
			mouse_state = 0;
			mouse_len = 0;
			esc_pending = 0;
			if (esc_timeout_tag != -1) {
				g_source_remove(esc_timeout_tag);
				esc_timeout_tag = -1;
			}
			if (!reemit_guard) {
				reemit_guard = 1;
				signal_emit("gui key pressed", 1, GINT_TO_POINTER(0x1b));
				signal_emit("gui key pressed", 1, GINT_TO_POINTER('['));
				signal_emit("gui key pressed", 1, GINT_TO_POINTER(key));
				reemit_guard = 0;
			}
			return TRUE;
		}
		
		if (*s == '<')
			return TRUE; /* the SGR report begins */

		/* Another CSI sequence. It ends at any final byte 0x40-0x7e
		 * (ECMA-48), except a '[' right after ESC [ (the Linux
		 * console's ESC [ [ A F-keys), or when it no longer fits */
		if ((key >= 0x40 && key <= 0x7e && !(key == '[' && mouse_len == 1)) ||
		    mouse_len == (int) sizeof(mouse_buf) - 1) {
			/* Complete escape sequence - re-emit all of it, the
			 * final byte included */
			int len = mouse_len;

			mouse_state = 0;
			mouse_len = 0;
			esc_pending = 0;
			if (esc_timeout_tag != -1) {
				g_source_remove(esc_timeout_tag);
				esc_timeout_tag = -1;
			}
			if (!reemit_guard) {
				int i;
				reemit_guard = 1;
				signal_emit("gui key pressed", 1, GINT_TO_POINTER(0x1b));
				signal_emit("gui key pressed", 1, GINT_TO_POINTER('['));
				for (i = 0; i < len; i++) {
					signal_emit("gui key pressed", 1, GINT_TO_POINTER((unsigned char)mouse_buf[i]));
				}
				reemit_guard = 0;
			}
			return TRUE;
		}
		return TRUE; /* Continue reading */
	}
	return FALSE;
}