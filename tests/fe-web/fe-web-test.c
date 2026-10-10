/*
 fe-web-test.c : helpers of the fe-web unit tests

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "fe-web-test.h"

#include <string.h>

void test_init(int *argc, char ***argv)
{
	g_test_init(argc, argv, NULL);
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
	fe_web_fuzz_init(FALSE);
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
