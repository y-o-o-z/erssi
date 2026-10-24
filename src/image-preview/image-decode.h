#ifndef IRSSI_IMAGE_PREVIEW_IMAGE_DECODE_H
#define IRSSI_IMAGE_PREVIEW_IMAGE_DECODE_H

#include <stddef.h>
#include <stdio.h>

/* At most this many pixels are decoded: a small file can declare a huge
 * image */
#define IMAGE_DECODE_MAX_PIXELS (40 * 1000 * 1000)

/* Decodes a JPEG, PNG, GIF or BMP image into RGBA pixels, 4 bytes each,
 * width * height of them; NULL when the format is unknown, the image too
 * large or broken, with the reason in *why. image_decode_file() reads fp
 * from its current position. Free the pixels with image_decode_free(). */
unsigned char *image_decode_file(FILE *fp, int *width, int *height, int *channels,
                                 const char **why);
unsigned char *image_decode_memory(const unsigned char *data, size_t len, int *width,
                                   int *height, int *channels, const char **why);
void image_decode_free(unsigned char *pixels);

#endif
