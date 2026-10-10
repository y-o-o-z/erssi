/*
 fe-web-pacing.c : login pacing of fe-web, per peer

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "module.h"
#include "fe-web-pacing.h"

#include <irssi/src/core/network.h>

#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

typedef struct {
	int fails;        /* wrong passwords in the window */
	time_t last_fail; /* the window ends FE_WEB_FAIL_WINDOW s after it */
	time_t last_check;
} PACING_BUCKET;

struct _FE_WEB_PACING {
	GHashTable *peers; /* key (char *) -> PACING_BUCKET * */
	PACING_BUCKET overflow;
};

FE_WEB_PACING *fe_web_pacing_new(void)
{
	FE_WEB_PACING *pacing = g_new0(FE_WEB_PACING, 1);

	pacing->peers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	return pacing;
}

void fe_web_pacing_free(FE_WEB_PACING *pacing)
{
	if (pacing == NULL)
		return;
	g_hash_table_destroy(pacing->peers);
	g_free(pacing);
}

void fe_web_pacing_reset(FE_WEB_PACING *pacing)
{
	g_hash_table_remove_all(pacing->peers);
	memset(&pacing->overflow, 0, sizeof(pacing->overflow));
}

static gboolean window_over(const PACING_BUCKET *bucket, time_t now)
{
	return now - bucket->last_fail > FE_WEB_FAIL_WINDOW;
}

static gboolean bucket_expired(gpointer key, gpointer value, gpointer data)
{
	(void) key;
	return window_over(value, *(time_t *) data);
}

/* The bucket of peer; NULL for a peer that is not in the table while
 * there is room for it. With 'add' a peer that is not in the table is
 * added when there is room. A full table first drops the peers whose
 * window is over, never one still counting; a peer that still finds no
 * room gets the overflow bucket. */
static PACING_BUCKET *bucket_of(FE_WEB_PACING *pacing, const char *peer, time_t now,
                                gboolean add)
{
	PACING_BUCKET *bucket;

	bucket = g_hash_table_lookup(pacing->peers, peer);
	if (bucket != NULL)
		return bucket;
	if (g_hash_table_size(pacing->peers) >= FE_WEB_PACING_MAX_PEERS)
		g_hash_table_foreach_remove(pacing->peers, bucket_expired, &now);
	if (g_hash_table_size(pacing->peers) >= FE_WEB_PACING_MAX_PEERS)
		return &pacing->overflow;
	if (!add)
		return NULL;
	bucket = g_new0(PACING_BUCKET, 1);
	/* the check of this login, which fe_web_pacing_may_check() let
	 * through without a bucket to record it in */
	bucket->last_check = now;
	g_hash_table_insert(pacing->peers, g_strdup(peer), bucket);
	return bucket;
}

gboolean fe_web_pacing_may_check(FE_WEB_PACING *pacing, const char *peer, time_t now)
{
	PACING_BUCKET *bucket = bucket_of(pacing, peer, now, FALSE);

	if (bucket == NULL)
		return TRUE; /* no wrong password from there */
	if (window_over(bucket, now))
		bucket->fails = 0;
	if (bucket->fails >= FE_WEB_MAX_FAILS && now - bucket->last_check < FE_WEB_CHECK_INTERVAL)
		return FALSE;
	bucket->last_check = now;
	return TRUE;
}

gboolean fe_web_pacing_failed(FE_WEB_PACING *pacing, const char *peer, time_t now)
{
	PACING_BUCKET *bucket = bucket_of(pacing, peer, now, TRUE);

	if (window_over(bucket, now))
		bucket->fails = 0;
	bucket->last_fail = now;
	if (bucket->fails < FE_WEB_MAX_FAILS)
		bucket->fails++;
	else
		return FALSE; /* paced already */
	return bucket->fails == FE_WEB_MAX_FAILS;
}

void fe_web_pacing_succeeded(FE_WEB_PACING *pacing, const char *peer)
{
	/* the overflow bucket is left as it is: the peers that share it
	 * include the one guessing */
	g_hash_table_remove(pacing->peers, peer);
}

char *fe_web_peer_key_uid(unsigned long uid)
{
	return g_strdup_printf("uid:%lu", uid);
}

/* An IPv4-mapped IPv6 address (::ffff:a.b.c.d): its IPv4 address in 'v4' */
static gboolean ip_mapped_v4(const IPADDR *ip, struct in_addr *v4)
{
	const struct in6_addr *a = &ip->ip;

	if (ip->family != AF_INET6 || !IN6_IS_ADDR_V4MAPPED(a))
		return FALSE;
	memcpy(v4, a->s6_addr + 12, 4);
	return TRUE;
}

static gboolean ip_v4(const IPADDR *ip, struct in_addr *v4)
{
	if (ip->family == AF_INET) {
		memcpy(v4, &ip->ip, sizeof(*v4));
		return TRUE;
	}
	return ip_mapped_v4(ip, v4);
}

char *fe_web_peer_key_ip(const IPADDR *ip)
{
	char host[INET6_ADDRSTRLEN];
	struct in_addr v4;
	struct in6_addr v6;

	if (ip_v4(ip, &v4)) {
		if (inet_ntop(AF_INET, &v4, host, sizeof(host)) == NULL)
			return g_strdup("ip:?");
		return g_strconcat("ip:", host, NULL);
	}
	/* one IPv6 host usually has a /64 to itself: the whole /64 counts
	 * as one peer, or one host would be 2^64 peers */
	v6 = ip->ip;
	memset(v6.s6_addr + 8, 0, 8);
	if (inet_ntop(AF_INET6, &v6, host, sizeof(host)) == NULL)
		return g_strdup("ip:?");
	return g_strconcat("ip:", host, "/64", NULL);
}

gboolean fe_web_ip_is_loopback(const IPADDR *ip)
{
	struct in_addr v4;

	if (ip_v4(ip, &v4))
		return (ntohl(v4.s_addr) >> 24) == 127;
	return IN6_IS_ADDR_LOOPBACK(&ip->ip);
}
