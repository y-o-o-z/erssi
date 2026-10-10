#ifndef IRSSI_FE_WEB_FE_WEB_PACING_H
#define IRSSI_FE_WEB_FE_WEB_PACING_H

/* Login pacing of fe-web, per peer.
 *
 * After FE_WEB_MAX_FAILS wrong passwords from one peer, with less than
 * FE_WEB_FAIL_WINDOW seconds between each and the one before, one login of
 * that peer is checked every FE_WEB_CHECK_INTERVAL seconds and the others
 * are refused unchecked (429), until FE_WEB_FAIL_WINDOW seconds pass
 * without a wrong password or a login succeeds. Not a lockout: one peer
 * guessing cannot keep the others out.
 *
 * A peer is a string key (fe_web_peer_key_uid(), fe_web_peer_key_ip()).
 * The table holds at most FE_WEB_PACING_MAX_PEERS peers, those with a
 * wrong password in their window; a peer is added on its first wrong
 * password. When it is full, peers whose window has passed are dropped,
 * never one still counting, so nobody can push a paced peer out by sending
 * wrong passwords from many addresses. A new peer that still finds no
 * room shares one overflow bucket with every other such peer. */

#include <glib.h>
#include <time.h>

#include <irssi/src/common.h>

#define FE_WEB_MAX_FAILS 5          /* moved from fe-web-server.c */
#define FE_WEB_CHECK_INTERVAL 2
#define FE_WEB_FAIL_WINDOW 60
#define FE_WEB_PACING_MAX_PEERS 256 /* beyond: one shared overflow bucket */

typedef struct _FE_WEB_PACING FE_WEB_PACING;

FE_WEB_PACING *fe_web_pacing_new(void);
void fe_web_pacing_free(FE_WEB_PACING *pacing);
/* forget every peer */
void fe_web_pacing_reset(FE_WEB_PACING *pacing);

/* FALSE: answer 429 without checking. TRUE: check, and the check is recorded */
gboolean fe_web_pacing_may_check(FE_WEB_PACING *pacing, const char *peer, time_t now);
/* a wrong password; TRUE exactly when this one starts the pacing of peer */
gboolean fe_web_pacing_failed(FE_WEB_PACING *pacing, const char *peer, time_t now);
/* a login of peer succeeded: its count starts again (not the overflow
 * bucket's, which other peers share) */
void fe_web_pacing_succeeded(FE_WEB_PACING *pacing, const char *peer);

/* Peer keys, to be freed */
/* "uid:1000" */
char *fe_web_peer_key_uid(unsigned long uid);
/* "ip:192.0.2.7", "ip:2001:db8:1:2::/64"; IPv4-mapped as IPv4 */
char *fe_web_peer_key_ip(const IPADDR *ip);
/* 127/8, ::1, ::ffff:127/104 */
gboolean fe_web_ip_is_loopback(const IPADDR *ip);

#endif
