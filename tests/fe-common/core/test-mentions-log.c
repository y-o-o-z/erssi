/*
 * Tests for the mentions log helpers (mentions-log.c).
 */
#include <irssi/src/common.h>
#include <irssi/src/fe-common/core/mentions-log.h>

#include <sys/stat.h>
#include <time.h>
#include <glib/gstdio.h>

static time_t make_t0(void)
{
	struct tm tm = {0};

	tm.tm_year = 2026 - 1900;
	tm.tm_mon = 10 - 1;
	tm.tm_mday = 10;
	tm.tm_hour = 12;
	tm.tm_min = 34;
	tm.tm_sec = 56;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static void test_line_format(void)
{
	char *line = mentions_log_line(make_t0(), "E2E", "#test", "bob", "hello, is alice here?");

	g_assert_cmpstr(line, ==, "2026-10-10 12:34:56 [E2E] #test <bob> hello, is alice here?\n");
	g_free(line);
}

static void test_line_utf8_bytes(void)
{
	const char *text = "alice, za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 g\xc4\x99\xc5\x9bl\xc4\x85 "
	                   "ja\xc5\xba\xc5\x84";
	char *line = mentions_log_line(make_t0(), "E2E", "#test", "bob", text);

	g_assert_true(g_str_has_suffix(line, "<bob> alice, za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 "
	                                     "g\xc4\x99\xc5\x9bl\xc4\x85 ja\xc5\xba\xc5\x84\n"));
	g_free(line);
}

static void test_line_nulls(void)
{
	char *line = mentions_log_line(make_t0(), NULL, NULL, NULL, NULL);

	g_assert_cmpstr(line, ==, "2026-10-10 12:34:56 [-] - <?> \n");
	g_free(line);
}

static void test_line_newlines(void)
{
	char *line = mentions_log_line(make_t0(), "E2E", "#test", "n", "  a\r\nb\t ");

	g_assert_true(g_str_has_suffix(line, "<n> a  b\n"));
	g_free(line);
}

static void test_append_creates(void)
{
	GError *error = NULL;
	char *tmp = g_dir_make_tmp("mentions-XXXXXX", &error);
	char *path, *dir, *content = NULL;
	struct stat st;

	g_assert_no_error(error);
	dir = g_build_filename(tmp, "new", "sub", NULL);
	path = g_build_filename(dir, "mentions.log", NULL);

	g_assert_true(mentions_log_append(path, "line one\n", &error));
	g_assert_no_error(error);
	g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "line one\n");
	g_assert_cmpint(stat(dir, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0700);
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);

	g_free(content);
	g_remove(path);
	g_rmdir(dir);
	g_free(dir);
	dir = g_build_filename(tmp, "new", NULL);
	g_rmdir(dir);
	g_free(dir);
	g_rmdir(tmp);
	g_free(path);
	g_free(tmp);
}

static void test_append_tightens_mode(void)
{
	GError *error = NULL;
	char *tmp = g_dir_make_tmp("mentions-XXXXXX", &error);
	char *path, *content = NULL;
	struct stat st;

	g_assert_no_error(error);
	path = g_build_filename(tmp, "mentions.log", NULL);
	g_assert_true(g_file_set_contents(path, "old\n", -1, NULL));
	g_assert_cmpint(chmod(path, 0644), ==, 0);

	g_assert_true(mentions_log_append(path, "new\n", &error));
	g_assert_no_error(error);
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
	g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "old\nnew\n");

	g_free(content);
	g_remove(path);
	g_rmdir(tmp);
	g_free(path);
	g_free(tmp);
}

static void test_append_to_directory_fails(void)
{
	GError *error = NULL;
	char *tmp = g_dir_make_tmp("mentions-XXXXXX", &error);

	g_assert_no_error(error);
	g_assert_false(mentions_log_append(tmp, "x\n", &error));
	g_assert_nonnull(error);

	g_clear_error(&error);
	g_rmdir(tmp);
	g_free(tmp);
}

static void test_path(void)
{
	char *p, *expected;

	p = mentions_log_path("", "/h/.erssi");
	g_assert_cmpstr(p, ==, "/h/.erssi/logs/mentions.log");
	g_free(p);

	p = mentions_log_path("~/m.log", "/x");
	expected = g_build_filename(g_get_home_dir(), "m.log", NULL);
	g_assert_cmpstr(p, ==, expected);
	g_free(p);
	g_free(expected);

	p = mentions_log_path("/abs/m.log", "/x");
	g_assert_cmpstr(p, ==, "/abs/m.log");
	g_free(p);
}

static void test_network_excluded(void)
{
	g_assert_true(mentions_network_excluded("Botnet", "IRCnetBot Botnet,PT_botnet"));
	g_assert_true(mentions_network_excluded("botnet", "BOTNET"));
	g_assert_false(mentions_network_excluded("IRCnet", "Botnet"));
	g_assert_false(mentions_network_excluded(NULL, "x"));
	g_assert_false(mentions_network_excluded("x", ""));
}

static void test_server_nick(void)
{
	g_assert_true(mentions_is_server_nick("irc.example.net"));
	g_assert_true(mentions_is_server_nick(""));
	g_assert_true(mentions_is_server_nick(NULL));
	g_assert_false(mentions_is_server_nick("NickServ"));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mentions_log/line_format", test_line_format);
	g_test_add_func("/mentions_log/line_utf8_bytes", test_line_utf8_bytes);
	g_test_add_func("/mentions_log/line_nulls", test_line_nulls);
	g_test_add_func("/mentions_log/line_newlines", test_line_newlines);
	g_test_add_func("/mentions_log/append_creates_0700_0600", test_append_creates);
	g_test_add_func("/mentions_log/append_tightens_mode", test_append_tightens_mode);
	g_test_add_func("/mentions_log/append_to_directory_fails", test_append_to_directory_fails);
	g_test_add_func("/mentions_log/path", test_path);
	g_test_add_func("/mentions_log/network_excluded", test_network_excluded);
	g_test_add_func("/mentions_log/server_nick", test_server_nick);

	return g_test_run();
}
