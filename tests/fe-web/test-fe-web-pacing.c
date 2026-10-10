/*
 test-fe-web-pacing.c : login pacing per peer, and the peer keys

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 After FE_WEB_MAX_FAILS wrong passwords within FE_WEB_FAIL_WINDOW seconds
 from one peer, one login of that peer is checked every
 FE_WEB_CHECK_INTERVAL seconds. Peers with a wrong password in the last
 FE_WEB_PEER_MEMORY seconds are suspects: once FE_WEB_GLOBAL_MAX_FAILS
 wrong passwords came within a minute, all suspects together get one check
 every FE_WEB_GLOBAL_CHECK_INTERVAL seconds. Clean peers (no wrong
 password remembered) and trusted ones (logged in before) are not held by
 that ceiling. When the table of suspects is full, new peers share one
 overflow bucket.
*/

#include "fe-web-test.h"

#include <irssi/src/fe-web/fe-web-pacing.h>
#include <irssi/src/core/network.h>

#include <glib.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#endif

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

/* "<prefix><i>" */
static const char *key_n(const char *prefix, int i)
{
	static char key[64];

	g_snprintf(key, sizeof(key), "%s%d", prefix, i);
	return key;
}

/* k0 .. k255 with one failure each at 'now': the table is full (and the
 * ceiling engaged for a minute) */
static void fill_table_at(FE_WEB_PACING *p, time_t now)
{
	int i;

	for (i = 0; i < FE_WEB_PACING_MAX_PEERS; i++)
		g_assert_false(fail_n(p, key_n("k", i), 1, now));
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, FE_WEB_PACING_MAX_PEERS);
}

static void fill_table(FE_WEB_PACING *p)
{
	fill_table_at(p, T);
}

/* FE_WEB_GLOBAL_MAX_FAILS clean peers "<prefix><i>" fail once each at
 * 'now': the ceiling is engaged, they are suspects */
static void engage(FE_WEB_PACING *p, const char *prefix, time_t now)
{
	int i;

	for (i = 0; i < FE_WEB_GLOBAL_MAX_FAILS; i++)
		g_assert_false(fail_n(p, key_n(prefix, i), 1, now));
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

/* A paced peer that keeps guessing stays paced: each paced failure moves
 * the window. A trusted peer, so that only its own bucket counts. */
static void test_paced_failures_extend_window(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	time_t t;

	fe_web_pacing_succeeded(p, "uid:1001");
	g_assert_true(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS, T));
	for (t = T + 2; t <= T + 60; t += 2) {
		g_assert_false(fe_web_pacing_may_check(p, "uid:1001", t - 1));
		g_assert_false(fail_n(p, "uid:1001", 1, t));
	}
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 62));
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", T + 62));
	/* and a minute after the last one it ends */
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 121));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 121));
	fe_web_pacing_free(p);
}

static void test_success_resets(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	g_assert_true(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS, T));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	fe_web_pacing_succeeded(p, "uid:1001");
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, 0);
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	g_assert_false(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS - 1, T + 2));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T + 2));
	/* the 5th after the reset starts pacing again */
	g_assert_true(fe_web_pacing_failed(p, "uid:1001", T + 2));
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", T + 3));
	/* a success of another peer changes nothing */
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

	/* the overflow bucket */
	fill_table(p);
	g_assert_true(fail_n(p, "new", FE_WEB_MAX_FAILS, T + 100));
	g_assert_false(fe_web_pacing_may_check(p, "other", T + 101));
	fe_web_pacing_reset(p);
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, 0);
	fill_table_at(p, T + 100);
	g_assert_true(fe_web_pacing_in_overflow(p, "other"));
	g_assert_true(fe_web_pacing_may_check(p, "other", T + 101));

	/* the ceiling */
	fe_web_pacing_reset(p);
	engage(p, "s", T);
	fe_web_pacing_reset(p);
	g_assert_false(fail_n(p, "s0", 1, T));
	g_assert_true(fe_web_pacing_may_check(p, "s0", T));
	g_assert_true(fe_web_pacing_may_check(p, "s0", T));

	/* the trusted peers */
	fe_web_pacing_reset(p);
	fe_web_pacing_succeeded(p, "t");
	fe_web_pacing_reset(p);
	fe_web_pacing_failed(p, "t", T);
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, 1);
	fe_web_pacing_free(p);
}

static void test_full_table_uses_overflow(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	/* filled at T; at T + 100 the ceiling is no longer engaged, and the
	 * table still remembers k0 .. k255 */
	fill_table(p);
	g_assert_true(fail_n(p, "new", FE_WEB_MAX_FAILS, T + 100));
	g_assert_true(fe_web_pacing_in_overflow(p, "new"));
	g_assert_false(fe_web_pacing_in_overflow(p, "k1"));
	g_assert_false(fe_web_pacing_may_check(p, "new", T + 101));
	g_assert_false(fe_web_pacing_may_check(p, "other", T + 101)); /* shared bucket */
	g_assert_true(fe_web_pacing_may_check(p, "k1", T + 101));
	/* the peers in the table are not touched by the overflow */
	g_assert_false(fail_n(p, "k1", FE_WEB_MAX_FAILS - 2, T + 101));
	g_assert_true(fe_web_pacing_may_check(p, "k2", T + 101));
	/* the overflow is paced, not locked */
	g_assert_true(fe_web_pacing_may_check(p, "other", T + 102));
	g_assert_false(fe_web_pacing_may_check(p, "new", T + 103));
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, FE_WEB_PACING_MAX_PEERS);
	fe_web_pacing_free(p);
}

static void test_success_keeps_overflow(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	fill_table(p);
	g_assert_true(fail_n(p, "new", FE_WEB_MAX_FAILS, T + 100));
	/* a success of peers that share the overflow bucket, the guesser
	 * among them, does not reset it */
	fe_web_pacing_succeeded(p, "other");
	fe_web_pacing_succeeded(p, "new");
	g_assert_false(fe_web_pacing_may_check(p, "third", T + 101));
	/* the one that logged in is trusted now: its own bucket */
	g_assert_false(fe_web_pacing_in_overflow(p, "new"));
	g_assert_true(fe_web_pacing_may_check(p, "new", T + 101));
	g_assert_true(fe_web_pacing_may_check(p, "new", T + 101));
	fe_web_pacing_free(p);
}

static void test_full_table_paced_peer_stays(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int i;

	/* a paced peer cannot flush itself out of the table with new peers */
	g_assert_true(fail_n(p, "guesser", FE_WEB_MAX_FAILS, T));
	for (i = 0; i < 2 * FE_WEB_PACING_MAX_PEERS; i++) {
		fe_web_pacing_may_check(p, key_n("k", i), T + 1);
		fe_web_pacing_failed(p, key_n("k", i), T + 1);
	}
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, FE_WEB_PACING_MAX_PEERS);
	g_assert_false(fe_web_pacing_in_overflow(p, "guesser"));
	g_assert_false(fe_web_pacing_may_check(p, "guesser", T + 1));
	/* nor by a minute of failures from the others */
	for (i = 0; i < 2 * FE_WEB_PACING_MAX_PEERS; i++)
		fe_web_pacing_failed(p, key_n("k", i), T + 59);
	g_assert_true(fe_web_pacing_may_check(p, "guesser", T + 59));
	g_assert_false(fe_web_pacing_may_check(p, "guesser", T + 60));
	fe_web_pacing_free(p);
}

static void test_expired_peers_make_room(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	const time_t later = T + FE_WEB_PEER_MEMORY + 1;

	fill_table(p);
	/* remembered for FE_WEB_PEER_MEMORY seconds, not only the window */
	g_assert_true(fe_web_pacing_may_check(p, "x", T + FE_WEB_PEER_MEMORY));
	g_assert_true(fe_web_pacing_in_overflow(p, "x"));
	g_assert_true(fail_n(p, "new2", FE_WEB_MAX_FAILS, later));
	g_assert_false(fe_web_pacing_in_overflow(p, "new2"));
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, 1);
	g_assert_true(fe_web_pacing_may_check(p, "other", later));
	g_assert_true(fe_web_pacing_may_check(p, "other", later));
	/* new2 has a bucket of its own */
	g_assert_false(fe_web_pacing_may_check(p, "new2", later));
	fe_web_pacing_free(p);
}

static void test_memory_keeps_suspects(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	const time_t mid = T + 300, later = T + FE_WEB_PEER_MEMORY + 1;

	g_assert_false(fail_n(p, "old", 1, T));
	/* 300 s later its window is long over, but it is still a suspect */
	engage(p, "e", mid);
	g_assert_true(fe_web_pacing_may_check(p, "e0", mid));
	g_assert_false(fe_web_pacing_may_check(p, "old", mid));
	/* FE_WEB_PEER_MEMORY s after its failure it is clean again */
	engage(p, "f", later);
	g_assert_true(fe_web_pacing_may_check(p, "f0", later));
	g_assert_true(fe_web_pacing_may_check(p, "old", later));
	g_assert_true(fe_web_pacing_may_check(p, "old", later));
	fe_web_pacing_free(p);
}

static void test_ceiling_engages_for_suspects(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int i;

	for (i = 0; i < FE_WEB_GLOBAL_MAX_FAILS - 1; i++)
		g_assert_false(fail_n(p, key_n("s", i), 1, T));
	/* not yet engaged */
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 1));
	/* the 20th wrong password within a minute engages it */
	fe_web_pacing_failed(p, "s0", T + 1);
	g_assert_false(fe_web_pacing_may_check(p, "s1", T + 1));
	g_assert_false(fe_web_pacing_may_check(p, "s1", T + 2));
	g_assert_true(fe_web_pacing_may_check(p, "s1", T + 3));
	g_assert_false(fe_web_pacing_may_check(p, "s2", T + 3));
	g_assert_false(fe_web_pacing_may_check(p, "s2", T + 4));
	g_assert_true(fe_web_pacing_may_check(p, "s2", T + 5));
	/* a minute after the last wrong password it is off */
	g_assert_true(fe_web_pacing_may_check(p, "s3", T + 62));
	g_assert_true(fe_web_pacing_may_check(p, "s4", T + 62));
	fe_web_pacing_free(p);
}

static void test_ceiling_spares_clean_and_trusted(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	fe_web_pacing_succeeded(p, "trusted");
	engage(p, "s", T);
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 1));
	g_assert_false(fe_web_pacing_may_check(p, "s1", T + 1));
	/* clean: no wrong password remembered */
	g_assert_true(fe_web_pacing_may_check(p, "fresh", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "fresh", T + 1));
	/* trusted: only its own bucket, even with wrong passwords */
	g_assert_true(fe_web_pacing_may_check(p, "trusted", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "trusted", T + 1));
	g_assert_false(fail_n(p, "trusted", FE_WEB_MAX_FAILS - 1, T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "trusted", T + 1));
	g_assert_false(fe_web_pacing_in_overflow(p, "trusted"));
	/* a wrong password does not trust: fresh is a suspect now */
	fe_web_pacing_failed(p, "fresh", T + 1);
	g_assert_false(fe_web_pacing_may_check(p, "fresh", T + 1));
	fe_web_pacing_free(p);
}

/* A trusted peer is held only by its own bucket, but its wrong passwords
 * count towards the ceiling all the same */
static void test_trusted_failures_count_towards_ceiling(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int i;

	fe_web_pacing_succeeded(p, "t");
	for (i = 0; i < FE_WEB_GLOBAL_MAX_FAILS - 1; i++)
		g_assert_false(fail_n(p, key_n("s", i), 1, T));
	g_assert_false(fail_n(p, "t", 1, T));
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 1));
	g_assert_false(fe_web_pacing_may_check(p, "s1", T + 1));
	/* and it is not held itself */
	g_assert_true(fe_web_pacing_may_check(p, "t", T + 1));
	fe_web_pacing_free(p);
}

/* A trusted peer that logs in again starts with a fresh bucket */
static void test_trusted_refresh_resets_bucket(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	fe_web_pacing_succeeded(p, "t");
	g_assert_true(fail_n(p, "t", FE_WEB_MAX_FAILS, T));
	g_assert_false(fe_web_pacing_may_check(p, "t", T + 1));
	fe_web_pacing_succeeded(p, "t");
	g_assert_true(fe_web_pacing_may_check(p, "t", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "t", T + 1));
	g_assert_false(fail_n(p, "t", FE_WEB_MAX_FAILS - 1, T + 1));
	fe_web_pacing_free(p);
}

/* A check refused by one bucket is not recorded in the other */
static void test_decide_then_record(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int i;

	engage(p, "s", T);
	/* g paced in its own bucket at T, without checks */
	for (i = 0; i < FE_WEB_MAX_FAILS; i++)
		fe_web_pacing_failed(p, "g", T);
	/* refused by its own bucket: the ceiling's slot stays free */
	g_assert_false(fe_web_pacing_may_check(p, "g", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 1));
	/* refused by the ceiling: its own bucket keeps its last check (T) */
	g_assert_false(fe_web_pacing_may_check(p, "g", T + 2));
	g_assert_true(fe_web_pacing_may_check(p, "g", T + 3));
	fe_web_pacing_free(p);
}

static void test_trusted_set_oldest_replaced(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int i;

	for (i = 0; i < FE_WEB_PACING_MAX_TRUSTED; i++)
		fe_web_pacing_succeeded(p, key_n("t", i));
	/* t0 logs in again: the newest now; t1 is the oldest */
	fe_web_pacing_succeeded(p, "t0");
	fe_web_pacing_succeeded(p, key_n("t", FE_WEB_PACING_MAX_TRUSTED));
	engage(p, "s", T);
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 1));
	/* t1 is no longer trusted: a wrong password makes it a suspect */
	fe_web_pacing_failed(p, "t1", T + 1);
	fe_web_pacing_failed(p, "t0", T + 1);
	fe_web_pacing_failed(p, "t2", T + 1);
	g_assert_false(fe_web_pacing_may_check(p, "t1", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "t0", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "t2", T + 1));
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, FE_WEB_GLOBAL_MAX_FAILS + 1);
	fe_web_pacing_free(p);
}

/* A clock that goes back never locks anyone out or keeps the table full */
static void test_backward_clock(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();

	g_assert_true(fail_n(p, "uid:1001", FE_WEB_MAX_FAILS, T));
	g_assert_false(fe_web_pacing_may_check(p, "uid:1001", T + 1));
	g_assert_true(fe_web_pacing_may_check(p, "uid:1001", T - 100));
	fe_web_pacing_reset(p);

	fill_table(p);
	g_assert_false(fail_n(p, "new", 1, T - 1000));
	g_assert_false(fe_web_pacing_in_overflow(p, "new"));
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, 1);
	fe_web_pacing_reset(p);

	/* the ceiling: engaged and used at T + 1 */
	engage(p, "s", T);
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 1));
	g_assert_false(fe_web_pacing_may_check(p, "s1", T + 1));
	fe_web_pacing_failed(p, "s1", T - 10);
	g_assert_true(fe_web_pacing_may_check(p, "s1", T - 10));
	fe_web_pacing_reset(p);

	/* the ceiling's last check after the clock, its window not over */
	engage(p, "s", T);
	g_assert_true(fe_web_pacing_may_check(p, "s0", T + 50));
	g_assert_true(fe_web_pacing_may_check(p, "s1", T + 10));
	fe_web_pacing_free(p);
}

/* 'keys' peers in turn, each trying once every second from T for
 * 'seconds' seconds, every check a wrong password; the checks made */
static int probe(FE_WEB_PACING *p, int keys, int seconds)
{
	int checks = 0, i;
	time_t t;

	for (t = T; t < T + seconds; t++) {
		for (i = 0; i < keys; i++) {
			char key[64];

			g_snprintf(key, sizeof(key), "ip:2001:db8:0:%x::/64", i);
			if (fe_web_pacing_may_check(p, key, t)) {
				checks++;
				fe_web_pacing_failed(p, key, t);
			}
		}
	}
	return checks;
}

/* Many /64s guessing together. The bound: each table entry gives one
 * check while its peer is clean, and an entry is dropped only
 * FE_WEB_PEER_MEMORY s after its last failure, so at most
 * FE_WEB_PACING_MAX_PEERS clean checks per FE_WEB_PEER_MEMORY + 1
 * seconds. Every other check is a suspect's: at most
 * FE_WEB_GLOBAL_MAX_FAILS before the ceiling engages (each is a wrong
 * password that counts), then one per FE_WEB_GLOBAL_CHECK_INTERVAL. */
static int probe_bound(int seconds)
{
	int periods = 1 + seconds / (FE_WEB_PEER_MEMORY + 1);

	return periods * FE_WEB_PACING_MAX_PEERS + FE_WEB_GLOBAL_MAX_FAILS +
	       (seconds + FE_WEB_GLOBAL_CHECK_INTERVAL - 1) / FE_WEB_GLOBAL_CHECK_INTERVAL;
}

static void test_distributed_probe_bounded(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int checks;

	/* 300 keys for 600 s: at most 256 + 20 + 300 = 576 (0.96/s) */
	checks = probe(p, 300, 600);
	g_test_message("300 keys, 600 s: %d checks (bound %d)", checks, probe_bound(600));
	g_assert_cmpint(probe_bound(600), ==, 576);
	g_assert_cmpint(checks, <=, probe_bound(600));
	/* paced, not locked: the ceiling's slots are used */
	g_assert_cmpint(checks, >=,
	                FE_WEB_PACING_MAX_PEERS + 600 / FE_WEB_GLOBAL_CHECK_INTERVAL - 1);
	g_assert_cmpuint(fe_web_pacing_table_size(p), ==, FE_WEB_PACING_MAX_PEERS);

	/* and for 1800 s, with the table drained twice on the way */
	fe_web_pacing_reset(p);
	checks = probe(p, 300, 1800);
	g_test_message("300 keys, 1800 s: %d checks (bound %d)", checks, probe_bound(1800));
	g_assert_cmpint(checks, <=, probe_bound(1800));
	fe_web_pacing_free(p);
}

/* One address guessing as fast as it can: its own bucket and the ceiling
 * give it one check every 2 s after the first 5; a trusted peer checking
 * every second and a clean one are not held */
static void test_single_address_probe(void)
{
	FE_WEB_PACING *p = fe_web_pacing_new();
	int checks = 0, trusted = 0, i;
	time_t t;

	fe_web_pacing_succeeded(p, "uid:1000");
	for (t = T; t < T + 600; t++) {
		for (i = 0; i < 10; i++) {
			if (fe_web_pacing_may_check(p, "ip:192.0.2.7", t)) {
				checks++;
				fe_web_pacing_failed(p, "ip:192.0.2.7", t);
			}
		}
		if (fe_web_pacing_may_check(p, "uid:1000", t))
			trusted++;
	}
	g_test_message("one address, 600 s: %d checks", checks);
	g_assert_cmpint(checks, <=, FE_WEB_MAX_FAILS + 600 / FE_WEB_CHECK_INTERVAL);
	g_assert_cmpint(trusted, ==, 600);
	g_assert_true(fe_web_pacing_may_check(p, "ip:192.0.2.99", t));
	g_assert_true(fe_web_pacing_may_check(p, "ip:192.0.2.99", t));
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

/* Every loopback address is one peer: they all are this machine, and a
 * local user can pick any of 127/8 as the source */
static void test_peer_key_loopback_shared(void)
{
	assert_key("127.0.0.1", "ip:loopback");
	assert_key("127.0.0.2", "ip:loopback");
	assert_key("127.255.1.2", "ip:loopback");
	assert_key("::1", "ip:loopback");
	assert_key("::ffff:127.9.9.9", "ip:loopback");
	assert_key("192.0.2.8", "ip:192.0.2.8");
	assert_key("2001:db8::2", "ip:2001:db8::/64");
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

/* The uid of a TCP loopback peer, from /proc/net/tcp and /proc/net/tcp6.
 *
 * The kernel prints each address as %08X of its raw 32-bit words, in host
 * order (0100007F for 127.0.0.1 on x86), the port as %04X, and the uid as
 * the 8th field. The fixtures are made from the raw words of the
 * addresses, so they are right on any byte order; the lines captured on
 * an x86 machine are used only on little-endian ones. */

static const char HEADER4[] =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  "
    "timeout inode                                                     \n";
static const char HEADER6[] =
    "  sl  local_address                         remote_address                        st "
    "tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

static guint32 raw_word(const void *addr, int i)
{
	guint32 w;

	memcpy(&w, (const guchar *) addr + 4 * i, 4);
	return w;
}

static struct in_addr addr4(const char *s)
{
	struct in_addr a;

	g_assert_cmpint(inet_pton(AF_INET, s, &a), ==, 1);
	return a;
}

static struct in6_addr addr6(const char *s)
{
	struct in6_addr a;

	g_assert_cmpint(inet_pton(AF_INET6, s, &a), ==, 1);
	return a;
}

/* One line of /proc/net/tcp: the socket at laddr:lport connected to
 * raddr:rport, in 'state', of 'uid' */
static char *line4(int sl, const char *laddr, int lport, const char *raddr, int rport, int state,
                   unsigned long uid)
{
	struct in_addr l = addr4(laddr), r = addr4(raddr);

	return g_strdup_printf("%4d: %08X:%04X %08X:%04X %02X 00000000:00000000 00:00000000 "
	                       "00000000 %5lu        0 4242 1 0000000000000000 20 0 0 10 -1\n",
	                       sl, (unsigned int) raw_word(&l, 0), lport,
	                       (unsigned int) raw_word(&r, 0), rport, state, uid);
}

/* One line of /proc/net/tcp6 */
static char *line6(int sl, const char *laddr, int lport, const char *raddr, int rport, int state,
                   unsigned long uid)
{
	struct in6_addr l = addr6(laddr), r = addr6(raddr);

	return g_strdup_printf("%4d: %08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X %02X "
	                       "00000000:00000000 00:00000000 00000000 %5lu        0 4242 1 "
	                       "0000000000000000 20 0 0 10 -1\n",
	                       sl, (unsigned int) raw_word(&l, 0), (unsigned int) raw_word(&l, 1),
	                       (unsigned int) raw_word(&l, 2), (unsigned int) raw_word(&l, 3),
	                       lport, (unsigned int) raw_word(&r, 0),
	                       (unsigned int) raw_word(&r, 1), (unsigned int) raw_word(&r, 2),
	                       (unsigned int) raw_word(&r, 3), rport, state, uid);
}

/* fe_web_proc_net_tcp_uid() for the IPv4 socket at peer:pport connected
 * to local:lport; *uid is left as it is unless found */
static gboolean find4(const char *table, const char *peer, int pport, const char *local,
                      int lport, uid_t *uid)
{
	struct in_addr p = addr4(peer), l = addr4(local);

	return fe_web_proc_net_tcp_uid(table, AF_INET, &p, pport, &l, lport, uid);
}

static gboolean find6(const char *table, const char *peer, int pport, const char *local,
                      int lport, uid_t *uid)
{
	struct in6_addr p = addr6(peer), l = addr6(local);

	return fe_web_proc_net_tcp_uid(table, AF_INET6, &p, pport, &l, lport, uid);
}

/* the uid fe_web_proc_net_tcp_uid() finds for 127.0.0.1:40000 connected
 * to 127.0.0.1:9001 in 'table', or -1 */
static long uid4(const char *table)
{
	uid_t uid = 4242;

	if (!find4(table, "127.0.0.1", 40000, "127.0.0.1", 9001, &uid)) {
		g_assert_cmpuint(uid, ==, 4242); /* untouched */
		return -1;
	}
	return (long) uid;
}

/* a table: the header, then the lines (each freed) */
static char *table_of(const char *header, char *line, ...)
{
	GString *s = g_string_new(header);
	va_list va;

	va_start(va, line);
	while (line != NULL) {
		g_string_append(s, line);
		g_free(line);
		line = va_arg(va, char *);
	}
	va_end(va);
	return g_string_free(s, FALSE);
}

/* header, our listener, our side of the connection (uid 1000) and the
 * peer's side (uid 1001): 127.0.0.1:40000 connected to 127.0.0.1:9001 */
static char *standard4(void)
{
	return table_of(HEADER4, line4(0, "127.0.0.1", 9001, "0.0.0.0", 0, 0x0A, 1000),
	                line4(1, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001),
	                line4(2, "127.0.0.1", 9001, "127.0.0.1", 40000, 0x01, 1000), NULL);
}

static void test_proc_net_tcp_v4(void)
{
	char *table = standard4();
	uid_t uid = 0;

	g_assert_true(find4(table, "127.0.0.1", 40000, "127.0.0.1", 9001, &uid));
	g_assert_cmpuint(uid, ==, 1001);
	/* the other way round: our own side of the connection */
	uid = 0;
	g_assert_true(find4(table, "127.0.0.1", 9001, "127.0.0.1", 40000, &uid));
	g_assert_cmpuint(uid, ==, 1000);
	g_free(table);
}

static void test_proc_net_tcp_v6(void)
{
	char *table;
	uid_t uid = 0;

	table = table_of(HEADER6, line6(0, "::1", 9001, "::", 0, 0x0A, 1000),
	                 line6(1, "::1", 40000, "::1", 9001, 0x01, 1001),
	                 line6(2, "::1", 9001, "::1", 40000, 0x01, 1000), NULL);
	g_assert_true(find6(table, "::1", 40000, "::1", 9001, &uid));
	g_assert_cmpuint(uid, ==, 1001);
	uid = 0;
	g_assert_true(find6(table, "::1", 9001, "::1", 40000, &uid));
	g_assert_cmpuint(uid, ==, 1000);
	/* the last word differs: ::1 against ::2 */
	g_assert_false(find6(table, "::2", 40000, "::1", 9001, &uid));
	g_assert_false(find6(table, "::1", 40000, "::2", 9001, &uid));
	/* the first word differs */
	g_assert_false(find6(table, "2001:db8::1", 40000, "::1", 9001, &uid));
	/* an IPv4 table is not read as IPv6, nor the other way */
	g_assert_false(find4(table, "127.0.0.1", 40000, "127.0.0.1", 9001, &uid));
	g_free(table);
	table = standard4();
	g_assert_false(find6(table, "::ffff:127.0.0.1", 40000, "::ffff:127.0.0.1", 9001, &uid));
	g_free(table);
}

/* Lines as this machine's kernel printed them (x86, little-endian), for
 * connections on loopback; the peer's uid made 1001 where marked, to tell
 * the two sides apart (both were the same user) */
static void test_proc_net_tcp_captured(void)
{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	static const char tcp[] =
	    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  "
	    "timeout inode                                                     \n"
	    "   27: 0100007F:E21F 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000"
	    "        0 110716981 1 0000000000000000 100 0 0 10 0                  \n"
	    /* the peer, 127.0.0.1:47210 -> 127.0.0.1:57887 (uid made 1001) */
	    "   46: 0100007F:B86A 0100007F:E21F 01 00000000:00000000 00:00000000 00000000  1001"
	    "        0 110716982 2 0000000000000000 20 0 0 10 -1                 \n"
	    /* an earlier connection with the same ports, closed: TIME_WAIT
	     * shows uid 0 */
	    "   45: 0100007F:B86A 0100007F:E21F 06 00000000:00000000 03:00001770 00000000     0"
	    "        0 0 3 0000000000000000                                      \n"
	    "   70: 0100007F:E21F 0100007F:B86A 01 00000000:00000000 00:00000000 00000000  1000"
	    "        0 110716983 1 0000000000000000 20 0 0 10 -1                 \n"
	    /* the IPv4 side of a client of a dual-stack listener (::) */
	    "   39: 0100007F:9DDC 0100007F:8195 01 00000000:00000000 00:00000000 00000000  1000"
	    "        0 110716990 2 0000000000000000 20 0 0 10 -1                 \n";
	static const char tcp6[] =
	    "  sl  local_address                         remote_address                        st "
	    "tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
	    "   11: 00000000000000000000000001000000:E21F 00000000000000000000000000000000:0000 0A "
	    "00000000:00000000 00:00000000 00000000  1000        0 110716986 1 0000000000000000 "
	    "100 0 0 10 0\n"
	    "   17: 00000000000000000000000001000000:E21F 00000000000000000000000001000000:C7F4 01 "
	    "00000000:00000000 00:00000000 00000000  1000        0 110716988 1 0000000000000000 "
	    "20 0 0 10 -1\n"
	    /* the peer ::1:51188 -> ::1:57887 (uid made 1001) */
	    "   19: 00000000000000000000000001000000:C7F4 00000000000000000000000001000000:E21F 01 "
	    "00000000:00000000 00:00000000 00000000  1001        0 110716987 2 0000000000000000 "
	    "20 0 0 10 -1\n"
	    /* the dual-stack listener's side: ::ffff:127.0.0.1:33173 ->
	     * ::ffff:127.0.0.1:40412 */
	    "    9: 0000000000000000FFFF00000100007F:8195 0000000000000000FFFF00000100007F:9DDC 01 "
	    "00000000:00000000 00:00000000 00000000  1000        0 110716991 1 0000000000000000 "
	    "20 0 0 10 -1\n"
	    /* a dual-stack client (an IPv6 socket connected to
	     * ::ffff:127.0.0.1) that closed: FIN_WAIT2 shows uid 0 */
	    "   21: 0000000000000000FFFF00000100007F:9FAC 0000000000000000FFFF00000100007F:E3D9 05 "
	    "00000000:00000000 03:00001769 00000000     0        0 0 3 0000000000000000\n";
	struct in6_addr m;
	uid_t uid = 0;

	g_assert_true(find4(tcp, "127.0.0.1", 47210, "127.0.0.1", 57887, &uid));
	g_assert_cmpuint(uid, ==, 1001);
	g_assert_true(find4(tcp, "127.0.0.1", 57887, "127.0.0.1", 47210, &uid));
	g_assert_cmpuint(uid, ==, 1000);
	g_assert_true(find4(tcp, "127.0.0.1", 40412, "127.0.0.1", 33173, &uid));
	g_assert_cmpuint(uid, ==, 1000);
	uid = 0;
	g_assert_true(find6(tcp6, "::1", 51188, "::1", 57887, &uid));
	g_assert_cmpuint(uid, ==, 1001);
	g_assert_true(find6(tcp6, "::ffff:127.0.0.1", 33173, "::ffff:127.0.0.1", 40412, &uid));
	g_assert_cmpuint(uid, ==, 1000);
	/* not ESTABLISHED */
	uid = 4242;
	g_assert_false(find6(tcp6, "::ffff:127.0.0.1", 40876, "::ffff:127.0.0.1", 58329, &uid));
	g_assert_cmpuint(uid, ==, 4242);
	/* the byte order matters: 7F000001 is not 127.0.0.1 here */
	m = addr6("::ffff:127.0.0.1");
	g_assert_cmpuint(raw_word(&m, 3), ==, 0x0100007F);
#else
	g_test_skip("lines captured on a little-endian machine");
#endif
}

/* Only the line with both the peer's and our address and port counts */
static void test_proc_net_tcp_exact_tuple(void)
{
	char *decoys, *table, *line;

	decoys = table_of(HEADER4,
	                  /* the peer's port on another address */
	                  line4(0, "127.0.0.2", 40000, "127.0.0.1", 9001, 0x01, 1002),
	                  /* the peer's address and port, connected elsewhere */
	                  line4(1, "127.0.0.1", 40000, "127.0.0.1", 9002, 0x01, 1003),
	                  line4(2, "127.0.0.1", 40000, "127.0.0.3", 9001, 0x01, 1004),
	                  /* our address and port, from another peer */
	                  line4(3, "127.0.0.1", 40001, "127.0.0.1", 9001, 0x01, 1005),
	                  line4(4, "127.0.0.4", 40000, "127.0.0.1", 9001, 0x01, 1006),
	                  /* listening on the peer's port */
	                  line4(5, "127.0.0.1", 40000, "0.0.0.0", 0, 0x0A, 1007), NULL);
	g_assert_cmpint(uid4(decoys), ==, -1);
	line = line4(6, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001);
	table = g_strconcat(decoys, line, NULL);
	g_assert_cmpint(uid4(table), ==, 1001);
	g_free(line);
	g_free(decoys);
	g_free(table);
}

/* Only a connection in state ESTABLISHED (01) */
static void test_proc_net_tcp_established_only(void)
{
	int state;

	for (state = 0; state <= 0xFF; state++) {
		char *table = table_of(HEADER4,
		                       line4(0, "127.0.0.1", 40000, "127.0.0.1", 9001, state, 1001),
		                       NULL);

		g_assert_cmpint(uid4(table), ==, state == 0x01 ? 1001 : -1);
		g_free(table);
	}
}

/* Two lines for one connection: not known */
static void test_proc_net_tcp_duplicate(void)
{
	char *table;

	table = table_of(HEADER4, line4(0, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001),
	                 line4(1, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1000), NULL);
	g_assert_cmpint(uid4(table), ==, -1);
	g_free(table);
	table = table_of(HEADER4, line4(0, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001),
	                 line4(7, "127.0.0.1", 9001, "127.0.0.1", 40000, 0x01, 1000),
	                 line4(9, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001), NULL);
	g_assert_cmpint(uid4(table), ==, -1);
	g_free(table);
}

static void test_proc_net_tcp_missing(void)
{
	char *table = standard4();
	uid_t uid = 4242;

	g_assert_false(find4(table, "127.0.0.1", 40002, "127.0.0.1", 9001, &uid));
	g_assert_false(find4(table, "127.0.0.1", 40000, "127.0.0.1", 9002, &uid));
	g_assert_false(find4("", "127.0.0.1", 40000, "127.0.0.1", 9001, &uid));
	g_assert_false(find4(HEADER4, "127.0.0.1", 40000, "127.0.0.1", 9001, &uid));
	g_assert_false(find4(NULL, "127.0.0.1", 40000, "127.0.0.1", 9001, &uid));
	g_assert_cmpuint(uid, ==, 4242);
	g_free(table);
}

/* the peer's line (127.0.0.1:40000 -> 127.0.0.1:9001, uid 1001) with
 * 'from' replaced by 'to' once */
static char *peer_line_with(const char *from, const char *to)
{
	char *line = line4(1, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001);
	char *at = strstr(line, from), *edited;

	g_assert_nonnull(at);
	*at = '\0';
	edited = g_strconcat(line, to, at + strlen(from), NULL);
	g_free(line);
	return edited;
}

static long uid_with(const char *from, const char *to)
{
	char *table = table_of(HEADER4, peer_line_with(from, to), NULL);
	long uid = uid4(table);

	g_free(table);
	return uid;
}

/* Lines the kernel never prints are not read */
static void test_proc_net_tcp_malformed(void)
{
	static const char *const bad_uids[] = {
		"1001x", "-1001", "+1001", "0x3E9", "1001:", "4294967295", "4294967296",
		"18446744073709552617", "99999999999999999999999", NULL
	};
	char *line, *table, *padded;
	int i;

	g_assert_cmpint(uid_with(" 1001 ", " 1001 "), ==, 1001);
	for (i = 0; bad_uids[i] != NULL; i++) {
		char *field = g_strconcat(" ", bad_uids[i], " ", NULL);

		g_assert_cmpint(uid_with(" 1001 ", field), ==, -1);
		g_free(field);
	}
	/* an inode beyond 32 bits is still a number */
	g_assert_cmpint(uid_with(" 4242 ", " 18446744073709551615 "), ==, 1001);
	g_assert_cmpint(uid_with(" 4242 ", " 42x "), ==, -1);
	g_assert_cmpint(uid_with("        0 ", "        x "), ==, -1);
	/* leading zeros are digits all the same */
	g_assert_cmpint(uid_with(" 1001 ", " 0001001 "), ==, 1001);
	/* no newline at the end: maybe cut short */
	g_assert_cmpint(uid_with("\n", ""), ==, -1);
	/* the fields after the uid are missing */
	g_assert_cmpint(uid_with("        0 4242 1 0000000000000000 20 0 0 10 -1", ""), ==, -1);
	g_assert_cmpint(uid_with(" 4242 1 0000000000000000 20 0 0 10 -1", ""), ==, -1);
	/* addresses, ports and the state with too few or too many digits,
	 * or not hex */
	g_assert_cmpint(uid_with(":9C40 ", ":9C4 "), ==, -1);
	g_assert_cmpint(uid_with(":9C40 ", ":09C40 "), ==, -1);
	g_assert_cmpint(uid_with(":2329 ", ":2329X "), ==, -1);
	g_assert_cmpint(uid_with(" 01 ", " 1 "), ==, -1);
	g_assert_cmpint(uid_with(" 01 ", " 001 "), ==, -1);
	g_assert_cmpint(uid_with(" 01 ", " 0x1 "), ==, -1);
	g_assert_cmpint(uid_with(" 01 ", " 1G "), ==, -1);
	g_assert_cmpint(uid_with(" 01 ", " 01G "), ==, -1);
	g_assert_cmpint(uid_with(":2329 ", ":2329 "), ==, 1001);
	/* a port that would be right if a digit too few were taken */
	{
		char *l9 = line4(1, "127.0.0.1", 9, "127.0.0.1", 9001, 0x01, 1001);
		char *at = strstr(l9, ":0009 ");
		uid_t uid = 4242;

		g_assert_nonnull(at);
		memcpy(at, ":009G ", 6);
		table = g_strconcat(HEADER4, l9, NULL);
		g_assert_false(find4(table, "127.0.0.1", 9, "127.0.0.1", 9001, &uid));
		memcpy(at, ":0009 ", 6);
		g_free(table);
		table = g_strconcat(HEADER4, l9, NULL);
		g_assert_true(find4(table, "127.0.0.1", 9, "127.0.0.1", 9001, &uid));
		g_free(table);
		g_free(l9);
	}
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	g_assert_cmpint(uid_with("0100007F:9C40", "100007F:9C40"), ==, -1);
	g_assert_cmpint(uid_with("0100007F:9C40", "00100007F:9C40"), ==, -1);
	g_assert_cmpint(uid_with("0100007F:9C40", "0100007G:9C40"), ==, -1);
	g_assert_cmpint(uid_with("0100007F:9C40", "+100007F:9C40"), ==, -1);
	g_assert_cmpint(uid_with("0100007F:9C40", "0100007F9C40"), ==, -1);
	/* lower case hex is still the same number */
	g_assert_cmpint(uid_with("0100007F:9C40", "0100007f:9c40"), ==, 1001);
#endif
	g_assert_cmpint(uid_with("   1: ", "   1 "), ==, -1);
	g_assert_cmpint(uid_with("   1: ", "   x: "), ==, -1);
	g_assert_cmpint(uid_with("   1: ", "   : "), ==, -1);
	/* fields are separated by spaces, no tabs, no other characters */
	g_assert_cmpint(uid_with(" 01 ", "\t01 "), ==, -1);
	g_assert_cmpint(uid_with(" 1001 ", "\t1001 "), ==, -1);
	g_assert_cmpint(uid_with(" 1001 ", " 1001\t"), ==, -1);

	/* a line longer than any the kernel prints */
	line = line4(1, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001);
	padded = g_strdup_printf("%*s%s", 600, "", line);
	table = g_strconcat(HEADER4, padded, NULL);
	g_assert_cmpint(uid4(table), ==, -1);
	g_free(table);
	g_free(padded);

	/* every line cut short (but ended): never another uid. A line cut
	 * inside the uid or the fields after it is not read. */
	for (i = 0; line[i] != '\0' && line[i] != '\n'; i++) {
		char *cut = g_strndup(line, i);
		long uid;

		table = g_strconcat(HEADER4, cut, "\n", NULL);
		uid = uid4(table);
		g_assert_true(uid == -1 || uid == 1001);
		/* cut before the inode: never read */
		if (strstr(cut, "        0 4") == NULL)
			g_assert_cmpint(uid, ==, -1);
		g_free(table);
		g_free(cut);
	}
	/* every byte of the line changed: no crash, and the uid found is
	 * one the line can say */
	for (i = 0; line[i] != '\0' && line[i] != '\n'; i++) {
		static const char subst[] = { ' ', '0', 'F', 'x', ':', '\t', '-', '9', '\n' };
		gsize k;

		for (k = 0; k < sizeof(subst); k++) {
			char *copy = g_strdup(line);
			long uid;

			copy[i] = subst[k];
			table = g_strconcat(HEADER4, copy, NULL);
			uid = uid4(table);
			g_assert_cmpint(uid, >=, -1);
			g_free(table);
			g_free(copy);
		}
	}
	g_free(line);
}

static void test_proc_net_tcp_bad_arguments(void)
{
	char *table = standard4();
	struct in_addr p = addr4("127.0.0.1"), l = addr4("127.0.0.1");
	uid_t uid = 4242;

	g_assert_false(fe_web_proc_net_tcp_uid(table, AF_UNIX, &p, 40000, &l, 9001, &uid));
	g_assert_false(fe_web_proc_net_tcp_uid(table, AF_INET, &p, -1, &l, 9001, &uid));
	g_assert_false(fe_web_proc_net_tcp_uid(table, AF_INET, &p, 40000, &l, 65536, &uid));
	g_assert_false(fe_web_proc_net_tcp_uid(table, AF_INET, &p, 40000 + 65536, &l, 9001, &uid));
	g_assert_false(fe_web_proc_net_tcp_uid(table, AF_INET, NULL, 40000, &l, 9001, &uid));
	g_assert_false(fe_web_proc_net_tcp_uid(table, AF_INET, &p, 40000, NULL, 9001, &uid));
	g_assert_cmpuint(uid, ==, 4242);
	g_free(table);
}

/* fe_web_tcp_sockaddr_uid(): the connection from a sockaddr, from the
 * files tcp and tcp6 of a directory */

static struct sockaddr_storage sa_of(const char *host, int port)
{
	struct sockaddr_storage ss;

	memset(&ss, 0, sizeof(ss));
	if (strchr(host, ':') != NULL) {
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *) &ss;

		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
		s6->sin6_addr = addr6(host);
	} else {
		struct sockaddr_in *s4 = (struct sockaddr_in *) &ss;

		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
		s4->sin_addr = addr4(host);
	}
	return ss;
}

static void write_table(const char *dir, const char *name, const char *text)
{
	char *path = g_build_filename(dir, name, NULL);

	if (text == NULL)
		unlink(path);
	else
		g_assert_true(g_file_set_contents(path, text, -1, NULL));
	g_free(path);
}

/* the uid fe_web_tcp_sockaddr_uid() finds in 'dir' for peer:pport
 * connected to local:lport, or -1 */
static long sa_uid(const char *dir, const char *peer, int pport, const char *local, int lport)
{
	struct sockaddr_storage p = sa_of(peer, pport), l = sa_of(local, lport);
	uid_t uid = 4242;
	int r;

	r = fe_web_tcp_sockaddr_uid((struct sockaddr *) &p, (struct sockaddr *) &l, dir, 65534,
	                            &uid);
	g_assert_cmpint(r, >=, 0);
	g_assert_cmpint(r, <=, 1);
	if (r == 0) {
		g_assert_cmpuint(uid, ==, 4242);
		return -1;
	}
	return (long) uid;
}

static void test_sockaddr_lookup(void)
{
	char *dir = test_tmpdir();
	char *tcp, *tcp6;

	tcp = table_of(HEADER4, line4(0, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001),
	               line4(1, "127.0.0.1", 9001, "127.0.0.1", 40000, 0x01, 1000),
	               line4(2, "192.0.2.1", 40000, "192.0.2.2", 9001, 0x01, 1001),
	               line4(3, "127.0.0.1", 40003, "192.0.2.2", 9001, 0x01, 1003),
	               line4(4, "127.0.0.1", 40004, "127.0.0.1", 9001, 0x01, 65534),
	               /* no socket is connected to 0.0.0.0:0, but the line
	                * can say so */
	               line4(5, "127.0.0.1", 40020, "0.0.0.0", 0, 0x01, 1020), NULL);
	tcp6 = table_of(HEADER6, line6(0, "::1", 40000, "::1", 9001, 0x01, 1001),
	                /* no socket has these two sides, but the line can say */
	                line6(4, "::1", 40010, "::ffff:127.0.0.1", 9001, 0x01, 1010),
	                line6(1, "::ffff:192.0.2.1", 40001, "::ffff:192.0.2.2", 9001, 0x01, 1001),
	                /* a dual-stack client of an IPv4 listener */
	                line6(2, "::ffff:127.0.0.1", 40005, "::ffff:127.0.0.1", 9001, 0x01, 1005),
	                line6(3, "2001:db8::1", 40000, "2001:db8::2", 9001, 0x01, 1001), NULL);
	write_table(dir, "tcp", tcp);
	write_table(dir, "tcp6", tcp6);

	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, 1001);
	g_assert_cmpint(sa_uid(dir, "::1", 40000, "::1", 9001), ==, 1001);
	/* IPv4-mapped is IPv4: looked for in tcp */
	g_assert_cmpint(sa_uid(dir, "::ffff:127.0.0.1", 40000, "::ffff:127.0.0.1", 9001), ==, 1001);
	/* an IPv4 connection from an IPv6 socket: in tcp6, mapped */
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40005, "127.0.0.1", 9001), ==, 1005);
	g_assert_cmpint(sa_uid(dir, "::ffff:127.0.0.1", 40005, "::ffff:127.0.0.1", 9001), ==, 1005);
	/* only peers on loopback, even when the table has them */
	g_assert_cmpint(sa_uid(dir, "192.0.2.1", 40000, "192.0.2.2", 9001), ==, -1);
	g_assert_cmpint(sa_uid(dir, "::ffff:192.0.2.1", 40001, "::ffff:192.0.2.2", 9001), ==, -1);
	g_assert_cmpint(sa_uid(dir, "2001:db8::1", 40000, "2001:db8::2", 9001), ==, -1);
	/* a loopback peer of a non-loopback address is still looked up */
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40003, "192.0.2.2", 9001), ==, 1003);
	/* the uid the kernel shows for a uid outside our user namespace */
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40004, "127.0.0.1", 9001), ==, -1);
	/* the two sides in different families */
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "::1", 9001), ==, -1);
	g_assert_cmpint(sa_uid(dir, "::1", 40000, "127.0.0.1", 9001), ==, -1);
	g_assert_cmpint(sa_uid(dir, "::1", 40010, "::ffff:127.0.0.1", 9001), ==, -1);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40020, "::1", 0), ==, -1);

	/* in both tables: two sockets for one connection, not known */
	write_table(dir, "tcp6", HEADER6);
	g_free(tcp6);
	tcp6 = table_of(HEADER6,
	                line6(0, "::ffff:127.0.0.1", 40000, "::ffff:127.0.0.1", 9001, 0x01, 1001),
	                NULL);
	write_table(dir, "tcp6", tcp6);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);

	/* no tcp6 (IPv6 off): IPv4 still found, IPv6 not */
	write_table(dir, "tcp6", NULL);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, 1001);
	g_assert_cmpint(sa_uid(dir, "::1", 40000, "::1", 9001), ==, -1);
	/* tcp6 there but unreadable (a directory): not known */
	{
		char *path = g_build_filename(dir, "tcp6", NULL);

		g_assert_cmpint(mkdir(path, 0700), ==, 0);
		g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);
		rmdir(path);
		/* or a file we may not read */
		write_table(dir, "tcp6", HEADER6);
		g_assert_cmpint(chmod(path, 0), ==, 0);
		if (access(path, R_OK) == 0)
			g_test_message("root reads it anyway: not checked");
		else
			g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);
		unlink(path);
		g_free(path);
	}
	/* no table at all */
	write_table(dir, "tcp", NULL);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);
	g_assert_cmpint(sa_uid("/nonexistent-fe-web-dir", "127.0.0.1", 40000, "127.0.0.1", 9001),
	                ==, -1);

	g_free(tcp);
	g_free(tcp6);
	test_rmtree(dir);
	g_free(dir);
}

/* The file is read in pieces: lines across them, lines too long, a file
 * too big */
static void test_sockaddr_file_reading(void)
{
	char *dir = test_tmpdir();
	char *peer = line4(1, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001);
	char *filler = line4(2, "127.0.0.1", 40001, "127.0.0.1", 9001, 0x01, 1002);
	GString *s;
	int n, k;

	/* the peer's line at every offset around the first few pieces */
	for (n = 0; n < 12000; n += 37) {
		s = g_string_new(HEADER4);
		while (s->len < (gsize) n)
			g_string_append(s, filler);
		g_string_append(s, peer);
		g_string_append(s, filler);
		write_table(dir, "tcp", s->str);
		g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, 1001);
		g_string_free(s, TRUE);
	}
	/* a long line of spaces before the peer's line makes one line too
	 * long: never read, nor what is left of it after a piece */
	for (k = 400; k < 20000; k = k * 3 / 2) {
		s = g_string_new(HEADER4);
		g_string_append_printf(s, "%*s%s", k, "", peer);
		write_table(dir, "tcp", s->str);
		g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);
		/* the next line is read again */
		g_string_append(s, peer);
		write_table(dir, "tcp", s->str);
		g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, 1001);
		g_string_free(s, TRUE);
	}
	/* the same with a piece ending at every place in the spaces before
	 * the line: what is left of the long line after the piece is never
	 * read as a line */
	for (k = 3600; k < 8800; k += 13) {
		s = g_string_new(HEADER4);
		g_string_append_printf(s, "%*s%s", k, "", peer);
		write_table(dir, "tcp", s->str);
		g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);
		g_string_free(s, TRUE);
	}
	/* more than FE_WEB_PROC_NET_MAX bytes: not known, even with the line
	 * at the start */
	s = g_string_new(HEADER4);
	g_string_append(s, peer);
	while (s->len <= FE_WEB_PROC_NET_MAX)
		g_string_append(s, filler);
	write_table(dir, "tcp", s->str);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);
	/* just within */
	g_string_truncate(s, FE_WEB_PROC_NET_MAX);
	while (s->len > 0 && s->str[s->len - 1] != '\n')
		g_string_truncate(s, s->len - 1);
	write_table(dir, "tcp", s->str);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, 1001);
	g_string_free(s, TRUE);

	g_free(peer);
	g_free(filler);
	test_rmtree(dir);
	g_free(dir);
}

/* At most FE_WEB_PROC_LOOKUPS_PER_SEC /proc lookups a second, for all
 * peers together */
static void test_proc_budget(void)
{
	FE_WEB_PROC_BUDGET b;
	int i;

	memset(&b, 0, sizeof(b));
	for (i = 0; i < FE_WEB_PROC_LOOKUPS_PER_SEC; i++)
		g_assert_true(fe_web_proc_budget_take(&b, T));
	g_assert_false(fe_web_proc_budget_take(&b, T));
	g_assert_false(fe_web_proc_budget_take(&b, T));
	/* the next second */
	for (i = 0; i < FE_WEB_PROC_LOOKUPS_PER_SEC; i++)
		g_assert_true(fe_web_proc_budget_take(&b, T + 1));
	g_assert_false(fe_web_proc_budget_take(&b, T + 1));
	/* a later second has the whole budget again */
	g_assert_true(fe_web_proc_budget_take(&b, T + 5));
	/* a clock that went back counts as time passed */
	g_assert_true(fe_web_proc_budget_take(&b, T + 5));
	g_assert_false(fe_web_proc_budget_take(&b, T + 5));
	g_assert_true(fe_web_proc_budget_take(&b, T - 100));
}

#ifdef __linux__
/* NETLINK_SOCK_DIAG: the kernel's answer for the one socket */

typedef struct {
	guint32 seq;
	int family, state;
	guint32 uid;
	guchar src[16], dst[16]; /* IPv4: the first 4 bytes */
	int sport, dport;
} DIAG_REPLY;

/* bytes after inet_diag_msg, as the kernel adds attributes */
#define DIAG_ATTRS 28

/* A reply at buf + off; the offset after it */
static gsize put_reply(guchar *buf, gsize off, const DIAG_REPLY *r)
{
	struct nlmsghdr h;
	struct inet_diag_msg d;

	memset(&h, 0, sizeof(h));
	memset(&d, 0, sizeof(d));
	h.nlmsg_len = NLMSG_LENGTH(sizeof(d)) + DIAG_ATTRS;
	h.nlmsg_type = SOCK_DIAG_BY_FAMILY;
	h.nlmsg_seq = r->seq;
	d.idiag_family = r->family;
	d.idiag_state = r->state;
	d.idiag_uid = r->uid;
	d.id.idiag_sport = htons(r->sport);
	d.id.idiag_dport = htons(r->dport);
	memcpy(d.id.idiag_src, r->src, 16);
	memcpy(d.id.idiag_dst, r->dst, 16);
	memset(buf + off, 0, NLMSG_ALIGN(h.nlmsg_len));
	memcpy(buf + off, &h, sizeof(h));
	memcpy(buf + off + NLMSG_HDRLEN, &d, sizeof(d));
	return off + NLMSG_ALIGN(h.nlmsg_len);
}

/* An NLMSG_ERROR with 'error' at buf + off */
static gsize put_error(guchar *buf, gsize off, guint32 seq, int error)
{
	struct nlmsghdr h;
	struct nlmsgerr e;

	memset(&h, 0, sizeof(h));
	memset(&e, 0, sizeof(e));
	h.nlmsg_len = NLMSG_LENGTH(sizeof(e));
	h.nlmsg_type = NLMSG_ERROR;
	h.nlmsg_seq = seq;
	e.error = error;
	memcpy(buf + off, &h, sizeof(h));
	memcpy(buf + off + NLMSG_HDRLEN, &e, sizeof(e));
	return off + NLMSG_ALIGN(h.nlmsg_len);
}

/* the kernel's reply for 127.0.0.1:40000 connected to 127.0.0.1:9001 */
static DIAG_REPLY reply4(void)
{
	DIAG_REPLY r;
	struct in_addr a = addr4("127.0.0.1");

	memset(&r, 0, sizeof(r));
	r.seq = 77;
	r.family = AF_INET;
	r.state = 1;
	r.uid = 1001;
	memcpy(r.src, &a, 4);
	memcpy(r.dst, &a, 4);
	r.sport = 40000;
	r.dport = 9001;
	return r;
}

/* the uid fe_web_sock_diag_reply() takes from buf for that connection
 * (asked as 'family': AF_INET, or AF_INET6 IPv4-mapped), or -1 */
static long diag_uid_as(const guchar *buf, gsize len, int family)
{
	struct in_addr a = addr4("127.0.0.1");
	struct in6_addr m = addr6("::ffff:127.0.0.1");
	const void *addr = family == AF_INET ? (const void *) &a : (const void *) &m;
	uid_t uid = 4242;

	if (fe_web_sock_diag_reply(buf, len, family, addr, 40000, addr, 9001, 77, 65534, &uid) !=
	    1) {
		g_assert_cmpuint(uid, ==, 4242);
		return -1;
	}
	return (long) uid;
}

static long diag_uid(const guchar *buf, gsize len)
{
	return diag_uid_as(buf, len, AF_INET);
}

static long diag_uid_of(const DIAG_REPLY *r)
{
	guchar buf[512];

	return diag_uid(buf, put_reply(buf, 0, r));
}

static void test_sock_diag_request(void)
{
	guint32 buf[64];
	struct nlmsghdr h;
	struct inet_diag_req_v2 q;
	struct in_addr p = addr4("127.0.0.1"), l = addr4("127.0.0.2");
	struct in6_addr p6 = addr6("::1"), l6 = addr6("2001:db8::1");
	gsize len;

	len = fe_web_sock_diag_request(buf, sizeof(buf), AF_INET, &p, 40000, &l, 9001, 77);
	g_assert_cmpuint(len, ==, NLMSG_LENGTH(sizeof(q)));
	memcpy(&h, buf, sizeof(h));
	memcpy(&q, (guchar *) buf + NLMSG_HDRLEN, sizeof(q));
	g_assert_cmpuint(h.nlmsg_len, ==, len);
	g_assert_cmpuint(h.nlmsg_type, ==, SOCK_DIAG_BY_FAMILY);
	/* one socket, no dump */
	g_assert_cmpuint(h.nlmsg_flags, ==, NLM_F_REQUEST);
	g_assert_cmpuint(h.nlmsg_seq, ==, 77);
	g_assert_cmpuint(h.nlmsg_pid, ==, 0);
	g_assert_cmpuint(q.sdiag_family, ==, AF_INET);
	g_assert_cmpuint(q.sdiag_protocol, ==, IPPROTO_TCP);
	g_assert_cmpuint(q.idiag_ext, ==, 0);
	g_assert_cmpuint(ntohs(q.id.idiag_sport), ==, 40000);
	g_assert_cmpuint(ntohs(q.id.idiag_dport), ==, 9001);
	g_assert_cmpmem(q.id.idiag_src, 4, &p, 4);
	g_assert_cmpmem(q.id.idiag_dst, 4, &l, 4);
	g_assert_cmpuint(q.id.idiag_src[1] | q.id.idiag_src[2] | q.id.idiag_src[3], ==, 0);
	g_assert_cmpuint(q.id.idiag_if, ==, 0);
	g_assert_cmpuint(q.id.idiag_cookie[0], ==, INET_DIAG_NOCOOKIE);
	g_assert_cmpuint(q.id.idiag_cookie[1], ==, INET_DIAG_NOCOOKIE);

	len = fe_web_sock_diag_request(buf, sizeof(buf), AF_INET6, &p6, 1, &l6, 65535, 78);
	g_assert_cmpuint(len, ==, NLMSG_LENGTH(sizeof(q)));
	memcpy(&q, (guchar *) buf + NLMSG_HDRLEN, sizeof(q));
	g_assert_cmpuint(q.sdiag_family, ==, AF_INET6);
	g_assert_cmpmem(q.id.idiag_src, 16, &p6, 16);
	g_assert_cmpmem(q.id.idiag_dst, 16, &l6, 16);
	g_assert_cmpuint(ntohs(q.id.idiag_sport), ==, 1);
	g_assert_cmpuint(ntohs(q.id.idiag_dport), ==, 65535);

	/* no room, no such family, no such port */
	g_assert_cmpuint(fe_web_sock_diag_request(buf, NLMSG_LENGTH(sizeof(q)) - 1, AF_INET, &p,
	                                          40000, &l, 9001, 77), ==, 0);
	g_assert_cmpuint(
	    fe_web_sock_diag_request(buf, sizeof(buf), AF_UNIX, &p, 40000, &l, 9001, 77), ==, 0);
	g_assert_cmpuint(
	    fe_web_sock_diag_request(buf, sizeof(buf), AF_INET, &p, 65536, &l, 9001, 77), ==, 0);
	g_assert_cmpuint(fe_web_sock_diag_request(buf, sizeof(buf), AF_INET, &p, 40000, &l, -1, 77),
	                 ==, 0);
}

static void test_sock_diag_reply(void)
{
	DIAG_REPLY r;
	struct in6_addr a6 = addr6("::1"), m = addr6("::ffff:127.0.0.1");
	guchar buf[512];
	uid_t uid = 4242;
	int state;

	r = reply4();
	g_assert_cmpint(diag_uid_of(&r), ==, 1001);

	/* IPv6 */
	r.family = AF_INET6;
	memcpy(r.src, &a6, 16);
	memcpy(r.dst, &a6, 16);
	g_assert_cmpint(fe_web_sock_diag_reply(buf, put_reply(buf, 0, &r), AF_INET6, &a6, 40000,
	                                       &a6, 9001, 77, 65534, &uid), ==, 1);
	g_assert_cmpuint(uid, ==, 1001);
	/* an IPv6 socket is not an IPv4 one */
	g_assert_cmpint(diag_uid_of(&r), ==, -1);

	/* a dual-stack client: asked as IPv4, the socket is IPv6 with
	 * mapped addresses */
	memcpy(r.src, &m, 16);
	memcpy(r.dst, &m, 16);
	g_assert_cmpint(diag_uid_of(&r), ==, 1001);
	/* asked as IPv4-mapped, the socket is IPv4 */
	r = reply4();
	g_assert_cmpint(diag_uid_as(buf, put_reply(buf, 0, &r), AF_INET6), ==, 1001);
	/* asked for ::1, an IPv4 answer */
	uid = 4242;
	g_assert_cmpint(fe_web_sock_diag_reply(buf, put_reply(buf, 0, &r), AF_INET6, &a6, 40000,
	                                       &a6, 9001, 77, 65534, &uid), ==, 0);
	g_assert_cmpuint(uid, ==, 4242);

	/* only ESTABLISHED: the kernel answers with a listener (10) on the
	 * port, or a closed connection (FIN_WAIT2 5, TIME_WAIT 6) */
	for (state = 0; state <= 0xff; state++) {
		r = reply4();
		r.state = state;
		g_assert_cmpint(diag_uid_of(&r), ==, state == 1 ? 1001 : -1);
	}
	/* the socket the kernel found is the one asked for */
	r = reply4();
	r.sport = 40001;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	r = reply4();
	r.dport = 0; /* a listener */
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	r = reply4();
	r.src[3] = 2;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	r = reply4();
	r.dst[0] = 0;
	r.dst[3] = 0;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	/* an IPv6 socket whose addresses start with the 4 bytes of
	 * 127.0.0.1 (and the rest zero) is not that IPv4 one */
	r = reply4();
	r.family = AF_INET6;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	/* IPv4 is the first 4 bytes only */
	r = reply4();
	r.src[4] = 1;
	g_assert_cmpint(diag_uid_of(&r), ==, 1001);
	/* our request */
	r = reply4();
	r.seq = 78;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	/* not a family the request had */
	r = reply4();
	r.family = AF_UNIX;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	/* the uid shown for one outside our user namespace, and -1 */
	r = reply4();
	r.uid = 65534;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	r.uid = 0xffffffff;
	g_assert_cmpint(diag_uid_of(&r), ==, -1);
	r.uid = 0;
	g_assert_cmpint(diag_uid_of(&r), ==, 0);
}

/* Replies that are not one socket */
static void test_sock_diag_reply_malformed(void)
{
	DIAG_REPLY r = reply4();
	struct nlmsghdr h;
	guchar buf[1024];
	gsize len, one, i;

	/* not found */
	g_assert_cmpint(diag_uid(buf, put_error(buf, 0, 77, -ENOENT)), ==, -1);
	g_assert_cmpint(diag_uid(buf, put_error(buf, 0, 77, 0)), ==, -1);
	/* two sockets, or one and an error */
	len = put_reply(buf, put_reply(buf, 0, &r), &r);
	g_assert_cmpint(diag_uid(buf, len), ==, -1);
	len = put_error(buf, put_reply(buf, 0, &r), 77, -ENOENT);
	g_assert_cmpint(diag_uid(buf, len), ==, -1);
	len = put_reply(buf, put_error(buf, 0, 77, -ENOENT), &r);
	g_assert_cmpint(diag_uid(buf, len), ==, -1);

	/* another type: NLMSG_DONE, NLMSG_NOOP, anything */
	one = put_reply(buf, 0, &r);
	memcpy(&h, buf, sizeof(h));
	h.nlmsg_type = NLMSG_DONE;
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, one), ==, -1);
	h.nlmsg_type = NLMSG_NOOP;
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, one), ==, -1);
	h.nlmsg_type = 0x1234;
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, one), ==, -1);

	/* cut short anywhere */
	one = put_reply(buf, 0, &r);
	for (i = 0; i < one; i++)
		g_assert_cmpint(diag_uid(buf, i), ==, -1);
	g_assert_cmpint(diag_uid(buf, one), ==, 1001);
	/* bytes after it */
	memset(buf + one, 0, 16);
	for (i = 1; i <= 16; i++)
		g_assert_cmpint(diag_uid(buf, one + i), ==, -1);
	/* nlmsg_len longer than what came, too short for the message, too
	 * short for a header, 0 */
	memcpy(&h, buf, sizeof(h));
	h.nlmsg_len = one + 4;
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, one), ==, -1);
	h.nlmsg_len = NLMSG_LENGTH(sizeof(struct inet_diag_msg)) - 1;
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, one), ==, -1);
	g_assert_cmpint(diag_uid(buf, h.nlmsg_len), ==, -1);
	h.nlmsg_len = NLMSG_LENGTH(sizeof(struct inet_diag_msg));
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, NLMSG_LENGTH(sizeof(struct inet_diag_msg))), ==, 1001);
	/* a message whose length is not a multiple of 4, ending where the
	 * reply ends: read, and nothing after it */
	h.nlmsg_len = one - 1;
	memcpy(buf, &h, sizeof(h));
	memset(buf + one - 1, 0xff, 64);
	g_assert_cmpint(diag_uid(buf, one - 1), ==, 1001);
	h.nlmsg_len = sizeof(h) - 1;
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, one), ==, -1);
	h.nlmsg_len = 0;
	memcpy(buf, &h, sizeof(h));
	g_assert_cmpint(diag_uid(buf, one), ==, -1);
	/* a family the request cannot have */
	{
		struct in_addr a = addr4("127.0.0.1");
		uid_t uid = 4242;

		one = put_reply(buf, 0, &r);
		g_assert_cmpint(fe_web_sock_diag_reply(buf, one, AF_UNIX, &a, 40000, &a, 9001, 77,
		                                       65534, &uid), ==, 0);
		g_assert_cmpuint(uid, ==, 4242);
	}
	/* no reply */
	g_assert_cmpint(diag_uid(buf, 0), ==, -1);
	g_assert_cmpint(diag_uid(NULL, one), ==, -1);
	/* a reply at an odd place in memory */
	one = put_reply(buf + 1, 0, &r);
	g_assert_cmpint(diag_uid(buf + 1, one), ==, 1001);
	/* every byte changed: no crash */
	one = put_reply(buf, 0, &r);
	for (i = 0; i < one; i++) {
		static const guchar subst[] = { 0x00, 0x01, 0x7f, 0x80, 0xff };
		gsize k;

		for (k = 0; k < sizeof(subst); k++) {
			guchar copy[512];

			memcpy(copy, buf, one);
			copy[i] = subst[k];
			g_assert_cmpint(diag_uid(copy, one), >=, -1);
		}
	}
}

/* The sock_diag seam: what the fake kernel answers */
static int fake_calls, fake_families[4];
static int fake_ret;         /* 0: the reply's length */
static int fake_state = 1;
static guint32 fake_uid = 4321;
static int fake_enoent_for_inet; /* AF_INET requests get ENOENT */
static int fake_sport_delta;

static gssize diag_fails(const void *req, gsize req_len, void *reply, gsize size)
{
	(void) req;
	(void) req_len;
	(void) reply;
	(void) size;
	fake_calls++;
	return -1;
}

static gssize diag_fake(const void *req, gsize req_len, void *reply, gsize size)
{
	struct nlmsghdr h;
	struct inet_diag_req_v2 q;
	DIAG_REPLY r;
	gsize len;

	g_assert_cmpuint(req_len, ==, NLMSG_LENGTH(sizeof(q)));
	g_assert_cmpuint(size, >=, 512);
	memcpy(&h, req, sizeof(h));
	memcpy(&q, (const guchar *) req + NLMSG_HDRLEN, sizeof(q));
	if (fake_calls < 4)
		fake_families[fake_calls] = q.sdiag_family;
	fake_calls++;
	if (fake_enoent_for_inet && q.sdiag_family == AF_INET)
		return put_error(reply, 0, h.nlmsg_seq, -ENOENT);
	memset(&r, 0, sizeof(r));
	r.seq = h.nlmsg_seq;
	r.family = q.sdiag_family;
	r.state = fake_state;
	r.uid = fake_uid;
	memcpy(r.src, q.id.idiag_src, 16);
	memcpy(r.dst, q.id.idiag_dst, 16);
	r.sport = ntohs(q.id.idiag_sport) + fake_sport_delta;
	r.dport = ntohs(q.id.idiag_dport);
	len = put_reply(reply, 0, &r);
	if (fake_ret == (int) size + 1) {
		/* a message as long as what came: more than the buffer */
		h.nlmsg_len = size + 1;
		memcpy(reply, &h.nlmsg_len, sizeof(h.nlmsg_len));
	}
	return fake_ret != 0 ? fake_ret : (gssize) len;
}

static void fake_reset(void)
{
	fake_calls = 0;
	memset(fake_families, 0, sizeof(fake_families));
	fake_ret = 0;
	fake_state = 1;
	fake_uid = 4321;
	fake_enoent_for_inet = 0;
	fake_sport_delta = 0;
	fe_web_sock_diag_set_func(diag_fake);
}

static int live_pair(const char *listen_host, int client_family, const char *connect_host,
                     int *client);

/* fe_web_tcp_peer_uid() with the fake kernel and no /proc lookup left */
static long fake_uid_of(int fd)
{
	FE_WEB_PROC_BUDGET spent = { FE_WEB_PROC_LOOKUPS_PER_SEC, T };
	uid_t uid = 4242;

	if (fe_web_tcp_peer_uid(fd, &spent, T, &uid) != 1) {
		g_assert_cmpuint(uid, ==, 4242);
		return -1;
	}
	return (long) uid;
}

static void test_sock_diag_seam(void)
{
	int client, fd = live_pair("127.0.0.1", AF_INET, "127.0.0.1", &client);

	g_assert_cmpint(fd, >=, 0);
	/* the reply comes from the exchange: one request, AF_INET */
	fake_reset();
	g_assert_cmpint(fake_uid_of(fd), ==, 4321);
	g_assert_cmpint(fake_calls, ==, 1);
	g_assert_cmpint(fake_families[0], ==, AF_INET);
	/* not found as IPv4: asked again as IPv4-mapped IPv6 */
	fake_reset();
	fake_enoent_for_inet = 1;
	g_assert_cmpint(fake_uid_of(fd), ==, 4321);
	g_assert_cmpint(fake_calls, ==, 2);
	g_assert_cmpint(fake_families[1], ==, AF_INET6);
	/* no answer, an error, nothing, too big for the buffer, or one
	 * that is not the socket: not known (and with no /proc lookup
	 * left, nothing else) */
	fake_reset();
	fe_web_sock_diag_set_func(diag_fails);
	g_assert_cmpint(fake_uid_of(fd), ==, -1);
	fake_reset();
	fake_ret = -1;
	g_assert_cmpint(fake_uid_of(fd), ==, -1);
	fake_reset();
	fake_ret = 0;
	fake_state = 6;
	g_assert_cmpint(fake_uid_of(fd), ==, -1);
	fake_reset();
	fake_ret = FE_WEB_SOCK_DIAG_REPLY_MAX + 1;
	g_assert_cmpint(fake_uid_of(fd), ==, -1);
	fake_reset();
	fake_sport_delta = 1;
	g_assert_cmpint(fake_uid_of(fd), ==, -1);
	/* the overflow uid */
	fake_reset();
	fake_uid = 65534;
	g_assert_cmpint(fake_uid_of(fd), ==, -1);
	/* sock_diag is not used for peers not on loopback: none here, but
	 * a Unix socket is never asked about */
	{
		int sv[2];

		fake_reset();
		g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
		g_assert_cmpint(fake_uid_of(sv[0]), ==, -1);
		g_assert_cmpint(fake_calls, ==, 0);
		close(sv[0]);
		close(sv[1]);
	}
	fe_web_sock_diag_set_func(NULL);
	close(fd);
	close(client);
}

/* The kernel itself, unprivileged: the socket of our own client */
static void test_sock_diag_live(void)
{
	int client, fd = live_pair("127.0.0.1", AF_INET, "127.0.0.1", &client);
	FE_WEB_PROC_BUDGET spent = { FE_WEB_PROC_LOOKUPS_PER_SEC, T };
	uid_t uid = 4242;

	g_assert_cmpint(fd, >=, 0);
	fe_web_sock_diag_set_func(NULL);
	g_assert_cmpint(fe_web_tcp_peer_uid(fd, &spent, T, &uid), ==, 1);
	g_assert_cmpuint(uid, ==, getuid());
	/* the client closed: FIN_WAIT2 (uid 0), not ESTABLISHED */
	close(client);
	g_usleep(20000);
	uid = 4242;
	g_assert_cmpint(fe_web_tcp_peer_uid(fd, &spent, T, &uid), ==, 0);
	g_assert_cmpuint(uid, ==, 4242);
	close(fd);
}
#endif

/* The file is read whole only if its last line ends */
static void test_sockaddr_unterminated_file(void)
{
	char *dir = test_tmpdir();
	char *peer = line4(1, "127.0.0.1", 40000, "127.0.0.1", 9001, 0x01, 1001);
	char *text;

	text = g_strconcat(HEADER4, peer, NULL);
	text[strlen(text) - 1] = '\0';
	write_table(dir, "tcp", text);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, -1);
	g_free(text);
	text = g_strconcat(HEADER4, peer, NULL);
	write_table(dir, "tcp", text);
	g_assert_cmpint(sa_uid(dir, "127.0.0.1", 40000, "127.0.0.1", 9001), ==, 1001);
	g_free(text);
	g_free(peer);
	test_rmtree(dir);
	g_free(dir);
}

/* Live sockets: a listener on 'listen_host', a client of 'family' that
 * connects to 'connect_host'; the accepted fd, the client's in *client
 * (-1 and a skip when the system cannot) */
static int live_pair(const char *listen_host, int client_family, const char *connect_host,
                     int *client)
{
	struct sockaddr_storage ls = sa_of(listen_host, 0), cs;
	socklen_t len = sizeof(ls);
	int lfd, cfd, afd, port;

	*client = -1;
	lfd = socket(ls.ss_family, SOCK_STREAM, 0);
	if (lfd < 0 || bind(lfd, (struct sockaddr *) &ls,
	                    ls.ss_family == AF_INET ? sizeof(struct sockaddr_in) :
	                                              sizeof(struct sockaddr_in6)) < 0) {
		if (lfd >= 0)
			close(lfd);
		g_test_skip("no IPv6 here");
		return -1;
	}
	g_assert_cmpint(listen(lfd, 4), ==, 0);
	g_assert_cmpint(getsockname(lfd, (struct sockaddr *) &ls, &len), ==, 0);
	port = ntohs(ls.ss_family == AF_INET ? ((struct sockaddr_in *) &ls)->sin_port :
	                                       ((struct sockaddr_in6 *) &ls)->sin6_port);
	cs = sa_of(connect_host, port);
	g_assert_cmpint(cs.ss_family, ==, client_family);
	cfd = socket(client_family, SOCK_STREAM, 0);
	if (cfd < 0) {
		close(lfd);
		g_test_skip("no IPv6 here");
		return -1;
	}
	if (connect(cfd, (struct sockaddr *) &cs,
	            client_family == AF_INET ? sizeof(struct sockaddr_in) :
	                                       sizeof(struct sockaddr_in6)) < 0) {
		close(cfd);
		close(lfd);
		g_test_skip("cannot connect (IPv6 off?)");
		return -1;
	}
	afd = accept(lfd, NULL, NULL);
	g_assert_cmpint(afd, >=, 0);
	close(lfd);
	*client = cfd;
	return afd;
}

/* the uid fe_web_tcp_peer_uid() finds for that pair: 1 and getuid() on
 * Linux, -1 elsewhere */
static void assert_live_own_uid(const char *listen_host, int client_family,
                                const char *connect_host)
{
	int client, fd = live_pair(listen_host, client_family, connect_host, &client);
	uid_t uid = 4242;
	int r;

	if (fd < 0)
		return;
	r = fe_web_tcp_peer_uid(fd, NULL, 0, &uid);
#ifdef __linux__
	g_assert_cmpint(r, ==, 1);
	g_assert_cmpuint(uid, ==, getuid());
	{
		FE_WEB_PROC_BUDGET spent = { FE_WEB_PROC_LOOKUPS_PER_SEC, T };

		/* sock_diag alone: no /proc lookup left */
		uid = 4242;
		g_assert_cmpint(fe_web_tcp_peer_uid(fd, &spent, T, &uid), ==, 1);
		g_assert_cmpuint(uid, ==, getuid());
		g_assert_cmpint(spent.spent, ==, FE_WEB_PROC_LOOKUPS_PER_SEC);
		/* /proc alone: sock_diag fails */
		fe_web_sock_diag_set_func(diag_fails);
		uid = 4242;
		g_assert_cmpint(fe_web_tcp_peer_uid(fd, NULL, 0, &uid), ==, 1);
		g_assert_cmpuint(uid, ==, getuid());
		/* neither */
		uid = 4242;
		g_assert_cmpint(fe_web_tcp_peer_uid(fd, &spent, T, &uid), ==, 0);
		g_assert_cmpuint(uid, ==, 4242);
		fe_web_sock_diag_set_func(NULL);
	}
#else
	g_assert_cmpint(r, ==, -1);
#endif
	close(fd);
	close(client);
}

static void test_live_loopback(void)
{
	assert_live_own_uid("127.0.0.1", AF_INET, "127.0.0.1");
}

static void test_live_loopback_v6(void)
{
	assert_live_own_uid("::1", AF_INET6, "::1");
}

/* Review Focus: a dual-stack listener (::) and a client on 127.0.0.1. The
 * accepted peer is ::ffff:127.0.0.1, the client's socket is in tcp as
 * 127.0.0.1. */
static void test_proc_net_tcp_v4_mapped(void)
{
	assert_live_own_uid("::", AF_INET, "127.0.0.1");
}

/* An IPv6 client socket connected to ::ffff:127.0.0.1 of an IPv4
 * listener: the client's socket is in tcp6, mapped */
static void test_live_dual_stack_client(void)
{
	assert_live_own_uid("127.0.0.1", AF_INET6, "::ffff:127.0.0.1");
}

/* A peer that closed is no longer ESTABLISHED: not known */
static void test_live_closed_peer(void)
{
	int client, fd = live_pair("127.0.0.1", AF_INET, "127.0.0.1", &client);
	uid_t uid = 4242;

	if (fd < 0)
		return;
	close(client);
	g_usleep(20000);
#ifdef __linux__
	g_assert_cmpint(fe_web_tcp_peer_uid(fd, NULL, 0, &uid), ==, 0);
#else
	g_assert_cmpint(fe_web_tcp_peer_uid(fd, NULL, 0, &uid), ==, -1);
#endif
	g_assert_cmpuint(uid, ==, 4242);
	close(fd);
}

/* Not a TCP socket */
static void test_live_not_tcp(void)
{
	int sv[2];
	uid_t uid = 4242;

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
#ifdef __linux__
	g_assert_cmpint(fe_web_tcp_peer_uid(sv[0], NULL, 0, &uid), ==, 0);
	g_assert_cmpint(fe_web_tcp_peer_uid(-1, NULL, 0, &uid), ==, 0);
#else
	g_assert_cmpint(fe_web_tcp_peer_uid(sv[0], NULL, 0, &uid), ==, -1);
#endif
	g_assert_cmpuint(uid, ==, 4242);
	close(sv[0]);
	close(sv[1]);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fe-web/pacing/five-failures-then-interval",
	                test_five_failures_then_interval);
	g_test_add_func("/fe-web/pacing/window-expires", test_window_expires);
	g_test_add_func("/fe-web/pacing/window-counts-from-last-failure",
	                test_window_counts_from_last_failure);
	g_test_add_func("/fe-web/pacing/paced-failures-extend-window",
	                test_paced_failures_extend_window);
	g_test_add_func("/fe-web/pacing/success-resets", test_success_resets);
	g_test_add_func("/fe-web/pacing/reset-forgets-all", test_reset_forgets_all);
	g_test_add_func("/fe-web/pacing/full-table-uses-overflow", test_full_table_uses_overflow);
	g_test_add_func("/fe-web/pacing/success-keeps-overflow", test_success_keeps_overflow);
	g_test_add_func("/fe-web/pacing/full-table-paced-peer-stays",
	                test_full_table_paced_peer_stays);
	g_test_add_func("/fe-web/pacing/expired-peers-make-room", test_expired_peers_make_room);
	g_test_add_func("/fe-web/pacing/memory-keeps-suspects", test_memory_keeps_suspects);
	g_test_add_func("/fe-web/pacing/ceiling-engages-for-suspects",
	                test_ceiling_engages_for_suspects);
	g_test_add_func("/fe-web/pacing/ceiling-spares-clean-and-trusted",
	                test_ceiling_spares_clean_and_trusted);
	g_test_add_func("/fe-web/pacing/trusted-failures-count-towards-ceiling",
	                test_trusted_failures_count_towards_ceiling);
	g_test_add_func("/fe-web/pacing/trusted-refresh-resets-bucket",
	                test_trusted_refresh_resets_bucket);
	g_test_add_func("/fe-web/pacing/decide-then-record", test_decide_then_record);
	g_test_add_func("/fe-web/pacing/trusted-set-oldest-replaced",
	                test_trusted_set_oldest_replaced);
	g_test_add_func("/fe-web/pacing/backward-clock", test_backward_clock);
	g_test_add_func("/fe-web/pacing/distributed-probe-bounded",
	                test_distributed_probe_bounded);
	g_test_add_func("/fe-web/pacing/single-address-probe", test_single_address_probe);
	g_test_add_func("/fe-web/peer-key/ip4", test_peer_key_ip4);
	g_test_add_func("/fe-web/peer-key/ip6-prefix", test_peer_key_ip6_prefix);
	g_test_add_func("/fe-web/peer-key/ip4-mapped", test_peer_key_ip4_mapped);
	g_test_add_func("/fe-web/peer-key/uid", test_peer_key_uid);
	g_test_add_func("/fe-web/peer-key/loopback-shared", test_peer_key_loopback_shared);
	g_test_add_func("/fe-web/peer-key/loopback", test_peer_key_loopback);
	g_test_add_func("/fe-web/peer/proc-net-tcp-v4", test_proc_net_tcp_v4);
	g_test_add_func("/fe-web/peer/proc-net-tcp-v6", test_proc_net_tcp_v6);
	g_test_add_func("/fe-web/peer/proc-net-tcp-captured", test_proc_net_tcp_captured);
	g_test_add_func("/fe-web/peer/proc-net-tcp-exact-tuple", test_proc_net_tcp_exact_tuple);
	g_test_add_func("/fe-web/peer/proc-net-tcp-established-only",
	                test_proc_net_tcp_established_only);
	g_test_add_func("/fe-web/peer/proc-net-tcp-duplicate", test_proc_net_tcp_duplicate);
	g_test_add_func("/fe-web/peer/proc-net-tcp-missing", test_proc_net_tcp_missing);
	g_test_add_func("/fe-web/peer/proc-net-tcp-malformed", test_proc_net_tcp_malformed);
	g_test_add_func("/fe-web/peer/proc-net-tcp-bad-arguments", test_proc_net_tcp_bad_arguments);
	g_test_add_func("/fe-web/peer/sockaddr-lookup", test_sockaddr_lookup);
	g_test_add_func("/fe-web/peer/sockaddr-file-reading", test_sockaddr_file_reading);
	g_test_add_func("/fe-web/peer/proc-net-tcp-v4-mapped", test_proc_net_tcp_v4_mapped);
	g_test_add_func("/fe-web/peer/live-loopback", test_live_loopback);
	g_test_add_func("/fe-web/peer/live-loopback-v6", test_live_loopback_v6);
	g_test_add_func("/fe-web/peer/live-dual-stack-client", test_live_dual_stack_client);
	g_test_add_func("/fe-web/peer/live-closed-peer", test_live_closed_peer);
	g_test_add_func("/fe-web/peer/live-not-tcp", test_live_not_tcp);
	g_test_add_func("/fe-web/peer/sockaddr-unterminated-file", test_sockaddr_unterminated_file);
	g_test_add_func("/fe-web/peer/proc-budget", test_proc_budget);
#ifdef __linux__
	g_test_add_func("/fe-web/peer/sock-diag-request", test_sock_diag_request);
	g_test_add_func("/fe-web/peer/sock-diag-reply", test_sock_diag_reply);
	g_test_add_func("/fe-web/peer/sock-diag-reply-malformed", test_sock_diag_reply_malformed);
	g_test_add_func("/fe-web/peer/sock-diag-seam", test_sock_diag_seam);
	g_test_add_func("/fe-web/peer/sock-diag-live", test_sock_diag_live);
#endif

	return g_test_run();
}
