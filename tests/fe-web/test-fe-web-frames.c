/*
 test-fe-web-frames.c : WebSocket frames from fe-web clients

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 RFC 6455 5.1: a client masks every frame it sends, and a server closes
 the connection on a frame that is not masked. fe-web answers such a frame
 with a close frame, status 1002 (protocol error), and handles nothing of
 it or after it: no command, no pong, no echo of a close status.
*/

#include "fe-web-test.h"

#include <irssi/src/fe-web/fe-web-crypto.h>

#include <string.h>
#include <sys/ioctl.h>

#define CHUNK 300007

static const guchar refused[] = { 0x88, 0x02, 0x03, 0xEA }; /* close, 1002 */
static const char json_ping[] = "{\"type\":\"ping\",\"id\":\"1\"}";

/* Does data hold the bytes 'what' anywhere */
static gboolean contains(const GByteArray *data, const void *what, gsize len)
{
	gsize i;

	for (i = 0; i + len <= data->len; i++) {
		if (memcmp(data->data + i, what, len) == 0)
			return TRUE;
	}
	return FALSE;
}

/* Send one frame, as one read */
static void send_frame(WEB_CLIENT_REC *c, int opcode, const void *payload, gsize len,
                       gboolean masked)
{
	GByteArray *f = test_frame(opcode, payload, len, masked);

	fe_web_fuzz_client_send(c, f->data, f->len);
	g_byte_array_unref(f);
}

/* The client got exactly the 1002 close and is gone */
static void assert_refused(WEB_CLIENT_REC *c)
{
	g_assert_false(fe_web_fuzz_client_alive(c));
	g_assert_cmpuint(fe_web_fuzz_received()->len, ==, sizeof(refused));
	g_assert_cmpmem(fe_web_fuzz_received()->data, sizeof(refused), refused,
	                sizeof(refused));
}

static void test_unmasked_text_closes_1002(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	send_frame(c, WS_OPCODE_TEXT, json_ping, strlen(json_ping), FALSE);
	assert_refused(c);
	fe_web_fuzz_client_free(c);
}

static void test_unmasked_binary_closes_1002(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	guchar data[40];

	memset(data, 0x5a, sizeof(data));
	send_frame(c, WS_OPCODE_BINARY, data, sizeof(data), FALSE);
	assert_refused(c);
	fe_web_fuzz_client_free(c);
}

static void test_unmasked_ping_closes(void)
{
	static const guchar pong_head = 0x8A;
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	send_frame(c, WS_OPCODE_PING, "abc", 3, FALSE);
	g_assert_false(contains(fe_web_fuzz_received(), &pong_head, 1));
	assert_refused(c);
	fe_web_fuzz_client_free(c);
}

static void test_unmasked_pong_closes(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	send_frame(c, WS_OPCODE_PONG, "x", 1, FALSE);
	assert_refused(c);
	fe_web_fuzz_client_free(c);
}

/* the client's status (1000) is not echoed: 1002 */
static void test_unmasked_close_closes(void)
{
	static const guchar status[2] = { 0x03, 0xE8 };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	send_frame(c, WS_OPCODE_CLOSE, status, 2, FALSE);
	assert_refused(c);
	fe_web_fuzz_client_free(c);
}

/* a masked first fragment, then an unmasked continuation */
static void test_unmasked_continuation_closes(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	GByteArray *f = test_frame(WS_OPCODE_TEXT, "{\"type\":", 8, TRUE);
	GByteArray *cont = test_frame(WS_OPCODE_CONTINUATION, "\"ping\"}", 7, FALSE);
	const GByteArray *got;

	f->data[0] &= 0x7f; /* no FIN */
	g_byte_array_append(f, cont->data, cont->len);
	fe_web_fuzz_client_send(c, f->data, f->len);
	/* what the first fragment got is not the point here: the last
	 * thing sent is the 1002 close */
	got = fe_web_fuzz_received();
	g_assert_false(fe_web_fuzz_client_alive(c));
	g_assert_cmpuint(got->len, >=, sizeof(refused));
	g_assert_cmpmem(got->data + got->len - sizeof(refused), sizeof(refused), refused,
	                sizeof(refused));
	g_byte_array_unref(cont);
	g_byte_array_unref(f);
	fe_web_fuzz_client_free(c);
}

/* the header is enough: no waiting for (and buffering of) the payload */
static void test_unmasked_header_alone_closes(void)
{
	static const guchar head[] = { 0x81, 126, 0x10, 0x00 }; /* 4096 bytes follow */
	static const guchar huge[] = { 0x81, 127, 0x80, 0, 0, 0, 0, 0, 0, 0 };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	fe_web_fuzz_client_send(c, head, sizeof(head));
	assert_refused(c);
	fe_web_fuzz_client_free(c);

	/* and a length over FE_WEB_MAX_FRAME, which is refused anyway, gets
	 * the close frame too */
	c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	fe_web_fuzz_client_send(c, huge, sizeof(huge));
	assert_refused(c);
	fe_web_fuzz_client_free(c);
}

/* the shortest frame there is: two bytes, an empty ping */
static void test_unmasked_empty_ping_closes(void)
{
	static const guchar empty_ping[] = { 0x89, 0x00 };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	fe_web_fuzz_client_send(c, empty_ping, sizeof(empty_ping));
	assert_refused(c);
	fe_web_fuzz_client_free(c);
}

/* positive: a masked ping cut after its first byte is answered once the
 * rest comes: one byte says nothing about the mask. After a real
 * handshake, so the input buffer still holds the request's bytes (byte 2
 * 'E', no mask bit) past what was read. */
static void test_masked_ping_split_pongs(void)
{
	static const guchar pong[] = { 0x8A, 0x03, 'a', 'b', 'c' };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", FALSE, FALSE);
	GByteArray *ping = test_frame(WS_OPCODE_PING, "abc", 3, TRUE);
	char *request = test_request(FE_WEB_FUZZ_PASSWORD);
	const GByteArray *got;
	gsize before;

	fe_web_fuzz_client_send(c, (const guchar *) request, strlen(request));
	g_assert_true(c->handshake_done);
	before = fe_web_fuzz_received()->len;

	fe_web_fuzz_client_send(c, ping->data, 1);
	g_assert_true(fe_web_fuzz_client_alive(c));
	fe_web_fuzz_client_send(c, ping->data + 1, ping->len - 1);
	g_assert_true(fe_web_fuzz_client_alive(c));
	got = fe_web_fuzz_received();
	g_assert_cmpuint(got->len, ==, before + sizeof(pong));
	g_assert_cmpmem(got->data + before, sizeof(pong), pong, sizeof(pong));
	g_free(request);
	g_byte_array_unref(ping);
	fe_web_fuzz_client_free(c);
}

/* positive: a masked ping is answered with its data, unmasked */
static void test_masked_ping_pongs(void)
{
	static const guchar pong[] = { 0x8A, 0x03, 'a', 'b', 'c' };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	send_frame(c, WS_OPCODE_PING, "abc", 3, TRUE);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_cmpuint(fe_web_fuzz_received()->len, ==, sizeof(pong));
	g_assert_cmpmem(fe_web_fuzz_received()->data, sizeof(pong), pong, sizeof(pong));
	fe_web_fuzz_client_free(c);
}

/* positive: a masked ping with more data than the 4 bytes of its mask
 * key is echoed exactly */
static void test_masked_ping_long_pongs(void)
{
	static const guchar pong[] = { 0x8A, 0x09, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i' };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	send_frame(c, WS_OPCODE_PING, "abcdefghi", 9, TRUE);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_cmpuint(fe_web_fuzz_received()->len, ==, sizeof(pong));
	g_assert_cmpmem(fe_web_fuzz_received()->data, sizeof(pong), pong, sizeof(pong));
	fe_web_fuzz_client_free(c);
}

/* The JSON of the first (encrypted, binary) frame fe-web sent; to be
 * freed */
static char *first_reply(void)
{
	const GByteArray *got = fe_web_fuzz_received();
	int fin, opcode, masked, plain_len = 0;
	guint64 payload_len;
	guchar mask_key[4];
	const guchar *payload;
	guchar *plain;
	char *json;

	g_assert_cmpint(fe_web_websocket_parse_frame(got->data, got->len, &fin, &opcode,
	                                             &masked, &payload_len, mask_key,
	                                             &payload),
	                ==, 1);
	g_assert_cmpint(opcode, ==, WS_OPCODE_BINARY);
	g_assert_false(masked);
	plain = g_malloc(payload_len + 1);
	g_assert_true(fe_web_crypto_decrypt(payload, payload_len, fe_web_crypto_get_key(),
	                                    plain, &plain_len));
	json = g_strndup((const char *) plain, plain_len);
	g_free(plain);
	return json;
}

/* A masked JSON ping whose id is 'id_len' bytes is answered with a pong
 * for that id: the whole payload was unmasked */
static void assert_long_ping_answered(gsize id_len)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	GString *msg = g_string_new("{\"type\":\"ping\",\"id\":\"");
	char *id, *want, *json;
	gsize i;

	for (i = 0; i < id_len; i++)
		g_string_append_c(msg, 'a' + i % 26);
	id = g_strndup(msg->str + strlen("{\"type\":\"ping\",\"id\":\""), id_len);
	g_string_append(msg, "\"}");

	send_frame(c, WS_OPCODE_TEXT, msg->str, msg->len, TRUE);
	g_assert_true(fe_web_fuzz_client_alive(c));
	json = first_reply();
	g_assert_nonnull(strstr(json, "\"type\":\"pong\""));
	want = g_strdup_printf("\"response_to\":\"%s\"", id);
	g_assert_nonnull(strstr(json, want));

	g_free(want);
	g_free(json);
	g_free(id);
	g_string_free(msg, TRUE);
	fe_web_fuzz_client_free(c);
}

/* the 16-bit length form (126 to 65535 bytes) */
static void test_masked_text_len16_answered(void)
{
	assert_long_ping_answered(300);
}

/* the 64-bit length form (65536 bytes and more) */
static void test_masked_text_len64_answered(void)
{
	assert_long_ping_answered(70000);
}

/* positive: a masked JSON ping is answered (a binary frame: the
 * harness's clients have encryption on, as fe-web's own) */
static void test_masked_text_answered(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	send_frame(c, WS_OPCODE_TEXT, json_ping, strlen(json_ping), TRUE);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_cmpuint(fe_web_fuzz_received()->len, >, 2);
	g_assert_true(test_starts_with(fe_web_fuzz_received(), "\x82"));
	fe_web_fuzz_client_free(c);
}

/* One read holds an unmasked frame and then masked ones;
 * nothing after the refusal is handled (no pong, no answer) */
static void test_nothing_after_refusal(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	GByteArray *all = test_frame(WS_OPCODE_TEXT, json_ping, strlen(json_ping), FALSE);
	GByteArray *ping = test_frame(WS_OPCODE_PING, "abc", 3, TRUE);
	GByteArray *text = test_frame(WS_OPCODE_TEXT, json_ping, strlen(json_ping), TRUE);
	GByteArray *close_frame = test_frame(WS_OPCODE_CLOSE, "\x03\xe8", 2, TRUE);

	g_byte_array_append(all, ping->data, ping->len);
	g_byte_array_append(all, text->data, text->len);
	g_byte_array_append(all, close_frame->data, close_frame->len);
	fe_web_fuzz_client_send(c, all->data, all->len);
	assert_refused(c);

	g_byte_array_unref(close_frame);
	g_byte_array_unref(text);
	g_byte_array_unref(ping);
	g_byte_array_unref(all);
	fe_web_fuzz_client_free(c);
}

/* Send chunks to a client whose browser does not read, until some are
 * queued: the socket is full then. Returns what was sent. */
static gsize fill_socket(WEB_CLIENT_REC *c)
{
	guchar *chunk = g_malloc(CHUNK);
	gsize sent = 0;
	int n;

	memset(chunk, 'x', CHUNK);
	for (n = 0; n < 64 && test_pending(c) == 0; n++) {
		g_assert_true(fe_web_client_send_raw(c, chunk, CHUNK));
		sent += CHUNK;
	}
	g_assert_cmpuint(test_pending(c), >, 0);
	g_free(chunk);
	return sent;
}

/* With output queued, the 1002 close goes out after it; what the browser
 * sends meanwhile is not read, and no second close frame follows */
static void test_refusal_after_queued_output(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	GByteArray *bad = test_frame(WS_OPCODE_TEXT, json_ping, strlen(json_ping), FALSE);
	GByteArray *ping = test_frame(WS_OPCODE_PING, "abc", 3, TRUE);
	const GByteArray *got;
	gsize sent, buffered;
	int rounds, unread = 0;

	sent = fill_socket(c);
	fe_web_fuzz_peer_write(bad->data, bad->len);
	fe_web_fuzz_client_input(c);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_true(c->closing);

	/* more frames, a masked ping and the unmasked one again: not read
	 * at all (the closing guard of client_input_once()), they stay in
	 * the socket and the input buffer keeps what it had */
	buffered = c->input_buffer->len;
	fe_web_fuzz_peer_write(ping->data, ping->len);
	fe_web_fuzz_peer_write(bad->data, bad->len);
	fe_web_fuzz_client_input(c);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_cmpint(c->recv_tag, ==, -1);
	g_assert_cmpuint(c->input_buffer->len, ==, buffered);
	g_assert_cmpint(ioctl(c->fd, FIONREAD, &unread), ==, 0);
	g_assert_cmpint(unread, ==, (int) (ping->len + bad->len));

	for (rounds = 0; rounds < 100000 && fe_web_fuzz_client_alive(c); rounds++) {
		fe_web_fuzz_read_peer(0);
		fe_web_fuzz_client_writable(c);
	}
	g_assert_false(fe_web_fuzz_client_alive(c));
	fe_web_fuzz_read_peer(0);

	got = fe_web_fuzz_received();
	g_assert_cmpuint(got->len, ==, sent + sizeof(refused));
	g_assert_cmpmem(got->data + sent, sizeof(refused), refused, sizeof(refused));

	g_byte_array_unref(ping);
	g_byte_array_unref(bad);
	fe_web_fuzz_client_free(c);
}

/* Before the handshake, frames are not parsed: the bytes are an HTTP
 * request (an incomplete one here), not a frame to refuse */
static void test_no_frames_before_handshake(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", FALSE, FALSE);

	send_frame(c, WS_OPCODE_TEXT, json_ping, strlen(json_ping), FALSE);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_false(c->handshake_done);
	g_assert_cmpuint(fe_web_fuzz_received()->len, ==, 0);
	fe_web_fuzz_client_free(c);
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);

	g_test_add_func("/fe-web/frames/unmasked-text-closes-1002",
	                test_unmasked_text_closes_1002);
	g_test_add_func("/fe-web/frames/unmasked-binary-closes-1002",
	                test_unmasked_binary_closes_1002);
	g_test_add_func("/fe-web/frames/unmasked-ping-closes", test_unmasked_ping_closes);
	g_test_add_func("/fe-web/frames/unmasked-pong-closes", test_unmasked_pong_closes);
	g_test_add_func("/fe-web/frames/unmasked-close-closes", test_unmasked_close_closes);
	g_test_add_func("/fe-web/frames/unmasked-continuation-closes",
	                test_unmasked_continuation_closes);
	g_test_add_func("/fe-web/frames/unmasked-header-alone-closes",
	                test_unmasked_header_alone_closes);
	g_test_add_func("/fe-web/frames/unmasked-empty-ping-closes",
	                test_unmasked_empty_ping_closes);
	g_test_add_func("/fe-web/frames/masked-ping-pongs", test_masked_ping_pongs);
	g_test_add_func("/fe-web/frames/masked-ping-split-pongs", test_masked_ping_split_pongs);
	g_test_add_func("/fe-web/frames/masked-ping-long-pongs", test_masked_ping_long_pongs);
	g_test_add_func("/fe-web/frames/masked-text-answered", test_masked_text_answered);
	g_test_add_func("/fe-web/frames/masked-text-len16-answered",
	                test_masked_text_len16_answered);
	g_test_add_func("/fe-web/frames/masked-text-len64-answered",
	                test_masked_text_len64_answered);
	g_test_add_func("/fe-web/frames/nothing-after-refusal", test_nothing_after_refusal);
	g_test_add_func("/fe-web/frames/refusal-after-queued-output",
	                test_refusal_after_queued_output);
	g_test_add_func("/fe-web/frames/no-frames-before-handshake",
	                test_no_frames_before_handshake);

	return g_test_run();
}
