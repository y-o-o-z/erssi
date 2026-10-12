/*
 fe-web-e2e.c : E2E state and key-request actions for web clients

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The protocol is docs/fe-web-e2e.md (version 1). fe-web knows nothing of
 E2E itself: the e2e module tells the state of a channel or query with the
 signal "e2e state" (server, target, JSON) - on every change, and at once
 when asked with "e2e state request" (server, target) - and acts on
 "e2e action" (server, target, action, handle). Without the module nobody
 answers: no e2e_state is ever sent, and an e2e_action is refused.

 The e2e module refuses an action it does not take with "e2e action
 refused" (server, target, handle, reason) while it handles it; a
 "mismatch" (the request waits with another key than the client's
 fingerprint) is answered as e2e_mismatch.

 An e2e_action is a message of a logged-in client, handled through
 fe_web_client_handle_message() like every other: the /UNLOAD guard
 counts it, and only a client that passed the login (with its pacing)
 gets here. It is checked word by word and goes on as four separate
 strings - no command line is built from it - and the e2e module acts
 only on a key request that waits with exactly that ident@host.
*/

#include "module.h"
#include "fe-web.h"

#include <irssi/src/core/levels.h>
#include <irssi/src/core/modules.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-common/core/printtext.h>

#include <string.h>

/* the longest ident@host and target an action may name */
#define E2E_NAME_MAX 255
/* how deep the e2e object may nest (e2e_window_info_json: 3) */
#define E2E_JSON_DEPTH 8

/* the dump in progress: the answer to its request is for that client */
static WEB_CLIENT_REC *dump_client;
static IRC_SERVER_REC *dump_server;
static const char *dump_target;

/* the action being handed to the e2e module, and why it refused it (its
   "e2e action refused" while the action is emitted) */
typedef struct {
	IRC_SERVER_REC *server;
	const char *target, *handle;
	char *reason;
} ACTION_ASKED;
static ACTION_ASKED *asked;

/* ---- the e2e object: one JSON object, nothing else ---- */

static void json_ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
		(*p)++;
}

static gboolean json_string(const char **p)
{
	const unsigned char *s = (const unsigned char *) *p;
	int i;

	if (*s != '"')
		return FALSE;
	for (s++;; s++) {
		if (*s == '"')
			break;
		if (*s < 0x20)	/* also the end of the text */
			return FALSE;
		if (*s != '\\')
			continue;
		s++;
		if (*s == 'u') {
			for (i = 1; i <= 4; i++)
				if (!g_ascii_isxdigit(s[i]))
					return FALSE;
			s += 4;
		} else if (*s == '\0' || strchr("\"\\/bfnrt", *s) == NULL) {
			return FALSE;
		}
	}
	*p = (const char *) s + 1;
	return TRUE;
}

static gboolean json_digits(const char **p)
{
	if (!g_ascii_isdigit(**p))
		return FALSE;
	while (g_ascii_isdigit(**p))
		(*p)++;
	return TRUE;
}

static gboolean json_number(const char **p)
{
	if (**p == '-')
		(*p)++;
	if (**p == '0')
		(*p)++;
	else if (!json_digits(p))
		return FALSE;
	if (**p == '.') {
		(*p)++;
		if (!json_digits(p))
			return FALSE;
	}
	if (**p == 'e' || **p == 'E') {
		(*p)++;
		if (**p == '+' || **p == '-')
			(*p)++;
		if (!json_digits(p))
			return FALSE;
	}
	return TRUE;
}

static gboolean json_word(const char **p, const char *word)
{
	gsize len = strlen(word);

	if (strncmp(*p, word, len) != 0)
		return FALSE;
	*p += len;
	return TRUE;
}

static gboolean json_value(const char **p, int depth);

/* the members of an object or the elements of an array, after the
   opening bracket, up to and with the closing one */
static gboolean json_items(const char **p, int depth, gboolean object)
{
	char close = object ? '}' : ']';

	json_ws(p);
	if (**p == close) {
		(*p)++;
		return TRUE;
	}
	for (;;) {
		if (object) {
			if (!json_string(p))
				return FALSE;
			json_ws(p);
			if (**p != ':')
				return FALSE;
			(*p)++;
		}
		if (!json_value(p, depth + 1))
			return FALSE;
		json_ws(p);
		if (**p == close) {
			(*p)++;
			return TRUE;
		}
		if (**p != ',')
			return FALSE;
		(*p)++;
		json_ws(p);
	}
}

static gboolean json_value(const char **p, int depth)
{
	if (depth > E2E_JSON_DEPTH)
		return FALSE;
	json_ws(p);
	switch (**p) {
	case '{':
	case '[':
		(*p)++;
		return json_items(p, depth, (*p)[-1] == '{');
	case '"':
		return json_string(p);
	case 't':
		return json_word(p, "true");
	case 'f':
		return json_word(p, "false");
	case 'n':
		return json_word(p, "null");
	default:
		return (**p == '-' || g_ascii_isdigit(**p)) && json_number(p);
	}
}

gboolean fe_web_e2e_json_valid(const char *json)
{
	const char *p = json;

	if (json == NULL || strncmp(json, "{\"v\":", 5) != 0 ||
	    json[strlen(json) - 1] != '}' || !g_utf8_validate(json, -1, NULL))
		return FALSE;
	/* exactly one value, and that is the object: nothing after it */
	return json_value(&p, 0) && *p == '\0';
}

/* ---- e2e_state ---- */

static void sig_e2e_state(SERVER_REC *server, const char *target, const char *json)
{
	IRC_SERVER_REC *irc = IRC_SERVER(server);
	WEB_MESSAGE_REC *msg;

	if (irc == NULL || target == NULL || !fe_web_e2e_json_valid(json))
		return;
	msg = fe_web_message_new(WEB_MSG_E2E_STATE);
	msg->id = fe_web_generate_message_id();
	msg->server_tag = g_strdup(server->tag);
	msg->target = g_strdup(target);
	msg->text = g_strdup(json);
	if (dump_client != NULL && irc == dump_server && strcmp(target, dump_target) == 0) {
		/* the answer to the dump's request: the client being dumped
		   has it in order, after the item's nicklist; the others got
		   the state when it changed */
		if (g_slist_find(web_clients, dump_client) != NULL)
			fe_web_send_message(dump_client, msg);
	} else {
		fe_web_send_to_server_clients(irc, msg);
	}
	fe_web_message_free(msg);
}

void fe_web_e2e_dump(WEB_CLIENT_REC *client, IRC_SERVER_REC *server, const char *target)
{
	WEB_CLIENT_REC *old_client = dump_client;
	IRC_SERVER_REC *old_server = dump_server;
	const char *old_target = dump_target;

	if (client == NULL || server == NULL || target == NULL || module_find("e2e") == NULL)
		return;
	dump_client = client;
	dump_server = server;
	dump_target = target;
	signal_emit("e2e state request", 2, server, target);
	dump_client = old_client;
	dump_server = old_server;
	dump_target = old_target;
}

/* ---- e2e_action ---- */

gboolean fe_web_e2e_handle_valid(const char *handle)
{
	const unsigned char *p;
	gsize len;
	int at = 0;

	if (handle == NULL)
		return FALSE;
	len = strlen(handle);
	if (len < 3 || len > E2E_NAME_MAX || handle[0] == '@' || handle[len - 1] == '@')
		return FALSE;
	for (p = (const unsigned char *) handle; *p != '\0'; p++) {
		if (*p <= 0x20 || *p == 0x7f || *p == ',' || *p == '!')
			return FALSE;
		if (*p == '@')
			at++;
	}
	return at == 1;
}

/* the start of a key's fingerprint as e2e_state shows it: exactly 16
   lowercase hex digits */
static gboolean fingerprint_valid(const char *fp)
{
	int i;

	if (fp == NULL)
		return FALSE;
	for (i = 0; i < 16; i++)
		if (!g_ascii_isdigit(fp[i]) && (fp[i] < 'a' || fp[i] > 'f'))
			return FALSE;
	return fp[16] == '\0';
}

static void sig_action_refused(SERVER_REC *server, const char *target, const char *handle,
                               const char *reason)
{
	if (asked != NULL && asked->reason == NULL && reason != NULL &&
	    IRC_SERVER(server) == asked->server && g_strcmp0(target, asked->target) == 0 &&
	    g_strcmp0(handle, asked->handle) == 0)
		asked->reason = g_strdup(reason);
}

/* a channel or nick: one IRC target, as the server would take it */
static gboolean target_valid(const char *target)
{
	const unsigned char *p;
	gsize len;

	if (target == NULL)
		return FALSE;
	len = strlen(target);
	if (len < 1 || len > E2E_NAME_MAX)
		return FALSE;
	for (p = (const unsigned char *) target; *p != '\0'; p++)
		if (*p <= 0x20 || *p == 0x7f || *p == ',')
			return FALSE;
	return TRUE;
}

static void send_result(WEB_CLIENT_REC *client, const char *id, gboolean success,
                        const char *message, const char *error_code)
{
	WEB_MESSAGE_REC *msg;
	GString *json;

	json = fe_web_build_command_result_json(success, message, error_code);
	msg = fe_web_message_new(WEB_MSG_COMMAND_RESULT);
	msg->id = fe_web_generate_message_id();
	msg->response_to = g_strdup(id);
	msg->text = g_string_free(json, FALSE);
	fe_web_send_message(client, msg);
	fe_web_message_free(msg);
}

/* said in erssi with fe-web's own words only: nothing the client sent is
   printed */
static void refuse(WEB_CLIENT_REC *client, const char *id, const char *error_code,
                   const char *why)
{
	char *text = g_strconcat("fe-web: e2e_action of a web client refused: ", why,
	                         " - nothing sent", NULL);

	printtext_string(NULL, NULL, MSGLEVEL_CLIENTERROR, text);
	g_free(text);
	send_result(client, id, FALSE, why, error_code);
}

void fe_web_handle_e2e_action(WEB_CLIENT_REC *client, const char *json)
{
	char *id, *tag, *target, *action, *handle, *fingerprint;
	IRC_SERVER_REC *server = NULL;
	ACTION_ASKED this, *outer;
	guint64 serial;

	/* only a logged-in client's message: none other is handled at all,
	   and this one never acts for a client that has not logged in */
	if (client == NULL || json == NULL || !client->authenticated || !client->handshake_done)
		return;

	id = fe_web_json_get_string(json, "id");
	tag = fe_web_json_get_string(json, "server");
	target = fe_web_json_get_string(json, "target");
	action = fe_web_json_get_string(json, "action");
	handle = fe_web_json_get_string(json, "handle");
	/* optional; when there, it must be one (not null, not a number) */
	fingerprint = fe_web_json_get_string(json, "fingerprint");

	if (action == NULL || (strcmp(action, "accept") != 0 && strcmp(action, "decline") != 0)) {
		refuse(client, id, "e2e_invalid", "the action is neither accept nor decline");
	} else if (!fe_web_e2e_handle_valid(handle)) {
		refuse(client, id, "e2e_invalid", "the handle is not one ident@host");
	} else if (fe_web_json_has_key(json, "fingerprint") && !fingerprint_valid(fingerprint)) {
		refuse(client, id, "e2e_invalid", "the fingerprint is not 16 lowercase hex digits");
	} else if (!target_valid(target)) {
		refuse(client, id, "e2e_invalid", "the target is not a channel or nick");
	} else if (tag == NULL || *tag == '\0') {
		refuse(client, id, "e2e_invalid", "no server named");
	} else if ((server = IRC_SERVER(server_find_tag(tag))) == NULL) {
		/* not there, or not IRC; one still connecting is the e2e
		   module's to refuse (it says so) */
		refuse(client, id, "e2e_no_server", "no such server");
	} else if (module_find("e2e") == NULL) {
		refuse(client, id, "e2e_unavailable", "the e2e module is not loaded");
	} else {
		/* the module decides and tells: in erssi, and as a new
		   e2e_state when something changed */
		serial = client->serial;
		memset(&this, 0, sizeof(this));
		this.server = server;
		this.target = target;
		this.handle = handle;
		outer = asked;
		asked = &this;
		server_ref(SERVER(server));
		signal_emit("e2e action", 5, server, target, action, handle, fingerprint);
		server_unref(SERVER(server));
		asked = outer;
		/* answered unless the client was closed meanwhile */
		if (g_slist_find(web_clients, client) != NULL && client->serial == serial) {
			if (g_strcmp0(this.reason, "mismatch") == 0)
				/* the request waits with another key than the
				   one the client showed: nothing done */
				send_result(client, id, FALSE, "the request waits with another key",
				            "e2e_mismatch");
			else
				send_result(client, id, TRUE, "handed to the e2e module", NULL);
		}
		g_free(this.reason);
	}

	g_free(id);
	g_free(tag);
	g_free(target);
	g_free(action);
	g_free(handle);
	g_free(fingerprint);
}

void fe_web_e2e_init(void)
{
	dump_client = NULL;
	asked = NULL;
	signal_add("e2e state", (SIGNAL_FUNC) sig_e2e_state);
	signal_add("e2e action refused", (SIGNAL_FUNC) sig_action_refused);
}

void fe_web_e2e_deinit(void)
{
	signal_remove("e2e state", (SIGNAL_FUNC) sig_e2e_state);
	signal_remove("e2e action refused", (SIGNAL_FUNC) sig_action_refused);
	dump_client = NULL;
}
