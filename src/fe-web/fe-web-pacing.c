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
	gboolean skipping = FALSE, ok = FALSE;
	int fd;

	path = g_build_filename(dir, name, NULL);
	fd = open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
	g_free(path);
	if (fd < 0)
		return errno == ENOENT;

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

int fe_web_tcp_sockaddr_uid(const struct sockaddr *peer, const struct sockaddr *local,
                            const char *proc_net, uid_t unmapped, uid_t *uid)
{
	TCP_LOOKUP t4, t6;
	struct in_addr p4, l4;
	struct in6_addr p6, l6;
	int pport, lport = 0, matches;
	uid_t found;

	memset(&l4, 0, sizeof(l4));

	if (peer == NULL || local == NULL || proc_net == NULL || uid == NULL)
		return 0;
	if (sockaddr_v4(peer, &p4, &pport)) {
		/* IPv4, or IPv4-mapped: the peer's socket is in tcp, or in
		 * tcp6 as IPv4-mapped when it is a dual-stack one */
		if (!sockaddr_v4(local, &l4, &lport))
			return 0;
		if ((ntohl(p4.s_addr) >> 24) != 127)
			return 0;
		p6 = mapped_v6(&p4);
		l6 = mapped_v6(&l4);
		if (!tcp_lookup_init(&t4, AF_INET, &p4, pport, &l4, lport) ||
		    !tcp_lookup_init(&t6, AF_INET6, &p6, pport, &l6, lport) ||
		    !tcp_lookup_file(&t4, proc_net, "tcp") ||
		    !tcp_lookup_file(&t6, proc_net, "tcp6"))
			return 0;
		matches = t4.matches + t6.matches;
		found = t4.matches > 0 ? t4.uid : t6.uid;
	} else if (peer->sa_family == AF_INET6) {
		const struct sockaddr_in6 *ps = (const struct sockaddr_in6 *) peer;
		const struct sockaddr_in6 *ls = (const struct sockaddr_in6 *) local;

		if (local->sa_family != AF_INET6 || IN6_IS_ADDR_V4MAPPED(&ls->sin6_addr))
			return 0;
		if (!IN6_IS_ADDR_LOOPBACK(&ps->sin6_addr))
			return 0;
		if (!tcp_lookup_init(&t6, AF_INET6, &ps->sin6_addr, ntohs(ps->sin6_port),
		                     &ls->sin6_addr, ntohs(ls->sin6_port)) ||
		    !tcp_lookup_file(&t6, proc_net, "tcp6"))
			return 0;
		matches = t6.matches;
		found = t6.uid;
	} else {
		return 0;
	}
	/* exactly one socket; and not the uid the kernel shows when it
	 * cannot show the real one in our user namespace */
	if (matches != 1 || found == unmapped)
		return 0;
	*uid = found;
	return 1;
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
#endif

int fe_web_tcp_peer_uid(int fd, uid_t *uid)
{
#ifdef __linux__
	struct sockaddr_storage peer, local;
	socklen_t plen = sizeof(peer), llen = sizeof(local);

	if (getpeername(fd, (struct sockaddr *) &peer, &plen) < 0 ||
	    getsockname(fd, (struct sockaddr *) &local, &llen) < 0)
		return 0;
	if (plen > sizeof(peer) || llen > sizeof(local) ||
	    (peer.ss_family == AF_INET && plen < sizeof(struct sockaddr_in)) ||
	    (peer.ss_family == AF_INET6 && plen < sizeof(struct sockaddr_in6)) ||
	    (local.ss_family == AF_INET && llen < sizeof(struct sockaddr_in)) ||
	    (local.ss_family == AF_INET6 && llen < sizeof(struct sockaddr_in6)))
		return 0;
	/* the tables of our network namespace, where a loopback peer is */
	return fe_web_tcp_sockaddr_uid((struct sockaddr *) &peer, (struct sockaddr *) &local,
	                               "/proc/net", overflow_uid(), uid);
#else
	/* The BSDs and macOS have no /proc/net; their socket tables
	 * (net.inet.tcp.pcblist) are for root only, so the peer stays its
	 * address */
	(void) fd;
	(void) uid;
	return -1;
#endif
}
