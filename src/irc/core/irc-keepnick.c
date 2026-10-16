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
    (/KEEPNICK sets it). The first connected server of the network takes
    it, as server_find_chatnet() finds it; when that one goes, the next
    one takes over;
  - keepnick_auto: the nick a connection was asked to use (connrec->nick:
    the network's nick or the global one), when it was taken at connect
    (433/437 before 001). For that connection only, never saved.
 A nick that cannot be sent as one IRC word (keepnick_nick_invalid()) is
 not kept; it is said once.

 How it is taken back, while the server is not on it:
  - MONITOR + <nick> when the server has MONITOR in 005: 731 (offline)
    tries;
  - otherwise ISON every keepnick_check_time;
  - the holder's NICK or QUIT seen in a shared channel tries.
 Never more than one NICK per check interval, whatever asks for it: a
 holder flipping nicks gets no NICK flood. The NICK goes through a
 redirect of its own (not the script's names): its 432/433/437/484 never
 print as errors. 432 (erroneous) and 484 (restricted) pause keeping, 433
 and 437 (IRCnet's nick delay) are tried again at the next check. When
 another client's NICK onto the nick ends the redirect first, the reply
 to ours that follows is still taken (see `awaiting').

 MONITOR replies about our nick (also the 730 the server sends when the
 nick becomes ours, before it read our MONITOR -) are not printed; the
 user's own MONITOR targets (seen in what is sent) still are, and our
 MONITOR - never removes one of them.

 A manual /NICK away from the kept nick (server->last_nick, set by /NICK)
 pauses keeping on that network until the nick is ours again or /KEEPNICK
 runs; a change the server forces (a collision, SVSNICK) does not.
 /UPGRADE keeps the state of a connection (session save/restore).

 Nothing at all is sent for a server with nothing to keep, nor while the
 Perl script keepnick is loaded: the script keeps the nicks then.
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
/* how long a MONITOR reply for a nick we just removed is still ours */
#define UNMONITOR_GRACE (30 * G_USEC_PER_SEC)

typedef struct {
	IRC_SERVER_REC *server;

	char *auto_nick;	/* keepnick_auto: kept for this connection */
	char *monitored;	/* the nick in our MONITOR list */
	char *unmonitored;	/* the nick of our last MONITOR - ... */
	gint64 unmonitored_until; /* ... and until when its replies are ours */
	GHashTable *user_monitor; /* the user's own MONITOR targets */
	char *told_invalid;	/* the invalid nick that was said */
	guint check_tag;	/* the check every keepnick_check_time */
	gint64 last_attempt;	/* monotonic time of our last NICK */

	unsigned int collided:1;	/* 433/437 before 001 */
	unsigned int auto_paused:1;	/* a manual /NICK paused auto_nick */
	unsigned int awaiting:1;	/* the reply to our NICK is still to come */
	unsigned int offline:1;		/* MONITOR: the nick is not on */
	unsigned int monitor_failed:1;	/* MONITOR list full: ISON */
	unsigned int monitor_resend:1;	/* after /UPGRADE: MONITOR + again */
} KEEPNICK_SERVER_REC;

static GHashTable *states;	/* IRC_SERVER_REC -> KEEPNICK_SERVER_REC */
static GHashTable *paused_nets;	/* lowercase chatnet -> 1 */
static GSList *keepnick_scripts; /* PERL_SCRIPT_REC of the script keepnick */
static gboolean script_announced; /* "the script is loaded" was said */
static IRC_SERVER_REC *regaining;
static gboolean sending_own;	/* our own MONITOR line is being sent */
static guint update_tag, script_tag;
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
		st->user_monitor = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		g_hash_table_insert(states, server, st);
	}
	return st;
}

static void state_free(KEEPNICK_SERVER_REC *st)
{
	if (st->check_tag != 0)
		g_source_remove(st->check_tag);
	g_hash_table_destroy(st->user_monitor);
	g_free(st->auto_nick);
	g_free(st->monitored);
	g_free(st->unmonitored);
	g_free(st->told_invalid);
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

const char *keepnick_nick_invalid(const char *nick, IRC_SERVER_REC *server)
{
	const char *nicklen;

	if (nick == NULL || *nick == '\0')
		return "empty";
	if (strpbrk(nick, "\r\n \t,!@") != NULL)
		return "it has a space, comma, ! or @, or a line break";
	if (strchr(":#&-", *nick) != NULL)
		return "it starts with :, #, & or -";
	nicklen = server == NULL || server->isupport == NULL ? NULL :
	          g_hash_table_lookup(server->isupport, "NICKLEN");
	if (nicklen != NULL && atoi(nicklen) > 0 && g_utf8_strlen(nick, -1) > atoi(nicklen))
		return "longer than the server's NICKLEN";
	return NULL;
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

/* a nick that cannot be kept: said once per server and nick */
static gboolean nick_usable(IRC_SERVER_REC *server, KEEPNICK_SERVER_REC *st, const char *nick)
{
	const char *why = keepnick_nick_invalid(nick, server);

	if (why == NULL)
		return TRUE;
	if (st == NULL && server->connected)
		st = state_get(server, TRUE);
	if (st != NULL && g_strcmp0(st->told_invalid, nick) != 0 && server->connected) {
		g_free(st->told_invalid);
		st->told_invalid = g_strdup(nick);
		signal_emit("keepnick invalid", 4, server, nick, server_label(server), why);
	}
	return FALSE;
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
	st = state_get(server, FALSE);
	if (net != NULL && net->keepnick != NULL) {
		/* another connection of the network keeps it */
		if (server_find_chatnet(net->name) != SERVER(server))
			return NULL;
		if (!nick_usable(server, st, net->keepnick))
			return NULL;
		*explicit = TRUE;
		*paused = keepnick_is_paused(net->name);
		return net->keepnick;
	}

	if (st == NULL || st->auto_nick == NULL || !nick_usable(server, st, st->auto_nick))
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

static gboolean user_monitors(KEEPNICK_SERVER_REC *st, const char *nick)
{
	char *key = g_utf8_casefold(nick, -1);
	gboolean found = g_hash_table_contains(st->user_monitor, key);

	g_free(key);
	return found;
}

/* our own MONITOR line, not to be taken for the user's */
static void send_monitor(IRC_SERVER_REC *server, const char *op, const char *nick)
{
	char *cmd = g_strdup_printf("MONITOR %s %s", op, nick);

	sending_own = TRUE;
	irc_send_cmd_now(server, cmd);
	sending_own = FALSE;
	g_free(cmd);
}

static void monitor_remove(KEEPNICK_SERVER_REC *st)
{
	if (st->monitored == NULL)
		return;
	/* the user's own entry stays */
	if (!user_monitors(st, st->monitored) && st->server->connected &&
	    !st->server->connection_lost)
		send_monitor(st->server, "-", st->monitored);
	/* its 730/731 that cross our MONITOR - are still ours */
	g_free(st->unmonitored);
	st->unmonitored = st->monitored;
	st->unmonitored_until = g_get_monotonic_time() + UNMONITOR_GRACE;
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
	st->awaiting = FALSE;
}

/* NICK <nick>, its replies redirected; at most one per check interval
   (a little less, the timer is not exact) */
static void attempt(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st;
	const char *nick;
	char *arg;
	gint64 now, gap;

	nick = wanted_nick(server);
	if (nick == NULL)
		return;
	st = state_get(server, TRUE);

	now = g_get_monotonic_time();
	gap = (gint64) check_time * 1000 - G_USEC_PER_SEC / 2;
	if (st->last_attempt != 0 && now - st->last_attempt < gap)
		return;

	st->last_attempt = now;
	st->awaiting = TRUE;

	/* the NICK reply has the nick as its first word (":nick" or "nick"),
	   the numerics as their second */
	arg = g_strdup_printf("%s :%s", nick, nick);
	server_redirect_event(server, "keepnick builtin nick", 1, arg, -1, NULL,
	                      "event nick", "redir keepnick builtin nick",
	                      "event 432", "redir keepnick builtin refused",
	                      "event 484", "redir keepnick builtin refused",
	                      "event 433", "redir keepnick builtin busy",
	                      "event 437", "redir keepnick builtin busy", NULL);
	g_free(arg);
	/* past the queue, as the script's send_raw: the nick is free now */
	arg = g_strdup_printf("NICK %s", nick);
	irc_send_cmd_now(server, arg);
	g_free(arg);
}

static void send_ison(IRC_SERVER_REC *server, const char *nick)
{
	server_redirect_event(server, "ison", 1, NULL, -1, NULL,
	                      "event 303", "redir keepnick builtin ison", NULL);
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
		if (st->monitored == NULL || nickcmp(server, st->monitored, nick) != 0 ||
		    st->monitor_resend) {
			monitor_remove(st);
			st->monitor_resend = FALSE;
			/* 730 or 731 comes back at once */
			send_monitor(server, "+", nick);
			st->monitored = g_strdup(nick);
		} else if (now && st->offline) {
			attempt(server);
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
			attempt(server);
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

/* the networks were read again (/RELOAD) or changed, or a server went
   (another one of its network may keep the nick now): once that is over */
static void sig_update_later(void)
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
		st = state_get(server, FALSE);
		if (st != NULL) {
			/* the network's nick replaces this connection's own */
			if (net->keepnick != NULL) {
				g_free(st->auto_nick);
				st->auto_nick = NULL;
				st->auto_paused = FALSE;
			}
			/* a new value is checked (and told) again */
			g_free(st->told_invalid);
			st->told_invalid = NULL;
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

/* another connection of ours is on the nick (of the same network) */
static gboolean own_server_on(IRC_SERVER_REC *server, const char *nick)
{
	GSList *tmp;

	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		IRC_SERVER_REC *other = IRC_SERVER(tmp->data);

		if (other == NULL || other == server || other->nick == NULL ||
		    g_strcmp0(other->connrec->chatnet, server->connrec->chatnet) != 0)
			continue;
		if (nickcmp(server, other->nick, nick) == 0)
			return TRUE;
	}
	return FALSE;
}

/* 001: keepnick_auto for a connection on another nick than the one it
   asked for, because that one was taken - not while the script keeps
   the nicks, not when the network keeps one of its own, not against
   another connection of ours */
static void sig_event_connected(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st;
	IRC_CHATNET_REC *net;
	const char *wanted;

	if (!IS_IRC_SERVER(server))
		return;
	st = state_get(server, FALSE);
	net = server_net(server);
	wanted = server->connrec->nick;
	if (st != NULL && st->collided && st->auto_nick == NULL && !keepnick_script_loaded() &&
	    settings_get_bool("keepnick_auto") && (net == NULL || net->keepnick == NULL) &&
	    wanted != NULL && *wanted != '\0' && nickcmp(server, server->nick, wanted) != 0 &&
	    keepnick_nick_invalid(wanted, NULL) == NULL && !own_server_on(server, wanted)) {
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

/* a connection restored after /UPGRADE has no 001 of its own */
static void sig_server_connected(IRC_SERVER_REC *server)
{
	if (IS_IRC_SERVER(server) && server->connected)
		keepnick_update(server, TRUE);
}

static void sig_server_gone(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st = g_hash_table_lookup(states, server);

	if (regaining == server)
		regaining = NULL;
	if (!IS_IRC_SERVER(server))
		return;
	if (st != NULL) {
		g_hash_table_remove(states, server);
		state_free(st);
	}
	/* another server of its network keeps the nick now */
	sig_update_later();
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
			attempt(server);
		g_free(params);
		return;
	}

	st = state_get(server, TRUE);
	/* /NICK sets last_nick; a nick the server forces does not match it */
	manual = !keepnick_regaining(SERVER(server)) && server->last_nick != NULL &&
	         nickcmp(server, server->last_nick, newnick) == 0;

	if (nickcmp(server, newnick, nick) == 0) {
		/* got it */
		st->awaiting = FALSE;
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
		attempt(server);
}

/* ---- the replies to our NICK ---- */

/* 432 erroneous, 484 restricted: trying again does not help */
static void refused(IRC_SERVER_REC *server, const char *reason)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	gboolean explicit, paused;
	const char *nick;

	if (st != NULL)
		st->awaiting = FALSE;
	nick = kept_nick(server, &explicit, &paused);
	if (nick == NULL || paused)
		return;
	if (explicit)
		net_pause(server_label(server));
	else if (st != NULL)
		st->auto_paused = TRUE;
	signal_emit("keepnick refused", 4, server, nick, server_label(server), reason);
	keepnick_update(server, FALSE);
}

static const char *refusal(const char *numeric)
{
	return g_strcmp0(numeric, "484") == 0 ? "restricted connection" : "erroneous nickname";
}

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
		attempt(server);
}

/* The NICK reply: the normal nick change, which fe-keepnick.c reports in
   its own words when it is ours. Another client's NICK onto the nick ends
   the redirect as well: it is passed on as it is, and the reply to our
   NICK that is still to come is caught by sig_late_reply(). */
static void sig_redir_nick(IRC_SERVER_REC *server, const char *data, const char *nick,
                           const char *address)
{
	IRC_SERVER_REC *old = regaining;
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	gboolean ours;

	ours = nick != NULL && nickcmp(server, nick, server->nick) == 0 && !keepnick_script_loaded();
	if (st != NULL && ours)
		st->awaiting = FALSE;
	if (ours)
		regaining = server;
	signal_emit("event nick", 4, server, data, nick, address);
	regaining = old;
}

static void sig_redir_busy(IRC_SERVER_REC *server)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);

	/* tried again at the next check */
	if (st != NULL)
		st->awaiting = FALSE;
}

static void sig_redir_refused(IRC_SERVER_REC *server)
{
	if (!keepnick_script_loaded())
		refused(server, refusal(current_server_event));
	else
		sig_redir_busy(server);
}

/* 432/433/437/484 after another client's NICK took our redirect: the
   reply to our NICK, not to the user's */
static void sig_late_reply(IRC_SERVER_REC *server, const char *data)
{
	KEEPNICK_SERVER_REC *st;
	const char *nick, *numeric = current_server_event;
	char *params, *target;
	gboolean ours;

	if (!IS_IRC_SERVER(server) || !server->connected)
		return;
	st = state_get(server, FALSE);
	if (st == NULL || !st->awaiting)
		return;
	/* only within the interval of our NICK */
	if (g_get_monotonic_time() - st->last_attempt > (gint64) check_time * 1000) {
		st->awaiting = FALSE;
		return;
	}
	nick = wanted_nick(server);
	params = event_get_params(data, 2, NULL, &target);
	ours = g_strcmp0(numeric, "484") == 0 ||
	       (nick != NULL && nickcmp(server, target, nick) == 0);
	g_free(params);
	if (!ours)
		return;

	signal_stop();
	st->awaiting = FALSE;
	if (g_strcmp0(numeric, "432") == 0 || g_strcmp0(numeric, "484") == 0)
		refused(server, refusal(numeric));
}

/* ---- MONITOR (730 online, 731 offline, 734 list full) ---- */

/* what the user sends: MONITOR + / - / C of their own */
static void sig_server_outgoing(IRC_SERVER_REC *server, GString *data)
{
	KEEPNICK_SERVER_REC *st;
	char **words, **targets, **tmp;

	if (sending_own || !IS_IRC_SERVER(server) || g_ascii_strncasecmp(data->str, "MONITOR ", 8) != 0)
		return;
	words = g_strsplit_set(data->str + 8, " \r\n", 3);
	st = state_get(server, TRUE);
	if (words[0] != NULL && g_ascii_strcasecmp(words[0], "C") == 0) {
		g_hash_table_remove_all(st->user_monitor);
	} else if (words[0] != NULL && words[1] != NULL &&
	           (strcmp(words[0], "+") == 0 || strcmp(words[0], "-") == 0)) {
		targets = g_strsplit(words[1] + (*words[1] == ':'), ",", -1);
		for (tmp = targets; *tmp != NULL; tmp++) {
			if (**tmp == '\0')
				continue;
			if (*words[0] == '+')
				g_hash_table_add(st->user_monitor, g_utf8_casefold(*tmp, -1));
			else {
				char *key = g_utf8_casefold(*tmp, -1);

				g_hash_table_remove(st->user_monitor, key);
				g_free(key);
			}
		}
		g_strfreev(targets);
	}
	g_strfreev(words);
}

/* a MONITOR reply target about our nick (the one we monitor, or the one
   we just removed) that the user does not monitor */
static gboolean monitor_ours(KEEPNICK_SERVER_REC *st, const char *target)
{
	if (user_monitors(st, target))
		return FALSE;
	if (st->monitored != NULL && nickcmp(st->server, target, st->monitored) == 0)
		return TRUE;
	return st->unmonitored != NULL && g_get_monotonic_time() < st->unmonitored_until &&
	       nickcmp(st->server, target, st->unmonitored) == 0;
}

/* 730/731: our targets are taken out of the list (`found' says if the
   monitored nick was in it), the rest (the user's own targets too) is
   printed as an unknown numeric would be */
static void monitor_reply(IRC_SERVER_REC *server, const char *numeric, const char *data,
                          const char *nick, const char *address, gboolean *found)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	char *params, *me, *list, **targets, **tmp, *line;
	GString *rest;

	*found = FALSE;
	params = event_get_params(data, 2, &me, &list);
	rest = g_string_new(NULL);
	targets = g_strsplit(list, ",", -1);
	for (tmp = targets; *tmp != NULL; tmp++) {
		char *name = g_strdup(*tmp), *bang = strchr(name, '!');

		if (bang != NULL)
			*bang = '\0';
		/* the monitored nick counts also when the user monitors it,
		   it is printed then */
		if (st != NULL && st->monitored != NULL && nickcmp(server, name, st->monitored) == 0)
			*found = TRUE;
		if ((st == NULL || !monitor_ours(st, name)) && **tmp != '\0')
			g_string_append_printf(rest, "%s%s", rest->len > 0 ? "," : "", *tmp);
		g_free(name);
	}
	g_strfreev(targets);

	if (rest->len > 0) {
		line = g_strdup_printf("%s %s :%s", numeric, me, rest->str);
		signal_emit("default event", 4, server, line, nick, address);
		g_free(line);
	}
	g_string_free(rest, TRUE);
	g_free(params);
}

static void sig_event_730(IRC_SERVER_REC *server, const char *data, const char *nick,
                          const char *address)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	gboolean found;

	monitor_reply(server, "730", data, nick, address, &found);
	if (found)
		st->offline = FALSE;
}

static void sig_event_731(IRC_SERVER_REC *server, const char *data, const char *nick,
                          const char *address)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	gboolean found;

	monitor_reply(server, "731", data, nick, address, &found);
	if (found) {
		st->offline = TRUE;
		attempt(server);
	}
}

static void sig_event_734(IRC_SERVER_REC *server, const char *data, const char *nick,
                          const char *address)
{
	KEEPNICK_SERVER_REC *st = state_get(server, FALSE);
	char *params, *targets, **list, **tmp, *line;
	gboolean ours = FALSE;

	params = event_get_params(data, 3, NULL, NULL, &targets);
	list = g_strsplit(targets, ",", -1);
	for (tmp = list; *tmp != NULL && !ours; tmp++)
		ours = st != NULL && st->monitored != NULL && nickcmp(server, *tmp, st->monitored) == 0;
	g_strfreev(list);
	g_free(params);
	if (!ours) {
		line = g_strconcat("734 ", data, NULL);
		signal_emit("default event", 4, server, line, nick, address);
		g_free(line);
		return;
	}
	/* no room in the list: ISON from now on */
	g_free(st->monitored);
	st->monitored = NULL;
	st->monitor_failed = TRUE;
	keepnick_update(server, TRUE);
}

/* ---- /UPGRADE ---- */

static void sig_session_save_server(IRC_SERVER_REC *server, CONFIG_REC *config, CONFIG_NODE *node)
{
	KEEPNICK_SERVER_REC *st;

	if (!IS_IRC_SERVER(server))
		return;
	st = state_get(server, FALSE);
	if (st != NULL && st->auto_nick != NULL) {
		config_node_set_str(config, node, "keepnick_auto_nick", st->auto_nick);
		config_node_set_bool(config, node, "keepnick_auto_paused", st->auto_paused);
	}
	if (st != NULL && st->monitored != NULL)
		config_node_set_str(config, node, "keepnick_monitored", st->monitored);
	if (st != NULL && g_hash_table_size(st->user_monitor) > 0) {
		GHashTableIter iter;
		GString *list = g_string_new(NULL);
		gpointer key;

		g_hash_table_iter_init(&iter, st->user_monitor);
		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_string_append_printf(list, "%s%s", list->len > 0 ? "," : "", (char *) key);
		config_node_set_str(config, node, "keepnick_user_monitor", list->str);
		g_string_free(list, TRUE);
	}
	if (keepnick_is_paused(server_label(server)))
		config_node_set_bool(config, node, "keepnick_paused", TRUE);
}

static void sig_session_restore_server(IRC_SERVER_REC *server, CONFIG_NODE *node)
{
	KEEPNICK_SERVER_REC *st;
	const char *value;

	if (!IS_IRC_SERVER(server))
		return;
	value = config_node_get_str(node, "keepnick_auto_nick", NULL);
	if (value != NULL && *value != '\0') {
		st = state_get(server, TRUE);
		st->auto_nick = g_strdup(value);
		st->auto_paused = config_node_get_bool(node, "keepnick_auto_paused", FALSE);
	}
	value = config_node_get_str(node, "keepnick_monitored", NULL);
	if (value != NULL && *value != '\0') {
		/* still in the server's list: MONITOR + again tells if it is
		   on (the answer is ours), and a nick kept no more is
		   removed */
		st = state_get(server, TRUE);
		st->monitored = g_strdup(value);
		st->monitor_resend = TRUE;
	}
	value = config_node_get_str(node, "keepnick_user_monitor", NULL);
	if (value != NULL && *value != '\0') {
		char **names = g_strsplit(value, ",", -1), **tmp;

		st = state_get(server, TRUE);
		for (tmp = names; *tmp != NULL; tmp++) {
			if (**tmp != '\0')
				g_hash_table_add(st->user_monitor, g_strdup(*tmp));
		}
		g_strfreev(names);
	}
	if (config_node_get_bool(node, "keepnick_paused", FALSE) && server->connrec->chatnet != NULL)
		net_pause(server->connrec->chatnet);
}

/* ---- keepnick.pl ---- */

static gboolean is_keepnick_script(PERL_SCRIPT_REC *script)
{
	return script != NULL && script->name != NULL &&
	       g_ascii_strcasecmp(script->name, "keepnick") == 0;
}

/* The script name is known before its code runs: the built-in steps
   back before the script binds anything. Loaded again (/SCRIPT LOAD of a
   loaded script unloads it first): nothing said, nothing sent. */
static void sig_script_created(PERL_SCRIPT_REC *script)
{
	if (!is_keepnick_script(script) || g_slist_find(keepnick_scripts, script) != NULL)
		return;
	keepnick_scripts = g_slist_prepend(keepnick_scripts, script);
	if (!script_announced) {
		script_announced = TRUE;
		update_all(FALSE);
		signal_emit("keepnick script", 1, GINT_TO_POINTER(TRUE));
	}
}

static gboolean script_gone(gpointer data)
{
	script_tag = 0;
	if (keepnick_script_loaded() || !script_announced)
		return FALSE;
	script_announced = FALSE;
	signal_emit("keepnick script", 1, GINT_TO_POINTER(FALSE));
	update_all(TRUE);
	return FALSE;
}

static void sig_script_destroyed(PERL_SCRIPT_REC *script)
{
	if (g_slist_find(keepnick_scripts, script) == NULL)
		return;
	keepnick_scripts = g_slist_remove(keepnick_scripts, script);
	/* once the unload (and a load again) is over */
	if (!keepnick_script_loaded() && script_tag == 0)
		script_tag = g_idle_add(script_gone, NULL);
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
	script_announced = FALSE;
	regaining = NULL;
	check_time = 0;

	settings_add_time("keepnick", "keepnick_check_time", "12s");
	settings_add_bool("keepnick", "keepnick_quiet", FALSE);
	settings_add_bool("keepnick", "keepnick_auto", TRUE);
	settings_add_bool("keepnick", "keepnick_autosave", TRUE);
	read_settings();

	/* names of its own: the script registers "keepnick nick" */
	server_redirect_register("keepnick builtin nick", FALSE, 0, NULL,
	                         "event nick", 0,  /* the nick changed */
	                         "event 432", 1,   /* ERR_ERRONEUSNICKNAME */
	                         "event 433", 1,   /* ERR_NICKNAMEINUSE */
	                         "event 437", 1,   /* ERR_UNAVAILRESOURCE (nick delay) */
	                         "event 484", -1,  /* ERR_RESTRICTED */
	                         NULL, NULL);

	signal_add("event 433", (SIGNAL_FUNC) sig_nick_taken);
	signal_add("event 437", (SIGNAL_FUNC) sig_nick_taken);
	signal_add_first("event 432", (SIGNAL_FUNC) sig_late_reply);
	signal_add_first("event 433", (SIGNAL_FUNC) sig_late_reply);
	signal_add_first("event 437", (SIGNAL_FUNC) sig_late_reply);
	signal_add_first("event 484", (SIGNAL_FUNC) sig_late_reply);
	signal_add("event connected", (SIGNAL_FUNC) sig_event_connected);
	signal_add_last("event 376", (SIGNAL_FUNC) sig_end_of_motd);
	signal_add_last("event 422", (SIGNAL_FUNC) sig_end_of_motd);
	signal_add("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_add("server disconnected", (SIGNAL_FUNC) sig_server_gone);
	signal_add("server destroyed", (SIGNAL_FUNC) sig_server_gone);
	signal_add("server outgoing modify", (SIGNAL_FUNC) sig_server_outgoing);
	signal_add("event nick", (SIGNAL_FUNC) sig_event_nick);
	signal_add("event quit", (SIGNAL_FUNC) sig_event_quit);
	signal_add("event 730", (SIGNAL_FUNC) sig_event_730);
	signal_add("event 731", (SIGNAL_FUNC) sig_event_731);
	signal_add("event 734", (SIGNAL_FUNC) sig_event_734);
	signal_add("redir keepnick builtin ison", (SIGNAL_FUNC) sig_redir_ison);
	signal_add("redir keepnick builtin nick", (SIGNAL_FUNC) sig_redir_nick);
	signal_add("redir keepnick builtin busy", (SIGNAL_FUNC) sig_redir_busy);
	signal_add("redir keepnick builtin refused", (SIGNAL_FUNC) sig_redir_refused);
	signal_add("session save server", (SIGNAL_FUNC) sig_session_save_server);
	signal_add("session restore server", (SIGNAL_FUNC) sig_session_restore_server);
	signal_add("script created", (SIGNAL_FUNC) sig_script_created);
	signal_add("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);
	signal_add("chatnet created", (SIGNAL_FUNC) sig_update_later);
	signal_add("chatnet removed", (SIGNAL_FUNC) sig_update_later);
	signal_add("setup reread chatnets", (SIGNAL_FUNC) sig_update_later);
	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
}

void irc_keepnick_deinit(void)
{
	GHashTableIter iter;
	KEEPNICK_SERVER_REC *st;

	signal_remove("event 433", (SIGNAL_FUNC) sig_nick_taken);
	signal_remove("event 437", (SIGNAL_FUNC) sig_nick_taken);
	signal_remove("event 432", (SIGNAL_FUNC) sig_late_reply);
	signal_remove("event 433", (SIGNAL_FUNC) sig_late_reply);
	signal_remove("event 437", (SIGNAL_FUNC) sig_late_reply);
	signal_remove("event 484", (SIGNAL_FUNC) sig_late_reply);
	signal_remove("event connected", (SIGNAL_FUNC) sig_event_connected);
	signal_remove("event 376", (SIGNAL_FUNC) sig_end_of_motd);
	signal_remove("event 422", (SIGNAL_FUNC) sig_end_of_motd);
	signal_remove("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_remove("server disconnected", (SIGNAL_FUNC) sig_server_gone);
	signal_remove("server destroyed", (SIGNAL_FUNC) sig_server_gone);
	signal_remove("server outgoing modify", (SIGNAL_FUNC) sig_server_outgoing);
	signal_remove("event nick", (SIGNAL_FUNC) sig_event_nick);
	signal_remove("event quit", (SIGNAL_FUNC) sig_event_quit);
	signal_remove("event 730", (SIGNAL_FUNC) sig_event_730);
	signal_remove("event 731", (SIGNAL_FUNC) sig_event_731);
	signal_remove("event 734", (SIGNAL_FUNC) sig_event_734);
	signal_remove("redir keepnick builtin ison", (SIGNAL_FUNC) sig_redir_ison);
	signal_remove("redir keepnick builtin nick", (SIGNAL_FUNC) sig_redir_nick);
	signal_remove("redir keepnick builtin busy", (SIGNAL_FUNC) sig_redir_busy);
	signal_remove("redir keepnick builtin refused", (SIGNAL_FUNC) sig_redir_refused);
	signal_remove("session save server", (SIGNAL_FUNC) sig_session_save_server);
	signal_remove("session restore server", (SIGNAL_FUNC) sig_session_restore_server);
	signal_remove("script created", (SIGNAL_FUNC) sig_script_created);
	signal_remove("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);
	signal_remove("chatnet created", (SIGNAL_FUNC) sig_update_later);
	signal_remove("chatnet removed", (SIGNAL_FUNC) sig_update_later);
	signal_remove("setup reread chatnets", (SIGNAL_FUNC) sig_update_later);
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);

	if (update_tag != 0) {
		g_source_remove(update_tag);
		update_tag = 0;
	}
	if (script_tag != 0) {
		g_source_remove(script_tag);
		script_tag = 0;
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
