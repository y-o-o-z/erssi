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
#include <gio/gio.h>
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

/* data deflated (zlib format) */
static GBytes *deflate(const guint8 *data, gsize len)
{
	GZlibCompressor *z = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 9);
	GOutputStream *mem = g_memory_output_stream_new_resizable();
	GOutputStream *out = g_converter_output_stream_new(mem, G_CONVERTER(z));
	GBytes *b;

	g_assert_true(g_output_stream_write_all(out, data, len, NULL, NULL, NULL));
	g_assert_true(g_output_stream_close(out, NULL, NULL));
	b = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(mem));
	g_object_unref(out);
	g_object_unref(mem);
	g_object_unref(z);
	return b;
}

static void chunk(GByteArray *b, const char *type, const guint8 *data, guint32 len)
{
	guint8 head[8] = { len >> 24, len >> 16, len >> 8, len, type[0], type[1], type[2], type[3] };
	static const guint8 crc[4] = { 0 };	/* stb_image does not check it */

	g_byte_array_append(b, head, 8);
	if (len > 0)
		g_byte_array_append(b, data, len);
	g_byte_array_append(b, crc, 4);
}

/* a whole RGBA PNG, 8 bits, its filtered rows raw (len bytes) */
static GByteArray *png(guint32 width, guint32 height, gboolean interlaced, const guint8 *raw,
                       gsize len)
{
	static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	guint8 ihdr[13] = { width >> 24, width >> 16, width >> 8, width,
	                    height >> 24, height >> 16, height >> 8, height, 8, 6, 0, 0, 0 };
	GByteArray *b = g_byte_array_new();
	GBytes *z = deflate(raw, len);
	gsize zlen;
	const guint8 *zd = g_bytes_get_data(z, &zlen);

	ihdr[12] = interlaced;
	g_byte_array_append(b, sig, 8);
	chunk(b, "IHDR", ihdr, 13);
	chunk(b, "IDAT", zd, zlen);
	chunk(b, "IEND", NULL, 0);
	g_bytes_unref(z);
	return b;
}

static unsigned char *decode(GByteArray *b, int *w, int *h, const char **why)
{
	int ch;

	*why = NULL;
	return image_decode_memory(b->data, b->len, w, h, &ch, why);
}

/* the inflated data of a PNG is bounded by its size, not only its
   header: a 1 x 1 image with 64 MB of zeros deflated (some 64 KB) is
   refused instead of inflated */
static void test_inflate_bomb(void)
{
	gsize big = 64 * 1024 * 1024;
	guint8 *zeros = g_malloc0(big);
	GByteArray *b = png(1, 1, FALSE, zeros, big);
	const char *why;
	int w, h;

	g_assert_cmpuint(b->len, <, 1024 * 1024);
	g_assert_null(decode(b, &w, &h, &why));
	g_assert_nonnull(why);
	g_byte_array_unref(b);
	g_free(zeros);
}

/* real images still decode: a plain one, an interlaced one (Adam7: 7
   passes, each with its own filter bytes) */
static void test_real_images(void)
{
	static const int pass[7][4] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
	                                { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
	guint8 plain[2 * (1 + 2 * 4)] = { 0, 255, 0, 0, 255, 0, 255, 0, 255,
	                                  0, 0, 0, 255, 255, 1, 2, 3, 4 };
	GByteArray *b = png(2, 2, FALSE, plain, sizeof(plain));
	unsigned char *px;
	const char *why;
	int w, h, i, n = 33;
	gsize len = 0;
	guint8 *raw;

	px = decode(b, &w, &h, &why);
	g_assert_nonnull(px);
	g_assert_cmpint(w, ==, 2);
	g_assert_cmpint(h, ==, 2);
	g_assert_cmpint(px[0], ==, 255);
	g_assert_cmpint(px[12], ==, 1);
	image_decode_free(px);
	g_byte_array_unref(b);

	for (i = 0; i < 7; i++) {
		int pw = (n - pass[i][0] + pass[i][2] - 1) / pass[i][2];
		int ph = (n - pass[i][1] + pass[i][3] - 1) / pass[i][3];

		if (pw > 0 && ph > 0)
			len += (gsize) ph * (1 + pw * 4);
	}
	raw = g_malloc0(len);
	b = png(n, n, TRUE, raw, len);
	px = decode(b, &w, &h, &why);
	if (px == NULL)
		g_error("an interlaced %dx%d PNG did not decode: %s", n, n, why);
	g_assert_cmpint(w, ==, n);
	image_decode_free(px);
	g_byte_array_unref(b);
	g_free(raw);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/image-decode/limits", test_limits);
	g_test_add_func("/image-decode/inflate-bomb", test_inflate_bomb);
	g_test_add_func("/image-decode/real-images", test_real_images);
	return g_test_run();
}
