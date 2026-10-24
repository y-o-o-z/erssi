/*
 decode.c : fuzz the image decoder of the image preview

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A click on an image URL that someone else posted downloads the file and
 decodes it in the erssi process (image-decode.c, the vendored stb_image
 with the preview's formats and limits). The input is the downloaded file.
*/

#include <irssi/src/fe-fuzz/fuzz-target.h>
#include <irssi/src/image-preview/image-decode.h>

#include <stdlib.h>

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void) argc;
	(void) argv;
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	int width, height, channels;
	const char *why;
	unsigned char *pixels;
	volatile unsigned char sum = 0;
	size_t i, n;

	pixels = image_decode_memory(data, size, &width, &height, &channels, &why);
	if (pixels == NULL)
		return 0;
	/* the preview reads every pixel: so does the target, so that a
	 * buffer shorter than width * height * 4 shows up */
	n = (size_t) width * height * 4;
	for (i = 0; i < n; i++)
		sum ^= pixels[i];
	(void) sum;
	image_decode_free(pixels);
	return 0;
}
