/*
 irc-keepnick.c : erssi - takes a nick back when it becomes free

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
 The built-in replacement of the keepnick.pl script (Peder Stray's
 keepnick 1.19, with the erssi fixes): the same commands (fe-keepnick.c),
 the same rules, and a few more.

 What is kept:
  - one nick per network, the network's `keepnick' option in chatnets
    (/KEEPNICK sets it). Only the first connected server of the network
    takes it, as server_find_chatnet() finds it;
  - keepnick_auto: the nick a connection was asked to use (connrec->nick:
    the network's nick or the global one), when it was taken at connect
    (433/437 before 001). For that connection only, never saved.

 How it is taken back, while the server is not on it:
  - MONITOR + <nick> when the server has MONITOR in 005: 731 (offline)
    tries at once;
  - otherwise ISON every keepnick_check_time;
  - the holder's NICK or QUIT seen in a shared channel tries at once.
 The NICK goes through a redirect: its 432/433/437/484 never print as
 errors. 432 (erroneous) and 484 (restricted) pause keeping, 433 and 437
 (IRCnet's nick delay) are tried again later: at most one NICK per check
 interval, except when the holder was just seen leaving.

 A manual /NICK away from the kept nick (server->last_nick, set by /NICK)
 pauses keeping on that network until the nick is ours again or /KEEPNICK
 runs; a change the server forces (a collision, SVSNICK) does not.

 Nothing at all is sent for a server with nothing to keep, nor while a
 Perl script named keepnick is loaded: the script keeps the nicks then.
*/

#include "module.h"
#include <irssi/src/core/signals.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/chatnets.h>
#include <irssi/src/lib-config/iconfig.h>

#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/irc-chatnets.h>
#include <irssi/src/irc/core/irc-keepnick.h>
#include <irssi/src/irc/core/servers-redirect.h>
#include <irssi/src/perl/perl-core.h>

#define CHECK_TIME_MIN 5000 /* msecs */

typedef struct {
	IRC_SERVER_REC *server;

	char *auto_nick;	/* keepnick_auto: kept for this connection */
	char *monitored;	/* the nick in our MONITOR list */
	guint check_tag;	/* the check every keepnick_check_time */
	gint64 last_attempt;	/* monotonic time of our last NICK */

	unsigned int collided:1;	/* 433/437 before 001 */
	unsigned int auto_paused:1;	/* a manual /NICK paused auto_nick */
	unsigned int pending:1;		/* our NICK waits for its reply */
	unsigned int delayed:1;		/* the last reply was 437 */
	unsigned int offline:1;		/* MONITOR: the nick is not on */
	unsigned int monitor_failed:1;	/* MONITOR list full: ISON */
} KEEPNICK_SERVER_REC;

static GHashTable *states;	/* IRC_SERVER_REC -> KEEPNICK_SERVER_REC */
static GHashTable *paused_nets;	/* lowercase chatnet -> 1 */
static GSList *keepnick_scripts; /* PERL_SCRIPT_REC of scripts named keepnick */
static IRC_SERVER_REC *regaining;
static guint update_tag;
static int check_time;

static int nickcmp(IRC_SERVER_REC *server, const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b ? 0 : 1;
	return server->nick_comp_func != NULL ? server->nick_comp_func(a, b) :
	                                        g_ascii_strcasecmp(a, b);
}

static KEEPNICK_SERVER_REC *state_get(IRC_SERVER_REC *server, gboolean create)
{
	KEEPNICK_SERVER_REC *st = g_hash_table_lookup(states, server);

	if (st == NULL && create) {
		st = g_new0(KEEPNICK_SERVER_REC, 1);
		st->server = server;
		g_hash_table_insert(states, server, st);
	}
	return st;
}

static void state_free(KEEPNICK_SERVER_REC *st)
{
	if (st->check_tag != 0)
		g_source_remove(st->check_tag);
	g_free(st->auto_nick);
	g_free(st->monitored);
	g_free(st);
}

static char *net_key(const char *chatnet)
{
	return g_ascii_strdown(chatnet, -1);
}

gboolean keepnick_is_paused(const char *chatnet)
{
	char *key;
	gboolean found;

	if (chatnet == NULL)
		return FALSE;
	key = net_key(chatnet);
	found = g_hash_table_contains(paused_nets, key);
	g_free(key);
	return found;
}

static void net_pause(const char *chatnet)
{
	g_hash_table_add(paused_nets, net_key(chatnet));
}

gboolean keepnick_resume(const char *chatnet)
{
	char *key;
	gboolean found;

	if (chatnet == NULL)
		return FALSE;
	key = net_key(chatnet);
	found = g_hash_table_remove(paused_nets, key);
	g_free(key);
	return found;
}

gboolean keepnick_script_loaded(void)
{
	return keepnick_scripts != NULL;
}

gboolean keepnick_regaining(SERVER_REC *server)
{
	return server != NULL && (SERVER_REC *) regaining == server;
}

/* the network of the server as configured (its own spelling) */
static IRC_CHATNET_REC *server_net(IRC_SERVER_REC *server)
{
	const char *chatnet = server->connrec->chatnet;

	return chatnet == NULL || *chatnet == '\0' ? NULL : IRC_CHATNET(chatnet_find(chatnet));
}

/* what messages call the server's network: its name, else the tag */
static const char *server_label(IRC_SERVER_REC *server)
{
	IRC_CHATNET_REC *net = server_net(server);

	return net != NULL ? net->name : server->tag;
}

/* The nick this server keeps, NULL if none. `explicit' says if it is the
   network's keepnick (else keepnick_auto's), `paused' if keeping is
   paused. */
static const char *kept_nick(IRC_SERVER_REC *server, gboolean *explicit, gboolean *paused)
{
	KEEPNICK_SERVER_REC *st;
	IRC_CHATNET_REC *net;

	*explicit = *paused = FALSE;
	net = server_net(server);
	if (net != NULL && net->keepnick != NULL) {
		/* another connection of the network keeps it */
		if (server_find_chatnet(net->name) != SERVER(server))
			return NULL;
		*explicit = TRUE;
		*paused = keepnick_is_paused(net->name);
		return net->keepnick;
	}

	st = state_get(server, FALSE);
	if (st == NULL || st->auto_nick == NULL)
		return NULL;
	*paused = st->auto_paused;
	return st->auto_nick;
}

/* the nick to take back now: kept, not paused, not ours already */
static const char *wanted_nick(IRC_SERVER_REC *server)
{
	gboolean explicit, paused;
	const char *nick;

	if (keepnick_script_loaded() || !server->connected || server->connection_lost)
		return NULL;
	nick = kept_nick(server, &explicit, &paused);
	if (nick == NULL || paused || nickcmp(server, server->nick, nick) == 0)
		return NULL;
	return nick;
}

static gboolean use_monitor(IRC_SERVER_REC *server, KEEPNICK_SERVER_REC *st)
{
	return !st->monitor_failed && server->isupport != NULL &&
	       g_hash_table_lookup(server->isupport, "MONITOR") != NULL;
}

static void monitor_remove(KEEPNICK_SERVER_REC *st)
{
	if (st->monitored == NULL)
		return;
	if (st->server->connected && !st->server->connection_lost)
		irc_send_cmdv(st->server, "MONITOR - %s", st->monitored);
	g_free(st->monitored);
	st->monitored = NULL;
	st->offline = FALSE;
}

/* nothing to keep: no timer, nothing monitored */
static void keepnick_stop(KEEPNICK_SERVER_REC *st)
{
	if (st->check_tag != 0) {
		g_source_remove(st->check_tag);
		st->check_tag = 0;
	}
	monitor_remove(st);
	st->pending = st->delayed = FALSE;
}

/* NICK <nick>, its replies redirected */
static void attempt(IRC_SERVER_REC *server, gboolean holder_left)
{
	KEEPNICK_SERVER_REC *st;
	const char *nick;
	char *arg;
	gint64 now, gap;

	nick = wanted_nick(server);
	if (nick == NULL)
		return;
	st = state_get(server, TRUE);

	/* one NICK per check interval (a little less, the timer is not
	   exact); only a holder seen leaving may try sooner, and not while
	   a NICK waits for its reply or the last one met the nick delay */
	now = g_get_monotonic_time();
	gap = (gint64) check_time * 1000 - G_USEC_PER_SEC / 2;
	if (st->last_attempt != 0 && now - st->last_attempt < gap &&
	    (!holder_left || st->pending || st->delayed))
		return;

	st->last_attempt = now;
	st->pending = TRUE;
	st->delayed = FALSE;

	/* the NICK reply has the nick as its first word (":nick" or "nick"),
	   the numerics as their second */
	arg = g_strdup_printf("%s :%s", nick, nick);
	server_redirect_event(server, "keepnick nick", 1, arg, -1, NULL,
	                      "event nick", "redir keepnick nick",
	                      "event 432", "redir keepnick refused",
	                      "event 484", "redir keepnick refused",
	                      "event 433", "redir keepnick busy",
	                      "event 437", "redir keepnick delayed", NULL);
	g_free(arg);
	/* past the queue, as the script's send_raw: the nick is free now */
	arg = g_strdup_printf("NICK %s", nick);
	irc_send_cmd_now(server, arg);
	g_free(arg);
}

static void send_ison(IRC_SERVER_REC *server, const char *nick)
{
	server_redirect_event(server, "ison", 1, NULL, -1, NULL,
	                      "event 303", "redir keepnick ison", NULL);
	irc_send_cmdv(server, "ISON :%s", nick);
}

static gboolean sig_check(IRC_SERVER_REC *server);

/* Starts, goes on with or stops keeping on the server, as there is a nick
   to take back or not. `now': look at once (ISON) instead of at the next
   check. */
static void keepnick_update(IRC_SERVER_REC *server, gboolean now)
{
	KEEPNICK_SERVER_REC *st;
	const char *nick;

	if (!IS_IRC_SERVER(server))
		return;

	nick = wanted_nick(server);
	if (nick == NULL) {
		st = state_get(server, FALSE);
		if (st != NULL)
			keepnick_stop(st);
		return;
	}

	st = state_get(server, TRUE);
	if (use_monitor(server, st)) {
		if (st->monitored == NULL || nickcmp(server, st->monitored, nick) != 0) {
			monitor_remove(st);
			/* 730 or 731 comes back at once */
			irc_send_cmdv(server, "MONITOR + %s", nick);
			st->monitored = g_strdup(nick);
		} else if (now && st->offline) {
			attempt(server, FALSE);
		}
	} else if (now) {
		send_ison(server, nick);
	}

	if (st->check_tag == 0)
		st->check_tag = g_timeout_add(check_time, (GSourceFunc) sig_check, server);
}

/* every keepnick_check_time while there is a nick to take back */
static gboolean sig_check(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	const char *nick;

	if (st == NULL)
		return FALSE;
	nick = wanted_nick(server);
	if (nick == NULL) {
		st->check_tag = 0;
		keepnick_stop(st);
		return FALSE;
	}

	if (use_monitor(server, st)) {
		/* 437 or 433 came back: try again while it is off */
		keepnick_update(server, FALSE);
		if (st->offline)
			attempt(server, FALSE);
	} else {
		send_ison(server, nick);
	}
	return TRUE;
}

static void update_all(gboolean now)
{
	GSList *tmp;

	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		if (IS_IRC_SERVER(tmp->data))
			keepnick_update(tmp->data, now);
	}
}

static gboolean update_all_idle(gpointer data)
{
	update_tag = 0;
	update_all(TRUE);
	return FALSE;
}

/* the networks were read again (/RELOAD) or changed: once that is over */
static void sig_chatnets_changed(void)
{
	if (update_tag == 0)
		update_tag = g_idle_add(update_all_idle, NULL);
}

void keepnick_set(IRC_CHATNET_REC *net, const char *nick)
{
	CONFIG_NODE *node;
	GSList *tmp;
	char *old;

	g_return_if_fail(IS_IRC_CHATNET(net));

	/* `nick' may be the old value */
	old = net->keepnick;
	net->keepnick = nick != NULL && *nick != '\0' ? g_strdup(nick) : NULL;
	g_free(old);

	node = iconfig_node_traverse("chatnets", TRUE);
	node = iconfig_node_section(node, net->name, NODE_TYPE_BLOCK);
	iconfig_node_set_str(node, "keepnick", net->keepnick);

	if (net->keepnick == NULL)
		keepnick_resume(net->name);

	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		IRC_SERVER_REC *server = IRC_SERVER(tmp->data);
		KEEPNICK_SERVER_REC *st;

		if (server == NULL || server_net(server) != net)
			continue;
		/* the network's nick replaces this connection's own */
		st = state_get(server, FALSE);
		if (st != NULL && net->keepnick != NULL) {
			g_free(st->auto_nick);
			st->auto_nick = NULL;
			st->auto_paused = FALSE;
		}
		keepnick_update(server, TRUE);
	}
}

const char *keepnick_auto_nick(IRC_SERVER_REC *server, gboolean *paused)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);

	if (paused != NULL)
		*paused = st != NULL && st->auto_paused;
	return st == NULL ? NULL : st->auto_nick;
}

gboolean keepnick_auto_stop(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);

	if (st == NULL || st->auto_nick == NULL)
		return FALSE;
	g_free(st->auto_nick);
	st->auto_nick = NULL;
	st->auto_paused = FALSE;
	keepnick_update(server, FALSE);
	return TRUE;
}

/* ---- server events ---- */

/* 433/437 before 001: the nick asked for was taken */
static void sig_nick_taken(IRC_SERVER_REC *server)
{
	if (IS_IRC_SERVER(server) && !server->connected)
		state_get(server, TRUE)->collided = TRUE;
}

/* 001: keepnick_auto for a connection on another nick than the one it
   asked for, because that one was taken */
static void sig_event_connected(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st;
	gboolean explicit, paused;
	const char *wanted;

	if (!IS_IRC_SERVER(server))
		return;
	st = state_get(server, FALSE);
	wanted = server->connrec->nick;
	if (st != NULL && st->collided && settings_get_bool("keepnick_auto") &&
	    wanted != NULL && *wanted != '\0' && nickcmp(server, server->nick, wanted) != 0 &&
	    kept_nick(server, &explicit, &paused) == NULL) {
		st->auto_nick = g_strdup(wanted);
		signal_emit("keepnick auto", 3, server, st->auto_nick, server_label(server));
	}
	/* the first look is at the end of the MOTD, after 005 */
	keepnick_update(server, FALSE);
}

/* end of MOTD (or no MOTD): 005 is known, MONITOR or ISON */
static void sig_end_of_motd(IRC_SERVER_REC *server)
{
	keepnick_update(server, TRUE);
}

/* a connection restored after /UPGRADE has no 001 */
static void sig_server_connected(IRC_SERVER_REC *server)
{
	if (IS_IRC_SERVER(server) && server->connected)
		keepnick_update(server, TRUE);
}

static void sig_server_gone(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st = g_hash_table_lookup(states, server);

	if (st == NULL)
		return;
	g_hash_table_remove(states, server);
	state_free(st);
	if (regaining == server)
		regaining = NULL;
}

/* our own nick changed (after irc-nicklist.c set server->nick), or
   someone's: the holder of the kept nick may have left it */
static void sig_event_nick(IRC_SERVER_REC *server, const char *data, const char *orignick)
{
	KEEPNICK_SERVER_REC *st;
	gboolean explicit, paused, manual;
	const char *nick;
	char *params, *newnick;

	if (!IS_IRC_SERVER(server) || orignick == NULL || keepnick_script_loaded())
		return;

	params = event_get_params(data, 1, &newnick);
	nick = kept_nick(server, &explicit, &paused);
	if (nick == NULL || nickcmp(server, server->nick, newnick) != 0) {
		/* not ours: the holder changed nick */
		if (nick != NULL && nickcmp(server, orignick, nick) == 0)
			attempt(server, TRUE);
		g_free(params);
		return;
	}

	st = state_get(server, TRUE);
	/* /NICK sets last_nick; a nick the server forces does not match it */
	manual = !keepnick_regaining(SERVER(server)) && server->last_nick != NULL &&
	         nickcmp(server, server->last_nick, newnick) == 0;

	if (nickcmp(server, newnick, nick) == 0) {
		/* got it */
		st->pending = st->delayed = FALSE;
		if (paused) {
			if (explicit)
				keepnick_resume(server_label(server));
			else
				st->auto_paused = FALSE;
			signal_emit("keepnick resumed", 3, server, nick, server_label(server));
		}
	} else if (manual && !paused && (!explicit || nickcmp(server, orignick, nick) == 0)) {
		/* away from the kept nick by hand (keepnick_auto: any /NICK) */
		if (explicit)
			net_pause(server_label(server));
		else
			st->auto_paused = TRUE;
		signal_emit("keepnick paused", 3, server, nick, server_label(server));
	}
	/* forced away (a collision, SVSNICK): look again at once */
	keepnick_update(server, !manual);
	g_free(params);
}

static void sig_event_quit(IRC_SERVER_REC *server, const char *data, const char *nick)
{
	const char *kept;

	if (!IS_IRC_SERVER(server) || nick == NULL)
		return;
	kept = wanted_nick(server);
	if (kept != NULL && nickcmp(server, nick, kept) == 0)
		attempt(server, TRUE);
}

/* ---- the redirected replies ---- */

static void sig_redir_ison(IRC_SERVER_REC *server, const char *data)
{
	char *params, *online, **nicks, **tmp;
	const char *nick;
	gboolean on = FALSE;

	nick = wanted_nick(server);
	if (nick == NULL)
		return;
	params = event_get_params(data, 2, NULL, &online);
	nicks = g_strsplit(online, " ", -1);
	for (tmp = nicks; *tmp != NULL && !on; tmp++)
		on = **tmp != '\0' && nickcmp(server, *tmp, nick) == 0;
	g_strfreev(nicks);
	g_free(params);
	if (!on)
		attempt(server, FALSE);
}

/* our NICK went through: the normal nick change, which fe-keepnick.c
   reports in its own words */
static void sig_redir_nick(IRC_SERVER_REC *server, const char *data, const char *nick,
                           const char *address)
{
	IRC_SERVER_REC *old = regaining;

	regaining = server;
	signal_emit("event nick", 4, server, data, nick, address);
	regaining = old;
}

static void sig_redir_busy(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);

	if (st != NULL)
		st->pending = st->delayed = FALSE;
}

static void sig_redir_delayed(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);

	if (st != NULL) {
		st->pending = FALSE;
		st->delayed = TRUE;
	}
}

/* 432 erroneous, 484 restricted: trying again does not help */
static void refused(IRC_SERVER_REC *server, const char *reason)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	gboolean explicit, paused;
	const char *nick;

	if (st != NULL)
		st->pending = st->delayed = FALSE;
	nick = kept_nick(server, &explicit, &paused);
	if (nick == NULL || paused)
		return;
	if (explicit)
		net_pause(server_label(server));
	else
		st->auto_paused = TRUE;
	signal_emit("keepnick refused", 4, server, nick, server_label(server), reason);
	keepnick_update(server, FALSE);
}

static void sig_redir_refused(IRC_SERVER_REC *server, const char *data)
{
	const char *event = current_server_event;

	refused(server, event != NULL && strcmp(event, "484") == 0 ? "restricted connection" :
	                                                              "erroneous nickname");
}

/* ---- MONITOR (730 online, 731 offline, 734 list full) ---- */

/* the line is no reply about our nick: printed as an unknown numeric
   would be, so that the user's own MONITOR still shows */
static void monitor_default(IRC_SERVER_REC *server, const char *numeric, const char *data,
                            const char *nick, const char *address)
{
	char *line = g_strconcat(numeric, " ", data, NULL);

	signal_emit("default event", 4, server, line, nick, address);
	g_free(line);
}

/* TRUE if the 730/731 list (nick!user@host or nick, comma separated) has
   the nick we monitor */
static gboolean monitor_lists(KEEPNICK_SERVER_REC *st, const char *data)
{
	char *params, *list, **targets, **tmp;
	gboolean found = FALSE;

	if (st == NULL || st->monitored == NULL)
		return FALSE;
	params = event_get_params(data, 2, NULL, &list);
	targets = g_strsplit(list, ",", -1);
	for (tmp = targets; *tmp != NULL && !found; tmp++) {
		char *bang = strchr(*tmp, '!');

		if (bang != NULL)
			*bang = '\0';
		found = nickcmp(st->server, *tmp, st->monitored) == 0;
	}
	g_strfreev(targets);
	g_free(params);
	return found;
}

static void sig_event_730(IRC_SERVER_REC *server, const char *data, const char *nick,
                          const char *address)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);

	if (!monitor_lists(st, data)) {
		monitor_default(server, "730", data, nick, address);
		return;
	}
	st->offline = FALSE;
}

static void sig_event_731(IRC_SERVER_REC *server, const char *data, const char *nick,
                          const char *address)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);

	if (!monitor_lists(st, data)) {
		monitor_default(server, "731", data, nick, address);
		return;
	}
	st->offline = TRUE;
	attempt(server, TRUE);
}

static void sig_event_734(IRC_SERVER_REC *server, const char *data, const char *nick,
                          const char *address)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	char *params, *targets;
	gboolean ours;

	params = event_get_params(data, 3, NULL, NULL, &targets);
	ours = st != NULL && st->monitored != NULL && strstr(targets, st->monitored) != NULL;
	g_free(params);
	if (!ours) {
		monitor_default(server, "734", data, nick, address);
		return;
	}
	/* no room in the list: ISON from now on */
	g_free(st->monitored);
	st->monitored = NULL;
	st->monitor_failed = TRUE;
	keepnick_update(server, TRUE);
}

/* ---- keepnick.pl ---- */

static gboolean is_keepnick_script(PERL_SCRIPT_REC *script)
{
	return script != NULL && script->name != NULL &&
	       g_ascii_strncasecmp(script->name, "keepnick", 8) == 0;
}

/* The script name is known before its code runs: the built-in steps
   back before the script binds anything. */
static void sig_script_created(PERL_SCRIPT_REC *script)
{
	gboolean was = keepnick_script_loaded();

	if (!is_keepnick_script(script) || g_slist_find(keepnick_scripts, script) != NULL)
		return;
	keepnick_scripts = g_slist_prepend(keepnick_scripts, script);
	if (!was) {
		update_all(FALSE);
		signal_emit("keepnick script", 1, GINT_TO_POINTER(TRUE));
	}
}

static void sig_script_destroyed(PERL_SCRIPT_REC *script)
{
	if (g_slist_find(keepnick_scripts, script) == NULL)
		return;
	keepnick_scripts = g_slist_remove(keepnick_scripts, script);
	if (!keepnick_script_loaded()) {
		signal_emit("keepnick script", 1, GINT_TO_POINTER(FALSE));
		update_all(TRUE);
	}
}

/* ---- settings ---- */

static void read_settings(void)
{
	int old = check_time;
	GHashTableIter iter;
	KEEPNICK_SERVER_REC *st;

	check_time = settings_get_time("keepnick_check_time");
	if (check_time < CHECK_TIME_MIN)
		check_time = CHECK_TIME_MIN;
	if (old == 0 || old == check_time)
		return;

	/* the checks that run go on at the new pace */
	g_hash_table_iter_init(&iter, states);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &st)) {
		if (st->check_tag == 0)
			continue;
		g_source_remove(st->check_tag);
		st->check_tag = g_timeout_add(check_time, (GSourceFunc) sig_check, st->server);
	}
}

void irc_keepnick_init(void)
{
	states = g_hash_table_new(NULL, NULL);
	paused_nets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	keepnick_scripts = NULL;
	regaining = NULL;
	check_time = 0;

	settings_add_time("keepnick", "keepnick_check_time", "12s");
	settings_add_bool("keepnick", "keepnick_quiet", FALSE);
	settings_add_bool("keepnick", "keepnick_auto", TRUE);
	read_settings();

	server_redirect_register("keepnick nick", FALSE, 0, NULL,
	                         "event nick", 0,  /* the nick changed */
	                         "event 432", 1,   /* ERR_ERRONEUSNICKNAME */
	                         "event 433", 1,   /* ERR_NICKNAMEINUSE */
	                         "event 437", 1,   /* ERR_UNAVAILRESOURCE (nick delay) */
	                         "event 484", -1,  /* ERR_RESTRICTED */
	                         NULL, NULL);

	signal_add("event 433", (SIGNAL_FUNC) sig_nick_taken);
	signal_add("event 437", (SIGNAL_FUNC) sig_nick_taken);
	signal_add("event connected", (SIGNAL_FUNC) sig_event_connected);
	signal_add_last("event 376", (SIGNAL_FUNC) sig_end_of_motd);
	signal_add_last("event 422", (SIGNAL_FUNC) sig_end_of_motd);
	signal_add("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_add("server disconnected", (SIGNAL_FUNC) sig_server_gone);
	signal_add("server destroyed", (SIGNAL_FUNC) sig_server_gone);
	signal_add("event nick", (SIGNAL_FUNC) sig_event_nick);
	signal_add("event quit", (SIGNAL_FUNC) sig_event_quit);
	signal_add("event 730", (SIGNAL_FUNC) sig_event_730);
	signal_add("event 731", (SIGNAL_FUNC) sig_event_731);
	signal_add("event 734", (SIGNAL_FUNC) sig_event_734);
	signal_add("redir keepnick ison", (SIGNAL_FUNC) sig_redir_ison);
	signal_add("redir keepnick nick", (SIGNAL_FUNC) sig_redir_nick);
	signal_add("redir keepnick busy", (SIGNAL_FUNC) sig_redir_busy);
	signal_add("redir keepnick delayed", (SIGNAL_FUNC) sig_redir_delayed);
	signal_add("redir keepnick refused", (SIGNAL_FUNC) sig_redir_refused);
	signal_add("script created", (SIGNAL_FUNC) sig_script_created);
	signal_add("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);
	signal_add("chatnet created", (SIGNAL_FUNC) sig_chatnets_changed);
	signal_add("chatnet removed", (SIGNAL_FUNC) sig_chatnets_changed);
	signal_add("setup reread chatnets", (SIGNAL_FUNC) sig_chatnets_changed);
	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
}

void irc_keepnick_deinit(void)
{
	GHashTableIter iter;
	KEEPNICK_SERVER_REC *st;

	signal_remove("event 433", (SIGNAL_FUNC) sig_nick_taken);
	signal_remove("event 437", (SIGNAL_FUNC) sig_nick_taken);
	signal_remove("event connected", (SIGNAL_FUNC) sig_event_connected);
	signal_remove("event 376", (SIGNAL_FUNC) sig_end_of_motd);
	signal_remove("event 422", (SIGNAL_FUNC) sig_end_of_motd);
	signal_remove("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_remove("server disconnected", (SIGNAL_FUNC) sig_server_gone);
	signal_remove("server destroyed", (SIGNAL_FUNC) sig_server_gone);
	signal_remove("event nick", (SIGNAL_FUNC) sig_event_nick);
	signal_remove("event quit", (SIGNAL_FUNC) sig_event_quit);
	signal_remove("event 730", (SIGNAL_FUNC) sig_event_730);
	signal_remove("event 731", (SIGNAL_FUNC) sig_event_731);
	signal_remove("event 734", (SIGNAL_FUNC) sig_event_734);
	signal_remove("redir keepnick ison", (SIGNAL_FUNC) sig_redir_ison);
	signal_remove("redir keepnick nick", (SIGNAL_FUNC) sig_redir_nick);
	signal_remove("redir keepnick busy", (SIGNAL_FUNC) sig_redir_busy);
	signal_remove("redir keepnick delayed", (SIGNAL_FUNC) sig_redir_delayed);
	signal_remove("redir keepnick refused", (SIGNAL_FUNC) sig_redir_refused);
	signal_remove("script created", (SIGNAL_FUNC) sig_script_created);
	signal_remove("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);
	signal_remove("chatnet created", (SIGNAL_FUNC) sig_chatnets_changed);
	signal_remove("chatnet removed", (SIGNAL_FUNC) sig_chatnets_changed);
	signal_remove("setup reread chatnets", (SIGNAL_FUNC) sig_chatnets_changed);
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);

	if (update_tag != 0) {
		g_source_remove(update_tag);
		update_tag = 0;
	}
	g_hash_table_iter_init(&iter, states);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &st))
		state_free(st);
	g_hash_table_destroy(states);
	g_hash_table_destroy(paused_nets);
	g_slist_free(keepnick_scripts);
	keepnick_scripts = NULL;
	regaining = NULL;
}
