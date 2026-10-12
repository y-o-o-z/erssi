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
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/lib-config/iconfig.h>

#include <irssi/src/irc/core/irc.h>
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
static int peer = -1;
static SERVER_REC *restored;

static void sig_sasl_failure(IRC_SERVER_REC *server, const char *reason)
{
	g_test_message("SASL failure: %s", reason);
	sasl_failures++;
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

	restored = NULL;
	sasl_failures = 0;
	signal_emit("session restore", 1, session);
	config_close(session);

	g_assert_nonnull(restored);
	return IRC_SERVER(restored);
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
	static GString *buf;
	gint64 end = g_get_monotonic_time() + ms * 1000;
	char tmp[512], *nl;
	ssize_t n;

	if (buf == NULL)
		buf = g_string_new(NULL);
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

	return g_test_run();
}
