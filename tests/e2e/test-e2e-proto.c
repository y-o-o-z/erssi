/*
 test-e2e-proto.c : key exchange, outbound gate and decryption of RPE2E

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 Two in-memory keyrings talk to each other through the functions the
 module calls, and the native side talks to messages that rpe2e.pl made
 (rpe2e-vectors.h). The behaviour checked is rpe2e.pl 0.2.2's.
*/

#include <glib.h>
#include <string.h>

#include <irssi/src/e2e/e2e-crypto.h>
#include <irssi/src/e2e/e2e-keyring.h>
#include <irssi/src/e2e/e2e-proto.h>
#include <irssi/src/e2e/e2e-wire.h>

#include "rpe2e-vectors.h"

#define ALICE "alice@127.0.0.1"
#define BOB   "bob@127.0.0.1"

static gint64 fake_now = 1791564129;

static gint64 fixed_now(void)
{
	return fake_now;
}

typedef struct {
	E2E_JSON *kr;
	E2E_IDENTITY id;
	GHashTable *stamps;
	const char *nick, *handle;
} PEER;

static void peer_init(PEER *p, const char *nick, const char *handle, int seed_byte)
{
	gboolean created = FALSE;
	char *error = NULL;

	p->kr = e2e_keyring_new();
	p->stamps = e2e_stamps_new();
	p->nick = nick;
	p->handle = handle;
	if (seed_byte >= 0) {
		unsigned char seed[32], pk[32], sk[64];
		E2E_JSON *ident = e2e_json_new(E2E_JSON_OBJECT);
		char *s;

		memset(seed, seed_byte, sizeof(seed));
		g_assert_true(e2e_ed25519_seed_keypair(seed, pk, sk));
		s = e2e_b64_encode(pk, 32);
		e2e_json_set_string(ident, "pk", s);
		g_free(s);
		s = e2e_b64_encode(sk, 64);
		e2e_json_set_string(ident, "sk", s);
		g_free(s);
		s = e2e_fingerprint_hex(pk);
		e2e_json_set_string(ident, "fp", s);
		g_free(s);
		e2e_json_set_int(ident, "created_at", 1);
		e2e_json_set(p->kr, "identity", ident);
	}
	g_assert_true(e2e_identity_get(p->kr, &p->id, TRUE, &created, &error));
	g_assert_true(created == (seed_byte < 0));
}

static void peer_free(PEER *p)
{
	e2e_json_free(p->kr);
	g_hash_table_destroy(p->stamps);
}

static void enable(PEER *p, const char *ctx, const char *mode)
{
	E2E_JSON *cfg = e2e_json_new(E2E_JSON_OBJECT);

	e2e_json_set_int(cfg, "enabled", 1);
	e2e_json_set_string(cfg, "mode", mode);
	e2e_json_set(e2e_json_get(p->kr, "channels"), ctx, cfg);
}

static E2E_JSON *path_get(E2E_JSON *node, ...)
{
	va_list va;
	const char *key;

	va_start(va, node);
	while (node != NULL && (key = va_arg(va, const char *)) != NULL)
		node = e2e_json_get(node, key);
	va_end(va);
	return node;
}

/* the CTCP body without the \001 framing */
static char *unframe(const char *ctcp)
{
	g_assert_true(ctcp[0] == '\001' && ctcp[strlen(ctcp) - 1] == '\001');
	return g_strndup(ctcp + 1, strlen(ctcp) - 2);
}

static void deliver_keyreq(PEER *to, PEER *from, const char *ctcp, const char *own,
                           char **rsp, char **reciprocal)
{
	char *body = unframe(ctcp);

	e2e_handle_keyreq(to->kr, &to->id, to->stamps, from->handle, from->nick, body, own,
	                  rsp, reciprocal);
	g_free(body);
}

static gboolean deliver_keyrsp(PEER *to, PEER *from, const char *ctcp)
{
	char *body = unframe(ctcp);
	gboolean ok = e2e_handle_keyrsp(to->kr, from->handle, from->nick, body);

	g_free(body);
	return ok;
}

static char *no_resolve(const char *nick, const E2E_JSON *kr, void *data)
{
	return NULL;
}

static char *table_resolve(const char *nick, const E2E_JSON *kr, void *data)
{
	const char *h = g_hash_table_lookup(data, nick);

	return h != NULL ? g_strdup(h) : NULL;
}

/* run one channel message through the gate of from and the decryption of to */
static char *send_channel(PEER *from, PEER *to, const char *chan, const char *text)
{
	E2E_GATE_RESULT g;
	E2E_IN_PARAMS in;
	E2E_IN_RESULT r;
	GString *got = g_string_new(NULL);
	guint i;

	e2e_gate_decide(from->kr, TRUE, &from->id, chan, text, no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	memset(&in, 0, sizeof(in));
	in.nick = from->nick;
	in.handle = from->handle;
	in.target = chan;
	in.resolve = no_resolve;
	in.keyreq_stamps = to->stamps;
	for (i = 0; i < g.wires->len; i++) {
		e2e_decrypt_incoming(to->kr, &to->id, &in, g_ptr_array_index(g.wires, i), &r);
		g_assert_cmpint(r.action, ==, E2E_IN_PLAIN);
		g_string_append(got, r.plain);
		e2e_in_result_clear(&r);
	}
	e2e_gate_result_clear(&g);
	return g_string_free(got, FALSE);
}

static void test_identity(void)
{
	E2E_JSON *kr = e2e_keyring_new();
	E2E_IDENTITY id, id2;
	gboolean created;
	char *error = NULL;
	E2E_JSON *ident;

	g_assert_false(e2e_identity_get(kr, &id, FALSE, &created, &error));
	g_free(error);
	error = NULL;
	g_assert_true(e2e_identity_get(kr, &id, TRUE, &created, &error));
	g_assert_true(created);
	g_assert_true(e2e_identity_get(kr, &id2, FALSE, &created, &error));
	g_assert_false(created);
	g_assert_cmpmem(id.sk, 64, id2.sk, 64);
	g_assert_cmpstr(id.fp_hex, ==, e2e_json_get_string(e2e_json_get(kr, "identity"), "fp"));

	/* a damaged identity is an error, never silently replaced */
	ident = e2e_json_get(kr, "identity");
	e2e_json_set_string(ident, "fp", "00000000000000000000000000000000");
	g_assert_false(e2e_identity_get(kr, &id2, TRUE, &created, &error));
	g_assert_nonnull(error);
	g_free(error);
	g_assert_cmpstr(e2e_json_get_string(ident, "fp"), ==, "00000000000000000000000000000000");
	e2e_json_free(kr);
}

/* the whole channel exchange in "normal" mode, both sides native */
static void test_handshake_normal_mode(void)
{
	PEER a, b;
	char *req, *rsp = NULL, *recip = NULL, *rsp2 = NULL, *recip2 = NULL, *error = NULL;
	char *got;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&a, "#test", "normal");
	enable(&b, "#test", "normal");

	/* /e2e handshake bob (on alice's side) */
	req = e2e_build_keyreq(a.kr, &a.id, "#test", BOB, &error);
	g_assert_nonnull(req);
	g_assert_nonnull(path_get(a.kr, "pending", "#test|" BOB, "eph_sk", NULL));
	/* a second one within 120 s is refused */
	g_assert_null(e2e_build_keyreq(a.kr, &a.id, "#test", BOB, &error));
	g_assert_cmpstr(error, ==, "key exchange already pending for #test|" BOB);
	g_free(error);

	/* bob is in normal mode: it waits for /e2e accept */
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_null(rsp);
	g_assert_null(recip);
	g_assert_nonnull(path_get(b.kr, "pending_inbound", ALICE "|#test", NULL));
	g_assert_cmpstr(e2e_json_get_string(path_get(b.kr, "incoming", ALICE "|#test", NULL), "status"), ==, "pending");
	g_free(req);

	/* /e2e accept alice: the KEYRSP from the stored request + our KEYREQ */
	g_assert_cmpint(e2e_accept(b.kr, &b.id, ALICE, "#test", NULL, BOB, &rsp, &recip), ==,
	                E2E_ACCEPT_SENT);
	g_assert_null(path_get(b.kr, "pending_inbound", ALICE "|#test", NULL));
	g_assert_true(e2e_peer_accepted(b.kr, ALICE, "#test", a.id.fp_hex));
	g_assert_nonnull(rsp);
	g_assert_nonnull(recip);
	g_assert_true(deliver_keyrsp(&a, &b, rsp));
	g_assert_cmpstr(e2e_json_get_string(path_get(a.kr, "incoming", BOB "|#test", NULL), "status"), ==, "trusted");
	g_assert_null(path_get(a.kr, "pending", "#test|" BOB, NULL));
	/* the same KEYRSP again: nothing pending any more */
	g_assert_false(deliver_keyrsp(&a, &b, rsp));

	/* bob -> alice works now */
	got = send_channel(&b, &a, "#test", "za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 g\xc4\x99\xc5\x9bl\xc4\x85 ja\xc5\xba\xc5\x84");
	g_assert_cmpstr(got, ==, "za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 g\xc4\x99\xc5\x9bl\xc4\x85 ja\xc5\xba\xc5\x84");
	g_free(got);

	/* bob's reciprocal KEYREQ: alice holds bob's key, but holding it is
	   no decision of hers to give him her own - it waits for her
	   /e2e accept as well, without touching the session she has */
	deliver_keyreq(&a, &b, recip, ALICE, &rsp2, &recip2);
	g_assert_null(rsp2);
	g_assert_null(recip2);
	g_assert_nonnull(path_get(a.kr, "pending_inbound", BOB "|#test", NULL));
	g_assert_cmpstr(e2e_json_get_string(path_get(a.kr, "incoming", BOB "|#test", NULL), "status"), ==, "trusted");
	g_assert_true(e2e_session_has_key(path_get(a.kr, "incoming", BOB "|#test", NULL)));
	g_assert_cmpint(e2e_accept(a.kr, &a.id, BOB, "#test", NULL, ALICE, &rsp2, &recip2), ==,
	                E2E_ACCEPT_SENT);
	g_assert_nonnull(rsp2);
	g_assert_null(recip2);	/* she has bob's key already */
	g_assert_true(deliver_keyrsp(&b, &a, rsp2));
	got = send_channel(&a, &b, "#test", "hello bob");
	g_assert_cmpstr(got, ==, "hello bob");
	g_free(got);

	/* a long message: several chunks, each decrypted on its own */
	{
		char *longtext = g_strnfill(500, 'q');

		got = send_channel(&a, &b, "#test", longtext);
		g_assert_cmpstr(got, ==, longtext);
		g_free(got);
		g_free(longtext);
	}

	/* accepted once: bob's next request (e.g. after he lost his keys) is
	   answered without asking again */
	g_free(rsp2);
	g_free(recip2);
	e2e_json_remove(e2e_json_get(b.kr, "pending"), "#test|" ALICE);
	req = e2e_build_keyreq(b.kr, &b.id, "#test", ALICE, &error);
	g_assert_nonnull(req);
	g_hash_table_remove_all(a.stamps);
	deliver_keyreq(&a, &b, req, ALICE, &rsp2, &recip2);
	g_assert_nonnull(rsp2);
	g_free(req);
	g_free(rsp);
	g_free(recip);
	g_free(rsp2);
	g_free(recip2);
	peer_free(&a);
	peer_free(&b);
}

static void test_handshake_modes(void)
{
	PEER a, b;
	char *req, *rsp = NULL, *recip = NULL, *error = NULL;
	E2E_JSON *rule;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);

	/* not enabled on bob's side: ignored, nothing stored */
	req = e2e_build_keyreq(a.kr, &a.id, "#test", BOB, &error);
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_null(rsp);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(b.kr, "peers")), ==, 0);

	/* quiet: ignored as well, the peer is remembered */
	enable(&b, "#test", "quiet");
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_null(rsp);
	g_assert_null(path_get(b.kr, "pending_inbound", ALICE "|#test", NULL));
	g_assert_cmpuint(e2e_json_size(e2e_json_get(b.kr, "peers")), ==, 1);

	/* auto-accept: KEYRSP and our own KEYREQ back */
	enable(&b, "#test", "auto-accept");
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_assert_nonnull(recip);
	g_assert_nonnull(path_get(b.kr, "outgoing_recipients", "#test|" ALICE, NULL));
	g_assert_true(e2e_peer_accepted(b.kr, ALICE, "#test", a.id.fp_hex));
	g_free(rsp);
	g_free(recip);
	/* no second reciprocal KEYREQ while ours is pending */
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_assert_null(recip);
	g_free(rsp);
	g_free(req);

	/* normal mode, but an autotrust rule matches: answered at once */
	enable(&b, "#other", "normal");
	rule = e2e_json_new(E2E_JSON_OBJECT);
	e2e_json_set_string(rule, "scope", "global");
	e2e_json_set_string(rule, "handle_pattern", "ALICE@127.0.0.*");
	e2e_json_array_add(e2e_json_get(b.kr, "autotrust"), rule);
	req = e2e_build_keyreq(a.kr, &a.id, "#other", BOB, &error);
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_assert_true(e2e_peer_accepted(b.kr, ALICE, "#other", a.id.fp_hex));
	g_free(rsp);
	g_free(recip);
	g_free(req);
	/* the rule removed: what it accepted stays accepted (as /e2e accept) */
	e2e_json_set(b.kr, "autotrust", e2e_json_new(E2E_JSON_ARRAY));
	e2e_json_remove(e2e_json_get(a.kr, "pending"), "#other|" BOB);
	req = e2e_build_keyreq(a.kr, &a.id, "#other", BOB, &error);
	g_hash_table_remove_all(b.stamps);
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_free(rsp);
	g_free(recip);
	g_free(req);
	/* another key under the same handle is not what was accepted */
	g_assert_false(e2e_peer_accepted(b.kr, ALICE, "#other", "00000000000000000000000000000000"));
	peer_free(&a);
	peer_free(&b);
}

static void test_trust_changes(void)
{
	PEER a, b, mallory;
	char *req, *rsp = NULL, *recip = NULL, *error = NULL;
	E2E_JSON *ptc;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&b, "#test", "auto-accept");
	req = e2e_build_keyreq(a.kr, &a.id, "#test", BOB, &error);
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_free(rsp);
	g_free(recip);
	g_free(req);

	/* a new key for the same ident@host: blocked, recorded for reverify */
	peer_init(&mallory, "alice", ALICE, -1);
	req = e2e_build_keyreq(mallory.kr, &mallory.id, "#test", BOB, &error);
	g_hash_table_remove_all(b.stamps);
	deliver_keyreq(&b, &mallory, req, BOB, &rsp, &recip);
	g_assert_null(rsp);
	ptc = e2e_json_get(b.kr, "pending_trust_change");
	g_assert_cmpuint(e2e_json_size(ptc), ==, 1);
	g_assert_cmpstr(e2e_json_get_string(g_ptr_array_index(ptc->array, 0), "change"), ==, "fingerprint_changed");
	g_assert_cmpstr(e2e_json_get_string(g_ptr_array_index(ptc->array, 0), "new_fp"), ==, mallory.id.fp_hex);
	g_free(req);

	/* a revoked peer stays revoked */
	e2e_json_set_string(path_get(b.kr, "peers", a.id.fp_hex, NULL), "status", "revoked");
	req = e2e_build_keyreq(a.kr, &a.id, "#test", BOB, &error);
	g_assert_null(req);	/* still pending on alice's side */
	g_free(error);
	e2e_json_remove(e2e_json_get(a.kr, "pending"), "#test|" BOB);
	req = e2e_build_keyreq(a.kr, &a.id, "#test", BOB, &error);
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_null(rsp);
	/* one record per handle and context: the newest */
	g_assert_cmpuint(e2e_json_size(ptc), ==, 1);
	g_assert_cmpstr(e2e_json_get_string(g_ptr_array_index(ptc->array, 0), "change"), ==, "revoked");
	g_free(req);
	peer_free(&a);
	peer_free(&b);
	peer_free(&mallory);
}

static void unhex(const char *hex, unsigned char *out, gsize len)
{
	g_assert_true(e2e_hex_decode(hex, out, len));
}

/* the native side ("alice", seed 0x41) against messages rpe2e.pl made */
static void test_rpe2e_messages(void)
{
	PEER a;
	E2E_JSON *pend, *peer;
	unsigned char eph[32], key[32], *sk;
	char *s, *rsp = NULL, *recip = NULL, *body;
	E2E_IN_PARAMS in;
	E2E_IN_RESULT r;
	gsize len, i;
	GString *joined;

	peer_init(&a, "alice", ALICE, 0x41);
	enable(&a, "#test", "auto-accept");

	/* rpe2e.pl's KEYRSP to alice's KEYREQ (ephemeral key 0x55...) */
	unhex(vec_alice_eph_sk_hex, eph, 32);
	pend = e2e_json_new(E2E_JSON_OBJECT);
	s = e2e_b64_encode(eph, 32);
	e2e_json_set_string(pend, "eph_sk", s);
	g_free(s);
	e2e_json_set_string(pend, "handle", BOB);
	e2e_json_set_string(pend, "channel", "#test");
	e2e_json_set_int(pend, "created_at", fake_now);
	e2e_json_set(e2e_json_get(a.kr, "pending"), "#test|" BOB, pend);
	g_assert_true(e2e_handle_keyrsp(a.kr, BOB, "bob", vec_keyrsp));
	sk = e2e_b64_decode(e2e_json_get_string(path_get(a.kr, "incoming", BOB "|#test", NULL), "sk"), &len);
	memset(key, 0x11, 32);
	g_assert_cmpmem(sk, len, key, 32);
	g_free(sk);

	/* and rpe2e.pl's messages on #test decrypt */
	memset(&in, 0, sizeof(in));
	in.nick = "bob";
	in.handle = BOB;
	in.target = "#test";
	in.resolve = no_resolve;
	in.keyreq_stamps = a.stamps;
	for (i = 0; i < 3; i++) {
		gsize j;

		joined = g_string_new(NULL);
		for (j = 0; vec_wire[i].wire[j] != NULL; j++) {
			e2e_decrypt_incoming(a.kr, &a.id, &in, vec_wire[i].wire[j], &r);
			g_assert_cmpint(r.action, ==, E2E_IN_PLAIN);
			g_string_append(joined, r.plain);
			e2e_in_result_clear(&r);
		}
		g_assert_cmpstr(joined->str, ==, vec_wire[i].plain);
		g_string_free(joined, TRUE);
	}

	/* rpe2e.pl's REKEY: accepted from the known peer bob */
	g_assert_true(e2e_handle_rekey(a.kr, &a.id, BOB, "bob", vec_rekey));
	sk = e2e_b64_decode(e2e_json_get_string(path_get(a.kr, "incoming", BOB "|#test", NULL), "sk"), &len);
	unhex(vec_rekey_key_hex, key, 32);
	g_assert_cmpmem(sk, len, key, 32);
	g_free(sk);

	/* the same REKEY again is a replay: refused */
	g_assert_false(e2e_handle_rekey(a.kr, &a.id, BOB, "bob", vec_rekey));

	/* rpe2e.pl's KEYREQ (bob, #test): alice answers (auto-accept), and the
	   KEYRSP opens with the ephemeral secret rpe2e.pl kept */
	g_hash_table_remove_all(a.stamps);
	e2e_handle_keyreq(a.kr, &a.id, a.stamps, BOB, "bob", vec_keyreq, ALICE, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_assert_null(recip);	/* bob's session is trusted already */
	body = unframe(rsp);
	{
		E2E_HANDSHAKE *hs = e2e_handshake_parse(body, E2E_HS_KEYRSP);
		unsigned char *bob_eph, shared[32], wrap[32], got[32], *ours;
		const char *info = "RPE2E01-WRAP:#test";

		g_assert_nonnull(hs);
		g_assert_true(e2e_handshake_verify(hs));
		bob_eph = e2e_b64_decode(vec_keyreq_eph_sk_b64, &len);
		g_assert_true(e2e_x25519(shared, bob_eph, hs->eph));
		g_assert_true(e2e_wrap_key(wrap, shared, info, strlen(info)));
		g_assert_true(e2e_xchacha_decrypt(got, hs->wrap_ct, hs->wrap_ctlen,
		                                  (const unsigned char *) info, strlen(info),
		                                  hs->wrap_nonce, wrap));
		ours = e2e_b64_decode(e2e_json_get_string(path_get(a.kr, "outgoing", "#test", NULL), "sk"), &len);
		g_assert_cmpmem(got, 32, ours, len);
		g_free(ours);
		g_free(bob_eph);
		e2e_handshake_free(hs);
	}
	g_free(body);
	g_free(rsp);

	/* a REKEY from someone never seen is ignored */
	peer = path_get(a.kr, "peers", NULL);
	g_hash_table_remove_all(peer->object);
	g_assert_false(e2e_handle_rekey(a.kr, &a.id, BOB, "bob", vec_rekey));
	peer_free(&a);
}

static void test_gate(void)
{
	PEER a;
	E2E_GATE_RESULT g;
	GHashTable *nicks = g_hash_table_new(g_str_hash, g_str_equal);
	E2E_JSON *out;

	peer_init(&a, "alice", ALICE, -1);
	g_hash_table_insert(nicks, (char *) "bob", (char *) BOB);

#define GATE(target, body) e2e_gate_decide(a.kr, TRUE, &a.id, target, body, table_resolve, nicks, &g)
	/* no E2E state: plain text */
	GATE("#test", "hello");
	g_assert_cmpint(g.action, ==, E2E_GATE_PASS);
	e2e_gate_result_clear(&g);
	GATE("bob", "hello");
	g_assert_cmpint(g.action, ==, E2E_GATE_PASS);
	e2e_gate_result_clear(&g);

	enable(&a, "#test", "normal");
	GATE("#test", "hello");
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	g_assert_cmpstr(g.ctx, ==, "#test");
	g_assert_true(g.save_needed);	/* the first key was generated */
	e2e_gate_result_clear(&g);
	GATE("#test", "again");
	g_assert_false(g.save_needed);
	e2e_gate_result_clear(&g);

	/* bot commands go out in clear text, with a warning */
	GATE("#test", ".op me");
	g_assert_cmpint(g.action, ==, E2E_GATE_BYPASS);
	g_assert_nonnull(strstr(g.message, "sent in CLEARTEXT"));
	e2e_gate_result_clear(&g);
	GATE("#test", "...so what");
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	e2e_gate_result_clear(&g);
	GATE("#test", "!!");
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	e2e_gate_result_clear(&g);
	/* on a channel without E2E silently */
	GATE("#plain", "!seen bob");
	g_assert_cmpint(g.action, ==, E2E_GATE_BYPASS);
	g_assert_null(g.message);
	e2e_gate_result_clear(&g);

	/* CTCP: ACTION is encrypted, anything else is not */
	GATE("#test", "\001ACTION waves\001");
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	e2e_gate_result_clear(&g);
	GATE("#test", "\001VERSION\001");
	g_assert_cmpint(g.action, ==, E2E_GATE_PASS);
	e2e_gate_result_clear(&g);

	/* STATUSMSG: @#test is #test */
	GATE("@#test", "ops only");
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	g_assert_cmpstr(g.ctx, ==, "#test");
	e2e_gate_result_clear(&g);
	/* +#test with both readings enabled cannot be decided */
	enable(&a, "+#test", "normal");
	GATE("+#test", "x");
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	g_assert_nonnull(strstr(g.message, "ambiguous target +#test"));
	e2e_gate_result_clear(&g);

	/* several targets in one line with an encrypted one among them */
	GATE("#plain,#test", "x");
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	e2e_gate_result_clear(&g);
	GATE("#plain,#other", "x");
	g_assert_cmpint(g.action, ==, E2E_GATE_PASS);
	e2e_gate_result_clear(&g);

	/* channel names are case-insensitive on IRC: another spelling of an
	   encrypted channel is refused, never sent in clear text */
	GATE("#TEST", "hello");
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	g_assert_nonnull(strstr(g.message, "#test"));
	e2e_gate_result_clear(&g);

	/* an empty line on an encrypted channel is not sent as plain text */
	GATE("#test", "");
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	g_assert_cmpstr(g.message, ==, E2E_REFUSE_ENCRYPT);
	e2e_gate_result_clear(&g);

	/* queries: keyed by the peer's handle */
	enable(&a, "@" BOB, "normal");
	GATE("bob", "private");
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	g_assert_cmpstr(g.ctx, ==, "@" BOB);
	e2e_gate_result_clear(&g);
	/* an old config row by bare nick, handle unknown: refused */
	enable(&a, "carol", "normal");
	GATE("carol", "private");
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	g_assert_cmpstr(g.message, ==, E2E_REFUSE_NO_HANDLE);
	e2e_gate_result_clear(&g);

	/* a keyring that could not be read: nothing but bot commands */
	e2e_gate_decide(a.kr, FALSE, &a.id, "#plain", "hello", table_resolve, nicks, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	g_assert_cmpstr(g.message, ==, E2E_REFUSE_KEYRING);
	e2e_gate_result_clear(&g);
	e2e_gate_decide(a.kr, FALSE, &a.id, "#plain", ".op", table_resolve, nicks, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_BYPASS);
	g_assert_nonnull(g.message);
	e2e_gate_result_clear(&g);

	/* a scheduled rotation: a new key, REKEY to the recipients first */
	{
		E2E_JSON *rec = e2e_json_new(E2E_JSON_OBJECT), *peer = e2e_json_new(E2E_JSON_OBJECT);
		unsigned char seed[32], pk[32], sk[64];
		char *pkb, *fp;
		const char *before;

		memset(seed, 9, sizeof(seed));
		e2e_ed25519_seed_keypair(seed, pk, sk);
		pkb = e2e_b64_encode(pk, 32);
		fp = e2e_fingerprint_hex(pk);
		e2e_json_set_string(peer, "pk", pkb);
		e2e_json_set_string(peer, "last_nick", "bob");
		e2e_json_set_string(peer, "last_handle", BOB);
		e2e_json_set(e2e_json_get(a.kr, "peers"), fp, peer);
		e2e_json_set_string(rec, "channel", "#test");
		e2e_json_set_string(rec, "handle", BOB);
		e2e_json_set_string(rec, "fingerprint", fp);
		e2e_json_set(e2e_json_get(a.kr, "outgoing_recipients"), "#test|" BOB, rec);
		/* REKEYs go to accepted recipients only */
		{
			E2E_JSON *acc = e2e_json_new(E2E_JSON_OBJECT);

			e2e_json_set_string(acc, "fp", fp);
			e2e_json_set(e2e_json_object_member(a.kr, "accepted"), BOB "|#test", acc);
		}
		out = path_get(a.kr, "outgoing", "#test", NULL);
		before = g_strdup(e2e_json_get_string(out, "sk"));
		e2e_json_set_int(out, "pending_rotation", 1);
		GATE("#test", "rotated");
		g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
		g_assert_true(g.save_needed);
		g_assert_cmpuint(g.notices->len, ==, 2);
		g_assert_cmpstr(g_ptr_array_index(g.notices, 0), ==, "bob");
		g_assert_true(g_str_has_prefix(g_ptr_array_index(g.notices, 1), "\001RPEE2E REKEY v=1 c=#test "));
		out = path_get(a.kr, "outgoing", "#test", NULL);
		g_assert_cmpstr(e2e_json_get_string(out, "sk"), !=, before);
		g_assert_false(e2e_json_truthy(e2e_json_get(out, "pending_rotation")));
		e2e_gate_result_clear(&g);
		g_free((char *) before);
		g_free(pkb);
		g_free(fp);
	}
#undef GATE
	g_hash_table_destroy(nicks);
	peer_free(&a);
}

static void test_incoming(void)
{
	PEER a, b;
	E2E_IN_PARAMS in;
	E2E_IN_RESULT r;
	E2E_GATE_RESULT g;
	const char *wire;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&b, "#test", "normal");
	e2e_gate_decide(b.kr, TRUE, &b.id, "#test", "hi", no_resolve, NULL, &g);
	wire = g_ptr_array_index(g.wires, 0);

	memset(&in, 0, sizeof(in));
	in.nick = "bob";
	in.handle = BOB;
	in.target = "#test";
	in.resolve = no_resolve;
	in.keyreq_stamps = a.stamps;

	/* plain text is not ours */
	e2e_decrypt_incoming(a.kr, &a.id, &in, "hello", &r);
	g_assert_cmpint(r.action, ==, E2E_IN_NOT_WIRE);
	e2e_in_result_clear(&r);
	/* no session, E2E off here: hidden, no KEYREQ */
	e2e_decrypt_incoming(a.kr, &a.id, &in, wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_DROP);
	g_assert_null(r.keyreq);
	e2e_in_result_clear(&r);
	/* E2E on: hidden, and a KEYREQ once per 30 s */
	g_hash_table_remove_all(a.stamps);
	enable(&a, "#test", "normal");
	e2e_decrypt_incoming(a.kr, &a.id, &in, wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_DROP);
	g_assert_nonnull(r.keyreq);
	g_assert_true(g_str_has_prefix(r.keyreq, "\001RPEE2E KEYREQ v=1 c=#test "));
	g_assert_true(r.save_needed);
	e2e_in_result_clear(&r);
	e2e_decrypt_incoming(a.kr, &a.id, &in, wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_DROP);
	g_assert_null(r.keyreq);
	e2e_in_result_clear(&r);

	/* too old: dropped before any lookup */
	fake_now += 1000;
	e2e_decrypt_incoming(a.kr, &a.id, &in, wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_DROP);
	g_assert_nonnull(strstr(r.debug, "timestamp skew"));
	e2e_in_result_clear(&r);
	fake_now -= 1000;

	/* our own message relayed back: opened with our outgoing key */
	in.nick = "bob";
	in.handle = BOB;
	in.is_own_line = TRUE;
	e2e_decrypt_incoming(b.kr, &b.id, &in, wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_PLAIN);
	g_assert_cmpstr(r.plain, ==, "hi");
	e2e_in_result_clear(&r);
	e2e_gate_result_clear(&g);

	/* a DM before our own handle is known waits */
	in.is_own_line = FALSE;
	in.target = "bob";
	in.own_handle = NULL;
	enable(&b, "@" ALICE, "normal");
	{
		GHashTable *nicks = g_hash_table_new(g_str_hash, g_str_equal);

		g_hash_table_insert(nicks, (char *) "alice", (char *) ALICE);
		e2e_gate_decide(b.kr, TRUE, &b.id, "alice", "secret", table_resolve, nicks, &g);
		g_hash_table_destroy(nicks);
	}
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	wire = g_ptr_array_index(g.wires, 0);
	e2e_decrypt_incoming(a.kr, &a.id, &in, wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_WAIT_OWN);
	e2e_in_result_clear(&r);
	/* with it, the KEYREQ asks for the "@<own>" direction */
	in.own_handle = ALICE;
	enable(&a, "@" BOB, "normal");
	g_hash_table_remove_all(a.stamps);
	e2e_decrypt_incoming(a.kr, &a.id, &in, wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_DROP);
	g_assert_nonnull(r.keyreq);
	g_assert_true(g_str_has_prefix(r.keyreq, "\001RPEE2E KEYREQ v=1 c=@" ALICE " "));
	e2e_in_result_clear(&r);
	e2e_gate_result_clear(&g);
	peer_free(&a);
	peer_free(&b);
}

static void test_stamps_and_glob(void)
{
	GHashTable *s = e2e_stamps_new();
	int i;

	g_assert_true(e2e_stamp_allow(s, "x", 10));
	g_assert_false(e2e_stamp_allow(s, "x", 10));
	g_assert_true(e2e_stamp_allow(s, "y", 10));
	fake_now += 10;
	g_assert_true(e2e_stamp_allow(s, "x", 10));
	/* expired stamps do not pile up */
	for (i = 0; i < 300; i++) {
		char *k = g_strdup_printf("k%d", i);

		e2e_stamp_allow(s, k, 10);
		g_free(k);
	}
	fake_now += 20;
	g_assert_true(e2e_stamp_allow(s, "last", 10));
	g_assert_cmpuint(g_hash_table_size(s), <=, 2);
	g_hash_table_destroy(s);

	{
		E2E_JSON *kr = e2e_keyring_new(), *p = e2e_json_new(E2E_JSON_OBJECT);

		e2e_json_set_string(p, "pk", "x");
		e2e_json_set(e2e_json_get(kr, "peers"), "00", p);
		g_assert_cmpstr(e2e_find_peer_by_handle(kr, "", NULL), ==, "00");
		g_assert_null(e2e_find_peer_by_handle(kr, "a@b", NULL));
		e2e_json_free(kr);
	}

	g_assert_true(e2e_glob_match("*@*.example.org", "Bob@Host.EXAMPLE.org"));
	g_assert_true(e2e_glob_match("b?b@*", "bob@x"));
	g_assert_false(e2e_glob_match("b?b@*", "bb@x"));
	g_assert_false(e2e_glob_match("*.example.org", "x.example.org.evil"));
	g_assert_true(e2e_glob_match("*", ""));
	g_assert_true(e2e_glob_match("a*b*c*d", "aXXbYYcZZd"));
	g_assert_false(e2e_glob_match("a*b*c*d*e", "aXXbYYcZZd"));
}

static void test_export_import(void)
{
	PEER a, b;
	char *req, *rsp = NULL, *recip = NULL, *error = NULL, *s1, *s2;
	E2E_JSON *doc, *kr;
	E2E_GATE_RESULT g;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&b, "#test", "auto-accept");
	req = e2e_build_keyreq(a.kr, &a.id, "#test", BOB, &error);
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	e2e_gate_decide(b.kr, TRUE, &b.id, "#test", "hi", no_resolve, NULL, &g);
	e2e_gate_result_clear(&g);

	doc = e2e_keyring_export(b.kr);
	g_assert_cmpint(e2e_json_get_int(doc, "version", 0), ==, 1);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(doc, "peers")), ==, 1);
	g_assert_cmpstr(e2e_json_get_string(g_ptr_array_index(e2e_json_get(doc, "peers")->array, 0), "fingerprint"), ==, a.id.fp_hex);
	kr = e2e_keyring_import(doc, &error);
	g_assert_nonnull(kr);
	/* what an export carries comes back the same (pending state is not
	   exported, as in rpe2e.pl) */
	e2e_json_set(b.kr, "pending", e2e_json_new(E2E_JSON_OBJECT));
	e2e_json_set(b.kr, "pending_inbound", e2e_json_new(E2E_JSON_OBJECT));
	/* nor are the decisions of this client (accepted peers, used REKEY
	   nonces): the format is rpe2e.pl's / repartee's */
	e2e_json_remove(b.kr, "accepted");
	e2e_json_remove(b.kr, "seen_rekeys");
	e2e_json_remove(kr, "accepted");
	e2e_json_remove(kr, "seen_rekeys");
	s1 = e2e_keyring_snapshot(b.kr);
	s2 = e2e_keyring_snapshot(kr);
	g_assert_cmpstr(s1, ==, s2);
	g_free(s1);
	g_free(s2);
	e2e_json_free(kr);
	e2e_json_free(doc);

	/* not an export */
	doc = e2e_json_new(E2E_JSON_OBJECT);
	e2e_json_set_int(doc, "peers", 3);
	g_assert_null(e2e_keyring_import(doc, &error));
	g_free(error);
	e2e_json_free(doc);
	g_free(req);
	g_free(rsp);
	g_free(recip);
	peer_free(&a);
	peer_free(&b);
}

/* ---- the findings of the security review ---- */

#define MAL   "mallory@127.0.0.1"
#define THIRD "@bob@b.host"

/* the first wire of a message from's gate makes for target */
static char *gate_wire(PEER *from, const char *target, const char *text)
{
	E2E_GATE_RESULT g;
	char *wire;

	e2e_gate_decide(from->kr, TRUE, &from->id, target, text, no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	wire = g_strdup(g_ptr_array_index(g.wires, 0));
	e2e_gate_result_clear(&g);
	return wire;
}

/* a channel wire from from, received by to */
static void recv_wire(PEER *to, PEER *from, const char *chan, const char *wire, E2E_IN_RESULT *r)
{
	E2E_IN_PARAMS in;

	memset(&in, 0, sizeof(in));
	in.nick = from->nick;
	in.handle = from->handle;
	in.target = chan;
	in.own_handle = to->handle;
	in.resolve = no_resolve;
	in.keyreq_stamps = to->stamps;
	e2e_decrypt_incoming(to->kr, &to->id, &in, wire, r);
}

static gboolean deliver_rekey(PEER *to, PEER *from, const char *ctcp)
{
	char *body = unframe(ctcp);
	gboolean ok = e2e_handle_rekey(to->kr, &to->id, from->handle, from->nick, body);

	g_free(body);
	return ok;
}

static gboolean session_key(PEER *p, const char *handle, const char *ctx, unsigned char key[32])
{
	char *k = g_strconcat(handle, "|", ctx, NULL);
	const char *b64 = e2e_json_get_string(path_get(p->kr, "incoming", k, NULL), "sk");
	unsigned char *raw;
	gsize len = 0;
	gboolean ok;

	g_free(k);
	raw = b64 != NULL ? e2e_b64_decode(b64, &len) : NULL;
	ok = raw != NULL && len == 32;
	if (ok)
		memcpy(key, raw, 32);
	g_free(raw);
	return ok;
}

/* to and from exchange keys for ctx, both in auto-accept mode; to holds
   from's key afterwards */
static void exchange(PEER *to, PEER *from, const char *ctx)
{
	char *req, *rsp = NULL, *recip = NULL, *error = NULL, *pk;

	pk = g_strconcat(ctx, "|", from->handle, NULL);
	e2e_json_remove(e2e_json_get(to->kr, "pending"), pk);
	g_free(pk);
	req = e2e_build_keyreq(to->kr, &to->id, ctx, from->handle, &error);
	g_assert_nonnull(req);
	deliver_keyreq(from, to, req, from->handle, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_assert_true(deliver_keyrsp(to, from, rsp));
	g_free(req);
	g_free(rsp);
	g_free(recip);
}

/* C1: a session we asked for is no permission to give our key away */
static void test_review_c1(void)
{
	PEER a, m;
	E2E_IN_RESULT r;
	char *wire, *rsp = NULL, *recip = NULL, *rsp2 = NULL, *recip2 = NULL, *req, *error = NULL;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&m, "mallory", MAL, -1);
	enable(&a, "#secret", "normal");
	enable(&m, "#secret", "auto-accept");
	g_free(gate_wire(&a, "#secret", "warm-up"));

	/* any wire from mallory: alice asks him for his key */
	wire = gate_wire(&m, "#secret", "x");
	recv_wire(&a, &m, "#secret", wire, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_DROP);
	g_assert_nonnull(r.keyreq);
	deliver_keyreq(&m, &a, r.keyreq, MAL, &rsp, &recip);
	e2e_in_result_clear(&r);
	g_assert_nonnull(rsp);
	g_assert_nonnull(recip);
	/* his answer: alice can read him now */
	g_assert_true(deliver_keyrsp(&a, &m, rsp));
	g_assert_false(e2e_peer_accepted(a.kr, MAL, "#secret", m.id.fp_hex));
	/* his request for alice's key waits for /e2e accept */
	deliver_keyreq(&a, &m, recip, ALICE, &rsp2, &recip2);
	g_assert_null(rsp2);
	g_assert_null(recip2);
	g_assert_nonnull(path_get(a.kr, "pending_inbound", MAL "|#secret", NULL));
	g_assert_cmpstr(e2e_json_get_string(path_get(a.kr, "incoming", MAL "|#secret", NULL), "status"), ==, "trusted");
	g_assert_true(e2e_session_has_key(path_get(a.kr, "incoming", MAL "|#secret", NULL)));
	g_free(rsp);
	g_free(recip);

	/* quiet mode: the same, it is no acceptance either */
	enable(&a, "#secret", "quiet");
	e2e_json_remove(e2e_json_get(a.kr, "pending_inbound"), MAL "|#secret");
	e2e_json_remove(e2e_json_get(m.kr, "pending"), "#secret|" ALICE);
	req = e2e_build_keyreq(m.kr, &m.id, "#secret", ALICE, &error);
	g_assert_nonnull(req);
	deliver_keyreq(&a, &m, req, ALICE, &rsp, &recip);
	g_assert_null(rsp);
	g_assert_nonnull(path_get(a.kr, "pending_inbound", MAL "|#secret", NULL));

	/* /e2e accept mallory: now he gets the key, and later requests too */
	g_assert_cmpint(e2e_accept(a.kr, &a.id, MAL, "#secret", NULL, ALICE, &rsp, &recip), ==,
	                E2E_ACCEPT_SENT);
	g_assert_nonnull(rsp);
	g_assert_true(e2e_peer_accepted(a.kr, MAL, "#secret", m.id.fp_hex));
	g_free(rsp);
	g_free(recip);
	deliver_keyreq(&a, &m, req, ALICE, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_free(rsp);
	g_free(recip);
	g_free(req);
	g_free(wire);
	peer_free(&a);
	peer_free(&m);
}

/* C2: a REKEY replaces a trusted session of that handle and context, it
   never creates one; a DM request names its sender */
static void test_review_c2(void)
{
	static const char *const ctxs[] = { "#secret", THIRD, NULL };
	PEER a, m, b;
	unsigned char key[32], got[32];
	gboolean rot, gen;
	char *req, *rsp = NULL, *recip = NULL, *error = NULL, *rk, *pk;
	int i;

	for (i = 0; ctxs[i] != NULL; i++) {
		peer_init(&a, "alice", ALICE, -1);
		peer_init(&m, "mallory", MAL, -1);
		enable(&a, ctxs[i], "normal");
		g_assert_true(e2e_outgoing_key(a.kr, ctxs[i], key, &rot, &gen));
		req = e2e_build_keyreq(m.kr, &m.id, ctxs[i], ALICE, &error);
		deliver_keyreq(&a, &m, req, ALICE, &rsp, &recip);
		g_assert_null(rsp);
		g_free(req);
		/* his REKEY wrapped to alice's public key */
		memset(key, 0x41, sizeof(key));
		rk = e2e_build_rekey(&m.id, ctxs[i], a.id.pk, key);
		g_assert_false(deliver_rekey(&a, &m, rk));
		g_assert_false(session_key(&a, MAL, ctxs[i], got) && memcmp(got, key, 32) == 0);
		g_free(rk);
		/* so a second request still gets nothing */
		pk = g_strconcat(ctxs[i], "|" ALICE, NULL);
		e2e_json_remove(e2e_json_get(m.kr, "pending"), pk);
		g_free(pk);
		fake_now += E2E_PENDING_KEYREQ_TTL + 1;
		req = e2e_build_keyreq(m.kr, &m.id, ctxs[i], ALICE, &error);
		g_assert_nonnull(req);
		g_hash_table_remove_all(a.stamps);
		deliver_keyreq(&a, &m, req, ALICE, &rsp, &recip);
		g_assert_null(rsp);
		g_free(req);
		peer_free(&a);
		peer_free(&m);
	}

	/* a DM context is "@" + the requester's own handle: a request naming
	   a third party is ignored, nothing is stored */
	peer_init(&a, "alice", ALICE, -1);
	peer_init(&m, "mallory", MAL, -1);
	enable(&a, THIRD, "normal");
	req = e2e_build_keyreq(m.kr, &m.id, THIRD, ALICE, &error);
	deliver_keyreq(&a, &m, req, ALICE, &rsp, &recip);
	g_assert_null(rsp);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "pending_inbound")), ==, 0);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "peers")), ==, 0);
	g_free(req);
	peer_free(&a);
	peer_free(&m);

	/* a real session: the REKEY replaces it, but only while E2E is on */
	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&a, "#x", "auto-accept");
	enable(&b, "#x", "auto-accept");
	exchange(&a, &b, "#x");
	memset(key, 0x42, sizeof(key));
	rk = e2e_build_rekey(&b.id, "#x", a.id.pk, key);
	g_assert_true(deliver_rekey(&a, &b, rk));
	g_assert_true(session_key(&a, BOB, "#x", got));
	g_assert_cmpmem(got, 32, key, 32);
	g_free(rk);
	e2e_json_set_int(path_get(a.kr, "channels", "#x", NULL), "enabled", 0);
	memset(key, 0x43, sizeof(key));
	rk = e2e_build_rekey(&b.id, "#x", a.id.pk, key);
	g_assert_false(deliver_rekey(&a, &b, rk));
	g_assert_true(session_key(&a, BOB, "#x", got));
	g_assert_cmpint(got[0], ==, 0x42);
	g_free(rk);
	/* no session on another channel of the same peer: none is created */
	enable(&a, "#y", "auto-accept");
	rk = e2e_build_rekey(&b.id, "#y", a.id.pk, key);
	g_assert_false(deliver_rekey(&a, &b, rk));
	g_assert_false(session_key(&a, BOB, "#y", got));
	g_free(rk);

	/* the DM direction bob -> alice: "@<alice>", config "@<bob>" */
	enable(&a, "@" BOB, "auto-accept");
	enable(&b, "@" ALICE, "auto-accept");
	exchange(&a, &b, "@" ALICE);
	memset(key, 0x44, sizeof(key));
	rk = e2e_build_rekey(&b.id, "@" ALICE, a.id.pk, key);
	g_assert_true(deliver_rekey(&a, &b, rk));
	g_assert_true(session_key(&a, BOB, "@" ALICE, got));
	g_assert_cmpint(got[0], ==, 0x44);
	g_free(rk);
	peer_free(&a);
	peer_free(&b);
}

/* M7: a REKEY is used once */
static void test_review_m7(void)
{
	PEER a, b;
	unsigned char k1[32], k2[32], got[32];
	char *rk1, *rk2;
	int i;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&a, "#x", "auto-accept");
	enable(&b, "#x", "auto-accept");
	exchange(&a, &b, "#x");
	memset(k1, 1, sizeof(k1));
	memset(k2, 2, sizeof(k2));
	rk1 = e2e_build_rekey(&b.id, "#x", a.id.pk, k1);
	rk2 = e2e_build_rekey(&b.id, "#x", a.id.pk, k2);
	g_assert_true(deliver_rekey(&a, &b, rk1));
	g_assert_true(deliver_rekey(&a, &b, rk2));
	/* the old one again would roll the session back */
	g_assert_false(deliver_rekey(&a, &b, rk1));
	g_assert_false(deliver_rekey(&a, &b, rk2));
	g_assert_true(session_key(&a, BOB, "#x", got));
	g_assert_cmpmem(got, 32, k2, 32);
	g_free(rk1);
	g_free(rk2);
	/* the used nonces are kept, never forgotten: once there are
	   E2E_MAX_SEEN_REKEYS, further REKEYs are refused (the next wire
	   then asks for a key) */
	for (i = 0; i < E2E_MAX_SEEN_REKEYS + 20; i++) {
		char *rk = e2e_build_rekey(&b.id, "#x", a.id.pk, k1);

		fake_now++;
		g_assert_true(deliver_rekey(&a, &b, rk) == (i < E2E_MAX_SEEN_REKEYS - 2));
		g_free(rk);
	}
	{
		E2E_JSON *seen = e2e_json_get(a.kr, "seen_rekeys");
		GPtrArray *keys = e2e_json_keys(seen);

		g_assert_cmpuint(keys->len, ==, 1);
		g_assert_cmpuint(e2e_json_size(e2e_json_get(seen, g_ptr_array_index(keys, 0))), ==,
		                 E2E_MAX_SEEN_REKEYS);
		g_ptr_array_unref(keys);
	}
	peer_free(&a);
	peer_free(&b);
}

static char *fixed_resolve(const char *nick, const E2E_JSON *kr, void *data)
{
	return g_strdup(data);
}

/* I3: E2E is on for a nick's old ident@host: refused, not clear text */
static void test_review_i3(void)
{
	E2E_JSON *kr = e2e_keyring_new(), *peer;
	E2E_GATE_RESULT g;

	enable(&(PEER) { .kr = kr }, "@bob@old.host", "normal");
	peer = e2e_json_object_member(e2e_json_get(kr, "peers"), "00");
	e2e_json_set_string(peer, "last_nick", "bob");
	e2e_json_set_string(peer, "last_handle", "bob@old.host");

	e2e_gate_decide(kr, TRUE, NULL, "bob", "my secret", fixed_resolve, (char *) "bob@NEW.host", &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	g_assert_nonnull(strstr(g.message, "bob@old.host"));
	e2e_gate_result_clear(&g);
	/* only the case of the host differs: the context would not match */
	e2e_gate_decide(kr, TRUE, NULL, "BOB", "my secret", fixed_resolve, (char *) "bob@OLD.host", &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	e2e_gate_result_clear(&g);
	/* the address E2E is on for: encrypted */
	e2e_gate_decide(kr, TRUE, NULL, "bob", "my secret", fixed_resolve, (char *) "bob@old.host", &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	e2e_gate_result_clear(&g);
	/* another nick: clear text as before */
	e2e_gate_decide(kr, TRUE, NULL, "carol", "hello", fixed_resolve, (char *) "carol@c.host", &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_PASS);
	e2e_gate_result_clear(&g);
	/* E2E turned off for the old address: clear text */
	e2e_json_set_int(path_get(kr, "channels", "@bob@old.host", NULL), "enabled", 0);
	e2e_gate_decide(kr, TRUE, NULL, "bob", "hello", fixed_resolve, (char *) "bob@NEW.host", &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_PASS);
	e2e_gate_result_clear(&g);
	e2e_json_free(kr);
}

/* I5: a session row without a real key is never trusted, never used */
static void test_review_i5(void)
{
	PEER a, m;
	char *req, *rsp = NULL, *recip = NULL, *error = NULL;
	unsigned char zero[32] = { 0 };
	const char *err = NULL;
	GPtrArray *w;
	E2E_JSON *row;
	E2E_IN_RESULT r;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&m, "mallory", MAL, -1);
	enable(&a, "#c", "normal");
	req = e2e_build_keyreq(m.kr, &m.id, "#c", ALICE, &error);
	deliver_keyreq(&a, &m, req, ALICE, &rsp, &recip);
	g_assert_null(rsp);
	row = path_get(a.kr, "incoming", MAL "|#c", NULL);
	g_assert_nonnull(row);
	g_assert_false(e2e_session_has_key(row));
	g_assert_false(e2e_session_has_key(NULL));
	/* /e2e decline, then /e2e accept: no request, no key - refused */
	e2e_json_remove(e2e_json_get(a.kr, "pending_inbound"), MAL "|#c");
	g_assert_cmpint(e2e_accept(a.kr, &a.id, MAL, "#c", NULL, ALICE, &rsp, &recip), ==,
	                E2E_ACCEPT_NO_KEY);
	g_assert_null(rsp);
	g_assert_cmpstr(e2e_json_get_string(row, "status"), !=, "trusted");
	/* /e2e unrevoke: the same */
	e2e_json_set_string(row, "status", "revoked");
	g_assert_cmpint(e2e_unrevoke(a.kr, MAL, "#c", NULL), ==, 0);
	g_assert_cmpstr(e2e_json_get_string(row, "status"), ==, "revoked");

	/* even marked trusted, an all-zero key decrypts nothing (anyone
	   could encrypt under it), and the peer is asked for a real one */
	e2e_json_set_string(row, "status", "trusted");
	w = e2e_encrypt_plain(zero, "#c", "forged under the zero key", fake_now, &err);
	g_hash_table_remove_all(a.stamps);
	recv_wire(&a, &m, "#c", g_ptr_array_index(w, 0), &r);
	g_assert_cmpint(r.action, ==, E2E_IN_DROP);
	g_assert_nonnull(r.keyreq);
	e2e_in_result_clear(&r);
	g_ptr_array_unref(w);

	/* with a real key both work */
	enable(&m, "#c", "auto-accept");
	exchange(&a, &m, "#c");
	row = path_get(a.kr, "incoming", MAL "|#c", NULL);
	g_assert_true(e2e_session_has_key(row));
	e2e_json_set_string(row, "status", "revoked");
	g_assert_cmpint(e2e_unrevoke(a.kr, MAL, "#c", NULL), ==, 1);
	g_assert_cmpstr(e2e_json_get_string(row, "status"), ==, "trusted");
	e2e_json_set_string(row, "status", "pending");
	g_assert_cmpint(e2e_accept(a.kr, &a.id, MAL, "#c", NULL, ALICE, &rsp, &recip), ==,
	                E2E_ACCEPT_TRUSTED);
	g_assert_cmpstr(e2e_json_get_string(row, "status"), ==, "trusted");
	g_assert_true(e2e_peer_accepted(a.kr, MAL, "#c", m.id.fp_hex));
	g_free(req);
	peer_free(&a);
	peer_free(&m);
}

/* M1: no line breaks or NUL out of a decrypted message */
static void test_review_m1(void)
{
	PEER a, b;
	E2E_WIRE w;
	E2E_IN_RESULT r;
	GByteArray *aad;
	unsigned char key[32];
	static const char pt[] = "one\rtwo\nthree\0four \002bold\002 \00304red";
	char *line;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&a, "#c", "auto-accept");
	enable(&b, "#c", "auto-accept");
	exchange(&a, &b, "#c");
	g_assert_true(e2e_outgoing_key(b.kr, "#c", key, &(gboolean) { 0 }, &(gboolean) { 0 }));

	memset(&w, 0, sizeof(w));
	g_assert_true(e2e_random_bytes(w.msgid, 8));
	g_assert_true(e2e_random_bytes(w.nonce, 24));
	w.ts = fake_now;
	w.part = w.total = 1;
	w.ctlen = sizeof(pt) - 1 + E2E_TAG_LEN;
	w.ct = g_malloc(w.ctlen);
	aad = e2e_aad("#c", w.msgid, w.ts, 1, 1);
	g_assert_true(e2e_xchacha_encrypt(w.ct, (const unsigned char *) pt, sizeof(pt) - 1,
	                                  aad->data, aad->len, w.nonce, key));
	g_byte_array_unref(aad);
	line = e2e_wire_encode(&w);
	g_free(w.ct);
	recv_wire(&a, &b, "#c", line, &r);
	g_assert_cmpint(r.action, ==, E2E_IN_PLAIN);
	g_assert_cmpstr(r.plain, ==, "one two three four \002bold\002 \00304red");
	e2e_in_result_clear(&r);
	g_free(line);
	peer_free(&a);
	peer_free(&b);
}

/* M6: a KEYRSP answers our request to that handle, and only one that
   opens counts */
static void test_review_m6(void)
{
	PEER a, b, m;
	unsigned char eph[32], eph_sk[32];
	char *req, *junk, *rsp = NULL, *recip = NULL, *error = NULL;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	peer_init(&m, "mallory", MAL, -1);
	enable(&a, "#t", "normal");
	enable(&b, "#t", "auto-accept");
	enable(&m, "#t", "auto-accept");
	req = e2e_build_keyreq(a.kr, &a.id, "#t", BOB, &error);
	/* signed by bob, but wrapped for another request */
	g_assert_true(e2e_x25519_keypair(eph_sk, eph));
	junk = e2e_build_keyrsp_for_req(b.kr, &b.id, "#t", ALICE, a.id.pk, eph);
	g_assert_false(deliver_keyrsp(&a, &b, junk));
	g_assert_nonnull(path_get(a.kr, "pending", "#t|" BOB, NULL));
	/* bob's real answer still works */
	deliver_keyreq(&b, &a, req, BOB, &rsp, &recip);
	g_assert_true(deliver_keyrsp(&a, &b, rsp));
	g_free(rsp);
	g_free(recip);
	g_free(junk);
	g_free(req);

	/* a request sent without a known handle (an old keyring): nobody may
	   answer it */
	req = e2e_build_keyreq(a.kr, &a.id, "#t", NULL, &error);
	g_assert_nonnull(path_get(a.kr, "pending", "#t", NULL));
	deliver_keyreq(&m, &a, req, MAL, &rsp, &recip);
	g_assert_nonnull(rsp);
	g_assert_false(deliver_keyrsp(&a, &m, rsp));
	g_assert_null(path_get(a.kr, "incoming", MAL "|#t", NULL));
	g_free(rsp);
	g_free(recip);
	g_free(req);
	peer_free(&a);
	peer_free(&b);
	peer_free(&m);
}

/* M8: what strangers can make us store is bounded, the handshakes are
   rate-limited */
static void test_review_m8(void)
{
	PEER a;
	GHashTable *stamps = e2e_stamps_new();
	int i;

	peer_init(&a, "alice", ALICE, -1);
	enable(&a, "#c", "normal");
	for (i = 0; i < E2E_MAX_UNACCEPTED_PEERS + 40; i++) {
		E2E_JSON *kr = e2e_keyring_new();
		E2E_IDENTITY id;
		gboolean created;
		char *handle, *req, *rsp = NULL, *recip = NULL, *error = NULL, *body;

		g_assert_true(e2e_identity_get(kr, &id, TRUE, &created, &error));
		handle = g_strdup_printf("u%d@h%d", i, i);
		req = e2e_build_keyreq(kr, &id, "#c", ALICE, &error);
		body = unframe(req);
		fake_now++;
		e2e_handle_keyreq(a.kr, &a.id, a.stamps, handle, "u", body, ALICE, &rsp, &recip);
		g_assert_null(rsp);
		g_free(body);
		g_free(req);
		g_free(handle);
		e2e_identity_wipe(&id);
		e2e_json_free(kr);
	}
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "pending_inbound")), <=, E2E_MAX_PENDING_INBOUND);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "incoming")), <=, E2E_MAX_PENDING_INBOUND);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "peers")), <=, E2E_MAX_UNACCEPTED_PEERS);
	/* the newest is kept, the oldest went */
	{
		char *k = g_strdup_printf("u%d@h%d|#c", E2E_MAX_UNACCEPTED_PEERS + 39, E2E_MAX_UNACCEPTED_PEERS + 39);

		g_assert_nonnull(path_get(a.kr, "pending_inbound", k, NULL));
		g_free(k);
		g_assert_null(path_get(a.kr, "pending_inbound", "u0@h0|#c", NULL));
	}

	/* KEYRSP and REKEY like KEYREQ: once per 10 s per sender and context */
	g_assert_true(e2e_handshake_allow(stamps, E2E_HS_KEYRSP, "x@y", "#c"));
	g_assert_false(e2e_handshake_allow(stamps, E2E_HS_KEYRSP, "x@y", "#c"));
	g_assert_true(e2e_handshake_allow(stamps, E2E_HS_KEYRSP, "x@y", "#d"));
	g_assert_true(e2e_handshake_allow(stamps, E2E_HS_REKEY, "x@y", "#c"));
	g_assert_false(e2e_handshake_allow(stamps, E2E_HS_REKEY, "x@y", "#c"));
	g_assert_true(e2e_handshake_allow(stamps, E2E_HS_KEYREQ, "x@y", "#c"));
	g_assert_false(e2e_handshake_allow(stamps, E2E_HS_KEYREQ, "x@y", "#c"));
	fake_now += E2E_KEYREQ_INBOUND_MIN_INTERVAL;
	g_assert_true(e2e_handshake_allow(stamps, E2E_HS_KEYRSP, "x@y", "#c"));
	g_hash_table_destroy(stamps);
	peer_free(&a);
}

/* M10: timestamps from an imported keyring, rotations, long targets */
static void test_review_m10(void)
{
	PEER a, b;
	E2E_GATE_RESULT g;
	E2E_JSON *row, *out;
	unsigned char before[32], key[32], eph[32], eph_sk[32];
	char *req, *rsp, *error = NULL, *chan, *sk, *xs, *ys;
	gboolean rot, gen;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&a, "#t", "normal");
	/* a pending request with an absurd created_at does not block a new
	   one (and does not overflow) */
	req = e2e_build_keyreq(a.kr, &a.id, "#t", BOB, &error);
	g_free(req);
	row = path_get(a.kr, "pending", "#t|" BOB, NULL);
	e2e_json_set_int(row, "created_at", G_MININT64);
	req = e2e_build_keyreq(a.kr, &a.id, "#t", BOB, &error);
	g_assert_nonnull(req);
	g_free(req);
	row = path_get(a.kr, "pending", "#t|" BOB, NULL);
	e2e_json_set_int(row, "created_at", G_MAXINT64);
	req = e2e_build_keyreq(a.kr, &a.id, "#t", BOB, &error);
	g_assert_nonnull(req);
	g_free(req);

	/* a KEYRSP does not consume a scheduled rotation (the REKEYs to the
	   other recipients would be missed): it hands out the current key,
	   the rotation stays due */
	g_assert_true(e2e_outgoing_key(a.kr, "#t", before, &rot, &gen));
	out = path_get(a.kr, "outgoing", "#t", NULL);
	e2e_json_set_int(out, "pending_rotation", 1);
	g_assert_true(e2e_x25519_keypair(eph_sk, eph));
	rsp = e2e_build_keyrsp_for_req(a.kr, &a.id, "#t", BOB, b.id.pk, eph);
	g_assert_nonnull(rsp);
	g_free(rsp);
	out = path_get(a.kr, "outgoing", "#t", NULL);
	g_assert_true(e2e_json_truthy(e2e_json_get(out, "pending_rotation")));
	sk = e2e_b64_encode(before, 32);
	g_assert_cmpstr(e2e_json_get_string(out, "sk"), ==, sk);
	g_free(sk);
	/* the next message rotates */
	e2e_gate_decide(a.kr, TRUE, &a.id, "#t", "x", no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	g_assert_true(e2e_outgoing_key(a.kr, "#t", key, &rot, &gen));
	g_assert_false(memcmp(key, before, 32) == 0);
	g_assert_true(e2e_gate_lines_fit(&g, "", "#t", 510));
	e2e_gate_result_clear(&g);

	/* a target so long the line would not fit: refused, not cut */
	xs = g_strnfill(200, 'x');
	ys = g_strnfill(180, 'y');
	chan = g_strconcat("#", xs, NULL);
	enable(&a, chan, "normal");
	e2e_gate_decide(a.kr, TRUE, &a.id, chan, ys, no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	g_assert_false(e2e_gate_lines_fit(&g, "", chan, 510));
	g_assert_false(e2e_gate_lines_fit(&g, "@label=x ", "#t", 300));
	e2e_gate_result_clear(&g);
	g_free(chan);
	g_free(xs);
	g_free(ys);
	peer_free(&a);
	peer_free(&b);
}

/* I2: one outgoing buffer, several IRC lines */
static void test_review_i2(void)
{
	static const char nul_buf[] = "PRIVMSG #pub :a\0PRIVMSG #secret :b";
	E2E_JSON *kr = e2e_keyring_new();
	char *msg;

	enable(&(PEER) { .kr = kr }, "#secret", "normal");
#define MULTI(text) e2e_gate_multiline(kr, TRUE, text, strlen(text), no_resolve, NULL)
	msg = MULTI("PRIVMSG #pub :hi\r\nPRIVMSG #secret :clear text secret");
	g_assert_nonnull(msg);
	g_free(msg);
	msg = MULTI("NICK me\r\nPRIVMSG #secret :clear text secret");
	g_assert_nonnull(msg);
	g_free(msg);
	msg = MULTI("PRIVMSG #pub :hi\rPRIVMSG #secret :CR only");
	g_assert_nonnull(msg);
	g_free(msg);
	msg = MULTI("privmsg #pub :hi\nprivmsg #SECRET :other case");
	g_assert_nonnull(msg);
	g_free(msg);
	msg = e2e_gate_multiline(kr, TRUE, nul_buf, sizeof(nul_buf) - 1, no_resolve, NULL);
	g_assert_nonnull(msg);
	g_free(msg);
	/* no encrypted conversation in it: it goes as it is */
	g_assert_null(MULTI("PRIVMSG #pub :a\r\nPRIVMSG #pub :b"));
	g_assert_null(MULTI("NICK a\r\nJOIN #b"));
	/* an unreadable keyring: no PRIVMSG at all */
	msg = e2e_gate_multiline(kr, FALSE, "PRIVMSG #pub :a\r\nPRIVMSG #pub :b",
	                         strlen("PRIVMSG #pub :a\r\nPRIVMSG #pub :b"), no_resolve, NULL);
	g_assert_nonnull(msg);
	g_free(msg);
#undef MULTI
	e2e_json_free(kr);
}

/* ---- second round of the review ---- */

#define CAROL "carol@127.0.0.1"

/* I3: E2E on for a nick whose address is not known at all now */
static void test_review2_i3_unresolved(void)
{
	E2E_JSON *kr = e2e_keyring_new(), *cfg, *peer;
	E2E_GATE_RESULT g;

	enable(&(PEER) { .kr = kr }, "@carol@c.host", "normal");
	cfg = path_get(kr, "channels", "@carol@c.host", NULL);
	e2e_json_set_string(cfg, "nick", "carol");
	/* the query was closed and carol left the common channels */
	e2e_gate_decide(kr, TRUE, NULL, "carol", "unresolved secret", no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	g_assert_nonnull(strstr(g.message, "carol@c.host"));
	e2e_gate_result_clear(&g);
	/* known through a peer record only */
	enable(&(PEER) { .kr = kr }, "@dave@d.host", "normal");
	peer = e2e_json_object_member(e2e_json_get(kr, "peers"), "01");
	e2e_json_set_string(peer, "last_nick", "dave");
	e2e_json_set_string(peer, "last_handle", "dave@d.host");
	e2e_gate_decide(kr, TRUE, NULL, "DAVE", "unresolved secret", no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_REFUSE);
	e2e_gate_result_clear(&g);
	/* anybody else: clear text as before */
	e2e_gate_decide(kr, TRUE, NULL, "erin", "hello", no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_PASS);
	e2e_gate_result_clear(&g);
	e2e_json_free(kr);
}

/* M7: replaying an old REKEY after many newer ones */
static void test_review2_rekey_replay(void)
{
	PEER a, b;
	unsigned char k1[32], k[32], got[32];
	char *rk1;
	int i;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&a, "#x", "auto-accept");
	enable(&b, "#x", "auto-accept");
	exchange(&a, &b, "#x");
	/* an all-zero key is no key: refused, the session stays */
	memset(k, 0, sizeof(k));
	rk1 = e2e_build_rekey(&b.id, "#x", a.id.pk, k);
	g_assert_false(deliver_rekey(&a, &b, rk1));
	g_assert_true(e2e_session_has_key(path_get(a.kr, "incoming", BOB "|#x", NULL)));
	g_free(rk1);
	memset(k1, 1, sizeof(k1));
	rk1 = e2e_build_rekey(&b.id, "#x", a.id.pk, k1);
	g_assert_true(deliver_rekey(&a, &b, rk1));
	/* as many newer ones as are remembered, and more */
	for (i = 0; i < E2E_MAX_SEEN_REKEYS + 5; i++) {
		char *rk;

		memset(k, 0x5a, sizeof(k));
		k[0] = i & 0xff;
		k[1] = i >> 8;
		fake_now++;
		rk = e2e_build_rekey(&b.id, "#x", a.id.pk, k);
		/* rk1 and the zero-key REKEY used two of them */
		g_assert_true(deliver_rekey(&a, &b, rk) == (i < E2E_MAX_SEEN_REKEYS - 2));
		g_free(rk);
	}
	/* rolling the session back to k1 */
	g_assert_false(deliver_rekey(&a, &b, rk1));
	g_assert_true(session_key(&a, BOB, "#x", got));
	g_assert_cmpint(got[0], ==, (E2E_MAX_SEEN_REKEYS - 3) & 0xff);
	g_assert_cmpint(got[1], ==, (E2E_MAX_SEEN_REKEYS - 3) >> 8);
	g_free(rk1);
	peer_free(&a);
	peer_free(&b);
}

/* M8: wires from many handles, half of them answering our KEYREQ */
static void test_review2_growth(void)
{
	PEER a, b;
	E2E_IN_RESULT r;
	unsigned char key[32];
	char *req, *rsp = NULL, *recip = NULL, *error = NULL, *last_fp = NULL;
	int i, n = 2 * E2E_MAX_UNACCEPTED_PEERS + 100;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	enable(&a, "#g", "normal");
	enable(&b, "#g", "auto-accept");
	/* bob: alice holds his key and accepted him */
	exchange(&a, &b, "#g");
	e2e_json_remove(e2e_json_get(b.kr, "pending"), "#g|" ALICE);
	req = e2e_build_keyreq(b.kr, &b.id, "#g", ALICE, &error);
	deliver_keyreq(&a, &b, req, ALICE, &rsp, &recip);
	g_assert_null(rsp);
	g_assert_cmpint(e2e_accept(a.kr, &a.id, BOB, "#g", NULL, ALICE, &rsp, &recip), ==,
	                E2E_ACCEPT_SENT);
	g_free(req);
	g_free(rsp);
	g_free(recip);

	for (i = 0; i < n; i++) {
		PEER m;
		char *handle = g_strdup_printf("u%d@h%d", i, i), *wire;

		peer_init(&m, "m", handle, -1);
		enable(&m, "#g", "auto-accept");
		wire = gate_wire(&m, "#g", "x");
		fake_now++;
		recv_wire(&a, &m, "#g", wire, &r);
		g_assert_nonnull(r.keyreq);
		if (i % 2 == 0) {
			deliver_keyreq(&m, &a, r.keyreq, handle, &rsp, &recip);
			g_assert_true(deliver_keyrsp(&a, &m, rsp));
			g_free(last_fp);
			last_fp = g_strdup(m.id.fp_hex);
			g_free(rsp);
			g_free(recip);
		}
		e2e_in_result_clear(&r);
		g_free(wire);
		peer_free(&m);
		g_free(handle);
	}
	/* our own requests nobody answered, and keys of people never accepted */
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "pending")), <=, E2E_MAX_PENDING_OUT);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "peers")), <=, E2E_MAX_UNACCEPTED_PEERS + 1);
	g_assert_cmpuint(e2e_json_size(e2e_json_get(a.kr, "incoming")), <=,
	                 E2E_MAX_UNACCEPTED_PEERS + 1 + E2E_MAX_PENDING_INBOUND);
	/* the accepted peer and its session stay */
	g_assert_nonnull(path_get(a.kr, "peers", b.id.fp_hex, NULL));
	g_assert_true(session_key(&a, BOB, "#g", key));
	g_assert_true(e2e_peer_accepted(a.kr, BOB, "#g", b.id.fp_hex));
	/* /e2e list tells the two apart */
	g_assert_true(e2e_session_accepted(a.kr, BOB, "#g", b.id.fp_hex));
	req = g_strdup_printf("u%d@h%d", n - 2, n - 2);
	g_assert_false(e2e_session_accepted(a.kr, req, "#g", last_fp));
	g_free(req);
	g_free(last_fp);
	peer_free(&a);
	peer_free(&b);
}

/* REKEYs go only to recipients that were accepted */
static void test_review2_rekey_recipients(void)
{
	PEER a, b, c;
	E2E_GATE_RESULT g;
	unsigned char eph[32], eph_sk[32], key[32];
	gboolean rot, gen;
	char *req, *rsp = NULL, *recip = NULL, *error = NULL, *legacy;
	guint i;

	peer_init(&a, "alice", ALICE, -1);
	peer_init(&b, "bob", BOB, -1);
	peer_init(&c, "carol", CAROL, -1);
	enable(&a, "#t", "normal");
	enable(&b, "#t", "auto-accept");
	g_assert_true(e2e_outgoing_key(a.kr, "#t", key, &rot, &gen));
	/* bob asked, alice accepted */
	req = e2e_build_keyreq(b.kr, &b.id, "#t", ALICE, &error);
	deliver_keyreq(&a, &b, req, ALICE, &rsp, &recip);
	g_assert_cmpint(e2e_accept(a.kr, &a.id, BOB, "#t", NULL, ALICE, &rsp, &recip), ==,
	                E2E_ACCEPT_SENT);
	g_free(req);
	g_free(rsp);
	g_free(recip);
	/* carol got the key without acceptance (a keyring from rpe2e.pl) */
	g_assert_true(e2e_x25519_keypair(eph_sk, eph));
	legacy = e2e_build_keyrsp_for_req(a.kr, &a.id, "#t", CAROL, c.id.pk, eph);
	g_free(legacy);
	e2e_json_set_string(path_get(a.kr, "peers", c.id.fp_hex, NULL), "last_nick", "carol");
	g_assert_nonnull(path_get(a.kr, "outgoing_recipients", "#t|" CAROL, NULL));

	e2e_json_set_int(path_get(a.kr, "outgoing", "#t", NULL), "pending_rotation", 1);
	e2e_gate_decide(a.kr, TRUE, &a.id, "#t", "rotated", no_resolve, NULL, &g);
	g_assert_cmpint(g.action, ==, E2E_GATE_CIPHER);
	g_assert_nonnull(g.notices);
	g_assert_cmpuint(g.notices->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(g.notices, 0), ==, "bob");
	/* carol is told about, and no longer a recipient */
	g_assert_nonnull(g.warnings);
	for (i = 0; i < g.warnings->len && strstr(g_ptr_array_index(g.warnings, i), "carol") == NULL; i++)
		;
	g_assert_cmpuint(i, <, g.warnings->len);
	g_assert_null(path_get(a.kr, "outgoing_recipients", "#t|" CAROL, NULL));
	e2e_gate_result_clear(&g);
	peer_free(&a);
	peer_free(&b);
	peer_free(&c);
}

/* /e2e reset drops queued PRIVMSGs: every line of a queued buffer counts */
static void test_review2_buffer_privmsg(void)
{
	static const char nul_buf[] = "NICK a\0PRIVMSG #s :b";

#define HAS(text) e2e_buffer_has_privmsg(text, strlen(text))
	g_assert_true(HAS("NICK x\r\nPRIVMSG #secret :queued"));
	g_assert_true(HAS("PING x\rprivmsg #c :d"));
	g_assert_true(HAS("PRIVMSG #a :b\r\n"));
	g_assert_true(e2e_buffer_has_privmsg(nul_buf, sizeof(nul_buf) - 1));
	g_assert_false(HAS("NICK a\r\nJOIN #b\r\n"));
#undef HAS
}

/* /e2e off in a query turns off the address of that query only */
static void test_review2_query_off(void)
{
	E2E_JSON *kr = e2e_keyring_new();
	GPtrArray *t;
	PEER p = { .kr = kr };

	enable(&p, "@bob@new.host", "normal");
	e2e_json_set_string(path_get(kr, "channels", "@bob@new.host", NULL), "nick", "bob");
	enable(&p, "@bob@old.host", "normal");
	e2e_json_set_string(path_get(kr, "channels", "@bob@old.host", NULL), "nick", "bob");
	t = e2e_query_off_targets(kr, "bob", "bob@new.host");
	g_assert_cmpuint(t->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(t, 0), ==, "@bob@new.host");
	g_ptr_array_unref(t);
	/* address unknown and two configs for the nick: none chosen */
	t = e2e_query_off_targets(kr, "bob", NULL);
	g_assert_cmpuint(t->len, ==, 0);
	g_ptr_array_unref(t);
	/* address unknown, one config for the nick: that one */
	enable(&p, "@carol@c.host", "normal");
	e2e_json_set_string(path_get(kr, "channels", "@carol@c.host", NULL), "nick", "carol");
	t = e2e_query_off_targets(kr, "carol", NULL);
	g_assert_cmpuint(t->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(t, 0), ==, "@carol@c.host");
	g_ptr_array_unref(t);
	e2e_json_free(kr);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	e2e_now = fixed_now;

	g_test_add_func("/e2e/proto/identity", test_identity);
	g_test_add_func("/e2e/proto/handshake-normal", test_handshake_normal_mode);
	g_test_add_func("/e2e/proto/handshake-modes", test_handshake_modes);
	g_test_add_func("/e2e/proto/trust-changes", test_trust_changes);
	g_test_add_func("/e2e/proto/rpe2e-messages", test_rpe2e_messages);
	g_test_add_func("/e2e/proto/gate", test_gate);
	g_test_add_func("/e2e/proto/incoming", test_incoming);
	g_test_add_func("/e2e/proto/stamps-and-glob", test_stamps_and_glob);
	g_test_add_func("/e2e/proto/export-import", test_export_import);
	g_test_add_func("/e2e/review/c1-keyrsp-is-no-acceptance", test_review_c1);
	g_test_add_func("/e2e/review/c2-rekey-needs-session", test_review_c2);
	g_test_add_func("/e2e/review/i2-multiline-buffer", test_review_i2);
	g_test_add_func("/e2e/review/i3-dm-handle-changed", test_review_i3);
	g_test_add_func("/e2e/review/i5-no-key-no-trust", test_review_i5);
	g_test_add_func("/e2e/review/m1-control-characters", test_review_m1);
	g_test_add_func("/e2e/review/m6-keyrsp-binding", test_review_m6);
	g_test_add_func("/e2e/review/m7-rekey-replay", test_review_m7);
	g_test_add_func("/e2e/review/m8-limits", test_review_m8);
	g_test_add_func("/e2e/review/m10-robustness", test_review_m10);
	g_test_add_func("/e2e/review2/i3-unresolved-nick", test_review2_i3_unresolved);
	g_test_add_func("/e2e/review2/m7-rekey-replay-after-many", test_review2_rekey_replay);
	g_test_add_func("/e2e/review2/m8-growth-from-wires", test_review2_growth);
	g_test_add_func("/e2e/review2/rekey-to-accepted-only", test_review2_rekey_recipients);
	g_test_add_func("/e2e/review2/queued-buffer-privmsg", test_review2_buffer_privmsg);
	g_test_add_func("/e2e/review2/query-off-this-address", test_review2_query_off);

	return g_test_run();
}
