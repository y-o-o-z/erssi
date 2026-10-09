/*
 e2e-core.c : RPE2E end-to-end encryption in erssi - the signal handlers

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The native replacement of the rpe2e.pl script (0.2.2), wire-compatible
 with repartee, rpe2e.pl and the WeeChat rpe2e.py. The hooks are the
 script's:

  - "server outgoing modify": THE outbound gate. irssi emits it for every
    line it is about to write to the socket, so every PRIVMSG - typed
    text, /me, /msg, /say, /quote, scripts, the web client - passes here.
    On an encrypted context the line is replaced by its ciphertext or
    dropped (the GString emptied: irc_server_send_and_redirect writes
    nothing then); it is never sent in clear text. A buffer holding
    several IRC lines (CR/LF from a script's raw line) is dropped whole
    when one of its PRIVMSGs would need the gate. Only the exact lines of
    ciphertext the gate itself sends pass unexamined.
  - "session save server" (first): /upgrade writes the flood queue to the
    socket directly, past the gate; the module sends those lines through
    irc_server_send_and_redirect first, so the gate sees each of them.
  - "event privmsg" (first): decryption before irssi splits CTCP/ACTION,
    so an encrypted /me renders as an action; the decrypted text is
    passed on with signal_continue, everything after (fe-common, fe-web,
    logs) sees plain text.
  - "ctcp reply" (first): the key exchange NOTICEs (RPEE2E KEYREQ ...).
  - 001 / 311 / JOIN / CHGHOST / 396: our own ident@host as peers see it,
    the context of the DMs we receive.

 If rpe2e.pl is loaded at the same time, both would encrypt and decrypt:
 the module notices the script's /e2e command (a Perl script named rpe2e*
 binding it; another script binding /e2e does not count) and stays
 inactive until the script is unloaded.
*/

#include "module.h"
#include "e2e-core.h"
#include "e2e-crypto.h"
#include "e2e-formats.h"
#include "e2e-keyring.h"
#include "e2e-wire.h"

#include <irssi/src/core/channels.h>
#include <irssi/src/core/commands.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/modules.h>
#include <irssi/src/core/nicklist.h>
#include <irssi/src/core/queries.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/servers-redirect.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/themes.h>
#include <irssi/src/fe-common/core/window-items.h>
#include <irssi/src/perl/perl-core.h>

#include <gmodule.h>
#include <sys/stat.h>

static char *keyring_dir;
static char *keyring_file;

static GHashTable *keyreq_out;		/* handle -> our last KEYREQ (30 s) */
static GHashTable *keyreq_in;		/* "kind|handle|ctx" -> their last handshake (10 s) */
static GHashTable *own_wait_notice;	/* "tag|nick" -> last "held" notice */
static GHashTable *own_handles;		/* server tag -> own ident@host */
/* the exact lines of ciphertext the gate is sending -> how many: these
   pass the gate (any other line, a wire too, is gated) */
static GHashTable *own_lines;
/* gate calls inside gate calls (a script sending from a signal emitted
   while we send): bounded */
static int gate_depth;
#define E2E_MAX_GATE_DEPTH 4

static gint64 lost_notice_at;

static gboolean script_seen;
static guint script_check_id;

/* ---- printing ---- */

void e2e_print_item(void *item, const char *fmt, ...)
{
	WI_ITEM_REC *wi = item;
	va_list va;
	char *text;

	va_start(va, fmt);
	text = g_strdup_vprintf(fmt, va);
	va_end(va);
	if (wi != NULL)
		printformat(wi->server, wi->visible_name, MSGLEVEL_CLIENTCRAP, TXT_E2E_MESSAGE, text);
	else
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, TXT_E2E_MESSAGE, text);
	g_free(text);
}

void *e2e_notice_item(SERVER_REC *server, const char *ctx, const char *nick)
{
	WI_ITEM_REC *item;

	if (server == NULL)
		return NULL;
	if (ctx != NULL && e2e_is_channel(ctx)) {
		item = window_item_find(server, ctx);
		if (item != NULL)
			return item;
	}
	if (nick != NULL && *nick != '\0')
		return window_item_find(server, nick);
	return NULL;
}

/* rpe2e.pl's RPE2E_DEBUG_BUFFER, as a setting */
void e2e_print_debug(SERVER_REC *server, const char *ctx, const char *nick,
                     const char *fmt, ...)
{
	WI_ITEM_REC *item;
	va_list va;
	char *text;

	if (!settings_get_bool("e2e_debug"))
		return;
	va_start(va, fmt);
	text = g_strdup_vprintf(fmt, va);
	va_end(va);
	item = e2e_notice_item(server, ctx, nick);
	if (item != NULL)
		printformat(item->server, item->visible_name, MSGLEVEL_CLIENTCRAP, TXT_E2E_DEBUG, text);
	else
		printformat(NULL, NULL, MSGLEVEL_CLIENTCRAP, TXT_E2E_DEBUG, text);
	g_free(text);
}

/* ---- rpe2e.pl loaded as well ---- */

/* a Perl script named rpe2e (rpe2e.pl, rpe2e-0.2.2.pl, ...) is loaded:
   perl_scripts of the perl module, looked up at run time - the module
   may not be loaded at all */
static gboolean rpe2e_script_running(void)
{
	MODULE_REC *perl = module_find("perl");
	MODULE_FILE_REC *file = perl != NULL ? module_file_find(perl, "core") : NULL;
	GModule *self = NULL, *gm;
	gpointer sym = NULL;
	gboolean found = FALSE;
	GSList *tmp;

	if (file == NULL)
		return FALSE;
	gm = file->gmodule;
	if (gm == NULL)
		gm = self = g_module_open(NULL, 0);	/* perl built in */
	if (gm != NULL && g_module_symbol(gm, "perl_scripts", &sym) && sym != NULL) {
		for (tmp = *(GSList **) sym; tmp != NULL && !found; tmp = tmp->next) {
			PERL_SCRIPT_REC *script = tmp->data;
			char *name = g_ascii_strdown(script->name != NULL ? script->name : "", -1);

			found = strstr(name, "rpe2e") != NULL;
			g_free(name);
		}
	}
	if (self != NULL)
		g_module_close(self);
	return found;
}

/* the script binds /e2e too. Only rpe2e.pl counts: another script or
   module binding the name must not switch the outbound gate off. */
static gboolean script_loaded(void)
{
	COMMAND_REC *rec = command_find("e2e");
	GSList *tmp;

	if (rec == NULL)
		return FALSE;
	for (tmp = rec->modules; tmp != NULL; tmp = tmp->next) {
		COMMAND_MODULE_REC *modrec = tmp->data;

		if (strcmp(modrec->name, "perl/core") == 0 && modrec->callbacks != NULL)
			return rpe2e_script_running();
	}
	return FALSE;
}

gboolean e2e_native_active(void)
{
	gboolean script = script_loaded();

	if (script && !script_seen) {
		script_seen = TRUE;
		e2e_print_item(NULL, "the rpe2e.pl script is loaded: the native e2e module leaves encryption to it and stays inactive. Unload the script (/script unload rpe2e, and remove it from scripts/autorun) to use the native module - it uses the same keyring, no new keys are needed.");
	} else if (!script && script_seen) {
		script_seen = FALSE;
		e2e_print_item(NULL, "rpe2e.pl is no longer loaded: the native e2e module encrypts now (same keyring)");
	}
	return !script;
}

static void sig_commandlist_new(COMMAND_REC *rec)
{
	if (rec != NULL && g_strcmp0(rec->cmd, "e2e") == 0)
		e2e_native_active();
}

static gboolean script_check(gpointer data)
{
	script_check_id = 0;
	e2e_native_active();
	return FALSE;
}

/* the script's command is unbound while it is being destroyed: look
   once that is over */
static void sig_script_destroyed(void)
{
	if (script_check_id == 0)
		script_check_id = g_idle_add(script_check, NULL);
}

/* ---- the keyring ---- */

const char *e2e_keyring_file(void)
{
	return keyring_file;
}

gboolean e2e_keyring_is_lost(void)
{
	return e2e_keyring_lost(keyring_file);
}

E2E_JSON *e2e_load_full(gboolean *lost)
{
	E2E_LOAD_STATUS status;
	char *notice = NULL;
	gboolean was_lost;
	E2E_JSON *kr;

	/* the directory 0700, as rpe2e.pl keeps it */
	chmod(keyring_dir, 0700);
	was_lost = e2e_keyring_lost(keyring_file);
	kr = e2e_keyring_load_status(keyring_file, &status, &notice);
	if (lost != NULL)
		*lost = status == E2E_LOAD_LOST;
	/* moved aside just now: always loud; still lost: once a minute */
	if (notice != NULL && (status != E2E_LOAD_LOST || !was_lost ||
	                       e2e_now() - lost_notice_at >= 60)) {
		if (status == E2E_LOAD_LOST)
			lost_notice_at = e2e_now();
		e2e_print_item(NULL, "%s", notice);
	}
	g_free(notice);
	return kr;
}

E2E_JSON *e2e_load(void)
{
	return e2e_load_full(NULL);
}

gboolean e2e_save(const E2E_JSON *kr)
{
	char *error = NULL;

	if (e2e_keyring_save(keyring_file, kr, &error))
		return TRUE;
	e2e_print_item(NULL, "%s", error);
	g_free(error);
	return FALSE;
}

gboolean e2e_ensure_identity(E2E_JSON *kr, E2E_IDENTITY *id)
{
	gboolean created;
	char *error = NULL;

	if (!e2e_identity_get(kr, id, TRUE, &created, &error)) {
		e2e_print_item(NULL, "no identity loaded: %s", error);
		g_free(error);
		return FALSE;
	}
	/* an identity that cannot be stored would be a throw-away key peers
	   pin and then see change */
	if (created && !e2e_save(kr)) {
		e2e_print_item(NULL, "cannot store a new E2E identity in %s", keyring_file);
		e2e_json_set(kr, "identity", e2e_json_new(E2E_JSON_NULL));
		e2e_identity_wipe(id);
		return FALSE;
	}
	return TRUE;
}

GHashTable *e2e_keyreq_out_stamps(void)
{
	return keyreq_out;
}

/* through the flood queue, like every other command: a burst of
   handshakes must not get us disconnected for Excess Flood */
void e2e_send_notice(SERVER_REC *server, const char *nick, const char *body)
{
	char *cmd;

	if (!IS_IRC_SERVER(server))
		return;
	cmd = g_strdup_printf("NOTICE %s :%s", nick, body);
	irc_send_cmd(IRC_SERVER(server), cmd);
	g_free(cmd);
}

/* a REKEY goes out right before the first message under the new key
   (which the gate sends at once): queued, it would arrive after it */
static void send_notice_now(SERVER_REC *server, const char *nick, const char *body)
{
	char *cmd = g_strdup_printf("NOTICE %s :%s", nick, body);

	irc_send_cmd_now(IRC_SERVER(server), cmd);
	g_free(cmd);
}

/* ---- handles ---- */

char *e2e_resolve_dm_handle(SERVER_REC *server, const E2E_JSON *kr, const char *nick)
{
	const char *cached;
	GSList *tmp;

	/* LIVE first: an open query or a common channel has the peer's
	   current ident@host, which a nick change does not alter (the
	   keyring's last_nick can be stale) */
	if (server != NULL) {
		QUERY_REC *query = query_find(server, nick);

		if (query != NULL && query->address != NULL && *query->address != '\0')
			return g_strdup(query->address);
		for (tmp = server->channels; tmp != NULL; tmp = tmp->next) {
			NICK_REC *rec = nicklist_find(tmp->data, nick);

			if (rec != NULL && rec->host != NULL && *rec->host != '\0')
				return g_strdup(rec->host);
		}
	}
	cached = e2e_find_handle_by_nick(kr, nick);
	return cached != NULL && *cached != '\0' ? g_strdup(cached) : NULL;
}

static char *resolve_cb(const char *nick, const E2E_JSON *kr, void *data)
{
	return e2e_resolve_dm_handle(data, kr, nick);
}

/* Only PREFIX-VISIBLE values are stored - what peers see in our message
   prefix: our JOIN, CHGHOST, RPL_HOSTHIDDEN (396), the self-WHOIS 311 and
   our nick in a nicklist. A self-USERHOST can answer with the real host
   and server->userhost is not updated on CHGHOST, so neither is used. */
static void set_own_handle(SERVER_REC *server, const char *handle)
{
	if (server == NULL || server->tag == NULL || handle == NULL || *handle == '\0')
		return;
	if (g_strcmp0(g_hash_table_lookup(own_handles, server->tag), handle) != 0)
		g_hash_table_replace(own_handles, g_strdup(server->tag), g_strdup(handle));
}

const char *e2e_own_handle(SERVER_REC *server)
{
	const char *handle;
	GSList *tmp;

	if (server == NULL || server->tag == NULL)
		return NULL;
	handle = g_hash_table_lookup(own_handles, server->tag);
	if (handle != NULL)
		return handle;
	if (server->nick != NULL && *server->nick != '\0') {
		for (tmp = server->channels; tmp != NULL; tmp = tmp->next) {
			NICK_REC *rec = nicklist_find(tmp->data, server->nick);

			if (rec != NULL && rec->host != NULL && *rec->host != '\0') {
				set_own_handle(server, rec->host);
				return g_hash_table_lookup(own_handles, server->tag);
			}
		}
	}
	return NULL;
}

char *e2e_incoming_ctx_for(SERVER_REC *server, const char *ctx)
{
	const char *own;

	if (ctx == NULL || *ctx != '@')
		return g_strdup(ctx);
	own = e2e_own_handle(server);
	return own != NULL && *own != '\0' ? g_strconcat("@", own, NULL) : NULL;
}

/* a one-shot WHOIS of ourselves: its 311 has the displayed host, exactly
   what peers see. Redirected, so its reply is not printed. */
static void send_self_whois(SERVER_REC *server)
{
	IRC_SERVER_REC *irc = IRC_SERVER(server);
	char *cmd;

	if (irc == NULL || !irc->connected || irc->nick == NULL || *irc->nick == '\0')
		return;
	server_redirect_event(irc, "whois", 1, irc->nick, -1, NULL,
	                      "event 311", "redir e2e whois user",
	                      "", "event empty", NULL);
	cmd = g_strdup_printf("WHOIS %s", irc->nick);
	irc_send_cmd(irc, cmd);
	g_free(cmd);
}

static gboolean is_own_nick(SERVER_REC *server, const char *nick)
{
	return server != NULL && nick != NULL && server->nick != NULL &&
	       g_ascii_strcasecmp(nick, server->nick) == 0;
}

/* registration complete: the server may give us another ident, host or
   cloak this time */
static void sig_event_001(SERVER_REC *server)
{
	if (!IS_IRC_SERVER(server))
		return;
	if (server->tag != NULL)
		g_hash_table_remove(own_handles, server->tag);
	send_self_whois(server);
}

/* RPL_WHOISUSER: "me nick ident host * :realname" */
static void sig_event_311(SERVER_REC *server, const char *data)
{
	char **f;

	if (!IS_IRC_SERVER(server) || data == NULL)
		return;
	f = g_strsplit(data, " ", 5);
	if (g_strv_length(f) >= 4 && is_own_nick(server, f[1]) && *f[2] != '\0' && *f[3] != '\0') {
		char *handle = g_strconcat(f[2], "@", f[3], NULL);

		set_own_handle(server, handle);
		g_free(handle);
	}
	g_strfreev(f);
}

/* our own JOIN carries our prefix exactly as peers see it */
static void sig_event_join(SERVER_REC *server, const char *data, const char *nick,
                           const char *address)
{
	if (IS_IRC_SERVER(server) && is_own_nick(server, nick) && address != NULL)
		set_own_handle(server, address);
}

/* RPL_HOSTHIDDEN: our new displayed host; the checks of irssi core's
   event_hosthidden */
static void sig_event_396(SERVER_REC *server, const char *data)
{
	const char *p, *own, *at;
	char *host, *handle, *ident;

	if (!IS_IRC_SERVER(server) || data == NULL)
		return;
	p = strchr(data, ' ');
	if (p == NULL)
		return;
	while (*p == ' ')
		p++;
	if (*p == ':')
		p++;
	host = g_strndup(p, strcspn(p, " "));
	if (*host == '\0' || strpbrk(host, "*?!#& \t") != NULL || strchr("@:-", *host) != NULL ||
	    host[strlen(host) - 1] == '-') {
		g_free(host);
		return;
	}
	at = strchr(host, '@');
	if (at != NULL && at > host) {
		set_own_handle(server, host);
	} else {
		own = e2e_own_handle(server);
		at = own != NULL ? strchr(own, '@') : NULL;
		if (at != NULL && at > own) {
			ident = g_strndup(own, at - own);
			handle = g_strconcat(ident, "@", host, NULL);
			set_own_handle(server, handle);
			g_free(handle);
			g_free(ident);
		}
	}
	g_free(host);
}

/* CHGHOST "newuser newhost" of us */
static void sig_event_chghost(SERVER_REC *server, const char *data, const char *nick)
{
	char **f;

	if (!IS_IRC_SERVER(server) || data == NULL || !is_own_nick(server, nick))
		return;
	f = g_strsplit(data, " ", 3);
	if (g_strv_length(f) >= 2) {
		const char *host = *f[1] == ':' ? f[1] + 1 : f[1];

		if (*f[0] != '\0' && *host != '\0') {
			char *handle = g_strconcat(f[0], "@", host, NULL);

			set_own_handle(server, handle);
			g_free(handle);
		}
	}
	g_strfreev(f);
}

/* ---- the outbound gate ---- */

static void own_line_add(const char *line)
{
	int n = GPOINTER_TO_INT(g_hash_table_lookup(own_lines, line));

	g_hash_table_replace(own_lines, g_strdup(line), GINT_TO_POINTER(n + 1));
}

/* TRUE (and counted off) when line is one the gate is sending */
static gboolean own_line_take(const char *line)
{
	int n = GPOINTER_TO_INT(g_hash_table_lookup(own_lines, line));

	if (n <= 0)
		return FALSE;
	if (n == 1)
		g_hash_table_remove(own_lines, line);
	else
		g_hash_table_replace(own_lines, g_strdup(line), GINT_TO_POINTER(n - 1));
	return TRUE;
}

static void send_privmsg(SERVER_REC *server, const char *tags, const char *target,
                         const char *body)
{
	/* IRCv3 tags of the original line stay on the chunks */
	char *cmd = g_strdup_printf("%sPRIVMSG %s :%s", tags, target, body);

	own_line_add(cmd);
	irc_send_cmd_now(IRC_SERVER(server), cmd);
	/* not taken when it never reached the socket (connection lost) */
	own_line_take(cmd);
	g_free(cmd);
}

static void gate_refuse(SERVER_REC *server, GString *str, const char *item, const char *msg)
{
	/* irssi already showed the line: say that it was not delivered */
	e2e_print_item(e2e_notice_item(server, item, item),
	               "%s — the message shown above was NOT delivered", msg);
	g_string_truncate(str, 0);
	signal_stop();
}

/* several IRC lines in one buffer (a script's raw line with CR/LF): the
   gate looks at one line, the server splits them */
static gboolean gate_multiline(SERVER_REC *server, GString *str, gsize len)
{
	E2E_LOAD_STATUS status;
	E2E_JSON *kr;
	char *error = NULL, *msg;

	if (memchr(str->str, '\r', len) == NULL && memchr(str->str, '\n', len) == NULL &&
	    memchr(str->str, '\0', len) == NULL)
		return FALSE;
	kr = e2e_keyring_load_checked(keyring_file, &status, &error);
	g_free(error);
	msg = e2e_gate_multiline(kr, status == E2E_LOAD_OK, str->str, len, resolve_cb, server);
	if (msg != NULL) {
		e2e_print_item(NULL, "%s", msg);
		g_string_truncate(str, 0);
		signal_stop();
		g_free(msg);
	}
	e2e_json_free(kr);
	return TRUE;
}

static void sig_server_outgoing(SERVER_REC *server, GString *str, int crlf)
{
	E2E_GATE_RESULT res;
	E2E_IDENTITY id;
	E2E_LOAD_STATUS status;
	E2E_JSON *kr;
	char *line, *tags, *target, *body, *error = NULL;
	gboolean have_id = FALSE, created;
	gsize len;
	guint i;

	if (!IS_IRC_SERVER(server) || str == NULL || !e2e_native_active())
		return;
	len = str->len;
	while (len > 0 && (str->str[len - 1] == '\n' || str->str[len - 1] == '\r'))
		len--;
	if (gate_multiline(server, str, len))
		return;
	line = g_strndup(str->str, len);
	/* the ciphertext the gate sends right now (below) */
	if (own_line_take(line)) {
		g_free(line);
		return;
	}
	if (!e2e_parse_privmsg_line(line, &tags, &target, &body)) {
		g_free(line);
		return;
	}
	g_free(line);
	/* a PRIVMSG sent from a handler of a signal we caused while sending:
	   gated like any other, but not without end */
	if (gate_depth >= E2E_MAX_GATE_DEPTH) {
		gate_refuse(server, str, target, E2E_REFUSE_ENCRYPT);
		goto out;
	}

	/* rpe2e.pl reads with _load_keyring_checked here: a damaged file is
	   left alone, and the gate refuses */
	kr = e2e_keyring_load_checked(keyring_file, &status, &error);
	g_free(error);
	error = NULL;
	/* only for a REKEY: never created here */
	if (status == E2E_LOAD_OK)
		have_id = e2e_identity_get(kr, &id, FALSE, &created, &error);
	g_free(error);
	e2e_gate_decide(kr, status == E2E_LOAD_OK, have_id ? &id : NULL, target, body,
	                resolve_cb, server, &res);
	/* the corrupt keyring was moved aside: say what to do */
	if (status == E2E_LOAD_LOST && res.action == E2E_GATE_REFUSE) {
		g_free(res.message);
		res.message = g_strdup(E2E_REFUSE_LOST);
	}
	/* a line cut at the server's limit would be garbage: not sent at all
	   (nothing saved either - a rotation stays due) */
	if (res.action == E2E_GATE_CIPHER &&
	    !e2e_gate_lines_fit(&res, tags, target, IRC_SERVER(server)->max_message_len)) {
		e2e_gate_result_clear(&res);
		res.action = E2E_GATE_REFUSE;
		res.message = g_strdup(E2E_REFUSE_TOO_LONG);
	}

	switch (res.action) {
	case E2E_GATE_PASS:
		break;
	case E2E_GATE_BYPASS:
		/* the clear-text bot command goes out unchanged */
		if (res.message != NULL)
			e2e_print_item(e2e_notice_item(server, target, target), "%s", res.message);
		break;
	case E2E_GATE_REFUSE:
		gate_refuse(server, str, target, res.message);
		break;
	case E2E_GATE_CIPHER:
		if (res.save_needed)
			e2e_save(kr);
		for (i = 0; res.warnings != NULL && i < res.warnings->len; i++)
			e2e_print_item(e2e_notice_item(server, res.ctx, target), "%s",
			               (char *) g_ptr_array_index(res.warnings, i));
		gate_depth++;
		for (i = 0; res.notices != NULL && i + 1 < res.notices->len; i += 2)
			send_notice_now(server, g_ptr_array_index(res.notices, i),
			                g_ptr_array_index(res.notices, i + 1));
		for (i = 0; i < res.wires->len; i++)
			send_privmsg(server, tags, target, g_ptr_array_index(res.wires, i));
		gate_depth--;
		g_string_truncate(str, 0);
		signal_stop();
		break;
	}
	e2e_gate_result_clear(&res);
	if (have_id)
		e2e_identity_wipe(&id);
	e2e_json_free(kr);
out:
	g_free(tags);
	g_free(target);
	if (body != NULL)
		e2e_wipe(body, strlen(body));
	g_free(body);
}

/* /upgrade: irc-session.c writes the flood queue straight to the socket,
   past "server outgoing modify". Send it the normal way first (in order),
   so every queued PRIVMSG meets the gate; redirected commands stay for
   irssi, which does not send those either. */
static void sig_session_save_server(SERVER_REC *server)
{
	IRC_SERVER_REC *irc = IRC_SERVER(server);
	GSList *tmp, *next;

	if (irc == NULL)
		return;
	for (tmp = irc->cmdqueue; tmp != NULL; tmp = next) {
		GSList *link = tmp->next;
		char *cmd = tmp->data;
		GString *str;

		next = link->next;
		if (link->data != NULL)
			continue;
		irc->cmdqueue = g_slist_delete_link(irc->cmdqueue, link);
		irc->cmdqueue = g_slist_delete_link(irc->cmdqueue, tmp);
		irc->cmdcount--;
		str = g_string_new(cmd);
		irc_server_send_and_redirect(irc, str, NULL);
		g_string_free(str, TRUE);
		g_free(cmd);
	}
}

/* /e2e reset: PRIVMSGs typed while the keyring was lost may still wait
   in the flood queue - the gate runs when they leave, and after the reset
   it would let them out in clear text. They are dropped instead. */
int e2e_drop_queued_privmsgs(void)
{
	GSList *stmp, *tmp, *next;
	int n = 0;

	for (stmp = servers; stmp != NULL; stmp = stmp->next) {
		IRC_SERVER_REC *irc = IRC_SERVER(stmp->data);

		if (irc == NULL)
			continue;
		for (tmp = irc->cmdqueue; tmp != NULL; tmp = next) {
			GSList *link = tmp->next;
			char *cmd = tmp->data;

			next = link->next;
			/* every line of it: "NICK x\r\nPRIVMSG #secret ..." too */
			if (!e2e_buffer_has_privmsg(cmd, strlen(cmd)))
				continue;
			if (link->data != NULL)
				server_redirect_destroy(link->data);
			irc->cmdqueue = g_slist_delete_link(irc->cmdqueue, link);
			irc->cmdqueue = g_slist_delete_link(irc->cmdqueue, tmp);
			irc->cmdcount--;
			e2e_wipe(cmd, strlen(cmd));
			g_free(cmd);
			n++;
		}
	}
	return n;
}

/* ---- incoming messages ---- */

static void sig_event_privmsg(SERVER_REC *server, const char *data, const char *nick,
                              const char *address)
{
	E2E_IN_PARAMS p;
	E2E_IN_RESULT res;
	E2E_IDENTITY id;
	E2E_JSON *kr;
	const char *sp, *text, *orig_nick = nick, *orig_address = address;
	char *target, **readings, *wait_key, *newdata;
	gboolean have_id, is_channel, lost;

	if (!IS_IRC_SERVER(server) || data == NULL || !e2e_native_active())
		return;
	/* "target :text" */
	sp = strchr(data, ' ');
	if (sp == NULL)
		return;
	text = sp;
	while (*text == ' ')
		text++;
	if (*text == ':')
		text++;
	/* the keyring is read only for well-formed RPE2E messages: anyone can
	   send "+RPE2E01 x", and it must not make us create or move files */
	if (strncmp(text, E2E_WIRE_PREFIX, strlen(E2E_WIRE_PREFIX)) != 0)
		return;
	{
		E2E_WIRE *w = e2e_wire_parse(text);

		if (w == NULL)
			return;
		e2e_wire_free(w);
	}
	if (nick == NULL)
		nick = "";
	if (address == NULL)
		address = "";
	target = g_strndup(data, sp - data);
	readings = e2e_channel_readings(target);
	is_channel = readings[0] != NULL;

	memset(&p, 0, sizeof(p));
	p.nick = nick;
	p.handle = address;
	p.is_own_line = is_own_nick(server, nick);
	/* the context: the channel; a DM is named by the sender, or by the
	   recipient for our own line relayed back */
	p.target = is_channel ? readings[0] : (p.is_own_line ? target : nick);
	p.alt_ctxs = is_channel && readings[1] != NULL ? readings + 1 : NULL;
	p.own_handle = e2e_own_handle(server);
	p.resolve = resolve_cb;
	p.resolve_data = server;
	p.keyreq_stamps = keyreq_out;

	/* the keyring is lost (moved aside): nothing to decrypt with, and no
	   new identity or KEYREQ until /e2e reset */
	kr = e2e_keyring_is_lost() ? NULL : e2e_load_full(&lost);
	if (kr == NULL || lost) {
		e2e_print_debug(server, target, nick, "wire from %s hidden: the keyring is lost (/e2e reset)",
		                nick);
		signal_stop();
		e2e_json_free(kr);
		g_strfreev(readings);
		g_free(target);
		return;
	}
	have_id = e2e_ensure_identity(kr, &id);
	e2e_decrypt_incoming(kr, have_id ? &id : NULL, &p, text, &res);

	switch (res.action) {
	case E2E_IN_NOT_WIRE:
		break;
	case E2E_IN_WAIT_OWN:
		/* decrypting or asking under the sender's handle would set up the
		   wrong direction: hold it, the next message works */
		wait_key = g_strconcat(server->tag, "|", nick, NULL);
		if (e2e_stamp_allow(own_wait_notice, wait_key, E2E_KEYREQ_MIN_INTERVAL))
			e2e_print_item(e2e_notice_item(server, nick, nick),
			               "encrypted DM from %s held — own identity not learned yet (waiting for the WHOIS reply)",
			               nick);
		g_free(wait_key);
		signal_stop();
		break;
	case E2E_IN_DROP:
		if (res.debug != NULL)
			e2e_print_debug(server, res.ctx, nick, "%s", res.debug);
		if (res.keyreq != NULL) {
			e2e_send_notice(server, nick, res.keyreq);
			e2e_save(kr);
			e2e_print_item(e2e_notice_item(server, res.ctx, nick), "KEYREQ sent to %s for %s",
			               nick, res.ctx);
			e2e_print_debug(server, res.ctx, nick, "TX auto-KEYREQ to %s for %s", nick, res.ctx);
		}
		signal_stop();
		break;
	case E2E_IN_PLAIN:
		/* a decrypted non-ACTION CTCP would be answered by irssi with a
		   PLAIN TEXT reply; our side never encrypts those, so drop it */
		if (res.plain[0] == '\001' && strncmp(res.plain, "\001ACTION ", 8) != 0) {
			e2e_print_item(e2e_notice_item(server, p.target, nick),
			               "dropped an encrypted non-ACTION CTCP from %s", nick);
			signal_stop();
			break;
		}
		/* the wire's own target, so the routing (channel / query) stays */
		newdata = g_strconcat(target, " :", res.plain, NULL);
		signal_continue(4, server, newdata, orig_nick, orig_address);
		e2e_wipe(newdata, strlen(newdata));
		g_free(newdata);
		break;
	}
	e2e_in_result_clear(&res);
	if (have_id)
		e2e_identity_wipe(&id);
	e2e_json_free(kr);
	g_strfreev(readings);
	g_free(target);
}

/* ---- the key exchange ---- */

static void sig_ctcp_reply(SERVER_REC *server, const char *args, const char *nick,
                           const char *address, const char *target)
{
	E2E_HANDSHAKE *parsed;
	E2E_HS_TYPE type;
	E2E_IDENTITY id;
	E2E_JSON *kr;
	const char *sender, *ctx;
	char *before, *after, *key, *rsp = NULL, *recip = NULL;
	gboolean have_id, lost;
	size_t taglen = strlen(E2E_CTCP_TAG);
	static const char *const names[] = { "", "KEYREQ", "KEYRSP", "REKEY" };

	if (!IS_IRC_SERVER(server) || args == NULL || strncmp(args, E2E_CTCP_TAG, taglen) != 0 ||
	    (args[taglen] != '\0' && !g_ascii_isspace(args[taglen])) || !e2e_native_active())
		return;
	/* anyone can send these: rate-limited, and the keyring is written only
	   when a handler changed it */
	signal_stop();
	if (nick == NULL)
		nick = "";
	sender = address != NULL ? address : "";
	type = e2e_handshake_type(args);
	if (type == E2E_HS_NONE)
		return;
	parsed = e2e_handshake_parse(args, type);
	ctx = parsed != NULL ? parsed->channel : "";

	/* a flood must not become a KEYRSP flood (Excess Flood), a stream of
	   signature checks or of keyring writes: each kind once per 10 s per
	   sender and context */
	if (!e2e_handshake_allow(keyreq_in, type, sender, ctx)) {
		e2e_print_debug(server, ctx, nick, "%s from %s (%s) for %s dropped: rate limit",
		                names[type], nick, sender, ctx);
		e2e_handshake_free(parsed);
		return;
	}
	kr = e2e_keyring_is_lost() ? NULL : e2e_load_full(&lost);
	if (kr == NULL || lost) {
		e2e_print_debug(server, ctx, nick, "%s from %s ignored: the keyring is lost (/e2e reset)",
		                names[type], nick);
		e2e_json_free(kr);
		e2e_handshake_free(parsed);
		return;
	}
	before = e2e_keyring_snapshot(kr);
	have_id = e2e_ensure_identity(kr, &id);
	switch (type) {
	case E2E_HS_KEYREQ:
		if (parsed != NULL)
			e2e_print_debug(server, ctx, nick, "RX KEYREQ from %s (%s) for %s", nick, sender, ctx);
		if (have_id)
			e2e_handle_keyreq(kr, &id, keyreq_out, sender, nick, args,
			                  e2e_own_handle(server), &rsp, &recip);
		break;
	case E2E_HS_KEYRSP:
		if (parsed != NULL)
			e2e_print_debug(server, ctx, nick, "RX KEYRSP from %s (%s) for %s", nick, sender, ctx);
		if (e2e_handle_keyrsp(kr, sender, nick, args))
			e2e_print_debug(server, ctx, nick, "KEYRSP from %s (%s) installed session on %s",
			                nick, sender, ctx);
		break;
	case E2E_HS_REKEY:
		if (have_id && e2e_handle_rekey(kr, &id, sender, nick, args))
			e2e_print_debug(server, ctx, nick, "REKEY from %s (%s) installed on %s",
			                nick, sender, ctx);
		break;
	default:
		break;
	}
	after = e2e_keyring_snapshot(kr);
	if (strcmp(before, after) != 0)
		e2e_save(kr);

	if (type == E2E_HS_KEYREQ && parsed != NULL && rsp == NULL) {
		key = g_strconcat(sender, "|", ctx, NULL);
		if (e2e_json_get(e2e_json_get(kr, "pending_inbound"), key) != NULL) {
			/* their key may be here already (they answered our request):
			   ours still goes to them only after /e2e accept */
			gboolean have_theirs = e2e_session_has_key(e2e_json_get(e2e_json_get(kr, "incoming"), key));

			e2e_print_item(e2e_notice_item(server, ctx, nick),
			               "Pending key exchange from %s (%s) for %s%s. Run /e2e accept %s or /e2e decline %s.",
			               nick, sender, ctx,
			               have_theirs ? " - you can read them already; they get your key only when you accept" : "",
			               nick, nick);
			e2e_print_debug(server, ctx, nick, "KEYREQ from %s (%s) is pending on %s",
			                nick, sender, ctx);
		}
		g_free(key);
	}
	if (rsp != NULL) {
		e2e_send_notice(server, nick, rsp);
		e2e_print_debug(server, ctx, nick, "TX KEYRSP to %s for %s", nick, ctx);
	}
	if (recip != NULL) {
		e2e_send_notice(server, nick, recip);
		e2e_print_debug(server, ctx, nick, "TX reciprocal KEYREQ to %s for %s", nick, ctx);
	}
	e2e_wipe(before, strlen(before));
	e2e_wipe(after, strlen(after));
	g_free(before);
	g_free(after);
	g_free(rsp);
	g_free(recip);
	if (have_id)
		e2e_identity_wipe(&id);
	e2e_json_free(kr);
	e2e_handshake_free(parsed);
}

/* ---- the module ---- */

void e2e_core_init(void)
{
	GSList *tmp;

	module_register("e2e", "core");
	theme_register(e2e_formats);
	settings_add_bool("e2e", "e2e_debug", FALSE);

	keyring_dir = g_build_filename(get_irssi_dir(), "rpe2e", NULL);
	keyring_file = g_build_filename(keyring_dir, "keyring.json", NULL);
	if (!g_file_test(keyring_dir, G_FILE_TEST_IS_DIR))
		g_mkdir_with_parents(keyring_dir, 0700);
	chmod(keyring_dir, 0700);

	keyreq_out = e2e_stamps_new();
	keyreq_in = e2e_stamps_new();
	own_wait_notice = e2e_stamps_new();
	own_handles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	own_lines = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	signal_add_first("server outgoing modify", (SIGNAL_FUNC) sig_server_outgoing);
	signal_add_first("session save server", (SIGNAL_FUNC) sig_session_save_server);
	signal_add_first("event privmsg", (SIGNAL_FUNC) sig_event_privmsg);
	signal_add_first("ctcp reply", (SIGNAL_FUNC) sig_ctcp_reply);
	signal_add("event 001", (SIGNAL_FUNC) sig_event_001);
	signal_add("event 311", (SIGNAL_FUNC) sig_event_311);
	signal_add("redir e2e whois user", (SIGNAL_FUNC) sig_event_311);
	signal_add("event join", (SIGNAL_FUNC) sig_event_join);
	signal_add("event chghost", (SIGNAL_FUNC) sig_event_chghost);
	signal_add("event 396", (SIGNAL_FUNC) sig_event_396);
	signal_add("commandlist new", (SIGNAL_FUNC) sig_commandlist_new);
	signal_add("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);

	e2e_commands_init();

	if (e2e_native_active()) {
		E2E_IDENTITY id;
		gboolean lost;
		E2E_JSON *kr = e2e_load_full(&lost);

		/* an unreadable keyring does not stop the module: the gate
		   refuses to send instead; a lost one gets no new identity
		   before /e2e reset */
		if (!lost && e2e_ensure_identity(kr, &id))
			e2e_identity_wipe(&id);
		e2e_json_free(kr);
	}
	/* loaded mid-session: 001 will not come for servers already up */
	for (tmp = servers; tmp != NULL; tmp = tmp->next)
		if (IS_IRC_SERVER(tmp->data))
			send_self_whois(tmp->data);
}

void e2e_core_deinit(void)
{
	signal_remove("server outgoing modify", (SIGNAL_FUNC) sig_server_outgoing);
	signal_remove("session save server", (SIGNAL_FUNC) sig_session_save_server);
	signal_remove("event privmsg", (SIGNAL_FUNC) sig_event_privmsg);
	signal_remove("ctcp reply", (SIGNAL_FUNC) sig_ctcp_reply);
	signal_remove("event 001", (SIGNAL_FUNC) sig_event_001);
	signal_remove("event 311", (SIGNAL_FUNC) sig_event_311);
	signal_remove("redir e2e whois user", (SIGNAL_FUNC) sig_event_311);
	signal_remove("event join", (SIGNAL_FUNC) sig_event_join);
	signal_remove("event chghost", (SIGNAL_FUNC) sig_event_chghost);
	signal_remove("event 396", (SIGNAL_FUNC) sig_event_396);
	signal_remove("commandlist new", (SIGNAL_FUNC) sig_commandlist_new);
	signal_remove("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);
	if (script_check_id != 0)
		g_source_remove(script_check_id);
	script_check_id = 0;

	e2e_commands_deinit();
	theme_unregister();

	g_hash_table_destroy(keyreq_out);
	g_hash_table_destroy(keyreq_in);
	g_hash_table_destroy(own_wait_notice);
	g_hash_table_destroy(own_handles);
	g_hash_table_destroy(own_lines);
	g_free(keyring_file);
	g_free(keyring_dir);
	keyring_file = keyring_dir = NULL;
	script_seen = FALSE;
}

void e2e_core_abicheck(int *version)
{
	*version = IRSSI_ABI_VERSION;
}
