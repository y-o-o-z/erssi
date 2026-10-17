/*
 irc.c : fuzz what fe-web makes of IRC traffic

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The server is fe_web_fuzz_server_new()'s (as in src/fe-fuzz/server.c).
 The input is lines from an IRC server ("\r\n" separated; the first byte
 picks whether they get a ":user " prefix). A web client that follows all
 servers turns them into JSON (messages, modes, nicklists, WHOIS, ...).
 Lines starting with "WEB " are a message from that web client instead
 (sync_server, names, mark_read, close_query, ...), so its requests meet
 real channels, nicks and queries.
*/

#include <irssi/src/fe-web/module.h>
#include <irssi/src/fe-fuzz/fe-web/fe-web-fuzz.h>

#include <irssi/src/core/signals.h>
#include <irssi/src/core/rawlog.h>
#include <irssi/src/core/servers-setup.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>

#include <stdlib.h>
#include <string.h>

static SERVER_REC *server;

/* the end of a login, which irc-servers.c does on 001 */
static void event_connected(IRC_SERVER_REC *irc, const char *data, const char *from)
{
	char *params, *nick;

	params = event_get_params(data, 1, &nick);
	if (g_strcmp0(irc->nick, nick) != 0) {
		g_free(irc->nick);
		irc->nick = g_strdup(nick);
	}
	g_free(irc->real_address);
	irc->real_address = g_strdup(from != NULL ? from : irc->connrec->address);
	irc->connected = 1;
	irc->real_connect_time = time(NULL);
	irc->wait_cmd = g_get_real_time();
	signal_emit("event connected", 1, irc);
	g_free(params);
}

static void server_new(void)
{
	server = SERVER(fe_web_fuzz_server_new("fuzz"));
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void) argc;
	(void) argv;
	fe_web_fuzz_init(TRUE);
	signal_add("event 001", (SIGNAL_FUNC) event_connected);
	rawlog_set_size(1);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	WEB_CLIENT_REC *client;
	gboolean prefixed;
	char *copy;
	char **lines, **line;

	if (size < 1)
		return 0;
	prefixed = data[0] & 1;

	client = fe_web_fuzz_client_new(TRUE);
	client->wants_all_servers = TRUE;
	server_new();

	copy = g_strndup((const char *) data + 1, size - 1);
	lines = g_strsplit(copy, "\r\n", -1);
	for (line = lines; *line != NULL; line++) {
		if (strncmp(*line, "WEB ", 4) == 0) {
			if (fe_web_fuzz_client_alive(client))
				fe_web_client_handle_message(client, *line + 4);
		} else {
			char *irc_line = g_strdup_printf(prefixed ? ":user %s" : "%s", *line);
			gboolean disconnected;

			server_ref(server);
			signal_emit("server incoming", 2, server, irc_line);
			disconnected = server->disconnected;
			server_unref(server);
			if (disconnected)
				server_new();
			g_free(irc_line);
		}
	}
	g_strfreev(lines);
	g_free(copy);

	server_disconnect(server);
	fe_web_fuzz_client_free(client);
	fe_web_fuzz_reset_setup();
	return 0;
}
