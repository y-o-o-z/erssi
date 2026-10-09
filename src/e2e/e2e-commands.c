/*
 e2e-commands.c : /E2E

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The subcommands, arguments and messages of rpe2e.pl 0.2.2 (cmd_on,
 cmd_accept, ... there). They work in the context of the active window:
 a channel, or - in a query - the peer by ident@host.
*/

#include "module.h"
#include "e2e-core.h"
#include "e2e-crypto.h"
#include "e2e-keyring.h"
#include "e2e-wire.h"

#include <irssi/src/core/commands.h>
#include <irssi/src/core/queries.h>
#include <irssi/src/core/window-item-def.h>
#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>

#include <errno.h>

#define NOT_IN_CONTEXT "not in a channel or query (or peer handle unknown — wait for a message from them)"

static const char *const subcommands[] = {
	"on", "off", "mode", "fingerprint", "list", "status", "accept", "decline", "revoke",
	"unrevoke", "forget", "handshake", "verify", "reverify", "rotate", "export", "import",
	"autotrust", "reset", "help", NULL
};

/* $witem->{name}: the channel, or the nick of a query */
static const char *item_name(WI_ITEM_REC *item)
{
	return item != NULL && item->name != NULL ? item->name : "";
}

/* _resolve_ctx_for_command: the channel, or "@<peer handle>" in a query */
static char *resolve_ctx(const E2E_JSON *kr, WI_ITEM_REC *item, const char *nick)
{
	char *handle, *ctx;

	if (item == NULL)
		return NULL;
	if (e2e_is_channel(item_name(item)))
		return g_strdup(item_name(item));
	handle = e2e_resolve_dm_handle(item->server, kr, nick != NULL ? nick : item_name(item));
	if (handle == NULL)
		return NULL;
	ctx = g_strconcat("@", handle, NULL);
	g_free(handle);
	return ctx;
}

/* a nick through the keyring, an ident@host as it is */
static char *resolve_handle(const E2E_JSON *kr, const char *who)
{
	if (strchr(who, '@') != NULL)
		return g_strdup(who);
	return g_strdup(e2e_find_handle_by_nick(kr, who));
}

/* the "@<own>" context a DM trust change must touch as well; NULL (and the
   refusal printed) while our handle is unknown - guessing would miss the
   real session or touch another network's */
static char *own_ctx_or_warn(WI_ITEM_REC *item, const char *ctx, const char *cmd, gboolean *ok)
{
	char *own;

	*ok = TRUE;
	if (*ctx != '@')
		return g_strdup(ctx);
	own = e2e_incoming_ctx_for(item != NULL ? item->server : NULL, ctx);
	if (own == NULL) {
		e2e_print_item(item, "%s: own identity not learned yet (waiting for the WHOIS reply) — try again in a moment",
		               cmd);
		*ok = FALSE;
	}
	return own;
}

static E2E_JSON *incoming_row(E2E_JSON *kr, const char *handle, const char *ctx)
{
	char *k = g_strconcat(handle, "|", ctx, NULL);
	E2E_JSON *row = e2e_json_get(e2e_json_get(kr, "incoming"), k);

	g_free(k);
	return row;
}

/* exactly the two context forms of this server, never handle-wide */
static void set_incoming_trust(E2E_JSON *kr, const char *handle, const char *ctx,
                               const char *own_ctx, const char *status)
{
	E2E_JSON *row = incoming_row(kr, handle, ctx);

	if (row != NULL)
		e2e_json_set_string(row, "status", status);
	if (own_ctx != NULL && strcmp(own_ctx, ctx) != 0) {
		row = incoming_row(kr, handle, own_ctx);
		if (row != NULL)
			e2e_json_set_string(row, "status", status);
	}
}

static int delete_rows(E2E_JSON *kr, const char *handle, const char *ctx, const char *own_ctx)
{
	static const char *const stores[] = { "incoming", "pending_inbound", NULL };
	int i, n = 0;
	char *k;

	for (i = 0; stores[i] != NULL; i++) {
		E2E_JSON *store = e2e_json_get(kr, stores[i]);

		k = g_strconcat(handle, "|", ctx, NULL);
		n += e2e_json_remove(store, k);
		g_free(k);
		if (own_ctx != NULL && strcmp(own_ctx, ctx) != 0) {
			k = g_strconcat(handle, "|", own_ctx, NULL);
			n += e2e_json_remove(store, k);
			g_free(k);
		}
	}
	return n;
}

/* keys "<handle>|..." or "...|<handle>" of a store */
static int delete_handle_keys(E2E_JSON *store, const char *handle)
{
	GPtrArray *keys = e2e_json_keys(store);
	GPtrArray *doomed = g_ptr_array_new_with_free_func(g_free);
	char *head = g_strconcat(handle, "|", NULL), *tail = g_strconcat("|", handle, NULL);
	guint i;
	int n;

	for (i = 0; i < keys->len; i++) {
		const char *k = g_ptr_array_index(keys, i);

		if (g_str_has_prefix(k, head) || g_str_has_suffix(k, tail))
			g_ptr_array_add(doomed, g_strdup(k));
	}
	g_ptr_array_unref(keys);
	for (i = 0; i < doomed->len; i++)
		e2e_json_remove(store, g_ptr_array_index(doomed, i));
	n = doomed->len;
	g_ptr_array_unref(doomed);
	g_free(head);
	g_free(tail);
	return n;
}

static void filter_trust_changes(E2E_JSON *kr, const char *handle, GPtrArray *taken)
{
	E2E_JSON *list = e2e_json_get(kr, "pending_trust_change");
	guint i;

	for (i = 0; i < list->array->len;) {
		E2E_JSON *row = g_ptr_array_index(list->array, i);

		if (g_strcmp0(e2e_json_get_string(row, "handle"), handle) == 0) {
			if (taken != NULL)
				g_ptr_array_add(taken, e2e_json_copy(row));
			g_ptr_array_remove_index(list->array, i);
		} else {
			i++;
		}
	}
}

/* nick: the query's nick for a "@<handle>" context (the gate refuses a
   DM to that nick at another ident@host), NULL for a channel */
static void set_channel(E2E_JSON *kr, const char *ctx, int enabled, const char *mode,
                        const char *nick)
{
	E2E_JSON *cfg = e2e_json_new(E2E_JSON_OBJECT);

	e2e_json_set_int(cfg, "enabled", enabled);
	e2e_json_set_string(cfg, "mode", mode);
	if (nick != NULL && *ctx == '@')
		e2e_json_set_string(cfg, "nick", nick);
	e2e_json_set(e2e_json_get(kr, "channels"), ctx, cfg);
}

static const char *query_nick(WI_ITEM_REC *item)
{
	return item != NULL && !e2e_is_channel(item_name(item)) ? item_name(item) : NULL;
}

static guint e2e_dm_configs_of_count(const E2E_JSON *kr, const char *nick)
{
	GPtrArray *on = e2e_dm_configs_of(kr, nick);
	guint n = on->len;

	g_ptr_array_unref(on);
	return n;
}

#define LOST_REFUSAL "the keyring is lost (a corrupt one was moved aside) — /e2e reset starts a new one; until then nothing is sent and nothing saved"

static const char *str_or(const char *s, const char *def)
{
	return s != NULL ? s : def;
}

/* SYNTAX: E2E [HELP] */
/* SYNTAX: E2E ON */
static void cmd_on(WI_ITEM_REC *item)
{
	E2E_JSON *kr = e2e_load();
	char *ctx = resolve_ctx(kr, item, NULL);

	if (ctx == NULL) {
		e2e_print_item(item, "%s", NOT_IN_CONTEXT);
	} else {
		set_channel(kr, ctx, 1, "normal", query_nick(item));
		e2e_save(kr);
		e2e_print_item(item, "enabled on %s (mode=normal)", ctx);
	}
	g_free(ctx);
	e2e_json_free(kr);
}

/* ctx off, its mode and query nick kept */
static void disable_ctx(E2E_JSON *kr, const char *ctx, const char *nick)
{
	E2E_JSON *cfg = e2e_json_get(e2e_json_get(kr, "channels"), ctx);
	char *mode = g_strdup(str_or(e2e_json_get_string(cfg, "mode"), "normal"));
	char *cnick = g_strdup(nick != NULL ? nick : e2e_json_get_string(cfg, "nick"));

	set_channel(kr, ctx, 0, mode, cnick);
	g_free(mode);
	g_free(cnick);
}

/* what is still on for the nick of a query */
static void print_still_on(WI_ITEM_REC *item, E2E_JSON *kr, const char *nick)
{
	GPtrArray *on = e2e_dm_configs_of(kr, nick);
	guint i;

	for (i = 0; i < on->len; i++)
		e2e_print_item(item, "E2E is still on for %s at %s — a message to %s is refused while their address is another; /e2e off %s turns it off",
		               nick, (char *) g_ptr_array_index(on, i), nick,
		               (char *) g_ptr_array_index(on, i));
	g_ptr_array_unref(on);
}

/* SYNTAX: E2E OFF [<ident@host>|<channel>] */
static void cmd_off(WI_ITEM_REC *item, const char *who)
{
	E2E_JSON *kr = e2e_load();
	const char *nick = query_nick(item);
	char *ctx = NULL, *handle;
	GPtrArray *targets;
	guint i;

	if (who != NULL) {
		/* an address of a query (or a channel), from any window */
		ctx = *who == '@' || e2e_is_channel(who) ? g_strdup(who) : g_strconcat("@", who, NULL);
		if (*ctx == '@' && strchr(ctx + 1, '@') == NULL)
			e2e_print_item(item, "usage: /e2e off [<ident@host>|<channel>]");
		else if (!e2e_ctx_enabled(kr, ctx))
			e2e_print_item(item, "E2E is not on for %s", ctx);
		else {
			disable_ctx(kr, ctx, NULL);
			e2e_save(kr);
			e2e_print_item(item, "disabled on %s", ctx);
		}
	} else if (nick == NULL) {
		ctx = resolve_ctx(kr, item, NULL);
		if (ctx == NULL) {
			e2e_print_item(item, "%s", NOT_IN_CONTEXT);
		} else {
			disable_ctx(kr, ctx, NULL);
			e2e_save(kr);
			e2e_print_item(item, "disabled on %s", ctx);
		}
	} else {
		/* a query: its address only (the live one, else the one config
		   of this query); other addresses of the nick are named */
		handle = e2e_resolve_dm_handle(item->server, kr, nick);
		targets = e2e_query_off_targets(kr, nick, handle);
		g_free(handle);
		for (i = 0; i < targets->len; i++) {
			disable_ctx(kr, g_ptr_array_index(targets, i), nick);
			e2e_print_item(item, "disabled on %s", (char *) g_ptr_array_index(targets, i));
		}
		if (targets->len > 0)
			e2e_save(kr);
		else if (e2e_dm_configs_of_count(kr, nick) == 0)
			e2e_print_item(item, "%s", NOT_IN_CONTEXT);
		else
			e2e_print_item(item, "the address of %s is not known now — nothing turned off", nick);
		print_still_on(item, kr, nick);
		g_ptr_array_unref(targets);
	}
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E MODE [normal|auto-accept|auto|quiet] */
static void cmd_mode(WI_ITEM_REC *item, const char *mode)
{
	E2E_JSON *kr;
	char *ctx;

	if (mode == NULL)
		mode = "normal";
	if (strcmp(mode, "auto") == 0)
		mode = "auto-accept";
	if (strcmp(mode, "auto-accept") != 0 && strcmp(mode, "normal") != 0 &&
	    strcmp(mode, "quiet") != 0) {
		e2e_print_item(item, "invalid mode: %s", mode);
		return;
	}
	kr = e2e_load();
	ctx = resolve_ctx(kr, item, NULL);
	if (ctx == NULL) {
		e2e_print_item(item, "%s", NOT_IN_CONTEXT);
	} else {
		set_channel(kr, ctx, 1, mode, query_nick(item));
		e2e_save(kr);
		e2e_print_item(item, "mode=%s on %s", mode, ctx);
	}
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E FINGERPRINT */
static void cmd_fingerprint(WI_ITEM_REC *item)
{
	E2E_JSON *kr = e2e_load();
	E2E_IDENTITY id;

	if (e2e_ensure_identity(kr, &id)) {
		e2e_print_item(item, "Fingerprint (mine):");
		e2e_print_item(item, "  hex  %s", id.fp_hex);
		e2e_identity_wipe(&id);
	}
	e2e_json_free(kr);
}

static guint enabled_count(const E2E_JSON *kr)
{
	E2E_JSON *channels = e2e_json_get(kr, "channels");
	GPtrArray *keys = e2e_json_keys(channels);
	guint i, n = 0;

	for (i = 0; i < keys->len; i++)
		n += e2e_ctx_enabled(kr, g_ptr_array_index(keys, i));
	g_ptr_array_unref(keys);
	return n;
}

/* SYNTAX: E2E STATUS */
static void cmd_status(WI_ITEM_REC *item)
{
	E2E_JSON *kr = e2e_load();
	const char *fp = e2e_json_get_string(e2e_json_get(kr, "identity"), "fp");

	e2e_print_item(item, "identity=%s peers=%u enabled_channels=%u", str_or(fp, "(none)"),
	               e2e_json_size(e2e_json_get(kr, "incoming")), enabled_count(kr));
	if (e2e_keyring_is_lost())
		e2e_print_item(item, "%s", LOST_REFUSAL);
	e2e_json_free(kr);
}

static void split_key(const char *key, char **a, char **b)
{
	const char *bar = strchr(key, '|');

	*a = bar != NULL ? g_strndup(key, bar - key) : g_strdup(key);
	*b = bar != NULL ? g_strdup(bar + 1) : NULL;
}

/* "trusted" means we read them; whether they get our key is "accepted" */
static const char *session_label(const E2E_JSON *kr, const char *handle, const char *ctx,
                                 const E2E_JSON *row)
{
	const char *status = str_or(e2e_json_get_string(row, "status"), "pending");

	if (strcmp(status, "trusted") == 0 && ctx != NULL &&
	    !e2e_session_accepted(kr, handle, ctx, e2e_json_get_string(row, "fp")))
		return "key held, not accepted";
	return status;
}

/* SYNTAX: E2E LIST [-all] */
static void cmd_list(WI_ITEM_REC *item, char **args)
{
	E2E_JSON *kr = e2e_load(), *src;
	GPtrArray *keys;
	gboolean all = FALSE, any = FALSE;
	char *ctx, *handle, *channel;
	guint i;

	for (i = 0; args[i] != NULL; i++)
		if (strcmp(args[i], "-all") == 0)
			all = TRUE;
	if (all) {
		e2e_print_item(item, "Keyring (all)");
		src = e2e_json_get(kr, "peers");
		keys = e2e_json_keys(src);
		if (keys->len > 0)
			e2e_print_item(item, "Peers");
		for (i = 0; i < keys->len; i++) {
			const char *fp = g_ptr_array_index(keys, i);
			E2E_JSON *p = e2e_json_get(src, fp);

			const char *status = str_or(e2e_json_get_string(p, "status"), "pending");

			if (strcmp(status, "trusted") == 0 && !e2e_fingerprint_accepted(kr, fp))
				status = "key held, not accepted";
			e2e_print_item(item, "  %s  [%s]  nick=%s fp=%.16s",
			               str_or(e2e_json_get_string(p, "last_handle"), "—"), status,
			               str_or(e2e_json_get_string(p, "last_nick"), "—"), fp);
			any = TRUE;
		}
		g_ptr_array_unref(keys);
		src = e2e_json_get(kr, "incoming");
		keys = e2e_json_keys(src);
		if (keys->len > 0)
			e2e_print_item(item, "Incoming Sessions");
		for (i = 0; i < keys->len; i++) {
			E2E_JSON *row = e2e_json_get(src, g_ptr_array_index(keys, i));

			split_key(g_ptr_array_index(keys, i), &handle, &channel);
			e2e_print_item(item, "  %s  %s  [%s]  fp=%.16s", handle, str_or(channel, ""),
			               session_label(kr, handle, channel, row),
			               str_or(e2e_json_get_string(row, "fp"), ""));
			g_free(handle);
			g_free(channel);
			any = TRUE;
		}
		g_ptr_array_unref(keys);
		if (!any)
			e2e_print_item(item, "(no remembered E2E state)");
		e2e_json_free(kr);
		return;
	}
	if (item == NULL) {
		e2e_print_item(NULL, "not in a chat buffer");
		e2e_json_free(kr);
		return;
	}
	ctx = resolve_ctx(kr, item, NULL);
	if (ctx == NULL) {
		e2e_print_item(item, "cannot resolve current context");
		e2e_json_free(kr);
		return;
	}
	/* a query lists everything from this peer (DM sessions live under
	   "@<own>", trust markers under "@<peer>"), a channel its sessions */
	src = e2e_json_get(kr, "incoming");
	keys = e2e_json_keys(src);
	for (i = 0; i < keys->len; i++) {
		E2E_JSON *row = e2e_json_get(src, g_ptr_array_index(keys, i));
		gboolean match;

		split_key(g_ptr_array_index(keys, i), &handle, &channel);
		match = *ctx == '@' ? strcmp(handle, ctx + 1) == 0 : g_strcmp0(channel, ctx) == 0;
		if (match) {
			e2e_print_item(item, "  %s on %s  fp=%.16s  status=%s", handle, str_or(channel, ""),
			               str_or(e2e_json_get_string(row, "fp"), ""),
			               session_label(kr, handle, channel, row));
			any = TRUE;
		}
		g_free(handle);
		g_free(channel);
	}
	g_ptr_array_unref(keys);
	if (!any)
		e2e_print_item(item, "no peers");
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E HANDSHAKE <nick> */
static void cmd_handshake(SERVER_REC *server, WI_ITEM_REC *item, const char *nick)
{
	E2E_JSON *kr;
	E2E_IDENTITY id;
	char *ctx, *kreq_ctx = NULL, *handle = NULL, *wire = NULL, *error = NULL;

	if (nick == NULL) {
		e2e_print_item(item, "usage: /e2e handshake <nick>");
		return;
	}
	if (item == NULL) {
		e2e_print_item(item, "not in a chat buffer");
		return;
	}
	if (!IS_IRC_SERVER(server) || !server->connected) {
		e2e_print_item(item, "not connected to a server");
		return;
	}
	kr = e2e_load();
	ctx = resolve_ctx(kr, item, nick);
	if (ctx == NULL) {
		e2e_print_item(item, "cannot resolve handle for %s", nick);
		goto out;
	}
	if (!e2e_ctx_enabled(kr, ctx)) {
		e2e_print_item(item, "e2e not enabled on %s", ctx);
		goto out;
	}
	/* a KEYREQ asks for the direction WE receive: for a DM it is stamped
	   with our own handle; the peer-keyed ctx is only the config key */
	kreq_ctx = e2e_incoming_ctx_for(server, ctx);
	if (kreq_ctx == NULL) {
		e2e_print_item(item, "own identity not learned yet (waiting for the WHOIS reply) — try again in a moment");
		goto out;
	}
	/* the live ident@host first: only an answer from that handle is taken */
	handle = e2e_resolve_dm_handle(server, kr, nick);
	if (handle == NULL) {
		e2e_print_item(item, "cannot resolve handle for %s — wait until they are in a common channel or send you a message",
		               nick);
		goto out;
	}
	if (!e2e_ensure_identity(kr, &id))
		goto out;
	wire = e2e_build_keyreq(kr, &id, kreq_ctx, handle, &error);
	e2e_identity_wipe(&id);
	if (wire == NULL) {
		e2e_print_item(item, "handshake failed: %s", error);
		goto out;
	}
	e2e_save(kr);
	e2e_send_notice(server, nick, wire);
	e2e_print_item(item, "KEYREQ sent to %s for %s", nick, ctx);
	e2e_print_debug(server, ctx, nick, "TX KEYREQ to %s for %s", nick, ctx);
out:
	g_free(error);
	g_free(wire);
	g_free(handle);
	g_free(kreq_ctx);
	g_free(ctx);
	e2e_json_free(kr);
}

/* the handle and context of /e2e <cmd> <nick>, or NULL with the reason
   printed */
static gboolean nick_context(E2E_JSON *kr, WI_ITEM_REC *item, const char *nick,
                             char **handle, char **ctx)
{
	*ctx = NULL;
	*handle = resolve_handle(kr, nick);
	if (*handle == NULL) {
		e2e_print_item(item, "cannot resolve handle for %s", nick);
		return FALSE;
	}
	*ctx = resolve_ctx(kr, item, nick);
	if (*ctx == NULL) {
		e2e_print_item(item, "cannot resolve context for %s", nick);
		return FALSE;
	}
	return TRUE;
}

/* SYNTAX: E2E ACCEPT <nick> */
static void cmd_accept(SERVER_REC *server, WI_ITEM_REC *item, const char *nick)
{
	E2E_JSON *kr;
	E2E_IDENTITY id;
	char *handle, *ctx, *own = NULL, *rsp = NULL, *recip = NULL;

	if (nick == NULL) {
		e2e_print_item(item, "usage: /e2e accept <nick>");
		return;
	}
	if (!IS_IRC_SERVER(server) || !server->connected) {
		e2e_print_item(item, "not connected to a server");
		return;
	}
	kr = e2e_load();
	if (!nick_context(kr, item, nick, &handle, &ctx))
		goto out;
	if (!e2e_ensure_identity(kr, &id))
		goto out;
	own = *ctx == '@' ? e2e_incoming_ctx_for(server, ctx) : NULL;
	switch (e2e_accept(kr, &id, handle, ctx, own, e2e_own_handle(server), &rsp, &recip)) {
	case E2E_ACCEPT_SENT:
		e2e_save(kr);
		e2e_send_notice(server, nick, rsp);
		if (recip != NULL)
			e2e_send_notice(server, nick, recip);
		e2e_print_item(item, "accepted %s (%s) on %s — KEYRSP sent", nick, handle, ctx);
		e2e_print_debug(server, ctx, nick, "TX KEYRSP to %s for %s", nick, ctx);
		if (recip != NULL)
			e2e_print_debug(server, ctx, nick, "TX KEYREQ to %s for %s", nick, ctx);
		break;
	case E2E_ACCEPT_TRUSTED:
		e2e_save(kr);
		e2e_print_item(item, "accepted %s (%s) on %s — their next key request is answered", nick,
		               handle, ctx);
		break;
	case E2E_ACCEPT_NO_KEY:
		e2e_print_item(item, "cannot accept %s on %s: no key from them is stored (only a request that is gone) — wait for their next request, or /e2e handshake %s",
		               nick, ctx, nick);
		break;
	case E2E_ACCEPT_DAMAGED:
		e2e_print_item(item, "the pending exchange from %s on %s is damaged — /e2e decline %s removes it",
		               nick, ctx, nick);
		break;
	case E2E_ACCEPT_FAILED:
		e2e_print_item(item, "cannot answer the key exchange from %s on %s (key material damaged?)",
		               nick, ctx);
		break;
	case E2E_ACCEPT_NOTHING:
		e2e_print_item(item, "no pending exchange or session for %s on %s", nick, ctx);
		break;
	}
	e2e_identity_wipe(&id);
out:
	g_free(own);
	g_free(rsp);
	g_free(recip);
	g_free(handle);
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E DECLINE <nick> */
static void cmd_decline(WI_ITEM_REC *item, const char *nick)
{
	E2E_JSON *kr, *row;
	char *handle, *ctx, *k;

	if (nick == NULL) {
		e2e_print_item(item, "usage: /e2e decline <nick>");
		return;
	}
	kr = e2e_load();
	if (nick_context(kr, item, nick, &handle, &ctx)) {
		k = g_strconcat(handle, "|", ctx, NULL);
		e2e_json_remove(e2e_json_get(kr, "pending_inbound"), k);
		g_free(k);
		row = incoming_row(kr, handle, ctx);
		if (row != NULL)
			e2e_json_set_string(row, "status", "revoked");
		e2e_forget_acceptance(kr, handle, ctx);
		e2e_save(kr);
		e2e_print_item(item, "declined %s on %s", nick, ctx);
	}
	g_free(handle);
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E REVOKE <nick> */
/* SYNTAX: E2E UNREVOKE <nick> */
static void cmd_revoke(WI_ITEM_REC *item, const char *nick, gboolean revoke)
{
	E2E_JSON *kr, *out, *peer;
	char *handle, *ctx, *own = NULL, *k;
	gboolean ok;

	if (nick == NULL) {
		e2e_print_item(item, "%s", revoke ? "usage: /e2e revoke <nick>" : "usage: /e2e unrevoke <nick>");
		return;
	}
	kr = e2e_load();
	if (!nick_context(kr, item, nick, &handle, &ctx))
		goto out;
	own = own_ctx_or_warn(item, ctx, revoke ? "/e2e revoke" : "/e2e unrevoke", &ok);
	if (!ok)
		goto out;
	if (!revoke && e2e_unrevoke(kr, handle, ctx, own) == 0) {
		/* trusted without a key would show anything encrypted under
		   the placeholder as genuine */
		e2e_print_item(item, "cannot unrevoke %s on %s: no key from them is stored — wait for their key, or /e2e handshake %s",
		               nick, ctx, nick);
		goto out;
	}
	if (revoke) {
		set_incoming_trust(kr, handle, ctx, own, "revoked");
		e2e_forget_acceptance(kr, handle, ctx);
		/* the peer must not read along any more: drop it from the REKEY
		   list and rotate our key */
		k = g_strconcat(ctx, "|", handle, NULL);
		e2e_json_remove(e2e_json_get(kr, "outgoing_recipients"), k);
		g_free(k);
		out = e2e_json_get(e2e_json_get(kr, "outgoing"), ctx);
		if (out != NULL)
			e2e_json_set_int(out, "pending_rotation", 1);
	}
	e2e_find_peer_by_handle(kr, handle, &peer);
	if (peer != NULL)
		e2e_json_set_string(peer, "status", revoke ? "revoked" : "trusted");
	e2e_save(kr);
	if (revoke)
		e2e_print_item(item, "revoked %s on %s — key will rotate", nick, ctx);
	else
		e2e_print_item(item, "unrevoked %s on %s — you read them again; they get your key after /e2e accept %s",
		               nick, ctx, nick);
out:
	g_free(own);
	g_free(handle);
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E FORGET [-all] <nick|ident@host> */
static void cmd_forget(WI_ITEM_REC *item, char **args)
{
	static const char *const stores[] = {
		"incoming", "pending_inbound", "outgoing_recipients", "pending", "accepted", NULL
	};
	E2E_JSON *kr;
	guint n = g_strv_length(args);
	gboolean all = FALSE, ok;
	const char *who, *fp;
	char *handle = NULL, *ctx = NULL, *own = NULL;
	int i, removed = 0;

	if (n == 0) {
		e2e_print_item(item, "usage: /e2e forget [-all] <nick|handle>");
		return;
	}
	/* -all at the start or at the end */
	who = args[0];
	if (n > 1 && strcmp(args[0], "-all") == 0) {
		all = TRUE;
		who = args[1];
	} else if (n > 1 && strcmp(args[n - 1], "-all") == 0) {
		all = TRUE;
	}
	kr = e2e_load();
	handle = resolve_handle(kr, who);
	if (handle == NULL) {
		e2e_print_item(item, "cannot resolve handle for %s", who);
		goto out;
	}
	if (all) {
		fp = e2e_find_peer_by_handle(kr, handle, NULL);
		if (fp != NULL) {
			char *copy = g_strdup(fp);

			removed += e2e_json_remove(e2e_json_get(kr, "peers"), copy);
			delete_handle_keys(e2e_json_get(kr, "seen_rekeys"), copy);
			g_free(copy);
		}
		for (i = 0; stores[i] != NULL; i++)
			removed += delete_handle_keys(e2e_json_get(kr, stores[i]), handle);
		filter_trust_changes(kr, handle, NULL);
		e2e_save(kr);
		e2e_print_item(item, "forgot %s (%s) globally — removed %d row(s)", who, handle, removed);
		goto out;
	}
	ctx = resolve_ctx(kr, item, who);
	if (ctx == NULL) {
		e2e_print_item(item, "cannot resolve context for %s", who);
		goto out;
	}
	own = own_ctx_or_warn(item, ctx, "/e2e forget", &ok);
	if (!ok)
		goto out;
	delete_rows(kr, handle, ctx, own);
	e2e_forget_acceptance(kr, handle, ctx);
	e2e_save(kr);
	e2e_print_item(item, "forgotten %s on %s", who, ctx);
out:
	g_free(own);
	g_free(ctx);
	g_free(handle);
	e2e_json_free(kr);
}

/* SYNTAX: E2E VERIFY <nick> */
static void cmd_verify(WI_ITEM_REC *item, const char *nick)
{
	E2E_JSON *kr, *row;
	E2E_IDENTITY id;
	char *handle, *ctx, *own;

	if (nick == NULL) {
		e2e_print_item(item, "usage: /e2e verify <nick>");
		return;
	}
	kr = e2e_load();
	if (!nick_context(kr, item, nick, &handle, &ctx))
		goto out;
	/* real DM sessions are recipient-keyed under "@<own>"; the peer-keyed
	   trust marker if only that exists */
	own = e2e_incoming_ctx_for(item != NULL ? item->server : NULL, ctx);
	row = own != NULL ? incoming_row(kr, handle, own) : NULL;
	if (row == NULL)
		row = incoming_row(kr, handle, ctx);
	g_free(own);
	if (row == NULL) {
		e2e_print_item(item, "no session for %s on %s", nick, ctx);
		goto out;
	}
	if (!e2e_ensure_identity(kr, &id))
		goto out;
	e2e_print_item(item, "Fingerprint Verification");
	e2e_print_item(item, "  You  ( local): %.16s", id.fp_hex);
	e2e_print_item(item, "  Them (%s): %.16s", nick, str_or(e2e_json_get_string(row, "fp"), ""));
	e2e_identity_wipe(&id);
out:
	g_free(handle);
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E REVERIFY <nick> */
static void cmd_reverify(WI_ITEM_REC *item, const char *nick)
{
	E2E_JSON *kr, *applied = NULL, *peer;
	GPtrArray *taken;
	const char *old_fp;
	char *handle;
	guint i;

	if (nick == NULL) {
		e2e_print_item(item, "usage: /e2e reverify <nick>");
		return;
	}
	kr = e2e_load();
	handle = resolve_handle(kr, nick);
	if (handle == NULL) {
		e2e_print_item(item, "cannot resolve handle for %s", nick);
		e2e_json_free(kr);
		return;
	}
	taken = g_ptr_array_new_with_free_func((GDestroyNotify) e2e_json_free);
	filter_trust_changes(kr, handle, taken);
	/* what was accepted was the old key */
	delete_handle_keys(e2e_json_get(kr, "accepted"), handle);
	for (i = 0; i < taken->len && applied == NULL; i++) {
		E2E_JSON *row = g_ptr_array_index(taken, i);

		if (g_strcmp0(e2e_json_get_string(row, "change"), "fingerprint_changed") == 0 &&
		    e2e_json_get_string(row, "new_pubkey") != NULL &&
		    e2e_json_get_string(row, "new_fp") != NULL)
			applied = row;
	}
	if (applied != NULL) {
		old_fp = e2e_json_get_string(applied, "old_fp");
		if (old_fp != NULL)
			e2e_json_remove(e2e_json_get(kr, "peers"), old_fp);
		delete_handle_keys(e2e_json_get(kr, "incoming"), handle);
		delete_handle_keys(e2e_json_get(kr, "outgoing_recipients"), handle);
		peer = e2e_json_new(E2E_JSON_OBJECT);
		e2e_json_set_string(peer, "pk", e2e_json_get_string(applied, "new_pubkey"));
		e2e_json_set_string(peer, "last_handle", handle);
		e2e_json_set_string(peer, "last_nick", nick);
		e2e_json_set_int(peer, "first_seen", e2e_now());
		e2e_json_set_int(peer, "last_seen", e2e_now());
		e2e_json_set_string(peer, "status", "trusted");
		e2e_json_set(e2e_json_get(kr, "peers"), e2e_json_get_string(applied, "new_fp"), peer);
		e2e_save(kr);
		e2e_print_item(item, "reverified %s: accepted new key fp=%.16s", nick,
		               e2e_json_get_string(applied, "new_fp"));
	} else {
		old_fp = e2e_find_peer_by_handle(kr, handle, NULL);
		if (old_fp != NULL) {
			char *copy = g_strdup(old_fp);

			e2e_json_remove(e2e_json_get(kr, "peers"), copy);
			g_free(copy);
		}
		delete_handle_keys(e2e_json_get(kr, "incoming"), handle);
		delete_handle_keys(e2e_json_get(kr, "outgoing_recipients"), handle);
		e2e_save(kr);
		e2e_print_item(item, "reverified %s: purged stale state; re-handshake to TOFU-pin the new key",
		               nick);
	}
	g_ptr_array_unref(taken);
	g_free(handle);
	e2e_json_free(kr);
}

/* SYNTAX: E2E ROTATE */
static void cmd_rotate(WI_ITEM_REC *item)
{
	E2E_JSON *kr, *out;
	unsigned char key[32];
	char *ctx, *b64;

	if (item == NULL) {
		e2e_print_item(NULL, "not in a chat buffer");
		return;
	}
	kr = e2e_load();
	ctx = resolve_ctx(kr, item, NULL);
	if (ctx == NULL) {
		e2e_print_item(item, "cannot resolve current context");
		e2e_json_free(kr);
		return;
	}
	out = e2e_json_get(e2e_json_get(kr, "outgoing"), ctx);
	if (out != NULL) {
		e2e_json_set_int(out, "pending_rotation", 1);
	} else if (e2e_random_bytes(key, sizeof(key))) {
		out = e2e_json_new(E2E_JSON_OBJECT);
		b64 = e2e_b64_encode(key, sizeof(key));
		e2e_json_set_string(out, "sk", b64);
		e2e_wipe(b64, strlen(b64));
		g_free(b64);
		e2e_wipe(key, sizeof(key));
		e2e_json_set_int(out, "created_at", e2e_now());
		e2e_json_set_int(out, "pending_rotation", 1);
		e2e_json_set(e2e_json_get(kr, "outgoing"), ctx, out);
	}
	e2e_save(kr);
	e2e_print_item(item, "rotation scheduled for %s", ctx);
	g_free(ctx);
	e2e_json_free(kr);
}

/* SYNTAX: E2E EXPORT <file> */
static void cmd_export(WI_ITEM_REC *item, const char *path)
{
	E2E_JSON *kr, *doc;
	char *data, *error = NULL;

	if (path == NULL) {
		e2e_print_item(item, "usage: /e2e export <file>");
		return;
	}
	kr = e2e_load();
	doc = e2e_keyring_export(kr);
	data = e2e_json_encode(doc, TRUE);
	/* private keys: created 0600, never over a file or through a symlink */
	if (e2e_write_new_file(path, data, strlen(data), &error))
		e2e_print_item(item, "exported keyring to %s", path);
	else
		e2e_print_item(item, "export failed: %s: %s%s", path, error,
		               strcmp(error, g_strerror(EEXIST)) == 0 ? " (choose a new file name)" : "");
	e2e_wipe(data, strlen(data));
	g_free(data);
	g_free(error);
	e2e_json_free(doc);
	e2e_json_free(kr);
}

/* SYNTAX: E2E IMPORT <file> */
static void cmd_import(WI_ITEM_REC *item, const char *path)
{
	E2E_JSON *doc, *kr;
	GError *gerror = NULL;
	const char *perr = NULL;
	char *data, *error = NULL, *bak;
	gboolean lost, saved;
	gsize len;

	if (path == NULL) {
		e2e_print_item(item, "usage: /e2e import <file>");
		return;
	}
	if (!g_file_get_contents(path, &data, &len, &gerror)) {
		e2e_print_item(item, "import failed: %s", gerror->message);
		g_error_free(gerror);
		return;
	}
	doc = e2e_json_parse(data, len, &perr);
	e2e_wipe(data, len);
	g_free(data);
	if (doc == NULL) {
		e2e_print_item(item, "import failed: %s", perr);
		return;
	}
	kr = e2e_keyring_import(doc, &error);
	e2e_json_free(doc);
	if (kr == NULL) {
		e2e_print_item(item, "import failed: %s", error);
		g_free(error);
		return;
	}
	/* the whole keyring is replaced: a private copy first. A lost
	   keyring (moved aside) is replaced too: that ends the lost state. */
	lost = e2e_keyring_is_lost();
	bak = e2e_keyring_backup(e2e_keyring_file(), &error);
	if (bak != NULL) {
		if (lost) {
			saved = e2e_keyring_start_fresh(e2e_keyring_file(), kr, &error);
			if (!saved)
				e2e_print_item(item, "%s", error);
			else if (e2e_drop_queued_privmsgs() > 0)
				e2e_print_item(item, "messages typed while the keyring was lost and still waiting to be sent were dropped");
		} else {
			saved = e2e_save(kr);
		}
	} else {
		saved = FALSE;
	}
	if (bak == NULL) {
		e2e_print_item(item, "import failed: cannot back up the current keyring (%s) — nothing changed",
		               error);
	} else if (!saved) {
		if (*bak != '\0')
			e2e_print_item(item, "import failed: keyring not saved (backup: %s)", bak);
		else
			e2e_print_item(item, "import failed: keyring not saved");
	} else if (*bak != '\0') {
		e2e_print_item(item, "imported keyring from %s (previous keyring: %s)", path, bak);
	} else {
		e2e_print_item(item, "imported keyring from %s", path);
	}
	g_free(bak);
	g_free(error);
	e2e_json_free(kr);
}

/* SYNTAX: E2E AUTOTRUST [LIST] */
/* SYNTAX: E2E AUTOTRUST ADD <scope> <pattern> */
/* SYNTAX: E2E AUTOTRUST REMOVE <pattern> */
static void cmd_autotrust(WI_ITEM_REC *item, char **args)
{
	E2E_JSON *kr = e2e_load(), *rules = e2e_json_get(kr, "autotrust"), *rule;
	char *op = g_ascii_strdown(args[0] != NULL ? args[0] : "list", -1);
	guint n = g_strv_length(args), i;

	if (strcmp(op, "list") == 0) {
		if (e2e_json_size(rules) == 0)
			e2e_print_item(item, "(no autotrust rules)");
		for (i = 0; i < e2e_json_size(rules); i++) {
			rule = g_ptr_array_index(rules->array, i);
			e2e_print_item(item, "  %s  %s", str_or(e2e_json_get_string(rule, "scope"), ""),
			               str_or(e2e_json_get_string(rule, "handle_pattern"), ""));
		}
	} else if (strcmp(op, "add") == 0) {
		if (n < 3) {
			e2e_print_item(item, "usage: /e2e autotrust add <scope> <pattern>");
		} else {
			rule = e2e_json_new(E2E_JSON_OBJECT);
			e2e_json_set_string(rule, "scope", args[1]);
			e2e_json_set_string(rule, "handle_pattern", args[2]);
			e2e_json_set_int(rule, "created_at", e2e_now());
			e2e_json_array_add(rules, rule);
			e2e_save(kr);
			e2e_print_item(item, "autotrust add %s %s", args[1], args[2]);
		}
	} else if (strcmp(op, "remove") == 0) {
		if (n < 2) {
			e2e_print_item(item, "usage: /e2e autotrust remove <pattern>");
		} else {
			for (i = 0; i < rules->array->len;) {
				rule = g_ptr_array_index(rules->array, i);
				if (g_strcmp0(str_or(e2e_json_get_string(rule, "handle_pattern"), ""), args[1]) == 0)
					g_ptr_array_remove_index(rules->array, i);
				else
					i++;
			}
			e2e_save(kr);
			e2e_print_item(item, "autotrust removed %s", args[1]);
		}
	} else {
		e2e_print_item(item, "usage: /e2e autotrust <list|add|remove>");
	}
	g_free(op);
	e2e_json_free(kr);
}

/* SYNTAX: E2E RESET */
static void cmd_reset(WI_ITEM_REC *item)
{
	E2E_LOAD_STATUS status;
	E2E_IDENTITY id;
	E2E_JSON *kr;
	gboolean created;
	char *error = NULL;

	if (!e2e_keyring_is_lost()) {
		kr = e2e_keyring_load_checked(e2e_keyring_file(), &status, &error);
		e2e_json_free(kr);
		if (status == E2E_LOAD_OK)
			e2e_print_item(item, "nothing to reset: the keyring %s is fine", e2e_keyring_file());
		else
			e2e_print_item(item, "nothing to reset: the keyring %s exists but cannot be read (%s) — it is never replaced; fix it or move it aside",
			               e2e_keyring_file(), error);
		g_free(error);
		return;
	}
	kr = e2e_keyring_new();
	if (!e2e_identity_get(kr, &id, TRUE, &created, &error)) {
		e2e_print_item(item, "reset failed: %s", error);
	} else {
		if (e2e_keyring_start_fresh(e2e_keyring_file(), kr, &error)) {
			int dropped = e2e_drop_queued_privmsgs();

			e2e_print_item(item, "a new keyring is started with a new identity (fingerprint %s). E2E is off everywhere until /e2e on; peers see a new key and need /e2e reverify <your nick>. The corrupt file stays as %s.corrupt-*",
			               id.fp_hex, e2e_keyring_file());
			if (dropped > 0)
				e2e_print_item(item, "%d message(s) typed while the keyring was lost and still waiting to be sent were dropped",
				               dropped);
		} else {
			e2e_print_item(item, "reset failed: %s", error);
		}
		e2e_identity_wipe(&id);
	}
	g_free(error);
	e2e_json_free(kr);
}

/* "/e2e" alone: how to start, with the state of the current window */
static void cmd_guide(WI_ITEM_REC *item)
{
	E2E_JSON *kr = e2e_load(), *incoming;
	const char *fp = e2e_json_get_string(e2e_json_get(kr, "identity"), "fp");
	char *ctx = resolve_ctx(kr, item, NULL);

	e2e_print_item(item, "End-to-end encryption (RPE2E, compatible with repartee)");
	if (e2e_keyring_is_lost())
		e2e_print_item(item, "%s", LOST_REFUSAL);
	e2e_print_item(item, "Your key fingerprint: %s", str_or(fp, "(none yet - created on first use)"));
	if (ctx != NULL) {
		E2E_JSON *cfg = e2e_json_get(e2e_json_get(kr, "channels"), ctx);
		char *own = e2e_incoming_ctx_for(item->server, ctx);
		const char *peer = *ctx == '@' ? ctx + 1 : NULL;
		GPtrArray *keys, *trusted = g_ptr_array_new_with_free_func(g_free);
		GString *who = g_string_new(NULL);
		guint i, j;

		/* a query's DM sessions live under "@<own>", its trust markers
		   under "@<peer>": both, for this peer only */
		incoming = e2e_json_get(kr, "incoming");
		keys = e2e_json_keys(incoming);
		for (i = 0; i < keys->len; i++) {
			char *h, *c;
			gboolean dup = FALSE;

			split_key(g_ptr_array_index(keys, i), &h, &c);
			if (c != NULL && (strcmp(c, ctx) == 0 || g_strcmp0(c, own) == 0) &&
			    (peer == NULL || strcmp(h, peer) == 0) &&
			    g_strcmp0(e2e_json_get_string(e2e_json_get(incoming, g_ptr_array_index(keys, i)),
			                                  "status"), "trusted") == 0) {
				for (j = 0; j < trusted->len && !dup; j++)
					dup = strcmp(g_ptr_array_index(trusted, j), h) == 0;
				if (!dup) {
					g_ptr_array_add(trusted, h);
					h = NULL;
				}
			}
			g_free(h);
			g_free(c);
		}
		g_ptr_array_unref(keys);
		for (i = 0; i < trusted->len; i++)
			g_string_append_printf(who, "%s%s", i > 0 ? ", " : "",
			                       (char *) g_ptr_array_index(trusted, i));
		if (e2e_json_truthy(e2e_json_get(cfg, "enabled")))
			e2e_print_item(item, "Here (%s): encryption ON, mode %s", ctx,
			               str_or(e2e_json_get_string(cfg, "mode"), "normal"));
		else
			e2e_print_item(item, "Here (%s): encryption off", ctx);
		e2e_print_item(item, "Keys from: %s", who->len > 0 ? who->str : "nobody yet");
		g_string_free(who, TRUE);
		g_ptr_array_unref(trusted);
		g_free(own);
	} else if (item != NULL && IS_QUERY(item)) {
		e2e_print_item(item, "Here: the address (ident@host) of %s is not known yet - wait for a message from them or join a common channel, then /e2e on.",
		               item_name(item));
	} else {
		e2e_print_item(item, "Run /e2e in a channel or query window to see its state.");
	}
	e2e_print_item(item, "Getting started on a channel:");
	e2e_print_item(item, "  1. in the channel window: /e2e on");
	e2e_print_item(item, "  2. the other person does the same (repartee, irssi, erssi, WeeChat: /e2e on)");
	e2e_print_item(item, "  3. keys are exchanged with the first message; on \"Pending key exchange from <nick>\"");
	e2e_print_item(item, "     accept it: /e2e accept <nick>  (or start it yourself: /e2e handshake <nick>)");
	e2e_print_item(item, "     in normal mode each of you accepts the other once: your key goes only to people you accepted");
	e2e_print_item(item, "  4. check: /e2e list - the peer shows as [trusted]");
	e2e_print_item(item, "  5. compare fingerprints over another channel (e.g. by phone): /e2e verify <nick>");
	e2e_print_item(item, "Turn off: /e2e off. Modes: /e2e mode normal (asks first) | auto-accept | quiet.");
	g_free(ctx);
	e2e_json_free(kr);
}

static void cmd_e2e(const char *data, SERVER_REC *server, WI_ITEM_REC *item)
{
	char **parts, **args, *sub;
	GPtrArray *words;
	guint i;

	if (!e2e_native_active()) {
		/* rpe2e.pl answers this command itself */
		e2e_print_item(item, "(the native e2e module is inactive while the rpe2e.pl script is loaded)");
		return;
	}
	if (item != NULL && server == NULL)
		server = item->server;

	/* split /\s+/, empty words dropped */
	words = g_ptr_array_new_with_free_func(g_free);
	parts = g_strsplit_set(data != NULL ? data : "", " \t\n\r\f\v", -1);
	for (i = 0; parts[i] != NULL; i++)
		if (*parts[i] != '\0')
			g_ptr_array_add(words, g_strdup(parts[i]));
	g_strfreev(parts);
	g_ptr_array_add(words, NULL);
	args = (char **) words->pdata;
	sub = g_ascii_strdown(args[0] != NULL ? args[0] : "", -1);
	if (args[0] != NULL)
		args++;

	if (e2e_keyring_is_lost() && *sub != '\0' && strcmp(sub, "help") != 0 &&
	    strcmp(sub, "reset") != 0 && strcmp(sub, "status") != 0 && strcmp(sub, "import") != 0) {
		/* nothing may be saved over the lost state */
		e2e_print_item(item, "%s", LOST_REFUSAL);
	} else if (*sub == '\0' || strcmp(sub, "help") == 0) {
		cmd_guide(item);
		e2e_print_item(item, "Encryption commands: on off mode fingerprint list status accept decline revoke unrevoke forget handshake verify reverify rotate export import autotrust reset");
	} else if (strcmp(sub, "on") == 0) {
		cmd_on(item);
	} else if (strcmp(sub, "off") == 0) {
		cmd_off(item, args[0]);
	} else if (strcmp(sub, "mode") == 0) {
		cmd_mode(item, args[0]);
	} else if (strcmp(sub, "fingerprint") == 0) {
		cmd_fingerprint(item);
	} else if (strcmp(sub, "list") == 0) {
		cmd_list(item, args);
	} else if (strcmp(sub, "status") == 0) {
		cmd_status(item);
	} else if (strcmp(sub, "accept") == 0) {
		cmd_accept(server, item, args[0]);
	} else if (strcmp(sub, "decline") == 0) {
		cmd_decline(item, args[0]);
	} else if (strcmp(sub, "revoke") == 0) {
		cmd_revoke(item, args[0], TRUE);
	} else if (strcmp(sub, "unrevoke") == 0) {
		cmd_revoke(item, args[0], FALSE);
	} else if (strcmp(sub, "forget") == 0) {
		cmd_forget(item, args);
	} else if (strcmp(sub, "handshake") == 0) {
		cmd_handshake(server, item, args[0]);
	} else if (strcmp(sub, "verify") == 0) {
		cmd_verify(item, args[0]);
	} else if (strcmp(sub, "reverify") == 0) {
		cmd_reverify(item, args[0]);
	} else if (strcmp(sub, "rotate") == 0) {
		cmd_rotate(item);
	} else if (strcmp(sub, "export") == 0) {
		cmd_export(item, args[0]);
	} else if (strcmp(sub, "import") == 0) {
		cmd_import(item, args[0]);
	} else if (strcmp(sub, "autotrust") == 0) {
		cmd_autotrust(item, args);
	} else if (strcmp(sub, "reset") == 0) {
		cmd_reset(item);
	} else {
		e2e_print_item(item, "unknown subcommand");
	}
	g_free(sub);
	g_ptr_array_unref(words);
}

/* the subcommands are dispatched by cmd_e2e (as in rpe2e.pl); they are
   bound only for completion and /help */
static void cmd_e2e_sub(void)
{
}

void e2e_commands_init(void)
{
	int i;

	command_bind("e2e", NULL, (SIGNAL_FUNC) cmd_e2e);
	for (i = 0; subcommands[i] != NULL; i++) {
		char *cmd = g_strconcat("e2e ", subcommands[i], NULL);

		command_bind(cmd, NULL, (SIGNAL_FUNC) cmd_e2e_sub);
		g_free(cmd);
	}
}

void e2e_commands_deinit(void)
{
	int i;

	command_unbind("e2e", (SIGNAL_FUNC) cmd_e2e);
	for (i = 0; subcommands[i] != NULL; i++) {
		char *cmd = g_strconcat("e2e ", subcommands[i], NULL);

		command_unbind(cmd, (SIGNAL_FUNC) cmd_e2e_sub);
		g_free(cmd);
	}
}
