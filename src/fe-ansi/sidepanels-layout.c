/*
 * sidepanels-layout.c : erssi
 *
 * Copyright (C) 2024-2025 erssi-org team
 * Lead Developer: Jerzy (kofany) Dąbrowski <https://github.com/kofany>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "module.h"
#include <irssi/src/core/signals.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/fe-ansi/mainwindows.h>
#include <irssi/src/fe-ansi/sidepanels.h>
#include <irssi/src/fe-ansi/sidepanels-layout.h>
#include <irssi/src/fe-ansi/sidepanels-render.h>
#include <irssi/src/fe-ansi/resize-debug.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/channels.h>
#include <irssi/src/core/queries.h>
#include <irssi/src/core/nicklist.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/fe-common/core/window-items.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/gui-printtext.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-ansi/module-formats.h>
#include <irssi/src/fe-common/core/themes.h>
#include <irssi/src/fe-ansi/gui-printtext.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <stdarg.h>
#include <stdlib.h>

/* Minimum width for main window content - if terminal is smaller,
 * sidepanels will be hidden to prevent UI freeze */
#define MIN_MAIN_WINDOW_WIDTH 20

/* The panels belong to the screen, not to a main window: the left one is
 * one strip of columns at the left of the whole screen, the right one one
 * at the right, and every main window (stacked or side by side) is laid
 * out between them. Their columns are reserved once, with
 * mainwindows_reserve_columns(), when a panel is created, and given back
 * when it goes: ctx->left_reserved/right_reserved are those columns, never
 * anything else. Before, every main window had its own panels and
 * reserved the columns again (wrong columns, panels over each other, and
 * a main window whose last column came before its first one hung erssi). */

/* External functions we need */
extern void sp_logf(const char *fmt, ...);
extern void clear_window_full(TERM_WINDOW *tw, int width, int height);
extern void sp_cache_clear(SP_PANEL_CACHE *cache);
/* Settings are accessed through functions from sidepanels.h */

void apply_reservations_all(void)
{
	GSList *tmp;
	for (tmp = mainwindows; tmp != NULL; tmp = tmp->next) {
		MAIN_WINDOW_REC *mw = tmp->data;
		/* reset previous reservations if any by setting negative, then apply new */
		if (mw->statusbar_columns_left)
			mainwindow_set_statusbar_columns(mw, -mw->statusbar_columns_left, 0);
		if (mw->statusbar_columns_right)
			mainwindow_set_statusbar_columns(mw, 0, -mw->statusbar_columns_right);
	}
}

void sig_mainwindow_created(MAIN_WINDOW_REC *mw)
{
	/* the panels are laid out for the screen in sp_layout_panels() */
	(void) mw;
}

/* The rows the panels cover: from the first text row of the highest main
 * window to the last text row of the lowest one. FALSE when there are
 * none (no main window, or a screen too low for any text). */
static gboolean panel_rows(int *y, int *h)
{
	GSList *tmp;
	int top = -1, bottom = -1;

	for (tmp = mainwindows; tmp != NULL; tmp = tmp->next) {
		MAIN_WINDOW_REC *mw = tmp->data;
		int first = mw->first_line + mw->statusbar_lines_top;
		int last = mw->last_line - mw->statusbar_lines_bottom;

		if (top < 0 || first < top)
			top = first;
		if (last > bottom)
			bottom = last;
	}
	if (top < 0 || bottom < top)
		return FALSE;
	*y = top;
	*h = bottom - top + 1;
	return TRUE;
}

static int band_rows_cmp(gconstpointer a, gconstpointer b)
{
	const SP_BAND_ROWS *ra = a, *rb = b;

	return ra->y < rb->y ? -1 : ra->y > rb->y;
}

/* The bands of the right panel, top down: one for every row of main
 * windows (those with the same first and last line; stacked ones are rows
 * of their own), over the text rows of that row. Each shows the nicks of
 * one main window of its row: the active main window if it is there,
 * else the one next to the panel (the largest last column). A GArray of
 * SP_BAND_ROWS, for g_array_free(). */
GArray *sp_right_band_rows(void)
{
	GArray *bands = g_array_new(FALSE, FALSE, sizeof(SP_BAND_ROWS));
	GSList *tmp;
	guint i;

	for (tmp = mainwindows; tmp != NULL; tmp = tmp->next) {
		MAIN_WINDOW_REC *mw = tmp->data;
		int first = mw->first_line + mw->statusbar_lines_top;
		int last = mw->last_line - mw->statusbar_lines_bottom;
		SP_BAND_ROWS *band = NULL;

		for (i = 0; i < bands->len && band == NULL; i++) {
			SP_BAND_ROWS *b = &g_array_index(bands, SP_BAND_ROWS, i);
			if (b->first_line == mw->first_line && b->last_line == mw->last_line)
				band = b;
		}
		if (band == NULL) {
			SP_BAND_ROWS rec;

			rec.first_line = mw->first_line;
			rec.last_line = mw->last_line;
			rec.y = first;
			rec.bottom = last;
			rec.mw = mw;
			g_array_append_val(bands, rec);
			continue;
		}
		/* side by side: the text rows of all of them */
		band->y = MIN(band->y, first);
		band->bottom = MAX(band->bottom, last);
		if (band->mw != active_mainwin &&
		    (mw == active_mainwin || mw->last_column > band->mw->last_column))
			band->mw = mw;
	}
	/* a row too low for any text has no band */
	for (i = bands->len; i > 0; i--) {
		SP_BAND_ROWS *b = &g_array_index(bands, SP_BAND_ROWS, i - 1);
		if (b->bottom < b->y)
			g_array_remove_index(bands, i - 1);
	}
	g_array_sort(bands, band_rows_cmp);
	return bands;
}

/* TRUE when a band of the right panel would show a channel's nicks */
static gboolean right_bands_have_channel(void)
{
	GArray *bands = sp_right_band_rows();
	gboolean found = FALSE;
	guint i;

	for (i = 0; i < bands->len && !found; i++) {
		WINDOW_REC *win = g_array_index(bands, SP_BAND_ROWS, i).mw->active;
		found = win != NULL && win->active != NULL && IS_CHANNEL(win->active);
	}
	g_array_free(bands, TRUE);
	return found;
}

/* Remove a panel and give back exactly the columns it reserved. */
static void destroy_left_panel(SP_MAINWIN_CTX *ctx)
{
	clear_window_full(ctx->left_tw, ctx->left_reserved, ctx->left_h);
	term_window_destroy_left_panel(ctx->left_tw);
	ctx->left_tw = NULL;
	ctx->left_h = 0;
	mainwindows_reserve_columns(-ctx->left_reserved, 0);
	ctx->left_reserved = 0;
	if (ctx->left_cache)
		sp_cache_clear(ctx->left_cache);
}

static void destroy_right_panel(SP_MAINWIN_CTX *ctx)
{
	clear_window_full(ctx->right_tw, ctx->right_reserved, ctx->right_h);
	term_window_destroy_right_panel(ctx->right_tw);
	ctx->right_tw = NULL;
	ctx->right_h = 0;
	mainwindows_reserve_columns(0, -ctx->right_reserved);
	ctx->right_reserved = 0;
	/* when the panel comes back the cache must not claim its old lines */
	sp_right_caches_clear(ctx);
}

static void create_left_panel(SP_MAINWIN_CTX *ctx, int width)
{
	mainwindows_reserve_columns(width, 0);
	ctx->left_reserved = width;
	/* created at row 0 with the full terminal height: sp_move_panels()
	   puts it where it belongs before anything is drawn in it */
	ctx->left_tw = term_window_create_left_panel(width);
	ctx->left_h = 0;
	if (ctx->left_cache)
		sp_cache_clear(ctx->left_cache);
}

static void create_right_panel(SP_MAINWIN_CTX *ctx, int width)
{
	mainwindows_reserve_columns(0, width);
	ctx->right_reserved = width;
	ctx->right_tw = term_window_create_right_panel(width);
	ctx->right_h = 0;
	sp_right_caches_clear(ctx);
}

/* Put the existing panels over the rows of the main windows. Creates,
 * destroys and reserves nothing: safe from "mainwindow resized". A moved
 * panel has none of its cached lines at the new rows - forget them, or
 * the old rows stay on screen shifted. */
void sp_move_panels(void)
{
	SP_MAINWIN_CTX *ctx = sp_get_ctx(FALSE);
	int x, y, h;

	if (ctx == NULL || !panel_rows(&y, &h))
		return;

	if (ctx->left_tw != NULL) {
		x = 0;
		if (ctx->left_x != x || ctx->left_y != y || ctx->left_h != h)
			sp_cache_clear(ctx->left_cache);
		term_window_move(ctx->left_tw, x, y, ctx->left_reserved, h);
		ctx->left_x = x;
		ctx->left_y = y;
		ctx->left_h = h;
	}
	if (ctx->right_tw != NULL) {
		x = MAX(0, term_width - ctx->right_reserved);
		if (ctx->right_x != x || ctx->right_y != y || ctx->right_h != h)
			sp_right_caches_clear(ctx);
		term_window_move(ctx->right_tw, x, y, ctx->right_reserved, h);
		ctx->right_x = x;
		ctx->right_y = y;
		ctx->right_h = h;
	}
}

/* Collapsing gives the panels' columns back after the main windows were
   laid out without them: lay them out again, once the current resize is
   over (calling it from here would recurse through "terminal resized"). */
static guint relayout_tag = 0;

static gboolean relayout_after_collapse(gpointer data)
{
	(void) data;
	relayout_tag = 0;
	mainwindows_resize(term_width, term_height);
	return FALSE;
}

/* sp_layout_panels() is running; a call from inside it (through the
   signals it emits) is remembered and done once it is over */
static gboolean layout_busy = FALSE;
static gboolean layout_pending = FALSE;

/* at most this many passes per call: a pass asked for from inside one is
   done after it, but a pass never makes an endless chain of them */
#define LAYOUT_MAX_PASSES 3

gboolean sp_layout_busy(void)
{
	return layout_busy;
}

/* Which panels the screen has room for, and their columns: a panel is
 * created (its columns reserved) or destroyed (given back) here only, so
 * the columns are reserved once whatever the number of main windows. The
 * main area keeps at least MIN_MAIN_WINDOW_WIDTH columns and room for the
 * widest row of main windows; the right panel goes first. */
static void layout_panels_once(SP_MAINWIN_CTX *ctx)
{
	int left_w, right_w, base, need, y, h;
	gboolean rows, want_left, want_right, show_left, show_right, changed, destroyed;
	GSList *tmp;

	left_w = get_sp_enable_left() ? MAX(0, get_sp_left_width()) : 0;
	right_w = get_sp_enable_right() ? MAX(0, get_sp_right_width()) : 0;
	rows = panel_rows(&y, &h);

	/* the main area without the panels' own columns */
	base = term_width - (screen_reserved_left - ctx->left_reserved) -
	       (screen_reserved_right - ctx->right_reserved);
	need = MAX(MIN_MAIN_WINDOW_WIDTH, mainwindows_min_columns());

	want_left = rows && left_w > 0;
	show_left = want_left && base - left_w >= need;

	/* the right panel shows the nicks of a channel in every row of main
	   windows: auto-hidden, it goes only when no row has a channel */
	want_right = rows && right_w > 0;
	if (want_right && get_sp_auto_hide_right())
		want_right = right_bands_have_channel();
	show_right = want_right && base - (show_left ? left_w : 0) - right_w >= need;

	/* a panel that goes or has a new width gives its columns back first */
	changed = destroyed = FALSE;
	if (ctx->right_tw != NULL && (!show_right || ctx->right_reserved != right_w)) {
		destroy_right_panel(ctx);
		changed = destroyed = TRUE;
	}
	if (ctx->left_tw != NULL && (!show_left || ctx->left_reserved != left_w)) {
		destroy_left_panel(ctx);
		changed = destroyed = TRUE;
	}
	if (show_left && ctx->left_tw == NULL) {
		create_left_panel(ctx, left_w);
		changed = TRUE;
	}
	if (show_right && ctx->right_tw == NULL) {
		create_right_panel(ctx, right_w);
		changed = TRUE;
	}
	ctx->left_w = ctx->left_reserved;
	ctx->right_w = ctx->right_reserved;

	sp_move_panels();

	if (changed) {
		resize_debug_log("SP_LAYOUT", "panels: left=%d right=%d, reserved left=%d right=%d",
		                 ctx->left_reserved, ctx->right_reserved,
		                 screen_reserved_left, screen_reserved_right);
		/* the main windows have new columns: clear what the panels left
		   behind, and every statusbar and entry follows its window */
		if (destroyed)
			mainwindows_recreate();
		mainwindows_redraw();
		/* the panels themselves are drawn once, below, not once for each
		   main window (sig_mainwindow_resized skips them while busy) */
		for (tmp = mainwindows; tmp != NULL; tmp = tmp->next)
			signal_emit("mainwindow resized", 1, tmp->data);
		sp_cache_clear(ctx->left_cache);
		sp_right_caches_clear(ctx);
		if (ctx->left_tw && ctx->left_h > 0)
			draw_left_contents(ctx);
		if (ctx->right_tw && ctx->right_h > 0)
			draw_right_contents(ctx);
		draw_panel_borders();
		irssi_set_dirty();
	}
	/* a panel went for lack of room: the screen is (or was) too narrow */
	if (destroyed && ((want_left && !show_left) || (want_right && !show_right)) &&
	    relayout_tag == 0)
		relayout_tag = g_idle_add(relayout_after_collapse, NULL);
}

void sp_layout_panels(void)
{
	SP_MAINWIN_CTX *ctx;
	int pass;

	if (layout_busy) {
		layout_pending = TRUE;
		return;
	}
	/* no panels before "irssi init finished" (sig_irssi_init_finished
	   creates the state): the saved layout is restored without them */
	ctx = sp_get_ctx(FALSE);
	if (ctx == NULL || mainwindows == NULL)
		return;

	layout_busy = TRUE;
	for (pass = 0; pass < LAYOUT_MAX_PASSES; pass++) {
		layout_pending = FALSE;
		layout_panels_once(ctx);
		if (!layout_pending)
			break;
	}
	layout_pending = FALSE;
	layout_busy = FALSE;
}

void update_left_selection_to_active(void)
{
	SP_MAINWIN_CTX *ctx = sp_get_ctx(FALSE);

	/* Simple: selection index = active window refnum - 1 (0-based indexing) */
	if (ctx != NULL && active_win != NULL)
		ctx->left_selected_index = active_win->refnum - 1;
}

void apply_and_redraw(void)
{
	sp_layout_panels();
	redraw_all();
}

/* The window numbers follow the window list (Alt+N is the Nth window in
 * the left panel, and the panel's selection is the active window's
 * number). Not before "irssi init finished": the saved layout is restored
 * then, and it names its windows by their saved numbers - each window it
 * creates renumbered the others, so the numbers it went on with named
 * other windows, and windows came back in other main windows. They are
 * renumbered once when the start is over (sig_irssi_init_finished). */
static gboolean renumber_started = FALSE;

void renumber_windows_start(void)
{
	renumber_started = TRUE;
}

void renumber_windows_by_position(void)
{
	GSList *sort_list, *s;
	int position = 1;

	if (!renumber_started)
		return;

	/* Get sorted list using shared function */
	sort_list = build_sorted_window_list();

	/* Renumber all windows according to sorted order */
	for (s = sort_list; s; s = s->next) {
		WINDOW_SORT_REC *sort_rec = s->data;
		WINDOW_REC *win = sort_rec->win;

		if (win->refnum != position) {
			window_set_refnum(win, position);
		}
		position++;
	}

	/* Clean up */
	free_sorted_window_list(sort_list);
}

/* The borders between the panels and the main windows, along the panels;
 * with the left panel, also the separator before every main window that
 * is not at the left edge (side by side). */
void draw_panel_borders(void)
{
	SP_MAINWIN_CTX *ctx = sp_get_ctx(FALSE);
	GSList *tmp;
	int y;

	if (ctx == NULL)
		return;

	if (ctx->left_tw && ctx->left_h > 0) {
		for (y = 0; y < ctx->left_h; y++)
			gui_printtext_window_border(ctx->left_x + ctx->left_reserved - 1,
			                            ctx->left_y + y);
		for (tmp = mainwindows; tmp != NULL; tmp = tmp->next) {
			MAIN_WINDOW_REC *mw = tmp->data;
			int border_x = mw->first_column + mw->statusbar_columns_left - 1;

			if (border_x < screen_reserved_left)
				continue;
			for (y = 0; y < mw->height - mw->statusbar_lines; y++)
				gui_printtext_window_border(border_x,
				                            mw->first_line + mw->statusbar_lines_top + y);
		}
	}

	if (ctx->right_tw && ctx->right_h > 0) {
		for (y = 0; y < ctx->right_h; y++)
			gui_printtext_window_border(ctx->right_x, ctx->right_y + y);
	}
}

void sidepanels_layout_init(void)
{
	/* Nothing to initialize */
}

void sidepanels_layout_deinit(void)
{
	if (relayout_tag != 0) {
		g_source_remove(relayout_tag);
		relayout_tag = 0;
	}
}
