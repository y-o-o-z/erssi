/*
 test-fe-web-password.c : how fe-web reads the password of a login

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A web client gives fe_web_password in an "Authorization: Bearer" header
 of the upgrade request or, as NexusIRC as published does, as the
 percent-encoded "password" parameter of the URL's query. The checks here run fe_web_verify_password() of
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

/* The password as the "password" parameter of the query (NexusIRC as
 * published sends it so, encoded with encodeURIComponent()) */
static void test_query(void)
{
	set_password(TEST_PASSWORD);
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD, ""), ==, 1);
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD "x", ""), ==, 0);
	g_assert_cmpint(verify("/?password=", ""), ==, 0);
	g_assert_cmpint(verify("/?password", ""), ==, 0);
	g_assert_cmpint(verify("/?a=1&password=" TEST_PASSWORD, ""), ==, 1);
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD "&a=1", ""), ==, 1);
	g_assert_cmpint(verify("/?mypassword=" TEST_PASSWORD, ""), ==, 0);
	g_assert_cmpint(verify("/?a=?password=" TEST_PASSWORD, ""), ==, 0);
	g_assert_cmpint(verify("/path?password=" TEST_PASSWORD, ""), ==, 1);
	/* the header, when there is one, is the password given */
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD, "Authorization: Bearer wrong\r\n"),
	                ==, 0);
	g_assert_cmpint(verify("/?password=wrong", "Authorization: Bearer " TEST_PASSWORD "\r\n"),
	                ==, 1);
	/* an Authorization header without a usable password is a wrong
	 * password: the query is read only when there is no such header */
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD, "Authorization: Bearer \r\n"), ==, 0);
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD, "Authorization: Bearer  \t\r\n"), ==, 0);
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD, "Authorization:\r\n"), ==, 0);
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD, "authorization: Basic eDp5\r\n"), ==, 0);
	/* a request target has no fragment (RFC 9112 3.2): what follows a '#'
	 * is not part of the query */
	g_assert_cmpint(verify("/?a=1#&password=" TEST_PASSWORD, ""), ==, 0);
	g_assert_cmpint(verify("/#?password=" TEST_PASSWORD, ""), ==, 0);
	g_assert_cmpint(verify("/?password=" TEST_PASSWORD "#x", ""), ==, 0);
}

/* the query of the request line only: "?password=" anywhere else in the
 * request (another header) is no password */
static void test_query_only_in_request_line(void)
{
	set_password(TEST_PASSWORD);
	g_assert_cmpint(verify("/", "Referer: https://example.org/?password=" TEST_PASSWORD "\r\n"),
	                ==, 0);
	g_assert_cmpint(verify("/", "X-Note: ?password=" TEST_PASSWORD "\r\n"), ==, 0);
}

/* A base64 secret (the startup hint suggests openssl rand -base64 32) has
 * '+', '/' and '='; a client that builds the URL correctly sends them
 * percent-encoded. Unencoded they work as before: '+' is a plus here, not
 * a space. */
static void test_query_percent_decoded(void)
{
	set_password("ab+c/d=");
	g_assert_cmpint(verify("/?password=ab%2Bc%2Fd%3D", ""), ==, 1);
	g_assert_cmpint(verify("/?password=ab%2bc%2fd%3d", ""), ==, 1);
	g_assert_cmpint(verify("/?password=ab+c/d=", ""), ==, 1);
	g_assert_cmpint(verify("/?password=ab%20c/d=", ""), ==, 0);

	set_password("a b&c");
	g_assert_cmpint(verify("/?password=a%20b%26c", ""), ==, 1);
	g_assert_cmpint(verify("/?password=a+b%26c", ""), ==, 0);

	/* UTF-8, as encodeURIComponent() sends it */
	set_password("za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87");
	g_assert_cmpint(verify("/?password=za%C5%BC%C3%B3%C5%82%C4%87", ""), ==, 1);

	/* broken escapes and an escaped NUL are no password */
	set_password("ab%2");
	g_assert_cmpint(verify("/?password=ab%2", ""), ==, 0);
	set_password("ab%zz");
	g_assert_cmpint(verify("/?password=ab%zz", ""), ==, 0);
	set_password("ab");
	g_assert_cmpint(verify("/?password=ab%00", ""), ==, 0);
	g_assert_cmpint(verify("/?password=ab", ""), ==, 1);
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

static void test_login_with_query(void)
{
	GByteArray *got;

	set_password("ab+c/d=");
	got = login("GET /?password=ab%2Bc%2Fd%3D HTTP/1.1\r\n" REQUEST_HEAD "\r\n");
	g_assert_true(test_starts_with(got, HTTP_101));
	g_byte_array_unref(got);
	got = login("GET /?password=ab HTTP/1.1\r\n" REQUEST_HEAD "\r\n");
	g_assert_true(test_starts_with(got, HTTP_401));
	g_byte_array_unref(got);
	set_password(TEST_PASSWORD);
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
	g_test_add_func("/fe-web/password/query", test_query);
	g_test_add_func("/fe-web/password/query-only-in-request-line",
	                test_query_only_in_request_line);
	g_test_add_func("/fe-web/password/query-percent-decoded", test_query_percent_decoded);
	g_test_add_func("/fe-web/password/login-with-query", test_login_with_query);

	return g_test_run();
}
