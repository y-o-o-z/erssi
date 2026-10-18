/*
 * A failed image download is tried once more after 3 seconds
 * (image-preview/image-preview-fetch.c, compiled into this test with a
 * shorter delay). The
 * waiting retry is a GLib timer with its own data: image_fetch_deinit()
 * and cancelling remove the timer and free the data, so nothing fires
 * after the module is gone, and a retry that fired is not freed again.
 * No request is made: the failure is reported by hand.
 */
#include <irssi/src/common.h>
/* 3 seconds in erssi */
#define IMAGE_FETCH_RETRY_DELAY_MS 200
#include <irssi/src/image-preview/image-preview-fetch.c>

#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

static IMAGE_PREVIEW_REC preview;
static gboolean preview_present;
static int preview_lookups;

void image_preview_debug_print(const char *fmt, ...)
{
}

IMAGE_PREVIEW_REC *image_preview_get(LINE_REC *line)
{
	preview_lookups++;
	/* without a preview record a retry that fires starts nothing */
	return preview_present ? &preview : NULL;
}

void image_preview_show_error_popup(void)
{
}

ImageUrlType image_preview_classify_url(const char *url)
{
	return URL_TYPE_PAGE_GENERIC;
}

void image_cache_enforce_limit(const char *path)
{
}

int settings_get_int(const char *key)
{
	return 10;
}

int settings_get_time(const char *key)
{
	return 60 * 1000;
}

int signal_emit(const char *signal, int params, ...)
{
	return 0;
}

/* a page download that failed, with one retry left */
static void fail_page_fetch(const char *url)
{
	IMAGE_FETCH_REC *fetch = g_new0(IMAGE_FETCH_REC, 1);

	fetch->url = g_strdup(url);
	fetch->cache_path = g_strdup("/nonexistent/image-preview-test.img");
	fetch->stage = FETCH_STAGE_HTML;
	g_hash_table_insert(active_fetches, fetch->url, fetch);

	memset(&preview, 0, sizeof(preview));
	preview_present = TRUE;
	preview_lookups = 0;
	fetch_complete(fetch, FALSE, "simulated failure");
	g_assert_cmpint(preview.retry_count, ==, 1);
	/* from now on the line is gone */
	preview_present = FALSE;
	preview_lookups = 0;
}

static gboolean quit_loop(gpointer loop)
{
	g_main_loop_quit(loop);
	return FALSE;
}

/* runs the main loop for longer than the retry delay */
static void run_past_retry_delay(void)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	g_timeout_add(IMAGE_FETCH_RETRY_DELAY_MS * 3, quit_loop, loop);
	g_main_loop_run(loop);
	g_main_loop_unref(loop);
}

/* image_fetch_deinit() removes the waiting retry: nothing fires later */
static void test_deinit_removes_retry(void)
{
	RetryData *retry;
	guint tag;

	image_fetch_init();
	fail_page_fetch("https://example.invalid/page");
	g_assert_cmpuint(g_slist_length(pending_retries), ==, 1);
	retry = pending_retries->data;
	tag = retry->tag;
	g_assert_cmpuint(tag, !=, 0);
	g_assert_cmpstr(retry->url, ==, "https://example.invalid/page");

	image_fetch_deinit();
	g_assert_null(pending_retries);
	g_assert_null(g_main_context_find_source_by_id(NULL, tag));

	run_past_retry_delay();
	g_assert_cmpint(preview_lookups, ==, 0);
}

/* a retry that fires leaves the list, so deinit does not free it again */
static void test_fired_retry(void)
{
	image_fetch_init();
	fail_page_fetch("https://example.invalid/fired");
	g_assert_cmpuint(g_slist_length(pending_retries), ==, 1);

	run_past_retry_delay();
	g_assert_cmpint(preview_lookups, ==, 1);
	g_assert_null(pending_retries);

	image_fetch_deinit();
}

/* cancelling the downloads (/IMAGE OFF, /IMAGE CLEAR) cancels the waiting
 * retries too, and cancelling one URL only its own */
static void test_cancel(void)
{
	image_fetch_init();
	fail_page_fetch("https://example.invalid/one");
	fail_page_fetch("https://example.invalid/two");
	g_assert_cmpuint(g_slist_length(pending_retries), ==, 2);

	image_fetch_cancel("https://example.invalid/one");
	g_assert_cmpuint(g_slist_length(pending_retries), ==, 1);
	g_assert_cmpstr(((RetryData *) pending_retries->data)->url, ==,
	                "https://example.invalid/two");

	image_fetch_cancel_all();
	g_assert_null(pending_retries);

	run_past_retry_delay();
	g_assert_cmpint(preview_lookups, ==, 0);
	image_fetch_deinit();
}

/* The download is written in place under the cache's name, and opened
 * before any request: a link put there is not written through (the file
 * it points to is not emptied), a FIFO does not hang erssi (alarm() ends a
 * hanging test). Both are refused, so no request is made. */
static void test_cache_file_refused(void)
{
	char *dir = g_dir_make_tmp("image-fetch-XXXXXX", NULL);
	char *target = g_build_filename(dir, "precious", NULL);
	char *link = g_build_filename(dir, "link.png", NULL);
	char *fifo = g_build_filename(dir, "fifo.png", NULL);
	char *content;

	g_assert_true(g_file_set_contents(target, "keep", -1, NULL));
	g_assert_cmpint(symlink(target, link), ==, 0);
	g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
	image_fetch_init();
	alarm(5);
	g_assert_false(image_fetch_start("http://127.0.0.1:9/link.png", link, NULL, NULL, FALSE));
	g_assert_false(image_fetch_start("http://127.0.0.1:9/fifo.png", fifo, NULL, NULL, FALSE));
	alarm(0);
	image_fetch_deinit();
	g_assert_true(g_file_get_contents(target, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "keep");
	g_free(content);

	g_unlink(fifo);
	g_unlink(link);
	g_unlink(target);
	g_rmdir(dir);
	g_free(fifo);
	g_free(link);
	g_free(target);
	g_free(dir);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* a refused cache file is reported with g_warning(): no failure here */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	g_test_add_func("/image-fetch/deinit_removes_retry", test_deinit_removes_retry);
	g_test_add_func("/image-fetch/fired_retry", test_fired_retry);
	g_test_add_func("/image-fetch/cancel", test_cancel);
	g_test_add_func("/image-fetch/cache_file_refused", test_cache_file_refused);

	return g_test_run();
}
