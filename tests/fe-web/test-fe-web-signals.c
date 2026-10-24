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
    not from every channel of the server;
  - the JSON is valid UTF-8 also when the server sends other bytes in a
    field irssi does not recode (a WHOIS realname in ISO-8859-2).
*/

#define MODULE_NAME "test-fe-web-signals"

#include "fe-web-test.h"

#include <irssi/src/core/misc.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/lib-config/iconfig.h>
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

/* the payloads of the frames read since 'from' (server frames are not
 * masked), one per line; to be freed */
static char *payloads_since(gsize from)
{
	const GByteArray *r;
	GString *out = g_string_new(NULL);
	gsize pos;

	fe_web_fuzz_read_peer(0);
	r = fe_web_fuzz_received();
	for (pos = from; pos + 2 <= r->len;) {
		guint64 len = r->data[pos + 1] & 0x7f;
		gsize head = 2;
		int i;

		g_assert_cmpuint(r->data[pos + 1] & 0x80, ==, 0);
		if (len == 126) {
			len = (r->data[pos + 2] << 8) | r->data[pos + 3];
			head = 4;
		} else if (len == 127) {
			for (len = 0, i = 0; i < 8; i++)
				len = (len << 8) | r->data[pos + 2 + i];
			head = 10;
		}
		g_assert_cmpuint(pos + head + len, <=, r->len);
		g_string_append_len(out, (const char *) r->data + pos + head, len);
		g_string_append_c(out, '\n');
		pos += head + len;
	}
	g_assert_cmpuint(pos, ==, r->len);
	return g_string_free(out, FALSE);
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

/* a realname in ISO-8859-2 (Polish networks): no invalid UTF-8 reaches
 * the client - read in recode_fallback, as irssi reads a message that is
 * no UTF-8 */
static void test_json_is_utf8(void)
{
	IRC_SERVER_REC *server = connected();
	WEB_CLIENT_REC *c = client_new();
	gsize mark;
	char *heard, *escaped, *fallback = g_strdup(settings_get_str("recode_fallback"));

	settings_set_str("recode_fallback", "ISO-8859-2");
	mark = heard_len();
	server_says(server, ":irc.example.net 311 me bob b example.net * :Kr\xf3l \xb3\xf3" "d\xbc");
	server_says(server, ":irc.example.net 318 me bob :End of /WHOIS list.");
	heard = payloads_since(mark);
	g_assert_nonnull(strstr(heard, "\"type\":\"whois\""));
	g_assert_true(g_utf8_validate(heard, -1, NULL));
	g_assert_nonnull(strstr(heard, "\"realname\":\"Kr\xc3\xb3l \xc5\x82\xc3\xb3""d\xc5\xba\""));
	g_free(heard);

	/* the escape itself: valid text is kept as it is */
	escaped = fe_web_escape_json("\xc5\xbc\x61\x62\x61 \"x\"");
	g_assert_cmpstr(escaped, ==, "\xc5\xbc\x61\x62\x61 \\\"x\\\"");
	g_free(escaped);
	/* a byte no charset reads: U+FFFD */
	settings_set_str("recode_fallback", "UTF-8");
	escaped = fe_web_escape_json("a\xff" "b");
	g_assert_cmpstr(escaped, ==, "a\xef\xbf\xbd" "b");
	g_free(escaped);
	settings_set_str("recode_fallback", fallback);
	g_free(fallback);

	fe_web_fuzz_client_free(c);
	fe_web_fuzz_server_free(server);
}

/* recode_fallback CP1252 (irssi's default) and "/RECODE ADD fuzz ISO-8859-2":
 * the realname is read as irssi reads that server's lines, in ISO-8859-2
 * (CP1252 made "\xb3" a "³" where it is a "ł") */
static void test_json_follows_recode(void)
{
	IRC_SERVER_REC *server = connected();
	WEB_CLIENT_REC *c = client_new();
	gsize mark;
	char *heard, *fallback = g_strdup(settings_get_str("recode_fallback"));

	settings_set_str("recode_fallback", "CP1252");
	iconfig_set_str("conversions", "fuzz", "ISO-8859-2");
	mark = heard_len();
	server_says(server, ":irc.example.net 311 me bob b example.net * :Kr\xf3l \xb3\xf3" "d\xbc");
	server_says(server, ":irc.example.net 318 me bob :End of /WHOIS list.");
	heard = payloads_since(mark);
	g_assert_true(g_utf8_validate(heard, -1, NULL));
	g_assert_nonnull(strstr(heard, "\"realname\":\"Kr\xc3\xb3l \xc5\x82\xc3\xb3""d\xc5\xba\""));
	g_free(heard);
	iconfig_set_str("conversions", "fuzz", NULL);
	settings_set_str("recode_fallback", fallback);
	g_free(fallback);

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
	g_test_add_func("/fe-web/signals/json-is-utf8", test_json_is_utf8);
	g_test_add_func("/fe-web/signals/json-follows-recode", test_json_follows_recode);

	ret = g_test_run();

	signal_remove("event 001", (SIGNAL_FUNC) event_connected);
	fe_web_signals_deinit();
	return ret;
}
