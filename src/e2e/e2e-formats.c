/*
 e2e-formats.c : the messages of the e2e module

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 Every message of the module is "[E2E] <text>", as rpe2e.pl prints them;
 the text is an argument, so a "%" in a channel name, a nick or a path is
 shown as it is, not read as a theme code.
*/

#include "module.h"
#include "e2e-formats.h"

FORMAT_REC e2e_formats[] = {
	{ MODULE_NAME, "E2E", 0 },

	{ "e2e_message", "[E2E] $0", 1, { FORMAT_STRING } },
	{ "e2e_debug", "[E2E debug] $0", 1, { FORMAT_STRING } },

	{ NULL, NULL, 0 }
};
