/*
 test-image-decode.c : the size limits of the preview's decoder

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A small PNG can declare an image whose decoding takes hundreds of
 megabytes on the main loop: such headers are refused before anything is
 decoded, and a 16-bit one, which stb_image holds twice as wide while it
 decodes, at half the size.
*/

#include <glib.h>
#include <string.h>

#include <irssi/src/image-preview/image-decode.h>

#define TOO_LARGE "unknown format or too large"

/* the signature, an IHDR of a width x height RGBA PNG of depth bits and
   an IDAT of junk: enough for the header check (stb_image reads up to
   the first IDAT), and the decoding fails */
static GByteArray *png_header(guint32 width, guint32 height, guint8 depth)
{
	static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	static const guint8 idat[] = { 0, 0, 0, 2, 'I', 'D', 'A', 'T', 0xff, 0xff, 0, 0, 0, 0 };
	GByteArray *b = g_byte_array_new();
	guint8 ihdr[8 + 13 + 4] = { 0, 0, 0, 13, 'I', 'H', 'D', 'R' };

	ihdr[8] = width >> 24; ihdr[9] = width >> 16; ihdr[10] = width >> 8; ihdr[11] = width;
	ihdr[12] = height >> 24; ihdr[13] = height >> 16; ihdr[14] = height >> 8; ihdr[15] = height;
	ihdr[16] = depth;
	ihdr[17] = 6;	/* RGBA */
	g_byte_array_append(b, sig, sizeof(sig));
	g_byte_array_append(b, ihdr, sizeof(ihdr));
	g_byte_array_append(b, idat, sizeof(idat));
	return b;
}

/* what the decoder says of that header: TOO_LARGE, or why else it failed */
static const char *verdict(guint32 width, guint32 height, guint8 depth)
{
	GByteArray *b = png_header(width, height, depth);
	const char *why = NULL;
	int w, h, ch;
	unsigned char *px = image_decode_memory(b->data, b->len, &w, &h, &ch, &why);

	g_assert_null(px);
	g_byte_array_unref(b);
	return why;
}

static void test_limits(void)
{
	/* within the limit: past the header check (the missing data fails) */
	g_assert_cmpstr(verdict(4096, 4096, 8), !=, TOO_LARGE);
	g_assert_cmpstr(verdict(2896, 2896, 16), !=, TOO_LARGE);
	/* past it */
	g_assert_cmpstr(verdict(4096, 4097, 8), ==, TOO_LARGE);
	g_assert_cmpstr(verdict(8192, 4882, 8), ==, TOO_LARGE);
	g_assert_cmpstr(verdict(4096, 4096, 16), ==, TOO_LARGE);
	g_assert_cmpstr(verdict(2897, 2897, 16), ==, TOO_LARGE);
	/* a side past 8192, whatever the area */
	g_assert_cmpstr(verdict(8193, 16, 8), ==, TOO_LARGE);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/image-decode/limits", test_limits);
	return g_test_run();
}
