/*
 test-fe-web-password.c : how fe-web reads the password of a login

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A web client gives fe_web_password in an "Authorization: Bearer" header
 of the upgrade request. The checks here run fe_web_verify_password() of
 fe-web-server.c on whole requests, and a few logins end to end.
*/

#include "fe-web-test.h"

#include <irssi/src/core/settings.h>

#include <string.h>

#define TEST_PASSWORD FE_WEB_FUZZ_PASSWORD

static const char HTTP_101[] = "HTTP/1.1 101 Switching Protocols\r\n";
static const char HTTP_401[] = "HTTP/1.1 401 Unauthorized\r\n";

#define REQUEST_HEAD                                     \
	"Host: 127.0.0.1:9001\r\n"                       \
	"Upgrade: websocket\r\n"                         \
	"Connection: Upgrade\r\n"                        \
	"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" \
	"Sec-WebSocket-Version: 13\r\n"

/* fe_web_verify_password() on "GET <target>", the usual headers and
 * 'extra' (whole header lines, or "") */
static int verify(const char *target, const char *extra)
{
	char *request;
	int ret;

	request = g_strdup_printf("GET %s HTTP/1.1\r\n" REQUEST_HEAD "%s\r\n", target, extra);
	ret = fe_web_fuzz_verify_password(request);
	g_free(request);
	return ret;
}

/* fe_web_verify_password() with "Authorization: <value>" */
static int verify_header(const char *value)
{
	char *line;
	int ret;

	line = g_strdup_printf("Authorization: %s\r\n", value);
	ret = verify("/", line);
	g_free(line);
	return ret;
}

static void set_password(const char *password)
{
	settings_set_str("fe_web_password", password);
}

static void test_bearer(void)
{
	set_password(TEST_PASSWORD);
	g_assert_cmpint(verify_header("Bearer " TEST_PASSWORD), ==, 1);
	g_assert_cmpint(verify_header("bearer " TEST_PASSWORD), ==, 1);
	g_assert_cmpint(verify_header("Bearer " TEST_PASSWORD "x"), ==, 0);
	g_assert_cmpint(verify_header("Bearer x" TEST_PASSWORD), ==, 0);
	g_assert_cmpint(verify_header("Basic " TEST_PASSWORD), ==, 0);
	g_assert_cmpint(verify_header("Bearer "), ==, 0);
	g_assert_cmpint(verify("/", ""), ==, 0);
}

/* RFC 9110 5.5: the whitespace around a field value is not part of it, and
 * proxies that rewrite headers may add some */
static void test_bearer_trailing_whitespace(void)
{
	set_password(TEST_PASSWORD);
	g_assert_cmpint(verify_header("Bearer " TEST_PASSWORD " "), ==, 1);
	g_assert_cmpint(verify_header("Bearer " TEST_PASSWORD "\t \t"), ==, 1);
	g_assert_cmpint(verify_header("Bearer   "), ==, 0);
	g_assert_cmpint(verify_header("Bearer " TEST_PASSWORD " x"), ==, 0);

	/* spaces inside the password are kept */
	set_password("two words");
	g_assert_cmpint(verify_header("Bearer two words"), ==, 1);
	g_assert_cmpint(verify_header("Bearer two  words"), ==, 0);
	g_assert_cmpint(verify_header("Bearer twowords"), ==, 0);

	/* a password that ends in whitespace cannot travel in a header as it
	 * is: it is compared without that whitespace */
	set_password("ends in space ");
	g_assert_cmpint(verify_header("Bearer ends in space "), ==, 1);
	g_assert_cmpint(verify_header("Bearer ends in space"), ==, 1);
	g_assert_cmpint(verify_header("Bearer ends in"), ==, 0);
	set_password(TEST_PASSWORD);
}

/* A whole login on a new connection; returns what the browser got (to be
 * unreferenced) */
static GByteArray *login(const char *request)
{
	WEB_CLIENT_REC *client;
	GByteArray *got;

	fe_web_fuzz_reset_login_state();
	client = fe_web_fuzz_client_new_peer("uid:test", FALSE, FALSE);
	fe_web_fuzz_client_send(client, (const guchar *) request, strlen(request));
	got = g_byte_array_new();
	g_byte_array_append(got, fe_web_fuzz_received()->data, fe_web_fuzz_received()->len);
	fe_web_fuzz_client_free(client);
	return got;
}

static void test_login_with_trailing_whitespace(void)
{
	GByteArray *got;

	set_password(TEST_PASSWORD);
	got = login("GET / HTTP/1.1\r\n" REQUEST_HEAD "Authorization: Bearer " TEST_PASSWORD
	            " \r\n\r\n");
	g_assert_true(test_starts_with(got, HTTP_101));
	g_byte_array_unref(got);
	got = login("GET / HTTP/1.1\r\n" REQUEST_HEAD "Authorization: Bearer wrong \r\n\r\n");
	g_assert_true(test_starts_with(got, HTTP_401));
	g_byte_array_unref(got);
	g_free(fe_web_fuzz_take_printed());
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);

	g_test_add_func("/fe-web/password/bearer", test_bearer);
	g_test_add_func("/fe-web/password/bearer-trailing-whitespace",
	                test_bearer_trailing_whitespace);
	g_test_add_func("/fe-web/password/login-with-trailing-whitespace",
	                test_login_with_trailing_whitespace);

	return g_test_run();
}
