#ifndef IRSSI_FE_WEB_FE_WEB_PACING_H
#define IRSSI_FE_WEB_FE_WEB_PACING_H

/* Login pacing of fe-web, per peer, with a ceiling for all suspects.
 *
 * A peer is a string key (fe_web_peer_key_uid(), fe_web_peer_key_ip()).
 * erssi's own uid is never passed here: fe-web-server.c neither paces nor
 * counts it, and does not make it trusted either (it needs no place among
 * the trusted peers).
 *
 * Per peer: after FE_WEB_MAX_FAILS wrong passwords from one peer, with at
 * most FE_WEB_FAIL_WINDOW seconds between each and the one before, one
 * login of that peer is checked every FE_WEB_CHECK_INTERVAL seconds and the
 * others are refused unchecked (429), until FE_WEB_FAIL_WINDOW seconds pass
 * without a wrong password or a login succeeds. Wrong passwords while
 * paced count too, so a peer that keeps guessing stays paced.
 *
 * Every peer is one of:
 * - trusted: it logged in with the right password (the last
 *   FE_WEB_PACING_MAX_TRUSTED such peers, the oldest replaced). Only its
 *   own bucket applies. Only a right password makes a peer trusted.
 * - suspect: it is in the table (a wrong password at most
 *   FE_WEB_PEER_MEMORY seconds ago), or it shares the overflow bucket. Its own
 *   bucket (or the overflow bucket) applies, and so does the ceiling: once
 *   FE_WEB_GLOBAL_MAX_FAILS wrong passwords from all peers came within
 *   FE_WEB_FAIL_WINDOW seconds of each other, all suspects together get
 *   one check every FE_WEB_GLOBAL_CHECK_INTERVAL seconds. A check is
 *   recorded only when every bucket that applies allows it.
 * - clean: not in the table while it has room. Checked freely; its first
 *   wrong password makes it a suspect.
 *
 * The table holds at most FE_WEB_PACING_MAX_PEERS peers. A peer is added
 * on its first wrong password, and dropped when it logs in, or once more
 * than FE_WEB_PEER_MEMORY seconds passed since its last one (601 s and
 * later; when the table is full or the peer is looked up). Nobody can
 * push a peer still remembered out of the table by sending wrong
 * passwords from many addresses; a new peer that finds no room shares the
 * overflow bucket with every other such peer.
 *
 * Bounds: one address gets FE_WEB_MAX_FAILS checks, then one every
 * FE_WEB_CHECK_INTERVAL seconds. Many addresses together get at most
 * FE_WEB_PACING_MAX_PEERS clean checks per FE_WEB_PEER_MEMORY + 1 seconds
 * (a slot is free again only 601 s after the last failure in it),
 * plus FE_WEB_GLOBAL_MAX_FAILS, plus one every FE_WEB_GLOBAL_CHECK_INTERVAL
 * seconds (about 1/s). Clean and trusted peers are not held by them.
 *
 * A clock that goes back counts as time passed: nobody is locked out and
 * the table drains. */

#include <glib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <irssi/src/common.h>

#define FE_WEB_MAX_FAILS 5          /* moved from fe-web-server.c */
#define FE_WEB_CHECK_INTERVAL 2
#define FE_WEB_FAIL_WINDOW 60
#define FE_WEB_PACING_MAX_PEERS 256 /* beyond: one shared overflow bucket */
/* failed checks of all non-owner peers within FE_WEB_FAIL_WINDOW that
 * engage the ceiling */
#define FE_WEB_GLOBAL_MAX_FAILS 20
/* engaged: one check per 2 s for all suspect peers together */
#define FE_WEB_GLOBAL_CHECK_INTERVAL 2
/* peer keys that logged in successfully (since start), replaced
 * oldest-first */
#define FE_WEB_PACING_MAX_TRUSTED 32
/* a table entry may be dropped only 600 s after its last failure */
#define FE_WEB_PEER_MEMORY 600

typedef struct _FE_WEB_PACING FE_WEB_PACING;

FE_WEB_PACING *fe_web_pacing_new(void);
void fe_web_pacing_free(FE_WEB_PACING *pacing);
/* forget every peer, the trusted ones, the overflow bucket and the
 * ceiling */
void fe_web_pacing_reset(FE_WEB_PACING *pacing);

/* FALSE: answer 429 without checking. TRUE: check, and the check is recorded */
gboolean fe_web_pacing_may_check(FE_WEB_PACING *pacing, const char *peer, time_t now);
/* a wrong password; TRUE exactly when this one starts the pacing of peer
 * (of the overflow bucket, when fe_web_pacing_in_overflow() says so) */
gboolean fe_web_pacing_failed(FE_WEB_PACING *pacing, const char *peer, time_t now);
/* a login of peer succeeded: it leaves the table and is trusted, with a
 * fresh bucket of its own. The overflow bucket, which other peers share,
 * is left as it is. */
void fe_web_pacing_succeeded(FE_WEB_PACING *pacing, const char *peer);
/* Is peer counted in the shared overflow bucket: not trusted, not in the
 * table, and the table full */
gboolean fe_web_pacing_in_overflow(FE_WEB_PACING *pacing, const char *peer);
/* peers in the table (for the tests) */
guint fe_web_pacing_table_size(FE_WEB_PACING *pacing);

/* Peer keys, to be freed */
/* "uid:1000" */
char *fe_web_peer_key_uid(unsigned long uid);
/* "ip:192.0.2.7", "ip:2001:db8:1:2::/64"; IPv4-mapped as IPv4. Every
 * loopback address is "ip:loopback": a local user can pick any source
 * address of 127/8, and all of them are this machine. */
char *fe_web_peer_key_ip(const IPADDR *ip);
/* 127/8, ::1, ::ffff:127/104 */
gboolean fe_web_ip_is_loopback(const IPADDR *ip);

/* The uid of a TCP peer on loopback, so that the users of one machine are
 * told apart: on loopback they all have the same address.
 *
 * The peer's socket is the one line of /proc/net/tcp (or tcp6) in state
 * ESTABLISHED (01) whose local address and port are the peer's and whose
 * remote address and port are ours. Any other number of such lines (none,
 * two), or a table that cannot be read whole, means: not known. A line the
 * kernel would not print (a field too long, too short, not a number, the
 * line not ended by a newline) is skipped. */

/* longer lines are not read; the kernel's are about 150 (tcp6: 180) */
#define FE_WEB_PROC_NET_LINE_MAX 512
/* a table longer than this (about 100,000 sockets) is not read at all */
#define FE_WEB_PROC_NET_MAX (16 * 1024 * 1024)

/* the uid that owns the socket at (peer_addr, peer_port) connected to
 * (local_addr, local_port), from the text of /proc/net/tcp (family AF_INET,
 * struct in_addr) or /proc/net/tcp6 (AF_INET6, struct in6_addr); ports in
 * host order. *uid is set only when TRUE. */
gboolean fe_web_proc_net_tcp_uid(const char *table, int family, const void *peer_addr,
                                 int peer_port, const void *local_addr, int local_port,
                                 uid_t *uid);
/* The same for the connection from 'peer' to 'local' (struct sockaddr_in
 * or sockaddr_in6, IPv4-mapped counts as IPv4), from the files tcp and
 * tcp6 of the directory 'proc_net' (/proc/net). An IPv4 connection is
 * looked for in both: in tcp, and in tcp6 as IPv4-mapped (a client's
 * dual-stack socket); a missing file has no lines. 1: found; 0: not
 * found, or the peer is not on loopback, or the uid is 'unmapped' (what
 * the kernel shows for a uid outside our user namespace) */
int fe_web_tcp_sockaddr_uid(const struct sockaddr *peer, const struct sockaddr *local,
                            const char *proc_net, uid_t unmapped, uid_t *uid);

/* /proc/net/tcp is generated anew on every read, about 2 us a socket, so
 * it is read at most FE_WEB_PROC_LOOKUPS_PER_SEC times per second of
 * time() for all peers together. This is a fixed window per time()
 * second, not a sliding one: up to 2 in each second, so up to 4 within
 * a real second across the boundary of two. A clock that went back
 * starts a new window. Zeroed: the whole budget left. */
#define FE_WEB_PROC_LOOKUPS_PER_SEC 2
typedef struct {
	int spent;
	time_t last;
} FE_WEB_PROC_BUDGET;
/* TRUE: a /proc lookup may be made now (and it is counted) */
gboolean fe_web_proc_budget_take(FE_WEB_PROC_BUDGET *budget, time_t now);

/* 1: uid found; 0: not found or the peer is not on loopback; -1: no way on
 * this system (not Linux). On Linux the kernel is asked for the one socket
 * (NETLINK_SOCK_DIAG); when it gives no answer, /proc/net/tcp and tcp6
 * are read if 'budget' (NULL: no limit) allows it at 'now'. */
int fe_web_tcp_peer_uid(int fd, FE_WEB_PROC_BUDGET *budget, time_t now, uid_t *uid);

#ifdef __linux__
/* NETLINK_SOCK_DIAG, the parts tested on their own.
 *
 * The request for the TCP socket whose local side is (src, sport) and
 * remote side (dst, dport): family AF_INET (struct in_addr) or AF_INET6
 * (struct in6_addr, IPv4-mapped allowed), ports in host order. Written to
 * buf; its length, 0 when it does not fit or the arguments are wrong. */
gsize fe_web_sock_diag_request(void *buf, gsize size, int family, const void *src, int sport,
                               const void *dst, int dport, guint32 seq);
/* The reply to that request: 1 and *uid when it is exactly one socket
 * (one SOCK_DIAG_BY_FAMILY message of 'seq', nothing else), ESTABLISHED,
 * with the tuple asked for (IPv4 and IPv4-mapped count as the same), and
 * its uid neither 'unmapped' nor -1. Anything else: 0. */
int fe_web_sock_diag_reply(const void *buf, gsize len, int family, const void *src, int sport,
                           const void *dst, int dport, guint32 seq, uid_t unmapped,
                           uid_t *uid);
/* the biggest reply read; a longer one is not used */
#define FE_WEB_SOCK_DIAG_REPLY_MAX 8192
/* Send the request to the kernel and read its reply into 'reply'; the
 * reply's length (more than 'size' when it did not fit), -1 on error */
typedef gssize (*FE_WEB_SOCK_DIAG_FUNC)(const void *req, gsize req_len, void *reply,
                                        gsize size);
/* for the tests: what asks the kernel (NULL: the kernel itself) */
void fe_web_sock_diag_set_func(FE_WEB_SOCK_DIAG_FUNC func);

/* What a reply says about sock_diag itself (its first message, of seq):
 * ANSWER, a SOCK_DIAG_BY_FAMILY message - sock_diag works, whatever the
 * socket; NOT_FOUND, NLMSG_ERROR -ENOENT - no such socket, or no
 * inet_diag/tcp_diag module (the kernel answers both the same way);
 * FAILED, anything else (another error, a reply cut short, not ours). */
#define FE_WEB_DIAG_ANSWER 1
#define FE_WEB_DIAG_NOT_FOUND 0
#define FE_WEB_DIAG_FAILED -1
int fe_web_sock_diag_status(const void *buf, gsize len, guint32 seq);

/* Each question may make the kernel load a module (modprobe, in the
 * accept path). After FE_WEB_SOCK_DIAG_MAX_FAILS questions in a row
 * that show sock_diag does not work, it is not asked for
 * FE_WEB_SOCK_DIAG_BACKOFF seconds (a clock that went back ends that),
 * then tried again; a working answer sets the count back to 0. */
#define FE_WEB_SOCK_DIAG_MAX_FAILS 3
#define FE_WEB_SOCK_DIAG_BACKOFF 60
/* forget the failures (for the tests) */
void fe_web_sock_diag_reset(void);
#endif

#endif
