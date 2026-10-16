#ifndef IRSSI_FE_TEXT_MAINWINDOWS_H
#define IRSSI_FE_TEXT_MAINWINDOWS_H

#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/fe-ansi/term.h>

#define WINDOW_MIN_SIZE 2
#define NEW_WINDOW_WIDTH 20 /* must be >= MIN_SCREEN_WIDTH defined in term.c */

#define MAIN_WINDOW_TEXT_HEIGHT(window) ((window)->height - (window)->statusbar_lines)

#define MAIN_WINDOW_TEXT_WIDTH(window) ((window)->width - (window)->statusbar_columns - 1)

typedef struct {
	WINDOW_REC *active;

	TERM_WINDOW *screen_win;
	int sticky_windows; /* number of sticky windows */

	int first_line,
	    last_line; /* first/last line used by this window (0..x) (includes statusbars) */
	int first_column,
	    last_column;   /* first/last column used by this window (0..x) (includes statusbars) */
	int width, height; /* width/height of the window (includes statusbars) */

	GSList *statusbars;
	int statusbar_lines_top, statusbar_lines_bottom;
	int statusbar_lines; /* top+bottom */
	int statusbar_columns_left, statusbar_columns_right;
	int statusbar_columns; /* left+right */

	unsigned int dirty : 1;      /* This window needs a redraw */
	unsigned int size_dirty : 1; /* We'll need to resize the window, but haven't got around
	                                doing it just yet. */

	/* The width a row of side-by-side windows is laid out from when the
	   main area changes (mainwindows_fit_line): ref_width is this
	   window's width when the widths were last set on purpose, ref_row
	   the row they were taken for (0: none, take them again) and
	   ref_windows its number of windows; fit_width is the width the last
	   layout gave it. Laid out from these and not from the last result,
	   the panels can come and go any number of times without a column
	   lost to the rounding. */
	int ref_width, fit_width;
	unsigned int ref_row;
	int ref_windows;
} MAIN_WINDOW_REC;

typedef struct {
	char *color;
	TERM_WINDOW *window;
} MAIN_WINDOW_BORDER_REC;

extern GSList *mainwindows;
extern MAIN_WINDOW_REC *active_mainwin;
extern MAIN_WINDOW_BORDER_REC *clrtoeol_info;
extern int screen_reserved_top, screen_reserved_bottom;
extern int screen_reserved_left, screen_reserved_right;

void mainwindows_init(void);
void mainwindows_deinit(void);

MAIN_WINDOW_REC *mainwindow_create(int);
void mainwindow_destroy(MAIN_WINDOW_REC *window);

void mainwindows_redraw(void);
void mainwindows_recreate(void);

/* Change the window height - the height includes the lines needed for
   statusbars. If resize_lower is TRUE, the lower window is first tried
   to be resized instead of upper window. */
void mainwindow_set_size(MAIN_WINDOW_REC *window, int height, int resize_lower);
void mainwindow_set_rsize(MAIN_WINDOW_REC *window, int width);
void mainwindows_resize(int width, int height);

void mainwindow_change_active(MAIN_WINDOW_REC *mainwin, WINDOW_REC *skip_window);

int mainwindows_reserve_lines(int top, int bottom);
int mainwindow_set_statusbar_lines(MAIN_WINDOW_REC *window, int top, int bottom);
void mainwindows_redraw_dirty(void);

/* Reserve columns at the left/right of the screen (for the side panels):
   every row of main windows is laid out again without them. */
int mainwindows_reserve_columns(int left, int right);
/* The fewest columns the main windows need side by side in the widest row. */
int mainwindows_min_columns(void);
int mainwindow_set_statusbar_columns(MAIN_WINDOW_REC *window, int left, int right);

GSList *mainwindows_get_sorted(int reverse);
GSList *mainwindows_get_line(MAIN_WINDOW_REC *rec);

#endif
