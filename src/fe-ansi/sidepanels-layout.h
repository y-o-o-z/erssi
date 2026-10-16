/*
 * sidepanels-layout.h : erssi
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

#ifndef IRSSI_FE_TEXT_SIDEPANELS_LAYOUT_H
#define IRSSI_FE_TEXT_SIDEPANELS_LAYOUT_H

#include <glib.h>
#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/mainwindows.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include "sidepanels-types.h"

/* The panels' state: one for the whole screen (created by the first call
   with create = TRUE). */
SP_MAINWIN_CTX *sp_get_ctx(gboolean create);

/* Panel positioning and geometry: the panels the screen has room for,
   their columns reserved once (sp_layout_panels), or only moved over the
   rows of the main windows (sp_move_panels, safe in "mainwindow resized") */
void sp_layout_panels(void);
gboolean sp_layout_busy(void);
void sp_move_panels(void);
/* the rows of the right panel's bands, one per row of main windows */
GArray *sp_right_band_rows(void);
void apply_reservations_all(void);
void apply_and_redraw(void);

/* Selection management */
void update_left_selection_to_active(void);

/* Window management */
void renumber_windows_by_position(void);

/* Border drawing between the panels and the main windows */
void draw_panel_borders(void);

/* Signal handlers are now in sidepanels-core.c */

/* Initialization */
void sidepanels_layout_init(void);
void sidepanels_layout_deinit(void);

#endif
