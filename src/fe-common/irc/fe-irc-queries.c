/*
 fe-irc-queries.c : irssi

    Copyright (C) 2000 Timo Sirainen

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "module.h"
#include <irssi/src/core/signals.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/queries.h>
#include <irssi/src/core/nicklist.h>
#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/irc-queries.h>
#include <irssi/src/irc/core/irc-chatnets.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/fe-common/core/window-items.h>

int query_type;

static QUERY_REC *query_find_address(SERVER_REC *server, const char *address)
{
	GSList *tmp;

	g_return_val_if_fail(IS_SERVER(server), NULL);

	for (tmp = server->queries; tmp != NULL; tmp = tmp->next) {
		QUERY_REC *rec = tmp->data;

		if (*rec->name != '=' && rec->address != NULL &&
		    g_ascii_strcasecmp(address, rec->address) == 0)
			return rec;
	}

	return NULL;
}

static int server_has_nick(SERVER_REC *server, const char *nick)
{
	GSList *tmp;

	for (tmp = server->channels; tmp != NULL; tmp = tmp->next) {
		CHANNEL_REC *channel = tmp->data;

		if (nicklist_find(channel, nick) != NULL)
			return TRUE;
	}

	return FALSE;
}

static void event_privmsg(SERVER_REC *server, const char *data,
			  const char *nick, const char *address)
{
	QUERY_REC *query;

	g_return_if_fail(data != NULL);

	if (nick == NULL || address == NULL || server_ischannel(server, data) ||
	    !settings_get_bool("query_track_nick_changes"))
                return;

	query = query_find(server, nick);
	if (query == NULL) {
		/* check if there's query with another nick from the same
		   address. it was probably a nick change or reconnect to
		   server, so rename the query. */
		query = query_find_address(server, address);
		if (query != NULL) {
			/* make sure the old nick doesn't exist anymore */
			if (!server_has_nick(server, query->name))
				query_change_nick(query, nick);
		}
	} else {
		/* process the changes to the query structure now, before the
		 * privmsg is dispatched. */
		if (g_strcmp0(query->name, nick) != 0)
			query_change_nick(query, nick);
		if (address != NULL && g_strcmp0(query->address, address) != 0)
			query_change_address(query, address);
	}
}

static void sig_window_bound_query(SERVER_REC *server)
{
	GSList *wtmp, *btmp, *bounds;

	if (!IS_IRC_SERVER(server))
		return;

	for (wtmp = windows; wtmp != NULL; wtmp = wtmp->next) {
		WINDOW_REC *win = wtmp->data;
		bounds = g_slist_copy(win->bound_items);

		for (btmp = bounds; btmp != NULL; btmp = btmp->next) {
			WINDOW_BIND_REC *bound = btmp->data;

			if (bound->type == query_type &&
			    g_strcmp0(server->tag, bound->servertag) == 0) {
				irc_query_create(bound->servertag, bound->name, TRUE);
			}
		}

		g_slist_free(bounds);
	}
}

/* the network window of SERVER, made if it has none: named after the tag
   and sticky to the server. NULL when another window has that name. */
static WINDOW_REC *network_window(SERVER_REC *server)
{
	WINDOW_REC *window;

	window = window_find_network(server->tag);
	if (window != NULL || window_find_name(server->tag) != NULL)
		return window;

	/* as /WINDOW NEW HIDE, without its messages */
	signal_emit("gui window create override", 1, GINT_TO_POINTER(MAIN_WINDOW_TYPE_HIDDEN));
	window = window_create(NULL, FALSE);
	window_set_name(window, server->tag);
	window_change_server(window, server);
	g_free(window->servertag);
	window->servertag = g_strdup(server->tag);
	return window;
}

/* one_window in the network's block: a query opens in the network window,
   not in a window of its own (fe-queries.c leaves a query that already has
   a window alone) */
static void sig_query_created(QUERY_REC *query, gpointer automatic)
{
	IRC_CHATNET_REC *chatnet;
	WINDOW_REC *window;

	if (!IS_IRC_SERVER(query->server) || window_item_window(query) != NULL ||
	    query->server->connrec->chatnet == NULL)
		return;

	chatnet = ircnet_find(query->server->connrec->chatnet);
	if (chatnet == NULL || !chatnet->one_window)
		return;

	window = network_window(query->server);
	if (window != NULL)
		window_item_add(window, (WI_ITEM_REC *) query, GPOINTER_TO_INT(automatic));
}

void fe_irc_queries_init(void)
{
	query_type = module_get_uniq_id_str("WINDOW ITEM TYPE", "QUERY");

	settings_add_bool("lookandfeel", "query_track_nick_changes", TRUE);

	signal_add("server connected", sig_window_bound_query);
	signal_add_first("event privmsg", (SIGNAL_FUNC) event_privmsg);
	signal_add_first("query created", (SIGNAL_FUNC) sig_query_created);
}

void fe_irc_queries_deinit(void)
{
	signal_remove("server connected", sig_window_bound_query);
	signal_remove("event privmsg", (SIGNAL_FUNC) event_privmsg);
	signal_remove("query created", (SIGNAL_FUNC) sig_query_created);
}
