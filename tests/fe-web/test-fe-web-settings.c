/*
 test-fe-web-settings.c : the fe_web_ settings start and stop the server

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 fe-web.c (re)starts the server when an fe_web_ setting changes. When the
 server cannot start (here: the directory of fe_web_socket is writable by
 others), the user fixes the cause and runs /SET fe_web_enabled ON again:
 that retries, although the value did not change. "stopped" is printed
 only for a server that was listening.
*/

#include "fe-web-test.h"

#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REFUSED "is writable by others (mode 0770)"
#define LISTENING "fe-web: WebSocket server listening on unix:"
#define STOPPED "fe-web: WebSocket server stopped"

static char *B, *dir, *path;

/* A socket path in a directory with mode 0770, where the server does not
 * start */
static void setup(void)
{
	B = test_tmpdir();
	dir = g_build_filename(B, "sock", NULL);
	path = g_build_filename(dir, "fe-web.sock", NULL);
	g_assert_cmpint(mkdir(dir, 0700), ==, 0);
	g_assert_cmpint(chmod(dir, 0770), ==, 0);
	settings_set_str("fe_web_socket", path);
	g_free(fe_web_fuzz_take_printed());
}

static void teardown(void)
{
	settings_set_bool("fe_web_enabled", FALSE);
	signal_emit("setup changed", 0);
	fe_web_fuzz_iterate(20);
	settings_set_str("fe_web_socket", "");
	g_free(fe_web_fuzz_take_printed());
	test_rmtree(B);
	g_free(B);
	g_free(dir);
	g_free(path);
}

/* Run "/<command>" as typed and let the deferred restart run; returns what
 * was printed (to be freed) */
static char *run(const char *command, const char *args)
{
	char *signal = g_strconcat("command ", command, NULL);

	signal_emit(signal, 3, args, NULL, NULL);
	g_free(signal);
	fe_web_fuzz_iterate(20);
	return fe_web_fuzz_take_printed();
}

static void assert_printed(const char *out, const char *what)
{
	if (strstr(out, what) == NULL)
		g_error("expected \"%s\" in the output:\n%s", what, out);
}

static void assert_not_printed(const char *out, const char *what)
{
	if (strstr(out, what) != NULL)
		g_error("did not expect \"%s\" in the output:\n%s", what, out);
}

static gboolean is_socket(const char *p)
{
	struct stat st;

	return lstat(p, &st) == 0 && S_ISSOCK(st.st_mode);
}

static void test_same_value_set_retries(void)
{
	char *out;

	setup();
	out = run("set", "fe_web_enabled ON");
	assert_printed(out, REFUSED);
	g_assert_false(is_socket(path));
	g_free(out);

	/* the cause fixed, the same value again */
	g_assert_cmpint(chmod(dir, 0700), ==, 0);
	out = run("set", "fe_web_enabled ON");
	assert_printed(out, LISTENING);
	g_assert_true(is_socket(path));
	g_free(out);

	/* while it runs, the same value changes nothing */
	out = run("set", "fe_web_enabled ON");
	assert_not_printed(out, STOPPED);
	assert_not_printed(out, LISTENING);
	g_free(out);

	out = run("set", "fe_web_enabled OFF");
	assert_printed(out, STOPPED);
	g_assert_false(is_socket(path));
	g_free(out);
	teardown();
}

/* /TOGGLE and the other fe_web_ settings retry as well, with options
 * before the name */
static void test_toggle_and_other_settings_retry(void)
{
	char *out, *args;

	setup();
	out = run("set", "fe_web_enabled ON");
	assert_printed(out, REFUSED);
	g_free(out);
	out = run("toggle", "fe_web_enabled ON");
	assert_printed(out, REFUSED);
	g_free(out);
	args = g_strconcat("fe_web_socket ", path, NULL);
	out = run("set", args);
	assert_printed(out, REFUSED);
	g_free(out);
	g_free(args);
	out = run("set", "-default fe_web_port");
	assert_printed(out, REFUSED);
	g_free(out);

	g_assert_cmpint(chmod(dir, 0700), ==, 0);
	out = run("toggle", "fe_web_enabled ON");
	assert_printed(out, LISTENING);
	g_free(out);
	teardown();
}

/* An unrelated /SET does not retry (it would print the error again at
 * every /SET of anything) */
static void test_other_set_does_not_retry(void)
{
	char *out;

	setup();
	out = run("set", "fe_web_enabled ON");
	assert_printed(out, REFUSED);
	g_free(out);
	out = run("set", "beep_msg_level HILIGHT");
	assert_not_printed(out, REFUSED);
	assert_not_printed(out, "fe-web:");
	g_free(out);
	teardown();
}

/* No "stopped" for a server that never started */
static void test_no_stopped_without_start(void)
{
	char *out;

	setup();
	out = run("set", "fe_web_enabled ON");
	assert_printed(out, REFUSED);
	g_free(out);
	out = run("set", "fe_web_enabled OFF");
	assert_not_printed(out, STOPPED);
	g_free(out);
	teardown();
}

int main(int argc, char **argv)
{
	int ret;

	test_init(&argc, &argv);
	/* fe_web_init() registers the settings the harness registered
	 * already (their values stay in the config) */
	settings_remove("fe_web_enabled");
	settings_remove("fe_web_port");
	settings_remove("fe_web_bind");
	settings_remove("fe_web_password");
	settings_remove("fe_web_socket");
	fe_web_init();
	g_assert_cmpstr(settings_get_str("fe_web_password"), ==, FE_WEB_FUZZ_PASSWORD);

	g_test_add_func("/fe-web/settings/same-value-set-retries", test_same_value_set_retries);
	g_test_add_func("/fe-web/settings/toggle-and-other-settings-retry",
	                test_toggle_and_other_settings_retry);
	g_test_add_func("/fe-web/settings/other-set-does-not-retry", test_other_set_does_not_retry);
	g_test_add_func("/fe-web/settings/no-stopped-without-start",
	                test_no_stopped_without_start);

	ret = g_test_run();
	fe_web_deinit();
	return ret;
}
