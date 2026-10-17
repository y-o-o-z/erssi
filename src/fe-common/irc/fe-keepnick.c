/*
 fe-keepnick.c : erssi - /KEEPNICK, /UNKEEPNICK, /LISTNICK and what
                 keepnick (irc/core/irc-keepnick.c) has to say

    Copyright (C) 2026 the Shellter team

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

/*
 The commands and messages of the keepnick.pl script, the same words. The
 script's own file, ~/.erssi/keepnick (lines "<network> <nick>"), is
 imported once into the networks' keepnick option (the marker is
 erssi.keepnick_imported in the config) and then left alone, and the
 script's keepnick_quiet and keepnick_autosave with it. keepnick_autosave
 (ON, as the script) saves the config at once after /KEEPNICK and
 /UNKEEPNICK. While the
 script is loaded, these commands do nothing: the script answers them.
*/

#include "module.h"
#include <irssi/src/fe-common/irc/module-formats.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/commands.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/chatnets.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/lib-config/iconfig.h>

#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/irc-chatnets.h>
#include <irssi/src/irc/core/irc-keepnick.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/shellter-defaults.h>

static char *import_note;	/* printed once the windows are there */
static guint import_note_tag;
static gboolean script_note;	/* the script is loaded (the note to print) */
static guint script_note_tag;

static void crap(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void crap(const char *fmt, ...)
{
	va_list va;
	char *text;

	va_start(va, fmt);
	text = g_strdup_vprintf(fmt, va);
	va_end(va);
	printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_CRAP, text);
	g_free(text);
}

static IRC_CHATNET_REC *server_net(IRC_SERVER_REC *server)
{
	const char *chatnet = server == NULL ? NULL : server->connrec->chatnet;

	return chatnet == NULL || *chatnet == '\0' ? NULL : IRC_CHATNET(chatnet_find(chatnet));
}

static const char *server_label(IRC_SERVER_REC *server)
{
	IRC_CHATNET_REC *net = server_net(server);

	return net != NULL ? net->name : server->tag;
}

/* keepnick_autosave: /KEEPNICK and /UNKEEPNICK save the config at once,
   as the script saved its file - when irssi's own autosave would write
   it: settings_autosave ON and the file not changed outside erssi (else
   the user's edits would be lost); never a config read with errors (the
   part not read would be lost). Otherwise one line says /SAVE does it. */
static void autosave(void)
{
	char *reason = NULL;

	if (!settings_get_bool("keepnick_autosave") || mainconfig->fname == NULL)
		return;
	if (!settings_get_bool("settings_autosave")) {
		crap("settings_autosave is OFF; /SAVE to write the kept nick");
		return;
	}
	if (config_last_error(mainconfig) != NULL) {
		crap("the config file has errors; /SAVE to write the kept nick");
		return;
	}
	if (irssi_config_is_changed(NULL)) {
		crap("the config file was changed outside erssi; /SAVE to write the kept nick");
		return;
	}
	if (!shellter_save_at_start(&reason))
		crap("the configuration could not be saved (%s); /SAVE to try again", reason);
	g_free(reason);
}

static gboolean is_net_option(const char *arg)
{
	return g_ascii_strcasecmp(arg, "-net") == 0 || g_ascii_strcasecmp(arg, "-network") == 0 ||
	       g_ascii_strcasecmp(arg, "-chatnet") == 0 || g_ascii_strcasecmp(arg, "-ircnet") == 0;
}

/* SYNTAX: KEEPNICK [-network <network>] [<nick>] */
static void cmd_keepnick(const char *data, SERVER_REC *server)
{
	IRC_SERVER_REC *irc;
	IRC_CHATNET_REC *net = NULL;
	char **args, **arg;
	const char *chatnet = NULL, *nick = NULL, *why;

	if (keepnick_script_loaded())
		return;	/* the script's command */

	args = g_strsplit_set(data, " \t", -1);
	for (arg = args; *arg != NULL; arg++) {
		if (**arg == '\0')
			continue;
		if (is_net_option(*arg)) {
			while (arg[1] != NULL && *arg[1] == '\0')
				arg++;
			if (arg[1] == NULL)
				break;
			chatnet = *++arg;
		} else if (**arg == '-') {
			/* as the script: said, and the command still runs */
			crap("Unknown parameter %s", *arg);
		} else if (nick == NULL) {
			nick = *arg;
		}
	}

	irc = IRC_SERVER(server);
	if (chatnet != NULL) {
		net = IRC_CHATNET(chatnet_find(chatnet));
		if (net == NULL) {
			crap("Unknown chat network: %s", chatnet);
			goto out;
		}
		irc = IRC_SERVER(server_find_chatnet(net->name));
	}

	if (irc == NULL && (nick == NULL || net == NULL)) {
		crap("Not connected to server");
		goto out;
	}
	if (net == NULL)
		net = server_net(irc);
	if (nick == NULL)
		nick = irc->nick;
	if (net == NULL) {
		crap("Unable to find server network, maybe you forgot /server add before connecting?");
		goto out;
	}

	why = keepnick_nick_invalid(nick, irc);
	if (why != NULL) {
		crap("%s cannot be kept on %s: %s", nick, net->name, why);
		goto out;
	}

	if (keepnick_resume(net->name))
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_UNHOLD, nick, net->name);
	printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_ADD, nick, net->name);
	keepnick_set(net, nick);
	autosave();
out:
	g_strfreev(args);
}

/* SYNTAX: UNKEEPNICK [<network>] */
static void cmd_unkeepnick(const char *data, SERVER_REC *server)
{
	IRC_SERVER_REC *irc = IRC_SERVER(server);
	IRC_CHATNET_REC *net;
	char *name;
	const char *nick;

	if (keepnick_script_loaded())
		return;

	name = g_strstrip(g_strdup(data));
	if (*name != '\0') {
		net = IRC_CHATNET(chatnet_find(name));
		if (net == NULL) {
			crap("Unknown chat network: %s", name);
			goto out;
		}
		irc = IRC_SERVER(server_find_chatnet(net->name));
		g_free(name);
		name = g_strdup(net->name);
	} else if (irc != NULL && irc->connrec->chatnet != NULL && *irc->connrec->chatnet != '\0') {
		net = server_net(irc);
		g_free(name);
		name = g_strdup(net != NULL ? net->name : irc->connrec->chatnet);
	} else if (irc != NULL && keepnick_auto_nick(irc, NULL) != NULL) {
		/* keepnick_auto on a server of no network */
		net = NULL;
		g_free(name);
		name = g_strdup(irc->tag);
	} else {
		crap("Usage: /UNKEEPNICK <chatnet> (this window has no network)");
		goto out;
	}

	if (net != NULL && net->keepnick != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_REMOVE, net->keepnick, name);
		keepnick_set(net, NULL);
		autosave();
	} else if (irc != NULL && (nick = keepnick_auto_nick(irc, NULL)) != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_REMOVE, nick, name);
		keepnick_auto_stop(irc);
	} else {
		crap("No nick is kept on %s", name);
	}
out:
	g_free(name);
}

static int net_cmp(IRC_CHATNET_REC *a, IRC_CHATNET_REC *b)
{
	return g_ascii_strcasecmp(a->name, b->name);
}

/* SYNTAX: LISTNICK */
static void cmd_listnick(void)
{
	GSList *nets = NULL, *tmp;
	gboolean any = FALSE;

	if (keepnick_script_loaded())
		return;

	for (tmp = chatnets; tmp != NULL; tmp = tmp->next) {
		IRC_CHATNET_REC *net = IRC_CHATNET(tmp->data);

		if (net != NULL && net->keepnick != NULL)
			nets = g_slist_insert_sorted(nets, net, (GCompareFunc) net_cmp);
	}

	for (tmp = nets; tmp != NULL; tmp = tmp->next) {
		IRC_CHATNET_REC *net = tmp->data;
		const char *why;
		char *state;

		/* not kept at all: not one IRC word, or too long for the
		   server that would keep it */
		why = keepnick_nick_invalid(net->keepnick, IRC_SERVER(server_find_chatnet(net->name)));
		state = why != NULL ? g_strconcat("refused: ", why, NULL) :
		        g_strdup(keepnick_is_paused(net->name) ? "inactive" : "active");
		if (!any)
			printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_LIST_HEADER);
		any = TRUE;
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_LIST_LINE, net->keepnick,
		            net->name, state);
		g_free(state);
	}
	g_slist_free(nets);

	/* keepnick_auto, for one connection */
	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		IRC_SERVER_REC *irc = IRC_SERVER(tmp->data);
		IRC_CHATNET_REC *net;
		gboolean paused;
		const char *nick;

		if (irc == NULL || (nick = keepnick_auto_nick(irc, &paused)) == NULL)
			continue;
		net = server_net(irc);
		if (net != NULL && net->keepnick != NULL)
			continue;
		if (!any)
			printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_LIST_HEADER);
		any = TRUE;
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_LIST_LINE, nick,
		            server_label(irc), paused ? "inactive, this connection" : "active, this connection");
	}

	if (any)
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_LIST_FOOTER);
	else
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_LIST_EMPTY);
}

/* ---- what keepnick says ---- */

/* Our NICK got the nick: "Nickstealer left" instead of the nick change,
   once in every window with a channel or query of the server (else in
   its window); keepnick_quiet: once, in the network window. Runs after
   the default handlers (side panels, web clients see the change) and
   before fe-messages.c prints it. */
static void sig_message_own_nick(SERVER_REC *server, const char *newnick, const char *oldnick,
                                 const char *address)
{
	IRC_SERVER_REC *irc = IRC_SERVER(server);
	const int level = MSGLEVEL_NICKS | MSGLEVEL_NO_ACT;
	TEXT_DEST_REC dest;
	gboolean printed = FALSE;
	const char *label;
	GSList *tmp, *items;

	if (irc == NULL || !keepnick_regaining(server))
		return;
	signal_stop();
	label = server_label(irc);

	if (settings_get_bool("keepnick_quiet")) {
		WINDOW_REC *window = window_find_network(server->tag);

		if (window != NULL) {
			format_create_dest(&dest, server, NULL, level, window);
			printformat_dest(&dest, IRCTXT_KEEPNICK_GOT_NICK, newnick, label);
		} else {
			printformat(server, NULL, level, IRCTXT_KEEPNICK_GOT_NICK, newnick, label);
		}
		return;
	}

	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *window = tmp->data;

		for (items = window->items; items != NULL; items = items->next) {
			WI_ITEM_REC *item = items->data;

			if (item->server != server)
				continue;
			format_create_dest(&dest, server, item->visible_name, level, window);
			printformat_dest(&dest, IRCTXT_KEEPNICK_GOT_NICK, newnick, label);
			printed = TRUE;
			break;
		}
	}
	if (!printed)
		printformat(server, NULL, level, IRCTXT_KEEPNICK_GOT_NICK, newnick, label);
}

static void sig_keepnick_paused(SERVER_REC *server, const char *nick, const char *network)
{
	printformat(server, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_HOLD, nick, network);
}

static void sig_keepnick_resumed(SERVER_REC *server, const char *nick, const char *network)
{
	printformat(server, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_UNHOLD, nick, network);
}

static void sig_keepnick_refused(SERVER_REC *server, const char *nick, const char *network,
                                 const char *reason)
{
	printformat(server, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_REFUSED, nick, network, reason);
}

static void sig_keepnick_invalid(SERVER_REC *server, const char *nick, const char *network,
                                 const char *reason)
{
	TEXT_DEST_REC dest;
	char *text = g_strdup_printf("%s cannot be kept on %s: %s", nick, network, reason);

	format_create_dest(&dest, server, NULL, MSGLEVEL_CLIENTCRAP, NULL);
	printformat_dest(&dest, IRCTXT_KEEPNICK_CRAP, text);
	g_free(text);
}

static void sig_keepnick_auto(SERVER_REC *server, const char *nick, const char *network)
{
	printformat(server, NULL, MSGLEVEL_CLIENTCRAP, IRCTXT_KEEPNICK_AUTO, nick, network);
}

static gboolean print_script_note(gpointer data)
{
	script_note_tag = 0;
	if (script_note)
		crap("the keepnick.pl script is loaded; built-in keepnick is off until it is unloaded");
	else
		crap("keepnick.pl is unloaded; built-in keepnick is on again");
	return FALSE;
}

/* said from the main loop: a script of autorun is loaded while erssi
   starts, before the windows and logs are there */
static void sig_keepnick_script(gpointer loaded)
{
	script_note = GPOINTER_TO_INT(loaded);
	if (script_note_tag == 0)
		script_note_tag = g_idle_add(print_script_note, NULL);
}

/* ---- ~/.erssi/keepnick ---- */

static gboolean print_import_note(gpointer data)
{
	import_note_tag = 0;
	if (import_note != NULL)
		crap("%s", import_note);
	g_free(import_note);
	import_note = NULL;
	return FALSE;
}

/* the script's settings (perl/core/scripts) for the built-in ones,
   unless those are set; the script's own entries stay for the script */
static void import_script_setting(const char *key)
{
	CONFIG_NODE *settings, *old, *new;

	settings = iconfig_node_traverse("settings", FALSE);
	old = settings == NULL ? NULL : iconfig_node_section(settings, "perl/core/scripts", -1);
	new = settings == NULL ? NULL : iconfig_node_section(settings, "irc/core", -1);
	if (old == NULL || config_node_find(old, key) == NULL ||
	    (new != NULL && config_node_find(new, key) != NULL))
		return;
	settings_set_bool(key, config_node_get_bool(old, key, FALSE));
}

/* The script's file, read as the script does (split on white space: the
   network, then the nick; a network listed again: the last line wins),
   into the networks that keep no nick yet. Once: the config gets
   erssi.keepnick_imported; the file stays as it is. */
static void import_script_file(void)
{
	CONFIG_NODE *marker;
	GHashTable *entries;
	GHashTableIter iter;
	GString *skipped, *invalid;
	char *path, *text, **lines, **line, *reason = NULL;
	gpointer key, value;
	int imported = 0, counter;

	marker = iconfig_node_traverse("erssi", FALSE);
	if (marker != NULL && config_node_get_bool(marker, "keepnick_imported", FALSE))
		return;

	path = g_build_filename(get_irssi_dir(), "keepnick", NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL)) {
		g_free(path);
		return;
	}

	counter = mainconfig->modifycounter;
	import_script_setting("keepnick_quiet");
	import_script_setting("keepnick_autosave");

	/* lowercase network -> nick, as the script's hash */
	entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	lines = g_strsplit(text, "\n", -1);
	for (line = lines; *line != NULL; line++) {
		char **words = g_strsplit_set(*line, " \t\r\f\v", -1);
		const char *name = NULL, *nick = NULL;
		char **w;

		for (w = words; *w != NULL && nick == NULL; w++) {
			if (**w == '\0')
				continue;
			if (name == NULL)
				name = *w;
			else
				nick = *w;
		}
		if (nick != NULL)
			g_hash_table_replace(entries, g_ascii_strdown(name, -1), g_strdup(nick));
		g_strfreev(words);
	}
	g_strfreev(lines);
	g_free(text);

	skipped = g_string_new(NULL);
	invalid = g_string_new(NULL);
	g_hash_table_iter_init(&iter, entries);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		IRC_CHATNET_REC *net = IRC_CHATNET(chatnet_find(key));

		if (net == NULL)
			g_string_append_printf(skipped, "%s%s", skipped->len > 0 ? ", " : "", (char *) key);
		else if (keepnick_nick_invalid(value, NULL) != NULL)
			g_string_append_printf(invalid, "%s%s", invalid->len > 0 ? ", " : "", (char *) value);
		else if (net->keepnick == NULL) {
			keepnick_set(net, value);
			imported++;
		}
	}
	g_hash_table_destroy(entries);

	marker = iconfig_node_traverse("erssi", TRUE);
	iconfig_node_set_bool(marker, "keepnick_imported", TRUE);

	if (mainconfig->modifycounter != counter) {
		if (config_last_error(mainconfig) != NULL) {
			/* a config read only in part is not written over */
			mainconfig->modifycounter = counter;
			reason = g_strdup("the config has errors");
		} else if (mainconfig->fname != NULL &&
		           g_file_test(mainconfig->fname, G_FILE_TEST_IS_REGULAR)) {
			/* saved at once, as the other start-time migrations */
			shellter_save_at_start(&reason);
		}
	}

	import_note = g_strdup_printf("imported %d nick%s from %s into the networks' keepnick "
	                              "option (/LISTNICK); the built-in keepnick does not read "
	                              "the file again%s%s%s%s%s%s",
	                              imported, imported == 1 ? "" : "s", path,
	                              skipped->len > 0 ? " - no such network: " : "", skipped->str,
	                              invalid->len > 0 ? " - not a nick: " : "", invalid->str,
	                              reason != NULL ? " - not saved yet (/SAVE): " : "",
	                              reason != NULL ? reason : "");
	import_note_tag = g_idle_add(print_import_note, NULL);
	g_string_free(skipped, TRUE);
	g_string_free(invalid, TRUE);
	g_free(reason);
	g_free(path);
}

void fe_keepnick_init(void)
{
	command_bind("keepnick", NULL, (SIGNAL_FUNC) cmd_keepnick);
	command_bind("unkeepnick", NULL, (SIGNAL_FUNC) cmd_unkeepnick);
	command_bind("listnick", NULL, (SIGNAL_FUNC) cmd_listnick);

	signal_add_full(MODULE_NAME, SIGNAL_PRIORITY_LOW - 1, "message own_nick",
	                (SIGNAL_FUNC) sig_message_own_nick, NULL);
	signal_add("keepnick paused", (SIGNAL_FUNC) sig_keepnick_paused);
	signal_add("keepnick resumed", (SIGNAL_FUNC) sig_keepnick_resumed);
	signal_add("keepnick refused", (SIGNAL_FUNC) sig_keepnick_refused);
	signal_add("keepnick auto", (SIGNAL_FUNC) sig_keepnick_auto);
	signal_add("keepnick invalid", (SIGNAL_FUNC) sig_keepnick_invalid);
	signal_add("keepnick script", (SIGNAL_FUNC) sig_keepnick_script);

	import_script_file();
}

void fe_keepnick_deinit(void)
{
	command_unbind("keepnick", (SIGNAL_FUNC) cmd_keepnick);
	command_unbind("unkeepnick", (SIGNAL_FUNC) cmd_unkeepnick);
	command_unbind("listnick", (SIGNAL_FUNC) cmd_listnick);

	signal_remove("message own_nick", (SIGNAL_FUNC) sig_message_own_nick);
	signal_remove("keepnick paused", (SIGNAL_FUNC) sig_keepnick_paused);
	signal_remove("keepnick resumed", (SIGNAL_FUNC) sig_keepnick_resumed);
	signal_remove("keepnick refused", (SIGNAL_FUNC) sig_keepnick_refused);
	signal_remove("keepnick auto", (SIGNAL_FUNC) sig_keepnick_auto);
	signal_remove("keepnick invalid", (SIGNAL_FUNC) sig_keepnick_invalid);
	signal_remove("keepnick script", (SIGNAL_FUNC) sig_keepnick_script);

	if (import_note_tag != 0) {
		g_source_remove(import_note_tag);
		import_note_tag = 0;
	}
	if (script_note_tag != 0) {
		g_source_remove(script_note_tag);
		script_note_tag = 0;
	}
	g_free(import_note);
	import_note = NULL;
}
