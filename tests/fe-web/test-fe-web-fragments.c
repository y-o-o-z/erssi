/*
 test-fe-web-fragments.c : fragmented messages and the frames RFC 6455
 refuses

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A client may send a message in several frames (5.4): a text or binary
 frame without FIN, continuation frames, the last with FIN; control frames
 may come between them. fe-web puts the message together (at most
 FE_WEB_MAX_FRAME bytes in FE_WEB_MAX_FRAGMENTS frames) and handles it as
 one. It fails the connection with a close frame of status
 - 1002 (protocol error) for reserved bits set (no extension is ever
   agreed), an opcode with no meaning, a continuation of no message, a new
   message while a fragmented one is open, and a control frame that is
   too long or fragmented;
 - 1007 for a text message that is not UTF-8 (8.1), also when the bad
   sequence spans two fragments (a character split between two fragments
   is fine), and for a close frame whose reason is not UTF-8 (5.5.1);
 - 1009 for a frame or message that is too big, or too many fragments.
 Nothing after the refused frame is handled.
*/

#include "fe-web-test.h"

#include <irssi/src/fe-web/fe-web-crypto.h>

#include <string.h>

static const guchar close_1002[] = { 0x88, 0x02, 0x03, 0xEA };
static const guchar close_1007[] = { 0x88, 0x02, 0x03, 0xEF };
static const guchar close_1009[] = { 0x88, 0x02, 0x03, 0xF1 };

/* A masked frame with this first byte (FIN, RSV and opcode); appended to
 * 'to' */
static void add_frame(GByteArray *to, guchar first, const void *payload, gsize len)
{
	GByteArray *f = test_frame(first & 0x0f, payload, len, TRUE);

	f->data[0] = first;
	g_byte_array_append(to, f->data, f->len);
	g_byte_array_unref(f);
}

static void add_text(GByteArray *to, guchar first, const char *text)
{
	add_frame(to, first, text, strlen(text));
}

/* a logged-in client whose answers are text frames, readable here */
static WEB_CLIENT_REC *plain_client(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	c->encryption_enabled = FALSE;
	return c;
}

static void send_all(WEB_CLIENT_REC *c, GByteArray *data)
{
	fe_web_fuzz_client_send(c, data->data, data->len);
	g_byte_array_unref(data);
}

/* Did the browser get text 'needle' (frame headers may hold NUL bytes) */
static gboolean heard(const char *needle)
{
	const GByteArray *r = fe_web_fuzz_received();
	gsize len = strlen(needle), i;

	for (i = 0; i + len <= r->len; i++) {
		if (memcmp(r->data + i, needle, len) == 0)
			return TRUE;
	}
	return FALSE;
}

/* The client got exactly this close frame and nothing else, and is gone */
static void assert_failed(WEB_CLIENT_REC *c, const guchar *close_frame)
{
	const GByteArray *r = fe_web_fuzz_received();

	g_assert_false(fe_web_fuzz_client_alive(c));
	g_assert_cmpmem(r->data, r->len, close_frame, 4);
}

/* Send 'data' (freed) to a fresh client: it is failed with 'close_frame'
 * and nothing of what was sent is answered */
static void assert_refused(GByteArray *data, const guchar *close_frame)
{
	WEB_CLIENT_REC *c = plain_client();

	send_all(c, data);
	assert_failed(c, close_frame);
	fe_web_fuzz_client_free(c);
}

/* ---- fragmented messages ---- */

static void test_text_in_three_fragments(void)
{
	WEB_CLIENT_REC *c = plain_client();
	GByteArray *data = g_byte_array_new();

	add_text(data, 0x01, "{\"type\":\"pi");
	add_text(data, 0x00, "ng\",\"id\":\"fr");
	add_text(data, 0x80, "ag\"}");
	send_all(c, data);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_true(heard("\"type\":\"pong\""));
	g_assert_true(heard("\"response_to\":\"frag\""));
	g_assert_null(c->fragments);
	fe_web_fuzz_client_free(c);
}

/* each fragment in a read of its own, cut inside its header and payload */
static void test_fragments_over_reads(void)
{
	WEB_CLIENT_REC *c = plain_client();
	GByteArray *data = g_byte_array_new();
	gsize i;

	add_text(data, 0x01, "{\"type\":\"ping\",");
	add_text(data, 0x80, "\"id\":\"reads\"}");
	for (i = 0; i < data->len; i += 3) {
		fe_web_fuzz_client_send(c, data->data + i, MIN(3, data->len - i));
		g_assert_true(fe_web_fuzz_client_alive(c));
	}
	g_byte_array_unref(data);
	g_assert_true(heard("\"response_to\":\"reads\""));
	fe_web_fuzz_client_free(c);
}

/* control frames between the fragments are handled at once */
static void test_ping_between_fragments(void)
{
	static const guchar pong[] = { 0x8A, 0x02, 'h', 'i' };
	WEB_CLIENT_REC *c = plain_client();
	GByteArray *data = g_byte_array_new();
	const GByteArray *r;

	add_text(data, 0x01, "{\"type\":\"ping\",");
	add_text(data, 0x89, "hi");
	add_frame(data, 0x8A, "", 0); /* an unasked pong: nothing */
	add_text(data, 0x80, "\"id\":\"between\"}");
	send_all(c, data);
	g_assert_true(fe_web_fuzz_client_alive(c));
	r = fe_web_fuzz_received();
	/* the pong first, then the answer to the whole message */
	g_assert_cmpuint(r->len, >, sizeof(pong));
	g_assert_cmpmem(r->data, sizeof(pong), pong, sizeof(pong));
	g_assert_true(heard("\"response_to\":\"between\""));
	fe_web_fuzz_client_free(c);
}

/* an encrypted message in fragments: decrypted once it is whole */
static void test_binary_in_fragments(void)
{
	static const char json[] = "{\"type\":\"ping\",\"id\":\"sealed\"}";
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	GByteArray *data = g_byte_array_new();
	guchar sealed[sizeof(json) + 64];
	int sealed_len = 0;

	g_assert_true(fe_web_crypto_encrypt((const guchar *) json, strlen(json),
	                                    fe_web_crypto_get_key(), sealed, &sealed_len));
	add_frame(data, 0x02, sealed, 10);
	add_frame(data, 0x00, sealed + 10, 10);
	add_frame(data, 0x80, sealed + 20, sealed_len - 20);
	send_all(c, data);
	g_assert_true(fe_web_fuzz_client_alive(c));
	/* answered with an encrypted frame */
	g_assert_true(test_starts_with(fe_web_fuzz_received(), "\x82"));
	fe_web_fuzz_client_free(c);
}

/* the most fragments a message may have, most of them empty */
static void test_most_fragments(void)
{
	WEB_CLIENT_REC *c = plain_client();
	GByteArray *data = g_byte_array_new();
	guint i;

	add_text(data, 0x01, "{\"type\":\"ping\",\"id\":\"many\"");
	for (i = 2; i < FE_WEB_MAX_FRAGMENTS; i++)
		add_frame(data, 0x00, "", 0);
	add_text(data, 0x80, "}");
	send_all(c, data);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_true(heard("\"response_to\":\"many\""));
	fe_web_fuzz_client_free(c);
}

/* an empty message in two fragments, then one with an empty last
 * fragment */
static void test_empty_fragments(void)
{
	WEB_CLIENT_REC *c = plain_client();
	GByteArray *data = g_byte_array_new();

	add_frame(data, 0x01, "", 0);
	add_frame(data, 0x80, "", 0);
	add_text(data, 0x01, "{\"type\":\"ping\",");
	add_text(data, 0x00, "\"id\":\"after\"}");
	add_frame(data, 0x80, "", 0);
	send_all(c, data);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_true(heard("\"response_to\":\"after\""));
	fe_web_fuzz_client_free(c);
}

/* ---- UTF-8 ---- */

/* "ż" is C5 BC: cut between the two fragments */
static void test_utf8_split_between_fragments(void)
{
	WEB_CLIENT_REC *c = plain_client();
	GByteArray *data = g_byte_array_new();

	add_text(data, 0x01, "{\"type\":\"ping\",\"id\":\"\xc5");
	add_text(data, 0x80, "\xbc\"}");
	send_all(c, data);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_true(heard("\"response_to\":\"\xc5\xbc\""));
	fe_web_fuzz_client_free(c);
}

static void test_invalid_utf8_closes_1007(void)
{
	static const char *const bad[] = {
		"{\"type\":\"ping\",\"id\":\"\xff\"}",
		"{\"type\":\"ping\",\"id\":\"\xc5\"}",          /* cut short */
		"{\"type\":\"ping\",\"id\":\"\xc0\xaf\"}",      /* overlong '/' */
		"{\"type\":\"ping\",\"id\":\"\xed\xa0\x80\"}",  /* a surrogate */
		"{\"type\":\"ping\",\"id\":\"\xf4\x90\x80\x80\"}", /* past U+10FFFF */
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		GByteArray *data = g_byte_array_new();

		add_text(data, 0x81, bad[i]);
		/* and nothing after it */
		add_text(data, 0x81, "{\"type\":\"ping\",\"id\":\"next\"}");
		assert_refused(data, close_1007);
	}
}

/* a bad sequence that starts in one fragment and ends in the next */
static void test_invalid_utf8_across_fragments_closes_1007(void)
{
	GByteArray *data = g_byte_array_new();

	add_text(data, 0x01, "{\"type\":\"ping\",\"id\":\"\xc5");
	add_text(data, 0x80, "x\"}");
	assert_refused(data, close_1007);
}

/* the reason of a close frame is UTF-8 too (5.5.1) */
static void test_close_reason_utf8(void)
{
	static const guchar normal[] = { 0x88, 0x02, 0x03, 0xE8 };
	GByteArray *data = g_byte_array_new();

	add_frame(data, 0x88, "\x03\xe8" "bye \xff", 7);
	assert_refused(data, close_1007);

	/* a valid one: the status is echoed */
	data = g_byte_array_new();
	add_frame(data, 0x88, "\x03\xe8" "bye \xc5\xbc", 8);
	assert_refused(data, normal);
}

/* U+0000 is valid UTF-8: not refused as invalid data */
static void test_nul_is_utf8(void)
{
	static const char json[] = "{\"type\":\"ping\",\"id\":\"nul\"}\0x";
	WEB_CLIENT_REC *c = plain_client();
	GByteArray *data = g_byte_array_new();

	add_frame(data, 0x81, json, sizeof(json) - 1);
	send_all(c, data);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_true(heard("\"response_to\":\"nul\""));
	fe_web_fuzz_client_free(c);
}

/* binary messages are not text: no UTF-8 check (here: refused for not
 * decrypting, without a 1007) */
static void test_binary_not_checked_as_utf8(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	GByteArray *data = g_byte_array_new();
	guchar junk[40];

	memset(junk, 0xff, sizeof(junk));
	add_frame(data, 0x82, junk, sizeof(junk));
	send_all(c, data);
	g_assert_false(fe_web_fuzz_client_alive(c));
	g_assert_cmpuint(fe_web_fuzz_received()->len, ==, 0);
	fe_web_fuzz_client_free(c);
}

/* ---- protocol errors ---- */

static void test_reserved_bits_close_1002(void)
{
	static const guchar rsv[] = { 0x40, 0x20, 0x10 };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(rsv); i++) {
		GByteArray *data = g_byte_array_new();

		add_text(data, 0x81 | rsv[i], "{\"type\":\"ping\",\"id\":\"1\"}");
		assert_refused(data, close_1002);

		data = g_byte_array_new();
		add_text(data, 0x89 | rsv[i], "hi");
		assert_refused(data, close_1002);

		/* on a continuation */
		data = g_byte_array_new();
		add_text(data, 0x01, "{\"type\":\"ping\",");
		add_text(data, 0x80 | rsv[i], "\"id\":\"1\"}");
		assert_refused(data, close_1002);
	}
}

static void test_unknown_opcodes_close_1002(void)
{
	static const int opcodes[] = { 3, 4, 5, 6, 7, 0xB, 0xC, 0xD, 0xE, 0xF };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(opcodes); i++) {
		GByteArray *data = g_byte_array_new();

		add_text(data, 0x80 | opcodes[i], "x");
		add_text(data, 0x81, "{\"type\":\"ping\",\"id\":\"1\"}");
		assert_refused(data, close_1002);
	}
}

static void test_continuation_without_start_closes_1002(void)
{
	GByteArray *data = g_byte_array_new();

	add_text(data, 0x80, "{\"type\":\"ping\",\"id\":\"1\"}");
	assert_refused(data, close_1002);

	/* nor after a message that is complete */
	data = g_byte_array_new();
	add_text(data, 0x81, "{\"type\":\"ping\",\"id\":\"1\"}");
	add_text(data, 0x00, "x");
	{
		WEB_CLIENT_REC *c = plain_client();
		const GByteArray *r;

		send_all(c, data);
		g_assert_false(fe_web_fuzz_client_alive(c));
		r = fe_web_fuzz_received();
		/* the ping was answered, then the close */
		g_assert_true(heard("\"type\":\"pong\""));
		g_assert_cmpmem(r->data + r->len - 4, 4, close_1002, 4);
		fe_web_fuzz_client_free(c);
	}
}

static void test_new_message_inside_fragmented_closes_1002(void)
{
	static const guchar starts[] = { 0x81, 0x01, 0x82, 0x02 };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(starts); i++) {
		GByteArray *data = g_byte_array_new();

		add_text(data, 0x01, "{\"type\":\"ping\",");
		add_text(data, starts[i], "{\"type\":\"ping\",\"id\":\"1\"}");
		assert_refused(data, close_1002);
	}
}

/* control frames: at most 125 bytes, never fragmented */
static void test_bad_control_frames_close_1002(void)
{
	char big[126];
	GByteArray *data;

	memset(big, 'x', sizeof(big));
	data = g_byte_array_new();
	add_frame(data, 0x89, big, sizeof(big));
	assert_refused(data, close_1002);

	data = g_byte_array_new();
	add_text(data, 0x09, "hi");
	assert_refused(data, close_1002);

	data = g_byte_array_new();
	add_frame(data, 0x08, "\x03\xe8", 2);
	assert_refused(data, close_1002);
}

/* ---- too big ---- */

/* on the header: the payload is not waited for */
static void test_frame_too_big_closes_1009(void)
{
	GByteArray *data = g_byte_array_new();
	guchar head[14] = { 0x81, 0xff };
	guint64 len = (guint64) FE_WEB_MAX_FRAME + 1;
	int i;

	for (i = 0; i < 8; i++)
		head[2 + i] = (len >> (56 - 8 * i)) & 0xff;
	memcpy(head + 10, "\x11\x22\x33\x44", 4);
	g_byte_array_append(data, head, sizeof(head));
	assert_refused(data, close_1009);
}

/* the fragments together over FE_WEB_MAX_FRAME, on the header of the one
 * that would take it over */
static void test_message_too_big_closes_1009(void)
{
	GByteArray *data = g_byte_array_new();
	guchar head[14] = { 0x00, 0xff };
	guint64 len = (guint64) FE_WEB_MAX_FRAME - 15;
	int i;

	add_frame(data, 0x01, "0123456789abcdef", 16);
	for (i = 0; i < 8; i++)
		head[2 + i] = (len >> (56 - 8 * i)) & 0xff;
	memcpy(head + 10, "\x11\x22\x33\x44", 4);
	g_byte_array_append(data, head, sizeof(head));
	assert_refused(data, close_1009);
}

static void test_too_many_fragments_close_1009(void)
{
	GByteArray *data = g_byte_array_new();
	guint i;

	add_text(data, 0x01, "{\"type\":\"ping\",\"id\":\"many\"");
	for (i = 1; i < FE_WEB_MAX_FRAGMENTS; i++)
		add_frame(data, 0x00, "", 0);
	add_text(data, 0x80, "}");
	assert_refused(data, close_1009);
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);

	g_test_add_func("/fe-web/fragments/text-in-three-fragments", test_text_in_three_fragments);
	g_test_add_func("/fe-web/fragments/fragments-over-reads", test_fragments_over_reads);
	g_test_add_func("/fe-web/fragments/ping-between-fragments", test_ping_between_fragments);
	g_test_add_func("/fe-web/fragments/binary-in-fragments", test_binary_in_fragments);
	g_test_add_func("/fe-web/fragments/most-fragments", test_most_fragments);
	g_test_add_func("/fe-web/fragments/empty-fragments", test_empty_fragments);
	g_test_add_func("/fe-web/fragments/utf8-split-between-fragments",
	                test_utf8_split_between_fragments);
	g_test_add_func("/fe-web/fragments/invalid-utf8-closes-1007",
	                test_invalid_utf8_closes_1007);
	g_test_add_func("/fe-web/fragments/invalid-utf8-across-fragments-closes-1007",
	                test_invalid_utf8_across_fragments_closes_1007);
	g_test_add_func("/fe-web/fragments/close-reason-utf8", test_close_reason_utf8);
	g_test_add_func("/fe-web/fragments/nul-is-utf8", test_nul_is_utf8);
	g_test_add_func("/fe-web/fragments/binary-not-checked-as-utf8",
	                test_binary_not_checked_as_utf8);
	g_test_add_func("/fe-web/fragments/reserved-bits-close-1002",
	                test_reserved_bits_close_1002);
	g_test_add_func("/fe-web/fragments/unknown-opcodes-close-1002",
	                test_unknown_opcodes_close_1002);
	g_test_add_func("/fe-web/fragments/continuation-without-start-closes-1002",
	                test_continuation_without_start_closes_1002);
	g_test_add_func("/fe-web/fragments/new-message-inside-fragmented-closes-1002",
	                test_new_message_inside_fragmented_closes_1002);
	g_test_add_func("/fe-web/fragments/bad-control-frames-close-1002",
	                test_bad_control_frames_close_1002);
	g_test_add_func("/fe-web/fragments/frame-too-big-closes-1009",
	                test_frame_too_big_closes_1009);
	g_test_add_func("/fe-web/fragments/message-too-big-closes-1009",
	                test_message_too_big_closes_1009);
	g_test_add_func("/fe-web/fragments/too-many-fragments-close-1009",
	                test_too_many_fragments_close_1009);

	return g_test_run();
}
