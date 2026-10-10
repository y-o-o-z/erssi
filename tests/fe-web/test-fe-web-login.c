/*
 test-fe-web-login.c : login pacing of fe-web, per peer

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 After 5 wrong passwords within a minute from one peer, fe-web checks one
 login from that peer every 2 s and answers the others with 429. Other
 peers log in as before, and the processes of erssi's own user (who can
 read the password in the config anyway) are never paced.

 The peers here are uid:<getuid() + 1> (the guesser) and uid:<getuid() + 2>,
 so that neither is erssi's own user, whatever uid runs the tests.
*/

#include "fe-web-test.h"

#include <irssi/src/core/settings.h>
#include <irssi/src/fe-web/fe-web-pacing.h>

#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define TEST_PASSWORD FE_WEB_FUZZ_PASSWORD
#define T 1000

static const char HTTP_401[] = "HTTP/1.1 401 Unauthorized\r\n";
static const char HTTP_429[] = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 2\r\n";
static const char HTTP_101[] = "HTTP/1.1 101 Switching Protocols\r\n";

static char guesser[32], other[32], third[32], fourth[32], own[32];

static void setup(void)
{
	g_snprintf(guesser, sizeof(guesser), "uid:%lu", (unsigned long) getuid() + 1);
	g_snprintf(other, sizeof(other), "uid:%lu", (unsigned long) getuid() + 2);
	g_snprintf(third, sizeof(third), "uid:%lu", (unsigned long) getuid() + 3);
	g_snprintf(fourth, sizeof(fourth), "uid:%lu", (unsigned long) getuid() + 4);
	g_snprintf(own, sizeof(own), "uid:%lu", (unsigned long) getuid());
	fe_web_fuzz_reset_login_state();
	fe_web_fuzz_set_clock(T);
	g_free(fe_web_fuzz_take_printed());
}

/* what the last attempt() got */
static GByteArray *last = NULL;

/* One login from 'peer' on a new connection; returns what the browser got
 * (valid until the next attempt) */
static GByteArray *attempt(const char *peer, const char *password)
{
	WEB_CLIENT_REC *client;
	char *request;

	client = fe_web_fuzz_client_new_peer(peer, FALSE, FALSE);
	request = test_request(password);
	fe_web_fuzz_client_send(client, (const guchar *) request, strlen(request));
	g_free(request);
	if (last != NULL)
		g_byte_array_unref(last);
	last = g_byte_array_new();
	g_byte_array_append(last, fe_web_fuzz_received()->data, fe_web_fuzz_received()->len);
	fe_web_fuzz_client_free(client);
	return last;
}

static void test_other_peer_logs_in(void)
{
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_429));
	g_assert_true(test_starts_with(attempt(other, TEST_PASSWORD), HTTP_101));
	/* and a wrong password from it is checked, not paced */
	g_assert_true(test_starts_with(attempt(other, "wrong"), HTTP_401));
}

static void test_paced_429_after_five(void)
{
	char *printed, *want;
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_429));
	/* the right password is not checked either while paced */
	g_assert_true(test_starts_with(attempt(guesser, TEST_PASSWORD), HTTP_429));
	printed = fe_web_fuzz_take_printed();
	want = g_strdup_printf("fe-web: 5 wrong passwords within a minute from %s - "
	                       "checking one login from there every 2 s",
	                       guesser);
	if (strstr(printed, want) == NULL)
		g_error("expected \"%s\" in the output:\n%s", want, printed);
	g_free(want);
	g_free(printed);

	/* one check every 2 s */
	fe_web_fuzz_set_clock(T + 2);
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_429));
	fe_web_fuzz_set_clock(T + 4);
	g_assert_true(test_starts_with(attempt(guesser, TEST_PASSWORD), HTTP_101));
}

static void test_own_uid_never_paced(void)
{
	int i;

	setup();
	for (i = 0; i < 7; i++)
		g_assert_true(test_starts_with(attempt(own, "wrong"), HTTP_401));
	g_assert_true(test_starts_with(attempt(own, TEST_PASSWORD), HTTP_101));
}

static void test_owner_logs_in_while_paced(void)
{
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	for (i = 0; i < 3; i++) {
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_429));
		g_assert_true(test_starts_with(attempt(own, TEST_PASSWORD), HTTP_101));
	}
}

static void test_success_resets_peer(void)
{
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	fe_web_fuzz_set_clock(T + 2);
	g_assert_true(test_starts_with(attempt(guesser, TEST_PASSWORD), HTTP_101));
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	/* that was 5 again */
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_429));
}

static void test_pacing_ends_after_a_minute(void)
{
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	fe_web_fuzz_set_clock(T + 61);
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
}

static void test_client_keeps_peer_key(void)
{
	WEB_CLIENT_REC *client;

	setup();
	client = fe_web_fuzz_client_new_peer(other, FALSE, FALSE);
	g_assert_cmpstr(client->peer_key, ==, other);
	g_assert_false(client->peer_shared);
	fe_web_fuzz_client_free(client);
}

/* How often 'needle' is in 'haystack' */
static int count_of(const char *haystack, const char *needle)
{
	int n = 0;

	while ((haystack = strstr(haystack, needle)) != NULL) {
		n++;
		haystack += strlen(needle);
	}
	return n;
}

static void assert_printed(const char *printed, const char *want)
{
	if (strstr(printed, want) == NULL)
		g_error("expected \"%s\" in the output:\n%s", want, printed);
}

/* The owner's wrong passwords count nowhere: not in the table, not
 * towards the ceiling */
static void test_owner_failures_not_counted(void)
{
	int i;

	setup();
	for (i = 0; i < FE_WEB_GLOBAL_MAX_FAILS; i++)
		g_assert_true(test_starts_with(attempt(own, "wrong"), HTTP_401));
	g_assert_cmpuint(fe_web_pacing_table_size(fe_web_fuzz_login_pacing()), ==, 0);
	/* with the ceiling engaged, the third of these would get 429 */
	for (i = 0; i < 4; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
}

/* The owner with a full table: still checked, and no slot taken */
static void test_owner_with_full_table(void)
{
	FE_WEB_PACING *p;
	int i;

	setup();
	p = fe_web_fuzz_login_pacing();
	for (i = 0; i < FE_WEB_PACING_MAX_PEERS - 1; i++) {
		char key[32];

		g_snprintf(key, sizeof(key), "ip:192.0.2.%d", i);
		g_assert_true(fe_web_pacing_may_check(p, key, T - 100));
		fe_web_pacing_failed(p, key, T - 100);
	}
	for (i = 0; i < 7; i++)
		g_assert_true(test_starts_with(attempt(own, "wrong"), HTTP_401));
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, FE_WEB_PACING_MAX_PEERS - 1);
	/* the last slot is still free for a new peer */
	g_assert_true(test_starts_with(attempt(other, "wrong"), HTTP_401));
	g_assert_false(fe_web_pacing_in_overflow(p, other));
	g_assert_true(test_starts_with(attempt(own, TEST_PASSWORD), HTTP_101));
}

/* One pacing notice a minute; the peers paced in between are counted */
static void test_paced_notice_once_a_minute(void)
{
	const char *peers[3];
	char *printed, *want;
	int i, j;

	setup();
	peers[0] = guesser;
	peers[1] = other;
	peers[2] = third;
	for (j = 0; j < 3; j++)
		for (i = 0; i < 5; i++)
			g_assert_true(test_starts_with(attempt(peers[j], "wrong"), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	g_assert_cmpint(count_of(printed, "wrong passwords within a minute"), ==, 1);
	want = g_strdup_printf("5 wrong passwords within a minute from %s", guesser);
	assert_printed(printed, want);
	g_free(want);
	g_free(printed);

	fe_web_fuzz_set_clock(T + 61);
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(fourth, "wrong"), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	assert_printed(printed, "fe-web: logins from 2 more peers paced since the last notice");
	want = g_strdup_printf("5 wrong passwords within a minute from %s", fourth);
	assert_printed(printed, want);
	g_free(want);
	g_free(printed);
}

/* One peer paced in between: "1 more peer" */
static void test_paced_notice_singular(void)
{
	char *printed;
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(other, "wrong"), HTTP_401));
	g_free(fe_web_fuzz_take_printed());
	fe_web_fuzz_set_clock(T + 61);
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(third, "wrong"), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	assert_printed(printed, "fe-web: logins from 1 more peer paced since the last notice\n");
	g_free(printed);
}

/* A clock that goes back does not silence the pacing notice */
static void test_paced_notice_backward_clock(void)
{
	char *printed, *want;
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	g_free(fe_web_fuzz_take_printed());
	fe_web_fuzz_set_clock(T - 100);
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(other, "wrong"), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	want = g_strdup_printf("5 wrong passwords within a minute from %s", other);
	assert_printed(printed, want);
	g_free(want);
	g_free(printed);
}

#define REFUSED_WRONG "fe-web: connection from fuzz:0 refused: wrong or missing password"

/* Nor the once-a-minute line of refused connections */
static void test_refused_log_backward_clock(void)
{
	char *printed;

	setup();
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	assert_printed(printed, REFUSED_WRONG);
	g_free(printed);
	/* within the minute: counted, not printed */
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	g_assert_null(strstr(printed, "refused"));
	g_free(printed);
	fe_web_fuzz_set_clock(T - 100);
	g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	assert_printed(printed, "fe-web: 1 more connections refused");
	assert_printed(printed, REFUSED_WRONG);
	g_free(printed);
}

/* The owner is never paced, so it takes no place among the trusted
 * peers either */
static void test_owner_takes_no_trusted_slot(void)
{
	FE_WEB_PACING *p;
	int i;

	setup();
	p = fe_web_fuzz_login_pacing();
	for (i = 0; i < FE_WEB_PACING_MAX_TRUSTED; i++) {
		char key[32];

		g_snprintf(key, sizeof(key), "ip:192.0.2.%d", i);
		fe_web_pacing_succeeded(p, key);
	}
	g_assert_true(test_starts_with(attempt(own, TEST_PASSWORD), HTTP_101));
	/* ip:192.0.2.0, the oldest, is still trusted: its failure is
	 * counted in its own bucket, not in the table */
	fe_web_pacing_failed(p, "ip:192.0.2.0", T);
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, 0);
}

/* The overflow bucket paces every new peer: the notice says so */
static void test_overflow_notice(void)
{
	FE_WEB_PACING *p;
	char *printed, *want;
	int i;

	setup();
	p = fe_web_fuzz_login_pacing();
	/* a full table, its failures out of the ceiling's minute */
	for (i = 0; i < FE_WEB_PACING_MAX_PEERS; i++) {
		char key[32];

		g_snprintf(key, sizeof(key), "ip:192.0.2.%d", i);
		g_assert_true(fe_web_pacing_may_check(p, key, T - 100));
		fe_web_pacing_failed(p, key, T - 100);
	}
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt(guesser, "wrong"), HTTP_401));
	g_assert_true(fe_web_pacing_in_overflow(p, guesser));
	g_assert_true(test_starts_with(attempt(other, "wrong"), HTTP_429));
	printed = fe_web_fuzz_take_printed();
	want = g_strdup_printf("fe-web: 5 wrong passwords within a minute from new peers, last %s "
	                       "(more than 256 peers, counted together) - checking one login "
	                       "from all of them every 2 s",
	                       guesser);
	assert_printed(printed, want);
	g_assert_null(strstr(printed, "checking one login from there"));
	g_free(want);
	g_free(printed);
}

/* A client of the TCP listener counts as its address (until the uid of a
 * loopback peer is known) */
static void test_tcp_peer_key(void)
{
	struct sockaddr_in sa;
	WEB_CLIENT_REC *c;
	int fd;

	setup();
	settings_set_str("fe_web_socket", "");
	settings_set_str("fe_web_bind", "127.0.0.1");
	settings_set_int("fe_web_port", 0);
	fe_web_server_init();
	g_assert_cmpint(fe_web_fuzz_listen_port(), >, 0);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	g_assert_cmpint(fd, >=, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(fe_web_fuzz_listen_port());
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	g_assert_cmpint(connect(fd, (struct sockaddr *) &sa, sizeof(sa)), ==, 0);
	fe_web_fuzz_iterate(100);
	g_assert_cmpuint(g_slist_length(web_clients), ==, 1);
	c = web_clients->data;
	g_assert_cmpstr(c->peer_key, ==, "ip:127.0.0.1");
	close(fd);
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
}

int main(int argc, char **argv)
{
	int ret;

	test_init(&argc, &argv);

	g_test_add_func("/fe-web/login/other-peer-logs-in", test_other_peer_logs_in);
	g_test_add_func("/fe-web/login/paced-429-after-five", test_paced_429_after_five);
	g_test_add_func("/fe-web/login/own-uid-never-paced", test_own_uid_never_paced);
	g_test_add_func("/fe-web/login/owner-logs-in-while-paced", test_owner_logs_in_while_paced);
	g_test_add_func("/fe-web/login/success-resets-peer", test_success_resets_peer);
	g_test_add_func("/fe-web/login/pacing-ends-after-a-minute",
	                test_pacing_ends_after_a_minute);
	g_test_add_func("/fe-web/login/client-keeps-peer-key", test_client_keeps_peer_key);
	g_test_add_func("/fe-web/login/owner-failures-not-counted",
	                test_owner_failures_not_counted);
	g_test_add_func("/fe-web/login/owner-with-full-table", test_owner_with_full_table);
	g_test_add_func("/fe-web/login/paced-notice-once-a-minute",
	                test_paced_notice_once_a_minute);
	g_test_add_func("/fe-web/login/overflow-notice", test_overflow_notice);
	g_test_add_func("/fe-web/login/paced-notice-singular", test_paced_notice_singular);
	g_test_add_func("/fe-web/login/paced-notice-backward-clock",
	                test_paced_notice_backward_clock);
	g_test_add_func("/fe-web/login/refused-log-backward-clock",
	                test_refused_log_backward_clock);
	g_test_add_func("/fe-web/login/owner-takes-no-trusted-slot",
	                test_owner_takes_no_trusted_slot);
	g_test_add_func("/fe-web/login/tcp-peer-key", test_tcp_peer_key);

	ret = g_test_run();
	if (last != NULL)
		g_byte_array_unref(last);
	return ret;
}
