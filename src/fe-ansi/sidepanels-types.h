/*
 * sidepanels-types.h : erssi
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

#ifndef IRSSI_FE_TEXT_SIDEPANELS_TYPES_H
#define IRSSI_FE_TEXT_SIDEPANELS_TYPES_H

#include <glib.h>
#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/fe-ansi/mainwindows.h>

/* Forward declaration for cache structures */
typedef struct _SP_PANEL_CACHE SP_PANEL_CACHE;

/* The rows of one band of the right panel (sp_right_band_rows): a row of
   main windows - those with the same first and last line - and the main
   window of it whose channel's nicks the band shows. */
typedef struct {
	int first_line, last_line; /* the row's main windows */
	int y, bottom;             /* the screen rows of their text */
	MAIN_WINDOW_REC *mw;
} SP_BAND_ROWS;

/* One band of the right panel: the nicks of one row of main windows'
   channel, over the text rows of that row (stacked rows have a band
   each). Its state is its own: scrolled, clicked and cached apart from
   the others. */
typedef struct {
	int y, h;               /* its rows, from the panel's top row */
	WINDOW_REC *window;     /* the window whose channel it shows */
	WI_ITEM_REC *item;      /* that window's item, when it was drawn */
	int scroll_offset;
	GSList *order;          /* nick pointers in the order drawn */
	SP_PANEL_CACHE *cache;  /* its lines, row 0 = its first row */
} SP_RIGHT_BAND;

/* SP_MAINWIN_CTX structure definition - shared across all modules. There is
   one, for the whole screen: the panels are not per main window. */
typedef struct {
	TERM_WINDOW *left_tw;
	TERM_WINDOW *right_tw;
	int left_w;
	int right_w;
	/* the columns the panel reserved (mainwindows_reserve_columns), 0
	   without the panel: given back exactly when it goes */
	int left_reserved;
	int right_reserved;
	/* selection and scroll state */
	int left_selected_index;
	int left_scroll_offset;
	int left_total_items;      /* Total items in left panel (for scroll limits) */
	/* cached geometry for hit-test and drawing */
	int left_x;
	int left_y;
	int left_h;
	int right_x;
	int right_y;
	int right_h;
	/* differential rendering cache */
	SP_PANEL_CACHE *left_cache;
	/* the right panel's bands (SP_RIGHT_BAND), top down, as last drawn */
	GPtrArray *right_bands;
	/* the rows between the bands (statusbars of the rows above) are to
	   be cleared: the panel moved, or a band has other rows */
	gboolean right_gaps_dirty;
} SP_MAINWIN_CTX;

/* Window Priority State - Simpler approach */
typedef struct {
	WINDOW_REC *window;
	int current_priority; /* 0=none, 1=events, 2=highlight, 3=activity, 4=nick/query */
} window_priority_state;

/* Window sorting structure for activity-based ordering */
typedef struct {
	WINDOW_REC *win;
	int sort_group; /* 0=Notices, 1=server, 2=channel, 3=query, 4=named_orphan, 5=unnamed_orphan */
	char *sort_key; /* For alphabetical sorting within group */
	SERVER_REC *server; /* Server for grouping */
} WINDOW_SORT_REC;

/*
 * Differential Rendering Cache
 *
 * Instead of clearing and redrawing entire panels on every update,
 * we cache the previous state and only redraw lines that changed.
 * This eliminates flicker and reduces CPU usage significantly.
 *
 * Inspired by WeeChat's approach and modern TUI frameworks.
 */

/* Maximum lines we can cache per panel */
#define SP_CACHE_MAX_LINES 256

/* Single cached line for differential rendering */
typedef struct {
	char *text;      /* Displayed text (without color codes) */
	char *prefix;    /* Prefix for right panel (nick status: @, +, etc.) */
	int format;      /* Theme format ID (TXT_SIDEPANEL_*) */
	int refnum;      /* Window refnum (left) or nick pointer hash (right) */
	gboolean valid;  /* Is this cache entry valid? */
} SP_LINE_CACHE;

/* Panel render cache */
struct _SP_PANEL_CACHE {
	SP_LINE_CACHE lines[SP_CACHE_MAX_LINES];
	int count;           /* Number of valid cached lines */
	int scroll_offset;   /* Cached scroll offset */
	int panel_height;    /* Cached panel height */
	int panel_width;     /* Cached panel width */
	gboolean initialized;
	/* every row was cleared for an empty band of this height and width
	 * (no channel): nothing to draw until that changes */
	gboolean blank;
	/* scroll markers drawn last time, so rows whose marker went away are
	 * redrawn (the row text itself did not change) */
	gboolean marker_above;
	gboolean marker_below;
	int marker_below_row;
};

#endif
