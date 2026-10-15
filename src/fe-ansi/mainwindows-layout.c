/*
 mainwindows-layout.c : irssi

    Copyright (C) 2001 Timo Sirainen

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

#include "module.h"
#include <errno.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/lib-config/iconfig.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/levels.h>

#include <irssi/src/fe-ansi/mainwindows.h>
#include <irssi/src/fe-ansi/gui-windows.h>
#include <irssi/src/fe-ansi/textbuffer-view.h>

/* larger than any terminal (its size is 16 bits) */
#define LAYOUT_MAX_VALUE 65535

static void sig_layout_window_save(WINDOW_REC *window, CONFIG_NODE *node)
{
	WINDOW_REC *active;
	GUI_WINDOW_REC *gui;

	gui = WINDOW_GUI(window);
	if (gui->sticky) {
		iconfig_node_set_bool(node, "sticky", TRUE);
		active = gui->parent->active;
		if (window != active)
			iconfig_node_set_int(node, "parent", active->refnum);
	}

	if (gui->view->hidden_level != settings_get_level("window_default_hidelevel")) {
		char *level = bits2level(gui->view->hidden_level);
		iconfig_node_set_str(node, "hidelevel", level);
		g_free(level);
	} else {
		iconfig_node_set_str(node, "hidelevel", NULL);
	}

	if (gui->use_scroll)
		iconfig_node_set_bool(node, "scroll", gui->scroll);
}

static void sig_layout_window_restore(WINDOW_REC *window, CONFIG_NODE *node)
{
	WINDOW_REC *parent;
	GUI_WINDOW_REC *gui;
	const char *default_hidelevel = settings_get_str("window_default_hidelevel");

	gui = WINDOW_GUI(window);

	parent = window_find_refnum(config_node_get_int(node, "parent", -1));
	if (parent != NULL)
		gui_window_reparent(window, WINDOW_MAIN(parent));

	if (config_node_get_bool(node, "sticky", FALSE))
		gui_window_set_sticky(window);

	textbuffer_view_set_hidden_level(
	    gui->view, level2bits(config_node_get_str(node, "hidelevel", default_hidelevel), NULL));

	if (config_node_get_str(node, "scroll", NULL) != NULL) {
		gui->use_scroll = TRUE;
		gui->scroll = config_node_get_bool(node, "scroll", TRUE);
		textbuffer_view_set_scroll(gui->view, gui->scroll);
	}
}

/* The columns of a layout are those of the main area: the columns the side
   panels (and anything else) reserve on the left are not part of it, so a
   layout restores the same with the panels on or off. Every number is one
   the restore accepts (layout_read): never below 0, a size never below 1,
   so erssi never saves a layout that its next start drops. */
static void main_window_save(MAIN_WINDOW_REC *window, CONFIG_NODE *node)
{
	char num[MAX_INT_STRLEN];

	ltoa(num, window->active->refnum);
	node = iconfig_node_section(node, num, NODE_TYPE_BLOCK);

	iconfig_node_set_int(node, "first_line", CLAMP(window->first_line, 0, LAYOUT_MAX_VALUE));
	iconfig_node_set_int(node, "lines", CLAMP(window->height, 1, LAYOUT_MAX_VALUE));
	iconfig_node_set_int(node, "first_column",
	                     CLAMP(window->first_column - screen_reserved_left, 0, LAYOUT_MAX_VALUE));
	iconfig_node_set_int(node, "columns", CLAMP(window->width, 1, LAYOUT_MAX_VALUE));
}

static void sig_layout_save(void)
{
	CONFIG_NODE *node;

	iconfig_set_str(NULL, "mainwindows", NULL);
	node = iconfig_node_traverse("mainwindows", TRUE);

	g_slist_foreach(mainwindows, (GFunc) main_window_save, node);
}

typedef struct {
	int refnum;
	int first_line, lines;
	int first_column, columns;
} LAYOUT_WINDOW_REC;

/* A number 0..LAYOUT_MAX_VALUE, the whole string. */
static gboolean layout_parse_num(const char *str, int *out)
{
	char *end;
	long value;

	if (str == NULL || !g_ascii_isdigit(*str))
		return FALSE;
	errno = 0;
	value = strtol(str, &end, 10);
	if (errno != 0 || *end != '\0' || value > LAYOUT_MAX_VALUE)
		return FALSE;
	*out = (int) value;
	return TRUE;
}

/* The value of KEY in the block NODE into *out, DEF when it is not there
   (DEF < 0: it must be there). Never a config getter on a node that is no
   block: a critical at start is fatal. */
static gboolean layout_get_num(CONFIG_NODE *node, const char *key, int def, int min, int *out)
{
	CONFIG_NODE *value = config_node_find(node, key);

	if (value == NULL) {
		*out = def;
		return def >= 0;
	}
	return value->type == NODE_TYPE_KEY && layout_parse_num(value->value, out) &&
	       *out >= min;
}

static int layout_window_cmp(const LAYOUT_WINDOW_REC *w1, const LAYOUT_WINDOW_REC *w2)
{
	if (w1->first_line != w2->first_line)
		return w1->first_line < w2->first_line ? -1 : 1;
	return w1->first_column < w2->first_column ? -1 : w1->first_column > w2->first_column;
}

/* The saved main windows, sorted from the top left, NULL when the block
   makes no sense (then nothing is restored: one main window): an entry
   that is no block, a key that is no window number, a key twice, a number
   out of range, main windows that overlap. A main window line ("band") is
   the windows with the same first_line; the first column may be that of
   the main area (0) or, as saved before, after a side panel. */
static GArray *layout_read(CONFIG_NODE *node)
{
	GArray *wins;
	GHashTable *seen;
	GSList *tmp;
	guint i;
	gboolean ok = TRUE;

	if (node == NULL || node->type != NODE_TYPE_BLOCK)
		return NULL;

	wins = g_array_new(FALSE, FALSE, sizeof(LAYOUT_WINDOW_REC));
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (tmp = config_node_first(node->value); ok && tmp != NULL; tmp = config_node_next(tmp)) {
		CONFIG_NODE *wnode = tmp->data;
		LAYOUT_WINDOW_REC rec;
		char *end;
		long refnum;

		if (wnode->type != NODE_TYPE_BLOCK || wnode->key == NULL ||
		    !g_ascii_isdigit(*wnode->key)) {
			ok = FALSE;
			break;
		}
		errno = 0;
		refnum = strtol(wnode->key, &end, 10);
		if (errno != 0 || *end != '\0' || refnum < 1 || refnum > G_MAXINT ||
		    g_hash_table_contains(seen, GINT_TO_POINTER((int) refnum))) {
			ok = FALSE;
			break;
		}
		rec.refnum = (int) refnum;
		g_hash_table_add(seen, GINT_TO_POINTER(rec.refnum));

		ok = layout_get_num(wnode, "first_line", -1, 0, &rec.first_line) &&
		     layout_get_num(wnode, "lines", -1, 1, &rec.lines) &&
		     layout_get_num(wnode, "first_column", 0, 0, &rec.first_column) &&
		     layout_get_num(wnode, "columns", NEW_WINDOW_WIDTH, 1, &rec.columns);
		if (ok)
			g_array_append_val(wins, rec);
	}
	g_hash_table_destroy(seen);

	if (ok && wins->len == 0)
		ok = FALSE;
	if (ok) {
		g_array_sort(wins, (GCompareFunc) layout_window_cmp);
		for (i = 1; i < wins->len; i++) {
			LAYOUT_WINDOW_REC *prev = &g_array_index(wins, LAYOUT_WINDOW_REC, i - 1);
			LAYOUT_WINDOW_REC *rec = &g_array_index(wins, LAYOUT_WINDOW_REC, i);

			if (rec->first_line == prev->first_line ?
			        rec->first_column < prev->first_column + prev->columns :
			        rec->first_line < prev->first_line + prev->lines) {
				ok = FALSE;
				break;
			}
		}
	}
	if (!ok) {
		g_array_free(wins, TRUE);
		return NULL;
	}
	return wins;
}

/* The heights of the bands (from the top, the lines of their first window)
   fitted to the terminal: as many as fit, each in proportion to its saved
   height and at least WINDOW_MIN_SIZE + 1. Returns the number of bands. */
static int layout_fit_heights(int *heights, int count, int avail_height)
{
	gint64 height;
	int i, diff, changed;

	if (avail_height <= (WINDOW_MIN_SIZE * 2) + 1) {
		/* we can fit only one window to screen -
		   give it all the height we can */
		heights[0] = MAX(avail_height, 1);
		return 1;
	}

	/* the bands that do not fit go, the lowest first */
	while (count > 1 && count * (WINDOW_MIN_SIZE + 1) > avail_height)
		count--;

	/* a band is created at least this high (a split makes lower ones, and
	   the sum of the saved heights is then no longer the one restored) */
	height = 0;
	for (i = 0; i < count; i++) {
		heights[i] = MAX(heights[i], WINDOW_MIN_SIZE + 1);
		height += heights[i];
	}
	if (height == avail_height)
		return count;

	/* try to keep the windows' size about the same in percents */
	for (i = 0; i < count; i++) {
		int size = (int) ((gint64) avail_height * heights[i] / height);
		heights[i] = MAX(size, WINDOW_MIN_SIZE + 1);
	}

	/* give/remove the last bits, a line at a time (never below the
	   minimum: the minimums all fit, see above) */
	height = 0;
	for (i = 0; i < count; i++)
		height += heights[i];
	diff = height < avail_height ? 1 : -1;
	changed = TRUE;
	while (height != avail_height && changed) {
		changed = FALSE;
		for (i = 0; i < count && height != avail_height; i++) {
			if (diff > 0 || heights[i] > WINDOW_MIN_SIZE + 1) {
				height += diff;
				heights[i] += diff;
				changed = TRUE;
			}
		}
	}
	return count;
}

static void sig_layout_restore(void)
{
	MAIN_WINDOW_REC *lower_window;
	WINDOW_REC *window, *first;
	GArray *wins;
	int *starts, *heights;
	int avail_height, avail_width, max_wins_line;
	int i, band, bands, all_bands, lower_size;

	/* not iconfig_node_traverse(): it is critical (fatal at start) when
	   "mainwindows" is no block */
	wins = layout_read(config_node_find(mainconfig->mainnode, "mainwindows"));
	if (wins == NULL)
		return;

	/* the bands from the top: index of their first window, their height */
	starts = g_new0(int, wins->len + 1);
	heights = g_new0(int, wins->len);
	bands = 0;
	for (i = 0; i < (int) wins->len; i++) {
		LAYOUT_WINDOW_REC *rec = &g_array_index(wins, LAYOUT_WINDOW_REC, i);
		LAYOUT_WINDOW_REC *prev = i == 0 ? NULL : rec - 1;

		if (prev == NULL || rec->first_line != prev->first_line) {
			starts[bands] = i;
			heights[bands] = rec->lines;
			bands++;
		}
	}
	starts[bands] = wins->len;

	/* fitted from the bottom: those that do not fit are the top ones, as
	   before (the lowest band is created first) */
	for (i = 0; i < bands / 2; i++) {
		int h = heights[i];
		heights[i] = heights[bands - 1 - i];
		heights[bands - 1 - i] = h;
	}
	avail_height = term_height - screen_reserved_top - screen_reserved_bottom;
	all_bands = bands;
	bands = layout_fit_heights(heights, bands, avail_height);

	avail_width = term_width - screen_reserved_left - screen_reserved_right;
	max_wins_line = (avail_width + 1) / (NEW_WINDOW_WIDTH + 1);
	if (max_wins_line < 1)
		max_wins_line = 1;

	/* create all the visible windows with correct size, the lowest band
	   first: each one splits the one created before */
	lower_window = NULL;
	lower_size = 0;
	first = NULL;
	for (band = 0; band < bands; band++) {
		/* heights[] is from the bottom, starts[] from the top */
		int top = all_bands - 1 - band;
		GSList *line, *tmp2;
		int *widths;
		int j, n, columns_count;

		window = NULL;
		columns_count = 0;
		n = MIN(starts[top + 1] - starts[top], max_wins_line);
		widths = g_new0(int, n);
		for (j = 0; j < n; j++) {
			LAYOUT_WINDOW_REC *rec =
			    &g_array_index(wins, LAYOUT_WINDOW_REC, starts[top] + j);
			WINDOW_REC *new_win;

			/* create a new window + mainwindow */
			signal_emit("gui window create override", 1,
			            GINT_TO_POINTER(window == NULL ? MAIN_WINDOW_TYPE_SPLIT :
			                                             MAIN_WINDOW_TYPE_RSPLIT));

			new_win = window_create(NULL, TRUE);
			window_set_refnum(new_win, rec->refnum);
			widths[j] = rec->columns;
			columns_count += rec->columns + (window == NULL ? 0 : 1);

			if (window == NULL)
				window = new_win;
			if (first == NULL)
				first = new_win;

			window_set_active(new_win);
			active_mainwin = WINDOW_MAIN(new_win);
		}

		line = g_slist_reverse(mainwindows_get_line(WINDOW_MAIN(window)));
		for (j = g_slist_length(line), tmp2 = line; tmp2 != NULL; tmp2 = tmp2->next, j--) {
			MAIN_WINDOW_REC *rec = tmp2->data;
			int width;

			if (j > n)
				continue;
			width = (int) ((gint64) widths[j - 1] * avail_width / columns_count);
			mainwindow_set_rsize(rec, MAX(NEW_WINDOW_WIDTH, width));
		}
		g_slist_free(line);
		g_free(widths);

		if (lower_size > 0)
			mainwindow_set_size(lower_window, lower_size, FALSE);

		lower_window = WINDOW_MAIN(window);
		lower_size = MAX(heights[band], WINDOW_MIN_SIZE + 1);
	}
	g_free(starts);
	g_free(heights);
	g_array_free(wins, TRUE);

	if (lower_size > 0)
		mainwindow_set_size(lower_window, lower_size, FALSE);

	if (first != NULL) {
		window_set_active(first);
		active_mainwin = WINDOW_MAIN(first);
	}
}

static void sig_layout_reset(void)
{
	iconfig_set_str(NULL, "mainwindows", NULL);
}

void mainwindows_layout_init(void)
{
	signal_add("layout save window", (SIGNAL_FUNC) sig_layout_window_save);
	signal_add("layout restore window", (SIGNAL_FUNC) sig_layout_window_restore);
	signal_add("layout save", (SIGNAL_FUNC) sig_layout_save);
	signal_add_first("layout restore", (SIGNAL_FUNC) sig_layout_restore);
	signal_add("layout reset", (SIGNAL_FUNC) sig_layout_reset);
}

void mainwindows_layout_deinit(void)
{
	signal_remove("layout save window", (SIGNAL_FUNC) sig_layout_window_save);
	signal_remove("layout restore window", (SIGNAL_FUNC) sig_layout_window_restore);
	signal_remove("layout save", (SIGNAL_FUNC) sig_layout_save);
	signal_remove("layout restore", (SIGNAL_FUNC) sig_layout_restore);
	signal_remove("layout reset", (SIGNAL_FUNC) sig_layout_reset);
}
