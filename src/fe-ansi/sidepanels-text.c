/*
 * sidepanels-text.c : erssi
 *
 * Copyright (C) 2024-2025 erssi-org team
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

#include <irssi/src/common.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/sidepanels-text.h>
#include <irssi/src/fe-ansi/textbuffer-unformat.h>

void sidepanel_text_walk(const char *text, SIDEPANEL_TEXT_FUNC func, void *data)
{
	const unsigned char *ptr, *next_ptr;
	unsigned int fg24, bg24;
	int color, char_width;
	unichar chr;

	color = ATTR_RESET;
	fg24 = bg24 = UINT_MAX;
	ptr = (const unsigned char *) text;
	while (*ptr != '\0') {
		if (*ptr == 4) {
			/* format code - colour change */
			ptr++;
			if (*ptr == '\0')
				break;
			textbuffer_unformat(&ptr, &color, &fg24, &bg24);
			continue;
		}

		chr = textbuffer_read_unichar(ptr, &next_ptr, &char_width);
		if (unichar_isprint(chr) && !func(chr, char_width, color, fg24, bg24, data))
			break;
		ptr = next_ptr;
	}
}
