/*
 * A failed image download is tried once more after 3 seconds
 * (image-preview/image-preview-fetch.c, compiled into this test with a
 * shorter delay). The
 * waiting retry is a GLib timer with its own data: image_fetch_deinit()
 * and cancelling remove the timer and free the data, so nothing fires
 * after the module is gone, and a retry that fired is not freed again.
 * No request is made: the failure is reported by hand. The socket a
 * download opens is close-on-exec.
 */
#include <irssi/src/common.h>
#include <sys/wait.h>
/* 3 seconds in erssi */
#define IMAGE_FETCH_RETRY_DELAY_MS 200
#include <irssi/src/image-preview/image-preview-fetch.c>

#include <arpa/inet.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

static IMAGE_PREVIEW_REC preview;
static gboolean preview_present;
static int preview_lookups;

void image_preview_debug_print(const char *fmt, ...)
{
}

IMAGE_PREVIEW_REC *image_preview_get_url(LINE_REC *line, const char *url)
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

/* the cache's hard limit (image_cache_full() in image-preview-cache.c) */
static gboolean cache_full;
static int full_notices;

gboolean image_cache_full(void)
{
	return cache_full;
}

void printtext(void *server, const char *target, int level, const char *text, ...)
{
	if (strstr(text, "no new download") != NULL)
		full_notices++;
}

#ifdef HAVE_CAPSICUM
/* safe-file.c opens through capsicum.c on FreeBSD; the test never
   enters capability mode, so a plain open() is what it would do */
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

/* Does a shell started with every descriptor left open (as a child of a
 * fork and exec) have descriptor 'fd'; -1: /proc/self/fd does not say */
static int child_has_fd(int fd)
{
	char *script, *argv[4];
	int status;

	if (!g_file_test("/proc/self/fd", G_FILE_TEST_IS_DIR))
		return -1;
	script = g_strdup_printf("test -e /proc/self/fd/%d", fd);
	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = script;
	argv[3] = NULL;
	g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_LEAVE_DESCRIPTORS_OPEN, NULL, NULL,
	                           NULL, NULL, &status, NULL));
	g_free(script);
	g_assert_true(WIFEXITED(status));
	return WEXITSTATUS(status) == 0;
}

/* The socket a download connects with is not inherited by a program
 * started while the download runs (/EXEC, a script's child, /UPGRADE):
 * it is close-on-exec */
static void test_socket_cloexec(void)
{
	struct curl_sockaddr address;
	struct sockaddr_in *sin = (struct sockaddr_in *) &address.addr;
	curl_socket_t fd;
	int inherited;

	memset(&address, 0, sizeof(address));
	address.family = AF_INET;
	address.socktype = SOCK_STREAM;
	address.protocol = IPPROTO_TCP;
	address.addrlen = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = htons(80);
	/* a public address to the filter, never connected to here (8.8.8.8) */
	sin->sin_addr.s_addr = htonl(0x08080808);

	fd = open_public_socket(NULL, CURLSOCKTYPE_IPCXN, &address);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(fcntl(fd, F_GETFD) & FD_CLOEXEC, ==, FD_CLOEXEC);

	if (child_has_fd(fd) != -1) {
		/* a copy without close-on-exec is inherited: the check works */
		inherited = fcntl(fd, F_DUPFD, 0);
		g_assert_cmpint(inherited, >=, 0);
		g_assert_cmpint(child_has_fd(inherited), ==, 1);
		close(inherited);
		g_assert_cmpint(child_has_fd(fd), ==, 0);
	}
	close(fd);
}

/* address_is_public() decides, for a resolved address a clicked link leads
 * to, whether a socket may be opened to it: a public web server only, never
 * this host, the LAN, a transition address that embeds one, or a reserved
 * range. */
static gboolean v4_public(guint32 host_order)
{
	struct sockaddr_in s;

	memset(&s, 0, sizeof(s));
	s.sin_family = AF_INET;
	s.sin_addr.s_addr = htonl(host_order);
	return address_is_public((struct sockaddr *) &s);
}

static gboolean v6_public(const char *txt)
{
	struct sockaddr_in6 s;

	memset(&s, 0, sizeof(s));
	s.sin6_family = AF_INET6;
	g_assert_cmpint(inet_pton(AF_INET6, txt, &s.sin6_addr), ==, 1);
	return address_is_public((struct sockaddr *) &s);
}

static void test_address_filter(void)
{
	/* denied IPv4 ranges */
	g_assert_false(v4_public(0x00000001)); /* 0.0.0.1       this-network */
	g_assert_false(v4_public(0x0A000001)); /* 10.0.0.1      private */
	g_assert_false(v4_public(0x7F000001)); /* 127.0.0.1     loopback */
	g_assert_false(v4_public(0xA9FE0001)); /* 169.254.0.1   link-local */
	g_assert_false(v4_public(0xAC100001)); /* 172.16.0.1    private */
	g_assert_false(v4_public(0xC0A80001)); /* 192.168.0.1   private */
	g_assert_false(v4_public(0x64400001)); /* 100.64.0.1    CGNAT */
	g_assert_false(v4_public(0xC0000001)); /* 192.0.0.1     IETF */
	g_assert_false(v4_public(0xC0000201)); /* 192.0.2.1     TEST-NET-1 (doc) */
	g_assert_false(v4_public(0xC6336401)); /* 198.51.100.1  TEST-NET-2 (doc) */
	g_assert_false(v4_public(0xCB007101)); /* 203.0.113.1   TEST-NET-3 (doc) */
	g_assert_false(v4_public(0xC6120001)); /* 198.18.0.1    benchmarking */
	g_assert_false(v4_public(0xE0000001)); /* 224.0.0.1     multicast */
	g_assert_false(v4_public(0xFFFFFFFF)); /* 255.255.255.255 broadcast */
	/* allowed IPv4, including the edges just outside a denied block */
	g_assert_true(v4_public(0x08080808));  /* 8.8.8.8 */
	g_assert_true(v4_public(0x5DB8D822));  /* 93.184.216.34 */
	g_assert_true(v4_public(0xAC020001));  /* 172.2.0.1   below 172.16/12 */
	g_assert_true(v4_public(0x64000001));  /* 100.0.0.1   below 100.64/10 */

	/* denied IPv6 ranges */
	g_assert_false(v6_public("::1"));        /* loopback */
	g_assert_false(v6_public("::"));         /* unspecified */
	g_assert_false(v6_public("fc00::1"));    /* ULA */
	g_assert_false(v6_public("fd00::1"));    /* ULA */
	g_assert_false(v6_public("fe80::1"));    /* link-local */
	g_assert_false(v6_public("fec0::1"));    /* site-local */
	g_assert_false(v6_public("ff02::1"));    /* multicast */
	g_assert_false(v6_public("2001::1"));    /* Teredo */
	g_assert_false(v6_public("2001:db8::1")); /* documentation */
	/* IPv6 that carries an IPv4 one takes that one's verdict */
	g_assert_false(v6_public("::ffff:127.0.0.1")); /* v4-mapped loopback */
	g_assert_false(v6_public("::ffff:10.0.0.1"));  /* v4-mapped private */
	g_assert_false(v6_public("64:ff9b::7f00:1"));  /* NAT64 loopback */
	g_assert_false(v6_public("2002:7f00:1::1"));   /* 6to4 loopback */
	g_assert_false(v6_public("64:ff9b:1::1"));     /* local-use NAT64 */
	/* allowed IPv6, including 2001::/16 that is neither Teredo nor doc */
	g_assert_true(v6_public("2606:4700:4700::1111"));
	g_assert_true(v6_public("2001:4860:4860::8888"));
	g_assert_true(v6_public("::ffff:8.8.8.8"));    /* v4-mapped public */
	g_assert_true(v6_public("2002:0808:0808::1")); /* 6to4 of 8.8.8.8 */
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

/* The download is written to <cache name>.part, opened before any
 * request: a link put there is not written through (the file it points to
 * is not emptied), a FIFO does not hang erssi (alarm() ends a hanging
 * test). Both are refused, so no request is made. */
static void test_cache_file_refused(void)
{
	char *dir = g_dir_make_tmp("image-fetch-XXXXXX", NULL);
	char *target = g_build_filename(dir, "precious", NULL);
	char *link = g_build_filename(dir, "link.png", NULL);
	char *link_part = g_build_filename(dir, "link.png.part", NULL);
	char *fifo = g_build_filename(dir, "fifo.png", NULL);
	char *fifo_part = g_build_filename(dir, "fifo.png.part", NULL);
	char *content;

	g_assert_true(g_file_set_contents(target, "keep", -1, NULL));
	g_assert_cmpint(symlink(target, link_part), ==, 0);
	g_assert_cmpint(mkfifo(fifo_part, 0600), ==, 0);
	image_fetch_init();
	alarm(5);
	g_assert_false(image_fetch_start("http://127.0.0.1:9/link.png", link, NULL, NULL, FALSE));
	g_assert_false(image_fetch_start("http://127.0.0.1:9/fifo.png", fifo, NULL, NULL, FALSE));
	alarm(0);
	image_fetch_deinit();
	g_assert_true(g_file_get_contents(target, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "keep");
	g_free(content);

	g_unlink(fifo_part);
	g_unlink(link_part);
	g_unlink(target);
	g_rmdir(dir);
	g_free(fifo);
	g_free(fifo_part);
	g_free(link);
	g_free(link_part);
	g_free(target);
	g_free(dir);
}

/* the hard limit of the cache holds for a retry too: a retry that fires
 * while the cache is full starts nothing, and the preview ends failed */
static void test_retry_refused_when_full(void)
{
	image_fetch_init();
	fail_page_fetch("https://example.invalid/full");
	g_assert_cmpuint(g_slist_length(pending_retries), ==, 1);
	preview_present = TRUE;
	cache_full = TRUE;
	full_notices = 0;
	run_past_retry_delay();
	g_assert_cmpint(preview_lookups, ==, 1);
	g_assert_cmpint(full_notices, ==, 1);
	g_assert_true(preview.fetch_failed);
	g_assert_false(preview.fetch_pending);
	g_assert_cmpuint(g_hash_table_size(active_fetches), ==, 0);
	g_free(preview.error_message);
	preview.error_message = NULL;
	cache_full = FALSE;
	preview_present = FALSE;
	image_fetch_deinit();
}

/* og:image is attribute text, and may be relative to its page: the URL
 * downloaded is decoded and resolved, and stays a web URL */
static void test_og_image_url(void)
{
	static const struct {
		const char *content;
		const char *expected;
	} rows[] = {
		{ "https://i.example.org/a.png?w=1&amp;h=2", "https://i.example.org/a.png?w=1&h=2" },
		{ "https://i.example.org/a.png?w=1&#38;h=2&#x26;q=3",
		  "https://i.example.org/a.png?w=1&h=2&q=3" },
		{ "/img/b.jpg", "https://www.example.org/img/b.jpg" },
		{ "c.jpg", "https://www.example.org/gallery/c.jpg" },
		{ "//cdn.example.net/d.webp", "https://cdn.example.net/d.webp" },
		{ "  https://i.example.org/e.gif  ", "https://i.example.org/e.gif" },
		{ "http://i.example.org/f.png", "http://i.example.org/f.png" },
		{ "file:///etc/passwd", NULL },
		{ "javascript:alert(1)", NULL },
		{ "ftp://i.example.org/g.png", NULL },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++) {
		char *html, *url;

		html = g_strdup_printf("<html><head><meta property=\"og:image\" content=\"%s\">"
		                       "</head></html>", rows[i].content);
		url = og_image_url(html, "https://www.example.org/gallery/page?id=7");
		if (g_strcmp0(url, rows[i].expected) != 0)
			g_error("og:image \"%s\": got %s, expected %s", rows[i].content,
				url != NULL ? url : "NULL",
				rows[i].expected != NULL ? rows[i].expected : "NULL");
		g_free(url);
		g_free(html);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* a refused cache file is reported with g_warning(): no failure here */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	g_test_add_func("/image-fetch/address_filter", test_address_filter);
	g_test_add_func("/image-fetch/socket_cloexec", test_socket_cloexec);
	g_test_add_func("/image-fetch/deinit_removes_retry", test_deinit_removes_retry);
	g_test_add_func("/image-fetch/fired_retry", test_fired_retry);
	g_test_add_func("/image-fetch/cancel", test_cancel);
	g_test_add_func("/image-fetch/cache_file_refused", test_cache_file_refused);
	g_test_add_func("/image-fetch/retry_refused_when_full", test_retry_refused_when_full);
	g_test_add_func("/image-fetch/og_image_url", test_og_image_url);

	return g_test_run();
}
