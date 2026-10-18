/*
 test-fe-web-netserver.c : network and server edits of web clients

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 network_list and server_list send passwords, and the network's
 autosendcmd (which often holds a NickServ password), as the mask "***".
 A client that edits a network or server sends back what it got (NexusIRC
 does): the mask leaves the stored value as it is. An edit is written to
 the config file at once, so that it is there after a restart without
 /SAVE - when irssi's own autosave would write it.
*/

#include "fe-web-test.h"

#include <irssi/src/core/chatnets.h>
#include <irssi/src/core/servers-setup.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/irc/core/irc-chatnets.h>
#include <irssi/src/irc/core/irc-servers-setup.h>
#include <irssi/src/lib-config/iconfig.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

/* The config file as a restart reads it: a value of it, or NULL (freed) */
static char *saved_value(const char *section, const char *key)
{
	CONFIG_REC *config;
	char *value;

	if (!g_file_test(mainconfig->fname, G_FILE_TEST_IS_REGULAR))
		return NULL;
	config = config_open(mainconfig->fname, -1);
	g_assert_nonnull(config);
	g_assert_cmpint(config_parse(config), ==, 0);
	value = g_strdup(config_get_str(config, section, key, NULL));
	config_close(config);
	return value;
}

/* the value of 'key' of the first server of the file with this address */
static char *saved_server_value(const char *address, const char *key)
{
	CONFIG_REC *config;
	CONFIG_NODE *servers;
	GSList *tmp;
	char *value = NULL;

	if (!g_file_test(mainconfig->fname, G_FILE_TEST_IS_REGULAR))
		return NULL;
	config = config_open(mainconfig->fname, -1);
	g_assert_nonnull(config);
	g_assert_cmpint(config_parse(config), ==, 0);
	servers = config_node_traverse(config, "servers", FALSE);
	for (tmp = servers != NULL ? config_node_first(servers->value) : NULL; tmp != NULL;
	     tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;

		if (g_strcmp0(config_node_get_str(node, "address", NULL), address) == 0) {
			value = g_strdup(config_node_get_str(node, key, NULL));
			break;
		}
	}
	config_close(config);
	return value;
}

static void assert_saved(const char *section, const char *key, const char *expected)
{
	char *value = saved_value(section, key);

	g_assert_cmpstr(value, ==, expected);
	g_free(value);
}

static void teardown(void)
{
	fe_web_fuzz_reset_setup();
	settings_set_bool("settings_autosave", FALSE);
	unlink(mainconfig->fname);
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

/* Another port of a known address is another server: added, and the
 * first one keeps its settings; a remove of a port there is none of
 * removes nothing */
static void test_server_other_port(void)
{
	SERVER_SETUP_REC *plain, *tls;
	char *heard;

	say_quiet("{\"type\":\"server_add\",\"id\":\"1\",\"address\":\"irc.p.example\","
	          "\"port\":6667,\"chatnet\":\"PNet\"}");
	heard = say("{\"type\":\"server_add\",\"id\":\"2\",\"address\":\"irc.p.example\","
	            "\"port\":6697,\"chatnet\":\"PNet\",\"use_tls\":1}");
	g_assert_nonnull(strstr(heard, "Server 'irc.p.example:6697' added successfully"));
	g_free(heard);
	g_assert_cmpuint(g_slist_length(setupservers), ==, 2);
	plain = server_setup_find("irc.p.example", 6667, "PNet");
	tls = server_setup_find("irc.p.example", 6697, "PNet");
	g_assert_nonnull(plain);
	g_assert_nonnull(tls);
	g_assert_true(plain != tls);
	g_assert_cmpint(plain->port, ==, 6667);
	g_assert_false(plain->use_tls);
	g_assert_cmpint(tls->port, ==, 6697);
	g_assert_true(tls->use_tls);

	heard = say("{\"type\":\"server_remove\",\"id\":\"3\",\"address\":\"irc.p.example\","
	            "\"port\":7000,\"chatnet\":\"PNet\"}");
	g_assert_nonnull(strstr(heard, "SERVER_NOT_FOUND"));
	g_free(heard);
	g_assert_cmpuint(g_slist_length(setupservers), ==, 2);
	teardown();
}

/* ---- saving ---- */

/* each edit is in the file at once, with settings_autosave ON */
static void test_edits_saved(void)
{
	char *heard, *value;

	settings_set_bool("settings_autosave", TRUE);
	unlink(mainconfig->fname);

	heard = say("{\"type\":\"network_add\",\"id\":\"1\",\"name\":\"SaveNet\","
	            "\"nick\":\"saver\",\"sasl_mechanism\":\"PLAIN\",\"sasl_username\":\"me\","
	            "\"sasl_password\":\"s3cret\",\"autosendcmd\":\"/msg NickServ id x\"}");
	g_assert_nonnull(strstr(heard, "Network 'SaveNet' added successfully\""));
	g_free(heard);
	assert_saved("chatnets/SaveNet", "nick", "saver");
	assert_saved("chatnets/SaveNet", "autosendcmd", "/msg NickServ id x");
	assert_saved("chatnets/SaveNet", "sasl_password", "s3cret");

	/* an edit with the masks sent back */
	say_quiet("{\"type\":\"network_add\",\"id\":\"2\",\"name\":\"SaveNet\","
	          "\"nick\":\"saver2\",\"sasl_password\":\"***\",\"autosendcmd\":\"***\"}");
	assert_saved("chatnets/SaveNet", "nick", "saver2");
	assert_saved("chatnets/SaveNet", "autosendcmd", "/msg NickServ id x");
	assert_saved("chatnets/SaveNet", "sasl_password", "s3cret");

	/* a server, then a change of it */
	say_quiet("{\"type\":\"server_add\",\"id\":\"3\",\"address\":\"irc.save.example\","
	          "\"port\":6697,\"chatnet\":\"SaveNet\",\"use_tls\":0}");
	value = saved_server_value("irc.save.example", "chatnet");
	g_assert_cmpstr(value, ==, "SaveNet");
	g_free(value);
	value = saved_server_value("irc.save.example", "use_tls");
	g_assert_true(value == NULL || g_ascii_strcasecmp(value, "no") == 0);
	g_free(value);
	say_quiet("{\"type\":\"server_add\",\"id\":\"4\",\"address\":\"irc.save.example\","
	          "\"port\":6697,\"chatnet\":\"SaveNet\",\"use_tls\":1}");
	value = saved_server_value("irc.save.example", "use_tls");
	g_assert_cmpstr(value, ==, "yes");
	g_free(value);

	/* and removals */
	say_quiet("{\"type\":\"server_remove\",\"id\":\"5\",\"address\":\"irc.save.example\","
	          "\"port\":6697,\"chatnet\":\"SaveNet\"}");
	value = saved_server_value("irc.save.example", "chatnet");
	g_assert_null(value);
	say_quiet("{\"type\":\"network_remove\",\"id\":\"6\",\"name\":\"SaveNet\"}");
	assert_saved("chatnets/SaveNet", "nick", NULL);
	teardown();
}

/* settings_autosave OFF: in memory only, and the result says so */
static void test_autosave_off_not_saved(void)
{
	char *heard;

	settings_set_bool("settings_autosave", FALSE);
	unlink(mainconfig->fname);
	heard = say("{\"type\":\"network_add\",\"id\":\"1\",\"name\":\"OffNet\"}");
	g_assert_nonnull(strstr(heard, "Network 'OffNet' added successfully; not saved: "
	                               "settings_autosave is OFF; /SAVE writes it"));
	g_free(heard);
	g_assert_nonnull(irc_chatnet_find("OffNet"));
	g_assert_false(g_file_test(mainconfig->fname, G_FILE_TEST_EXISTS));
	teardown();
}

/* a config file changed outside erssi is not overwritten */
static void test_changed_file_not_overwritten(void)
{
	static const char outside[] = "# edited by hand\nchatnets = { HandNet = { }; };\n";
	char *heard, *data;
	struct stat st;

	settings_set_bool("settings_autosave", TRUE);
	unlink(mainconfig->fname);
	say_quiet("{\"type\":\"network_add\",\"id\":\"1\",\"name\":\"FirstNet\"}");
	g_assert_true(g_file_test(mainconfig->fname, G_FILE_TEST_IS_REGULAR));

	/* a modification time other than the one erssi saw */
	g_assert_true(g_file_set_contents(mainconfig->fname, outside, -1, NULL));
	g_assert_cmpint(stat(mainconfig->fname, &st), ==, 0);
	{
		struct timespec times[2] = { { st.st_mtime + 10, 0 }, { st.st_mtime + 10, 0 } };

		g_assert_cmpint(utimensat(AT_FDCWD, mainconfig->fname, times, 0), ==, 0);
	}

	heard = say("{\"type\":\"network_add\",\"id\":\"2\",\"name\":\"SecondNet\"}");
	g_assert_nonnull(strstr(heard, "not saved: the config file was changed outside "
	                               "erssi; /SAVE writes it"));
	g_free(heard);
	g_assert_true(g_file_get_contents(mainconfig->fname, &data, NULL, NULL));
	g_assert_cmpstr(data, ==, outside);
	g_free(data);
	teardown();
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);
	g_assert_nonnull(mainconfig->fname);

	g_test_add_func("/fe-web/netserver/sasl-password-mask-keeps-value",
	                test_sasl_password_mask_keeps_value);
	g_test_add_func("/fe-web/netserver/autosendcmd-masked", test_autosendcmd_masked);
	g_test_add_func("/fe-web/netserver/server-password-mask-keeps-value",
	                test_server_password_mask_keeps_value);
	g_test_add_func("/fe-web/netserver/server-other-port", test_server_other_port);
	g_test_add_func("/fe-web/netserver/edits-saved", test_edits_saved);
	g_test_add_func("/fe-web/netserver/autosave-off-not-saved", test_autosave_off_not_saved);
	g_test_add_func("/fe-web/netserver/changed-file-not-overwritten",
	                test_changed_file_not_overwritten);

	return g_test_run();
}
