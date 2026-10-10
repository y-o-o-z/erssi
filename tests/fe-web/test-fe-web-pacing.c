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
	g_test_add_func("/fe-web/peer-key/loopback", test_peer_key_loopback);

	return g_test_run();
}
