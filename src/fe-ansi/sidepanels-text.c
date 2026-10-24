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

void sidepanel_text_walk(const char *text, int type, SIDEPANEL_TEXT_FUNC func, void *data)
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

		if (type == TERM_TYPE_UTF8) {
			chr = textbuffer_read_unichar(ptr, &next_ptr, &char_width);
		} else if (type == TERM_TYPE_BIG5 && is_big5(ptr[0], ptr[1])) {
			chr = ptr[0] << 8 | ptr[1];
			char_width = 2;
			next_ptr = ptr + 2;
		} else {
			/* an 8-bit terminal shows the bytes as they are, like
			 * the text view does: decoding them as UTF-8 would turn
			 * a Latin-1 name into U+FFFD and hand the terminal code
			 * points it cannot show */
			chr = *ptr;
			char_width = 1;
			next_ptr = ptr + 1;
		}
		/* in the 8-bit modes the 0x80..0x9F bytes fail this too: C1 */
		if (unichar_isprint(chr) && !func(chr, char_width, color, fg24, bg24, data))
			break;
		ptr = next_ptr;
	}
}
