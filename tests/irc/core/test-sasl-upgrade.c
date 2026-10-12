/*
 * A server that logged in with SASL keeps its connection over /UPGRADE,
 * and /UPGRADE is no way around SASL.
 *
 * The new process restores each server from the session file. A server
 * that had registered (001) before the upgrade gets a made-up "event 001"
 * (irc-session.c); the SASL check that runs on 001 (and on 375) must not
 * take that for the end of a failed login: no "SASL failure", no
 * disconnect. A server that was still registering (CAP, AUTHENTICATE in
 * flight) cannot finish its login on the inherited socket: it is
 * connected again from the start, where SASL is enforced as on any new
 * connection. A connection made in the normal way that did not log in is
 * still dropped.
 *
 * The rest of the connection state comes back too: the CAP state (or,
 * from an older session file, the client asks the server for it again
 * without ending a negotiation twice), and a TLS socket that cannot be
 * taken over is closed, not left open.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/lib-config/iconfig.h>

#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-cap.h>
#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/sasl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../../test-home.h"

/* irc-core.c */
void irc_core_init(void);
void irc_core_deinit(void);

#define MODULE_NAME "test-sasl-upgrade"

/* what a session file holds for a server past 001 and the MOTD */
#define REGISTERED \
	"real_address = \"irc.example.net\"; userhost = \"u@127.0.0.1\"; " \
	"isupport_sent = \"yes\"; motd_got = \"yes\"; " \
	"isupport = { CHANTYPES = \"#\"; }; "

#define WAIT_MS 5000
#define WELCOME ":irc.example.net 001 user :Welcome\r\n"

static int sasl_failures;
static int cap_ends;
static int peer = -1;
static SERVER_REC *restored;
static GString *linebuf; /* what client_sends() read but did not use yet */

static void sig_sasl_failure(IRC_SERVER_REC *server, const char *reason)
{
	g_test_message("SASL failure: %s", reason);
	sasl_failures++;
}

static void sig_cap_end(IRC_SERVER_REC *server)
{
	cap_ends++;
}

/* keeps the record the session restore made, even when it is let go */
static void sig_restore_server(SERVER_REC *server, CONFIG_NODE *node)
{
	restored = server;
	server_ref(server);
}

static void pump(void)
{
	int i;

	for (i = 0; i < 50 && g_main_context_iteration(NULL, FALSE); i++)
		;
}

/* restores the server in `session', as the process started by /UPGRADE
   does; closes `session' */
static IRC_SERVER_REC *restore_config(CONFIG_REC *session)
{
	restored = NULL;
	sasl_failures = 0;
	cap_ends = 0;
	g_string_truncate(linebuf, 0);
	signal_emit("session restore", 1, session);
	config_close(session);

	g_assert_nonnull(restored);
	return IRC_SERVER(restored);
}

/* restores one SASL PLAIN server from a session file, as the process
   started by /UPGRADE does; `extra' is added to the server's block */
static IRC_SERVER_REC *restore_at(const char *address, int port, const char *extra)
{
	CONFIG_REC *session;
	char *data;
	int sv[2];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	/* the socket erssi inherits over /UPGRADE does not block */
	g_assert_cmpint(fcntl(sv[0], F_SETFL, O_NONBLOCK), ==, 0);
	g_assert_cmpint(fcntl(sv[1], F_SETFL, O_NONBLOCK), ==, 0);
	peer = sv[1];

	data = g_strdup_printf(
	    "servers = ( { chat_type = \"IRC\"; address = \"%s\"; "
	    "port = \"%d\"; nick = \"user\"; handle = \"%d\"; "
	    "sasl_mechanism = \"%d\"; sasl_username = \"user\"; "
	    "sasl_password = \"secret\"; %s } );",
	    address, port, sv[0], SASL_MECHANISM_PLAIN, extra);
	session = config_open(NULL, -1);
	g_assert_cmpint(config_parse_data(session, data, "session"), ==, 0);
	g_free(data);

	return restore_config(session);
}

static IRC_SERVER_REC *restore(const char *extra)
{
	IRC_SERVER_REC *server = restore_at("irc.example.net", 6667, extra);

	g_assert_true(server->session_reconnect);
	g_assert_nonnull(g_slist_find(servers, server));
	return server;
}

/* the server sends `lines' on the inherited socket */
static void server_says(const char *lines)
{
	g_assert_cmpint(write(peer, lines, strlen(lines)), ==, (int) strlen(lines));
	pump();
}

static void say(int fd, const char *lines)
{
	g_assert_cmpint(write(fd, lines, strlen(lines)), ==, (int) strlen(lines));
	pump();
}

/* TRUE once the other end of `fd' is closed */
static gboolean closed_within(int fd, int ms)
{
	gint64 end = g_get_monotonic_time() + ms * 1000;
	char buf[512];
	ssize_t n;

	while (g_get_monotonic_time() < end) {
		pump();
		n = read(fd, buf, sizeof(buf));
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
			return TRUE;
		if (n < 0)
			g_usleep(1000);
	}
	return FALSE;
}

static void finish_one(SERVER_REC *server)
{
	if (!server->disconnected) {
		server->connection_lost = FALSE;
		server->no_reconnect = TRUE;
		server_disconnect(server);
	}
}

static void finish(IRC_SERVER_REC *server)
{
	finish_one(SERVER(server));
	server_unref(SERVER(server));
	restored = NULL;
	close(peer);
	peer = -1;
}

static void assert_kept(IRC_SERVER_REC *server)
{
	g_assert_cmpint(sasl_failures, ==, 0);
	g_assert_false(server->connection_lost);
	g_assert_true(server->connected);

	/* the next lines from the server: a PING, and a MOTD (375 runs the
	   SASL check too) */
	server_says("PING :check\r\n"
	            ":irc.example.net 375 user :- irc.example.net Message of the Day -\r\n"
	            ":irc.example.net 376 user :End of MOTD\r\n");
	g_assert_cmpint(sasl_failures, ==, 0);
	g_assert_false(server->disconnected);
	g_assert_nonnull(g_slist_find(servers, server));
}

/* a session file written by this version: the login is remembered */
static void test_restored_sasl_server_kept(void)
{
	IRC_SERVER_REC *server = restore(REGISTERED "connected = \"yes\"; sasl_success = \"yes\";");

	assert_kept(server);
	g_assert_true(server->sasl_success);
	finish(server);
}

/* a session file written by an older erssi, without sasl_success and
   without the registered flag: past the MOTD it had registered, and with
   a SASL mechanism set it had logged in (or the old erssi would have
   dropped it) */
static void test_old_session_file_kept(void)
{
	IRC_SERVER_REC *server = restore(REGISTERED);

	assert_kept(server);
	g_assert_true(server->sasl_success);
	finish(server);
}

/* the next /UPGRADE writes the login and the registration down again */
static void test_save_keeps_sasl_success(void)
{
	IRC_SERVER_REC *server = restore(REGISTERED "connected = \"yes\"; sasl_success = \"yes\";");
	CONFIG_REC *session;
	CONFIG_NODE *node;

	session = config_open(NULL, -1);
	signal_emit("session save", 1, session);
	node = config_node_traverse(session, "(servers", FALSE);
	g_assert_nonnull(node);
	node = config_node_nth(node, 0);
	g_assert_nonnull(node);
	g_assert_true(config_node_get_bool(node, "sasl_success", FALSE));
	g_assert_true(config_node_get_bool(node, "connected", FALSE));
	g_assert_cmpint(config_node_get_int(node, "sasl_mechanism", 0), ==, SASL_MECHANISM_PLAIN);
	config_close(session);

	/* "session save" lets go of the server */
	g_assert_true(server->disconnected);
	finish(server);
}

/* a connection made in the normal way that did not log in: 001 still
   drops it (sasl_disconnect_on_failure is on by default) */
static void test_fresh_connection_still_dropped(void)
{
	IRC_SERVER_REC *server = restore(REGISTERED);

	server->session_reconnect = FALSE;
	server->sasl_success = FALSE;
	server->connected = FALSE;
	server->connection_lost = FALSE;
	sasl_failures = 0;
	signal_emit("event 001", 3, server, "user :Welcome", "irc.example.net");
	g_assert_true(server->connection_lost);
	g_assert_false(server->connected);
	g_assert_cmpint(sasl_failures, ==, 1);
	finish(server);
}

/* registered without a login before the upgrade (sasl_disconnect_on_failure
   was off then): the made-up 001 keeps it, but a real 375 later is checked
   as on any connection - with the setting on now, it is dropped */
static void test_registered_without_login_checked_later(void)
{
	IRC_SERVER_REC *server = restore(REGISTERED "connected = \"yes\"; sasl_success = \"no\";");

	g_assert_cmpint(sasl_failures, ==, 0);
	g_assert_true(server->connected);
	g_assert_false(server->connection_lost);
	g_assert_false(server->sasl_success);

	server_says(":irc.example.net 375 user :- irc.example.net Message of the Day -\r\n");
	g_assert_true(server->disconnected || server->connection_lost);
	g_assert_cmpint(sasl_failures, ==, 1);
	finish(server);
}

/* --- /UPGRADE while the connection is still registering --- */

static int listen_local(int *port)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	g_assert_cmpint(fd, >=, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	g_assert_cmpint(bind(fd, (struct sockaddr *) &sa, sizeof(sa)), ==, 0);
	g_assert_cmpint(listen(fd, 4), ==, 0);
	g_assert_cmpint(getsockname(fd, (struct sockaddr *) &sa, &len), ==, 0);
	g_assert_cmpint(fcntl(fd, F_SETFL, O_NONBLOCK), ==, 0);
	*port = ntohs(sa.sin_port);
	return fd;
}

static int accept_within(int lfd, int ms)
{
	gint64 end = g_get_monotonic_time() + ms * 1000;
	int fd;

	while (g_get_monotonic_time() < end) {
		pump();
		fd = accept(lfd, NULL, NULL);
		if (fd >= 0) {
			g_assert_cmpint(fcntl(fd, F_SETFL, O_NONBLOCK), ==, 0);
			return fd;
		}
		g_usleep(1000);
	}
	return -1;
}

/* reads what the client sends on `fd' until a line starts with `prefix' */
static gboolean client_sends(int fd, const char *prefix, int ms)
{
	GString *buf = linebuf;
	gint64 end = g_get_monotonic_time() + ms * 1000;
	char tmp[512], *nl;
	ssize_t n;

	while (g_get_monotonic_time() < end) {
		while ((nl = strchr(buf->str, '\n')) != NULL) {
			gboolean found = strncmp(buf->str, prefix, strlen(prefix)) == 0;

			g_test_message("client: %.*s", (int) (nl - buf->str), buf->str);
			g_string_erase(buf, 0, nl - buf->str + 1);
			if (found)
				return TRUE;
		}
		pump();
		n = read(fd, tmp, sizeof(tmp));
		if (n > 0)
			g_string_append_len(buf, tmp, n);
		else if (n == 0)
			break;
		else
			g_usleep(1000);
	}
	g_string_truncate(buf, 0);
	return FALSE;
}

/* the record the fresh connection runs on, if any */
static SERVER_REC *fresh_record(IRC_SERVER_REC *old)
{
	GSList *tmp;

	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		SERVER_REC *rec = tmp->data;

		if (rec != SERVER(old) && rec->connrec == SERVER(old)->connrec)
			return rec;
	}
	return NULL;
}

/*
 * /UPGRADE happened while the connection was still registering. On the
 * inherited socket the server then says `old_lines' (the rest of the
 * login: the CAP LS reply, a 904) and lets the client on with 001.
 * The client must not end up registered there without SASL: the restored
 * record is let go and the server connected again, where `new_cap_ls'
 * (and, when SASL is offered, a 904) ends in a drop.
 */
static void upgrade_while_registering(const char *extra, const char *old_lines,
                                      const char *new_cap_ls)
{
	IRC_SERVER_REC *server;
	SERVER_REC *fresh;
	int lfd, port, cfd;
	gboolean old_closed;

	lfd = listen_local(&port);
	server = restore_at("127.0.0.1", port, extra);

	old_closed = closed_within(peer, 200);
	if (!old_closed) {
		/* still on the inherited socket: play the rest of the login */
		server_says(old_lines);
		server_says(WELCOME);
	}
	/* not registered without SASL on the inherited socket */
	g_assert_true(server->disconnected || server->connection_lost);
	g_assert_false(server->connected && !server->connection_lost);
	/* which is closed, not left half-registered on the server */
	g_assert_true(old_closed);

	/* connected again from the start */
	cfd = accept_within(lfd, WAIT_MS);
	g_assert_cmpint(cfd, >=, 0);
	if (cfd >= 0) {
		g_assert_true(client_sends(cfd, "CAP LS", WAIT_MS));
		fresh = fresh_record(server);
		g_assert_nonnull(fresh);
		if (fresh != NULL) {
			server_ref(fresh);
			/* a new connection: its real 001 is no made-up one */
			g_assert_false(fresh->session_reconnect);
			sasl_failures = 0;
			say(cfd, new_cap_ls);
			if (strstr(new_cap_ls, "sasl") != NULL) {
				g_assert_true(client_sends(cfd, "CAP REQ :sasl", WAIT_MS));
				say(cfd, ":irc.example.net CAP * ACK :sasl\r\n");
				g_assert_true(client_sends(cfd, "AUTHENTICATE PLAIN", WAIT_MS));
				say(cfd, ":irc.example.net 904 * :SASL authentication failed\r\n");
			}
			g_assert_true(client_sends(cfd, "CAP END", WAIT_MS));
			/* the client may be gone already: no check on the write */
			if (write(cfd, WELCOME, strlen(WELCOME)) < 0)
				g_test_message("001 not written: %s", g_strerror(errno));
			pump();
			/* SASL enforced: the fresh connection is dropped */
			g_assert_true(fresh->disconnected);
			g_assert_false(fresh->connected);
			g_assert_cmpint(sasl_failures, >=, 1);
			finish_one(fresh);
			server_unref(fresh);
			/* the socket goes with the last reference */
			g_assert_true(closed_within(cfd, WAIT_MS));
		}
		close(cfd);
	}
	close(lfd);
	finish(server);
}

/* Scenario A: /UPGRADE after CAP LS was sent, before the reply (session
   file of this version: connected = no) */
static void test_upgrade_before_cap_ls_reply(void)
{
	upgrade_while_registering("connected = \"no\"; sasl_success = \"no\";",
	                          ":irc.example.net CAP * LS :sasl\r\n",
	                          ":irc.example.net CAP * LS :sasl\r\n");
}

/* Scenario B: /UPGRADE while AUTHENTICATE is in flight, then 904 (session
   file of an older erssi: no registered flag, no MOTD yet) */
static void test_upgrade_mid_sasl_904(void)
{
	upgrade_while_registering("",
	                          ":irc.example.net 904 user :SASL authentication failed\r\n",
	                          ":irc.example.net CAP * LS :\r\n");
}

/* --- the rest of the state /UPGRADE has to keep --- */

#define CAPS \
	"cap_complete = \"yes\"; " \
	"cap_supported = ( \"sasl=PLAIN,EXTERNAL\", \"message-tags\", \"extended-join\", " \
	"\"draft/chathistory=100\" ); " \
	"cap_active = ( \"message-tags\", \"extended-join\" ); "

static gboolean has_cap_active(IRC_SERVER_REC *server, const char *cap)
{
	GSList *tmp;

	for (tmp = server->cap_active; tmp != NULL; tmp = tmp->next) {
		if (strcmp(tmp->data, cap) == 0)
			return TRUE;
	}
	return FALSE;
}

static void assert_caps(IRC_SERVER_REC *server)
{
	char *val;

	g_assert_true(server->cap_complete);
	g_assert_nonnull(server->cap_supported);
	if (server->cap_supported == NULL)
		return;
	g_assert_true(g_hash_table_lookup_extended(server->cap_supported, "sasl", NULL,
	                                           (gpointer *) &val));
	g_assert_cmpstr(val, ==, "PLAIN,EXTERNAL");
	g_assert_true(g_hash_table_lookup_extended(server->cap_supported, "message-tags", NULL,
	                                           (gpointer *) &val));
	g_assert_null(val);
	g_assert_true(g_hash_table_lookup_extended(server->cap_supported, "draft/chathistory",
	                                           NULL, (gpointer *) &val));
	g_assert_cmpstr(val, ==, "100");
	g_assert_cmpuint(g_hash_table_size(server->cap_supported), ==, 4);
	g_assert_cmpuint(g_slist_length(server->cap_active), ==, 2);
	g_assert_true(has_cap_active(server, "message-tags"));
	g_assert_true(has_cap_active(server, "extended-join"));
}

/* a session file of this version: the CAP state comes back as it was */
static void test_caps_restored(void)
{
	IRC_SERVER_REC *server;
	char *tags, *body, *line;

	/* registered without a login while sasl_disconnect_on_failure was
	   off: the server did offer SASL, so a later MOTD says nothing */
	settings_set_bool("sasl_disconnect_on_failure", FALSE);
	server = restore(REGISTERED CAPS "connected = \"yes\"; sasl_success = \"no\";");
	assert_caps(server);
	g_assert_null(server->cap_queue);

	/* the negotiation is over: a cap is asked for at once, not queued */
	g_assert_false(irc_cap_toggle(server, "away-notify", TRUE));
	g_assert_null(server->cap_queue);

	/* message-tags is on: client tags do not count against the 510 bytes
	   of the message (without the cap the line is cut at 510) */
	tags = g_strnfill(120, 'a');
	body = g_strnfill(480, 'b');
	line = g_strdup_printf("@+data=%s PRIVMSG #test :%s", tags, body);
	irc_send_cmd_now(server, line);
	g_free(line);
	line = g_strdup_printf("@+data=%s PRIVMSG #test :%s\r", tags, body);
	g_assert_true(client_sends(peer, line, WAIT_MS));
	g_free(line);
	g_free(tags);
	g_free(body);

	server_says(":irc.example.net 375 user :- irc.example.net Message of the Day -\r\n"
	            ":irc.example.net 376 user :End of MOTD\r\n");
	g_assert_cmpint(sasl_failures, ==, 0);
	g_assert_false(server->disconnected);
	g_assert_cmpint(cap_ends, ==, 0);

	settings_set_bool("sasl_disconnect_on_failure", TRUE);
	finish(server);
}

/* save, write the file, read it back and restore: /UPGRADE twice */
static void test_caps_round_trip(void)
{
	IRC_SERVER_REC *server = restore(REGISTERED CAPS "connected = \"yes\"; sasl_success = \"yes\";");
	CONFIG_REC *session;
	char *path;

	session = config_open(NULL, -1);
	signal_emit("session save", 1, session);
	g_assert_true(server->disconnected);
	finish_one(SERVER(server));
	server_unref(SERVER(server));

	path = g_build_filename(test_home_dir, "session", NULL);
	g_assert_cmpint(config_write(session, path, 0600), ==, 0);
	config_close(session);
	session = config_open(path, -1);
	g_assert_nonnull(session);
	g_assert_cmpint(config_parse(session), ==, 0);
	g_free(path);

	/* the saved handle is the same socket, still open: as after exec() */
	server = restore_config(session);
	g_assert_true(server->session_reconnect);
	g_assert_true(server->connected);
	g_assert_true(server->sasl_success);
	assert_caps(server);
	g_assert_cmpint(cap_ends, ==, 0);
	finish(server);
}

/* an older session file has no CAP state: the client asks the server
   again (CAP LS, CAP LIST), without a second CAP END */
static void test_old_file_cap_resync(void)
{
	IRC_SERVER_REC *server = restore(REGISTERED);

	g_assert_true(server->connected);
	g_assert_true(server->cap_complete);
	g_assert_true(client_sends(peer, "CAP LS 302", WAIT_MS));
	g_assert_true(client_sends(peer, "CAP LIST", WAIT_MS));

	server_says(":irc.example.net CAP user LS * :sasl=PLAIN message-tags\r\n"
	            ":irc.example.net CAP user LS :extended-join draft/chathistory=100\r\n"
	            ":irc.example.net CAP user LIST * :message-tags\r\n"
	            ":irc.example.net CAP user LIST :extended-join\r\n"
	            "PING :after\r\n");
	g_assert_true(client_sends(peer, "PONG", WAIT_MS));

	g_assert_true(g_hash_table_lookup_extended(server->cap_supported, "message-tags", NULL, NULL));
	g_assert_true(g_hash_table_lookup_extended(server->cap_supported, "draft/chathistory", NULL, NULL));
	g_assert_true(has_cap_active(server, "message-tags"));
	g_assert_true(has_cap_active(server, "extended-join"));
	g_assert_cmpuint(g_slist_length(server->cap_active), ==, 2);
	g_assert_null(server->cap_queue);
	g_assert_cmpint(cap_ends, ==, 0);
	g_assert_cmpint(sasl_failures, ==, 0);
	g_assert_false(server->disconnected);

	/* nothing more was sent: no CAP END, no CAP REQ, no AUTHENTICATE */
	irc_send_cmd_now(server, "PING :marker");
	g_assert_true(client_sends(peer, "PING :marker", WAIT_MS));
	finish(server);
}

/* an older session file without the MOTD flag but with channels: a JOIN
   needs registration, so it had registered */
static void test_old_file_channels_mean_registered(void)
{
	IRC_SERVER_REC *server;
	int lfd, port, cfd;

	lfd = listen_local(&port);
	server = restore_at("127.0.0.1", port,
	                    "real_address = \"irc.example.net\"; userhost = \"u@127.0.0.1\"; "
	                    "channels = ( { name = \"#test\"; } ); ");
	g_assert_true(server->session_reconnect);
	g_assert_true(server->connected);
	g_assert_false(server->disconnected);
	g_assert_nonnull(g_slist_find(servers, server));
	g_assert_true(server->sasl_success);
	cfd = accept_within(lfd, 200);
	g_assert_cmpint(cfd, <, 0);
	if (cfd >= 0)
		close(cfd);
	close(lfd);
	finish(server);
}

/* TLS cannot be taken over: the inherited socket is closed (it used to be
   left open) and a new connection made */
static void test_tls_inherited_socket_closed(void)
{
	IRC_SERVER_REC *server;
	GSList *tmp;
	int lfd, port, cfd;
	GLogLevelFlags loglev;

	/* the new TLS connection meets a listener that speaks no TLS: its
	   handshake errors are not what this test is about */
	loglev = g_log_set_always_fatal(G_LOG_FATAL_MASK);
	lfd = listen_local(&port);
	server = restore_at("127.0.0.1", port,
	                    REGISTERED "connected = \"yes\"; sasl_success = \"yes\"; "
	                    "use_tls = \"yes\"; tls_verify = \"no\";");
	g_assert_true(server->disconnected);
	g_assert_true(closed_within(peer, WAIT_MS));

	cfd = accept_within(lfd, WAIT_MS);
	g_assert_cmpint(cfd, >=, 0);
	for (tmp = lookup_servers; tmp != NULL; tmp = tmp->next) {
		if (((SERVER_REC *) tmp->data)->connrec == SERVER(server)->connrec)
			break;
	}
	if (tmp == NULL) {
		for (tmp = servers; tmp != NULL; tmp = tmp->next) {
			if (((SERVER_REC *) tmp->data)->connrec == SERVER(server)->connrec)
				break;
		}
	}
	g_assert_nonnull(tmp);
	if (tmp != NULL)
		finish_one(tmp->data);
	if (cfd >= 0)
		close(cfd);
	close(lfd);
	finish(server);
	pump();
	g_log_set_always_fatal(loglev);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/sasl-upgrade/restored-sasl-server-kept", test_restored_sasl_server_kept);
	g_test_add_func("/sasl-upgrade/old-session-file-kept", test_old_session_file_kept);
	g_test_add_func("/sasl-upgrade/save-keeps-sasl-success", test_save_keeps_sasl_success);
	g_test_add_func("/sasl-upgrade/fresh-connection-still-dropped",
	                test_fresh_connection_still_dropped);
	g_test_add_func("/sasl-upgrade/registered-without-login-checked-later",
	                test_registered_without_login_checked_later);
	g_test_add_func("/sasl-upgrade/upgrade-before-cap-ls-reply",
	                test_upgrade_before_cap_ls_reply);
	g_test_add_func("/sasl-upgrade/upgrade-mid-sasl-904", test_upgrade_mid_sasl_904);
	g_test_add_func("/upgrade-state/caps-restored", test_caps_restored);
	g_test_add_func("/upgrade-state/caps-round-trip", test_caps_round_trip);
	g_test_add_func("/upgrade-state/old-file-cap-resync", test_old_file_cap_resync);
	g_test_add_func("/upgrade-state/old-file-channels-mean-registered",
	                test_old_file_channels_mean_registered);
	g_test_add_func("/upgrade-state/tls-inherited-socket-closed",
	                test_tls_inherited_socket_closed);

#if GLIB_CHECK_VERSION(2,38,0)
	g_test_set_nonfatal_assertions();
#endif

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	signal(SIGPIPE, SIG_IGN);

	/* the core is brought up once: it is not meant to be re-initialised */
	args_execute(0, NULL);
	core_init();
	irc_core_init();
	signal_emit("irssi init finished", 0);
	signal_add("server sasl failure", (SIGNAL_FUNC) sig_sasl_failure);
	signal_add_last("session restore server", (SIGNAL_FUNC) sig_restore_server);
	signal_add("server cap end", (SIGNAL_FUNC) sig_cap_end);
	linebuf = g_string_new(NULL);

	return g_test_run();
}
