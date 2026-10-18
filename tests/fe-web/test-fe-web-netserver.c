/*
 test-fe-web-netserver.c : network and server edits of web clients

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 network_list and server_list send passwords, and the network's
 autosendcmd (which often holds a NickServ password), as the mask "***".
 A client that edits a network or server sends back what it got (NexusIRC
 does): the mask leaves the stored value as it is.
*/

#include "fe-web-test.h"

#include <irssi/src/core/chatnets.h>
#include <irssi/src/core/servers-setup.h>
#include <irssi/src/irc/core/irc-chatnets.h>
#include <irssi/src/irc/core/irc-servers-setup.h>

#include <string.h>

/* ---- a logged-in client that reads plain text frames ---- */

static WEB_CLIENT_REC *client_new(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	/* text frames, readable here (the AES layer is fe-web-crypto's) */
	c->encryption_enabled = FALSE;
	return c;
}

/* send 'json' as the browser does (a masked text frame); returns what the
 * browser read since the client was made, frame headers (which may hold
 * NUL bytes) as '.'; to be freed */
static char *client_says(WEB_CLIENT_REC *c, const char *json)
{
	GByteArray *f = test_frame(WS_OPCODE_TEXT, json, strlen(json), TRUE);
	const GByteArray *r;
	char *text;
	guint i;

	fe_web_fuzz_client_send(c, f->data, f->len);
	g_byte_array_unref(f);
	g_assert_true(fe_web_fuzz_client_alive(c));

	fe_web_fuzz_read_peer(0);
	r = fe_web_fuzz_received();
	text = g_malloc(r->len + 1);
	for (i = 0; i < r->len; i++)
		text[i] = r->data[i] != '\0' ? (char) r->data[i] : '.';
	text[r->len] = '\0';
	return text;
}

/* one message from a fresh client; returns what it got back (freed) */
static char *say(const char *json)
{
	WEB_CLIENT_REC *c = client_new();
	char *heard = client_says(c, json);

	fe_web_fuzz_client_free(c);
	return heard;
}

static void say_quiet(const char *json)
{
	g_free(say(json));
}

/* network_list's entry for 'name', as JSON (to be freed) */
static char *network_json(const char *name)
{
	IRC_CHATNET_REC *rec = irc_chatnet_find(name);
	GString *json;

	g_assert_nonnull(rec);
	json = fe_web_build_network_json(rec);
	return g_string_free(json, FALSE);
}

static void teardown(void)
{
	fe_web_fuzz_reset_setup();
}

/* ---- the masks ---- */

static void test_sasl_password_mask_keeps_value(void)
{
	IRC_CHATNET_REC *rec;
	char *json;

	say_quiet("{\"type\":\"network_add\",\"id\":\"1\",\"name\":\"MaskNet\","
	          "\"sasl_mechanism\":\"PLAIN\",\"sasl_username\":\"me\","
	          "\"sasl_password\":\"s3cret\"}");
	rec = irc_chatnet_find("MaskNet");
	g_assert_nonnull(rec);
	g_assert_cmpstr(rec->sasl_password, ==, "s3cret");

	json = network_json("MaskNet");
	g_assert_nonnull(strstr(json, "\"sasl_password\":\"***\""));
	g_assert_null(strstr(json, "s3cret"));
	g_free(json);

	/* the edit form sends back what network_list gave it */
	say_quiet("{\"type\":\"network_add\",\"id\":\"2\",\"name\":\"MaskNet\","
	          "\"nick\":\"newnick\",\"sasl_mechanism\":\"PLAIN\","
	          "\"sasl_username\":\"me\",\"sasl_password\":\"***\"}");
	rec = irc_chatnet_find("MaskNet");
	g_assert_cmpstr(rec->nick, ==, "newnick");
	g_assert_cmpstr(rec->sasl_password, ==, "s3cret");

	/* a new password is taken */
	say_quiet("{\"type\":\"network_add\",\"id\":\"3\",\"name\":\"MaskNet\","
	          "\"sasl_password\":\"other\"}");
	g_assert_cmpstr(irc_chatnet_find("MaskNet")->sasl_password, ==, "other");
	teardown();
}

static void test_autosendcmd_masked(void)
{
	IRC_CHATNET_REC *rec;
	char *json;

	say_quiet("{\"type\":\"network_add\",\"id\":\"1\",\"name\":\"CmdNet\","
	          "\"autosendcmd\":\"/msg NickServ identify hunter2\"}");
	json = network_json("CmdNet");
	g_assert_nonnull(strstr(json, "\"autosendcmd\":\"***\""));
	g_assert_null(strstr(json, "hunter2"));
	g_free(json);

	/* the mask sent back keeps it */
	say_quiet("{\"type\":\"network_add\",\"id\":\"2\",\"name\":\"CmdNet\","
	          "\"autosendcmd\":\"***\"}");
	rec = irc_chatnet_find("CmdNet");
	g_assert_cmpstr(rec->autosendcmd, ==, "/msg NickServ identify hunter2");

	/* a new one is taken */
	say_quiet("{\"type\":\"network_add\",\"id\":\"3\",\"name\":\"CmdNet\","
	          "\"autosendcmd\":\"/join #x\"}");
	g_assert_cmpstr(irc_chatnet_find("CmdNet")->autosendcmd, ==, "/join #x");

	/* none is null, not the mask: a client shows an empty field */
	say_quiet("{\"type\":\"network_add\",\"id\":\"4\",\"name\":\"NoCmdNet\"}");
	json = network_json("NoCmdNet");
	g_assert_nonnull(strstr(json, "\"autosendcmd\":null"));
	g_free(json);
	teardown();
}

static void test_server_password_mask_keeps_value(void)
{
	SERVER_SETUP_REC *rec;

	say_quiet("{\"type\":\"server_add\",\"id\":\"1\",\"address\":\"irc.example.org\","
	          "\"port\":6697,\"chatnet\":\"SrvNet\",\"password\":\"pass1\"}");
	rec = server_setup_find("irc.example.org", 6697, "SrvNet");
	g_assert_nonnull(rec);
	g_assert_cmpstr(rec->password, ==, "pass1");
	say_quiet("{\"type\":\"server_add\",\"id\":\"2\",\"address\":\"irc.example.org\","
	          "\"port\":6697,\"chatnet\":\"SrvNet\",\"password\":\"***\"}");
	g_assert_cmpstr(server_setup_find("irc.example.org", 6697, "SrvNet")->password, ==,
	                "pass1");
	teardown();
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);

	g_test_add_func("/fe-web/netserver/sasl-password-mask-keeps-value",
	                test_sasl_password_mask_keeps_value);
	g_test_add_func("/fe-web/netserver/autosendcmd-masked", test_autosendcmd_masked);
	g_test_add_func("/fe-web/netserver/server-password-mask-keeps-value",
	                test_server_password_mask_keeps_value);

	return g_test_run();
}
