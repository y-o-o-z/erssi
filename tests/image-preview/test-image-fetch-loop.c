/*
 * A download must not hold up the main loop: input and the screen are
 * handled by the same loop. image-preview-fetch.c (compiled into this test,
 * allowed to connect to 127.0.0.1) downloads from a local server that sends
 * a little data every 20 ms, and a 10 ms timer counts how often the loop
 * gets to run meanwhile. curl used to be polled from a 50 ms timer that
 * waited inside curl_multi_poll() for up to 10 rounds while data kept
 * coming: the loop then ran about a quarter of the time.
 */
#include <irssi/src/common.h>
/* the test server is on this host */
#define IMAGE_FETCH_ADDRESS_ALLOWED(sa) (address_is_public(sa) || TRUE)
#include <irssi/src/image-preview/image-preview-fetch.c>

#include <arpa/inet.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

void image_preview_debug_print(const char *fmt, ...)
{
}

IMAGE_PREVIEW_REC *image_preview_get_url(LINE_REC *line, const char *url)
{
	return NULL;
}

void image_preview_show_error_popup(void)
{
}

ImageUrlType image_preview_classify_url(const char *url)
{
	return URL_TYPE_DIRECT_IMAGE;
}

void image_cache_enforce_limit(const char *path)
{
}

gboolean image_cache_full(void)
{
	return FALSE;
}

void printtext(void *server, const char *target, int level, const char *text, ...)
{
}

#ifdef HAVE_CAPSICUM
int capsicum_open_wrapper(const char *path, int flags, int mode)
{
	return open(path, flags, mode);
}
#endif

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

static int listen_fd;
static volatile gboolean server_stop;
/* the picture: chunks of 100 bytes, Content-Length announced */
static int server_chunks = 150, server_length = 1000000;

/* answers one request with a picture that comes 100 bytes every 20 ms */
static gpointer slow_server(gpointer data)
{
	char request[4096], chunk[100], *header;
	GString *got;
	int fd, i;

	fd = accept(listen_fd, NULL, NULL);
	if (fd < 0)
		return NULL;
	got = g_string_new(NULL);
	while (strstr(got->str, "\r\n\r\n") == NULL) {
		ssize_t n = read(fd, request, sizeof(request));

		if (n <= 0)
			break;
		g_string_append_len(got, request, n);
	}
	g_string_free(got, TRUE);

	memset(chunk, 'x', sizeof(chunk));
	memcpy(chunk, "\x89PNG\r\n\x1a\n", 8);
	header = g_strdup_printf("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
	                         "Content-Length: %d\r\n\r\n", server_length);
	if (send(fd, header, strlen(header), MSG_NOSIGNAL) < 0)
		server_stop = TRUE;
	g_free(header);
	/* 150 chunks: 3 seconds, longer than the test looks */
	for (i = 0; i < server_chunks && !server_stop; i++) {
		if (send(fd, chunk, sizeof(chunk), MSG_NOSIGNAL) < 0)
			break;
		g_usleep(20 * 1000);
	}
	close(fd);
	return NULL;
}

static int ticks;
static gint64 last_tick, max_gap;

static gboolean tick(gpointer data)
{
	gint64 now = g_get_monotonic_time();

	if (last_tick != 0 && now - last_tick > max_gap)
		max_gap = now - last_tick;
	last_tick = now;
	ticks++;
	return TRUE;
}

static gboolean quit_loop(gpointer loop)
{
	g_main_loop_quit(loop);
	return FALSE;
}

static GThread *start_server(int *port)
{
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);

	server_stop = FALSE;
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	g_assert_cmpint(listen_fd, >=, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	g_assert_cmpint(bind(listen_fd, (struct sockaddr *) &sin, sizeof(sin)), ==, 0);
	g_assert_cmpint(listen(listen_fd, 1), ==, 0);
	g_assert_cmpint(getsockname(listen_fd, (struct sockaddr *) &sin, &len), ==, 0);
	*port = ntohs(sin.sin_port);
	return g_thread_new("slow-server", slow_server, NULL);
}

static void test_download_does_not_block(void)
{
	GMainLoop *loop;
	GThread *server;
	IMAGE_FETCH_REC *fetch;
	char *dir, *path, *url;
	guint tick_tag;
	int port;

	server_chunks = 150;
	server_length = 1000000;
	server = start_server(&port);
	dir = g_dir_make_tmp("image-fetch-loop-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "picture.png", NULL);
	url = g_strdup_printf("http://127.0.0.1:%d/picture.png", port);

	image_fetch_init();
	g_assert_true(image_fetch_start(url, path, NULL, NULL, FALSE));

	/* let the transfer start, then count for one second */
	loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add(300, quit_loop, loop);
	g_main_loop_run(loop);
	fetch = g_hash_table_lookup(active_fetches, url);
	g_assert_nonnull(fetch);
	g_assert_cmpint(fetch->received_bytes, >, 0);

	ticks = 0;
	last_tick = max_gap = 0;
	tick_tag = g_timeout_add(10, tick, NULL);
	g_timeout_add(1000, quit_loop, loop);
	g_main_loop_run(loop);
	g_source_remove(tick_tag);
	g_main_loop_unref(loop);

	g_test_message("main loop ran %d times in 1 s, longest gap %" G_GINT64_FORMAT " ms",
	               ticks, max_gap / 1000);
	/* the download was still running all along */
	fetch = g_hash_table_lookup(active_fetches, url);
	g_assert_nonnull(fetch);
	/* about 100 with nothing in the way; the old polling gave 20-30 */
	if (ticks < 50)
		g_error("the main loop ran only %d times in a second during a download "
			"(longest gap %" G_GINT64_FORMAT " ms)", ticks, max_gap / 1000);

	image_fetch_deinit();
	server_stop = TRUE;
	g_thread_join(server);
	close(listen_fd);

	g_unlink(path);
	g_rmdir(dir);
	g_free(url);
	g_free(path);
	g_free(dir);
}

/* driven by the main loop alone, a download ends and is complete */
static void test_download_completes(void)
{
	GMainLoop *loop;
	GThread *server;
	char *dir, *path, *url, *content;
	gsize size;
	int port, waited;

	server_chunks = 5;
	server_length = 500;
	server = start_server(&port);
	dir = g_dir_make_tmp("image-fetch-loop-XXXXXX", NULL);
	g_assert_nonnull(dir);
	path = g_build_filename(dir, "picture.png", NULL);
	url = g_strdup_printf("http://127.0.0.1:%d/picture.png", port);

	image_fetch_init();
	g_assert_true(image_fetch_start(url, path, NULL, NULL, FALSE));
	loop = g_main_loop_new(NULL, FALSE);
	for (waited = 0; waited < 50 && g_hash_table_size(active_fetches) > 0; waited++) {
		g_timeout_add(100, quit_loop, loop);
		g_main_loop_run(loop);
	}
	g_main_loop_unref(loop);
	g_assert_cmpuint(g_hash_table_size(active_fetches), ==, 0);
	g_assert_true(g_file_get_contents(path, &content, &size, NULL));
	g_assert_cmpuint(size, ==, 500);
	g_free(content);

	image_fetch_deinit();
	g_thread_join(server);
	close(listen_fd);
	g_unlink(path);
	g_rmdir(dir);
	g_free(url);
	g_free(path);
	g_free(dir);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/image-fetch/download_does_not_block", test_download_does_not_block);
	g_test_add_func("/image-fetch/download_completes", test_download_completes);
	return g_test_run();
}
