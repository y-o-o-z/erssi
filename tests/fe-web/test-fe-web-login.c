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

#include <errno.h>
#include <fcntl.h>
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

/* fe-web listening on TCP at 'bind' and a client connected to 'host'
 * (of 'family'); the one web client, NULL and a skip when this system
 * cannot (no IPv6). Close *fd and call fe_web_server_deinit() after. */
static WEB_CLIENT_REC *tcp_client(const char *bind, int family, const char *host, int *fd)
{
	struct sockaddr_storage ss;
	socklen_t len;

	setup();
	*fd = -1;
	settings_set_str("fe_web_socket", "");
	settings_set_str("fe_web_bind", bind);
	settings_set_int("fe_web_port", 0);
	fe_web_server_init();
	if (fe_web_fuzz_listen_port() <= 0) {
		fe_web_server_deinit();
		g_free(fe_web_fuzz_take_printed());
		g_test_skip("cannot listen there (no IPv6?)");
		return NULL;
	}

	memset(&ss, 0, sizeof(ss));
	if (family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in *) &ss;

		sa->sin_family = AF_INET;
		sa->sin_port = htons(fe_web_fuzz_listen_port());
		g_assert_cmpint(inet_pton(AF_INET, host, &sa->sin_addr), ==, 1);
		len = sizeof(*sa);
	} else {
		struct sockaddr_in6 *sa = (struct sockaddr_in6 *) &ss;

		sa->sin6_family = AF_INET6;
		sa->sin6_port = htons(fe_web_fuzz_listen_port());
		g_assert_cmpint(inet_pton(AF_INET6, host, &sa->sin6_addr), ==, 1);
		len = sizeof(*sa);
	}
	*fd = socket(family, SOCK_STREAM, 0);
	if (*fd < 0 || connect(*fd, (struct sockaddr *) &ss, len) != 0) {
		if (*fd >= 0)
			close(*fd);
		*fd = -1;
		fe_web_server_deinit();
		g_free(fe_web_fuzz_take_printed());
		g_test_skip("cannot connect there (no IPv6?)");
		return NULL;
	}
	fe_web_fuzz_iterate(100);
	g_assert_cmpuint(g_slist_length(web_clients), ==, 1);
	return web_clients->data;
}

static void tcp_client_done(int fd)
{
	close(fd);
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
}

/* A TCP client on loopback counts as the uid of its process (Linux), or
 * else as its address, which every user of the machine shares */
static void assert_loopback_key(const char *bind, int family, const char *host,
                                const char *key_elsewhere)
{
	WEB_CLIENT_REC *c;
	int fd;

	c = tcp_client(bind, family, host, &fd);
	if (c == NULL)
		return;
#ifdef __linux__
	g_assert_cmpstr(c->peer_key, ==, own);
	g_assert_false(c->peer_shared);
#else
	g_assert_cmpstr(c->peer_key, ==, key_elsewhere);
	g_assert_true(c->peer_shared);
#endif
	(void) key_elsewhere;
	tcp_client_done(fd);
}

static void test_tcp_loopback_key_is_uid(void)
{
	assert_loopback_key("127.0.0.1", AF_INET, "127.0.0.1", "ip:loopback");
}

static void test_tcp6_loopback_key_is_uid(void)
{
	assert_loopback_key("::1", AF_INET6, "::1", "ip:loopback");
}

/* fe_web_bind :: and a client on 127.0.0.1 (the peer is
 * ::ffff:127.0.0.1) */
static void test_tcp_dual_stack_key_is_uid(void)
{
	assert_loopback_key("::", AF_INET, "127.0.0.1", "ip:loopback");
}

/* A loopback peer whose uid is not known (it closed before fe-web took
 * the connection, so its socket is no longer ESTABLISHED) counts as its
 * address, which other users share - never as some uid */
static void test_tcp_unknown_uid_key_is_address(void)
{
	struct sockaddr_in sa;
	WEB_CLIENT_REC *c;
	int fd, i;

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
	close(fd);
	g_usleep(20000);
	/* only the accept: the client's input is not handled yet */
	for (i = 0; i < 100 && web_clients == NULL; i++) {
		g_main_context_iteration(NULL, FALSE);
		if (web_clients == NULL)
			g_usleep(1000);
	}
	g_assert_cmpuint(g_slist_length(web_clients), ==, 1);
	c = web_clients->data;
	g_assert_cmpstr(c->peer_key, ==, "ip:loopback");
	g_assert_true(c->peer_shared);
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
}

/* One login from 'peer' with peer_shared as given */
static GByteArray *attempt_shared(const char *peer, const char *password, gboolean shared)
{
	WEB_CLIENT_REC *client;
	char *request;

	client = fe_web_fuzz_client_new_peer(peer, FALSE, FALSE);
	client->peer_shared = shared;
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

static const char SHARED_HINT[] =
    "fe-web: other users of this machine can reach this port - /SET fe_web_socket "
    "~/.erssi/fe-web.sock lets only your own processes in";

/* Paced on a loopback address all users share: a hint to use the Unix
 * socket, with the pacing notice */
static void test_shared_loopback_hint(void)
{
	char *printed;
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt_shared("ip:loopback", "wrong", TRUE),
		                               HTTP_401));
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, "fe_web_socket ~/.erssi/fe-web.sock"));
	assert_printed(printed, SHARED_HINT);
	g_free(printed);

	/* not on every failure while paced */
	fe_web_fuzz_set_clock(T + 2);
	g_assert_true(test_starts_with(attempt_shared("ip:loopback", "wrong", TRUE), HTTP_401));
	printed = fe_web_fuzz_take_printed();
	g_assert_null(strstr(printed, "fe_web_socket"));
	g_free(printed);
}

/* No hint for a peer of its own (a uid, a remote address) */
static void test_no_hint_unless_shared(void)
{
	char *printed;
	int i;

	setup();
	for (i = 0; i < 5; i++)
		g_assert_true(test_starts_with(attempt_shared("ip:192.0.2.1", "wrong", FALSE),
		                               HTTP_401));
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, "5 wrong passwords within a minute from ip:192.0.2.1"));
	g_assert_null(strstr(printed, "fe_web_socket"));
	g_free(printed);
}

/* The hint comes with the pacing notice, which is printed once a minute
 * at most */
static void test_shared_hint_once_a_minute(void)
{
	char *printed;
	int i;

	setup();
	for (i = 0; i < 5; i++)
		attempt_shared("ip:192.0.2.1", "wrong", FALSE);
	g_free(fe_web_fuzz_take_printed());
	fe_web_fuzz_set_clock(T + 1);
	for (i = 0; i < 5; i++)
		attempt_shared("ip:loopback", "wrong", TRUE);
	printed = fe_web_fuzz_take_printed();
	g_assert_null(strstr(printed, "fe_web_socket"));
	g_free(printed);
}

/* fe-web listening on 127.0.0.1, port 0 (setup() first) */
static void tcp_listen(void)
{
	settings_set_str("fe_web_socket", "");
	settings_set_str("fe_web_bind", "127.0.0.1");
	settings_set_int("fe_web_port", 0);
	fe_web_server_init();
	g_assert_cmpint(fe_web_fuzz_listen_port(), >, 0);
}

/* Connect from 'src' (any port) to fe-web; the new web client (fd in
 * *fd, to be closed - its client is then closed too, so keep the fds of
 * several open until the end). NULL if this machine has no address 'src':
 * Linux answers on all of 127/8, FreeBSD's lo0 has only 127.0.0.1. */
static WEB_CLIENT_REC *try_connect_from(const char *src, int *fd)
{
	struct sockaddr_in sa;
	guint before = g_slist_length(web_clients);
	int i;

	*fd = socket(AF_INET, SOCK_STREAM, 0);
	g_assert_cmpint(*fd, >=, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	g_assert_cmpint(inet_pton(AF_INET, src, &sa.sin_addr), ==, 1);
	if (bind(*fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
		g_assert_cmpint(errno, ==, EADDRNOTAVAIL);
		close(*fd);
		*fd = -1;
		return NULL;
	}
	sa.sin_port = htons(fe_web_fuzz_listen_port());
	g_assert_cmpint(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), ==, 1);
	g_assert_cmpint(connect(*fd, (struct sockaddr *) &sa, sizeof(sa)), ==, 0);
	for (i = 0; i < 200 && g_slist_length(web_clients) == before; i++)
		fe_web_fuzz_iterate(5);
	g_assert_cmpuint(g_slist_length(web_clients), ==, before + 1);
	return g_slist_last(web_clients)->data;
}

static WEB_CLIENT_REC *connect_from(const char *src, int *fd)
{
	WEB_CLIENT_REC *c = try_connect_from(src, fd);

	g_assert_nonnull(c);
	return c;
}

/* The 2 MiB send buffer for large state dumps is above FreeBSD's default
 * kern.ipc.maxsockbuf (2 MiB including its bookkeeping, so ENOBUFS): a
 * smaller one is taken, with no warning */
static void test_tcp_send_buffer_no_warning(void)
{
	WEB_CLIENT_REC *c;
	char *printed;
	int fd;

	setup();
	tcp_listen();
	g_free(fe_web_fuzz_take_printed());
	c = connect_from("127.0.0.1", &fd);
	g_assert_nonnull(c);
	printed = fe_web_fuzz_take_printed();
	g_assert_null(strstr(printed, "SO_SNDBUF"));
	g_free(printed);
	close(fd);
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
}

#ifdef __linux__
static int diag_calls;

static gssize diag_fails(const void *req, gsize req_len, void *reply, gsize size)
{
	(void) req;
	(void) req_len;
	(void) reply;
	(void) size;
	diag_calls++;
	return -1;
}

static int open_fds[8], open_count;

/* The key of a new connection from 'src' (kept open until
 * close_connections()) */
static void assert_connect_key(const char *src, const char *want)
{
	WEB_CLIENT_REC *c;

	g_assert_cmpint(open_count, <, G_N_ELEMENTS(open_fds));
	c = connect_from(src, &open_fds[open_count++]);
	g_assert_cmpstr(c->peer_key, ==, want);
	g_assert_true(c->peer_shared == (strcmp(want, "ip:loopback") == 0));
}

static void close_connections(void)
{
	while (open_count > 0)
		close(open_fds[--open_count]);
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
}

/* /proc is read at most twice a second for all peers; sock_diag answers
 * do not count */
static void test_tcp_proc_budget(void)
{
	setup();
	tcp_listen();
	/* sock_diag cannot answer: /proc twice, then not known */
	fe_web_sock_diag_set_func(diag_fails);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", "ip:loopback");
	/* the next second */
	fe_web_fuzz_set_clock(T + 1);
	assert_connect_key("127.0.0.1", own);
	close_connections();

	setup();
	tcp_listen();
	fe_web_sock_diag_set_func(NULL);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", own);
	fe_web_sock_diag_set_func(diag_fails);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", "ip:loopback");
	fe_web_sock_diag_set_func(NULL);
	close_connections();
}
#endif

#ifdef __linux__
/* sock_diag failing on every accept: after 3, it is not asked for 60 s
 * (fe-web's time()) */
static void test_tcp_sock_diag_backoff(void)
{
	setup();
	tcp_listen();
	fe_web_sock_diag_set_func(diag_fails);
	diag_calls = 0;
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", "ip:loopback");
	g_assert_cmpint(diag_calls, ==, FE_WEB_SOCK_DIAG_MAX_FAILS);
	fe_web_fuzz_set_clock(T + 1);
	assert_connect_key("127.0.0.1", own);
	g_assert_cmpint(diag_calls, ==, FE_WEB_SOCK_DIAG_MAX_FAILS);
	fe_web_fuzz_set_clock(T + FE_WEB_SOCK_DIAG_BACKOFF);
	assert_connect_key("127.0.0.1", own);
	g_assert_cmpint(diag_calls, ==, FE_WEB_SOCK_DIAG_MAX_FAILS + 1);
	fe_web_sock_diag_set_func(NULL);
	close_connections();
}

/* The queue full of ip:loopback connections (sock_diag fails, the /proc
 * budget is used up): a new one closes the oldest of them, never the
 * owner's pending login. sig_listen protects erssi's own uid, not the
 * key of the connection coming in. */
static void test_tcp_full_queue_keeps_owner(void)
{
	WEB_CLIENT_REC *owner, *c;
	struct sockaddr_in sa;
	char buf[64];
	int fds[8];
	int i;

	setup();
	tcp_listen();
	fe_web_sock_diag_set_func(diag_fails);
	fe_web_fuzz_spend_proc_budget();
	owner = fe_web_fuzz_client_new_peer(own, FALSE, FALSE);
	for (i = 0; i < 7; i++) {
		c = connect_from("127.0.0.1", &fds[i]);
		g_assert_cmpstr(c->peer_key, ==, "ip:loopback");
		g_assert_cmpint(fcntl(fds[i], F_SETFL, O_NONBLOCK), ==, 0);
	}
	g_assert_cmpuint(g_slist_length(web_clients), ==, 8);

	fds[7] = socket(AF_INET, SOCK_STREAM, 0);
	g_assert_cmpint(fds[7], >=, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(fe_web_fuzz_listen_port());
	g_assert_cmpint(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), ==, 1);
	g_assert_cmpint(connect(fds[7], (struct sockaddr *) &sa, sizeof(sa)), ==, 0);
	/* accepted once the oldest ip:loopback connection reads end of file */
	for (i = 0; i < 200 && read(fds[0], buf, sizeof(buf)) != 0; i++)
		fe_web_fuzz_iterate(5);
	g_assert_cmpint(i, <, 200);
	g_assert_true(fe_web_fuzz_client_alive(owner));
	g_assert_cmpuint(g_slist_length(web_clients), ==, 8);
	c = g_slist_last(web_clients)->data;
	g_assert_cmpstr(c->peer_key, ==, "ip:loopback");

	fe_web_fuzz_client_free(owner);
	for (i = 0; i < 8; i++)
		close(fds[i]);
	fe_web_sock_diag_set_func(NULL);
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
}
#endif

/* A loopback peer whose uid is not known is one peer, whatever 127/8
 * source address it picks */
static void test_tcp_loopback_sources_share_key(void)
{
	setup();
	tcp_listen();
#ifdef __linux__
	fe_web_sock_diag_set_func(diag_fails);
	/* the /proc budget used up */
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.1", own);
	assert_connect_key("127.0.0.2", "ip:loopback");
	assert_connect_key("127.0.0.3", "ip:loopback");
	assert_connect_key("127.1.2.3", "ip:loopback");
	fe_web_sock_diag_set_func(NULL);
	close_connections();
#else
	{
		/* only the sources this machine has (FreeBSD: 127.0.0.1 alone) */
		const char *const srcs[] = { "127.0.0.1", "127.0.0.2", "127.1.2.3" };
		WEB_CLIENT_REC *c;
		guint i, tried = 0;
		int fd;

		for (i = 0; i < G_N_ELEMENTS(srcs); i++) {
			c = try_connect_from(srcs[i], &fd);
			if (c == NULL)
				continue;
			tried++;
			g_assert_cmpstr(c->peer_key, ==, "ip:loopback");
			close(fd);
		}
		g_assert_cmpuint(tried, >=, 1);
		if (tried == 1)
			g_test_message("only 127.0.0.1 here: other loopback sources not tried");
	}
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
#endif
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
	g_test_add_func("/fe-web/login/tcp-loopback-key-is-uid", test_tcp_loopback_key_is_uid);
	g_test_add_func("/fe-web/login/tcp6-loopback-key-is-uid", test_tcp6_loopback_key_is_uid);
	g_test_add_func("/fe-web/login/tcp-dual-stack-key-is-uid", test_tcp_dual_stack_key_is_uid);
	g_test_add_func("/fe-web/login/tcp-unknown-uid-key-is-address",
	                test_tcp_unknown_uid_key_is_address);
	g_test_add_func("/fe-web/login/shared-loopback-hint", test_shared_loopback_hint);
	g_test_add_func("/fe-web/login/no-hint-unless-shared", test_no_hint_unless_shared);
	g_test_add_func("/fe-web/login/shared-hint-once-a-minute", test_shared_hint_once_a_minute);
#ifdef __linux__
	g_test_add_func("/fe-web/login/tcp-proc-budget", test_tcp_proc_budget);
	g_test_add_func("/fe-web/login/tcp-sock-diag-backoff", test_tcp_sock_diag_backoff);
	g_test_add_func("/fe-web/login/tcp-full-queue-keeps-owner",
	                test_tcp_full_queue_keeps_owner);
#endif
	g_test_add_func("/fe-web/login/tcp-loopback-sources-share-key",
	                test_tcp_loopback_sources_share_key);
	g_test_add_func("/fe-web/login/tcp-send-buffer-no-warning",
	                test_tcp_send_buffer_no_warning);

	ret = g_test_run();
	if (last != NULL)
		g_byte_array_unref(last);
	return ret;
}
