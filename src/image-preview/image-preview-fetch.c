/*
 image-preview-fetch.c : Async HTTP image fetching for erssi-nc

    Copyright (C) 2024 erssi team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "module.h"
#include "image-preview.h"

#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/safe-file.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/fe-common/core/printtext.h>

#include <curl/curl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* Maximum HTML size for og:image extraction (512KB) */
#define MAX_HTML_SIZE (512 * 1024)

/* Fetch request state */
struct _IMAGE_FETCH_REC {
	char *url;               /* URL being fetched */
	char *cache_path;        /* Target cache path */
	char *part_path;         /* written here, renamed to cache_path when complete */
	CURL *curl_handle;       /* curl easy handle */
	FILE *fp;                /* Output file handle */
	LINE_REC *line;          /* Associated line in textbuffer */
	WINDOW_REC *window;      /* Target window */
	gint64 start_time;       /* Start timestamp for timeout */
	gint64 content_length;   /* Content-Length from response */
	gint64 received_bytes;   /* Actual bytes received for image data */
	gboolean cancelled;      /* Cancellation flag */

	/* Two-stage fetch support for page URLs */
	FetchStage stage;        /* Current fetch stage */
	char *original_url;      /* Original page URL (for stage 2) */
	GString *html_buffer;    /* HTML response buffer (stage 1) */
};

/* curl multi-handle for concurrent requests */
static CURLM *curl_multi = NULL;

/* Active fetches: url -> IMAGE_FETCH_REC */
static GHashTable *active_fetches = NULL;

/* libcurl is driven from the main loop: it names the sockets to watch
 * (CURLMOPT_SOCKETFUNCTION) and when to call it back (CURLMOPT_TIMERFUNCTION),
 * and is only called when one of them is ready or the time has come. It
 * never waits itself, so a download does not hold up input and drawing. */
static GHashTable *curl_watches = NULL; /* socket -> GSource id */
static guint curl_timeout_tag = 0;
static int curl_running = 0;            /* transfers curl has going */

/* Maximum concurrent fetches */
#define MAX_CONCURRENT_FETCHES 3

/* Retry data structure */
typedef struct {
	char *url;
	char *cache_path;
	LINE_REC *line;
	WINDOW_REC *window;
	gboolean is_page_url;
	guint tag;               /* its g_timeout_add() source */
} RetryData;

/* A failed download is tried once more after this delay */
#ifndef IMAGE_FETCH_RETRY_DELAY_MS
#define IMAGE_FETCH_RETRY_DELAY_MS 3000
#endif

/* Retries waiting for their timer, so that cancelling and
 * image_fetch_deinit() can remove the timers and free them */
static GSList *pending_retries = NULL;

/* Forward declarations */
static void fetch_complete(IMAGE_FETCH_REC *fetch, gboolean success, const char *error);
static void fetch_rec_free(IMAGE_FETCH_REC *fetch);
static void image_fetch_start_stage2(IMAGE_FETCH_REC *fetch, const char *og_image_url);
static char *extract_og_image(const char *html);
static gboolean retry_fetch_callback(gpointer user_data);

/* Remove a stuck fetch from active_fetches (cleanup when detected stuck) */
void image_fetch_cleanup_stuck(const char *url)
{
	GHashTableIter iter;
	gpointer key, value;
	const char *key_to_remove = NULL;

	if (active_fetches == NULL || url == NULL)
		return;

	/* Try direct lookup first */
	if (g_hash_table_remove(active_fetches, url)) {
		image_preview_debug_print("FETCH_CLEANUP: removed stuck fetch for %s", url);
		return;
	}

	/* Search by original_url for page URLs */
	g_hash_table_iter_init(&iter, active_fetches);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		IMAGE_FETCH_REC *f = value;
		if (f->original_url != NULL && strcmp(f->original_url, url) == 0) {
			key_to_remove = key;
			break;
		}
	}

	if (key_to_remove != NULL) {
		g_hash_table_remove(active_fetches, key_to_remove);
		image_preview_debug_print("FETCH_CLEANUP: removed stuck fetch (by original_url) for %s", url);
	}
}

/* Check if a fetch is actually active (in hash table AND curl timer running) */
gboolean image_fetch_is_active(const char *url)
{
	IMAGE_FETCH_REC *fetch;
	GHashTableIter iter;
	gpointer key, value;
	int still_running = 0;
	gboolean active;

	if (active_fetches == NULL || url == NULL)
		return FALSE;

	/* Check if URL is in active_fetches (also check original_url for page URLs) */
	fetch = g_hash_table_lookup(active_fetches, url);

	/* If not found directly, it might be a page URL where og:image is being fetched */
	if (fetch == NULL) {
		g_hash_table_iter_init(&iter, active_fetches);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			IMAGE_FETCH_REC *f = value;
			if (f->original_url != NULL && strcmp(f->original_url, url) == 0) {
				fetch = f;
				break;
			}
		}
	}

	if (fetch == NULL) {
		image_preview_debug_print("FETCH_IS_ACTIVE: %s NOT in active_fetches", url);
		return FALSE;
	}

	/* Active means: in hash table, with its transfer in curl, and curl
	 * waiting for a socket or its timer */
	still_running = curl_running;
	active = fetch->curl_handle != NULL &&
		(still_running > 0 || curl_timeout_tag != 0 ||
		 (curl_watches != NULL && g_hash_table_size(curl_watches) > 0));
	image_preview_debug_print("FETCH_IS_ACTIVE: %s found, timer=%u still_running=%d -> %s",
	                          url, curl_timeout_tag, still_running, active ? "ACTIVE" : "STUCK");
	return active;
}

/* Debug: dump current fetch state */
void image_fetch_debug_dump(void)
{
	GHashTableIter iter;
	gpointer key, value;
	int count = 0;

	image_preview_debug_print("FETCH_DUMP: curl_multi=%p timer_tag=%u running=%d sockets=%u",
	                          (void*)curl_multi, curl_timeout_tag, curl_running,
	                          curl_watches != NULL ? g_hash_table_size(curl_watches) : 0);

	if (active_fetches == NULL) {
		image_preview_debug_print("FETCH_DUMP: active_fetches is NULL!");
		return;
	}

	image_preview_debug_print("FETCH_DUMP: %u active fetches",
	                          g_hash_table_size(active_fetches));

	g_hash_table_iter_init(&iter, active_fetches);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		IMAGE_FETCH_REC *fetch = value;
		const char *stage_str = "?";
		switch (fetch->stage) {
			case FETCH_STAGE_IMAGE: stage_str = "IMAGE"; break;
			case FETCH_STAGE_HTML: stage_str = "HTML"; break;
			case FETCH_STAGE_OG_IMAGE: stage_str = "OG_IMAGE"; break;
		}
		image_preview_debug_print("FETCH_DUMP: [%d] url=%s stage=%s cancelled=%d",
		                          count++, fetch->url, stage_str, fetch->cancelled);
	}

}

/* A clicked link is someone else's URL: it may only lead to a public web
 * server - not to this host, the LAN or another protocol, also not after a
 * redirect or through the page's og:image. */
static gboolean address_is_public(const struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		guint32 a = ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);

		return !((a >> 24) == 0 || (a >> 24) == 10 || (a >> 24) == 127 ||
		         (a >> 16) == 0xA9FE ||            /* 169.254/16 link-local */
		         (a >> 20) == 0xAC1 ||             /* 172.16/12 */
		         (a >> 16) == 0xC0A8 ||            /* 192.168/16 */
		         (a >> 22) == (0x64400000 >> 22) || /* 100.64/10 CGNAT */
		         (a >> 8) == 0xC00000 ||           /* 192.0.0/24 IETF */
		         (a >> 17) == (0xC6120000 >> 17) || /* 198.18/15 benchmarking */
		         (a >> 28) >= 0xE);                /* multicast, reserved */
	}
	if (sa->sa_family == AF_INET6) {
		const struct in6_addr *a6 = &((const struct sockaddr_in6 *)sa)->sin6_addr;
		const guint8 *b = a6->s6_addr;

		/* IPv6 addresses that carry an IPv4 one lead to that one:
		 * mapped/compatible, NAT64 64:ff9b::/96 (and the local-use
		 * 64:ff9b:1::/48) and 6to4 2002::/16 */
		static const guint8 nat64[12] = { 0x00, 0x64, 0xff, 0x9b };
		const guint8 *v4addr = NULL;

		if (IN6_IS_ADDR_V4MAPPED(a6) || IN6_IS_ADDR_V4COMPAT(a6) ||
		    memcmp(b, nat64, 12) == 0)
			v4addr = b + 12;
		else if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b &&
		         b[4] == 0x00 && b[5] == 0x01)
			return FALSE;   /* local-use NAT64: may embed anything */
		else if (b[0] == 0x20 && b[1] == 0x02)
			v4addr = b + 2;
		if (v4addr != NULL) {
			struct sockaddr_in v4;

			memset(&v4, 0, sizeof(v4));
			v4.sin_family = AF_INET;
			memcpy(&v4.sin_addr, v4addr, 4);
			return address_is_public((struct sockaddr *)&v4);
		}
		return !(IN6_IS_ADDR_UNSPECIFIED(a6) || IN6_IS_ADDR_LOOPBACK(a6) ||
		         (b[0] & 0xFE) == 0xFC ||                   /* fc00::/7 ULA */
		         (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) || /* fe80::/10 */
		         (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) || /* fec0::/10 site-local */
		         b[0] == 0xFF);                             /* multicast */
	}
	return FALSE;
}

/* the test of the main loop downloads from this host */
#ifndef IMAGE_FETCH_ADDRESS_ALLOWED
#define IMAGE_FETCH_ADDRESS_ALLOWED(sa) address_is_public(sa)
#endif

/* The socket of a download is close-on-exec: neither /EXEC children nor
 * the programs scripts start, nor the binary /UPGRADE runs, get it. curl
 * does not set that on its sockets, so it is done here. */
static curl_socket_t open_public_socket(void *clientp, curlsocktype purpose,
                                        struct curl_sockaddr *address)
{
	curl_socket_t fd;

	if (!IMAGE_FETCH_ADDRESS_ALLOWED(&address->addr)) {
		image_preview_debug_print("FETCH: refusing to connect to a local address");
		return CURL_SOCKET_BAD;
	}
#ifdef SOCK_CLOEXEC
	fd = socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
#else
	fd = socket(address->family, address->socktype, address->protocol);
	if (fd != CURL_SOCKET_BAD)
		fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
	return fd;
}

/* Options for every request: web only, public addresses, size and speed
 * limits */
static void set_safe_options(CURL *curl)
{
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_public_socket);
	/* direct connections only: through http(s)_proxy the check above would
	 * see the proxy, not the server, and the proxy would reach the LAN */
	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
	                 (curl_off_t)settings_get_int(IMAGE_PREVIEW_MAX_FILE_SIZE) * 1024 * 1024);
	/* a server sending 1 KB/s must not hold the download for a minute */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
}

/* Track bytes written for debug */
static gint64 total_bytes_written = 0;

/* Write callback for curl */
static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	IMAGE_FETCH_REC *fetch = userdata;
	size_t total = size * nmemb;
	size_t written;
	static gint64 last_log = 0;

	if (fetch->cancelled) {
		image_preview_debug_print("FETCH: write_callback cancelled");
		return 0;  /* Abort transfer */
	}

	if (fetch->fp == NULL) {
		image_preview_debug_print("FETCH: write_callback fp is NULL!");
		return 0;
	}

	/* Content-Length can be missing or wrong (chunked, compressed): count
	 * what really arrives */
	if (fetch->received_bytes + (gint64)total >
	    (gint64)settings_get_int(IMAGE_PREVIEW_MAX_FILE_SIZE) * 1024 * 1024) {
		image_preview_debug_print("FETCH: image larger than image_preview_max_file_size, cancelling");
		fetch->cancelled = TRUE;
		return 0;
	}

	written = fwrite(ptr, size, nmemb, fetch->fp);
	total_bytes_written += written;
	fetch->received_bytes += written;  /* Track per-fetch */

	/* Log every ~100KB */
	if (total_bytes_written - last_log > 100000) {
		image_preview_debug_print("FETCH: write_callback stage=%d written=%zu total=%" G_GINT64_FORMAT " received=%" G_GINT64_FORMAT,
		                          fetch->stage, written, total_bytes_written, fetch->received_bytes);
		last_log = total_bytes_written;
	}

	if (written != total) {
		image_preview_debug_print("FETCH: write_callback ERROR: wrote %zu of %zu bytes!",
		                          written, total);
	}

	return written;
}

/* Header callback for curl - check Content-Length and log headers */
static size_t header_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	IMAGE_FETCH_REC *fetch = userdata;
	size_t total = size * nmemb;
	char *header = ptr;
	gint64 max_size;

	/* Log important headers for debugging */
	if (g_ascii_strncasecmp(header, "HTTP/", 5) == 0) {
		/* Log HTTP status line */
		char status[128];
		char *nl;
		size_t len = total < 127 ? total : 127;
		strncpy(status, header, len);
		status[len] = '\0';
		/* Remove trailing newlines */
		nl = strchr(status, '\r');
		if (nl) *nl = '\0';
		nl = strchr(status, '\n');
		if (nl) *nl = '\0';
		image_preview_debug_print("FETCH: HTTP response: %s (stage=%d)", status, fetch->stage);
	}

	if (g_ascii_strncasecmp(header, "Content-Type:", 13) == 0) {
		char ctype[256];
		char *nl;
		size_t len = total < 255 ? total : 255;
		strncpy(ctype, header, len);
		ctype[len] = '\0';
		nl = strchr(ctype, '\r');
		if (nl) *nl = '\0';
		nl = strchr(ctype, '\n');
		if (nl) *nl = '\0';
		image_preview_debug_print("FETCH: %s (stage=%d)", ctype, fetch->stage);
	}

	/* Check for Content-Length header */
	if (g_ascii_strncasecmp(header, "Content-Length:", 15) == 0) {
		fetch->content_length = g_ascii_strtoll(header + 15, NULL, 10);
		image_preview_debug_print("FETCH: Content-Length: %lld (stage=%d)",
		                          (long long)fetch->content_length, fetch->stage);

		/* For HTML stage, limit is different */
		if (fetch->stage == FETCH_STAGE_HTML) {
			if (fetch->content_length > MAX_HTML_SIZE) {
				image_preview_debug_print("FETCH: HTML too large in header, cancelling");
				fetch->cancelled = TRUE;
				return 0;
			}
			return total;
		}

		/* Check against max file size for image downloads */
		max_size = settings_get_int(IMAGE_PREVIEW_MAX_FILE_SIZE) * 1024 * 1024;
		if (fetch->content_length > max_size) {
			image_preview_debug_print("FETCH: Image too large (%lld > %lld), cancelling",
			                          (long long)fetch->content_length, (long long)max_size);
			fetch->cancelled = TRUE;
			return 0;  /* Abort */
		}
	}

	return total;
}

/* Write callback for HTML pages (stage 1) - accumulate in buffer */
static size_t write_callback_html(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	IMAGE_FETCH_REC *fetch = userdata;
	size_t total = size * nmemb;

	if (fetch->cancelled) {
		image_preview_debug_print("FETCH: write_callback_html - cancelled, returning 0");
		return 0;
	}

	if (fetch->html_buffer == NULL) {
		image_preview_debug_print("FETCH: write_callback_html - html_buffer is NULL!");
		return 0;
	}

	/* Accumulate HTML in buffer */
	g_string_append_len(fetch->html_buffer, ptr, total);

	image_preview_debug_print("FETCH: write_callback_html - received %zu bytes, total now %zu",
	                          total, fetch->html_buffer->len);

	/* Enforce size limit */
	if (fetch->html_buffer->len > MAX_HTML_SIZE) {
		image_preview_debug_print("FETCH: HTML too large (%zu > %d), cancelling",
		                          fetch->html_buffer->len, MAX_HTML_SIZE);
		fetch->cancelled = TRUE;
		return 0;
	}

	return total;
}

/* Extract og:image URL from HTML content */
static char *extract_og_image(const char *html)
{
	GRegex *regex;
	GMatchInfo *match_info;
	char *og_image = NULL;
	GError *error = NULL;

	if (html == NULL || *html == '\0') {
		image_preview_debug_print("FETCH: extract_og_image - HTML is NULL or empty");
		return NULL;
	}

	image_preview_debug_print("FETCH: extract_og_image - parsing %zu bytes of HTML", strlen(html));

	/* Log first 500 chars of HTML for debugging */
	if (strlen(html) > 0) {
		char preview[501];
		strncpy(preview, html, 500);
		preview[500] = '\0';
		/* Replace newlines with spaces for logging */
		for (char *p = preview; *p; p++) {
			if (*p == '\n' || *p == '\r') *p = ' ';
		}
		image_preview_debug_print("FETCH: HTML preview: %.200s...", preview);
	}

	/* Pattern matches both attribute orders:
	 * <meta property="og:image" content="URL">
	 * <meta content="URL" property="og:image"> */
	regex = g_regex_new(
		"<meta[^>]+property=[\"']og:image[\"'][^>]+content=[\"']([^\"']+)[\"']"
		"|<meta[^>]+content=[\"']([^\"']+)[\"'][^>]+property=[\"']og:image[\"']",
		G_REGEX_CASELESS | G_REGEX_DOTALL, 0, &error);

	if (error != NULL) {
		image_preview_debug_print("FETCH: og:image regex compile failed: %s", error->message);
		g_error_free(error);
		return NULL;
	}

	if (g_regex_match(regex, html, 0, &match_info)) {
		image_preview_debug_print("FETCH: regex matched!");
		/* Try first capture group */
		og_image = g_match_info_fetch(match_info, 1);
		image_preview_debug_print("FETCH: group 1 = '%s'", og_image ? og_image : "(null)");
		if (og_image == NULL || *og_image == '\0') {
			g_free(og_image);
			/* Try second capture group */
			og_image = g_match_info_fetch(match_info, 2);
			image_preview_debug_print("FETCH: group 2 = '%s'", og_image ? og_image : "(null)");
		}
	} else {
		image_preview_debug_print("FETCH: regex did NOT match - no og:image meta tag found");
		/* Try to find any og:image text in HTML for debugging */
		if (strstr(html, "og:image") != NULL) {
			image_preview_debug_print("FETCH: 'og:image' string exists but regex didn't match pattern");
		} else {
			image_preview_debug_print("FETCH: 'og:image' string NOT found in HTML at all");
		}
	}

	g_match_info_free(match_info);
	g_regex_unref(regex);

	if (og_image != NULL && *og_image != '\0') {
		image_preview_debug_print("FETCH: SUCCESS extracted og:image: %s", og_image);
	} else {
		g_free(og_image);
		og_image = NULL;
		image_preview_debug_print("FETCH: FAILED to extract og:image");
	}

	return og_image;
}

/* The og:image value is HTML attribute text: "&amp;" in it is "&". Only
 * the entities a URL needs are decoded (the five basic ones and numeric
 * references to printable ASCII), anything else is kept as it is. */
static char *html_attr_decode(const char *value)
{
	static const struct {
		const char *name;
		char chr;
	} entities[] = {
		{ "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
	};
	GString *out;
	const char *p;

	out = g_string_sized_new(strlen(value));
	for (p = value; *p != '\0'; p++) {
		gboolean done = FALSE;
		guint i;

		if (*p != '&') {
			g_string_append_c(out, *p);
			continue;
		}
		for (i = 0; i < G_N_ELEMENTS(entities) && !done; i++) {
			size_t len = strlen(entities[i].name);

			if (strncmp(p + 1, entities[i].name, len) == 0) {
				g_string_append_c(out, entities[i].chr);
				p += len;
				done = TRUE;
			}
		}
		if (!done && p[1] == '#') {
			const char *num = p + 2;
			char *end;
			gulong chr;

			if (*num == 'x' || *num == 'X')
				chr = strtoul(num + 1, &end, 16);
			else
				chr = strtoul(num, &end, 10);
			if (end != num && end != num + 1 && *end == ';' && chr >= 0x20 && chr < 0x7f) {
				g_string_append_c(out, (char) chr);
				p = end;
				done = TRUE;
			}
		}
		if (!done)
			g_string_append_c(out, '&');
	}
	return g_string_free(out, FALSE);
}

/* The URL of the picture: og:image decoded and, when it is relative
 * ("/img.png", "//cdn/x.png"), resolved against the page it was found on.
 * NULL unless the result is an http or https URL - the download is limited
 * to those anyway, and to public addresses (open_public_socket()). */
static char *og_image_resolve(const char *page_url, const char *value)
{
	char *decoded, *result = NULL;

	decoded = html_attr_decode(value);
	g_strstrip(decoded);
	if (*decoded == '\0') {
		g_free(decoded);
		return NULL;
	}
#if LIBCURL_VERSION_NUM >= 0x073e00
	{
		CURLU *url = curl_url();
		char *scheme = NULL, *full = NULL;

		if (url != NULL && page_url != NULL &&
		    curl_url_set(url, CURLUPART_URL, page_url, 0) == CURLUE_OK &&
		    curl_url_set(url, CURLUPART_URL, decoded, 0) == CURLUE_OK &&
		    curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
		    (g_ascii_strcasecmp(scheme, "http") == 0 ||
		     g_ascii_strcasecmp(scheme, "https") == 0) &&
		    curl_url_get(url, CURLUPART_URL, &full, 0) == CURLUE_OK)
			result = g_strdup(full);
		curl_free(scheme);
		curl_free(full);
		curl_url_cleanup(url);
	}
#else
	/* no URL parser in this libcurl: absolute URLs only */
	if (g_ascii_strncasecmp(decoded, "http://", 7) == 0 ||
	    g_ascii_strncasecmp(decoded, "https://", 8) == 0)
		result = g_strdup(decoded);
#endif
	g_free(decoded);
	return result;
}

/* The picture of a downloaded page, NULL when it names none */
static char *og_image_url(const char *html, const char *page_url)
{
	char *value, *url;

	value = extract_og_image(html);
	if (value == NULL)
		return NULL;
	url = og_image_resolve(page_url, value);
	image_preview_debug_print("FETCH: og:image %s -> %s", value, url != NULL ? url : "(refused)");
	g_free(value);
	return url;
}

/* Hands every finished transfer to fetch_complete() */
static void check_multi_info(void)
{
	CURLMsg *msg;
	int msgs_left;

	while ((msg = curl_multi_info_read(curl_multi, &msgs_left)) != NULL) {
		if (msg->msg == CURLMSG_DONE) {
			CURL *easy = msg->easy_handle;
			CURLcode result = msg->data.result;
			IMAGE_FETCH_REC *fetch = NULL;

			/* Get our fetch record from the easy handle */
			curl_easy_getinfo(easy, CURLINFO_PRIVATE, &fetch);

			image_preview_debug_print("FETCH: transfer done, result=%d (%s)",
			                          result, curl_easy_strerror(result));

			if (fetch != NULL) {
				if (result == CURLE_OK && !fetch->cancelled) {
					fetch_complete(fetch, TRUE, NULL);
				} else if (fetch->cancelled) {
					fetch_complete(fetch, FALSE, "Cancelled or file too large");
				} else {
					fetch_complete(fetch, FALSE, curl_easy_strerror(result));
				}
			}
		}
	}
}

/* Lets curl work on a socket that is ready (or on its timers) */
static void curl_socket_action(curl_socket_t fd, int flags)
{
	CURLMcode mc;

	if (curl_multi == NULL)
		return;
	mc = curl_multi_socket_action(curl_multi, fd, flags, &curl_running);
	if (mc != CURLM_OK)
		image_preview_debug_print("FETCH: curl_multi_socket_action failed: %s",
		                          curl_multi_strerror(mc));
	check_multi_info();
}

static gboolean curl_fd_ready(GIOChannel *source, GIOCondition condition, gpointer data)
{
	int fd = GPOINTER_TO_INT(data);
	int flags = 0;

	/* a hang-up is read as the end of the data */
	if (condition & (G_IO_IN | G_IO_PRI | G_IO_HUP))
		flags |= CURL_CSELECT_IN;
	if (condition & G_IO_OUT)
		flags |= CURL_CSELECT_OUT;
	if (condition & (G_IO_ERR | G_IO_NVAL))
		flags |= CURL_CSELECT_ERR;
	/* curl removes or replaces this watch through curl_socket_cb()
	 * when it is done with the socket */
	curl_socket_action(fd, flags);
	return G_SOURCE_CONTINUE;
}

static void curl_watch_remove(curl_socket_t fd)
{
	gpointer tag;

	if (curl_watches != NULL &&
	    g_hash_table_lookup_extended(curl_watches, GINT_TO_POINTER(fd), NULL, &tag)) {
		g_source_remove(GPOINTER_TO_UINT(tag));
		g_hash_table_remove(curl_watches, GINT_TO_POINTER(fd));
	}
}

/* CURLMOPT_SOCKETFUNCTION: watch fd for what curl waits for */
static int curl_socket_cb(CURL *easy, curl_socket_t fd, int what, void *userp, void *socketp)
{
	GIOCondition condition = 0;
	GIOChannel *channel;
	guint tag;

	curl_watch_remove(fd);
	if (what == CURL_POLL_REMOVE || curl_watches == NULL)
		return 0;

	if (what & CURL_POLL_IN)
		condition |= G_IO_IN | G_IO_PRI | G_IO_HUP | G_IO_ERR;
	if (what & CURL_POLL_OUT)
		condition |= G_IO_OUT | G_IO_ERR;
	/* the watch keeps the channel; it does not close the socket */
	channel = g_io_channel_unix_new(fd);
	tag = g_io_add_watch(channel, condition, curl_fd_ready, GINT_TO_POINTER(fd));
	g_io_channel_unref(channel);
	g_hash_table_insert(curl_watches, GINT_TO_POINTER(fd), GUINT_TO_POINTER(tag));
	return 0;
}

static gboolean curl_timeout_fire(gpointer data)
{
	curl_timeout_tag = 0;
	curl_socket_action(CURL_SOCKET_TIMEOUT, 0);
	return G_SOURCE_REMOVE;
}

/* CURLMOPT_TIMERFUNCTION: call curl back in timeout_ms (-1: never). Not
 * at once from here: curl must not be called from its own callback. */
static int curl_timer_cb(CURLM *multi, long timeout_ms, void *userp)
{
	if (curl_timeout_tag != 0) {
		g_source_remove(curl_timeout_tag);
		curl_timeout_tag = 0;
	}
	if (timeout_ms >= 0)
		curl_timeout_tag = g_timeout_add(timeout_ms, curl_timeout_fire, NULL);
	return 0;
}

static void retry_data_free(RetryData *retry)
{
	g_free(retry->url);
	g_free(retry->cache_path);
	g_free(retry);
}

/* Starts the timer of a retry */
static void schedule_retry(RetryData *retry)
{
	retry->tag = g_timeout_add(IMAGE_FETCH_RETRY_DELAY_MS, retry_fetch_callback, retry);
	pending_retries = g_slist_prepend(pending_retries, retry);
}

/* Removes the timers of the waiting retries of url (all when NULL) */
static void cancel_retries(const char *url)
{
	GSList *tmp, *next;

	for (tmp = pending_retries; tmp != NULL; tmp = next) {
		RetryData *retry = tmp->data;

		next = tmp->next;
		if (url != NULL && g_strcmp0(retry->url, url) != 0)
			continue;
		if (retry->tag != 0)
			g_source_remove(retry->tag);
		pending_retries = g_slist_delete_link(pending_retries, tmp);
		retry_data_free(retry);
	}
}

/* Retry timer callback - starts a new fetch after 3 second delay */
static gboolean retry_fetch_callback(gpointer user_data)
{
	RetryData *retry = user_data;
	IMAGE_PREVIEW_REC *preview;

	pending_retries = g_slist_remove(pending_retries, retry);
	retry->tag = 0;

	image_preview_debug_print("RETRY: timer fired, starting retry for %s", retry->url);

	/* Find the preview record */
	preview = image_preview_get_url(retry->line, retry->url);
	if (preview == NULL) {
		image_preview_debug_print("RETRY: preview record gone, aborting");
		retry_data_free(retry);
		return FALSE;
	}

	/* Reset fetch state for retry */
	preview->fetch_pending = TRUE;
	preview->fetch_failed = FALSE;
	g_free(preview->error_message);
	preview->error_message = NULL;

	/* Start the fetch again */
	if (!image_fetch_start(retry->url, retry->cache_path, retry->line,
	                       retry->window, retry->is_page_url)) {
		image_preview_debug_print("RETRY: fetch_start failed, showing error");
		preview->fetch_pending = FALSE;
		preview->fetch_failed = TRUE;
		preview->error_message = g_strdup("Retry failed");

		/* Show error popup */
		if (preview->show_on_complete) {
			preview->show_on_complete = FALSE;
			image_preview_show_error_popup();
		}
	}

	retry_data_free(retry);
	return FALSE;  /* Don't repeat timer */
}

/* The URL of the line the download is for: the page of a page URL */
static const char *fetch_record_url(IMAGE_FETCH_REC *fetch)
{
	return fetch->original_url != NULL ? fetch->original_url : fetch->url;
}

/* Handle fetch completion */
static void fetch_complete(IMAGE_FETCH_REC *fetch, gboolean success, const char *error)
{
	IMAGE_PREVIEW_REC *preview;

	if (fetch == NULL)
		return;

	image_preview_debug_print("FETCH: complete url=%s stage=%d success=%d error=%s",
	                          fetch->url, fetch->stage, success, error ? error : "none");

	/* Handle HTML stage completion (stage 1 of two-stage fetch) */
	if (fetch->stage == FETCH_STAGE_HTML) {
		char *og_image = NULL;

		image_preview_debug_print("FETCH: HTML stage complete, success=%d", success);

		/* Remove from curl multi but don't destroy handle yet */
		if (fetch->curl_handle != NULL) {
			curl_multi_remove_handle(curl_multi, fetch->curl_handle);
		}

		if (success && fetch->html_buffer != NULL && fetch->html_buffer->len > 0) {
			const char *page_url = NULL;

			image_preview_debug_print("FETCH: HTML buffer has %zu bytes, parsing for og:image...",
			                          fetch->html_buffer->len);
			/* relative to the page as served, after any redirect */
			if (fetch->curl_handle == NULL ||
			    curl_easy_getinfo(fetch->curl_handle, CURLINFO_EFFECTIVE_URL,
			                      &page_url) != CURLE_OK || page_url == NULL)
				page_url = fetch->original_url != NULL ? fetch->original_url : fetch->url;
			/* Parse HTML for og:image */
			og_image = og_image_url(fetch->html_buffer->str, page_url);
		} else {
			image_preview_debug_print("FETCH: HTML stage failed - success=%d buffer=%p len=%zu",
			                          success,
			                          (void*)fetch->html_buffer,
			                          fetch->html_buffer ? fetch->html_buffer->len : 0);
		}

		if (og_image != NULL) {
			/* Start stage 2: fetch the actual image */
			image_preview_debug_print("FETCH: og:image found! Starting stage 2 fetch for: %s", og_image);
			image_fetch_start_stage2(fetch, og_image);
			g_free(og_image);
			return;  /* Don't cleanup yet - stage 2 will continue */
		}

		/* HTML fetch failed or no og:image found */
		image_preview_debug_print("FETCH: HTML stage FAILED - no og:image found");

		/* Cleanup curl handle now */
		if (fetch->curl_handle != NULL) {
			curl_easy_cleanup(fetch->curl_handle);
			fetch->curl_handle = NULL;
		}

		/* Update preview record - check for retry */
		preview = image_preview_get_url(fetch->line, fetch_record_url(fetch));
		if (preview != NULL) {
			/* Check if we can retry (max 1 retry) */
			if (preview->retry_count < 1) {
				RetryData *retry;

				preview->retry_count++;
				image_preview_debug_print("FETCH: HTML stage - scheduling retry #%d in 3 seconds",
				                          preview->retry_count);

				/* Prepare retry data */
				retry = g_new0(RetryData, 1);
				retry->url = g_strdup(fetch->original_url ? fetch->original_url : fetch->url);
				retry->cache_path = g_strdup(fetch->cache_path);
				retry->line = fetch->line;
				retry->window = fetch->window;
				retry->is_page_url = TRUE;  /* It was a page URL */

				/* Schedule retry in 3 seconds */
				schedule_retry(retry);

				/* Remove from active fetches so retry can proceed */
				if (active_fetches != NULL) {
					g_hash_table_remove(active_fetches, fetch->original_url ? fetch->original_url : fetch->url);
				}
				return;
			}

			/* Already retried, mark as failed */
			image_preview_debug_print("FETCH: HTML stage - retry exhausted, marking as failed");
			preview->fetch_pending = FALSE;
			preview->fetch_failed = TRUE;
			preview->error_message = g_strdup("No og:image found in page");

			/* Show error popup if user was waiting */
			if (preview->show_on_complete) {
				preview->show_on_complete = FALSE;
				image_preview_show_error_popup();
			}
		}

		/* Remove from active fetches */
		if (active_fetches != NULL) {
			g_hash_table_remove(active_fetches, fetch->original_url ? fetch->original_url : fetch->url);
		}
		return;
	}

	/* Direct image fetch or stage 2 (og:image fetch) completion */
	image_preview_debug_print("FETCH: image download complete, stage=%d received=%" G_GINT64_FORMAT " expected=%" G_GINT64_FORMAT,
	                          fetch->stage, fetch->received_bytes, fetch->content_length);

	/* Close file */
	if (fetch->fp != NULL) {
		fflush(fetch->fp);
		fclose(fetch->fp);
		fetch->fp = NULL;
	}

	/* Verify we got all expected bytes (if Content-Length was provided) */
	if (success && fetch->content_length > 0 && fetch->received_bytes < fetch->content_length) {
		image_preview_debug_print("FETCH: INCOMPLETE DOWNLOAD! Expected %" G_GINT64_FORMAT " bytes but got %" G_GINT64_FORMAT,
		                          fetch->content_length, fetch->received_bytes);
		success = FALSE;
		error = "Incomplete download";
	}

	/* complete: under the cache's name; anything else: gone, whether or
	 * not the line still has a record */
	if (fetch->part_path != NULL) {
		if (success && rename(fetch->part_path, fetch->cache_path) != 0) {
			image_preview_debug_print("FETCH: rename to %s failed: %s",
			                          fetch->cache_path, strerror(errno));
			success = FALSE;
			error = "Failed to save the download";
		}
		if (!success)
			unlink(fetch->part_path);
	}

	/* Remove from curl multi */
	if (fetch->curl_handle != NULL) {
		curl_multi_remove_handle(curl_multi, fetch->curl_handle);
		curl_easy_cleanup(fetch->curl_handle);
		fetch->curl_handle = NULL;
	}

	/* Update preview record: the line's record for this URL, not one
	 * a new line at the address of a freed one has */
	preview = image_preview_get_url(fetch->line, fetch_record_url(fetch));
	if (preview != NULL) {
		preview->fetch_pending = FALSE;
		if (success) {
			preview->fetch_failed = FALSE;
			image_preview_debug_print("FETCH: saved to %s", fetch->cache_path);
			/* the new file may take the cache over its size limit */
			image_cache_enforce_limit(fetch->cache_path);
		} else {
			/* Check if we can retry (max 1 retry) */
			if (preview->retry_count < 1) {
				RetryData *retry;
				ImageUrlType url_type;

				preview->retry_count++;
				image_preview_debug_print("FETCH: scheduling retry #%d in 3 seconds",
				                          preview->retry_count);

				/* Determine if original URL was a page URL */
				url_type = image_preview_classify_url(
					fetch->original_url ? fetch->original_url : fetch->url);

				/* Prepare retry data */
				retry = g_new0(RetryData, 1);
				retry->url = g_strdup(fetch->original_url ? fetch->original_url : fetch->url);
				retry->cache_path = g_strdup(fetch->cache_path);
				retry->line = fetch->line;
				retry->window = fetch->window;
				retry->is_page_url = (url_type != URL_TYPE_DIRECT_IMAGE);

				/* Schedule retry in 3 seconds */
				schedule_retry(retry);

				/* Keep fetch_pending true so we don't trigger error state */
				preview->fetch_pending = TRUE;
			} else {
				/* Already retried, mark as failed */
				image_preview_debug_print("FETCH: retry exhausted, marking as failed");
				preview->fetch_failed = TRUE;
				preview->error_message = g_strdup(error ? error : "Unknown error");

				/* Show error popup if user was waiting */
				if (preview->show_on_complete) {
					preview->show_on_complete = FALSE;
					image_preview_show_error_popup();
				}
			}
		}
	} else {
		image_preview_debug_print("FETCH: WARNING preview record is NULL!");
	}

	/* Emit signal for UI update - only for the record of this download,
	 * the receiver finds it by the line alone */
	if (success && preview != NULL) {
		image_preview_debug_print("FETCH: emitting 'image preview ready' signal");
		signal_emit("image preview ready", 2, fetch->line, fetch->window);
	}

	/* Remove from active fetches - use original_url if this was a stage 2 fetch */
	if (active_fetches != NULL) {
		const char *key = fetch->original_url ? fetch->original_url : fetch->url;
		g_hash_table_remove(active_fetches, key);
	}
}

/* Free fetch record */
static void fetch_rec_free(IMAGE_FETCH_REC *fetch)
{
	if (fetch == NULL)
		return;

	if (fetch->fp != NULL) {
		fclose(fetch->fp);
	}
	/* a download that did not complete (cancelled, deinit, failed): after
	 * a completed one the .part file was renamed and this does nothing */
	if (fetch->part_path != NULL)
		unlink(fetch->part_path);

	if (fetch->curl_handle != NULL) {
		if (curl_multi != NULL) {
			curl_multi_remove_handle(curl_multi, fetch->curl_handle);
		}
		curl_easy_cleanup(fetch->curl_handle);
	}

	if (fetch->html_buffer != NULL) {
		g_string_free(fetch->html_buffer, TRUE);
	}

	g_free(fetch->url);
	g_free(fetch->cache_path);
	g_free(fetch->part_path);
	g_free(fetch->original_url);
	g_free(fetch);
}

/* The download is written to <cache name>.part: not through a symbolic
 * link, never into a FIFO or another user's file. Only a complete download
 * is renamed to the cache's name, so the cache never finds a partial file
 * (its first bytes would pass the check for an image's magic bytes); the
 * .part file is removed when the download fails, is cancelled or is still
 * running when the module goes (fetch_rec_free()). */
static FILE *open_cache_file(const char *path)
{
	FILE *fp;
	int fd, err;

	fd = safe_file_open_truncate(path, 0600);
	if (fd < 0)
		return NULL;
	fp = fdopen(fd, "wb");
	if (fp == NULL) {
		err = errno;
		close(fd);
		errno = err;
	}
	return fp;
}

static FILE *open_part_file(IMAGE_FETCH_REC *fetch)
{
	if (fetch->part_path == NULL)
		fetch->part_path = g_strconcat(fetch->cache_path, ".part", NULL);
	return open_cache_file(fetch->part_path);
}

/* Start stage 2: fetch actual image from og:image URL */
static void image_fetch_start_stage2(IMAGE_FETCH_REC *fetch, const char *og_image_url)
{
	CURLMcode mc;
	int timeout_ms;

	image_preview_debug_print("FETCH: stage2 start og_image=%s", og_image_url);

	/* Reset bytes counter for stage 2 */
	total_bytes_written = 0;

	/* Cleanup stage 1 resources */
	if (fetch->html_buffer != NULL) {
		g_string_free(fetch->html_buffer, TRUE);
		fetch->html_buffer = NULL;
	}

	/* Setup stage 2 */
	fetch->stage = FETCH_STAGE_OG_IMAGE;
	g_free(fetch->url);
	fetch->url = g_strdup(og_image_url);
	fetch->content_length = 0;
	fetch->received_bytes = 0;  /* Reset for stage 2 */

	/* Open cache file for image */
	fetch->fp = open_part_file(fetch);
	if (fetch->fp == NULL) {
		image_preview_debug_print("FETCH: stage2 failed to open cache file: %s", strerror(errno));
		fetch_complete(fetch, FALSE, "Failed to open cache file");
		return;
	}

	/* Reconfigure curl for image fetch */
	curl_easy_reset(fetch->curl_handle);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_URL, og_image_url);
	set_safe_options(fetch->curl_handle);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_WRITEDATA, fetch);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_HEADERDATA, fetch);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_PRIVATE, fetch);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_USERAGENT,
	                 "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15");
	curl_easy_setopt(fetch->curl_handle, CURLOPT_NOSIGNAL, 1L);

	/* Enable HTTP/2 */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
	/* Keep connection alive for potential reuse */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
	/* Accept compressed responses */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_ACCEPT_ENCODING, "");
	/* Set buffer size for better throughput */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_BUFFERSIZE, 102400L);

	/* Set timeout */
	timeout_ms = settings_get_time(IMAGE_PREVIEW_TIMEOUT);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
	/* Also set connect timeout */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);

	/* Re-add to multi handle */
	mc = curl_multi_add_handle(curl_multi, fetch->curl_handle);
	if (mc != CURLM_OK) {
		image_preview_debug_print("FETCH: stage2 curl_multi_add_handle failed: %s",
		                          curl_multi_strerror(mc));
		fetch_complete(fetch, FALSE, "curl_multi_add_handle failed");
		return;
	}

	/* curl_multi_add_handle() set curl's timer: the main loop goes on
	 * with stage 2 */
	image_preview_debug_print("FETCH: stage2 started successfully");
}

/* Start async fetch */
gboolean image_fetch_start(const char *url, const char *cache_path,
                           LINE_REC *line, WINDOW_REC *window,
                           gboolean is_page_url)
{
	IMAGE_FETCH_REC *fetch;
	CURLMcode mc;
	int timeout_ms;

	image_preview_debug_print("FETCH: start url=%s cache=%s is_page=%d",
	                          url, cache_path, is_page_url);

	if (curl_multi == NULL || url == NULL || cache_path == NULL) {
		image_preview_debug_print("FETCH: start failed - NULL params (multi=%p)", curl_multi);
		return FALSE;
	}

	/* Check if already fetching this URL */
	if (active_fetches != NULL && g_hash_table_contains(active_fetches, url)) {
		image_preview_debug_print("FETCH: already fetching this URL");
		return FALSE;
	}

	/* Check concurrent fetch limit */
	if (active_fetches != NULL && g_hash_table_size(active_fetches) >= MAX_CONCURRENT_FETCHES) {
		image_preview_debug_print("FETCH: concurrent limit reached");
		return FALSE;
	}

	/* the cache's hard limit (image_cache_full()), for every download -
	 * a retry too: files too new to be removed fill it, no more until
	 * they age */
	if (image_cache_full()) {
		image_preview_debug_print("FETCH: cache over twice its size limit");
		printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
		          "Image preview: the cache holds more than twice "
		          "image_preview_cache_size of files from the last minutes - no new "
		          "download until they can be removed (/IMAGE CLEAR empties the cache)");
		return FALSE;
	}

	/* Create fetch record */
	fetch = g_new0(IMAGE_FETCH_REC, 1);
	fetch->url = g_strdup(url);
	fetch->cache_path = g_strdup(cache_path);
	fetch->line = line;
	fetch->window = window;
	fetch->start_time = g_get_monotonic_time();
	fetch->cancelled = FALSE;

	/* Setup based on URL type */
	if (is_page_url) {
		/* Stage 1: Fetch HTML page */
		fetch->stage = FETCH_STAGE_HTML;
		fetch->original_url = g_strdup(url);
		fetch->html_buffer = g_string_new(NULL);
		fetch->fp = NULL;  /* No file output in stage 1 */
	} else {
		/* Direct image fetch */
		fetch->stage = FETCH_STAGE_IMAGE;
		fetch->original_url = NULL;
		fetch->html_buffer = NULL;

		/* Open output file */
		fetch->fp = open_part_file(fetch);
		if (fetch->fp == NULL) {
			g_warning("image-fetch: Failed to open %s for writing: %s",
			          cache_path, strerror(errno));
			fetch_rec_free(fetch);
			return FALSE;
		}
	}

	/* Create curl easy handle */
	fetch->curl_handle = curl_easy_init();
	if (fetch->curl_handle == NULL) {
		g_warning("image-fetch: curl_easy_init failed");
		fetch_rec_free(fetch);
		return FALSE;
	}

	/* Configure curl */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_URL, url);
	set_safe_options(fetch->curl_handle);
	if (is_page_url) {
		curl_easy_setopt(fetch->curl_handle, CURLOPT_WRITEFUNCTION, write_callback_html);
	} else {
		curl_easy_setopt(fetch->curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
	}
	curl_easy_setopt(fetch->curl_handle, CURLOPT_WRITEDATA, fetch);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_HEADERDATA, fetch);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_PRIVATE, fetch);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_USERAGENT,
	                 "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15");
	curl_easy_setopt(fetch->curl_handle, CURLOPT_NOSIGNAL, 1L);

	/* Enable HTTP/2 */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
	/* Keep connection alive for potential reuse */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
	/* Accept compressed responses */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_ACCEPT_ENCODING, "");
	/* Set buffer size for better throughput */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_BUFFERSIZE, 102400L);

	/* Set timeout */
	timeout_ms = settings_get_time(IMAGE_PREVIEW_TIMEOUT);
	curl_easy_setopt(fetch->curl_handle, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
	/* Also set connect timeout */
	curl_easy_setopt(fetch->curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);

	/* Add to multi handle */
	mc = curl_multi_add_handle(curl_multi, fetch->curl_handle);
	if (mc != CURLM_OK) {
		g_warning("image-fetch: curl_multi_add_handle failed: %s",
		          curl_multi_strerror(mc));
		fetch_rec_free(fetch);
		return FALSE;
	}

	/* Track active fetch - use original_url for page URLs so we can find it later */
	g_hash_table_insert(active_fetches, fetch->original_url ? fetch->original_url : fetch->url, fetch);

	/* curl_multi_add_handle() set curl's timer: the main loop takes it
	 * from here */
	image_preview_debug_print("FETCH: started successfully, stage=%d", fetch->stage);
	return TRUE;
}

/* Cancel fetch by URL */
void image_fetch_cancel(const char *url)
{
	IMAGE_FETCH_REC *fetch;

	if (url == NULL)
		return;
	cancel_retries(url);
	if (active_fetches == NULL)
		return;

	fetch = g_hash_table_lookup(active_fetches, url);
	if (fetch != NULL) {
		fetch->cancelled = TRUE;
	}
}

/* Cancel all active fetches */
void image_fetch_cancel_all(void)
{
	GHashTableIter iter;
	gpointer key, value;

	cancel_retries(NULL);
	if (active_fetches == NULL)
		return;

	g_hash_table_iter_init(&iter, active_fetches);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		IMAGE_FETCH_REC *fetch = value;
		fetch->cancelled = TRUE;
	}
}

/* Initialize fetch system */
void image_fetch_init(void)
{
	/* Initialize curl */
	curl_global_init(CURL_GLOBAL_DEFAULT);

	/* Create multi handle */
	curl_multi = curl_multi_init();
	if (curl_multi == NULL) {
		g_warning("image-fetch: curl_multi_init failed");
		return;
	}

	/* Configure multi handle for better reliability */
	/* Limit concurrent connections to prevent overload */
	curl_multi_setopt(curl_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 4L);
	/* Enable HTTP/2 multiplexing */
	curl_multi_setopt(curl_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

	curl_watches = g_hash_table_new(g_direct_hash, g_direct_equal);
	curl_multi_setopt(curl_multi, CURLMOPT_SOCKETFUNCTION, curl_socket_cb);
	curl_multi_setopt(curl_multi, CURLMOPT_TIMERFUNCTION, curl_timer_cb);

	/* Create active fetches hash table */
	active_fetches = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       NULL, (GDestroyNotify)fetch_rec_free);

	image_preview_debug_print("FETCH: curl_multi initialized with HTTP/2 multiplexing");
}

/* Deinitialize fetch system */
void image_fetch_deinit(void)
{
	/* the retries still waiting: their lines and windows go away */
	cancel_retries(NULL);

	/* Clear active fetches (removing their handles tells curl_socket_cb()
	 * which sockets are no longer watched) */
	if (active_fetches != NULL) {
		g_hash_table_destroy(active_fetches);
		active_fetches = NULL;
	}

	/* no more callbacks, then the watches and the timer go */
	if (curl_multi != NULL) {
		curl_multi_setopt(curl_multi, CURLMOPT_SOCKETFUNCTION, NULL);
		curl_multi_setopt(curl_multi, CURLMOPT_TIMERFUNCTION, NULL);
	}
	if (curl_watches != NULL) {
		GHashTableIter iter;
		gpointer key, tag;

		g_hash_table_iter_init(&iter, curl_watches);
		while (g_hash_table_iter_next(&iter, &key, &tag))
			g_source_remove(GPOINTER_TO_UINT(tag));
		g_hash_table_destroy(curl_watches);
		curl_watches = NULL;
	}
	if (curl_timeout_tag != 0) {
		g_source_remove(curl_timeout_tag);
		curl_timeout_tag = 0;
	}
	curl_running = 0;

	/* Cleanup curl multi */
	if (curl_multi != NULL) {
		curl_multi_cleanup(curl_multi);
		curl_multi = NULL;
	}

	/* Global curl cleanup */
	curl_global_cleanup();
}
