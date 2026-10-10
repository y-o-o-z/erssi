/*
 fe-web-test.c : helpers of the fe-web unit tests

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "fe-web-test.h"

#include <irssi/src/core/settings.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

/* Warnings, criticals and errors go to stderr (meson shows it for a failed
 * test; stdout is the TAP stream); the fatal mask still applies after the
 * handler. Messages, info and debug are dropped, as the fuzz harness does. */
static void test_log_handler(const gchar *domain, GLogLevelFlags level, const gchar *message,
                             gpointer data)
{
	const char *name;

	(void) data;
	if (level & G_LOG_LEVEL_ERROR)
		name = "ERROR";
	else if (level & G_LOG_LEVEL_CRITICAL)
		name = "CRITICAL";
	else if (level & G_LOG_LEVEL_WARNING)
		name = "WARNING";
	else
		return;
	g_printerr("%s%s%s: %s\n", domain != NULL ? domain : "", domain != NULL ? "-" : "", name,
	           message);
}

void test_init(int *argc, char ***argv)
{
	g_test_init(argc, argv, NULL);
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
	fe_web_fuzz_init(FALSE);
	/* after fe_web_fuzz_init(), which drops everything */
	g_log_set_default_handler(test_log_handler, NULL);
}

GByteArray *test_frame(int opcode, const void *payload, gsize len, gboolean masked)
{
	static const guchar mask[4] = { 0x11, 0x22, 0x33, 0x44 };
	const guchar *p = payload;
	GByteArray *frame;
	guchar head[14];
	gsize head_len = 2, i;

	head[0] = 0x80 | (opcode & 0x0f);
	if (len < 126) {
		head[1] = len;
	} else if (len <= 0xffff) {
		head[1] = 126;
		head[2] = len >> 8;
		head[3] = len & 0xff;
		head_len = 4;
	} else {
		head[1] = 127;
		for (i = 0; i < 8; i++)
			head[2 + i] = ((guint64) len >> (56 - 8 * i)) & 0xff;
		head_len = 10;
	}
	if (masked) {
		head[1] |= 0x80;
		memcpy(head + head_len, mask, 4);
		head_len += 4;
	}

	frame = g_byte_array_sized_new(head_len + len);
	g_byte_array_append(frame, head, head_len);
	for (i = 0; i < len; i++) {
		guchar c = masked ? p[i] ^ mask[i % 4] : p[i];

		g_byte_array_append(frame, &c, 1);
	}
	return frame;
}

char *test_request(const char *password)
{
	GString *req = g_string_new("GET / HTTP/1.1\r\n"
	                            "Host: 127.0.0.1:9001\r\n"
	                            "Upgrade: websocket\r\n"
	                            "Connection: Upgrade\r\n"
	                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
	                            "Sec-WebSocket-Version: 13\r\n");

	if (password != NULL)
		g_string_append_printf(req, "Authorization: Bearer %s\r\n", password);
	g_string_append(req, "\r\n");
	return g_string_free(req, FALSE);
}

gboolean test_starts_with(const GByteArray *data, const char *prefix)
{
	gsize len = strlen(prefix);

	return data->len >= len && memcmp(data->data, prefix, len) == 0;
}

gsize test_pending(WEB_CLIENT_REC *client)
{
	return client->output_buffer->len - client->output_pos;
}

char *test_tmpdir(void)
{
	const char *base = g_get_tmp_dir();
	char *tmpl, *dir;

	if (strlen(base) >= 40)
		base = "/tmp";
	tmpl = g_build_filename(base, "fws-XXXXXX", NULL);
	dir = g_mkdtemp_full(tmpl, 0700);
	g_assert_nonnull(dir);
	return dir;
}

void test_rmtree(const char *path)
{
	struct stat st;

	if (lstat(path, &st) < 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		GDir *dir;
		const char *name;

		/* a mode that lets us in */
		chmod(path, 0700);
		dir = g_dir_open(path, 0, NULL);
		if (dir != NULL) {
			while ((name = g_dir_read_name(dir)) != NULL) {
				char *child = g_build_filename(path, name, NULL);

				test_rmtree(child);
				g_free(child);
			}
			g_dir_close(dir);
		}
		rmdir(path);
	} else {
		unlink(path);
	}
}

static void unix_addr(struct sockaddr_un *sa, const char *path)
{
	g_assert_cmpuint(strlen(path), <, sizeof(sa->sun_path));
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	memcpy(sa->sun_path, path, strlen(path));
}

int test_connect_unix(const char *path)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	g_assert_cmpint(fd, >=, 0);
	unix_addr(&sa, path);
	if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	g_assert_cmpint(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK), ==, 0);
	return fd;
}

int test_listening_socket(const char *path)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	g_assert_cmpint(fd, >=, 0);
	unix_addr(&sa, path);
	g_assert_cmpint(bind(fd, (struct sockaddr *) &sa, sizeof(sa)), ==, 0);
	g_assert_cmpint(listen(fd, 5), ==, 0);
	return fd;
}

void test_dead_socket(const char *path)
{
	close(test_listening_socket(path));
}

char *test_start_unix(const char *path)
{
	settings_set_str("fe_web_socket", path);
	fe_web_server_init();
	return fe_web_fuzz_take_printed();
}
