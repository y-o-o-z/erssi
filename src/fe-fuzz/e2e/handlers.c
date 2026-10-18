/*
 handlers.c : fuzz the key exchange and the decryption on one keyring

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 What an IRC peer drives: e2e_handle_keyreq, e2e_handle_keyrsp,
 e2e_handle_rekey and e2e_decrypt_incoming, with the pruning and the
 limits under them, and what the user answers with (accept, decline,
 reverify), all against ONE keyring that holds contexts, sessions and
 waiting rows. Random bytes rarely make a valid signature, so the input
 is a script: each line is one step, and most steps make their handshake
 or message with a real key of one of four fixed peers, so the fuzzer
 reaches the state behind the signature checks; raw steps hand the
 handlers the rest of the line as it is.

   q P H C    peer P sends a KEYREQ for context C from handle H
   a P H C    we ask peer P (at handle H) for its key of C, it answers
   r P H C    peer P sends a REKEY for C (a new key) from handle H
   m P H C T  peer P sends text T to C (its own key) from handle H
   M P H C T  the same message again (a replay)
   A H C      /e2e accept H in C;  D H C  /e2e decline H in C
   V H        /e2e reverify H
   K H body   a raw KEYREQ body from H; R H body (KEYRSP); X H body (REKEY)
   W H C wire a raw wire for C from H
   T n        the clock moves n seconds

 P is 0-3; H 0-5 picks one of a few handles (two peers can share one, a
 peer can come back at another: the trust changes); C 0-4 a channel,
 with E2E on in normal, auto-accept and quiet mode, or off, or a DM.
 After each input the keyring must re-encode to itself (the JSON layer)
 and stay within the documented limits.
*/

#include <irssi/src/fe-fuzz/e2e/e2e-fuzz.h>

#include <irssi/src/e2e/e2e-crypto.h>
#include <irssi/src/e2e/e2e-keyring.h>
#include <irssi/src/e2e/e2e-proto.h>
#include <irssi/src/e2e/e2e-wire.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PEERS 4
#define OWN "me@fuzz.host"

static const char *const handles[] = {
	"p0@a.host", "p1@b.host", "p2@c.host", "p3@d.host", "p0@new.host", "x@evil.host"
};
static const char *const nicks[] = { "p0", "p1", "p2", "p3", "p0", "x" };
static const char *const ctxs[] = { "#normal", "#auto", "#quiet", "#off", "@me@fuzz.host" };

typedef struct {
	E2E_JSON *kr;
	E2E_IDENTITY id;
} PEER;

static E2E_JSON *seed;		/* our keyring as each input starts */
static E2E_IDENTITY own;
static PEER peer_seed[PEERS];
static gint64 clock_now;

static gint64 fuzz_now(void)
{
	return clock_now;
}

static void set_identity(E2E_JSON *kr, int seed_byte, E2E_IDENTITY *id)
{
	unsigned char s[32], pk[32], sk[64];
	E2E_JSON *ident = e2e_json_new(E2E_JSON_OBJECT);
	gboolean created;
	char *b, *error = NULL;

	memset(s, seed_byte, sizeof(s));
	if (!e2e_ed25519_seed_keypair(s, pk, sk))
		abort();
	b = e2e_b64_encode(pk, 32);
	e2e_json_set_string(ident, "pk", b);
	g_free(b);
	b = e2e_b64_encode(sk, 64);
	e2e_json_set_string(ident, "sk", b);
	g_free(b);
	b = e2e_fingerprint_hex(pk);
	e2e_json_set_string(ident, "fp", b);
	g_free(b);
	e2e_json_set_int(ident, "created_at", 1);
	e2e_json_set(kr, "identity", ident);
	if (!e2e_identity_get(kr, id, FALSE, &created, &error))
		abort();
}

static void enable(E2E_JSON *kr, const char *ctx, const char *mode, int on)
{
	E2E_JSON *cfg = e2e_json_new(E2E_JSON_OBJECT);

	e2e_json_set_int(cfg, "enabled", on);
	e2e_json_set_string(cfg, "mode", mode);
	e2e_json_set(e2e_json_get(kr, "channels"), ctx, cfg);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	int i, h;

	(void) argc;
	(void) argv;
	clock_now = 1791564129;
	e2e_now = fuzz_now;
	seed = e2e_keyring_new();
	set_identity(seed, 0x41, &own);
	enable(seed, "#normal", "normal", 1);
	enable(seed, "#auto", "auto-accept", 1);
	enable(seed, "#quiet", "quiet", 1);
	enable(seed, "#off", "normal", 0);
	/* the DMs of every handle on */
	for (h = 0; h < (int) G_N_ELEMENTS(handles); h++) {
		char *dm = g_strconcat("@", handles[h], NULL);

		enable(seed, dm, "normal", 1);
		g_free(dm);
	}
	for (i = 0; i < PEERS; i++) {
		peer_seed[i].kr = e2e_keyring_new();
		set_identity(peer_seed[i].kr, 0x50 + i, &peer_seed[i].id);
		/* the peers answer everyone, in every context */
		enable(peer_seed[i].kr, "#normal", "auto-accept", 1);
		enable(peer_seed[i].kr, "#auto", "auto-accept", 1);
		enable(peer_seed[i].kr, "#quiet", "auto-accept", 1);
		enable(peer_seed[i].kr, "#off", "auto-accept", 1);
		enable(peer_seed[i].kr, "@" OWN, "auto-accept", 1);
	}
	return 0;
}

static char *no_resolve(const char *nick, const E2E_JSON *kr, void *data)
{
	(void) nick;
	(void) kr;
	(void) data;
	return NULL;
}

static char *unframe(const char *ctcp)
{
	size_t len = strlen(ctcp);

	return len >= 2 ? g_strndup(ctcp + 1, len - 2) : g_strdup(ctcp);
}

/* a digit field of the line, its value clamped to n choices */
static int pick(char **f, int i, int n)
{
	if ((int) g_strv_length(f) <= i || !g_ascii_isdigit(*f[i]))
		return 0;
	return (*f[i] - '0') % n;
}

static void decrypt(E2E_JSON *kr, E2E_REPLAY *replay, GHashTable *stamps, int h, const char *ctx,
                    const char *wire)
{
	E2E_IN_PARAMS in;
	E2E_IN_RESULT res;
	char *text;

	memset(&in, 0, sizeof(in));
	in.nick = nicks[h];
	in.handle = handles[h];
	/* a DM to us arrives under the sender's nick */
	in.target = *ctx == '@' ? nicks[h] : ctx;
	in.own_handle = OWN;
	in.resolve = no_resolve;
	in.keyreq_stamps = stamps;
	in.replay = replay;
	e2e_decrypt_incoming(kr, &own, &in, wire, &res);
	if (res.action == E2E_IN_DROP) {
		text = e2e_in_placeholder(kr, &res, in.nick, in.handle);
		if (text == NULL || strchr(text, '\n') != NULL || !g_utf8_validate(text, -1, NULL))
			abort();
		g_free(text);
	}
	if (res.action == E2E_IN_PLAIN && !g_utf8_validate(res.plain, -1, NULL))
		abort();
	e2e_in_result_clear(&res);
}

static void step(E2E_JSON *kr, PEER *peers, E2E_REPLAY *replay, GHashTable *stamps, char *line)
{
	char **f = g_strsplit(line, " ", 5);
	int p = pick(f, 1, PEERS), h = pick(f, 2, G_N_ELEMENTS(handles));
	const char *ctx = ctxs[pick(f, 3, G_N_ELEMENTS(ctxs))];
	char *rsp = NULL, *recip = NULL, *body, *req, *error = NULL;
	E2E_REVERIFY rv;

	switch (f[0] != NULL ? f[0][0] : 0) {
	case 'q': {
		/* a DM request names its sender's own address */
		char *c = *ctx == '@' ? g_strconcat("@", handles[h], NULL) : g_strdup(ctx);

		e2e_json_remove(e2e_json_get(peers[p].kr, "pending"), c);
		req = e2e_build_keyreq(peers[p].kr, &peers[p].id, c, NULL, &error);
		if (req != NULL) {
			body = unframe(req);
			e2e_handle_keyreq(kr, &own, stamps, NULL, handles[h], nicks[h], body, OWN, &rsp,
			                  &recip);
			g_free(body);
		}
		g_free(req);
		g_free(c);
		break;
	}
	case 'a':
		req = e2e_build_keyreq(kr, &own, ctx, handles[h], &error);
		if (req != NULL) {
			GHashTable *theirs = e2e_stamps_new();

			body = unframe(req);
			e2e_handle_keyreq(peers[p].kr, &peers[p].id, theirs, NULL, OWN, "me", body,
			                  handles[h], &rsp, &recip);
			g_hash_table_destroy(theirs);
			g_free(body);
			if (rsp != NULL) {
				body = unframe(rsp);
				e2e_handle_keyrsp(kr, NULL, handles[h], nicks[h], body);
				g_free(body);
			}
		}
		g_free(req);
		break;
	case 'r': {
		unsigned char key[32];

		memset(key, 0x60 + p + h, sizeof(key));
		req = e2e_build_rekey(&peers[p].id, ctx, own.pk, key);
		if (req != NULL) {
			body = unframe(req);
			e2e_handle_rekey(kr, &own, NULL, handles[h], nicks[h], body);
			g_free(body);
		}
		g_free(req);
		break;
	}
	case 'm':
	case 'M': {
		E2E_GATE_RESULT g;
		guint i, n = f[0][0] == 'M' ? 2 : 1;

		e2e_gate_decide(peers[p].kr, TRUE, &peers[p].id, NULL, ctx,
		                g_strv_length(f) > 4 ? f[4] : "x",
		                no_resolve, NULL, &g);
		for (i = 0; g.action == E2E_GATE_CIPHER && i < g.wires->len * n; i++)
			decrypt(kr, replay, stamps, h, ctx, g_ptr_array_index(g.wires, i % g.wires->len));
		e2e_gate_result_clear(&g);
		break;
	}
	case 'A':
		/* A H C: the fields shift by one */
		h = pick(f, 1, G_N_ELEMENTS(handles));
		ctx = ctxs[pick(f, 2, G_N_ELEMENTS(ctxs))];
		e2e_accept(kr, &own, handles[h], ctx, *ctx == '@' ? ctx : NULL, OWN, &rsp, &recip);
		break;
	case 'D':
		h = pick(f, 1, G_N_ELEMENTS(handles));
		ctx = ctxs[pick(f, 2, G_N_ELEMENTS(ctxs))];
		e2e_stop_giving_key(kr, handles[h], ctx);
		break;
	case 'V':
		h = pick(f, 1, G_N_ELEMENTS(handles));
		e2e_reverify(kr, handles[h], NULL, NULL, &rv);
		e2e_reverify_clear(&rv);
		break;
	case 'K':
	case 'R':
	case 'X':
		h = pick(f, 1, G_N_ELEMENTS(handles));
		body = g_strv_length(f) > 2 ? g_strjoinv(" ", f + 2) : g_strdup("");
		if (f[0][0] == 'K')
			e2e_handle_keyreq(kr, &own, stamps, NULL, handles[h], nicks[h], body, OWN, &rsp,
			                  &recip);
		else if (f[0][0] == 'R')
			e2e_handle_keyrsp(kr, NULL, handles[h], nicks[h], body);
		else
			e2e_handle_rekey(kr, &own, NULL, handles[h], nicks[h], body);
		g_free(body);
		break;
	case 'W':
		h = pick(f, 1, G_N_ELEMENTS(handles));
		ctx = ctxs[pick(f, 2, G_N_ELEMENTS(ctxs))];
		body = g_strv_length(f) > 3 ? g_strjoinv(" ", f + 3) : g_strdup("");
		decrypt(kr, replay, stamps, h, ctx, body);
		g_free(body);
		break;
	case 'T':
		if (f[1] != NULL)
			clock_now += g_ascii_strtoll(f[1], NULL, 10) % 100000;
		break;
	default:
		break;
	}
	g_free(rsp);
	g_free(recip);
	g_free(error);
	g_strfreev(f);
}

/* the limits of what anyone can make us store (e2e-proto.h) */
static void check_limits(const E2E_JSON *kr)
{
	E2E_JSON *seen = e2e_json_get(kr, "seen_rekeys");
	GPtrArray *keys;
	guint i;

	if (e2e_json_size(e2e_json_get(kr, "pending_inbound")) > E2E_MAX_PENDING_INBOUND_ALL ||
	    e2e_json_size(e2e_json_get(kr, "pending")) > E2E_MAX_PENDING_OUT ||
	    e2e_json_size(e2e_json_get(kr, "pending_trust_change")) > E2E_MAX_TRUST_CHANGES)
		abort();
	keys = e2e_json_keys(seen);
	for (i = 0; i < keys->len; i++)
		if (e2e_json_size(e2e_json_get(seen, g_ptr_array_index(keys, i))) > E2E_MAX_SEEN_REKEYS)
			abort();
	g_ptr_array_unref(keys);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	E2E_JSON *kr, *again;
	PEER peers[PEERS];
	E2E_REPLAY *replay;
	GHashTable *stamps;
	char *text, **lines, *enc, *enc2;
	const char *error = NULL;
	int i, n;

	text = g_strndup((const char *) data, size);
	lines = g_strsplit(text, "\n", 64);
	clock_now = 1791564129;
	kr = e2e_json_copy(seed);
	for (i = 0; i < PEERS; i++) {
		peers[i].kr = e2e_json_copy(peer_seed[i].kr);
		peers[i].id = peer_seed[i].id;
	}
	replay = e2e_replay_new(16);
	stamps = e2e_stamps_new();
	for (n = 0; lines[n] != NULL; n++)
		step(kr, peers, replay, stamps, lines[n]);
	check_limits(kr);
	/* what the handlers stored is a keyring that reads back as it is */
	enc = e2e_keyring_snapshot(kr);
	again = e2e_json_parse(enc, strlen(enc), &error);
	if (again == NULL)
		abort();
	enc2 = e2e_keyring_snapshot(again);
	if (strcmp(enc, enc2) != 0)
		abort();
	g_free(enc);
	g_free(enc2);
	e2e_json_free(again);
	g_hash_table_destroy(stamps);
	e2e_replay_free(replay);
	for (i = 0; i < PEERS; i++)
		e2e_json_free(peers[i].kr);
	e2e_json_free(kr);
	g_strfreev(lines);
	g_free(text);
	return 0;
}
