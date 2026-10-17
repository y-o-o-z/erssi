/*
 * Tests for the anti-floodnet module: it counts senders, not messages,
 * spares the people you talk to (but not a query the flood itself opened),
 * caps its memory and lets go of a server that is gone.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/queries.h>
#include <irssi/src/core/servers-setup.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>

#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/printtext.h>

#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/irc-queries.h>
#include <irssi/src/irc/anti-floodnet/anti-floodnet.h>

#include <string.h>
#include "../../test-home.h"

/* irc-core.c */
void irc_core_init(void);
void irc_core_deinit(void);

/* irc-session.c */
void irc_session_init(void);
void irc_session_deinit(void);

/* fe-common-irc.c */
void fe_common_irc_init(void);

#define MODULE_NAME "tests"

extern ANTI_FLOODNET_REC *floodnet;

static SERVER_REC *server;
static int delivered;
static GString *printed;

static void probe(void)
{
	delivered++;
}

static void sig_print_text(TEXT_DEST_REC *dest, const char *text, const char *stripped)
{
	g_string_append(printed, stripped);
	g_string_append_c(printed, '\n');
}

static void irc_server_init_bare_minimum(IRC_SERVER_REC *irc)
{
	irc->isupport = g_hash_table_new((GHashFunc) i_istr_hash, (GCompareFunc) i_istr_equal);
	g_hash_table_insert(irc->isupport, g_strdup("CHANMODES"), g_strdup("beI,k,l,imnpst"));
	g_hash_table_insert(irc->isupport, g_strdup("PREFIX"), g_strdup("(ohv)@%+"));
}

/* a real IRC server record that never sends anything; its nick is "user" */
static SERVER_REC *new_server(void)
{
	CHAT_PROTOCOL_REC *proto = chat_protocol_find("IRC");
	SERVER_CONNECT_REC *conn;
	SERVER_REC *rec;

	conn = server_create_conn(proto->id, "localhost", 0, "", "", "user");
	rec = proto->server_init_connect(conn);
	rec->session_reconnect = TRUE;
	rec->tag = g_strdup("testserver");

	/* skip the session handlers, which would try to send data */
	irc_session_deinit();
	irc_irc_deinit();
	server_connect_finished(rec);
	irc_server_init_bare_minimum(IRC_SERVER(rec));
	irc_irc_init();
	irc_session_init();

	rec->connection_lost = TRUE;
	return rec;
}

static void set_up(void)
{
	irc_anti_floodnet_deinit();
	irc_anti_floodnet_init();
	delivered = 0;
	g_string_truncate(printed, 0);
	server = new_server();
}

static void tear_down(void)
{
	/* a query would come back with the next test's server, of the same tag */
	while (queries != NULL)
		query_destroy(queries->data);
	settings_set_level("autocreate_query_level", "MSGS");
	settings_set_int("anti_floodnet_time_window", 0);
	signal_emit("setup changed", 0);
	if (server != NULL) {
		server->connection_lost = FALSE;
		server_disconnect(server);
		server = NULL;
	}
}

static void pm(const char *nick, const char *address, const char *text)
{
	char *data = g_strdup_printf("user :%s", text);

	signal_emit("event privmsg", 4, server, data, nick, address);
	g_free(data);
}

static const char *line_n(int i)
{
	static char buf[32];

	g_snprintf(buf, sizeof(buf), "line number %d", i);
	return buf;
}

static void test_one_tilde_sender_pastes(void)
{
	int i;

	for (i = 0; i < 8; i++)
		pm("kumpel", "~kumpel@h.example.invalid", line_n(i));
	g_assert_cmpint(delivered, ==, 8);
	g_assert_false(floodnet->in_protection_mode);
}

static void test_tilde_senders_flood(void)
{
	int i;
	char nick[8], addr[64];

	for (i = 1; i <= 5; i++) {
		g_snprintf(nick, sizeof(nick), "fn%d", i);
		g_snprintf(addr, sizeof(addr), "~fn%d@h%d.example.invalid", i, i);
		pm(nick, addr, "KUP TANIO www.spam.example");
	}
	g_assert_true(floodnet->in_protection_mode);
	g_assert_cmpint(delivered, <=, 4);
}

/* not the duplicate rule: every text differs, only the ~idents give it away */
static void test_tilde_senders_flood_distinct_text(void)
{
	int i;
	char nick[8], addr[64];

	for (i = 1; i <= 5; i++) {
		g_snprintf(nick, sizeof(nick), "fn%d", i);
		g_snprintf(addr, sizeof(addr), "~fn%d@h%d.example.invalid", i, i);
		pm(nick, addr, line_n(i));
	}
	g_assert_true(floodnet->in_protection_mode);
	g_assert_cmpint(delivered, <=, 4);
}

static void test_same_text_one_sender(void)
{
	int i;

	for (i = 0; i < 5; i++)
		pm("bob", "bob@h.example.invalid", "hej hej");
	g_assert_cmpint(delivered, ==, 5);
}

static void test_same_text_three_senders(void)
{
	pm("x1", "x1@h1.example.invalid", "hello");
	pm("x2", "x2@h2.example.invalid", "hi");
	pm("a", "a@h3.example.invalid", "spam");
	pm("b", "b@h4.example.invalid", "spam");
	pm("c", "c@h5.example.invalid", "spam");
	g_assert_cmpint(delivered, ==, 4);
}

static void start_flood(void)
{
	int i;
	char nick[8], addr[64];

	for (i = 1; i <= 5; i++) {
		g_snprintf(nick, sizeof(nick), "fn%d", i);
		g_snprintf(addr, sizeof(addr), "~fn%d@h%d.example.invalid", i, i);
		pm(nick, addr, "KUP TANIO www.spam.example");
	}
	g_assert_true(floodnet->in_protection_mode);
}

/* a query the user opened with /QUERY */
static void test_query_partner_passes(void)
{
	start_flood();

	irc_query_create(server->tag, "friend", FALSE);
	delivered = 0;
	pm("friend", "~friend@h.example.invalid", "cze");
	g_assert_cmpint(delivered, ==, 1);
}

/* a query that was there before, such as one a restored layout made */
static void test_earlier_query_passes(void)
{
	irc_query_create(server->tag, "friend", TRUE);
	start_flood();

	delivered = 0;
	pm("friend", "~friend@h.example.invalid", "cze");
	g_assert_cmpint(delivered, ==, 1);
}

/* With the default autocreate_query_level, every message that gets
 * through opens a query with its sender. The first bots of a flood get
 * through before protection starts; their queries must not let the rest
 * of their flood in. */
static void test_flood_made_query_not_exempt(void)
{
	int i;
	char nick[8], addr[64];

	start_flood();
	g_assert_nonnull(query_find(server, "fn1"));

	delivered = 0;
	for (i = 1; i <= 5; i++) {
		g_snprintf(nick, sizeof(nick), "fn%d", i);
		g_snprintf(addr, sizeof(addr), "~fn%d@h%d.example.invalid", i, i);
		pm(nick, addr, line_n(i));
	}
	g_assert_cmpint(delivered, ==, 0);
}

/* the same for the duplicate rule, with senders that have an ident */
static void test_flood_made_query_duplicate_text(void)
{
	pm("x1", "x1@h1.example.invalid", "hello");
	pm("x2", "x2@h2.example.invalid", "hi");
	pm("a", "a@h3.example.invalid", "spam");
	pm("b", "b@h4.example.invalid", "spam");
	pm("c", "c@h5.example.invalid", "spam");
	g_assert_true(floodnet->in_protection_mode);
	g_assert_nonnull(query_find(server, "a"));

	delivered = 0;
	pm("a", "a@h3.example.invalid", "spam");
	pm("b", "b@h4.example.invalid", "spam");
	g_assert_cmpint(delivered, ==, 0);
}

/* someone who opened a query and got an answer is someone you talk to */
static void test_answered_query_passes(void)
{
	pm("friend", "~friend@h.example.invalid", "hej");
	g_assert_nonnull(query_find(server, "friend"));
	signal_emit("message own_private", 4, server, "hej, co tam?", "friend", "friend");
	start_flood();

	delivered = 0;
	pm("friend", "~friend@h.example.invalid", "nic");
	g_assert_cmpint(delivered, ==, 1);
}

/* the answer can also come back from a bouncer as our own message */
static void test_answer_from_bouncer_passes(void)
{
	pm("friend", "~friend@h.example.invalid", "hej");
	signal_emit("event privmsg", 4, server, "friend :hej, co tam?", "user", "user@h.example.invalid");
	start_flood();

	delivered = 0;
	pm("friend", "~friend@h.example.invalid", "nic");
	g_assert_cmpint(delivered, ==, 1);
}

/* a private NOTICE opens a query too when autocreate_query_level has
 * NOTICES */
static void test_notice_made_query_not_exempt(void)
{
	settings_set_level("autocreate_query_level", "MSGS NOTICES");
	signal_emit("setup changed", 0);

	signal_emit("event notice", 4, server, "user :KUP TANIO", "nb", "~nb@h.example.invalid");
	g_assert_nonnull(query_find(server, "nb"));
	start_flood();

	delivered = 0;
	pm("nb", "~nb@h.example.invalid", "KUP TANIO 2");
	g_assert_cmpint(delivered, ==, 0);
}

/* the answer can be an ACTION, sent back by a bouncer (from another of
 * its clients, or as echo-message) */
static void test_answer_as_action_passes(void)
{
	pm("friend", "~friend@h.example.invalid", "hej");
	signal_emit("event privmsg", 4, server, "friend :\001ACTION macha\001", "user",
		    "user@h.example.invalid");
	start_flood();

	delivered = 0;
	pm("friend", "~friend@h.example.invalid", "nic");
	g_assert_cmpint(delivered, ==, 1);
}

/* or a NOTICE, sent back the same way */
static void test_answer_as_notice_passes(void)
{
	pm("friend", "~friend@h.example.invalid", "hej");
	signal_emit("event notice", 4, server, "friend :hej", "user", "user@h.example.invalid");
	start_flood();

	delivered = 0;
	pm("friend", "~friend@h.example.invalid", "nic");
	g_assert_cmpint(delivered, ==, 1);
}

static void test_server_message(void)
{
	pm("irc.example.invalid", NULL, "server notice");
	pm("irc.example.invalid", "", "server notice");
	g_assert_cmpint(delivered, ==, 2);
}

static void test_window_capped(void)
{
	int i;
	char nick[16], addr[64], text[32];

	/* only the cap counts here: no 600 query windows to slow the loop
	 * down, and no message so old that it leaves the window on a busy
	 * machine */
	settings_set_level("autocreate_query_level", "");
	settings_set_int("anti_floodnet_time_window", 3600);
	signal_emit("setup changed", 0);

	for (i = 0; i < 600; i++) {
		g_snprintf(nick, sizeof(nick), "n%d", i);
		g_snprintf(addr, sizeof(addr), "u%d@h%d.example.invalid", i, i);
		g_snprintf(text, sizeof(text), "distinct text %d", i);
		pm(nick, addr, text);
	}
	g_assert_cmpint(floodnet->message_count, ==, 500);
}

static void expire_all_blocks(void)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, floodnet->blocked_patterns);
	while (g_hash_table_iter_next(&iter, &key, &value))
		*(time_t *) value = time(NULL) - 1;
}

static void test_blocked_patterns_capped(void)
{
	int i;
	char text[16];

	for (i = 0; i < 1100; i++) {
		g_snprintf(text, sizeof(text), "t%d", i);
		block_duplicate_message(text, 60);
	}
	g_assert_cmpuint(g_hash_table_size(floodnet->blocked_patterns), ==, 1000);

	expire_all_blocks();
	block_duplicate_message("new", 60);
	g_assert_cmpuint(g_hash_table_size(floodnet->blocked_patterns), ==, 1);
}

static void test_protection_ends_without_messages(void)
{
	GSList *tmp;
	int i;
	char nick[8], addr[64];

	for (i = 1; i <= 5; i++) {
		g_snprintf(nick, sizeof(nick), "fn%d", i);
		g_snprintf(addr, sizeof(addr), "~fn%d@h%d.example.invalid", i, i);
		pm(nick, addr, "KUP TANIO www.spam.example");
	}
	g_assert_true(floodnet->in_protection_mode);

	for (tmp = floodnet->message_window; tmp != NULL; tmp = tmp->next)
		((FLOODMSG_REC *) tmp->data)->timestamp = time(NULL) - 100;
	expire_all_blocks();
	cleanup_old_messages(time(NULL));
	check_protection_status();

	g_assert_false(floodnet->in_protection_mode);
	g_assert_nonnull(strstr(printed->str, "Protection ended"));
}

static void test_ctcp_state_dropped_on_disconnect(void)
{
	IRC_SERVER_REC *gone;
	int i;

	for (i = 0; i < 5; i++)
		signal_emit("ctcp msg version", 5, server, "", "nick", "u@h.example.invalid", "user");
	g_assert_true(is_ctcp_blocked(IRC_SERVER(server)));

	/* the pointer is only a hash key from here on */
	gone = IRC_SERVER(server);
	server->connection_lost = FALSE;
	server_disconnect(server);
	server = NULL;
	g_assert_false(is_ctcp_blocked(gone));
}

typedef void (*TEST_FUNC)(void);

static void run_fixture(gconstpointer data)
{
	set_up();
	((TEST_FUNC) data)();
	tear_down();
}

static void add(const char *path, TEST_FUNC func)
{
	g_test_add_data_func(path, (gconstpointer) func, run_fixture);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	add("/anti-floodnet/one-tilde-sender-pastes", test_one_tilde_sender_pastes);
	add("/anti-floodnet/tilde-senders-flood", test_tilde_senders_flood);
	add("/anti-floodnet/tilde-senders-flood-distinct-text",
	    test_tilde_senders_flood_distinct_text);
	add("/anti-floodnet/same-text-one-sender", test_same_text_one_sender);
	add("/anti-floodnet/same-text-three-senders", test_same_text_three_senders);
	add("/anti-floodnet/query-partner-passes", test_query_partner_passes);
	add("/anti-floodnet/earlier-query-passes", test_earlier_query_passes);
	add("/anti-floodnet/flood-made-query-not-exempt", test_flood_made_query_not_exempt);
	add("/anti-floodnet/flood-made-query-duplicate-text",
	    test_flood_made_query_duplicate_text);
	add("/anti-floodnet/answered-query-passes", test_answered_query_passes);
	add("/anti-floodnet/answer-from-bouncer-passes", test_answer_from_bouncer_passes);
	add("/anti-floodnet/notice-made-query-not-exempt", test_notice_made_query_not_exempt);
	add("/anti-floodnet/answer-as-action-passes", test_answer_as_action_passes);
	add("/anti-floodnet/answer-as-notice-passes", test_answer_as_notice_passes);
	add("/anti-floodnet/server-message", test_server_message);
	add("/anti-floodnet/window-capped", test_window_capped);
	add("/anti-floodnet/blocked-patterns-capped", test_blocked_patterns_capped);
	add("/anti-floodnet/protection-ends-without-messages",
	    test_protection_ends_without_messages);
	add("/anti-floodnet/ctcp-state-dropped-on-disconnect",
	    test_ctcp_state_dropped_on_disconnect);

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;

	/* the core is brought up once: it is not meant to be re-initialised */
	args_execute(0, NULL);
	core_init();
	irc_core_init();
	fe_common_core_init();
	/* fe-events turns a private message into a query, as in the client */
	fe_common_irc_init();
	settings_set_level("autocreate_query_level", "MSGS");
	signal_emit("setup changed", 0);
	irc_anti_floodnet_init();
	signal_emit("irssi init finished", 0);
	signal_add_last("event privmsg", (SIGNAL_FUNC) probe);
	signal_add("print text", (SIGNAL_FUNC) sig_print_text);
	printed = g_string_new("");

	return g_test_run();
}
