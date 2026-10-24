/*
 * The image preview keeps a record per clicked line, found by the line's
 * address (image-preview/image-preview-records.c). Lines freed by /CLEAR
 * or with their window send no "line removed", and the next lines printed
 * get their addresses: their records must go with them, and a record left
 * behind must not be taken for a new line's (the picture of one link
 * shown for another).
 */
#include <irssi/src/common.h>
#include <irssi/src/image-preview/image-preview.h>

void image_preview_debug_print(const char *fmt, ...)
{
}

/* lines and buffers are only addresses here */
static char lines[4], buffers[2];
#define LINE(i) ((LINE_REC *) &lines[i])
#define BUFFER(i) ((void *) &buffers[i])

/* a record with a downloaded picture, as after a click */
static IMAGE_PREVIEW_REC *add_cached(int line, int buffer, const char *url, const char *path)
{
	IMAGE_PREVIEW_REC *rec;

	rec = image_preview_rec_add(LINE(line), NULL, BUFFER(buffer), url);
	g_assert_nonnull(rec);
	rec->cache_path = g_strdup(path);
	return rec;
}

/* the line at a reused address has another link: the old record (and its
 * picture) is not the new line's, and is gone */
static void test_reused_address_other_url(void)
{
	image_preview_records_init();
	add_cached(0, 0, "https://a.example/one.png", "/cache/one.png");

	g_assert_null(image_preview_get_url(LINE(0), "https://b.example/two.png"));
	g_assert_null(image_preview_get(LINE(0)));
	g_assert_cmpuint(image_preview_records_count(), ==, 0);

	/* the same link again is the same picture */
	add_cached(0, 0, "https://a.example/one.png", "/cache/one.png");
	g_assert_cmpstr(image_preview_get_url(LINE(0), "https://a.example/one.png")->cache_path,
	                ==, "/cache/one.png");
	image_preview_records_deinit();
}

/* /CLEAR and closing a window drop the records of that buffer only */
static void test_forget_buffer(void)
{
	image_preview_records_init();
	add_cached(0, 0, "https://a.example/1.png", "/cache/1.png");
	add_cached(1, 0, "https://a.example/2.png", "/cache/2.png");
	add_cached(2, 1, "https://a.example/3.png", "/cache/3.png");

	g_assert_cmpuint(image_preview_forget_buffer(BUFFER(0)), ==, 2);
	g_assert_null(image_preview_get(LINE(0)));
	g_assert_null(image_preview_get(LINE(1)));
	g_assert_nonnull(image_preview_get_url(LINE(2), "https://a.example/3.png"));
	g_assert_cmpuint(image_preview_records_count(), ==, 1);

	g_assert_true(image_preview_forget_line(LINE(2)));
	g_assert_false(image_preview_forget_line(LINE(2)));
	g_assert_cmpuint(image_preview_records_count(), ==, 0);
	image_preview_records_deinit();
}

/* a record replaces the one its line had (no leak, no second record) */
static void test_add_replaces(void)
{
	image_preview_records_init();
	add_cached(3, 0, "https://a.example/old.png", "/cache/old.png");
	add_cached(3, 0, "https://a.example/new.png", "/cache/new.png");
	g_assert_cmpuint(image_preview_records_count(), ==, 1);
	g_assert_cmpstr(image_preview_get(LINE(3))->url, ==, "https://a.example/new.png");
	image_preview_records_deinit();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/image-records/reused_address_other_url",
	                test_reused_address_other_url);
	g_test_add_func("/image-records/forget_buffer", test_forget_buffer);
	g_test_add_func("/image-records/add_replaces", test_add_replaces);

	return g_test_run();
}
