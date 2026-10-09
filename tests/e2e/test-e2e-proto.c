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
	{
		E2E_JSON *pend = path_get(b.kr, "pending_inbound", ALICE "|#test", NULL);
		unsigned char *pub, *eph;
		gsize len;

		pub = e2e_b64_decode(e2e_json_get_string(pend, "pubkey"), &len);
		eph = e2e_b64_decode(e2e_json_get_string(pend, "eph_x25519"), &len);
		rsp = e2e_build_keyrsp_for_req(b.kr, &b.id, "#test", ALICE, pub, eph);
		g_free(pub);
		g_free(eph);
		e2e_json_remove(e2e_json_get(b.kr, "pending_inbound"), ALICE "|#test");
		recip = e2e_build_reciprocal_keyreq_on_accept(b.kr, &b.id, "#test", ALICE, BOB);
	}
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

	/* bob's reciprocal KEYREQ: alice holds a trusted session from bob
	   already, so even in normal mode she answers at once (and asks for
	   nothing back) */
	deliver_keyreq(&a, &b, recip, ALICE, &rsp2, &recip2);
	g_assert_nonnull(rsp2);
	g_assert_null(recip2);
	g_assert_null(path_get(a.kr, "pending_inbound", BOB "|#test", NULL));
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
	g_free(rsp);
	g_free(recip);
	g_free(req);
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

	/* rpe2e.pl's KEYREQ (bob, #test): alice answers, and the KEYRSP opens
	   with the ephemeral secret rpe2e.pl kept */
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

	return g_test_run();
}
