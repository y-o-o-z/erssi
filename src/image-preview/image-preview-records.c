/*
 image-preview-records.c : the preview record of each clicked line

    Copyright (C) 2024 erssi team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "module.h"
#include "image-preview.h"

#include <string.h>

/* LINE_REC * -> IMAGE_PREVIEW_REC *. A line is identified by its address
 * only, and a freed line's address is soon given to a new line, so a
 * record must not outlive its line: every way lines are freed ends here
 * (image_preview_forget_line(), image_preview_forget_buffer()), and a
 * record is only used for the URL it was made for. */
static GHashTable *image_previews = NULL;

static void image_preview_rec_free(IMAGE_PREVIEW_REC *rec)
{
	if (rec == NULL)
		return;

	/* Cancel any pending cache cleanup timer */
	if (rec->cache_cleanup_tag != 0) {
		g_source_remove(rec->cache_cleanup_tag);
		rec->cache_cleanup_tag = 0;
	}

	if (rec->rendered != NULL) {
		g_string_free(rec->rendered, TRUE);
	}

	g_free(rec->url);
	g_free(rec->cache_path);
	g_free(rec->error_message);
	g_free(rec);
}

void image_preview_records_init(void)
{
	image_previews = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                       (GDestroyNotify) image_preview_rec_free);
}

void image_preview_records_deinit(void)
{
	if (image_previews != NULL) {
		g_hash_table_destroy(image_previews);
		image_previews = NULL;
	}
}

IMAGE_PREVIEW_REC *image_preview_get(LINE_REC *line)
{
	if (image_previews == NULL || line == NULL)
		return NULL;

	return g_hash_table_lookup(image_previews, line);
}

IMAGE_PREVIEW_REC *image_preview_get_url(LINE_REC *line, const char *url)
{
	IMAGE_PREVIEW_REC *rec;

	rec = image_preview_get(line);
	if (rec == NULL || url == NULL)
		return NULL;
	if (g_strcmp0(rec->url, url) != 0) {
		/* the record of a line that was freed without us being told,
		 * found again through a new line at the same address */
		image_preview_debug_print("RECORDS: dropping stale record of %s for line %p (now %s)",
		                          rec->url, (void *) line, url);
		g_hash_table_remove(image_previews, line);
		return NULL;
	}
	return rec;
}

IMAGE_PREVIEW_REC *image_preview_rec_add(LINE_REC *line, WINDOW_REC *window,
                                         void *buffer, const char *url)
{
	IMAGE_PREVIEW_REC *rec;

	g_return_val_if_fail(image_previews != NULL, NULL);
	g_return_val_if_fail(line != NULL, NULL);

	rec = g_new0(IMAGE_PREVIEW_REC, 1);
	rec->line = line;
	rec->window = window;
	rec->buffer = buffer;
	rec->url = g_strdup(url);
	/* replaces (and frees) an older record of the same line */
	g_hash_table_insert(image_previews, line, rec);
	return rec;
}

gboolean image_preview_forget_line(LINE_REC *line)
{
	if (image_previews == NULL || line == NULL)
		return FALSE;
	return g_hash_table_remove(image_previews, line);
}

static gboolean rec_in_buffer(gpointer key, gpointer value, gpointer buffer)
{
	IMAGE_PREVIEW_REC *rec = value;

	return rec->buffer == buffer;
}

guint image_preview_forget_buffer(void *buffer)
{
	if (image_previews == NULL || buffer == NULL)
		return 0;
	return g_hash_table_foreach_remove(image_previews, rec_in_buffer, buffer);
}

guint image_preview_records_count(void)
{
	return image_previews == NULL ? 0 : g_hash_table_size(image_previews);
}

void image_preview_clear_planes(void)
{
	GHashTableIter iter;
	gpointer key, value;

	if (image_previews == NULL)
		return;

	g_hash_table_iter_init(&iter, image_previews);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		IMAGE_PREVIEW_REC *rec = value;
		if (rec->rendered != NULL) {
			g_string_free(rec->rendered, TRUE);
			rec->rendered = NULL;
		}
	}
}

void image_preview_records_remove_all(void)
{
	if (image_previews != NULL)
		g_hash_table_remove_all(image_previews);
}
