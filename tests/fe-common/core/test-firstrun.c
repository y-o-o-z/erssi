/*
 * Tests for firstrun.c: the first-run setup questions, the answers applied
 * to the config, nick validation and the poll() based fd reader.
 */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#define _GNU_SOURCE /* RTLD_NEXT */
#include <irssi/src/common.h>
#include <glib/gstdio.h>
#include <irssi/src/lib-config/iconfig.h>
#include <irssi/src/fe-common/core/firstrun.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* tcsetattr() for firstrun.c (the definition in the executable comes before
 * the C library's): counts the calls that wait for the terminal's output to
 * drain (TCSADRAIN, TCSAFLUSH). On the BSDs such a call on a pty blocks until
 * the master has read all of it, so a terminal that has stopped reading (^S,
 * a stalled ssh) would hang the password prompt; Linux returns at once. */
static int draining_tcsetattr;
static int all_tcsetattr; /* every call: shows that firstrun.c comes here at all */

int tcsetattr(int fd, int action, const struct termios *t)
{
	static int (*real)(int, int, const struct termios *);

	if (real == NULL)
		real = (int (*)(int, int, const struct termios *)) dlsym(RTLD_NEXT, "tcsetattr");
	g_assert_nonnull(real);
	g_atomic_int_inc(&all_tcsetattr);
	if (action != TCSANOW)
		g_atomic_int_inc(&draining_tcsetattr);
	return real(fd, action, t);
}

static const char SAMPLE[] =
    "chatnets = {\n"
    "  IRCnet = { type = \"IRC\"; };\n"
    "  liberachat = { type = \"IRC\"; };\n"
    "  SILC = { type = \"SILC\"; };\n"
    "  Empty = { type = \"IRC\"; };\n"
    "  zunverified = { type = \"IRC\"; };\n"
    "};\n"
    "servers = (\n"
    "  { address = \"irc.atw-inter.net\"; chatnet = \"IRCnet\"; port = \"6667\"; },\n"
    "  { address = \"irc.example.net\"; chatnet = \"IRCnet\"; port = \"6668\"; },\n"
    "  { address = \"irc.libera.chat\"; chatnet = \"liberachat\"; port = \"6697\";"
    " use_tls = \"yes\"; },\n"
    "  { address = \"silc.example.org\"; chatnet = \"SILC\"; port = \"706\"; },\n"
    "  { address = \"irc.example.org\"; chatnet = \"zunverified\"; port = \"9999\";"
    " use_tls = \"yes\"; tls_verify = \"no\"; }\n"
    ");\n";

static CONFIG_REC *sample_config(void)
{
	CONFIG_REC *config = config_open(NULL, -1);
	g_assert_nonnull(config);
	g_assert_cmpint(config_parse_data(config, SAMPLE, "sample"), ==, 0);
	return config;
}

typedef struct {
	const char *const *lines; /* NULL terminated */
	int pos;
	gboolean secrets[16];
} SCRIPT;

static char *script_read(void *ctx, gboolean secret)
{
	SCRIPT *s = ctx;
	const char *line = s->lines[s->pos];

	if (line == NULL)
		return NULL;
	s->secrets[s->pos++] = secret;
	return g_strdup(line);
}

static char *slurp(FILE *f)
{
	long len;
	char *buf;

	fflush(f);
	len = ftell(f);
	buf = g_malloc0(len + 1);
	rewind(f);
	g_assert_cmpint(fread(buf, 1, len, f), ==, len);
	return buf;
}

static const char *const THEMES[] = { "shellter", "default", NULL };

static FIRSTRUN_RESULT run_ask(const char *const *lines, char **themes, FIRSTRUN_ANSWERS *a,
                               SCRIPT *s, char **outtext)
{
	CONFIG_REC *config = sample_config();
	GPtrArray *nets = firstrun_networks(config);
	FILE *out = tmpfile();
	FIRSTRUN_RESULT r;

	g_assert_nonnull(out);
	memset(s, 0, sizeof(*s));
	s->lines = lines;
	r = firstrun_ask(out, script_read, s, nets, themes, "defnick", "default", a);
	*outtext = slurp(out);
	fclose(out);
	g_ptr_array_free(nets, TRUE);
	config_close(config);
	return r;
}

static void test_wanted(void)
{
	g_assert_true(firstrun_wanted(FALSE, TRUE, FALSE, FALSE));
	g_assert_false(firstrun_wanted(TRUE, TRUE, FALSE, FALSE));
	g_assert_false(firstrun_wanted(FALSE, FALSE, FALSE, FALSE));
	g_assert_false(firstrun_wanted(FALSE, TRUE, FALSE, TRUE));
	g_assert_true(firstrun_wanted(TRUE, TRUE, TRUE, FALSE));
	g_assert_false(firstrun_wanted(TRUE, FALSE, TRUE, FALSE));
}

static void test_networks(void)
{
	CONFIG_REC *config = sample_config();
	GPtrArray *nets = firstrun_networks(config);
	FIRSTRUN_NETWORK *n;

	g_assert_cmpuint(nets->len, ==, 3);
	n = g_ptr_array_index(nets, 0);
	g_assert_cmpstr(n->chatnet, ==, "IRCnet");
	g_assert_cmpstr(n->address, ==, "irc.atw-inter.net");
	g_assert_cmpint(n->port, ==, 6667);
	g_assert_false(n->tls);
	n = g_ptr_array_index(nets, 1);
	g_assert_cmpstr(n->chatnet, ==, "liberachat");
	g_assert_cmpstr(n->address, ==, "irc.libera.chat");
	g_assert_cmpint(n->port, ==, 6697);
	g_assert_true(n->tls);
	g_assert_true(n->tls_verify);
	n = g_ptr_array_index(nets, 2);
	g_assert_cmpstr(n->chatnet, ==, "zunverified");
	g_assert_true(n->tls);
	g_assert_false(n->tls_verify);
	g_ptr_array_free(nets, TRUE);
	config_close(config);
}

static void test_ask_full(void)
{
	const char *const lines[] = { "", "2", "wizuser", "acct", "s3cret", "1", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_cmpstr(a.chatnet, ==, "liberachat");
	g_assert_cmpstr(a.nick, ==, "wizuser");
	g_assert_cmpstr(a.sasl_user, ==, "acct");
	g_assert_cmpstr(a.sasl_pass, ==, "s3cret");
	g_assert_cmpstr(a.theme, ==, "shellter");
	g_assert_false(s.secrets[0]);
	g_assert_false(s.secrets[1]);
	g_assert_false(s.secrets[2]);
	g_assert_false(s.secrets[3]);
	g_assert_true(s.secrets[4]);
	g_assert_false(s.secrets[5]);
	g_assert_nonnull(strstr(out, "Network [1]:"));
	g_assert_nonnull(strstr(out, "SASL account on liberachat (empty = none):"));
	g_assert_nonnull(strstr(out, "  2) liberachat   irc.libera.chat:6697 TLS"));
	g_assert_nonnull(strstr(out, "  0) none - connect later with /CONNECT"));
	g_assert_null(strstr(out, "s3cret"));
	g_free(out);
	firstrun_answers_clear(&a);
	g_assert_null(a.sasl_pass);
}

static void test_ask_defaults_plaintext(void)
{
	const char *const lines[] = { "", "", "", "", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_cmpstr(a.chatnet, ==, "IRCnet");
	g_assert_cmpstr(a.nick, ==, "defnick");
	g_assert_cmpstr(a.theme, ==, "default");
	g_assert_null(a.sasl_user);
	g_assert_null(a.sasl_pass);
	g_assert_null(strstr(out, "SASL account"));
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_ask_unverified_tls_no_sasl(void)
{
	const char *const lines[] = { "", "3", "nick1", "", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_cmpstr(a.chatnet, ==, "zunverified");
	g_assert_null(a.sasl_user);
	g_assert_null(strstr(out, "SASL account"));
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_ask_stored_password_wiped_on_eof(void)
{
	const char *const lines[] = { "", "2", "wizuser", "acct", "s3cret", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_SKIPPED);
	g_assert_null(a.sasl_pass);
	g_assert_null(a.sasl_user);
	g_assert_null(a.nick);
	g_free(out);
}

static void test_ask_account_control_bytes(void)
{
	const char *const lines[] = { "", "2", "wizuser", "ac\x01" "ct", "acct", "pw", "1", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_cmpstr(a.sasl_user, ==, "acct");
	g_assert_cmpstr(a.sasl_pass, ==, "pw");
	g_assert_nonnull(strstr(out, "not a valid account"));
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_ask_none(void)
{
	const char *const lines[] = { "y", "0", "nick1", "", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_null(a.chatnet);
	g_assert_cmpstr(a.nick, ==, "nick1");
	g_assert_null(a.sasl_user);
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_ask_no(void)
{
	const char *const lines[] = { "n", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_SKIPPED);
	g_assert_cmpint(s.pos, ==, 1);
	g_assert_nonnull(strstr(out, "Set up now? [Y/n]"));
	g_assert_null(strstr(out, "Network"));
	g_free(out);
}

static void test_ask_eof_skips(void)
{
	const char *const lines[] = { "", "1", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_SKIPPED);
	g_assert_null(a.chatnet);
	g_assert_null(a.nick);
	g_assert_null(a.sasl_user);
	g_assert_null(a.sasl_pass);
	g_assert_null(a.theme);
	g_free(out);
}

static void test_ask_eof_at_password_wipes(void)
{
	const char *const lines[] = { "", "2", "wizuser", "acct", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_SKIPPED);
	g_assert_null(a.nick);
	g_assert_null(a.sasl_user);
	g_free(out);
}

static void test_ask_empty_account_skips_password(void)
{
	const char *const lines[] = { "", "2", "wizuser", "", "", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_null(a.sasl_user);
	g_assert_null(a.sasl_pass);
	g_assert_null(strstr(out, "SASL password"));
	g_assert_cmpstr(a.theme, ==, "default");
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_ask_reask_nick(void)
{
	const char *const lines[] = { "", "1", "bad nick", "good_nick", "", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, (char **) THEMES, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_cmpstr(a.nick, ==, "good_nick");
	g_assert_nonnull(strstr(out, "not a valid nick"));
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_ask_bad_choice_3x(void)
{
	const char *const lines[] = { "", "99", "abc", "-1", "x y", "", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, NULL, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_cmpstr(a.chatnet, ==, "IRCnet");
	g_assert_cmpstr(a.nick, ==, "defnick");
	g_assert_cmpstr(a.theme, ==, "default");
	g_assert_nonnull(strstr(out, "not a valid choice"));
	g_assert_nonnull(strstr(out, "not a valid nick"));
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_ask_bad_nick_3x(void)
{
	const char *const lines[] = { "", "0", "a b", "c d", "e f", "", NULL };
	FIRSTRUN_ANSWERS a;
	SCRIPT s;
	char *out;

	g_assert_cmpint(run_ask(lines, NULL, &a, &s, &out), ==, FIRSTRUN_DONE);
	g_assert_cmpstr(a.nick, ==, "defnick");
	g_assert_cmpint(s.pos, ==, 5);
	g_free(out);
	firstrun_answers_clear(&a);
}

static void test_apply(void)
{
	CONFIG_REC *config = sample_config();
	FIRSTRUN_ANSWERS a = { g_strdup("liberachat"), g_strdup("n"), g_strdup("acct"),
	                       g_strdup("s3cret"), NULL };
	GSList *tmp;
	CONFIG_NODE *servers;
	int seen = 0;

	firstrun_apply_config(config, &a);
	g_assert_cmpstr(config_get_str(config, "chatnets/liberachat", "sasl_mechanism", NULL), ==,
	                "PLAIN");
	g_assert_cmpstr(config_get_str(config, "chatnets/liberachat", "sasl_username", NULL), ==,
	                "acct");
	g_assert_cmpstr(config_get_str(config, "chatnets/liberachat", "sasl_password", NULL), ==,
	                "s3cret");
	g_assert_null(config_get_str(config, "chatnets/IRCnet", "sasl_mechanism", NULL));

	servers = config_node_traverse(config, "servers", FALSE);
	g_assert_nonnull(servers);
	for (tmp = config_node_first(servers->value); tmp != NULL; tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;
		const char *addr = config_node_get_str(node, "address", "");

		if (strcmp(addr, "irc.libera.chat") == 0) {
			g_assert_true(config_node_get_bool(node, "autoconnect", FALSE));
			seen++;
		} else {
			g_assert_null(config_node_find(node, "autoconnect"));
		}
	}
	g_assert_cmpint(seen, ==, 1);
	firstrun_answers_clear(&a);
	config_close(config);
}

static void test_apply_autoconnect_first_server_only(void)
{
	CONFIG_REC *config = sample_config();
	FIRSTRUN_ANSWERS a = { g_strdup("IRCnet"), g_strdup("n"), NULL, NULL, NULL };
	CONFIG_NODE *servers;
	GSList *l;
	int n = 0;

	firstrun_apply_config(config, &a);
	servers = config_node_traverse(config, "servers", FALSE);
	for (l = config_node_first(servers->value); l != NULL; l = config_node_next(l), n++) {
		CONFIG_NODE *node = l->data;
		if (n == 0)
			g_assert_true(config_node_get_bool(node, "autoconnect", FALSE));
		else
			g_assert_null(config_node_find(node, "autoconnect"));
	}
	g_assert_null(config_get_str(config, "chatnets/IRCnet", "sasl_mechanism", NULL));
	firstrun_answers_clear(&a);
	config_close(config);
}

static char *write_config(CONFIG_REC *config)
{
	char *dir = g_dir_make_tmp("firstrun-XXXXXX", NULL);
	char *path = g_build_filename(dir, "cfg", NULL);
	char *data = NULL;

	g_assert_nonnull(dir);
	g_assert_cmpint(config_write(config, path, 0600), ==, 0);
	g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
	g_remove(path);
	g_rmdir(dir);
	g_free(path);
	g_free(dir);
	return data;
}

static void test_apply_none(void)
{
	CONFIG_REC *config = sample_config();
	FIRSTRUN_ANSWERS a = { NULL, g_strdup("n"), NULL, NULL, g_strdup("default") };
	char *before = write_config(config);
	char *after;

	firstrun_apply_config(config, &a);
	after = write_config(config);
	g_assert_cmpstr(before, ==, after);
	g_free(before);
	g_free(after);
	firstrun_answers_clear(&a);
	config_close(config);
}

static void test_apply_unknown_chatnet(void)
{
	CONFIG_REC *config = sample_config();
	FIRSTRUN_ANSWERS a = { g_strdup("Nope"), g_strdup("n"), g_strdup("acct"),
	                       g_strdup("pw"), NULL };
	char *before = write_config(config);
	char *after;

	firstrun_apply_config(config, &a);
	after = write_config(config);
	g_assert_cmpstr(before, ==, after);
	g_free(before);
	g_free(after);
	firstrun_answers_clear(&a);
	config_close(config);
}

static void test_apply_only_expected_keys(void)
{
	CONFIG_REC *config = sample_config();
	FIRSTRUN_ANSWERS a = { g_strdup("liberachat"), g_strdup("n"), g_strdup("acct"),
	                       g_strdup("pw"), NULL };
	CONFIG_NODE *net, *servers;
	GSList *l;

	firstrun_apply_config(config, &a);
	net = config_node_traverse(config, "chatnets/liberachat", FALSE);
	g_assert_nonnull(net);
	/* type + the three SASL keys, nothing else */
	g_assert_cmpuint(g_slist_length(net->value), ==, 4);
	g_assert_cmpstr(config_node_get_str(net, "type", NULL), ==, "IRC");
	net = config_node_traverse(config, "chatnets/IRCnet", FALSE);
	g_assert_cmpuint(g_slist_length(net->value), ==, 1);
	servers = config_node_traverse(config, "servers", FALSE);
	for (l = servers->value; l != NULL; l = l->next) {
		CONFIG_NODE *node = l->data;
		const char *addr = config_node_get_str(node, "address", "");

		/* the libera node gains exactly autoconnect (4 keys before) */
		if (strcmp(addr, "irc.libera.chat") == 0)
			g_assert_cmpuint(g_slist_length(node->value), ==, 5);
	}
	firstrun_answers_clear(&a);
	config_close(config);
}

static void test_nick_valid(void)
{
	char long31[32], long30[31];

	memset(long31, 'a', 31);
	long31[31] = '\0';
	memset(long30, 'a', 30);
	long30[30] = '\0';
	g_assert_true(firstrun_nick_valid("alice"));
	g_assert_true(firstrun_nick_valid("a-b_[]{}|^`\\"));
	g_assert_false(firstrun_nick_valid("-x"));
	g_assert_false(firstrun_nick_valid("1x"));
	g_assert_false(firstrun_nick_valid(""));
	g_assert_false(firstrun_nick_valid(NULL));
	g_assert_false(firstrun_nick_valid("has space"));
	g_assert_true(firstrun_nick_valid(long30));
	g_assert_false(firstrun_nick_valid(long31));
	g_assert_false(firstrun_nick_valid("za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87"));
}

static void test_read_fd_timeout(void)
{
	int p[2];
	gint64 t0;

	g_assert_cmpint(pipe(p), ==, 0);
	t0 = g_get_monotonic_time();
	g_assert_null(firstrun_read_fd(p[0], 50, FALSE));
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 1000000);
	close(p[0]);
	close(p[1]);
}

static void test_read_fd_line(void)
{
	int p[2];
	char *s;

	g_assert_cmpint(pipe(p), ==, 0);
	g_assert_cmpint(write(p[1], "abc\r\n", 5), ==, 5);
	s = firstrun_read_fd(p[0], 500, FALSE);
	g_assert_cmpstr(s, ==, "abc");
	g_free(s);
	close(p[1]);
	g_assert_null(firstrun_read_fd(p[0], 500, FALSE));
	close(p[0]);
}

static void test_read_fd_two_lines(void)
{
	int p[2];
	char *s;

	g_assert_cmpint(pipe(p), ==, 0);
	g_assert_cmpint(write(p[1], "one\n\ntwo\n", 9), ==, 9);
	s = firstrun_read_fd(p[0], 500, FALSE);
	g_assert_cmpstr(s, ==, "one");
	g_free(s);
	s = firstrun_read_fd(p[0], 500, FALSE);
	g_assert_cmpstr(s, ==, "");
	g_free(s);
	s = firstrun_read_fd(p[0], 500, FALSE);
	g_assert_cmpstr(s, ==, "two");
	g_free(s);
	close(p[0]);
	close(p[1]);
}

/* documented rule: a partial line then EOF returns the text, then timeout returns NULL */
static void test_read_fd_partial_eof(void)
{
	int p[2];
	char *s;

	g_assert_cmpint(pipe(p), ==, 0);
	g_assert_cmpint(write(p[1], "part", 4), ==, 4);
	close(p[1]);
	s = firstrun_read_fd(p[0], 500, FALSE);
	g_assert_cmpstr(s, ==, "part");
	g_free(s);
	g_assert_null(firstrun_read_fd(p[0], 500, FALSE));
	close(p[0]);
}

static void test_read_fd_partial_timeout(void)
{
	int p[2];
	gint64 t0;

	g_assert_cmpint(pipe(p), ==, 0);
	g_assert_cmpint(write(p[1], "part", 4), ==, 4);
	t0 = g_get_monotonic_time();
	g_assert_null(firstrun_read_fd(p[0], 100, FALSE));
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 1000000);
	close(p[0]);
	close(p[1]);
}

static void test_read_fd_overlong(void)
{
	int p[2];
	char big[1200];
	char *s;

	memset(big, 'x', sizeof(big));
	big[sizeof(big) - 1] = '\n';
	g_assert_cmpint(pipe(p), ==, 0);
	g_assert_cmpint(write(p[1], big, sizeof(big)), ==, (int) sizeof(big));
	g_assert_cmpint(write(p[1], "next\n", 5), ==, 5);
	s = firstrun_read_fd(p[0], 500, FALSE);
	g_assert_cmpuint(strlen(s), ==, 511);
	g_free(s);
	s = firstrun_read_fd(p[0], 500, FALSE);
	g_assert_cmpstr(s, ==, "next");
	g_free(s);
	close(p[0]);
	close(p[1]);
}

static void test_read_fd_zero_timeout_polls(void)
{
	int p[2];
	char *s;

	g_assert_cmpint(pipe(p), ==, 0);
	g_assert_null(firstrun_read_fd(p[0], 0, FALSE));
	g_assert_cmpint(write(p[1], "x\n", 2), ==, 2);
	s = firstrun_read_fd(p[0], 0, FALSE);
	g_assert_cmpstr(s, ==, "x");
	g_free(s);
	close(p[0]);
	close(p[1]);
}

static void noop_handler(int sig)
{
	(void) sig;
}

/* a handled signal (SIGTERM/SIGHUP in core) aborts the read instead of being swallowed */
static void test_read_fd_eintr_aborts(void)
{
	struct sigaction sa, old;
	int p[2];
	gint64 t0;

	g_assert_cmpint(pipe(p), ==, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = noop_handler; /* no SA_RESTART */
	sigemptyset(&sa.sa_mask);
	g_assert_cmpint(sigaction(SIGALRM, &sa, &old), ==, 0);
	t0 = g_get_monotonic_time();
	alarm(1);
	g_assert_null(firstrun_read_fd(p[0], 10000, FALSE));
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 5000000);
	alarm(0);
	sigaction(SIGALRM, &old, NULL);
	close(p[0]);
	close(p[1]);
}

static void test_clear_idempotent(void)
{
	FIRSTRUN_ANSWERS a = { g_strdup("c"), g_strdup("n"), g_strdup("u"), g_strdup("p"),
	                       g_strdup("t") };

	firstrun_answers_clear(&a);
	g_assert_null(a.chatnet);
	g_assert_null(a.sasl_pass);
	firstrun_answers_clear(&a);
	firstrun_answers_clear(NULL);
}

/* a real terminal: no echo while reading a secret, ECHO restored on every exit */
static int open_pty(int *master)
{
	int slave;
	char *name;

	*master = posix_openpt(O_RDWR | O_NOCTTY);
	if (*master < 0)
		return -1;
	if (grantpt(*master) != 0 || unlockpt(*master) != 0 ||
	    (name = ptsname(*master)) == NULL) {
		close(*master);
		return -1;
	}
	slave = open(name, O_RDWR | O_NOCTTY);
	if (slave < 0)
		close(*master);
	return slave;
}

/* The terminal of a test: a thread that types on the master side of a pty
 * and, like a terminal emulator, reads what is written to the slave (the
 * prompt, the newline after a secret) until late_join(). A stalled terminal
 * (^S, a stalled ssh) never reads it. */
typedef struct {
	int fd;
	const char *text;
	gboolean stalled;
	gint stop;
	GString *seen; /* what the terminal read */
} LATE_WRITE;

static void late_init(LATE_WRITE *w, int master, const char *text)
{
	w->fd = master;
	w->text = text;
	w->stalled = FALSE;
	w->stop = 0;
	w->seen = g_string_new(NULL);
}

/* reads the master until late_join() */
static void terminal_reads(LATE_WRITE *w)
{
	char buf[256];

	while (!w->stalled && !g_atomic_int_get(&w->stop)) {
		struct pollfd pfd = { w->fd, POLLIN, 0 };
		ssize_t n;

		if (poll(&pfd, 1, 10) <= 0)
			continue;
		n = read(w->fd, buf, sizeof(buf));
		if (n > 0)
			g_string_append_len(w->seen, buf, n);
		else if (n == 0 || errno != EINTR)
			break;
	}
}

/* what the terminal read, and what is still there to read; g_free() it */
static char *late_join(GThread *th, LATE_WRITE *w)
{
	char buf[256];
	int fl;
	ssize_t n;

	g_atomic_int_set(&w->stop, 1);
	g_thread_join(th);
	fl = fcntl(w->fd, F_GETFL);
	g_assert_cmpint(fcntl(w->fd, F_SETFL, fl | O_NONBLOCK), ==, 0);
	while ((n = read(w->fd, buf, sizeof(buf))) > 0)
		g_string_append_len(w->seen, buf, n);
	g_assert_cmpint(fcntl(w->fd, F_SETFL, fl), ==, 0);
	return g_string_free(w->seen, FALSE);
}

/* types the answer after ECHO has been turned off by the reader */
static gpointer late_writer(gpointer data)
{
	LATE_WRITE *w = data;
	gpointer ret = NULL;

	g_usleep(200 * 1000);
	if (write(w->fd, w->text, strlen(w->text)) < 0)
		ret = GINT_TO_POINTER(1);
	terminal_reads(w);
	return ret;
}

static void test_read_fd_secret_pty(void)
{
	LATE_WRITE w;
	GThread *th;
	int master, slave;
	struct termios t;
	char *s, *out;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	t.c_lflag |= ECHO;
	g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);

	late_init(&w, master, "hunter2\n");
	th = g_thread_new("late-writer", late_writer, &w);
	s = firstrun_read_fd(slave, 3000, TRUE);
	out = late_join(th, &w);
	g_assert_cmpstr(s, ==, "hunter2");
	g_free(s);
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	g_assert_true(t.c_lflag & ECHO);

	/* nothing typed from the master side may have been echoed back */
	g_assert_null(strstr(out, "hunter2"));
	g_free(out);

	/* timeout path restores ECHO too */
	g_assert_null(firstrun_read_fd(slave, 50, TRUE));
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	g_assert_true(t.c_lflag & ECHO);

	/* partial line then timeout: NULL, ECHO restored */
	g_assert_cmpint(write(master, "zz", 2), ==, 2);
	g_assert_null(firstrun_read_fd(slave, 50, TRUE));
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	g_assert_true(t.c_lflag & ECHO);

	close(slave);
	close(master);
}

static void pty_late_write(int master, const char *text, GThread **th, LATE_WRITE *w)
{
	late_init(w, master, text);
	*th = g_thread_new("late-writer", late_writer, w);
}

static void assert_cooked(int slave)
{
	struct termios t;

	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	g_assert_true(t.c_lflag & ECHO);
	g_assert_true(t.c_lflag & ISIG);
	g_assert_true(t.c_lflag & ICANON);
}

static volatile sig_atomic_t raised, cooked_at_signal;
static int signal_tty = -1;
static volatile sig_atomic_t abort_flag;
static gboolean tstp_aborts; /* SIGTSTP also sets abort_flag (a SIGTERM while stopped) */

static void record_signal(int signo)
{
	struct termios t;

	if (signo == SIGTSTP && tstp_aborts)
		abort_flag = 1;

	/* tcgetattr is async-signal-safe: the state the signal sees */
	cooked_at_signal = signal_tty >= 0 && tcgetattr(signal_tty, &t) == 0 &&
	                   (t.c_lflag & (ECHO | ICANON | ISIG)) == (ECHO | ICANON | ISIG);
	raised = signo;
}

/* SIGINT, SIGQUIT and SIGTSTP record instead of acting; returns the old actions */
static void record_signals(struct sigaction old[3])
{
	static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP };
	struct sigaction sa;
	int i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = record_signal;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < 3; i++)
		g_assert_cmpint(sigaction(sigs[i], &sa, &old[i]), ==, 0);
	raised = 0;
}

static void restore_signals(const struct sigaction old[3])
{
	sigaction(SIGINT, &old[0], NULL);
	sigaction(SIGQUIT, &old[1], NULL);
	sigaction(SIGTSTP, &old[2], NULL);
}

/* ^C and ^\ abort the read and then give the signal the terminal would have sent */
static void test_read_fd_secret_interrupt(void)
{
	const char *const inputs[] = { "ab\003", "ab\034" };
	const int expect[] = { SIGINT, SIGQUIT };
	struct sigaction old[3];
	int master, slave, i;
	struct termios t;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	t.c_lflag |= ECHO | ISIG | ICANON;
	g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);

	for (i = 0; i < 2; i++) {
		LATE_WRITE w;
		GThread *th;
		gint64 t0 = g_get_monotonic_time();
		char *s;

		record_signals(old);
		signal_tty = slave;
		pty_late_write(master, inputs[i], &th, &w);
		s = firstrun_read_fd(slave, 5000, TRUE);
		g_free(late_join(th, &w));
		restore_signals(old);
		signal_tty = -1;
		g_assert_null(s);
		g_assert_cmpint(raised, ==, expect[i]);
		/* raised after the terminal was restored */
		g_assert_true(cooked_at_signal);
		/* aborted at the control byte, not by the timeout */
		g_assert_cmpint(g_get_monotonic_time() - t0, <, 4000000);
		assert_cooked(slave);
	}
	close(slave);
	close(master);
}

/* "ab", ^Z; then (the reader has raised SIGTSTP and read again) "cd", Enter */
static gpointer suspend_writer(gpointer data)
{
	LATE_WRITE *w = data;
	gpointer ret = GINT_TO_POINTER(1);

	g_usleep(200 * 1000);
	if (write(w->fd, "ab\032", 3) != 3)
		goto out;
	/* "cd" comes after the restart (the drop of input typed while stopped
	 * is not tested: a pty delivers input asynchronously, so a byte written
	 * from the handler can still land after the reader's input flush) */
	while (!raised)
		g_usleep(1000);
	g_usleep(300 * 1000);
	if (write(w->fd, "cd\n", 3) != 3)
		goto out;
	ret = NULL;
out:
	terminal_reads(w);
	return ret;
}

/* ^Z restores the terminal, raises SIGTSTP and reads the secret again from the start */
static void test_read_fd_secret_suspend(void)
{
	struct sigaction old[3];
	struct termios t;
	LATE_WRITE w;
	GThread *th;
	int master, slave;
	char *s, *out;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	t.c_lflag |= ECHO | ISIG | ICANON;
	g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);
	record_signals(old);
	signal_tty = slave;
	late_init(&w, master, NULL);
	th = g_thread_new("suspend-writer", suspend_writer, &w);
	s = firstrun_read_fd(slave, 5000, TRUE);
	out = late_join(th, &w);
	restore_signals(old);
	signal_tty = -1;
	g_assert_cmpint(raised, ==, SIGTSTP);
	g_assert_true(cooked_at_signal);
	g_assert_cmpstr(s, ==, "cd");
	g_free(s);
	assert_cooked(slave);
	/* after the resume the prompt is shown again */
	g_assert_nonnull(strstr(out, FIRSTRUN_SECRET_PROMPT));
	g_free(out);
	close(slave);
	close(master);
}

/* a set abort flag: the read does not start, nothing is consumed */
static void test_read_fd_abort_at_start(void)
{
	int master, slave;
	char *s;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	g_assert_cmpint(write(master, "x\n", 2), ==, 2);
	abort_flag = 1;
	firstrun_set_abort_flag(&abort_flag);
	g_assert_null(firstrun_read_fd(slave, 2000, FALSE));
	g_assert_null(firstrun_read_fd(slave, 2000, TRUE));
	abort_flag = 0;
	s = firstrun_read_fd(slave, 2000, FALSE);
	firstrun_set_abort_flag(NULL);
	g_assert_cmpstr(s, ==, "x");
	g_free(s);
	close(slave);
	close(master);
}

/* "ab", then the flag is set (as a signal handler would), then "c\n" */
static gpointer abort_writer(gpointer data)
{
	LATE_WRITE *w = data;
	gpointer ret = GINT_TO_POINTER(1);

	g_usleep(200 * 1000);
	if (write(w->fd, "ab", 2) != 2)
		goto out;
	g_usleep(200 * 1000);
	/* Set from a thread, like a signal handler sets it from the middle of the
	   read. It is a plain volatile sig_atomic_t, exactly what production
	   uses, and the write() below orders it before the reader's next byte;
	   a data race detector would flag it, which is the point of the model. */
	abort_flag = 1;
	if (write(w->fd, "c\n", 2) != 2)
		goto out;
	ret = NULL;
out:
	terminal_reads(w);
	return ret;
}

/* the flag is checked between bytes, so a line that keeps coming is not taken */
static void test_read_fd_abort_between_bytes(void)
{
	gboolean secret;

	for (secret = FALSE; secret <= TRUE; secret++) {
		LATE_WRITE w;
		GThread *th;
		int master, slave;
		char *s;

		slave = open_pty(&master);
		if (slave < 0) {
			g_test_skip("no pty available");
			return;
		}
		abort_flag = 0;
		firstrun_set_abort_flag(&abort_flag);
		late_init(&w, master, NULL);
		th = g_thread_new("abort-writer", abort_writer, &w);
		s = firstrun_read_fd(slave, 4000, secret);
		g_free(late_join(th, &w));
		firstrun_set_abort_flag(NULL);
		g_assert_null(s);
		assert_cooked(slave);
		close(slave);
		close(master);
	}
	abort_flag = 0;
}

/* a SIGTERM that came while stopped at the password prompt (kill %1 sends SIGTERM and
 * SIGCONT): after the resume the read ends, without taking the terminal again */
static void test_read_fd_abort_after_suspend(void)
{
	struct sigaction old[3];
	LATE_WRITE w;
	GThread *th;
	int master, slave;
	gint64 t0;
	char *s, *out;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	record_signals(old);
	abort_flag = 0;
	tstp_aborts = TRUE;
	firstrun_set_abort_flag(&abort_flag);
	t0 = g_get_monotonic_time();
	pty_late_write(master, "ab\032", &th, &w);
	s = firstrun_read_fd(slave, 4000, TRUE);
	out = late_join(th, &w);
	firstrun_set_abort_flag(NULL);
	tstp_aborts = FALSE;
	restore_signals(old);
	g_assert_cmpint(raised, ==, SIGTSTP);
	g_assert_null(s);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 3000000);
	assert_cooked(slave);
	g_assert_null(strstr(out, FIRSTRUN_SECRET_PROMPT));
	g_free(out);
	abort_flag = 0;
	close(slave);
	close(master);
}

static void test_printable(void)
{
	g_assert_true(firstrun_printable(""));
	g_assert_true(firstrun_printable("irc.example.net ~[]"));
	g_assert_false(firstrun_printable(NULL));
	g_assert_false(firstrun_printable("a\033[2Jb"));
	g_assert_false(firstrun_printable("a\tb"));
	g_assert_false(firstrun_printable("a\177"));
	g_assert_false(firstrun_printable("za\xc5\xbc"));
}

/* names and addresses that are printed must be printable; so must the defaults */
static void test_unprintable_skipped(void)
{
	static const char cfg[] =
	    "chatnets = { ok = { type = \"IRC\"; }; \"b\033[2J\" = { type = \"IRC\"; };"
	    " c = { type = \"IRC\"; }; };\n"
	    "servers = ( { address = \"irc.example.net\"; chatnet = \"ok\"; },"
	    " { address = \"irc.example.org\"; chatnet = \"b\033[2J\"; },"
	    " { address = \"x\033]0;t\007\"; chatnet = \"c\"; } );\n";
	const char *const lines[] = { "", "0", "", "", NULL };
	CONFIG_REC *config = config_open(NULL, -1);
	FIRSTRUN_ANSWERS a;
	GPtrArray *nets;
	FILE *out;
	SCRIPT sc;
	char *text;

	g_assert_cmpint(config_parse_data(config, cfg, "unprintable"), ==, 0);
	nets = firstrun_networks(config);
	g_assert_cmpuint(nets->len, ==, 1);
	g_assert_cmpstr(((FIRSTRUN_NETWORK *) g_ptr_array_index(nets, 0))->chatnet, ==, "ok");

	out = tmpfile();
	g_assert_nonnull(out);
	memset(&sc, 0, sizeof(sc));
	sc.lines = lines;
	g_assert_cmpint(firstrun_ask(out, script_read, &sc, nets, (char **) THEMES, "n\033[2J",
	                             "t\033[2J", &a),
	                ==, FIRSTRUN_DONE);
	text = slurp(out);
	fclose(out);
	g_assert_null(strchr(text, '\033'));
	g_assert_nonnull(strstr(text, "Nick []: "));
	g_assert_nonnull(strstr(text, "Theme []: "));
	g_assert_null(a.nick);
	g_assert_null(a.theme);
	firstrun_answers_clear(&a);
	g_free(text);
	g_ptr_array_free(nets, TRUE);
	config_close(config);
}

static void test_read_fd_secret_erase(void)
{
	LATE_WRITE w;
	GThread *th;
	int master, slave;
	char *s;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	pty_late_write(master, "ab\177c\n", &th, &w);
	s = firstrun_read_fd(slave, 3000, TRUE);
	g_free(late_join(th, &w));
	g_assert_cmpstr(s, ==, "ac");
	g_free(s);
	assert_cooked(slave);
	close(slave);
	close(master);
}

/* a half-typed password is not left in the input queue after a timeout */
static void test_read_fd_secret_flush(void)
{
	struct termios t;
	int master, slave, fl;
	char c;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	g_assert_cmpint(write(master, "zz", 2), ==, 2);
	g_assert_null(firstrun_read_fd(slave, 50, TRUE));
	/* typed after the flush on entry: make it arrive during the read */
	{
		LATE_WRITE w;
		GThread *th;

		pty_late_write(master, "yy", &th, &w);
		g_assert_null(firstrun_read_fd(slave, 600, TRUE));
		g_free(late_join(th, &w));
	}
	assert_cooked(slave);
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	t.c_lflag &= ~(tcflag_t) ICANON;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);
	fl = fcntl(slave, F_GETFL);
	fcntl(slave, F_SETFL, fl | O_NONBLOCK);
	g_assert_cmpint(read(slave, &c, 1), <=, 0);
	close(slave);
	close(master);
}

/* A terminal that has stopped reading (^S, a stalled ssh): the prompt it was
 * sent is never read, and the secret is still read without waiting for it
 * (no TCSADRAIN or TCSAFLUSH, see tcsetattr() above; on FreeBSD the read
 * hung there). Input typed ahead of the prompt is still dropped, and on ^C
 * the terminal is still restored before the signal. */
static void test_read_fd_secret_stalled_terminal(void)
{
	static const char prompt[] = "Password: ";
	const char *const typed[] = { "ab\n", "ab\003" };
	struct sigaction old[3];
	struct termios t, raw;
	int master, slave, i;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	t.c_lflag |= ECHO | ISIG | ICANON;
	g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);
	for (i = 0; i < 2; i++) {
		struct pollfd pfd = { slave, POLLIN, 0 };
		LATE_WRITE w;
		GThread *th;
		char *s;

		/* typed ahead, and in the slave's input queue (no longer on its
		 * way through the pty) before the read starts */
		raw = t;
		raw.c_lflag &= ~(tcflag_t) (ICANON | ECHO);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		g_assert_cmpint(tcsetattr(slave, TCSANOW, &raw), ==, 0);
		g_assert_cmpint(write(master, "zz", 2), ==, 2);
		g_assert_cmpint(poll(&pfd, 1, 2000), ==, 1);
		g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);
		g_assert_cmpint(write(slave, prompt, strlen(prompt)), ==, (gssize) strlen(prompt));

		record_signals(old);
		signal_tty = slave;
		g_atomic_int_set(&draining_tcsetattr, 0);
		g_atomic_int_set(&all_tcsetattr, 0);
		late_init(&w, master, typed[i]);
		w.stalled = TRUE;
		th = g_thread_new("late-writer", late_writer, &w);
		s = firstrun_read_fd(slave, 3000, TRUE);
		g_thread_join(th);
		g_string_free(w.seen, TRUE);
		restore_signals(old);
		signal_tty = -1;
		/* switched on entry and back: fewer calls, and firstrun.c bypassed the stand-in */
		g_assert_cmpint(g_atomic_int_get(&all_tcsetattr), >=, 2);
		g_assert_cmpint(g_atomic_int_get(&draining_tcsetattr), ==, 0);
		if (i == 0) {
			g_assert_cmpint(raised, ==, 0);
			g_assert_cmpstr(s, ==, "ab");
		} else {
			g_assert_cmpint(raised, ==, SIGINT);
			g_assert_true(cooked_at_signal);
			g_assert_null(s);
		}
		g_free(s);
		assert_cooked(slave);
	}
	close(slave);
	close(master);
}

static char *pty_secret(const char *typed, gboolean *aborted_fast)
{
	LATE_WRITE w;
	GThread *th;
	int master, slave;
	char *s;
	gint64 t0 = g_get_monotonic_time();

	slave = open_pty(&master);
	if (slave < 0)
		return (char *) -1;
	pty_late_write(master, typed, &th, &w);
	s = firstrun_read_fd(slave, 4000, TRUE);
	g_free(late_join(th, &w));
	*aborted_fast = g_get_monotonic_time() - t0 < 3000000;
	assert_cooked(slave);
	close(slave);
	close(master);
	return s;
}

#define PTY_CASE(name, typed, expect)                                                              \
	static void name(void)                                                                     \
	{                                                                                          \
		gboolean fast;                                                                     \
		char *s = pty_secret(typed, &fast);                                                \
                                                                                                   \
		if (s == (char *) -1) {                                                            \
			g_test_skip("no pty available");                                           \
			return;                                                                    \
		}                                                                                  \
		g_assert_true(fast);                                                               \
		g_assert_cmpstr(s, ==, expect);                                                    \
		g_free(s);                                                                         \
	}

#define X64 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
#define OVERLONG X64 X64 X64 X64 X64 X64 X64 X64 X64 "\n"

PTY_CASE(test_secret_utf8_erase, "za\xc5\xbc\177\n", "za")
PTY_CASE(test_secret_kill, "abc\025d\n", "d")
PTY_CASE(test_secret_eof_start, "\004", NULL)
PTY_CASE(test_secret_eof_mid, "ab\004", "ab")
PTY_CASE(test_secret_ctrl_ignored, "a\027\033\001\011b\n", "ab")
PTY_CASE(test_secret_overlong_rejected, OVERLONG, NULL)
/* arrow keys and other escape sequences are dropped whole, not as "[D" */
PTY_CASE(test_secret_escape_csi, "a\033[Db\033[1;5Cc\033[200~d\n", "abcd")
PTY_CASE(test_secret_escape_ss3, "a\033OAb\033OHc\n", "abc")
PTY_CASE(test_secret_escape_meta, "a\033xb\n", "ab")
PTY_CASE(test_secret_escape_then_enter, "ab\033\n", "ab")
PTY_CASE(test_secret_escape_then_erase, "abc\033[\177\n", "ab")
PTY_CASE(test_secret_escape_utf8_after, "a\033[D\xc5\xbc\n", "a\xc5\xbc")

static void test_secret_closed_pty(void)
{
	int master, slave;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	close(master);
	g_assert_null(firstrun_read_fd(slave, 2000, TRUE));
	close(slave);
}

static gboolean flood_stop;

static gpointer flooder(gpointer data)
{
	int fd = GPOINTER_TO_INT(data);
	char chunk[256];

	memset(chunk, 'x', sizeof(chunk));
	while (!g_atomic_int_get(&flood_stop))
		if (write(fd, chunk, sizeof(chunk)) < 0)
			g_usleep(100);
	return NULL;
}

/* a stream that never sends a newline cannot hold the reader past its deadline */
static void test_read_fd_flood_deadline(void)
{
	int p[2];
	GThread *th;
	gint64 t0;
	char *s;

	g_assert_cmpint(pipe(p), ==, 0);
	g_assert_cmpint(fcntl(p[1], F_SETFL, O_NONBLOCK), ==, 0);
	g_atomic_int_set(&flood_stop, FALSE);
	th = g_thread_new("flooder", flooder, GINT_TO_POINTER(p[1]));
	t0 = g_get_monotonic_time();
	s = firstrun_read_fd(p[0], 200, FALSE);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 2000000);
	g_assert_null(s);
	g_atomic_int_set(&flood_stop, TRUE);
	g_thread_join(th);
	close(p[0]);
	close(p[1]);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/firstrun/wanted", test_wanted);
	g_test_add_func("/firstrun/networks", test_networks);
	g_test_add_func("/firstrun/ask_full", test_ask_full);
	g_test_add_func("/firstrun/ask_defaults_plaintext", test_ask_defaults_plaintext);
	g_test_add_func("/firstrun/ask_none", test_ask_none);
	g_test_add_func("/firstrun/ask_unverified_tls_no_sasl", test_ask_unverified_tls_no_sasl);
	g_test_add_func("/firstrun/ask_stored_password_wiped_on_eof",
	                test_ask_stored_password_wiped_on_eof);
	g_test_add_func("/firstrun/ask_account_control_bytes", test_ask_account_control_bytes);
	g_test_add_func("/firstrun/apply_unknown_chatnet", test_apply_unknown_chatnet);
	g_test_add_func("/firstrun/apply_only_expected_keys", test_apply_only_expected_keys);
	g_test_add_func("/firstrun/read_fd_zero_timeout_polls", test_read_fd_zero_timeout_polls);
	g_test_add_func("/firstrun/read_fd_eintr_aborts", test_read_fd_eintr_aborts);
	g_test_add_func("/firstrun/read_fd_secret_interrupt", test_read_fd_secret_interrupt);
	g_test_add_func("/firstrun/read_fd_secret_suspend", test_read_fd_secret_suspend);
	g_test_add_func("/firstrun/read_fd_abort_at_start", test_read_fd_abort_at_start);
	g_test_add_func("/firstrun/read_fd_abort_between_bytes", test_read_fd_abort_between_bytes);
	g_test_add_func("/firstrun/read_fd_abort_after_suspend", test_read_fd_abort_after_suspend);
	g_test_add_func("/firstrun/printable", test_printable);
	g_test_add_func("/firstrun/unprintable_skipped", test_unprintable_skipped);
	g_test_add_func("/firstrun/read_fd_secret_erase", test_read_fd_secret_erase);
	g_test_add_func("/firstrun/read_fd_secret_flush", test_read_fd_secret_flush);
	g_test_add_func("/firstrun/read_fd_secret_stalled_terminal",
	                test_read_fd_secret_stalled_terminal);
	g_test_add_func("/firstrun/ask_no", test_ask_no);
	g_test_add_func("/firstrun/ask_eof_skips", test_ask_eof_skips);
	g_test_add_func("/firstrun/ask_eof_at_password_wipes", test_ask_eof_at_password_wipes);
	g_test_add_func("/firstrun/ask_empty_account_skips_password",
	                test_ask_empty_account_skips_password);
	g_test_add_func("/firstrun/ask_reask_nick", test_ask_reask_nick);
	g_test_add_func("/firstrun/ask_bad_choice_3x", test_ask_bad_choice_3x);
	g_test_add_func("/firstrun/ask_bad_nick_3x", test_ask_bad_nick_3x);
	g_test_add_func("/firstrun/apply", test_apply);
	g_test_add_func("/firstrun/apply_autoconnect_first_server_only",
	                test_apply_autoconnect_first_server_only);
	g_test_add_func("/firstrun/apply_none", test_apply_none);
	g_test_add_func("/firstrun/nick_valid", test_nick_valid);
	g_test_add_func("/firstrun/read_fd_timeout", test_read_fd_timeout);
	g_test_add_func("/firstrun/read_fd_line", test_read_fd_line);
	g_test_add_func("/firstrun/read_fd_two_lines", test_read_fd_two_lines);
	g_test_add_func("/firstrun/read_fd_partial_eof", test_read_fd_partial_eof);
	g_test_add_func("/firstrun/read_fd_partial_timeout", test_read_fd_partial_timeout);
	g_test_add_func("/firstrun/read_fd_overlong", test_read_fd_overlong);
	g_test_add_func("/firstrun/read_fd_secret_pty", test_read_fd_secret_pty);
	g_test_add_func("/firstrun/secret_utf8_erase", test_secret_utf8_erase);
	g_test_add_func("/firstrun/secret_kill", test_secret_kill);
	g_test_add_func("/firstrun/secret_eof_start", test_secret_eof_start);
	g_test_add_func("/firstrun/secret_eof_mid", test_secret_eof_mid);
	g_test_add_func("/firstrun/secret_ctrl_ignored", test_secret_ctrl_ignored);
	g_test_add_func("/firstrun/secret_overlong_rejected", test_secret_overlong_rejected);
	g_test_add_func("/firstrun/secret_escape_csi", test_secret_escape_csi);
	g_test_add_func("/firstrun/secret_escape_ss3", test_secret_escape_ss3);
	g_test_add_func("/firstrun/secret_escape_meta", test_secret_escape_meta);
	g_test_add_func("/firstrun/secret_escape_then_enter", test_secret_escape_then_enter);
	g_test_add_func("/firstrun/secret_escape_then_erase", test_secret_escape_then_erase);
	g_test_add_func("/firstrun/secret_escape_utf8_after", test_secret_escape_utf8_after);
	g_test_add_func("/firstrun/secret_closed_pty", test_secret_closed_pty);
	g_test_add_func("/firstrun/read_fd_flood_deadline", test_read_fd_flood_deadline);
	g_test_add_func("/firstrun/clear_idempotent", test_clear_idempotent);

	return g_test_run();
}
