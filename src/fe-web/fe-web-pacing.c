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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#ifdef __linux__
#include <netinet/tcp.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#endif

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

	/* a local user may pick any 127/8 source address: one peer */
	if (fe_web_ip_is_loopback(ip))
		return g_strdup("ip:loopback");
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

/* The uid of a TCP peer on loopback, from /proc/net/tcp and tcp6.
 *
 * A line of the table, as the kernel prints it, cut in two here (fields
 * separated by spaces, the ones after the inode not used):
 *
 *   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt
 *    1: 0100007F:9C40 0100007F:2329 01 00000000:00000000 00:00000000 00000000
 *
 *     uid  timeout inode
 *    1001        0 4242 1 0000000000000000 20 0 0 10 -1
 *
 * An address is %08X of each 32-bit word of the address as it is in
 * memory (one word in tcp, four in tcp6), so 127.0.0.1 is 0100007F on a
 * little-endian machine and 7F000001 on a big-endian one. Each word is
 * parsed as hex and compared with the word read from the address bytes,
 * which is right on either. The port is %04X of the port number. */

typedef struct {
	int words;            /* 1: an IPv4 table, 4: an IPv6 one */
	guint32 peer[4], local[4];
	int peer_port, local_port;
	int matches;          /* lines found, counting stops at 2 */
	uid_t uid;            /* of the last one */
} TCP_LOOKUP;

static gboolean tcp_lookup_init(TCP_LOOKUP *t, int family, const void *peer_addr, int peer_port,
                                const void *local_addr, int local_port)
{
	int i;

	memset(t, 0, sizeof(*t));
	if (peer_addr == NULL || local_addr == NULL || peer_port < 0 || peer_port > 0xffff ||
	    local_port < 0 || local_port > 0xffff)
		return FALSE;
	if (family == AF_INET)
		t->words = 1;
	else if (family == AF_INET6)
		t->words = 4;
	else
		return FALSE;
	for (i = 0; i < t->words; i++) {
		memcpy(&t->peer[i], (const guchar *) peer_addr + 4 * i, 4);
		memcpy(&t->local[i], (const guchar *) local_addr + 4 * i, 4);
	}
	t->peer_port = peer_port;
	t->local_port = local_port;
	return TRUE;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* exactly 'digits' hex digits at *p (at most 8): their value, and *p
 * after them. No sign, no 0x, no spaces (all of which strtoul() takes). */
static gboolean parse_hex(const char **p, const char *end, int digits, guint32 *value)
{
	guint32 v = 0;
	int i, d;

	if (end - *p < digits)
		return FALSE;
	for (i = 0; i < digits; i++) {
		d = hex_digit((*p)[i]);
		if (d < 0)
			return FALSE;
		v = (v << 4) | (guint32) d;
	}
	*p += digits;
	*value = v;
	return TRUE;
}

/* one or more spaces, then a field */
static gboolean skip_spaces(const char **p, const char *end)
{
	if (*p >= end || **p != ' ')
		return FALSE;
	while (*p < end && **p == ' ')
		(*p)++;
	return *p < end;
}

/* a field of anything but spaces */
static gboolean skip_field(const char **p, const char *end)
{
	const char *start = *p;

	while (*p < end && **p != ' ')
		(*p)++;
	return *p > start;
}

/* digits, at most G_MAXUINT32 (what follows is checked by the caller) */
static gboolean parse_decimal(const char **p, const char *end, guint64 *value)
{
	guint64 v = 0;
	const char *start = *p;

	while (*p < end && **p >= '0' && **p <= '9') {
		v = v * 10 + (guint64) (**p - '0');
		if (v > G_MAXUINT32)
			return FALSE;
		(*p)++;
	}
	if (*p == start)
		return FALSE;
	*value = v;
	return TRUE;
}

/* a field of digits of any value (the inode is an unsigned long) */
static gboolean skip_digits(const char **p, const char *end)
{
	const char *start = *p;

	while (*p < end && **p >= '0' && **p <= '9')
		(*p)++;
	return *p > start && (*p == end || **p == ' ');
}

/* "<words * 8 hex digits>:<4 hex digits>" (every field is followed by
 * spaces, which skip_spaces() checks) */
static gboolean parse_endpoint(const char **p, const char *end, int words, guint32 *addr,
                               int *port)
{
	guint32 v;
	int i;

	for (i = 0; i < words; i++) {
		if (!parse_hex(p, end, 8, &addr[i]))
			return FALSE;
	}
	if (*p >= end || **p != ':')
		return FALSE;
	(*p)++;
	if (!parse_hex(p, end, 4, &v))
		return FALSE;
	*port = (int) v;
	return TRUE;
}

/* One line (without its newline): is it the socket looked for */
static void tcp_lookup_line(TCP_LOOKUP *t, const char *line, gsize len)
{
	const char *p = line, *end = line + len;
	guint32 laddr[4], raddr[4], state;
	int lport, rport;
	guint64 uid;

	if (len > FE_WEB_PROC_NET_LINE_MAX)
		return;
	/* "   1:" */
	while (p < end && *p == ' ')
		p++;
	if (p == end || *p < '0' || *p > '9')
		return;
	while (p < end && *p >= '0' && *p <= '9')
		p++;
	if (p == end || *p != ':')
		return;
	p++;
	if (!skip_spaces(&p, end) || !parse_endpoint(&p, end, t->words, laddr, &lport))
		return;
	if (!skip_spaces(&p, end) || !parse_endpoint(&p, end, t->words, raddr, &rport))
		return;
	/* the state: 01 is ESTABLISHED */
	if (!skip_spaces(&p, end) || !parse_hex(&p, end, 2, &state))
		return;
	if (state != 0x01)
		return;
	/* tx_queue:rx_queue, tr:tm->when, retrnsmt */
	if (!skip_spaces(&p, end) || !skip_field(&p, end) || !skip_spaces(&p, end) ||
	    !skip_field(&p, end) || !skip_spaces(&p, end) || !skip_field(&p, end))
		return;
	/* the uid, then timeout and inode: a line cut short is not read */
	if (!skip_spaces(&p, end) || !parse_decimal(&p, end, &uid))
		return;
	if (!skip_spaces(&p, end) || !skip_digits(&p, end) || !skip_spaces(&p, end) ||
	    !skip_digits(&p, end))
		return;
	/* uid_t can hold it, and it is not (uid_t) -1 */
	if ((guint64) (uid_t) uid != uid || (uid_t) uid == (uid_t) -1)
		return;

	/* the peer's own socket: its local side is the peer, its remote
	 * side is us */
	if (lport != t->peer_port || rport != t->local_port ||
	    memcmp(laddr, t->peer, t->words * sizeof(guint32)) != 0 ||
	    memcmp(raddr, t->local, t->words * sizeof(guint32)) != 0)
		return;
	if (t->matches < 2)
		t->matches++;
	t->uid = (uid_t) uid;
}

/* Every line of 'text' that ends with a newline */
static void tcp_lookup_text(TCP_LOOKUP *t, const char *text, gsize len)
{
	const char *p = text, *end = text + len, *nl;

	while (p < end && (nl = memchr(p, '\n', end - p)) != NULL) {
		tcp_lookup_line(t, p, nl - p);
		p = nl + 1;
	}
}

gboolean fe_web_proc_net_tcp_uid(const char *table, int family, const void *peer_addr,
                                 int peer_port, const void *local_addr, int local_port,
                                 uid_t *uid)
{
	TCP_LOOKUP t;

	if (table == NULL || uid == NULL ||
	    !tcp_lookup_init(&t, family, peer_addr, peer_port, local_addr, local_port))
		return FALSE;
	tcp_lookup_text(&t, table, strlen(table));
	if (t.matches != 1)
		return FALSE;
	*uid = t.uid;
	return TRUE;
}

/* Read the table 'dir'/'name' into 't', a piece at a time, at most
 * FE_WEB_PROC_NET_MAX bytes. FALSE when it could not be read whole; a
 * missing file has no lines. A line longer than the piece is skipped to
 * its end, never read from its middle. */
static gboolean tcp_lookup_file(TCP_LOOKUP *t, const char *dir, const char *name)
{
	char buf[FE_WEB_PROC_NET_LINE_MAX * 8];
	char *path;
	gsize fill = 0, total = 0;
	gboolean skipping = FALSE, ok = FALSE, missing;
	int fd;

	path = g_build_filename(dir, name, NULL);
	fd = open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
	missing = fd < 0 && errno == ENOENT;
	g_free(path);
	if (fd < 0)
		return missing;

	for (;;) {
		ssize_t n = read(fd, buf + fill, sizeof(buf) - fill);
		char *p, *end, *nl;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			break;
		if (n == 0) {
			/* the last line has no newline: maybe cut short */
			ok = TRUE;
			break;
		}
		total += n;
		if (total > FE_WEB_PROC_NET_MAX)
			break;
		fill += n;
		p = buf;
		end = buf + fill;
		while ((nl = memchr(p, '\n', end - p)) != NULL) {
			if (!skipping)
				tcp_lookup_line(t, p, nl - p);
			skipping = FALSE;
			p = nl + 1;
		}
		fill = end - p;
		if (fill == sizeof(buf)) {
			/* no newline in a whole piece: too long, skip to
			 * its end */
			skipping = TRUE;
			fill = 0;
		} else {
			memmove(buf, p, fill);
		}
	}
	close(fd);
	return ok;
}

/* A TCP connection on loopback: the peer's side and ours */
typedef struct {
	int family;                    /* AF_INET (IPv4-mapped too) or AF_INET6 */
	struct in_addr peer4, local4;   /* AF_INET */
	struct in6_addr peer6, local6;  /* AF_INET6; for AF_INET: IPv4-mapped */
	int peer_port, local_port;
} TCP_PEER;

static gboolean sockaddr_v4(const struct sockaddr *sa, struct in_addr *addr, int *port)
{
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *) sa;

		*addr = sin->sin_addr;
		*port = ntohs(sin->sin_port);
		return TRUE;
	}
	if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) sa;

		if (!IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
			return FALSE;
		memcpy(addr, sin6->sin6_addr.s6_addr + 12, 4);
		*port = ntohs(sin6->sin6_port);
		return TRUE;
	}
	return FALSE;
}

/* 'addr' as IPv4-mapped IPv6 */
static struct in6_addr mapped_v6(const struct in_addr *addr)
{
	struct in6_addr a;

	memset(&a, 0, sizeof(a));
	a.s6_addr[10] = 0xff;
	a.s6_addr[11] = 0xff;
	memcpy(a.s6_addr + 12, addr, 4);
	return a;
}

/* The connection from 'peer' to 'local': FALSE unless both are of one
 * family (IPv4-mapped counts as IPv4) and the peer is on loopback */
static gboolean tcp_peer_of(TCP_PEER *c, const struct sockaddr *peer,
                            const struct sockaddr *local)
{
	memset(c, 0, sizeof(*c));
	if (sockaddr_v4(peer, &c->peer4, &c->peer_port)) {
		if (!sockaddr_v4(local, &c->local4, &c->local_port))
			return FALSE;
		if ((ntohl(c->peer4.s_addr) >> 24) != 127)
			return FALSE;
		c->family = AF_INET;
		c->peer6 = mapped_v6(&c->peer4);
		c->local6 = mapped_v6(&c->local4);
		return TRUE;
	}
	if (peer->sa_family == AF_INET6) {
		const struct sockaddr_in6 *ps = (const struct sockaddr_in6 *) peer;
		const struct sockaddr_in6 *ls = (const struct sockaddr_in6 *) local;

		if (local->sa_family != AF_INET6 || IN6_IS_ADDR_V4MAPPED(&ls->sin6_addr))
			return FALSE;
		if (!IN6_IS_ADDR_LOOPBACK(&ps->sin6_addr))
			return FALSE;
		c->family = AF_INET6;
		c->peer6 = ps->sin6_addr;
		c->local6 = ls->sin6_addr;
		c->peer_port = ntohs(ps->sin6_port);
		c->local_port = ntohs(ls->sin6_port);
		return TRUE;
	}
	return FALSE;
}

/* The uid from tcp and tcp6 of 'proc_net' */
static int tcp_peer_proc_uid(const TCP_PEER *c, const char *proc_net, uid_t unmapped, uid_t *uid)
{
	TCP_LOOKUP t4, t6;
	int matches;
	uid_t found;

	if (c->family == AF_INET) {
		/* the peer's socket is in tcp, or in tcp6 as IPv4-mapped
		 * when it is a dual-stack one */
		if (!tcp_lookup_init(&t4, AF_INET, &c->peer4, c->peer_port, &c->local4,
		                     c->local_port) ||
		    !tcp_lookup_init(&t6, AF_INET6, &c->peer6, c->peer_port, &c->local6,
		                     c->local_port) ||
		    !tcp_lookup_file(&t4, proc_net, "tcp") ||
		    !tcp_lookup_file(&t6, proc_net, "tcp6"))
			return 0;
		matches = t4.matches + t6.matches;
		found = t4.matches > 0 ? t4.uid : t6.uid;
	} else {
		if (!tcp_lookup_init(&t6, AF_INET6, &c->peer6, c->peer_port, &c->local6,
		                     c->local_port) ||
		    !tcp_lookup_file(&t6, proc_net, "tcp6"))
			return 0;
		matches = t6.matches;
		found = t6.uid;
	}
	/* exactly one socket; and not the uid the kernel shows when it
	 * cannot show the real one in our user namespace */
	if (matches != 1 || found == unmapped)
		return 0;
	*uid = found;
	return 1;
}

int fe_web_tcp_sockaddr_uid(const struct sockaddr *peer, const struct sockaddr *local,
                            const char *proc_net, uid_t unmapped, uid_t *uid)
{
	TCP_PEER c;

	if (peer == NULL || local == NULL || proc_net == NULL || uid == NULL)
		return 0;
	if (!tcp_peer_of(&c, peer, local))
		return 0;
	return tcp_peer_proc_uid(&c, proc_net, unmapped, uid);
}

gboolean fe_web_proc_budget_take(FE_WEB_PROC_BUDGET *budget, time_t now)
{
	/* a new second, or a clock that went back: the whole budget */
	if (now != budget->last) {
		budget->spent = 0;
		budget->last = now;
	}
	if (budget->spent >= FE_WEB_PROC_LOOKUPS_PER_SEC)
		return FALSE;
	budget->spent++;
	return TRUE;
}

#ifdef __linux__
/* the uid /proc shows for one outside our user namespace (65534 unless
 * changed) */
static uid_t overflow_uid(void)
{
	char buf[32];
	ssize_t n;
	guint64 v = 0;
	int fd, i;

	fd = open("/proc/sys/kernel/overflowuid", O_RDONLY | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return 65534;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 65534;
	for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9' && v <= G_MAXUINT32; i++)
		v = v * 10 + (guint64) (buf[i] - '0');
	if (i == 0 || v > G_MAXUINT32)
		return 65534;
	return (uid_t) v;
}

/* NETLINK_SOCK_DIAG: the kernel looks the one socket up by its tuple
 * (inet_diag_find_one_icsk()), however many sockets there are. For a
 * tuple without a connection it may answer with a listener on that port,
 * or with a closed connection (FIN_WAIT2, TIME_WAIT, uid 0): only an
 * ESTABLISHED socket that has exactly the tuple asked for is used. */

typedef struct {
	struct nlmsghdr nlh;
	struct inet_diag_req_v2 req;
} SOCK_DIAG_REQUEST;

gsize fe_web_sock_diag_request(void *buf, gsize size, int family, const void *src, int sport,
                               const void *dst, int dport, guint32 seq)
{
	SOCK_DIAG_REQUEST m;
	gsize addr_len;

	if (buf == NULL || src == NULL || dst == NULL || size < sizeof(m) || sport < 0 ||
	    sport > 0xffff || dport < 0 || dport > 0xffff)
		return 0;
	if (family == AF_INET)
		addr_len = 4;
	else if (family == AF_INET6)
		addr_len = 16;
	else
		return 0;
	memset(&m, 0, sizeof(m));
	m.nlh.nlmsg_len = sizeof(m);
	m.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
	/* one socket: no NLM_F_DUMP */
	m.nlh.nlmsg_flags = NLM_F_REQUEST;
	m.nlh.nlmsg_seq = seq;
	m.req.sdiag_family = family;
	m.req.sdiag_protocol = IPPROTO_TCP;
	m.req.idiag_states = 1 << TCP_ESTABLISHED;
	m.req.id.idiag_sport = htons(sport);
	m.req.id.idiag_dport = htons(dport);
	memcpy(m.req.id.idiag_src, src, addr_len);
	memcpy(m.req.id.idiag_dst, dst, addr_len);
	m.req.id.idiag_if = 0;
	m.req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
	m.req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;
	memcpy(buf, &m, sizeof(m));
	return sizeof(m);
}

/* An address as the kernel echoes it (inet_diag_sockid, of a socket of
 * 'family'), or as asked for: IPv4 or IPv4-mapped as 4 bytes, else 16.
 * The length, 0 for a family that is neither. */
static gsize diag_addr(int family, const void *addr, guchar out[16])
{
	const guchar *a = addr;

	if (family == AF_INET) {
		memcpy(out, a, 4);
		return 4;
	}
	if (family != AF_INET6)
		return 0;
	if (IN6_IS_ADDR_V4MAPPED((const struct in6_addr *) addr)) {
		memcpy(out, a + 12, 4);
		return 4;
	}
	memcpy(out, a, 16);
	return 16;
}

int fe_web_sock_diag_reply(const void *buf, gsize len, int family, const void *src, int sport,
                           const void *dst, int dport, guint32 seq, uid_t unmapped,
                           uid_t *uid)
{
	const guchar *p = buf;
	guchar want_src[16], want_dst[16], got_src[16], got_dst[16];
	gsize want_len, off = 0;
	struct in6_addr a6;
	int found = 0;
	guint32 found_uid = 0;

	if (buf == NULL || src == NULL || dst == NULL || uid == NULL ||
	    (family != AF_INET && family != AF_INET6))
		return 0;
	/* an in_addr is not 16 bytes long: copy what the family has */
	memset(&a6, 0, sizeof(a6));
	memcpy(&a6, src, family == AF_INET ? 4 : 16);
	want_len = diag_addr(family, &a6, want_src);
	memset(&a6, 0, sizeof(a6));
	memcpy(&a6, dst, family == AF_INET ? 4 : 16);
	if (want_len == 0 || diag_addr(family, &a6, want_dst) != want_len)
		return 0;

	while (len - off >= NLMSG_HDRLEN) {
		struct nlmsghdr h;
		struct inet_diag_msg d;
		gsize got_len;

		memcpy(&h, p + off, sizeof(h));
		if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > len - off)
			return 0;
		/* only one answer: no error, no other message */
		if (h.nlmsg_seq != seq || h.nlmsg_type != SOCK_DIAG_BY_FAMILY ||
		    h.nlmsg_len < NLMSG_LENGTH(sizeof(d)))
			return 0;
		memcpy(&d, p + off + NLMSG_HDRLEN, sizeof(d));
		if (d.idiag_state != TCP_ESTABLISHED)
			return 0;
		got_len = diag_addr(d.idiag_family, d.id.idiag_src, got_src);
		if (got_len != want_len ||
		    diag_addr(d.idiag_family, d.id.idiag_dst, got_dst) != want_len)
			return 0;
		if (memcmp(got_src, want_src, want_len) != 0 ||
		    memcmp(got_dst, want_dst, want_len) != 0 || ntohs(d.id.idiag_sport) != sport ||
		    ntohs(d.id.idiag_dport) != dport)
			return 0;
		found++;
		found_uid = d.idiag_uid;
		/* the padding of the last message may be missing: it ends
		 * the reply (and off never passes len) */
		off += NLMSG_ALIGN(h.nlmsg_len);
		if (off > len)
			off = len;
	}
	/* bytes left that are no message */
	if (off < len)
		return 0;
	if (found != 1 || (uid_t) found_uid != found_uid || (uid_t) found_uid == (uid_t) -1 ||
	    (uid_t) found_uid == unmapped)
		return 0;
	*uid = (uid_t) found_uid;
	return 1;
}

/* Ask the kernel; only a reply from the kernel itself (port id 0) is
 * read. The reply was queued before sendto() returned. */
static gssize sock_diag_kernel(const void *req, gsize req_len, void *reply, gsize size)
{
	struct sockaddr_nl kernel, from;
	socklen_t from_len = sizeof(from);
	ssize_t n;
	int fd;

	fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
	if (fd < 0)
		return -1;
	memset(&kernel, 0, sizeof(kernel));
	kernel.nl_family = AF_NETLINK;
	if (sendto(fd, req, req_len, 0, (struct sockaddr *) &kernel, sizeof(kernel)) !=
	    (ssize_t) req_len) {
		close(fd);
		return -1;
	}
	memset(&from, 0, sizeof(from));
	do {
		n = recvfrom(fd, reply, size, MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr *) &from,
		             &from_len);
	} while (n < 0 && errno == EINTR);
	close(fd);
	if (n < 0 || from.nl_family != AF_NETLINK || from.nl_pid != 0)
		return -1;
	return n;
}

static FE_WEB_SOCK_DIAG_FUNC sock_diag_func = sock_diag_kernel;

void fe_web_sock_diag_set_func(FE_WEB_SOCK_DIAG_FUNC func)
{
	sock_diag_func = func != NULL ? func : sock_diag_kernel;
}

int fe_web_sock_diag_status(const void *buf, gsize len, guint32 seq)
{
	struct nlmsghdr h;
	int error;

	if (buf == NULL || len < NLMSG_HDRLEN)
		return FE_WEB_DIAG_FAILED;
	memcpy(&h, buf, sizeof(h));
	if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > len || h.nlmsg_seq != seq)
		return FE_WEB_DIAG_FAILED;
	if (h.nlmsg_type == SOCK_DIAG_BY_FAMILY)
		return FE_WEB_DIAG_ANSWER;
	if (h.nlmsg_type != NLMSG_ERROR || h.nlmsg_len < NLMSG_LENGTH(sizeof(error)))
		return FE_WEB_DIAG_FAILED;
	/* struct nlmsgerr starts with the error */
	memcpy(&error, (const guchar *) buf + NLMSG_HDRLEN, sizeof(error));
	return error == -ENOENT ? FE_WEB_DIAG_NOT_FOUND : FE_WEB_DIAG_FAILED;
}

/* One sock_diag question: the socket at (src, sport) connected to (dst,
 * dport). 1 and *uid when it is the one asked for; *status what the
 * reply says about sock_diag (FE_WEB_DIAG_*). */
static int sock_diag_ask(int family, const void *src, int sport, const void *dst, int dport,
                         uid_t unmapped, uid_t *uid, int *status)
{
	static guint32 seq;
	union {
		struct nlmsghdr nlh;
		guchar bytes[sizeof(SOCK_DIAG_REQUEST)];
	} req;
	union {
		struct nlmsghdr nlh;
		guchar bytes[FE_WEB_SOCK_DIAG_REPLY_MAX];
	} reply;
	gsize req_len;
	gssize n;

	*status = FE_WEB_DIAG_FAILED;
	seq++;
	req_len = fe_web_sock_diag_request(req.bytes, sizeof(req.bytes), family, src, sport, dst,
	                                   dport, seq);
	if (req_len == 0)
		return 0;
	n = sock_diag_func(req.bytes, req_len, reply.bytes, sizeof(reply.bytes));
	if (n <= 0 || (gsize) n > sizeof(reply.bytes))
		return 0;
	*status = fe_web_sock_diag_status(reply.bytes, n, seq);
	return fe_web_sock_diag_reply(reply.bytes, n, family, src, sport, dst, dport, seq, unmapped,
	                              uid);
}

/* sock_diag questions in a row that showed it does not work, and when
 * the last of them was asked */
static int diag_failures = 0;
static time_t diag_failed_at = 0;

void fe_web_sock_diag_reset(void)
{
	diag_failures = 0;
	diag_failed_at = 0;
}

/* Not asking sock_diag now: it failed often enough, and not long ago (a
 * clock that went back counts as long ago) */
static gboolean sock_diag_paused(time_t now)
{
	if (diag_failures < FE_WEB_SOCK_DIAG_MAX_FAILS)
		return FALSE;
	return now >= diag_failed_at && now - diag_failed_at < FE_WEB_SOCK_DIAG_BACKOFF;
}

/* The uid of the connection's peer from sock_diag: as IPv4, and for an
 * IPv4 one also as IPv4-mapped IPv6 when not found as IPv4.
 *
 * ENOENT means no such socket, but the kernel says the same when the
 * inet_diag or tcp_diag module is missing. So then our own side of the
 * connection is asked for: it exists while we hold the connection, and
 * if it is not found either, sock_diag does not work. Only a
 * SOCK_DIAG_BY_FAMILY answer (to the question or to that one) shows it
 * works; every other result (another error, no reply, a reply cut short
 * or not from the kernel, no netlink socket) counts as a failure. */
static int tcp_peer_diag_uid(const TCP_PEER *c, uid_t unmapped, time_t now, uid_t *uid)
{
	int found, status;
	uid_t ours;

	if (sock_diag_paused(now))
		return 0;
	if (c->family == AF_INET) {
		found = sock_diag_ask(AF_INET, &c->peer4, c->peer_port, &c->local4, c->local_port,
		                      unmapped, uid, &status);
		if (found != 1 && status == FE_WEB_DIAG_NOT_FOUND)
			found = sock_diag_ask(AF_INET6, &c->peer6, c->peer_port, &c->local6,
			                      c->local_port, unmapped, uid, &status);
	} else {
		found = sock_diag_ask(AF_INET6, &c->peer6, c->peer_port, &c->local6, c->local_port,
		                      unmapped, uid, &status);
	}
	if (found != 1 && status == FE_WEB_DIAG_NOT_FOUND) {
		if (c->family == AF_INET)
			sock_diag_ask(AF_INET, &c->local4, c->local_port, &c->peer4, c->peer_port,
			              unmapped, &ours, &status);
		else
			sock_diag_ask(AF_INET6, &c->local6, c->local_port, &c->peer6, c->peer_port,
			              unmapped, &ours, &status);
	}
	if (status == FE_WEB_DIAG_ANSWER) {
		diag_failures = 0;
	} else {
		if (diag_failures < FE_WEB_SOCK_DIAG_MAX_FAILS)
			diag_failures++;
		diag_failed_at = now;
	}
	return found == 1 ? 1 : 0;
}
#endif

int fe_web_tcp_peer_uid(int fd, FE_WEB_PROC_BUDGET *budget, time_t now, uid_t *uid)
{
#ifdef __linux__
	struct sockaddr_storage peer, local;
	socklen_t plen = sizeof(peer), llen = sizeof(local);
	TCP_PEER c;
	uid_t unmapped;

	if (uid == NULL || getpeername(fd, (struct sockaddr *) &peer, &plen) < 0 ||
	    getsockname(fd, (struct sockaddr *) &local, &llen) < 0)
		return 0;
	if (plen > sizeof(peer) || llen > sizeof(local) ||
	    (peer.ss_family == AF_INET && plen < sizeof(struct sockaddr_in)) ||
	    (peer.ss_family == AF_INET6 && plen < sizeof(struct sockaddr_in6)) ||
	    (local.ss_family == AF_INET && llen < sizeof(struct sockaddr_in)) ||
	    (local.ss_family == AF_INET6 && llen < sizeof(struct sockaddr_in6)))
		return 0;
	if (!tcp_peer_of(&c, (struct sockaddr *) &peer, (struct sockaddr *) &local))
		return 0;
	unmapped = overflow_uid();
	/* the kernel's answer for this one socket */
	if (tcp_peer_diag_uid(&c, unmapped, now, uid) == 1)
		return 1;
	/* else the tables of our network namespace, where a loopback peer
	 * is - they take a while to read, so only within the budget */
	if (budget != NULL && !fe_web_proc_budget_take(budget, now))
		return 0;
	return tcp_peer_proc_uid(&c, "/proc/net", unmapped, uid);
#else
	/* The BSDs and macOS have no /proc/net; their socket tables
	 * (net.inet.tcp.pcblist) are for root only, so the peer stays its
	 * address */
	(void) fd;
	(void) budget;
	(void) now;
	(void) uid;
	return -1;
#endif
}
