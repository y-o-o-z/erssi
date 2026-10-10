#ifndef IRSSI_FE_WEB_FE_WEB_PACING_H
#define IRSSI_FE_WEB_FE_WEB_PACING_H

/* Login pacing of fe-web, per peer, with a ceiling for all suspects.
 *
 * A peer is a string key (fe_web_peer_key_uid(), fe_web_peer_key_ip()).
 * erssi's own uid is never passed here (fe-web-server.c).
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
 * - suspect: it is in the table (a wrong password in the last
 *   FE_WEB_PEER_MEMORY seconds), or it shares the overflow bucket. Its own
 *   bucket (or the overflow bucket) applies, and so does the ceiling: once
 *   FE_WEB_GLOBAL_MAX_FAILS wrong passwords from all peers came within
 *   FE_WEB_FAIL_WINDOW seconds of each other, all suspects together get
 *   one check every FE_WEB_GLOBAL_CHECK_INTERVAL seconds. A check is
 *   recorded only when every bucket that applies allows it.
 * - clean: not in the table while it has room. Checked freely; its first
 *   wrong password makes it a suspect.
 *
 * The table holds at most FE_WEB_PACING_MAX_PEERS peers. A peer is added
 * on its first wrong password and dropped FE_WEB_PEER_MEMORY seconds after
 * its last one (when the table is full or the peer is looked up), or when
 * it logs in. Nobody can push a peer still remembered out of the table by
 * sending wrong passwords from many addresses; a new peer that finds no
 * room shares the overflow bucket with every other such peer.
 *
 * Bounds: one address gets FE_WEB_MAX_FAILS checks, then one every
 * FE_WEB_CHECK_INTERVAL seconds. Many addresses together get at most
 * FE_WEB_PACING_MAX_PEERS clean checks per FE_WEB_PEER_MEMORY seconds,
 * plus FE_WEB_GLOBAL_MAX_FAILS, plus one every FE_WEB_GLOBAL_CHECK_INTERVAL
 * seconds (about 1/s). Clean and trusted peers are not held by them.
 *
 * A clock that goes back counts as time passed: nobody is locked out and
 * the table drains. */

#include <glib.h>
#include <time.h>

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
/* "ip:192.0.2.7", "ip:2001:db8:1:2::/64"; IPv4-mapped as IPv4 */
char *fe_web_peer_key_ip(const IPADDR *ip);
/* 127/8, ::1, ::ffff:127/104 */
gboolean fe_web_ip_is_loopback(const IPADDR *ip);

#endif
