/*
 * resize-debug.h : erssi
 *
 * Comprehensive resize debugging system for terminal resize analysis.
 * Logs all stages of resize handling to help diagnose terminal-specific issues.
 *
 * Copyright (C) 2024-2025 erssi-org team
 */

#ifndef RESIZE_DEBUG_H
#define RESIZE_DEBUG_H

#include <glib.h>
#include <stdio.h>

/* Open the debug log PATH for appending: a file of the user's own,
 * created readable and writable by the user only (0600, whatever the
 * umask; an existing log is made 0600 too), never through a symbolic link
 * and never anything but a regular file the user owns. NULL (errno set)
 * when it cannot be opened. For sidepanel_debug and resize_debug: their
 * logs hold channel names, nicks and window names. */
FILE *debug_log_open(const char *path);

/* Initialize resize debug logging - call once at startup; it follows
 * /SET resize_debug from then on */
void resize_debug_init(void);

/* Cleanup - call at shutdown */
void resize_debug_deinit(void);

/* Log a resize event with timestamp and details */
void resize_debug_log(const char *stage, const char *fmt, ...);

/* Log terminal dimensions change */
void resize_debug_dimensions(const char *stage, int old_cols, int old_rows,
                              int new_cols, int new_rows);

/* Log cache operation */
void resize_debug_cache(const char *operation, const char *panel, int window_id);

/* Log redraw operation */
void resize_debug_redraw(const char *type, int window_id);

/* Mark start of resize sequence (for timing) */
void resize_debug_start_sequence(void);

/* Mark end of resize sequence and log total time */
void resize_debug_end_sequence(void);

/* Check if debug logging is enabled */
gboolean resize_debug_enabled(void);

/* Force flush log to disk */
void resize_debug_flush(void);

#endif /* RESIZE_DEBUG_H */
