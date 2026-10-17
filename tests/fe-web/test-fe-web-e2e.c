/*
 test-fe-web-e2e.c : the E2E state and key-request actions of web clients

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 fe-web's half of docs/fe-web-e2e.md: an "e2e state" signal becomes an
 e2e_state message (its JSON embedded only when it is one well-formed
 object that starts with {"v":), the state dump asks for the state of
 every channel and query ("e2e state request"), and an e2e_action of a
 logged-in client becomes the "e2e action" signal - only with a valid
 action, ident@host, server and target, and only when the e2e module is
 there. The e2e module itself is not in this harness: a module record
 named "e2e" stands for it.
*/

/* for signal_add() and module_register() */
#define MODULE_NAME "test-fe-web-e2e"

#include "fe-web-test.h"

#include <irssi/src/core/modules.h>
#include <irssi/src/core/modules-load.h>
#include <irssi/src/core/queries.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/irc/core/irc-channels.h>
#include <irssi/src/irc/core/irc-queries.h>

#include <string.h>

/* ---- what the signals carried ---- */

typedef struct {
	SERVER_REC *server;
	char *target, *action, *handle, *fingerprint;
} ACTION_SEEN;

static GPtrArray *actions;	/* ACTION_SEEN * */
static GPtrArray *requests;	/* ACTION_SEEN *, action and handle NULL */
/* what the stand-in e2e module answers a request with (NULL: nothing),
 * and the target it also tells the state of then (NULL: none) */
static const char *answer_json;
static const char *also_target;

static void seen_free(ACTION_SEEN *s)
{
	g_free(s->target);
	g_free(s->action);
	g_free(s->handle);
	g_free(s->fingerprint);
	g_free(s);
}

/* what the stand-in e2e module answers an action with: "e2e action
 * refused" with this reason (NULL: nothing), for this handle (NULL: the
 * action's) */
static const char *refuse_reason;
static const char *refuse_handle;
/* refuse with a NULL reason */
static gboolean refuse_without_reason;

static void sig_action(SERVER_REC *server, const char *target, const char *action,
                       const char *handle, const char *fingerprint)
{
	ACTION_SEEN *s = g_new0(ACTION_SEEN, 1);

	s->server = server;
	s->target = g_strdup(target);
	s->action = g_strdup(action);
	s->handle = g_strdup(handle);
	s->fingerprint = g_strdup(fingerprint);
	g_ptr_array_add(actions, s);
	/* answered synchronously, as the e2e module does */
	if (refuse_reason != NULL || refuse_without_reason)
		signal_emit("e2e action refused", 4, server, target,
		            refuse_handle != NULL ? refuse_handle : handle, refuse_reason);
}

static void sig_request(SERVER_REC *server, const char *target)
{
	ACTION_SEEN *s = g_new0(ACTION_SEEN, 1);

	s->server = server;
	s->target = g_strdup(target);
	g_ptr_array_add(requests, s);
	/* answered synchronously, as the e2e module does */
	if (answer_json != NULL)
		signal_emit("e2e state", 3, server, target, answer_json);
	if (answer_json != NULL && also_target != NULL)
		signal_emit("e2e state", 3, server, also_target, answer_json);
}

static void reset_seen(void)
{
	g_ptr_array_set_size(actions, 0);
	g_ptr_array_set_size(requests, 0);
	g_free(fe_web_fuzz_take_printed());
}

/* ---- the stand-in e2e module ---- */

static void e2e_module_add(void)
{
	if (module_find("e2e") == NULL)
		module_register("e2e", "core");
	g_assert_nonnull(module_find("e2e"));
}

static void e2e_module_remove(void)
{
	MODULE_REC *rec = module_find("e2e");

	if (rec != NULL)
		module_unload(rec);
	g_assert_null(module_find("e2e"));
}

/* ---- a logged-in client that reads plain text frames ---- */

static WEB_CLIENT_REC *client_new(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	/* text frames, readable here (the AES layer is fe-web-crypto's) */
	c->encryption_enabled = FALSE;
	return c;
}

/* send 'json' as the browser does: a masked text frame */
static void client_says(WEB_CLIENT_REC *c, const char *json)
{
	GByteArray *f = test_frame(WS_OPCODE_TEXT, json, strlen(json), TRUE);

	fe_web_fuzz_client_send(c, f->data, f->len);
	g_byte_array_unref(f);
	g_assert_true(fe_web_fuzz_client_alive(c));
}

/* the text the browser read since the client was made, frame headers
 * (which may hold NUL bytes) as '.'; to be freed */
static char *client_heard(void)
{
	const GByteArray *r;
	char *text;
	guint i;

	fe_web_fuzz_read_peer(0);
	r = fe_web_fuzz_received();
	text = g_malloc(r->len + 1);
	for (i = 0; i < r->len; i++)
		text[i] = r->data[i] != '\0' ? (char) r->data[i] : '.';
	text[r->len] = '\0';
	return text;
}

/* offset of 'needle' in what the browser read, -1 if not there */
static gssize heard_at(const char *needle)
{
	char *all = client_heard();
	const char *p = strstr(all, needle);
	gssize at = p != NULL ? p - all : -1;

	g_free(all);
	return at;
}

/* ---- e2e_state messages ---- */

static char *state_json(const char *text)
{
	WEB_MESSAGE_REC *msg = fe_web_message_new(WEB_MSG_E2E_STATE);
	char *json;

	msg->id = g_strdup("x");
	msg->server_tag = g_strdup("fuzz");
	msg->target = g_strdup("#test");
	msg->timestamp = 1;
	msg->text = g_strdup(text);
	json = fe_web_message_to_json(msg);
	fe_web_message_free(msg);
	return json;
}

static void test_state_message(void)
{
	/* not one JSON object starting with {"v": - never embedded */
	static const char *const refused[] = {
		"garbage",
		"",
		"{}",
		"{\"state\":\"on\",\"v\":1}",
		"{\"v\":1",
		"{\"v\":1}}",
		"{\"v\":1} ",
		"{\"v\":1},\"x\":{\"y\":1}",
		"{\"v\":1,\"state\":\"on\"},\"type\":\"auth_ok\",\"z\":{\"v\":1}",
		"{\"v\":1,\"state\":\"o\"n\"}",
		"{\"v\":1,\"state\":\"on\\\"}",
		"{\"v\":1,\"state\":\"o\nn\"}",
		"{\"v\":1,\"state\":\"\\x41\"}",
		"{\"v\":1,\"state\":\"\\u12\"}",
		"{\"v\":1,\"state\":\"\xc3\x28\"}",
		"{\"v\":1,\"state\":\"\xff\"}",
		"{\"v\":1,\"pending\":[}",
		"{\"v\":1,\"pending\":[{]}",
		"{\"v\":1,\"pending\":[1,]}",
		"{\"v\":1,\"a\":1,}",
		"{\"v\":01}",
		"{\"v\":1.}",
		"{\"v\":-}",
		"{\"v\":tru}",
		"{\"v\":1,\"a\"}",
		"{\"v\":1,\"a\":}",
		"{\"v\":1 \"a\":1}",
		"{\"v\":1,1:1}",
		"{\"v\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]}",
	};
	char *json;
	gsize i;

	json = state_json("{\"v\":1,\"state\":\"on\"}");
	g_assert_cmpstr(json, ==,
	                "{\"id\":\"x\",\"type\":\"e2e_state\",\"server\":\"fuzz\",\"channel\":"
	                "\"#test\",\"timestamp\":1,\"e2e\":{\"v\":1,\"state\":\"on\"}}");
	g_free(json);

	/* what e2e_window_info_json() writes: every kind of value */
	json = state_json("{\"v\":1,\"state\":\"on\",\"network\":\"E2E\",\"context\":\"#test\","
	                  "\"mode\":null,\"sessions\":2,\"pending\":[{\"nick\":\"b\\\"\\\\\\u0001\","
	                  "\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"0123456789abcdef\","
	                  "\"received_at\":-1}],\"x\":[true,false,1.5e-3,\"\xc5\xbc\"]}");
	g_assert_nonnull(strstr(json, ",\"e2e\":{\"v\":1,\"state\":\"on\",\"network\""));
	g_assert_true(g_str_has_suffix(json, "\"\xc5\xbc\"]}}"));
	g_free(json);

	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		json = state_json(refused[i]);
		if (strstr(json, "\"e2e\"") != NULL)
			g_error("embedded: %s -> %s", refused[i], json);
		g_assert_null(strstr(json, "\"text\""));
		g_assert_true(g_str_has_suffix(json, ",\"timestamp\":1}"));
		g_free(json);
	}

	/* no text at all */
	json = state_json(NULL);
	g_assert_null(strstr(json, "\"e2e\""));
	g_free(json);
}

/* the signal reaches the clients of that server, and only them */
static void test_state_signal(void)
{
	IRC_SERVER_REC *server = fe_web_fuzz_server_new("fuzz");
	IRC_SERVER_REC *other = fe_web_fuzz_server_new("other");
	WEB_CLIENT_REC *c = client_new();

	c->server = server;
	signal_emit("e2e state", 3, server, "#test", "{\"v\":1,\"state\":\"waiting\"}");
	g_assert_cmpint(heard_at("\"type\":\"e2e_state\",\"server\":\"fuzz\",\"channel\":\"#test\""),
	                >=, 0);
	g_assert_cmpint(heard_at("\"e2e\":{\"v\":1,\"state\":\"waiting\"}}"), >=, 0);

	/* another server's state: not for this client */
	signal_emit("e2e state", 3, other, "#test", "{\"v\":1,\"state\":\"blocked\"}");
	g_assert_cmpint(heard_at("blocked"), <, 0);

	/* JSON that is not one object: nothing sent at all */
	signal_emit("e2e state", 3, server, "#test", "{\"v\":1},\"x\":{\"v\":1}");
	g_assert_cmpint(heard_at("\"x\""), <, 0);
	signal_emit("e2e state", 3, server, "#test", NULL);
	signal_emit("e2e state", 3, server, NULL, "{\"v\":1}");
	signal_emit("e2e state", 3, NULL, "#test", "{\"v\":1}");
	{
		char *all = client_heard();
		const char *p = all;
		int n = 0;

		while ((p = strstr(p, "\"type\":\"e2e_state\"")) != NULL) {
			n++;
			p++;
		}
		g_assert_cmpint(n, ==, 1);
		g_free(all);
	}

	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(other);
	fe_web_fuzz_server_free(server);
}

/* ---- e2e_action ---- */

static void assert_one_action(SERVER_REC *server, const char *target, const char *action,
                              const char *handle)
{
	ACTION_SEEN *s;

	g_assert_cmpuint(actions->len, ==, 1);
	s = g_ptr_array_index(actions, 0);
	g_assert_true(s->server == server);
	g_assert_cmpstr(s->target, ==, target);
	g_assert_cmpstr(s->action, ==, action);
	g_assert_cmpstr(s->handle, ==, handle);
}

static void assert_fingerprint(const char *fingerprint)
{
	g_assert_cmpuint(actions->len, ==, 1);
	g_assert_cmpstr(((ACTION_SEEN *) g_ptr_array_index(actions, 0))->fingerprint, ==, fingerprint);
}

static void test_action_signal(void)
{
	IRC_SERVER_REC *server;
	WEB_CLIENT_REC *c;

	e2e_module_add();
	server = fe_web_fuzz_server_new("fuzz");
	c = client_new();

	reset_seen();
	client_says(c, "{\"type\":\"e2e_action\",\"id\":\"r1\",\"server\":\"fuzz\",\"target\":"
	               "\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}");
	assert_one_action(SERVER(server), "#test", "accept", "bob@127.0.0.1");
	g_assert_cmpint(heard_at("\"type\":\"command_result\",\"response_to\":\"r1\""), >=, 0);
	g_assert_cmpint(heard_at("\"success\":true"), >=, 0);

	/* decline, in a query; a UTF-8 host */
	reset_seen();
	client_says(c, "{\"type\":\"e2e_action\",\"id\":\"r2\",\"server\":\"fuzz\",\"target\":"
	               "\"bob\",\"action\":\"decline\",\"handle\":\"bob@\xc5\xbc" "aba.example.org\"}");
	assert_one_action(SERVER(server), "bob", "decline", "bob@\xc5\xbc" "aba.example.org");
	/* no fingerprint: none passed on */
	assert_fingerprint(NULL);

	/* the fingerprint the client showed goes along */
	reset_seen();
	client_says(c, "{\"type\":\"e2e_action\",\"id\":\"r3\",\"server\":\"fuzz\",\"target\":"
	               "\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\","
	               "\"fingerprint\":\"0123456789abcdef\"}");
	assert_one_action(SERVER(server), "#test", "accept", "bob@127.0.0.1");
	assert_fingerprint("0123456789abcdef");
	g_assert_cmpint(heard_at("\"response_to\":\"r3\",\"success\":true"), >=, 0);

	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
	e2e_module_remove();
}

/* the e2e module refuses because the request's key is not the one the
 * client showed: e2e_mismatch; every other refusal of the module:
 * e2e_refused with its reason; one for another action, or none: success */
static void test_action_mismatch(void)
{
	static const char json[] =
	    "{\"type\":\"e2e_action\",\"id\":\"m1\",\"server\":\"fuzz\",\"target\":"
	    "\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\","
	    "\"fingerprint\":\"0123456789abcdef\"}";
	IRC_SERVER_REC *server;
	WEB_CLIENT_REC *c;

	e2e_module_add();
	server = fe_web_fuzz_server_new("fuzz");
	c = client_new();

	reset_seen();
	refuse_reason = "mismatch";
	client_says(c, json);
	g_assert_cmpuint(actions->len, ==, 1);
	g_assert_cmpint(heard_at("\"response_to\":\"m1\",\"success\":false"), >=, 0);
	g_assert_cmpint(heard_at("\"error_code\":\"e2e_mismatch\",\"reason\":\"mismatch\""), >=, 0);
	g_assert_cmpint(heard_at("e2e_refused"), <, 0);
	fe_web_fuzz_client_free(c);

	/* a mismatch of another handle is not this action's */
	c = client_new();
	reset_seen();
	refuse_reason = "mismatch";
	refuse_handle = "eve@127.0.0.1";
	client_says(c, json);
	g_assert_cmpint(heard_at("\"success\":true"), >=, 0);
	g_assert_cmpint(heard_at("e2e_mismatch"), <, 0);
	g_assert_cmpint(heard_at("\"reason\""), <, 0);
	refuse_reason = refuse_handle = NULL;
	fe_web_fuzz_client_free(c);

	/* told outside an action: nothing */
	c = client_new();
	signal_emit("e2e action refused", 4, server, "#test", "bob@127.0.0.1", "mismatch");
	g_assert_cmpint(heard_at("command_result"), <, 0);
	fe_web_fuzz_client_free(c);

	fe_web_fuzz_server_free(server);
	e2e_module_remove();
}

/* every other refusal of the module: success false, e2e_refused, and the
 * module's reason (a word of [a-z_], else "unknown") */
static void test_action_refused(void)
{
	static const char json[] =
	    "{\"type\":\"e2e_action\",\"id\":\"q1\",\"server\":\"fuzz\",\"target\":"
	    "\"#test\",\"action\":\"decline\",\"handle\":\"bob@127.0.0.1\"}";
	static const char *const reasons[][2] = {
		{ "no_request", "no_request" },
		{ "inactive", "inactive" },
		{ "lost", "lost" },
		{ "not_connected", "not_connected" },
		/* one the module may tell later: as it is */
		{ "busy", "busy" },
		/* from a script: never more than a word goes to the client */
		{ "x\",\"success\":true", "unknown" },
		{ "", "unknown" },
		{ "NO_REQUEST", "unknown" },
		{ "a_reason_far_too_long_to_be_one_word", "unknown" },
	};
	IRC_SERVER_REC *server;
	WEB_CLIENT_REC *c;
	gsize i;

	e2e_module_add();
	server = fe_web_fuzz_server_new("fuzz");

	for (i = 0; i < G_N_ELEMENTS(reasons); i++) {
		char *expect = g_strdup_printf(
		    "\"response_to\":\"q1\",\"success\":false,\"message\":\"%s",
		    "the e2e module refused the action: ");
		char *code = g_strdup_printf("\"error_code\":\"e2e_refused\",\"reason\":\"%s\",",
		                             reasons[i][1]);

		c = client_new();
		reset_seen();
		refuse_reason = reasons[i][0];
		client_says(c, json);
		refuse_reason = NULL;
		g_assert_cmpuint(actions->len, ==, 1);
		if (heard_at(expect) < 0 || heard_at(code) < 0) {
			char *all = client_heard();

			g_error("reason \"%s\": no %s / %s in %s", reasons[i][0], expect, code, all);
		}
		g_assert_cmpint(heard_at("\"success\":true"), <, 0);
		g_assert_cmpint(heard_at("e2e_mismatch"), <, 0);
		fe_web_fuzz_client_free(c);
		g_free(expect);
		g_free(code);
	}

	/* a refusal without a reason is one all the same */
	c = client_new();
	reset_seen();
	refuse_without_reason = TRUE;
	client_says(c, json);
	refuse_without_reason = FALSE;
	g_assert_cmpint(heard_at("\"error_code\":\"e2e_refused\",\"reason\":\"unknown\","), >=, 0);
	g_assert_cmpint(heard_at("\"success\":true"), <, 0);
	fe_web_fuzz_client_free(c);

	/* no refusal: handed over */
	c = client_new();
	reset_seen();
	client_says(c, json);
	g_assert_cmpint(heard_at("\"success\":true"), >=, 0);
	g_assert_cmpint(heard_at("\"reason\""), <, 0);
	fe_web_fuzz_client_free(c);

	fe_web_fuzz_server_free(server);
	e2e_module_remove();
}

/* one action that must not reach the e2e module: answered with 'code' */
static void assert_rejected(WEB_CLIENT_REC *c, const char *json, const char *code)
{
	char *expect = g_strdup_printf("\"error_code\":\"%s\"", code);
	gssize before = heard_at(expect);
	char *all;

	reset_seen();
	client_says(c, json);
	if (actions->len != 0)
		g_error("e2e action emitted for %s", json);
	all = client_heard();
	/* answered: one more refusal with that code than before */
	if ((before < 0 && strstr(all, expect) == NULL) ||
	    (before >= 0 && strstr(all + before + 1, expect) == NULL))
		g_error("no %s answer for %s", code, json);
	g_assert_null(strstr(all, "\"success\":true"));
	g_free(all);
	g_free(expect);
}

static void test_action_rejects(void)
{
	static const char *const invalid[] = {
		/* the action */
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"trust\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"Accept\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept \",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":1,\"handle\":\"bob@127.0.0.1\"}",
		/* the handle */
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"a b@c\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"@c\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"a@\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"a@b@c\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"abc\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"a\\r\\nPRIVMSG x :y@c\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"a,b@c\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"n!a@c\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"a@b\\u0001c\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":7}",
		/* the target */
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#a b\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#a,#b\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#a\\n\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
		/* the fingerprint: 16 lowercase hex digits when given */
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"0123456789ABCDEF\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"0123456789abcde\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"0123456789abcdef0\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"0123456789abcdeg\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"0123456789abcd f\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":123}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":null}",
		/* the server */
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
		"{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"\",\"target\":\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
	};
	IRC_SERVER_REC *server;
	WEB_CLIENT_REC *c;
	GString *big;
	char *json, *printed;
	gsize i;

	e2e_module_add();
	server = fe_web_fuzz_server_new("fuzz");
	c = client_new();

	for (i = 0; i < G_N_ELEMENTS(invalid); i++)
		assert_rejected(c, invalid[i], "e2e_invalid");
	/* 300 bytes */
	big = g_string_new("{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":"
	                   "\"#test\",\"action\":\"accept\",\"handle\":\"bob@");
	for (i = 0; i < 296; i++)
		g_string_append_c(big, 'h');
	g_string_append(big, "\"}");
	assert_rejected(c, big->str, "e2e_invalid");
	g_string_free(big, TRUE);

	/* a server that is not there */
	assert_rejected(c, "{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"nope\",\"target\":"
	                   "\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}",
	                "e2e_no_server");
	/* nothing of the client is printed: what fe-web says is its own */
	reset_seen();
	client_says(c, "{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"%n%s\\u0003,4\",\"target\":"
	               "\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}");
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, "e2e_action"));
	g_assert_null(strstr(printed, "%n"));
	g_assert_null(strstr(printed, "\003"));
	g_free(printed);

	/* without the e2e module */
	e2e_module_remove();
	json = "{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":"
	       "\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}";
	assert_rejected(c, json, "e2e_unavailable");

	/* and with it again: the same message goes through */
	e2e_module_add();
	reset_seen();
	client_says(c, json);
	assert_one_action(SERVER(server), "#test", "accept", "bob@127.0.0.1");

	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
	e2e_module_remove();
}

/* a client that has not logged in: nothing, not even an answer */
static void test_action_needs_login(void)
{
	static const char json[] =
	    "{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":"
	    "\"#test\",\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\"}";
	IRC_SERVER_REC *server;
	WEB_CLIENT_REC *c;

	e2e_module_add();
	server = fe_web_fuzz_server_new("fuzz");

	/* not logged in: its frames are a login request, never a message */
	c = fe_web_fuzz_client_new_peer("p", FALSE, FALSE);
	reset_seen();
	fe_web_client_handle_message(c, json);
	g_assert_cmpuint(actions->len, ==, 0);
	fe_web_fuzz_client_free(c);

	/* past the WebSocket handshake but not authenticated */
	c = fe_web_fuzz_client_new_peer("p", FALSE, FALSE);
	c->handshake_done = TRUE;
	reset_seen();
	fe_web_client_handle_message(c, json);
	g_assert_cmpuint(actions->len, ==, 0);
	fe_web_fuzz_client_free(c);

	fe_web_fuzz_server_free(server);
	e2e_module_remove();
}

/* what an "e2e action" handler runs still runs inside fe-web's input
 * handling: /UNLOAD of fe-web is refused */
static void emit_unload(void)
{
	signal_emit("command unload", 3, "web", NULL, NULL);
}

static gboolean unload_reached;

static void probe_unload(void)
{
	unload_reached = TRUE;
}

static void test_action_unload_guard(void)
{
	IRC_SERVER_REC *server;
	WEB_CLIENT_REC *c;
	char *printed;

	e2e_module_add();
	server = fe_web_fuzz_server_new("fuzz");
	c = client_new();
	signal_add("e2e action", (SIGNAL_FUNC) emit_unload);
	signal_add("command unload", (SIGNAL_FUNC) probe_unload);

	reset_seen();
	unload_reached = FALSE;
	client_says(c, "{\"type\":\"e2e_action\",\"id\":\"1\",\"server\":\"fuzz\",\"target\":"
	               "\"#test\",\"action\":\"decline\",\"handle\":\"bob@127.0.0.1\"}");
	g_assert_cmpuint(actions->len, ==, 1);
	g_assert_false(unload_reached);
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, "/UNLOAD of fe-web from a web client is refused"));
	g_free(printed);
	g_assert_nonnull(module_find("web"));
	g_assert_true(fe_web_fuzz_client_alive(c));

	signal_remove("command unload", (SIGNAL_FUNC) probe_unload);
	signal_remove("e2e action", (SIGNAL_FUNC) emit_unload);
	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
	e2e_module_remove();
}

static void test_handle_valid(void)
{
	static const char *const good[] = {
		"a@b", "bob@127.0.0.1", "~bob@host.example.org", "bob@\xc5\xbc" "aba",
		"b.o-b_[x]@2001:db8::1",
	};
	static const char *const bad[] = {
		"", "a@", "@b", "ab", "a@b@c", "@@@", "a b@c", "a@b c", "a\t@b", "a@b\n",
		"a\001@b", "a,b@c", "a@b,c", "n!a@b", "a@b!", "a\177@b",
	};
	GString *s;
	gsize i;

	g_assert_false(fe_web_e2e_handle_valid(NULL));
	for (i = 0; i < G_N_ELEMENTS(good); i++)
		if (!fe_web_e2e_handle_valid(good[i]))
			g_error("refused: %s", good[i]);
	for (i = 0; i < G_N_ELEMENTS(bad); i++)
		if (fe_web_e2e_handle_valid(bad[i]))
			g_error("taken: %s", bad[i]);

	/* 255 bytes: taken; 256: not */
	s = g_string_new("a@");
	while (s->len < 255)
		g_string_append_c(s, 'h');
	g_assert_true(fe_web_e2e_handle_valid(s->str));
	g_string_append_c(s, 'h');
	g_assert_false(fe_web_e2e_handle_valid(s->str));
	g_string_free(s, TRUE);
}

/* ---- the state dump ---- */

static void test_dump_requests_state(void)
{
	IRC_SERVER_REC *server;
	WEB_CLIENT_REC *c, *other;
	ACTION_SEEN *s;
	gssize nicklist, state, opened, qstate;
	char *printed;

	e2e_module_add();
	server = fe_web_fuzz_server_new("fuzz");
	irc_channel_create(server, "#test", NULL, FALSE);
	irc_query_create("fuzz", "bob", FALSE);
	c = client_new();
	c->server = server;

	reset_seen();
	answer_json = "{\"v\":1,\"state\":\"on\"}";
	fe_web_dump_state(c);
	answer_json = NULL;
	g_assert_cmpuint(requests->len, ==, 2);
	s = g_ptr_array_index(requests, 0);
	g_assert_true(s->server == SERVER(server));
	g_assert_cmpstr(s->target, ==, "#test");
	s = g_ptr_array_index(requests, 1);
	g_assert_true(s->server == SERVER(server));
	g_assert_cmpstr(s->target, ==, "bob");

	/* right after the channel's nicklist, and after the query_opened */
	nicklist = heard_at("\"type\":\"nicklist\"");
	state = heard_at("\"type\":\"e2e_state\",\"server\":\"fuzz\",\"channel\":\"#test\"");
	opened = heard_at("\"type\":\"query_opened\"");
	qstate = heard_at("\"type\":\"e2e_state\",\"server\":\"fuzz\",\"channel\":\"bob\"");
	g_assert_cmpint(nicklist, >=, 0);
	g_assert_cmpint(state, >, nicklist);
	g_assert_cmpint(opened, >, state);
	g_assert_cmpint(qstate, >, opened);

	/* the answer to a dump goes to the client being dumped only: another
	 * client of the server (one without a socket here, which fe-web
	 * reports when it tries to send to it) gets nothing of it */
	other = fe_web_client_create(-1, "other:0");
	other->handshake_done = TRUE;
	other->authenticated = TRUE;
	other->server = server;
	reset_seen();
	answer_json = "{\"v\":1,\"state\":\"on\"}";
	fe_web_dump_state(c);
	answer_json = NULL;
	g_assert_cmpuint(requests->len, ==, 2);
	printed = fe_web_fuzz_take_printed();
	g_assert_null(strstr(printed, "Cannot send e2e_state"));
	g_free(printed);
	/* but a state of another item told meanwhile is everyone's */
	answer_json = "{\"v\":1,\"state\":\"on\"}";
	also_target = "#elsewhere";
	fe_web_dump_state(c);
	answer_json = also_target = NULL;
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, "Cannot send e2e_state"));
	g_free(printed);
	g_assert_cmpint(heard_at("\"channel\":\"#elsewhere\""), >=, 0);
	/* a change, outside a dump: every client of the server */
	signal_emit("e2e state", 3, server, "#test", "{\"v\":1,\"state\":\"error\"}");
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, "Cannot send e2e_state"));
	g_free(printed);
	g_assert_cmpint(heard_at("\"state\":\"error\""), >=, 0);
	fe_web_client_destroy(other);
	fe_web_fuzz_client_free(c);

	/* without the e2e module nothing is asked */
	e2e_module_remove();
	c = client_new();
	c->server = server;
	reset_seen();
	answer_json = "{\"v\":1,\"state\":\"on\"}";
	fe_web_dump_state(c);
	answer_json = NULL;
	g_assert_cmpuint(requests->len, ==, 0);
	g_assert_cmpint(heard_at("e2e_state"), <, 0);
	fe_web_fuzz_client_free(c);

	fe_web_fuzz_server_free(server);
}

int main(int argc, char **argv)
{
	int ret;

	test_init(&argc, &argv);
	fe_web_client_commands_init();
	fe_web_e2e_init();
	actions = g_ptr_array_new_with_free_func((GDestroyNotify) seen_free);
	requests = g_ptr_array_new_with_free_func((GDestroyNotify) seen_free);
	signal_add("e2e action", (SIGNAL_FUNC) sig_action);
	signal_add("e2e state request", (SIGNAL_FUNC) sig_request);

	g_test_add_func("/fe-web/e2e/state-message", test_state_message);
	g_test_add_func("/fe-web/e2e/state-signal", test_state_signal);
	g_test_add_func("/fe-web/e2e/action-signal", test_action_signal);
	g_test_add_func("/fe-web/e2e/action-rejects", test_action_rejects);
	g_test_add_func("/fe-web/e2e/action-mismatch", test_action_mismatch);
	g_test_add_func("/fe-web/e2e/action-refused", test_action_refused);
	g_test_add_func("/fe-web/e2e/action-needs-login", test_action_needs_login);
	g_test_add_func("/fe-web/e2e/action-unload-guard", test_action_unload_guard);
	g_test_add_func("/fe-web/e2e/handle-valid", test_handle_valid);
	g_test_add_func("/fe-web/e2e/dump-requests-state", test_dump_requests_state);

	ret = g_test_run();

	signal_remove("e2e state request", (SIGNAL_FUNC) sig_request);
	signal_remove("e2e action", (SIGNAL_FUNC) sig_action);
	fe_web_e2e_deinit();
	fe_web_client_commands_deinit();
	g_ptr_array_unref(actions);
	g_ptr_array_unref(requests);
	return ret;
}
