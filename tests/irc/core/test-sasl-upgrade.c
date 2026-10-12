/*
 * A server that logged in with SASL keeps its connection over /UPGRADE.
 *
 * The new process restores the server from the session file and emits a
 * made-up "event 001" for it (irc-session.c). The SASL check that runs on
 * 001 (and on 375) must not take that for the end of a failed login: no
 * "SASL failure", no disconnect. A connection made in the normal way that
 * did not log in is still dropped.
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

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../../test-home.h"

/* irc-core.c */
void irc_core_init(void);
void irc_core_deinit(void);

#define MODULE_NAME "test-sasl-upgrade"

static int sasl_failures;
static int peer = -1;

static void sig_sasl_failure(IRC_SERVER_REC *server, const char *reason)
{
	g_test_message("SASL failure: %s", reason);
	sasl_failures++;
}

/* restores one SASL PLAIN server from a session file, as the process
   started by /UPGRADE does; `extra' is added to the server's block */
static IRC_SERVER_REC *restore(const char *extra)
{
	CONFIG_REC *session;
	char *data;
	int sv[2];
	guint before;
	IRC_SERVER_REC *server;

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	/* the socket erssi inherits over /UPGRADE does not block */
	g_assert_cmpint(fcntl(sv[0], F_SETFL, O_NONBLOCK), ==, 0);
	peer = sv[1];

	data = g_strdup_printf(
	    "servers = ( { chat_type = \"IRC\"; address = \"irc.example.net\"; "
	    "port = \"6667\"; nick = \"user\"; handle = \"%d\"; "
	    "real_address = \"irc.example.net\"; userhost = \"u@127.0.0.1\"; "
	    "sasl_mechanism = \"%d\"; sasl_username = \"user\"; "
	    "sasl_password = \"secret\"; isupport_sent = \"yes\"; "
	    "motd_got = \"yes\"; isupport = { CHANTYPES = \"#\"; }; %s } );",
	    sv[0], SASL_MECHANISM_PLAIN, extra);
	session = config_open(NULL, -1);
	g_assert_cmpint(config_parse_data(session, data, "session"), ==, 0);
	g_free(data);

	before = g_slist_length(servers);
	sasl_failures = 0;
	signal_emit("session restore", 1, session);
	config_close(session);

	g_assert_cmpuint(g_slist_length(servers), ==, before + 1);
	server = IRC_SERVER(g_slist_last(servers)->data);
	g_assert_nonnull(server);
	g_assert_true(server->session_reconnect);
	server_ref(SERVER(server));
	return server;
}

/* the server sends `lines'; erssi reads them as it would in the main loop */
static void server_says(const char *lines)
{
	int i;

	g_assert_cmpint(write(peer, lines, strlen(lines)), ==, (int) strlen(lines));
	for (i = 0; i < 50 && g_main_context_iteration(NULL, FALSE); i++)
		;
}

static void finish(IRC_SERVER_REC *server)
{
	if (!server->disconnected) {
		server->connection_lost = FALSE;
		server->no_reconnect = TRUE;
		server_disconnect(SERVER(server));
	}
	server_unref(SERVER(server));
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
	IRC_SERVER_REC *server = restore("sasl_success = \"yes\";");

	assert_kept(server);
	g_assert_true(server->sasl_success);
	finish(server);
}

/* a session file written by an older erssi, without sasl_success */
static void test_old_session_file_kept(void)
{
	IRC_SERVER_REC *server = restore("");

	assert_kept(server);
	finish(server);
}

/* the next /UPGRADE writes the login down again */
static void test_save_keeps_sasl_success(void)
{
	IRC_SERVER_REC *server = restore("sasl_success = \"yes\";");
	CONFIG_REC *session;
	CONFIG_NODE *node;

	session = config_open(NULL, -1);
	signal_emit("session save", 1, session);
	node = config_node_traverse(session, "(servers", FALSE);
	g_assert_nonnull(node);
	node = config_node_nth(node, 0);
	g_assert_nonnull(node);
	g_assert_true(config_node_get_bool(node, "sasl_success", FALSE));
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
	IRC_SERVER_REC *server = restore("");

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

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/sasl-upgrade/restored-sasl-server-kept", test_restored_sasl_server_kept);
	g_test_add_func("/sasl-upgrade/old-session-file-kept", test_old_session_file_kept);
	g_test_add_func("/sasl-upgrade/save-keeps-sasl-success", test_save_keeps_sasl_success);
	g_test_add_func("/sasl-upgrade/fresh-connection-still-dropped",
	                test_fresh_connection_still_dropped);

#if GLIB_CHECK_VERSION(2,38,0)
	g_test_set_nonfatal_assertions();
#endif

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;

	/* the core is brought up once: it is not meant to be re-initialised */
	args_execute(0, NULL);
	core_init();
	irc_core_init();
	signal_emit("irssi init finished", 0);
	signal_add("server sasl failure", (SIGNAL_FUNC) sig_sasl_failure);

	return g_test_run();
}
