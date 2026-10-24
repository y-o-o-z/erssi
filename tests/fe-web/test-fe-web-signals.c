/*
 test-fe-web-signals.c : what fe-web sends web clients for IRC events

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 Lines from a server go through irssi (core, irc, fe-common) into
 fe-web-signals.c, which turns them into JSON for a web client that reads
 plain text frames here:
  - a QUIT removes the nick from the nicklists of the channels it was in,
    not from every channel of the server.
*/

#define MODULE_NAME "test-fe-web-signals"

#include "fe-web-test.h"

#include <irssi/src/core/misc.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>

#include <string.h>

/* as src/fe-fuzz/fe-web/irc.c: the stand-in server is connected at 001 */
static void event_connected(IRC_SERVER_REC *irc, const char *data, const char *from)
{
	char *params, *nick;

	params = event_get_params(data, 1, &nick);
	g_free(irc->nick);
	irc->nick = g_strdup(nick);
	g_free(irc->real_address);
	irc->real_address = g_strdup(from != NULL ? from : irc->connrec->address);
	irc->connected = 1;
	irc->real_connect_time = time(NULL);
	irc->wait_cmd = g_get_real_time();
	signal_emit("event connected", 1, irc);
	g_free(params);
}

/* the line is parsed in place: a copy */
static void server_says(IRC_SERVER_REC *server, const char *line)
{
	char *copy = g_strdup(line);

	signal_emit("server incoming", 2, server, copy);
	g_free(copy);
}

/* a logged-in client of every server, reading text frames */
static WEB_CLIENT_REC *client_new(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	c->encryption_enabled = FALSE;
	c->wants_all_servers = TRUE;
	return c;
}

/* what the browser read since 'from' (frame headers, which may hold NUL
 * bytes, as '.'); to be freed */
static char *heard_since(gsize from)
{
	const GByteArray *r;
	char *text;
	gsize i;

	fe_web_fuzz_read_peer(0);
	r = fe_web_fuzz_received();
	g_assert_cmpuint(from, <=, r->len);
	text = g_malloc(r->len - from + 1);
	for (i = from; i < r->len; i++)
		text[i - from] = r->data[i] != '\0' ? (char) r->data[i] : '.';
	text[r->len - from] = '\0';
	return text;
}

static gsize heard_len(void)
{
	fe_web_fuzz_read_peer(0);
	return fe_web_fuzz_received()->len;
}

static guint count(const char *text, const char *needle)
{
	guint n = 0;

	for (; (text = strstr(text, needle)) != NULL; text += strlen(needle))
		n++;
	return n;
}

static IRC_SERVER_REC *connected(void)
{
	IRC_SERVER_REC *server = fe_web_fuzz_server_new("fuzz");

	server_says(server, ":irc.example.net 001 me :Welcome");
	g_assert_true(server->connected);
	return server;
}

static void join(IRC_SERVER_REC *server, const char *channel, const char *names)
{
	char *line;

	line = g_strdup_printf(":me!u@example.net JOIN %s", channel);
	server_says(server, line);
	g_free(line);
	line = g_strdup_printf(":irc.example.net 353 me = %s :%s", channel, names);
	server_says(server, line);
	g_free(line);
	line = g_strdup_printf(":irc.example.net 366 me %s :End of /NAMES list.", channel);
	server_says(server, line);
	g_free(line);
}

/* bob is in #a and #c of four channels: his QUIT removes him there only */
static void test_quit_removes_where_member(void)
{
	IRC_SERVER_REC *server = connected();
	WEB_CLIENT_REC *c = client_new();
	gsize mark;
	char *heard;

	join(server, "#a", "me bob");
	join(server, "#b", "me carol");
	join(server, "#c", "me @bob");
	join(server, "#d", "me");
	mark = heard_len();
	server_says(server, ":bob!b@example.net QUIT :bye");
	heard = heard_since(mark);
	g_assert_cmpuint(count(heard, "\"type\":\"nicklist_update\""), ==, 2);
	g_assert_nonnull(strstr(heard, "\"channel\":\"#a\",\"nick\":\"bob\""));
	g_assert_nonnull(strstr(heard, "\"channel\":\"#c\",\"nick\":\"bob\""));
	g_assert_null(strstr(heard, "\"channel\":\"#b\""));
	g_assert_null(strstr(heard, "\"channel\":\"#d\""));
	/* the quit itself is still told once */
	g_assert_cmpuint(count(heard, "\"type\":\"user_quit\""), ==, 1);
	g_free(heard);

	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
}

int main(int argc, char **argv)
{
	int ret;

	test_init(&argc, &argv);
	fe_web_signals_init();
	signal_add("event 001", (SIGNAL_FUNC) event_connected);

	g_test_add_func("/fe-web/signals/quit-removes-where-member", test_quit_removes_where_member);

	ret = g_test_run();

	signal_remove("event 001", (SIGNAL_FUNC) event_connected);
	fe_web_signals_deinit();
	return ret;
}
