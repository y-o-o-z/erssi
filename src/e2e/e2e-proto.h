#ifndef IRSSI_E2E_PROTO_H
#define IRSSI_E2E_PROTO_H

/*
 The RPE2E protocol on an in-memory keyring (e2e-keyring.h): identity,
 key exchange (KEYREQ / KEYRSP / REKEY), trust decisions, the outbound
 gate and the decryption of incoming messages - rpe2e.pl's logic without
 irssi. Loading, saving and printing are the caller's.

 Contexts: a channel name, or "@ident@host" for a query. Outgoing DM keys
 and the config of a query are keyed by the PEER's handle ("@<peer>"),
 incoming DM sessions by OUR handle ("@<own>", recipient-keyed, see
 repartee's docs/rpe2e-dm-addendum.md).
*/

#include "e2e-json.h"
#include "e2e-wire.h"

#define E2E_KEYREQ_MIN_INTERVAL         30
#define E2E_KEYREQ_INBOUND_MIN_INTERVAL 10
#define E2E_PENDING_KEYREQ_TTL          120
/* growth limits of what anyone can make us store */
#define E2E_MAX_PENDING_INBOUND         64
#define E2E_MAX_UNACCEPTED_PEERS        256
#define E2E_MAX_PENDING_OUT             256
#define E2E_MAX_TRUST_CHANGES           128
#define E2E_MAX_SEEN_REKEYS             1024

/* seconds since the epoch; tests replace it */
extern gint64 (*e2e_now)(void);

typedef struct {
	unsigned char pk[32];
	unsigned char sk[64];	/* seed || pk, as libsodium and keyring.json */
	unsigned char fp[16];
	char fp_hex[33];
} E2E_IDENTITY;

/* The keyring's identity. A missing one is created into kr when create is
   set (*created tells; the caller saves). A damaged one is an error, never
   replaced. */
gboolean e2e_identity_get(E2E_JSON *kr, E2E_IDENTITY *id, gboolean create,
                          gboolean *created, char **error);
void e2e_identity_wipe(E2E_IDENTITY *id);
char *e2e_fingerprint_hex(const unsigned char pk[32]);

/* "at most once per interval seconds" per key, expired stamps pruned once
   the map holds more than 256 */
GHashTable *e2e_stamps_new(void);
gboolean e2e_stamp_allow(GHashTable *stamps, const char *key, int interval);

/* ---- keyring lookups ---- */
gboolean e2e_ctx_enabled(const E2E_JSON *kr, const char *ctx);
/* the newest peer seen with this nick (case-insensitive): its handle */
const char *e2e_find_handle_by_nick(const E2E_JSON *kr, const char *nick);
/* the peer whose last handle this is: its fingerprint (hex), and *peer */
const char *e2e_find_peer_by_handle(const E2E_JSON *kr, const char *handle, E2E_JSON **peer);
/* rpe2e.pl's _glob_match_ci: * and ?, ASCII case-insensitive */
gboolean e2e_glob_match(const char *pattern, const char *text);
gboolean e2e_autotrust_matches(const E2E_JSON *kr, const char *handle, const char *ctx);
/* the outgoing key of ctx; a new one when there is none or a rotation is
   due (*rotated). FALSE when a stored key is damaged. */
gboolean e2e_outgoing_key(E2E_JSON *kr, const char *ctx, unsigned char key[32],
                          gboolean *rotated, gboolean *generated);

/* ---- key exchange ---- */

/* "\001RPEE2E KEYREQ ...\001"; NULL and *error (e.g. "key exchange already
   pending for ...") */
char *e2e_build_keyreq(E2E_JSON *kr, const E2E_IDENTITY *id, const char *ctx,
                       const char *handle, char **error);
char *e2e_build_keyrsp_for_req(E2E_JSON *kr, const E2E_IDENTITY *id, const char *ctx,
                               const char *sender_handle, const unsigned char req_pub[32],
                               const unsigned char req_eph[32]);
char *e2e_build_rekey(const E2E_IDENTITY *id, const char *ctx,
                      const unsigned char peer_pk[32], const unsigned char new_key[32]);
/* the KEYREQ we send back after accepting one (NULL if not needed) */
char *e2e_build_reciprocal_keyreq_on_accept(E2E_JSON *kr, const E2E_IDENTITY *id,
                                            const char *ctx, const char *sender_handle,
                                            const char *own_handle);

/* An incoming KEYREQ: *rsp (KEYRSP to send) and *reciprocal (our own
   KEYREQ back) when they are due; both NULL when it is ignored or waits
   for /e2e accept (then pending_inbound has it). outgoing_stamps is the
   30 s limiter of KEYREQs we send. */
void e2e_handle_keyreq(E2E_JSON *kr, const E2E_IDENTITY *id, GHashTable *outgoing_stamps,
                       const char *sender_handle, const char *nick, const char *body,
                       const char *own_handle, char **rsp, char **reciprocal);
/* TRUE when a session was installed */
gboolean e2e_handle_keyrsp(E2E_JSON *kr, const char *sender_handle, const char *nick,
                           const char *body);
gboolean e2e_handle_rekey(E2E_JSON *kr, const E2E_IDENTITY *id, const char *sender_handle,
                          const char *nick, const char *body);
/* "at most once per 10 s" for each kind of handshake, sender and context */
gboolean e2e_handshake_allow(GHashTable *stamps, E2E_HS_TYPE type, const char *sender,
                             const char *ctx);

/* ---- trust decisions of the user ---- */

/* the session row holds a real key (32 bytes, not all zero) */
gboolean e2e_session_has_key(const E2E_JSON *row);
/* handle may receive our key for ctx: /e2e accept, autotrust or auto-accept
   mode said so for this fingerprint ("accepted" in the keyring) */
gboolean e2e_peer_accepted(const E2E_JSON *kr, const char *handle, const char *ctx,
                           const char *fp_hex);
/* the incoming session handle|ctx (fingerprint fp) is of a peer the user
   accepted - for a DM session ("@<own>") the acceptance of "@<handle>" */
gboolean e2e_session_accepted(const E2E_JSON *kr, const char *handle, const char *ctx,
                              const char *fp);
/* the ident@host of every "@<handle>" config with E2E on that belongs to
   nick (its "nick" member, or a peer last seen as nick there) */
GPtrArray *e2e_dm_configs_of(const E2E_JSON *kr, const char *nick);
/* /e2e off in the query with nick (live: its ident@host now, or NULL):
   the "@<handle>" contexts to turn off */
GPtrArray *e2e_query_off_targets(const E2E_JSON *kr, const char *nick, const char *live);
/* some "accepted" row is for this fingerprint */
gboolean e2e_fingerprint_accepted(const E2E_JSON *kr, const char *fp_hex);
/* /e2e revoke, decline, forget: handle no longer gets our key for ctx */
void e2e_forget_acceptance(E2E_JSON *kr, const char *handle, const char *ctx);

typedef enum {
	E2E_ACCEPT_SENT,	/* a pending request answered: *rsp (and *reciprocal) */
	E2E_ACCEPT_TRUSTED,	/* no pending request: the session with a key trusted */
	E2E_ACCEPT_NO_KEY,	/* only a session without a real key: nothing changed */
	E2E_ACCEPT_NOTHING,	/* neither a pending request nor a session */
	E2E_ACCEPT_DAMAGED,	/* the pending request does not decode */
	E2E_ACCEPT_FAILED	/* the KEYRSP could not be built */
} E2E_ACCEPT_RESULT;

/* /e2e accept: handle and ctx as in pending_inbound; own_ctx is "@<own>"
   for a query (where its DM session lives), else NULL */
E2E_ACCEPT_RESULT e2e_accept(E2E_JSON *kr, const E2E_IDENTITY *id, const char *handle,
                             const char *ctx, const char *own_ctx, const char *own_handle,
                             char **rsp, char **reciprocal);
/* /e2e unrevoke: the sessions of handle on ctx and own_ctx that hold a key
   are trusted again; how many (0: nothing changed) */
int e2e_unrevoke(E2E_JSON *kr, const char *handle, const char *ctx, const char *own_ctx);

/* ---- the outbound gate ---- */

/* live handle of a nick (open query, common channel) or the keyring's,
   g_free'd by the caller; NULL when unknown */
typedef char *(*E2E_RESOLVE_FUNC)(const char *nick, const E2E_JSON *kr, void *data);

typedef enum {
	E2E_GATE_PASS,		/* send the line unchanged */
	E2E_GATE_CIPHER,	/* send wires instead */
	E2E_GATE_REFUSE,	/* send nothing, show message */
	E2E_GATE_BYPASS		/* bot command: send in clear text, show message if set */
} E2E_GATE_ACTION;

typedef struct {
	E2E_GATE_ACTION action;
	char *ctx;
	GPtrArray *wires;	/* CIPHER: the message bodies */
	GPtrArray *notices;	/* REKEYs to send first: nick, body, nick, body ... */
	GPtrArray *warnings;	/* to show (rekey problems) */
	char *message;
	gboolean save_needed;	/* a key was generated */
} E2E_GATE_RESULT;

#define E2E_REFUSE_NO_HANDLE "cannot encrypt PM without peer handle — wait for a message from them first"
#define E2E_REFUSE_KEYRING   "cannot encrypt — keyring read failed; message NOT sent (E2E stays on)"
#define E2E_REFUSE_ENCRYPT   "encryption failed — message NOT sent as plaintext (use /e2e off to send cleartext)"
#define E2E_REFUSE_TOO_LONG  "the encrypted line would be longer than the server allows (target name too long) — message NOT sent"
#define E2E_REFUSE_LOST      "cannot encrypt — the keyring was corrupt and is moved aside, E2E state is lost; message NOT sent. /e2e reset starts a new keyring"

/* kr_ok: the keyring was read (an unreadable one may hold an enabled
   context: everything but a bot command is refused). id may be NULL
   (no REKEY then). */
/* the ident@host of another "@<handle>" config with E2E on for nick (a
   query's "nick" member, or a peer last seen as nick) than live; NULL
   when there is none. The gate refuses a DM to nick then. */
char *e2e_enabled_dm_elsewhere(const E2E_JSON *kr, const char *nick, const char *live);
void e2e_gate_decide(E2E_JSON *kr, gboolean kr_ok, const E2E_IDENTITY *id,
                     const char *target, const char *body,
                     E2E_RESOLVE_FUNC resolve, void *resolve_data, E2E_GATE_RESULT *res);
void e2e_gate_result_clear(E2E_GATE_RESULT *res);
/* every IRC line of the wires fits max_len bytes (without CR LF) */
gboolean e2e_gate_lines_fit(const E2E_GATE_RESULT *res, const char *tags, const char *target,
                            gsize max_len);
/* An outgoing buffer with more than one IRC line in it (CR, LF or NUL
   before its end): the refusal message when one of its PRIVMSGs would
   need encryption or be refused by the gate, NULL when it may go out */
char *e2e_gate_multiline(E2E_JSON *kr, gboolean kr_ok, const char *buf, gsize len,
                         E2E_RESOLVE_FUNC resolve, void *resolve_data);
/* one of the IRC lines in buf (split at CR, LF and NUL) is a PRIVMSG */
gboolean e2e_buffer_has_privmsg(const char *buf, gsize len);

/* ---- incoming messages ---- */

typedef struct {
	const char *nick;	/* sender */
	const char *handle;	/* sender's ident@host */
	const char *target;	/* channel; DM: the sender's nick, own line: the recipient
				   (a target that is no channel name makes it a DM) */
	char **alt_ctxs;	/* other readings of an ambiguous +#chan / &#chan, or NULL */
	gboolean is_own_line;	/* our own message relayed back (bouncer) */
	const char *own_handle;	/* NULL while unknown; a DM then has to wait */
	E2E_RESOLVE_FUNC resolve;
	void *resolve_data;
	GHashTable *keyreq_stamps;	/* the 30 s limiter of our KEYREQs */
} E2E_IN_PARAMS;

typedef enum {
	E2E_IN_NOT_WIRE,	/* not an RPE2E message: leave it alone */
	E2E_IN_DROP,		/* hide it */
	E2E_IN_PLAIN,		/* show plain instead */
	E2E_IN_WAIT_OWN		/* a DM before our handle is known: hide, tell */
} E2E_IN_ACTION;

typedef struct {
	E2E_IN_ACTION action;
	char *plain;		/* PLAIN: valid UTF-8 */
	char *ctx;
	char *keyreq;		/* DROP: an automatic KEYREQ to send to the sender */
	char *debug;		/* why it was dropped */
	gboolean save_needed;
} E2E_IN_RESULT;

void e2e_decrypt_incoming(E2E_JSON *kr, const E2E_IDENTITY *id, const E2E_IN_PARAMS *p,
                          const char *text, E2E_IN_RESULT *res);
void e2e_in_result_clear(E2E_IN_RESULT *res);

/* ---- /e2e export and import ---- */
E2E_JSON *e2e_keyring_export(const E2E_JSON *kr);
/* NULL and *error when doc is no keyring export */
E2E_JSON *e2e_keyring_import(const E2E_JSON *doc, char **error);

#endif
