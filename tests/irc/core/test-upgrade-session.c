/*
 * /UPGRADE writes the session file whole before it lets a connection go:
 * when the file cannot be written (a full disk or quota, a read-only
 * directory), erssi keeps running with every connection, says why, and
 * starts no new process. Here the file cannot be written because a
 * directory stands where it goes - before the save (the check that comes
 * first finds it), and only after the save (the write itself fails).
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

#define MODULE_NAME "test-upgrade-session"

static SERVER_REC *restored;
static int peer = -1;
static int gui_exits;
static GString *dialogs;
static char *session_path;
static gboolean block_while_saving;

static void sig_restore_server(SERVER_REC *server, CONFIG_NODE *node)
{
	restored = server;
	server_ref(server);
}

static void sig_gui_exit(void)
{
	gui_exits++;
}

static void sig_gui_dialog(const char *type, const char *text)
{
	g_string_append_printf(dialogs, "%s: %s\n", type, text);
}

/* the session file cannot be written any more once the save has begun */
static void sig_session_save(CONFIG_REC *config)
{
	if (block_while_saving)
		g_assert_cmpint(g_mkdir(session_path, 0700), ==, 0);
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
	g_rmdir(session_path);
	g_unlink(session_path);
	g_string_truncate(dialogs, 0);
	gui_exits = 0;
	block_while_saving = FALSE;
}

static void upgrade(void)
{
	signal_emit("command upgrade", 3, "/bin/true", NULL, NULL);
}

/* the connection is still erssi's: in the list, with its socket */
static void assert_kept(SERVER_REC *server)
{
	g_assert_false(server->disconnected);
	g_assert_nonnull(g_slist_find(servers, server));
	g_assert_nonnull(server->handle);
	g_assert_cmpint(gui_exits, ==, 0);
	g_assert_nonnull(strstr(dialogs->str, "error: /UPGRADE not done: cannot write"));
	g_assert_nonnull(strstr(dialogs->str, session_path));
}

/* found before anything is saved: nothing is closed */
static void test_refused_before_save(void)
{
	SERVER_REC *server = connected_server();

	g_assert_cmpint(g_mkdir(session_path, 0700), ==, 0);
	upgrade();
	assert_kept(server);
	g_assert_nonnull(strstr(dialogs->str, "Nothing was closed"));
	finish(server);
}

/* the write itself fails: the saved connections are not let go */
static void test_refused_after_save(void)
{
	SERVER_REC *server = connected_server();

	block_while_saving = TRUE;
	upgrade();
	assert_kept(server);
	g_assert_nonnull(strstr(dialogs->str, "The connections stay"));
	finish(server);
}

/* every byte the server end can read now */
static GString *peer_reads(GString *into)
{
	char buf[4096];
	ssize_t n;

	while ((n = read(peer, buf, sizeof(buf))) > 0)
		g_string_append_len(into, buf, n);
	return into;
}

static int count_lines(const char *text, const char *needle)
{
	int n = 0;

	for (; (text = strstr(text, needle)) != NULL; text += strlen(needle))
		n++;
	return n;
}

/* The save sends the commands waiting in the flood queue at once; when
 * the write then fails, the connection stays here, and those commands
 * must not be sent a second time by the queue (no e2e module loaded,
 * which takes them out of the queue on its own) */
static void test_refused_after_save_sends_once(void)
{
	SERVER_REC *server = connected_server();
	IRC_SERVER_REC *irc = IRC_SERVER(server);
	GString *got = g_string_new(NULL);
	gint64 end;
	int i;

	irc->max_cmds_at_once = 5;
	irc->cmd_queue_speed = 60000;
	for (i = 0; i < 10; i++) {
		char *cmd = g_strdup_printf("PRIVMSG #x :m%d", i);

		irc_send_cmd(irc, cmd);
		g_free(cmd);
	}
	g_assert_nonnull(irc->cmdqueue);
	block_while_saving = TRUE;
	upgrade();
	assert_kept(server);
	/* the queue runs on, as fast as it can */
	irc->cmd_queue_speed = 0;
	irc->wait_cmd = 0;
	end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < end) {
		g_main_context_iteration(NULL, FALSE);
		peer_reads(got);
		g_usleep(10000);
	}
	g_assert_cmpint(count_lines(got->str, "PRIVMSG #x :m"), ==, 10);
	for (i = 0; i < 10; i++) {
		char *line = g_strdup_printf("PRIVMSG #x :m%d\r\n", i);

		g_assert_cmpint(count_lines(got->str, line), ==, 1);
		g_free(line);
	}
	g_string_free(got, TRUE);
	finish(server);
}

/* a symbolic link where the session goes: dropped, its target untouched
 * (the probe wrote through it) */
static void test_link_not_followed(void)
{
	SERVER_REC *server = connected_server();
	char *victim = g_strdup_printf("%s/victim", get_irssi_dir()), *data = NULL;

	g_assert_true(g_file_set_contents(victim, "precious", -1, NULL));
	g_assert_cmpint(symlink(victim, session_path), ==, 0);
	upgrade();
	g_assert_cmpint(gui_exits, ==, 1);
	g_assert_true(g_file_get_contents(victim, &data, NULL, NULL));
	g_assert_cmpstr(data, ==, "precious");
	g_assert_false(g_file_test(session_path, G_FILE_TEST_IS_SYMLINK));
	g_assert_true(g_file_test(session_path, G_FILE_TEST_IS_REGULAR));
	g_free(data);
	g_unlink(victim);
	g_free(victim);
	finish(server);
}

/* a session file that can be written: the server is handed over */
static void test_upgrade_written(void)
{
	SERVER_REC *server = connected_server();
	CONFIG_REC *session;

	upgrade();
	g_assert_cmpstr(dialogs->str, ==, "");
	g_assert_cmpint(gui_exits, ==, 1);
	g_assert_true(server->disconnected);
	g_assert_null(g_slist_find(servers, server));
	session = config_open(session_path, -1);
	g_assert_nonnull(session);
	g_assert_cmpint(config_parse(session), ==, 0);
	g_assert_cmpstr(config_node_get_str(config_node_nth(config_node_traverse(session, "(servers", FALSE), 0),
	                                    "address", NULL), ==, "irc.example.net");
	config_close(session);
	finish(server);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/upgrade/refused-before-save", test_refused_before_save);
	g_test_add_func("/upgrade/refused-after-save", test_refused_after_save);
	g_test_add_func("/upgrade/refused-after-save-sends-once",
	                test_refused_after_save_sends_once);
	g_test_add_func("/upgrade/link-not-followed", test_link_not_followed);
	g_test_add_func("/upgrade/written", test_upgrade_written);

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	signal(SIGPIPE, SIG_IGN);

	args_execute(0, NULL);
	core_init();
	irc_core_init();
	signal_emit("irssi init finished", 0);
	signal_add_last("session restore server", (SIGNAL_FUNC) sig_restore_server);
	signal_add("gui exit", (SIGNAL_FUNC) sig_gui_exit);
	signal_add("gui dialog", (SIGNAL_FUNC) sig_gui_dialog);
	signal_add_first("session save", (SIGNAL_FUNC) sig_session_save);
	dialogs = g_string_new(NULL);
	session_path = g_strdup_printf("%s/session", get_irssi_dir());

	return g_test_run();
}
