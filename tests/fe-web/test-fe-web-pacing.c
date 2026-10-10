/*
 test-fe-web-pacing.c : login pacing per peer, and the peer keys

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 After FE_WEB_MAX_FAILS wrong passwords within FE_WEB_FAIL_WINDOW seconds
 from one peer, one login of that peer is checked every
 FE_WEB_CHECK_INTERVAL seconds. Other peers are not affected, except when
 the table of peers is full: then new peers share one overflow bucket.
*/

#include <irssi/src/fe-web/fe-web-pacing.h>
#include <irssi/src/core/network.h>

#include <glib.h>
#include <string.h>

#define T 1000

/* 'n' wrong passwords from 'peer' at 'now', each checked first; the
 * result of the last fe_web_pacing_failed() */
static gboolean fail_n(FE_WEB_PACING *p, const char *peer, int n, time_t now)
{
	gboolean started = FALSE;
	int i;

	for (i = 0; i < n; i++) {
		g_assert_true(fe_web_pacing_may_check(p, peer, now));
		started = fe_web_pacing_failed(p, peer, now);
	}
	return started;
}

static void test_five_failures_then_interval(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int i;
	for (i = 1; i <= 4; i++) {
		g_assert_true(fe_web_pacing_may_check(p, "uid:1001", 1000));
		g_assert_false(fe_web_pacing_failed(p, "uid:1001", 1000));
	}
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", 1000));
	g_assert_true(fe_web_pacing_failed(p, "uid:1001", 1000));   /* 5th starts pacing */
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", 1001));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", 1002));
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", 1003));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1000", 1001)); /* another peer */
	fe_web_pacing_free(p);
}

static void test_window_expires(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	g_assert_true(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS, T));
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", T + 1));
	/* a failure at T + 1 would have moved the window: none came */
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + FE_WEB_FAIL_WINDOW + 1));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + FE_WEB_FAIL_WINDOW + 1));
	/* and the count starts again: 4 more are not paced */
	g_assert_false(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS - 1, T + 62));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 62));
	fe_web_pacing_free(p);
}

static void test_window_counts_from_last_failure(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	/* failures 50 s apart stay within the window of the one before */
	g_assert_false(fail_n(p, "uid:1001", 1, T));
	g_assert_false(fail_n(p, "uid:1001", 1, T + 50));
	g_assert_false(fail_n(p, "uid:1001", 1, T + 100));
	g_assert_false(fail_n(p, "uid:1001", 1, T + 150));
	g_assert_true(fail_n(p, "uid:1001", 1, T + 200));
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", T + 201));
	fe_web_pacing_free(p);
}

static void test_success_resets(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	g_assert_true(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS, T));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	fe_web_pacing_succeeded(p, "uid:1001");
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	g_assert_false(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS - 1, T + 2));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	/* the 5th after the reset starts pacing again */
	g_assert_true(fe_web_pacing_failed(p, "uid:1001", T + 2));
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", T + 3));
	/* a success of an unknown peer changes nothing */
	fe_web_pacing_succeeded(p, "uid:1002");
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", T + 3));
	fe_web_pacing_free(p);
}

static void test_reset_forgets_all(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	g_assert_true(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS, T));
	fe_web_pacing_reset(p);
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T));
	fe_web_pacing_free(p);
}

/* k0 .. k255 with one failure each at T: the table is full */
static void fill_table(FE_WEB_PACING *p)
{
	int i;

	for (i = 0; i < FE_WEB_PACING_MAX_PEERS; i++) {
		char key[16];

		g_snprintf(key, sizeof(key), "k%d", i);
		g_assert_false(fail_n(p, key, 1, T));
	}
}

static void test_full_table_uses_overflow(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	fill_table(p);
	g_assert_true(fail_n(p, "new", FE_WEB_MAX_FAILS, T));
	g_assert_false(fe_web_pacing_may_check(p, "new", T + 1));
	g_assert_false(fe_web_pacing_may_check(p, "other", T + 1)); /* shared bucket */
	g_assert_true(fe_web_pacing_may_check(p, "k1", T + 1));
	/* the peers in the table are not touched by the overflow */
	g_assert_false(fail_n(p, "k1", FE_WEB_MAX_FAILS - 2, T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "k2", T + 1));
	/* the overflow is paced, not locked */
	g_assert_true(fe_web_pacing_may_check(p, "other", T + 2));
	g_assert_false(fe_web_pacing_may_check(p, "new", T + 3));
	fe_web_pacing_free(p);
}

static void test_full_table_paced_peer_stays(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int i;

	/* a paced peer cannot flush itself out of the table with new peers */
	g_assert_true(fail_n(p, "guesser", FE_WEB_MAX_FAILS, T));
	for (i = 0; i < 2 * FE_WEB_PACING_MAX_PEERS; i++) {
		char key[16];

		g_snprintf(key, sizeof(key), "k%d", i);
		fe_web_pacing_may_check(p, key, T + 1);
		fe_web_pacing_failed(p, key, T + 1);
	}
	g_assert_false(fe_web_pacing_may_check(p, "guesser", T + 1));
	/* nor after a minute of failures from the others */
	for (i = 0; i < 2 * FE_WEB_PACING_MAX_PEERS; i++) {
		char key[16];

		g_snprintf(key, sizeof(key), "k%d", i);
		fe_web_pacing_failed(p, key, T + 59);
	}
	g_assert_true(fe_web_pacing_may_check(p, "guesser", T + 59));
	g_assert_false(fe_web_pacing_may_check(p, "guesser", T + 60));
	fe_web_pacing_free(p);
}

static void test_expired_peers_make_room(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	fill_table(p);
	g_assert_true(fail_n(p, "new", FE_WEB_MAX_FAILS, T));
	g_assert_true(fail_n(p, "new2", FE_WEB_MAX_FAILS, T + FE_WEB_FAIL_WINDOW + 1));
	g_assert_true(fe_web_pacing_may_check(p, "other", T + FE_WEB_FAIL_WINDOW + 1));
	g_assert_true(fe_web_pacing_may_check(p, "other", T + FE_WEB_FAIL_WINDOW + 1));
	/* new2 has a bucket of its own */
	g_assert_false(fe_web_pacing_may_check(p, "new2", T + FE_WEB_FAIL_WINDOW + 1));
	fe_web_pacing_free(p);
}

static char *key_of(const char *host)
{
	IPADDR ip;

	memset(&ip, 0, sizeof(ip));
	g_assert_cmpint(net_host2ip(host, &ip), ==, 0);
	return fe_web_peer_key_ip(&ip);
}

static void assert_key(const char *host, const char *want)
{
	char *key = key_of(host);

	g_assert_cmpstr(key, ==, want);
	g_free(key);
}

static void test_peer_key_ip4(void)
{
	assert_key("192.0.2.7", "ip:192.0.2.7");
}

static void test_peer_key_ip6_prefix(void)
{
	assert_key("2001:db8:1:2:3:4:5:6", "ip:2001:db8:1:2::/64");
	assert_key("2001:db8:1:2::", "ip:2001:db8:1:2::/64");
	assert_key("2001:db8::1", "ip:2001:db8::/64");
}

static void test_peer_key_ip4_mapped(void)
{
	assert_key("::ffff:192.0.2.7", "ip:192.0.2.7");
}

static void test_peer_key_uid(void)
{
	char *key = fe_web_peer_key_uid(1000);

	g_assert_cmpstr(key, ==, "uid:1000");
	g_free(key);
}

static gboolean loopback(const char *host)
{
	IPADDR ip;

	memset(&ip, 0, sizeof(ip));
	g_assert_cmpint(net_host2ip(host, &ip), ==, 0);
	return fe_web_ip_is_loopback(&ip);
}

static void test_peer_key_loopback(void)
{
	g_assert_true(loopback("127.0.0.1"));
	g_assert_true(loopback("127.1.2.3"));
	g_assert_true(loopback("::1"));
	g_assert_true(loopback("::ffff:127.0.0.1"));
	g_assert_false(loopback("192.0.2.1"));
	g_assert_false(loopback("::2"));
	g_assert_false(loopback("::ffff:192.0.2.1"));
	g_assert_false(loopback("2001:db8::1"));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fe-web/pacing/five-failures-then-interval",
	                test_five_failures_then_interval);
	g_test_add_func("/fe-web/pacing/window-expires", test_window_expires);
	g_test_add_func("/fe-web/pacing/window-counts-from-last-failure",
	                test_window_counts_from_last_failure);
	g_test_add_func("/fe-web/pacing/success-resets", test_success_resets);
	g_test_add_func("/fe-web/pacing/reset-forgets-all", test_reset_forgets_all);
	g_test_add_func("/fe-web/pacing/full-table-uses-overflow", test_full_table_uses_overflow);
	g_test_add_func("/fe-web/pacing/full-table-paced-peer-stays",
	                test_full_table_paced_peer_stays);
	g_test_add_func("/fe-web/pacing/expired-peers-make-room", test_expired_peers_make_room);
	g_test_add_func("/fe-web/peer-key/ip4", test_peer_key_ip4);
	g_test_add_func("/fe-web/peer-key/ip6-prefix", test_peer_key_ip6_prefix);
	g_test_add_func("/fe-web/peer-key/ip4-mapped", test_peer_key_ip4_mapped);
	g_test_add_func("/fe-web/peer-key/uid", test_peer_key_uid);
	g_test_add_func("/fe-web/peer-key/loopback", test_peer_key_loopback);

	return g_test_run();
}
