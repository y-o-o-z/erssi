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
	int fails;        /* wrong passwords in the window, at most the limit */
	time_t last_fail; /* the window ends FE_WEB_FAIL_WINDOW s after it */
	time_t last_check;
} PACING_BUCKET;

typedef struct {
	char *key;
	PACING_BUCKET bucket;
} TRUSTED_PEER;

struct _FE_WEB_PACING {
	GHashTable *peers; /* suspects: key (char *) -> PACING_BUCKET * */
	PACING_BUCKET overflow; /* suspects the table has no room for */
	PACING_BUCKET global;   /* the ceiling: wrong passwords of all peers */
	GQueue trusted;         /* TRUSTED_PEER *, the oldest first */
};

/* More than 'secs' seconds from 'then' to 'now'. A clock that went back
 * (now < then) counts as long ago: nobody is locked out by it. */
static gboolean older_than(time_t then, time_t now, int secs)
{
	return now < then || now - then > secs;
}

/* At least 'secs' seconds from 'then' to 'now', the same way */
static gboolean at_least(time_t then, time_t now, int secs)
{
	return now < then || now - then >= secs;
}

/* Does the bucket let a check through now: fewer than max_fails wrong
 * passwords in its window, or 'interval' seconds since its last check.
 * Nothing is recorded but the end of a window. */
static gboolean bucket_open(PACING_BUCKET *bucket, time_t now, int max_fails, int interval)
{
	if (older_than(bucket->last_fail, now, FE_WEB_FAIL_WINDOW))
		bucket->fails = 0;
	return bucket->fails < max_fails || at_least(bucket->last_check, now, interval);
}

/* A wrong password: TRUE when it is the one that reaches max_fails. Also
 * while paced the window moves, so guessing on keeps the pacing on. */
static gboolean bucket_fail(PACING_BUCKET *bucket, time_t now, int max_fails)
{
	if (older_than(bucket->last_fail, now, FE_WEB_FAIL_WINDOW))
		bucket->fails = 0;
	bucket->last_fail = now;
	if (bucket->fails >= max_fails)
		return FALSE;
	return ++bucket->fails == max_fails;
}

static void trusted_free(gpointer data)
{
	TRUSTED_PEER *trusted = data;

	g_free(trusted->key);
	g_free(trusted);
}

static GList *trusted_link(FE_WEB_PACING *pacing, const char *peer)
{
	GList *link;

	for (link = pacing->trusted.head; link != NULL; link = link->next) {
		if (strcmp(((TRUSTED_PEER *) link->data)->key, peer) == 0)
			return link;
	}
	return NULL;
}

static PACING_BUCKET *trusted_bucket(FE_WEB_PACING *pacing, const char *peer)
{
	GList *link = trusted_link(pacing, peer);

	return link != NULL ? &((TRUSTED_PEER *) link->data)->bucket : NULL;
}

FE_WEB_PACING *fe_web_pacing_new(void)
{
	FE_WEB_PACING *pacing = g_new0(FE_WEB_PACING, 1);

	pacing->peers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_queue_init(&pacing->trusted);
	return pacing;
}

void fe_web_pacing_reset(FE_WEB_PACING *pacing)
{
	TRUSTED_PEER *trusted;

	g_hash_table_remove_all(pacing->peers);
	memset(&pacing->overflow, 0, sizeof(pacing->overflow));
	memset(&pacing->global, 0, sizeof(pacing->global));
	while ((trusted = g_queue_pop_head(&pacing->trusted)) != NULL)
		trusted_free(trusted);
}

void fe_web_pacing_free(FE_WEB_PACING *pacing)
{
	if (pacing == NULL)
		return;
	fe_web_pacing_reset(pacing);
	g_hash_table_destroy(pacing->peers);
	g_free(pacing);
}

static gboolean bucket_forgotten(gpointer key, gpointer value, gpointer data)
{
	const PACING_BUCKET *bucket = value;

	(void) key;
	return older_than(bucket->last_fail, *(time_t *) data, FE_WEB_PEER_MEMORY);
}

/* The bucket of a peer that is not trusted: its entry in the table, the
 * overflow bucket when the table has no room for it, or NULL for a clean
 * peer (not in the table, room for it). With 'add' a clean peer is added.
 * Entries FE_WEB_PEER_MEMORY s after their last failure are forgotten,
 * when looked up or to make room; never earlier, so a peer still
 * remembered cannot be pushed out with new ones. */
static PACING_BUCKET *suspect_bucket(FE_WEB_PACING *pacing, const char *peer, time_t now,
                                     gboolean add)
{
	PACING_BUCKET *bucket;

	bucket = g_hash_table_lookup(pacing->peers, peer);
	if (bucket != NULL && bucket_forgotten(NULL, bucket, &now)) {
		g_hash_table_remove(pacing->peers, peer);
		bucket = NULL;
	}
	if (bucket != NULL)
		return bucket;
	if (g_hash_table_size(pacing->peers) >= FE_WEB_PACING_MAX_PEERS)
		g_hash_table_foreach_remove(pacing->peers, bucket_forgotten, &now);
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
	PACING_BUCKET *bucket;

	bucket = trusted_bucket(pacing, peer);
	if (bucket != NULL) {
		/* trusted: its own bucket only */
		if (!bucket_open(bucket, now, FE_WEB_MAX_FAILS, FE_WEB_CHECK_INTERVAL))
			return FALSE;
		bucket->last_check = now;
		return TRUE;
	}
	bucket = suspect_bucket(pacing, peer, now, FALSE);
	if (bucket == NULL)
		return TRUE; /* clean: no wrong password remembered */

	/* a suspect: first decide with both buckets, then record in both */
	if (!bucket_open(bucket, now, FE_WEB_MAX_FAILS, FE_WEB_CHECK_INTERVAL) ||
	    !bucket_open(&pacing->global, now, FE_WEB_GLOBAL_MAX_FAILS,
	                 FE_WEB_GLOBAL_CHECK_INTERVAL))
		return FALSE;
	bucket->last_check = now;
	pacing->global.last_check = now;
	return TRUE;
}

gboolean fe_web_pacing_failed(FE_WEB_PACING *pacing, const char *peer, time_t now)
{
	PACING_BUCKET *bucket;

	/* every wrong password counts towards the ceiling */
	bucket_fail(&pacing->global, now, FE_WEB_GLOBAL_MAX_FAILS);

	bucket = trusted_bucket(pacing, peer);
	if (bucket == NULL)
		bucket = suspect_bucket(pacing, peer, now, TRUE);
	return bucket_fail(bucket, now, FE_WEB_MAX_FAILS);
}

void fe_web_pacing_succeeded(FE_WEB_PACING *pacing, const char *peer)
{
	TRUSTED_PEER *trusted;
	GList *link;

	/* the overflow bucket is left as it is: the peers that share it
	 * include the one guessing */
	g_hash_table_remove(pacing->peers, peer);

	link = trusted_link(pacing, peer);
	if (link != NULL) {
		/* the newest again, with a fresh bucket */
		trusted = link->data;
		g_queue_delete_link(&pacing->trusted, link);
	} else {
		if (g_queue_get_length(&pacing->trusted) >= FE_WEB_PACING_MAX_TRUSTED)
			trusted_free(g_queue_pop_head(&pacing->trusted));
		trusted = g_new0(TRUSTED_PEER, 1);
		trusted->key = g_strdup(peer);
	}
	memset(&trusted->bucket, 0, sizeof(trusted->bucket));
	g_queue_push_tail(&pacing->trusted, trusted);
}

gboolean fe_web_pacing_in_overflow(FE_WEB_PACING *pacing, const char *peer)
{
	return trusted_link(pacing, peer) == NULL &&
	       g_hash_table_lookup(pacing->peers, peer) == NULL &&
	       g_hash_table_size(pacing->peers) >= FE_WEB_PACING_MAX_PEERS;
}

guint fe_web_pacing_table_size(FE_WEB_PACING *pacing)
{
	return g_hash_table_size(pacing->peers);
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
