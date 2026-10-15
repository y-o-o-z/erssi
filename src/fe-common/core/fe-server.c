/*
 fe-server.c : irssi

    Copyright (C) 1999-2001 Timo Sirainen

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
#include <irssi/src/core/commands.h>
#include <irssi/src/core/network.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/settings.h>

#include <irssi/src/core/chat-protocols.h>
#include <irssi/src/core/chatnets.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/servers-setup.h>
#include <irssi/src/core/servers-reconnect.h>
#include <irssi/src/core/queries.h>

#include <irssi/src/fe-common/core/module-formats.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/fe-common/core/fe-server.h>

/* the server or reconnect window_close_disconnects is ending: its own line
   says why, the usual quit and connection lines would only repeat it */
static SERVER_REC *closing_server;
static RECONNECT_REC *closing_reconnect;

static void print_servers(void)
{
	GSList *tmp;

	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		SERVER_REC *rec = tmp->data;

		printformat(NULL, NULL, MSGLEVEL_CRAP, TXT_SERVER_LIST,
			    rec->tag, rec->connrec->address, rec->connrec->port,
			    rec->connrec->chatnet == NULL ? "" : rec->connrec->chatnet, rec->connrec->nick);
	}
}

static void print_lookup_servers(void)
{
	GSList *tmp;
	for (tmp = lookup_servers; tmp != NULL; tmp = tmp->next) {
		SERVER_REC *rec = tmp->data;

		printformat(NULL, NULL, MSGLEVEL_CRAP, TXT_SERVER_LOOKUP_LIST,
			    rec->tag, rec->connrec->address, rec->connrec->port,
			    rec->connrec->chatnet == NULL ? "" : rec->connrec->chatnet, rec->connrec->nick);
	}
}

static void print_reconnects(void)
{
	GSList *tmp;
	char *tag, *next_connect;
	int left;

	for (tmp = reconnects; tmp != NULL; tmp = tmp->next) {
		RECONNECT_REC *rec = tmp->data;
		SERVER_CONNECT_REC *conn = rec->conn;

		tag = g_strdup_printf("RECON-%d", rec->tag);
		left = rec->next_connect-time(NULL);
		next_connect = g_strdup_printf("%02d:%02d", left/60, left%60);
		printformat(NULL, NULL, MSGLEVEL_CRAP, TXT_SERVER_RECONNECT_LIST,
			    tag, conn->address, conn->port,
			    conn->chatnet == NULL ? "" : conn->chatnet,
			    conn->nick, next_connect);
		g_free(next_connect);
		g_free(tag);
	}
}

static SERVER_SETUP_REC *create_server_setup(GHashTable *optlist)
{
	CHAT_PROTOCOL_REC *rec;
        SERVER_SETUP_REC *server;
        char *chatnet;

	rec = chat_protocol_find_net(optlist);
	if (rec == NULL)
                rec = chat_protocol_get_default();
	else {
		chatnet = g_hash_table_lookup(optlist, "network");
		if (chatnet == NULL && g_hash_table_lookup(optlist, rec->chatnet) != NULL)
			chatnet = rec->chatnet;
		if (chatnet_find(chatnet) == NULL) {
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
				    TXT_UNKNOWN_CHATNET, chatnet);
			return NULL;
		}
	}

	if (rec == NULL) {
		/* no protocols loaded, bail out */
		signal_emit("chat protocol unknown", 1, "(none)");
		return NULL;
	}

	server = rec->create_server_setup();
	server->chat_type = rec->id;
	server->tls_verify = TRUE;
	return server;
}

static void cmd_server_add_modify(const char *data, gboolean add)
{
        GHashTable *optlist;
	SERVER_SETUP_REC *rec, *tmp;
	char *addr, *portstr, *password, *value, *chatnet, *old_chatnet;
	void *free_arg;
	gboolean newrec;
	int port, old_port, add_port;

	if (!cmd_get_params(data, &free_arg, 3 | PARAM_FLAG_OPTIONS,
		"server add", &optlist, &addr, &portstr, &password))
		return;

	if (*addr == '\0') cmd_param_error(CMDERR_NOT_ENOUGH_PARAMS);

	port = old_port = -1;

	value = g_hash_table_lookup(optlist, "port");
	if (value != NULL && *value != '\0')
		port = add_port = atoi(value);
	else if (g_hash_table_lookup(optlist, "tls") ||
		 g_hash_table_lookup(optlist, "ssl"))
		add_port = DEFAULT_SERVER_ADD_TLS_PORT;
	else
		add_port = DEFAULT_SERVER_ADD_PORT;

	if (*portstr != '\0')
		old_port = atoi(portstr);

	chatnet = g_hash_table_lookup(optlist, "network");

	rec = server_setup_find(addr, old_port != -1 ? old_port : add_port, chatnet);
	if (old_port == -1 && rec != NULL)
		old_port = rec->port;

	if (port == -1)
		port = old_port != -1 ? old_port : add_port;

	/* make sure the new port doesn't exist */
	tmp = server_setup_find(addr, port, chatnet);
	if (tmp != NULL && tmp->port == port)
		rec = tmp;

	if (rec == NULL || (rec->port != old_port && rec->port != port)) {
		newrec = TRUE;
		if (add == FALSE) {
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_SETUPSERVER_NOT_FOUND,
			            addr, old_port == -1 ? port : old_port);
			cmd_params_free(free_arg);
			return;
		}

		rec = create_server_setup(optlist);
		if (rec == NULL) {
			cmd_params_free(free_arg);
			return;
		}
		rec->address = g_strdup(addr);
		rec->port = port;
	} else {
		newrec = FALSE;
		old_chatnet = g_strdup(rec->chatnet);
		old_port = rec->port;
		rec->port = port;

		if (*password != '\0') g_free_and_null(rec->password);
		if (g_hash_table_lookup(optlist, "host")) {
			g_free_and_null(rec->own_host);
			rec->own_ip4 = rec->own_ip6 = NULL;
		}
	}

	if (g_hash_table_lookup(optlist, "6"))
		rec->family = AF_INET6;
        else if (g_hash_table_lookup(optlist, "4"))
		rec->family = AF_INET;

	value = g_hash_table_lookup(optlist, "tls_cert");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_cert");
	if (value != NULL && *value != '\0') {
		rec->tls_cert = g_strdup(value);
		if (newrec) {
			/* convenience and backward compatibility, turn on tls if tls_cert is given
			 */
			rec->use_tls = TRUE;
		}
	}

	value = g_hash_table_lookup(optlist, "tls_pkey");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_pkey");
	if (value != NULL && *value != '\0')
		rec->tls_pkey = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_pass");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_pass");
	if (value != NULL && *value != '\0')
		rec->tls_pass = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_cafile");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_cafile");
	if (value != NULL && *value != '\0')
		rec->tls_cafile = g_strdup(value);
	else if (value != NULL && *value == '\0')
		g_free_and_null(rec->tls_cafile);

	value = g_hash_table_lookup(optlist, "tls_capath");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_capath");
	if (value != NULL && *value != '\0')
		rec->tls_capath = g_strdup(value);
	else if (value != NULL && *value == '\0')
		g_free_and_null(rec->tls_capath);

	value = g_hash_table_lookup(optlist, "tls_ciphers");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_ciphers");
	if (value != NULL && *value != '\0')
		rec->tls_ciphers = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_pinned_cert");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_pinned_cert");
	if (value != NULL && *value != '\0')
		rec->tls_pinned_cert = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_pinned_pubkey");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_pinned_pubkey");
	if (value != NULL && *value != '\0')
		rec->tls_pinned_pubkey = g_strdup(value);

	if ((rec->tls_cafile != NULL && rec->tls_cafile[0] != '\0')
	||  (rec->tls_capath != NULL && rec->tls_capath[0] != '\0'))
		rec->tls_verify = TRUE;

	if (g_hash_table_lookup(optlist, "tls_verify") ||
	    g_hash_table_lookup(optlist, "ssl_verify")) {
		rec->tls_verify = TRUE;
		if (newrec) {
			/* convenience and backward compatibility, turn on tls if tls_verify is
			 * given */
			rec->use_tls = TRUE;
		}
	} else if (g_hash_table_lookup(optlist, "notls_verify") ||
	           g_hash_table_lookup(optlist, "nossl_verify")) {
		rec->tls_verify = FALSE;
	}

	if (g_hash_table_lookup(optlist, "tls") || g_hash_table_lookup(optlist, "ssl"))
		rec->use_tls = TRUE;
	else if (g_hash_table_lookup(optlist, "notls") || g_hash_table_lookup(optlist, "nossl"))
		rec->use_tls = FALSE;

	if (g_hash_table_lookup(optlist, "auto")) rec->autoconnect = TRUE;
	if (g_hash_table_lookup(optlist, "noauto")) rec->autoconnect = FALSE;
	if (g_hash_table_lookup(optlist, "proxy")) rec->no_proxy = FALSE;
	if (g_hash_table_lookup(optlist, "noproxy")) rec->no_proxy = TRUE;

	if (*password != '\0' && g_strcmp0(password, "-") != 0) rec->password = g_strdup(password);
	value = g_hash_table_lookup(optlist, "host");
	if (value != NULL && *value != '\0') {
		rec->own_host = g_strdup(value);
		rec->own_ip4 = rec->own_ip6 = NULL;
	}

	signal_emit("server add fill", 3, rec, optlist, GINT_TO_POINTER(add));

	if (newrec) {
		server_setup_add(rec);
	} else {
		server_setup_modify(rec, old_port, old_chatnet);
		g_free(old_chatnet);
	}

	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_SETUPSERVER_ADDED, addr, port);

	cmd_params_free(free_arg);
}

static void cmd_server_add(const char *data)
{
	cmd_server_add_modify(data, TRUE);
}

static void cmd_server_modify(const char *data)
{
	cmd_server_add_modify(data, FALSE);
}

/* SYNTAX: SERVER REMOVE <address> [<port>] [<network>] */
static void cmd_server_remove(const char *data)
{
	SERVER_SETUP_REC *rec;
	char *addr, *port, *chatnet;
	void *free_arg;
	int portnum;

	if (!cmd_get_params(data, &free_arg, 3, &addr, &port, &chatnet))
		return;
	if (*addr == '\0') cmd_param_error(CMDERR_NOT_ENOUGH_PARAMS);

	if (*port == '\0') {
		portnum = DEFAULT_SERVER_ADD_PORT;
		if (*chatnet == '\0')
			rec = server_setup_find(addr, -1, NULL);
		else
			rec = server_setup_find(addr, -1, chatnet);
	}
	else
	{
		portnum = atoi(port);
		if (*chatnet == '\0')
			rec = server_setup_find(addr, portnum, NULL);
		else
			rec = server_setup_find(addr, portnum, chatnet);
	}

	if (rec == NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_SETUPSERVER_NOT_FOUND, addr,
		            portnum);
	else {
		portnum = rec->port;
		server_setup_remove(rec);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_SETUPSERVER_REMOVED, addr,
		            portnum);
	}

	cmd_params_free(free_arg);
}

static void cmd_server(const char *data)
{
	if (*data != '\0')
		return;

	if (servers == NULL && lookup_servers == NULL &&
	    reconnects == NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
			    TXT_NO_CONNECTED_SERVERS);
	} else {
		print_servers();
		print_lookup_servers();
		print_reconnects();
	}

        signal_stop();
}

static void cmd_server_connect(const char *data)
{
	GHashTable *optlist;
	char *addr;
	void *free_arg;

	if (!cmd_get_params(data, &free_arg, 1 | PARAM_FLAG_OPTIONS,
			    "connect", &optlist, &addr))
		return;

	if (*addr == '\0' || g_strcmp0(addr, "+") == 0)
		cmd_param_error(CMDERR_NOT_ENOUGH_PARAMS);
	if (*addr == '+') window_create(NULL, FALSE);

	cmd_params_free(free_arg);
}

static void server_command(const char *data, SERVER_REC *server,
			   WI_ITEM_REC *item)
{
	if (server == NULL) {
		/* this command accepts non-connected server too */
		server = active_win->connect_server;
	}

	signal_continue(3, data, server, item);
}

static void sig_server_looking(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_LOOKING_UP, server->connrec->address);
}

static void sig_server_connecting(SERVER_REC *server, IPADDR *ip)
{
	char ipaddr[MAX_IP_LEN];

	g_return_if_fail(server != NULL);

	if (ip == NULL)
		ipaddr[0] = '\0';
	else
		net_ip2host(ip, ipaddr);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    !server->connrec->reconnecting ?
		    TXT_CONNECTING : TXT_RECONNECTING,
		    server->connrec->address, ipaddr, server->connrec->port);
}

static void sig_server_connected(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_CONNECTION_ESTABLISHED, server->connrec->address);
}

static void sig_connect_failed(SERVER_REC *server, gchar *msg)
{
	g_return_if_fail(server != NULL);

	if (msg == NULL) {
		/* no message so this wasn't unexpected fail - send
		   connection_lost message instead */
		if (server == closing_server)
			return;
		printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
			    TXT_CONNECTION_LOST, server->connrec->address);
	} else {
		printformat(server, NULL, MSGLEVEL_CLIENTERROR,
			    TXT_CANT_CONNECT, server->connrec->address, server->connrec->port, msg);
	}
}

static void sig_server_disconnected(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	if (server == closing_server)
		return;
	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_CONNECTION_LOST, server->connrec->address);
}

static void sig_server_quit(SERVER_REC *server, const char *msg)
{
	g_return_if_fail(server != NULL);

	if (server == closing_server)
		return;
	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_SERVER_QUIT, server->connrec->address, msg);
}

static void sig_server_lag_disconnected(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_LAG_DISCONNECTED, server->connrec->address,
		    time(NULL)-(server->lag_sent / G_TIME_SPAN_SECOND));
}

static void sig_server_reconnect_removed(RECONNECT_REC *reconnect)
{
	g_return_if_fail(reconnect != NULL);

	if (reconnect == closing_reconnect)
		return;
	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_RECONNECT_REMOVED, reconnect->conn->address, reconnect->conn->port,
		    reconnect->conn->chatnet == NULL ? "" : reconnect->conn->chatnet);
}

static void sig_server_reconnect_not_found(const char *tag)
{
	g_return_if_fail(tag != NULL);

	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_RECONNECT_NOT_FOUND, tag);
}

static void sig_chat_protocol_unknown(const char *protocol)
{
	g_return_if_fail(protocol != NULL);

	printformat(NULL, NULL, MSGLEVEL_CLIENTERROR,
                    TXT_UNKNOWN_CHAT_PROTOCOL, protocol);
}

static gboolean tag_is(const char *servertag, const char *tag)
{
	return servertag != NULL && g_ascii_strcasecmp(servertag, tag) == 0;
}

static const char *window_item_server_tag(WI_ITEM_REC *item)
{
	if (item->server != NULL)
		return item->server->tag;
	/* a query keeps its server's tag while the server is away */
	if (IS_QUERY(item))
		return QUERY(item)->server_tag;
	return NULL;
}

/* The window shows the server TAG: it is the server's own window (sticky to
   it, as a network window is), holds a channel or query of it, is kept for
   one (bound to it: waiting to rejoin after a reconnect or a kick), or is a
   window of no particular server that shows messages (a level) and has it
   as its server - the status window. A level-0 window that only happens to
   have the server as its active one shows nothing of it. */
static gboolean window_shows_server(WINDOW_REC *window, const char *tag)
{
	GSList *tmp;

	if (tag_is(window->servertag, tag))
		return TRUE;
	if (window->servertag == NULL && window->level != 0 &&
	    ((window->active_server != NULL && tag_is(window->active_server->tag, tag)) ||
	     (window->connect_server != NULL && tag_is(window->connect_server->tag, tag))))
		return TRUE;
	for (tmp = window->items; tmp != NULL; tmp = tmp->next) {
		if (tag_is(window_item_server_tag(tmp->data), tag))
			return TRUE;
	}
	for (tmp = window->bound_items; tmp != NULL; tmp = tmp->next) {
		WINDOW_BIND_REC *bind = tmp->data;

		if (tag_is(bind->servertag, tag))
			return TRUE;
	}
	return FALSE;
}

static GSList *tags_add(GSList *tags, const char *tag)
{
	GSList *tmp;

	if (tag == NULL || *tag == '\0')
		return tags;
	for (tmp = tags; tmp != NULL; tmp = tmp->next) {
		if (g_ascii_strcasecmp(tmp->data, tag) == 0)
			return tags;
	}
	return g_slist_prepend(tags, g_strdup(tag));
}

GSList *fe_server_window_tags(WINDOW_REC *window, GSList *tags)
{
	GSList *tmp;

	g_return_val_if_fail(window != NULL, tags);

	if (!settings_get_bool("window_close_disconnects"))
		return tags;

	/* the same servers window_shows_server() finds; "*" is no server */
	if (window->servertag != NULL && g_strcmp0(window->servertag, "*") != 0)
		tags = tags_add(tags, window->servertag);
	if (window->servertag == NULL && window->level != 0) {
		if (window->active_server != NULL)
			tags = tags_add(tags, window->active_server->tag);
		if (window->connect_server != NULL)
			tags = tags_add(tags, window->connect_server->tag);
	}
	for (tmp = window->items; tmp != NULL; tmp = tmp->next)
		tags = tags_add(tags, window_item_server_tag(tmp->data));
	for (tmp = window->bound_items; tmp != NULL; tmp = tmp->next) {
		WINDOW_BIND_REC *bind = tmp->data;

		tags = tags_add(tags, bind->servertag);
	}
	return tags;
}

/* something of the server TAG is still to be seen */
static gboolean server_tag_visible(const char *tag, SERVER_REC *server)
{
	GSList *tmp;

	if (server != NULL && (server->channels != NULL || server->queries != NULL))
		return TRUE;
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		if (window_shows_server(tmp->data, tag))
			return TRUE;
	}
	return FALSE;
}

/* what /CONNECT takes to come back: the network, or the address */
static const char *connect_target(SERVER_CONNECT_REC *conn)
{
	return conn->chatnet != NULL && *conn->chatnet != '\0' ? conn->chatnet : conn->address;
}

/* drops the reconnects waiting under TAG */
static void reconnects_drop(const char *tag)
{
	GSList *drop, *tmp;

	/* a server that lost its link waits in the reconnect queue under its
	   tag: the reconnect would bring back a server no window shows */
	drop = NULL;
	for (tmp = reconnects; tmp != NULL; tmp = tmp->next) {
		RECONNECT_REC *rec = tmp->data;

		if (tag_is(rec->conn->tag, tag))
			drop = g_slist_prepend(drop, rec);
	}
	for (tmp = drop; tmp != NULL; tmp = tmp->next) {
		/* a "server reconnect remove" handler may have removed it */
		if (g_slist_find(reconnects, tmp->data) == NULL)
			continue;
		closing_reconnect = tmp->data;
		server_reconnect_destroy(closing_reconnect);
		closing_reconnect = NULL;
	}
	g_slist_free(drop);
}

static void server_close_unseen(const char *tag)
{
	SERVER_REC *server;
	RECONNECT_REC *reconnect;
	GSList *tmp;
	char *target;
	int format;

	server = server_find_tag(tag);
	if (server == NULL)
		server = server_find_lookup_tag(tag);
	reconnect = NULL;
	for (tmp = reconnects; tmp != NULL && reconnect == NULL; tmp = tmp->next) {
		if (tag_is(((RECONNECT_REC *) tmp->data)->conn->tag, tag))
			reconnect = tmp->data;
	}
	if ((server == NULL && reconnect == NULL) || server_tag_visible(tag, server))
		return;

	target = g_strdup(connect_target(server != NULL ? server->connrec : reconnect->conn));
	format = server != NULL ? TXT_SERVER_WINDOW_CLOSED : TXT_SERVER_WINDOW_CLOSED_RECONNECT;
	if (server != NULL) {
		/* as /DISCONNECT does, with the user's quit message */
		closing_server = server;
		server_ref(server);
		signal_emit("server quit", 2, server, settings_get_str("quit_message"));
		server_disconnect(server);
		server_unref(server);
		closing_server = NULL;
	}
	reconnects_drop(tag);
	printformat_window(active_win, MSGLEVEL_CLIENTNOTICE, format, tag, target);
	g_free(target);
}

void fe_server_windows_closed(GSList *tags)
{
	GSList *tmp;

	for (tmp = tags; tmp != NULL; tmp = tmp->next)
		server_close_unseen(tmp->data);
	g_slist_free_full(tags, g_free);
}

static SERVER_REC *chatnet_server_find(GSList *list, const char *chatnet)
{
	for (; list != NULL; list = list->next) {
		SERVER_REC *server = list->data;

		if (tag_is(server->connrec->chatnet, chatnet))
			return server;
	}
	return NULL;
}

/* /CONNECT <network> while a server of that network is connected,
   connecting or waiting to reconnect opens no second connection: the
   server's window becomes active and one line says so. /CONNECT <address>
   is left to the core command - that is how a second connection is made. */
static void cmd_connect_existing(const char *data)
{
	GHashTable *optlist;
	CHATNET_REC *chatnet;
	SERVER_REC *server;
	RECONNECT_REC *reconnect;
	WINDOW_REC *window;
	GSList *tmp;
	char *addr, *port, *password, *nick;
	const char *tag;
	void *free_arg;

	g_return_if_fail(data != NULL);

	if (!cmd_get_params(data, &free_arg, 4 | PARAM_FLAG_OPTIONS, "connect", &optlist, &addr,
	                    &port, &password, &nick)) {
		/* the error is printed already, the core would print it again */
		signal_stop();
		return;
	}
	if (*addr == '+')
		addr++;
	chatnet = *addr == '\0' ? NULL : chatnet_find(addr);
	if (chatnet == NULL) {
		cmd_params_free(free_arg);
		return;
	}

	server = chatnet_server_find(servers, chatnet->name);
	if (server == NULL)
		server = chatnet_server_find(lookup_servers, chatnet->name);
	reconnect = NULL;
	for (tmp = reconnects; tmp != NULL && server == NULL && reconnect == NULL; tmp = tmp->next) {
		RECONNECT_REC *rec = tmp->data;

		if (tag_is(rec->conn->chatnet, chatnet->name))
			reconnect = rec;
	}
	if (server == NULL && reconnect == NULL) {
		cmd_params_free(free_arg);
		return;
	}

	tag = server != NULL ? server->tag :
	      reconnect->conn->tag != NULL ? reconnect->conn->tag : chatnet->name;
	window = window_find_network(tag);
	for (tmp = windows; tmp != NULL && window == NULL; tmp = tmp->next) {
		if (window_shows_server(tmp->data, tag))
			window = tmp->data;
	}
	if (window != NULL)
		window_set_active(window);
	printformat_window(active_win, MSGLEVEL_CLIENTNOTICE,
	                   server != NULL && server->connected ? TXT_SERVER_ALREADY_CONNECTED :
	                   TXT_SERVER_ALREADY_CONNECTING, tag);

	signal_stop();
	cmd_params_free(free_arg);
}

void fe_server_init(void)
{
	settings_add_bool("lookandfeel", "window_close_disconnects", TRUE);

	command_bind("server", NULL, (SIGNAL_FUNC) cmd_server);
	command_bind("server connect", NULL, (SIGNAL_FUNC) cmd_server_connect);
	command_bind("server add", NULL, (SIGNAL_FUNC) cmd_server_add);
	command_bind("server modify", NULL, (SIGNAL_FUNC) cmd_server_modify);
	command_bind("server remove", NULL, (SIGNAL_FUNC) cmd_server_remove);
	command_bind_first("server", NULL, (SIGNAL_FUNC) server_command);
	command_bind_first("disconnect", NULL, (SIGNAL_FUNC) server_command);
	command_bind_first("connect", NULL, (SIGNAL_FUNC) cmd_connect_existing);

	command_set_options(
	    "server add", "4 6 !! ~ssl ~nossl ~+ssl_cert ~+ssl_pkey ~+ssl_pass ~ssl_verify "
	                  "~nossl_verify ~+ssl_cafile ~+ssl_capath ~+ssl_ciphers ~+ssl_fingerprint "
	                  "tls notls +tls_cert +tls_pkey +tls_pass tls_verify notls_verify "
	                  "+tls_cafile +tls_capath +tls_ciphers +tls_pinned_cert "
	                  "+tls_pinned_pubkey auto noauto proxy noproxy -host -port noautosendcmd");
	command_set_options(
	    "server modify",
	    "4 6 !! ~ssl ~nossl ~+ssl_cert ~+ssl_pkey ~+ssl_pass ~ssl_verify ~nossl_verify "
	    "~+ssl_cafile ~+ssl_capath ~+ssl_ciphers ~+ssl_fingerprint tls notls +tls_cert "
	    "+tls_pkey +tls_pass tls_verify notls_verify +tls_cafile +tls_capath +tls_ciphers "
	    "+tls_pinned_cert +tls_pinned_pubkey auto noauto proxy noproxy -host -port "
	    "noautosendcmd");

	signal_add("server looking", (SIGNAL_FUNC) sig_server_looking);
	signal_add("server connecting", (SIGNAL_FUNC) sig_server_connecting);
	signal_add("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_add("server connect failed", (SIGNAL_FUNC) sig_connect_failed);
	signal_add("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_add("server quit", (SIGNAL_FUNC) sig_server_quit);

	signal_add("server lag disconnect", (SIGNAL_FUNC) sig_server_lag_disconnected);
	signal_add("server reconnect remove", (SIGNAL_FUNC) sig_server_reconnect_removed);
	signal_add("server reconnect not found", (SIGNAL_FUNC) sig_server_reconnect_not_found);

	signal_add("chat protocol unknown", (SIGNAL_FUNC) sig_chat_protocol_unknown);
}

void fe_server_deinit(void)
{
	command_unbind("server", (SIGNAL_FUNC) cmd_server);
	command_unbind("server connect", (SIGNAL_FUNC) cmd_server_connect);
	command_unbind("server add", (SIGNAL_FUNC) cmd_server_add);
	command_unbind("server modify", (SIGNAL_FUNC) cmd_server_modify);
	command_unbind("server remove", (SIGNAL_FUNC) cmd_server_remove);
	command_unbind("server", (SIGNAL_FUNC) server_command);
	command_unbind("disconnect", (SIGNAL_FUNC) server_command);
	command_unbind("connect", (SIGNAL_FUNC) cmd_connect_existing);

	signal_remove("server looking", (SIGNAL_FUNC) sig_server_looking);
	signal_remove("server connecting", (SIGNAL_FUNC) sig_server_connecting);
	signal_remove("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_remove("server connect failed", (SIGNAL_FUNC) sig_connect_failed);
	signal_remove("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_remove("server quit", (SIGNAL_FUNC) sig_server_quit);

	signal_remove("server lag disconnect", (SIGNAL_FUNC) sig_server_lag_disconnected);
	signal_remove("server reconnect remove", (SIGNAL_FUNC) sig_server_reconnect_removed);
	signal_remove("server reconnect not found", (SIGNAL_FUNC) sig_server_reconnect_not_found);

	signal_remove("chat protocol unknown", (SIGNAL_FUNC) sig_chat_protocol_unknown);
}
