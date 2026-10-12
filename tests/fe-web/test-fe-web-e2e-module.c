/*
 test-fe-web-e2e-module.c : fe-web with the e2e module's e2e-web.c

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The real e2e-web.c (src/e2e) between fe-web and stand-ins for the rest
 of the e2e module (below): the window state is what a test sets, the
 keyring file is not there, and e2e_web_action() records what it gets.
 What a web client is told on /unload of the module, and how an action
 and its refusal travel both ways.
*/

#include "fe-web-test.h"

#include <irssi/src/core/servers.h>
#include <irssi/src/core/window-item-def.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/modules.h>
#include <irssi/src/core/modules-load.h>
#include <irssi/src/irc/core/irc-channels.h>
#include <irssi/src/e2e/e2e-core.h>
#include <irssi/src/e2e/e2e-web.h>

#include <string.h>

/* ---- stand-ins for e2e-core.c and e2e-commands.c ---- */

static E2E_WIN_STATE stub_state = E2E_WIN_OFF;
static gboolean stub_script = FALSE;
static char *stub_keyring;
/* what e2e_web_action() got last, and the refusal it tells (NULL: none) */
static char *acted_target, *acted_handle, *acted_fingerprint;
static gboolean acted_accept;
static int acted;
static const char *stub_refuse;

const char *e2e_keyring_file(void)
{
	return stub_keyring;
}

gboolean e2e_script_loaded(void)
{
	return stub_script;
}

gboolean e2e_same_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
	       a->st_mtime == b->st_mtime;
}

void e2e_server_net(SERVER_REC *server, E2E_NET *net)
{
	e2e_net_init(net, NULL, server->tag, NULL, TRUE);
}

gboolean e2e_window_info_with(const E2E_JSON *kr, gboolean kr_ok, WI_ITEM_REC *item,
                              E2E_WIN_INFO *out)
{
	memset(out, 0, sizeof(*out));
	if (item == NULL || item->server == NULL)
		return FALSE;
	out->state = stub_state;
	out->mode = g_strdup("normal");
	out->pending = g_ptr_array_new_with_free_func((GDestroyNotify) e2e_pending_free);
	return TRUE;
}

gboolean e2e_window_info_for(SERVER_REC *server, WI_ITEM_REC *item, E2E_WIN_INFO *out)
{
	return e2e_window_info_with(NULL, TRUE, item, out);
}

void e2e_web_action(SERVER_REC *server, const char *target, gboolean accept, const char *handle,
                    const char *fingerprint)
{
	acted++;
	acted_accept = accept;
	g_free(acted_target);
	g_free(acted_handle);
	g_free(acted_fingerprint);
	acted_target = g_strdup(target);
	acted_handle = g_strdup(handle);
	acted_fingerprint = g_strdup(fingerprint);
	if (stub_refuse != NULL)
		signal_emit("e2e action refused", 4, server, target, handle, stub_refuse);
}

/* ---- helpers ---- */

static WEB_CLIENT_REC *client_new(IRC_SERVER_REC *server)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	c->encryption_enabled = FALSE;
	c->server = server;
	return c;
}

/* what the browser read, NUL bytes (frame headers) as '.'; to be freed */
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

static int heard_count(const char *needle)
{
	char *all = client_heard();
	const char *p = all;
	int n = 0;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p++;
	}
	g_free(all);
	return n;
}

static void client_says(WEB_CLIENT_REC *c, const char *json)
{
	GByteArray *f = test_frame(WS_OPCODE_TEXT, json, strlen(json), TRUE);

	fe_web_fuzz_client_send(c, f->data, f->len);
	g_byte_array_unref(f);
}

#define STATE_TEST "\"type\":\"e2e_state\",\"server\":\"fuzz\",\"channel\":\"#test\""
#define OFF "\"e2e\":{\"v\":1,\"state\":\"off\""
#define WAITING "\"e2e\":{\"v\":1,\"state\":\"waiting\""

/* ---- tests ---- */

/* told through the dump's request only (no refresh has run: the module was
 * just loaded), then the module goes: the client is told "off" */
static void test_unload_after_request(void)
{
	IRC_SERVER_REC *server = fe_web_fuzz_server_new("fuzz");
	WEB_CLIENT_REC *c;

	irc_channel_create(server, "#test", NULL, FALSE);
	stub_state = E2E_WIN_WAITING;
	e2e_web_init();
	c = client_new(server);
	fe_web_dump_state(c);
	g_assert_cmpint(heard_count(WAITING), ==, 1);
	g_assert_cmpint(heard_count(OFF), ==, 0);

	e2e_web_deinit();
	g_assert_cmpint(heard_count(OFF), ==, 1);

	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
	stub_state = E2E_WIN_OFF;
}

/* told by a refresh, then by a request something the refresh has not
 * seen yet, then the module goes: "off" once for each item told */
static void test_unload_after_refresh(void)
{
	IRC_SERVER_REC *server = fe_web_fuzz_server_new("fuzz");
	WEB_CLIENT_REC *c;

	irc_channel_create(server, "#test", NULL, FALSE);
	irc_channel_create(server, "#other", NULL, FALSE);
	c = client_new(server);
	stub_state = E2E_WIN_OFF;
	e2e_web_init();
	e2e_web_refresh();
	/* the refresh's "off" (mode normal) for both */
	g_assert_cmpint(heard_count("\"channel\":\"#test\""), ==, 1);
	g_assert_cmpint(heard_count("\"channel\":\"#other\""), ==, 1);
	stub_state = E2E_WIN_WAITING;
	signal_emit("e2e state request", 2, server, "#test");
	g_assert_cmpint(heard_count(WAITING), ==, 1);

	e2e_web_deinit();
	/* the module's own "off" has no mode: after the refresh's ones */
	g_assert_cmpint(heard_count(OFF ",\"network\":\"fuzz\",\"context\":null,\"mode\":null"), ==, 2);

	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
	stub_state = E2E_WIN_OFF;
}

/* nothing told, nothing to take back */
static void test_unload_nothing_told(void)
{
	IRC_SERVER_REC *server = fe_web_fuzz_server_new("fuzz");
	WEB_CLIENT_REC *c;

	irc_channel_create(server, "#test", NULL, FALSE);
	c = client_new(server);
	e2e_web_init();
	e2e_web_deinit();
	g_assert_cmpint(heard_count("e2e_state"), ==, 0);
	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
}

/* an action from the web reaches e2e_web_action() with its fingerprint,
 * and the module's mismatch comes back as e2e_mismatch */
static void test_action_both_ways(void)
{
	static const char with_fp[] =
	    "{\"type\":\"e2e_action\",\"id\":\"a1\",\"server\":\"fuzz\",\"target\":\"#test\","
	    "\"action\":\"accept\",\"handle\":\"bob@127.0.0.1\",\"fingerprint\":\"0123456789abcdef\"}";
	IRC_SERVER_REC *server = fe_web_fuzz_server_new("fuzz");
	WEB_CLIENT_REC *c;

	irc_channel_create(server, "#test", NULL, FALSE);
	e2e_web_init();

	c = client_new(server);
	acted = 0;
	client_says(c, with_fp);
	g_assert_cmpint(acted, ==, 1);
	g_assert_true(acted_accept);
	g_assert_cmpstr(acted_target, ==, "#test");
	g_assert_cmpstr(acted_handle, ==, "bob@127.0.0.1");
	g_assert_cmpstr(acted_fingerprint, ==, "0123456789abcdef");
	g_assert_cmpint(heard_count("\"response_to\":\"a1\",\"success\":true"), ==, 1);
	fe_web_fuzz_client_free(c);

	c = client_new(server);
	stub_refuse = "mismatch";
	client_says(c, with_fp);
	stub_refuse = NULL;
	g_assert_cmpint(acted, ==, 2);
	g_assert_cmpint(heard_count("\"error_code\":\"e2e_mismatch\""), ==, 1);
	fe_web_fuzz_client_free(c);

	/* decline, no fingerprint */
	c = client_new(server);
	client_says(c, "{\"type\":\"e2e_action\",\"id\":\"a3\",\"server\":\"fuzz\",\"target\":"
	               "\"#test\",\"action\":\"decline\",\"handle\":\"bob@127.0.0.1\"}");
	g_assert_cmpint(acted, ==, 3);
	g_assert_false(acted_accept);
	g_assert_null(acted_fingerprint);
	fe_web_fuzz_client_free(c);

	e2e_web_deinit();
	fe_web_fuzz_server_free(server);
}

int main(int argc, char **argv)
{
	int ret;

	test_init(&argc, &argv);
	fe_web_e2e_init();
	/* the module is there: fe-web asks it, and hands it actions */
	module_register("e2e", "core");
	/* a keyring file that is not there: an empty keyring */
	stub_keyring = g_build_filename(g_get_home_dir(), "no-such-keyring.json", NULL);

	g_test_add_func("/fe-web/e2e-module/unload-after-request", test_unload_after_request);
	g_test_add_func("/fe-web/e2e-module/unload-after-refresh", test_unload_after_refresh);
	g_test_add_func("/fe-web/e2e-module/unload-nothing-told", test_unload_nothing_told);
	g_test_add_func("/fe-web/e2e-module/action-both-ways", test_action_both_ways);

	ret = g_test_run();

	fe_web_e2e_deinit();
	g_free(stub_keyring);
	g_free(acted_target);
	g_free(acted_handle);
	g_free(acted_fingerprint);
	return ret;
}
