/*
 * keepnick reads the MONITOR replies (730/731/734) without binding their
 * events: irssi prints a reply no handler takes ("default event"), and a
 * script that handles one prints it itself. Bound to "event 730",
 * keepnick made irssi print it a second time, on every server, also with
 * no nick kept.
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

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../../test-home.h"

/* irc-core.c */
void irc_core_init(void);
void irc_core_deinit(void);

#define MODULE_NAME "test-keepnick-monitor"

static SERVER_REC *restored;
static int peer = -1;
static int script_lines, default_lines;
static char *default_line;

static void sig_restore_server(SERVER_REC *server, CONFIG_NODE *node)
{
	restored = server;
	server_ref(server);
}

/* a MONITOR notify script */
static void sig_script_730(SERVER_REC *server, const char *data)
{
	script_lines++;
}

/* what irssi prints as an unknown numeric */
static void sig_default_event(SERVER_REC *server, const char *line)
{
	default_lines++;
	g_free(default_line);
	default_line = g_strdup(line);
}

static void server_says(const char *lines)
{
	int i;

	g_assert_cmpint(write(peer, lines, strlen(lines)), ==, (int) strlen(lines));
	for (i = 0; i < 50 && g_main_context_iteration(NULL, FALSE); i++)
		;
}

/* a connected server on a socketpair, as /UPGRADE finds one */
static SERVER_REC *connected_server(void)
{
	CONFIG_REC *session;
	char *data;
	int sv[2];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	g_assert_cmpint(fcntl(sv[0], F_SETFL, O_NONBLOCK), ==, 0);
	g_assert_cmpint(fcntl(sv[1], F_SETFL, O_NONBLOCK), ==, 0);
	peer = sv[1];
	data = g_strdup_printf("servers = ( { chat_type = \"IRC\"; address = \"irc.example.net\"; "
	                       "port = \"6667\"; nick = \"user\"; handle = \"%d\"; "
	                       "real_address = \"irc.example.net\"; userhost = \"u@127.0.0.1\"; "
	                       "isupport_sent = \"yes\"; motd_got = \"yes\"; "
	                       "isupport = { CHANTYPES = \"#\"; }; connected = \"yes\"; } );",
	                       sv[0]);
	session = config_open(NULL, -1);
	g_assert_cmpint(config_parse_data(session, data, "session"), ==, 0);
	g_free(data);
	restored = NULL;
	signal_emit("session restore", 1, session);
	config_close(session);
	g_assert_nonnull(restored);
	g_assert_true(restored->connected);
	return restored;
}

static void finish(SERVER_REC *server)
{
	if (!server->disconnected) {
		server->connection_lost = FALSE;
		server->no_reconnect = TRUE;
		server_disconnect(server);
	}
	server_unref(server);
	restored = NULL;
	close(peer);
	peer = -1;
	script_lines = default_lines = 0;
	g_free(default_line);
	default_line = NULL;
}

/* a script handles 730: its line only, irssi prints nothing more */
static void test_script_handles_it(void)
{
	SERVER_REC *server = connected_server();

	signal_add("event 730", (SIGNAL_FUNC) sig_script_730);
	server_says(":irc.example.net 730 user :zed!z@example.net\r\n");
	signal_remove("event 730", (SIGNAL_FUNC) sig_script_730);
	g_assert_cmpint(script_lines, ==, 1);
	g_assert_cmpint(default_lines, ==, 0);
	finish(server);
}

/* no one handles them: printed once, as they came */
static void test_unhandled_printed(void)
{
	SERVER_REC *server = connected_server();

	server_says(":irc.example.net 731 user :zed\r\n");
	g_assert_cmpint(default_lines, ==, 1);
	g_assert_cmpstr(default_line, ==, "731 user :zed");
	server_says(":irc.example.net 734 user 100 zed :Monitor list is full.\r\n");
	g_assert_cmpint(default_lines, ==, 2);
	g_assert_cmpstr(default_line, ==, "734 user 100 zed :Monitor list is full.");
	finish(server);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/keepnick/monitor/script-handles-it", test_script_handles_it);
	g_test_add_func("/keepnick/monitor/unhandled-printed", test_unhandled_printed);

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	signal(SIGPIPE, SIG_IGN);

	args_execute(0, NULL);
	core_init();
	irc_core_init();
	signal_emit("irssi init finished", 0);
	signal_add_last("session restore server", (SIGNAL_FUNC) sig_restore_server);
	signal_add_last("default event", (SIGNAL_FUNC) sig_default_event);

	return g_test_run();
}
