/*
 test-fe-web-output.c : the output queue of fe-web clients

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 Everything for a web client goes through fe_web_client_send_raw(): what
 the socket (or TLS) does not take is queued, in order, up to 32 MB, and a
 client being closed still gets its last frame.
*/

#include "fe-web-test.h"

#include <string.h>

#define MB (1024 * 1024)
/* not a power of two: the last chunk that fills the socket is usually
 * written only in part */
#define CHUNK 300007
/* copies of the limits in fe-web-server.c */
#define FE_WEB_MAX_OUTPUT (32 * MB)
#define FE_WEB_LINGER_TIMEOUT 5

static guchar A[1000], B[500];

static void fill_patterns(void)
{
	gsize i;

	for (i = 0; i < sizeof(A); i++)
		A[i] = i * 7 % 251;
	for (i = 0; i < sizeof(B); i++)
		B[i] = i * 13 % 251;
}

/* byte 'pos' of what fill_socket() sends: never 0, period 251 */
static guchar fill_byte(gsize pos)
{
	return pos % 251 + 1;
}

/* Send chunks to a client whose browser does not read, until some are
 * queued: the socket is full then. Returns what was sent. */
static gsize fill_socket(WEB_CLIENT_REC *c)
{
	guchar *chunk = g_malloc(CHUNK);
	gsize sent = 0, i;
	int n;

	for (n = 0; n < 64 && test_pending(c) == 0; n++) {
		for (i = 0; i < CHUNK; i++)
			chunk[i] = fill_byte(sent + i);
		g_assert_true(fe_web_client_send_raw(c, chunk, CHUNK));
		sent += CHUNK;
	}
	g_assert_cmpuint(test_pending(c), >, 0);
	g_free(chunk);
	return sent;
}

/* Is data what fill_socket() sent, in order */
static void assert_filled(const guchar *data, gsize len)
{
	gsize i;

	for (i = 0; i < len; i++) {
		if (data[i] != fill_byte(i))
			g_error("byte %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT
			        " is %u, not %u", i, len, data[i], fill_byte(i));
	}
}

/* A masked close frame with status 1000, from the browser */
static void peer_sends_close(WEB_CLIENT_REC *c)
{
	static const guchar status[2] = { 0x03, 0xE8 };
	GByteArray *close_frame = test_frame(WS_OPCODE_CLOSE, status, 2, TRUE);

	fe_web_fuzz_peer_write(close_frame->data, close_frame->len);
	g_byte_array_unref(close_frame);
	fe_web_fuzz_client_input(c);
}

/* A partial TLS write and then a full socket: the rest is queued and goes
 * out before what is sent after it */
static void test_partial_write_keeps_order(void)
{
	static const int script[] = { 100, FE_WEB_SSL_WANT_WRITE };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, TRUE);

	fe_web_fuzz_tls_write_script(script, 2);
	g_assert_true(fe_web_client_send_raw(c, A, 1000));
	g_assert_cmpuint(fe_web_fuzz_read_peer(0), ==, 100);
	g_assert_cmpuint(test_pending(c), ==, 900);
	g_assert_cmpint(c->send_tag, !=, -1);
	g_assert_true(fe_web_client_send_raw(c, B, 500)); /* flushed: script used up */
	fe_web_fuzz_read_peer(0);
	g_assert_cmpuint(fe_web_fuzz_received()->len, ==, 1500);
	g_assert_cmpmem(fe_web_fuzz_received()->data, 1000, A, 1000);
	g_assert_cmpmem(fe_web_fuzz_received()->data + 1000, 500, B, 500);
	g_assert_cmpuint(test_pending(c), ==, 0);
	g_assert_cmpint(c->send_tag, ==, -1);
	fe_web_fuzz_client_free(c);
}

/* TLS has to read before it can write: no writable watch (the socket is
 * writable and the watch would spin); the output goes on after the next
 * read, followed by the answer to what was read */
static void test_want_read_waits_for_input(void)
{
	static const int script[] = { FE_WEB_SSL_WANT_READ };
	static const guchar pong[] = { 0x8A, 0x01, 'x' };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, TRUE);
	GByteArray *ping = test_frame(WS_OPCODE_PING, "x", 1, TRUE);
	const GByteArray *got;

	fe_web_fuzz_tls_write_script(script, 1);
	g_assert_true(fe_web_client_send_raw(c, A, 1000));
	g_assert_true(fe_web_client_send_raw(c, B, 500));
	g_assert_true(c->output_want_read);
	g_assert_cmpint(c->send_tag, ==, -1); /* no writable watch: it would spin */
	g_assert_cmpuint(fe_web_fuzz_read_peer(0), ==, 0);

	fe_web_fuzz_peer_write(ping->data, ping->len); /* masked ping "x" */
	g_byte_array_unref(ping);
	fe_web_fuzz_client_input(c);
	fe_web_fuzz_read_peer(0);

	got = fe_web_fuzz_received();
	g_assert_cmpuint(got->len, ==, 1500 + sizeof(pong));
	g_assert_cmpmem(got->data, 1000, A, 1000);
	g_assert_cmpmem(got->data + 1000, 500, B, 500);
	g_assert_cmpmem(got->data + 1500, sizeof(pong), pong, sizeof(pong));
	g_assert_false(c->output_want_read);
	g_assert_cmpuint(test_pending(c), ==, 0);
	fe_web_fuzz_client_free(c);
}

/* Up to 32 MB is queued; one byte more and the client is cut off, and
 * closed from the main loop */
static void test_overflow_closes(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	guchar *rest;
	gsize queued;
	char *printed;

	g_free(fe_web_fuzz_take_printed());
	fill_socket(c);
	queued = test_pending(c);
	g_assert_cmpuint(queued, <, FE_WEB_MAX_OUTPUT);

	rest = g_malloc0(FE_WEB_MAX_OUTPUT - queued);
	g_assert_true(fe_web_client_send_raw(c, rest, FE_WEB_MAX_OUTPUT - queued));
	g_free(rest);
	g_assert_cmpuint(test_pending(c), ==, FE_WEB_MAX_OUTPUT);

	g_assert_false(fe_web_client_send_raw(c, "x", 1));
	g_assert_true(c->output_failed);
	g_assert_cmpuint(c->output_buffer->len, ==, 0);
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, "does not read what is sent to it "
	                                 "(over 32 MB queued) - disconnected"));
	g_free(printed);

	/* not closed by the call itself: the caller may still use it */
	g_assert_true(fe_web_fuzz_client_alive(c));
	fe_web_fuzz_iterate(100);
	g_assert_false(fe_web_fuzz_client_alive(c));
	fe_web_fuzz_client_free(c);
}

/* A write error: FALSE, and the client is closed from the main loop */
static void test_write_error_closes_later(void)
{
	static const int script[] = { -1 };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, TRUE);

	fe_web_fuzz_tls_write_script(script, 1);
	g_assert_false(fe_web_client_send_raw(c, A, 1000));
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_cmpuint(c->close_tag, !=, 0);
	fe_web_fuzz_iterate(100);
	g_assert_false(fe_web_fuzz_client_alive(c));
	/* the TLS layer marks the channel: no close_notify after an error */
	g_assert_cmpint(fe_web_fuzz_tls_freed_failed(), ==, 1);
	fe_web_fuzz_client_free(c);
}

/* Over 32 MB queued for a TLS client: the channel is closed without a
 * close_notify, as a record may be cut */
static void test_overflow_cuts_tls_record(void)
{
	static const int script[] = { 100, FE_WEB_SSL_WANT_WRITE };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, TRUE);
	guchar *big;

	fe_web_fuzz_tls_write_script(script, 2);
	g_assert_true(fe_web_client_send_raw(c, A, 1000));
	g_assert_cmpuint(test_pending(c), ==, 900);

	big = g_malloc0(FE_WEB_MAX_OUTPUT);
	g_assert_false(fe_web_client_send_raw(c, big, FE_WEB_MAX_OUTPUT));
	g_free(big);
	g_assert_true(c->output_failed);
	g_free(fe_web_fuzz_take_printed());

	fe_web_fuzz_iterate(100);
	g_assert_false(fe_web_fuzz_client_alive(c));
	g_assert_cmpint(fe_web_fuzz_tls_freed_failed(), ==, 1);
	fe_web_fuzz_client_free(c);
}

/* The browser closes while output is queued: the close frame is queued
 * behind it, nothing more is accepted, and the client is closed once the
 * close frame is written */
static void test_close_after_queued_output(void)
{
	static const guchar close_reply[] = { 0x88, 0x02, 0x03, 0xE8 };
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	const GByteArray *got;
	gsize sent;
	int rounds;

	sent = fill_socket(c);
	peer_sends_close(c);
	g_assert_true(fe_web_fuzz_client_alive(c));
	g_assert_true(c->closing);
	g_assert_cmpuint(c->close_tag, !=, 0);
	g_assert_false(fe_web_client_send_raw(c, "x", 1));

	for (rounds = 0; rounds < 100000 && fe_web_fuzz_client_alive(c); rounds++) {
		fe_web_fuzz_read_peer(0);
		fe_web_fuzz_client_writable(c);
	}
	g_assert_false(fe_web_fuzz_client_alive(c));
	fe_web_fuzz_read_peer(0);

	got = fe_web_fuzz_received();
	assert_filled(got->data, MIN(got->len, sent));
	g_assert_cmpuint(got->len, ==, sent + sizeof(close_reply));
	g_assert_cmpmem(got->data + got->len - sizeof(close_reply), sizeof(close_reply),
	                close_reply, sizeof(close_reply));
	fe_web_fuzz_client_free(c);
}

/* ... and when the browser never reads, after FE_WEB_LINGER_TIMEOUT */
static void test_linger_timeout(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	fill_socket(c);
	peer_sends_close(c);
	g_assert_true(c->closing);
	g_assert_true(fe_web_fuzz_client_alive(c));
	/* not before the timeout ... */
	fe_web_fuzz_iterate((FE_WEB_LINGER_TIMEOUT - 1) * 1000);
	g_assert_true(fe_web_fuzz_client_alive(c));
	/* ... and by then (GLib rounds the seconds up, by under one) */
	fe_web_fuzz_iterate(2 * 1000);
	g_assert_false(fe_web_fuzz_client_alive(c));
	fe_web_fuzz_client_free(c);
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);
	fill_patterns();

	g_test_add_func("/fe-web/output/partial-write-keeps-order",
	                test_partial_write_keeps_order);
	g_test_add_func("/fe-web/output/want-read-waits-for-input",
	                test_want_read_waits_for_input);
	g_test_add_func("/fe-web/output/overflow-closes", test_overflow_closes);
	g_test_add_func("/fe-web/output/write-error-closes-later",
	                test_write_error_closes_later);
	g_test_add_func("/fe-web/output/overflow-cuts-tls-record",
	                test_overflow_cuts_tls_record);
	g_test_add_func("/fe-web/output/close-after-queued-output",
	                test_close_after_queued_output);
	g_test_add_func("/fe-web/output/linger-timeout", test_linger_timeout);

	return g_test_run();
}
