/*
 image-decode.c : decode the images of the preview, with stb_image

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The images come from URLs that other people post, so this is the one
 place erssi decodes untrusted binary data. It is a file of its own so that
 the fuzz target (src/fe-fuzz/image-preview) runs the decoder exactly as
 the preview builds it: the same formats and the same limits.
*/

#include <irssi/src/image-preview/image-decode.h>

#include <limits.h>

/* STB image for loading - single header library */
#define STB_IMAGE_IMPLEMENTATION
/* A small file can declare a huge image: refuse it instead of allocating
 * gigabytes on the main loop */
#define STBI_MAX_DIMENSIONS 8192
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

static int too_large(int width, int height)
{
	return width <= 0 || height <= 0 ||
	       (long long) width * height > IMAGE_DECODE_MAX_PIXELS;
}

unsigned char *image_decode_file(FILE *fp, int *width, int *height, int *channels,
                                 const char **why)
{
	unsigned char *pixels;

	/* the header first: only an image of a size we accept is decoded */
	if (!stbi_info_from_file(fp, width, height, channels) || too_large(*width, *height)) {
		*why = "unknown format or too large";
		return NULL;
	}
	pixels = stbi_load_from_file(fp, width, height, channels, 4);
	if (pixels == NULL)
		*why = stbi_failure_reason();
	return pixels;
}

unsigned char *image_decode_memory(const unsigned char *data, size_t len, int *width,
                                   int *height, int *channels, const char **why)
{
	unsigned char *pixels;

	if (len == 0 || len > INT_MAX ||
	    !stbi_info_from_memory(data, (int) len, width, height, channels) ||
	    too_large(*width, *height)) {
		*why = "unknown format or too large";
		return NULL;
	}
	pixels = stbi_load_from_memory(data, (int) len, width, height, channels, 4);
	if (pixels == NULL)
		*why = stbi_failure_reason();
	return pixels;
}

void image_decode_free(unsigned char *pixels)
{
	stbi_image_free(pixels);
}
