/*
 * Tests for firstrun.c: the first-run setup questions, the answers applied
 * to the config, nick validation and the poll() based fd reader.
 */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <irssi/src/common.h>
#include <glib/gstdio.h>
#include <irssi/src/lib-config/iconfig.h>
#include <irssi/src/fe-common/core/firstrun.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static const char SAMPLE[] =
    "chatnets = {\n"
    "  IRCnet = { type = \"IRC\"; };\n"
    "  liberachat = { type = \"IRC\"; };\n"
    "  SILC = { type = \"SILC\"; };\n"
    "  Empty = { type = \"IRC\"; };\n"
    "};\n"
    "servers = (\n"
    "  { address = \"irc.atw-inter.net\"; chatnet = \"IRCnet\"; port = \"6667\"; },\n"
    "  { address = \"irc.example.net\"; chatnet = \"IRCnet\"; port = \"6668\"; },\n"
    "  { address = \"irc.libera.chat\"; chatnet = \"liberachat\"; port = \"6697\";"
    " use_tls = \"yes\"; },\n"
    "  { address = \"silc.example.org\"; chatnet = \"SILC\"; port = \"706\"; }\n"
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

	g_assert_cmpuint(nets->len, ==, 2);
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

static void test_nick_valid(void)
{
	char long31[32], long30[31];

	memset(long31, 'a', 31);
	long31[31] = '\0';
	memset(long30, 'a', 30);
	long30[30] = '\0';
	g_assert_true(firstrun_nick_valid("yooz"));
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

typedef struct {
	int fd;
	const char *text;
} LATE_WRITE;

/* types the answer after ECHO has been turned off by the reader */
static gpointer late_writer(gpointer data)
{
	LATE_WRITE *w = data;

	g_usleep(200 * 1000);
	if (write(w->fd, w->text, strlen(w->text)) < 0)
		return GINT_TO_POINTER(1);
	return NULL;
}

static void test_read_fd_secret_pty(void)
{
	LATE_WRITE w;
	GThread *th;
	int master, slave;
	struct termios t;
	char *s, echo[64];
	ssize_t n;

	slave = open_pty(&master);
	if (slave < 0) {
		g_test_skip("no pty available");
		return;
	}
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	t.c_lflag |= ECHO;
	g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);

	w.fd = master;
	w.text = "hunter2\n";
	th = g_thread_new("late-writer", late_writer, &w);
	s = firstrun_read_fd(slave, 3000, TRUE);
	g_thread_join(th);
	g_assert_cmpstr(s, ==, "hunter2");
	g_free(s);
	g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
	g_assert_true(t.c_lflag & ECHO);

	/* nothing typed from the master side may have been echoed back */
	g_assert_cmpint(fcntl(master, F_SETFL, O_NONBLOCK), ==, 0);
	n = read(master, echo, sizeof(echo) - 1);
	if (n > 0) {
		echo[n] = '\0';
		g_assert_null(strstr(echo, "hunter2"));
	}

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

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/firstrun/wanted", test_wanted);
	g_test_add_func("/firstrun/networks", test_networks);
	g_test_add_func("/firstrun/ask_full", test_ask_full);
	g_test_add_func("/firstrun/ask_defaults_plaintext", test_ask_defaults_plaintext);
	g_test_add_func("/firstrun/ask_none", test_ask_none);
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
	g_test_add_func("/firstrun/clear_idempotent", test_clear_idempotent);

	return g_test_run();
}
