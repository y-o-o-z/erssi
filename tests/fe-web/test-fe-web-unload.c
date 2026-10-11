/*
 test-fe-web-unload.c : /UNLOAD of fe-web sent from a web client

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A web client's command runs while fe-web handles that client's input:
 /UNLOAD of fe-web would deinitialise fe-web (and free the client) and
 close its code under the running frame loop. fe-web refuses it while a
 web client's command runs, however the command got there (an alias,
 /EVAL, an abbreviation, upper case); typed in erssi it still runs.

 The harness's module "web" (file "fe", fe-web) is static, so cmd_unload()
 leaves it alone: these tests see whether the command reaches the
 handlers of cmd_unload()'s priority (a probe there), not an unload.
*/

/* for signal_add() */
#define MODULE_NAME "test-fe-web-unload"

#include "fe-web-test.h"

#include <irssi/src/core/commands.h>
#include <irssi/src/core/modules.h>
#include <irssi/src/core/modules-load.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/settings.h>

#include <string.h>

#define REFUSED "/UNLOAD of fe-web from a web client is refused"

/* the brief's text, in full */
#define REFUSED_FULL                                                                     \
	"fe-web: /UNLOAD of fe-web from a web client is refused (it would remove fe-web " \
	"while it runs the command) - run it in erssi itself, or /SET fe_web_enabled OFF"

static gboolean reached;
static char *reached_data;

static void probe(const char *data)
{
	reached = TRUE;
	g_free(reached_data);
	reached_data = g_strdup(data);
}

static void reset(void)
{
	reached = FALSE;
	g_free(reached_data);
	reached_data = NULL;
	g_free(fe_web_fuzz_take_printed());
}

/* Run 'command' as web client c sends it */
static void from_web(WEB_CLIENT_REC *c, const char *command)
{
	reset();
	fe_web_client_execute_command(c, command, NULL);
}

/* Run 'command' as typed in erssi */
static void from_terminal(const char *command)
{
	reset();
	signal_emit("send command", 3, command, NULL, NULL);
}

/* The refusal was printed, the command did not reach cmd_unload() and
 * fe-web is still registered */
static void assert_refused(void)
{
	char *printed = fe_web_fuzz_take_printed();

	g_assert_false(reached);
	g_assert_nonnull(strstr(printed, REFUSED));
	g_assert_nonnull(module_find("web"));
	g_free(printed);
}

/* The command reached cmd_unload() with 'data', and nothing was refused */
static void assert_reached(const char *data)
{
	char *printed = fe_web_fuzz_take_printed();

	g_assert_true(reached);
	g_assert_cmpstr(reached_data, ==, data);
	g_assert_null(strstr(printed, REFUSED));
	g_free(printed);
}

static void test_web_client_refused(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	char *printed;

	from_web(c, "/unload web");
	g_assert_false(reached);
	printed = fe_web_fuzz_take_printed();
	g_assert_nonnull(strstr(printed, REFUSED_FULL));
	g_free(printed);
	g_assert_nonnull(module_find("web"));
	g_assert_true(fe_web_fuzz_client_alive(c));
	fe_web_fuzz_client_free(c);
}

/* the whole way: a masked JSON command frame, through the frame loop of
 * fe-web-server.c and fe_web_client_handle_message() */
static void test_frame_refused(void)
{
	static const char json[] = "{\"type\":\"command\",\"command\":\"/unload web\"}";
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	GByteArray *f = test_frame(WS_OPCODE_TEXT, json, strlen(json), TRUE);

	reset();
	fe_web_fuzz_client_send(c, f->data, f->len);
	assert_refused();
	g_assert_true(fe_web_fuzz_client_alive(c));

	g_byte_array_unref(f);
	fe_web_fuzz_client_free(c);
}

static void test_submodule_refused(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	from_web(c, "/unload web fe");
	assert_refused();
	fe_web_fuzz_client_free(c);
}

/* module names are found without regard to case, so is the command */
static void test_case_insensitive(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	from_web(c, "/unload WEB");
	assert_refused();
	from_web(c, "/UNLOAD Web");
	assert_refused();
	from_web(c, "/UnLoAd   wEb   ");
	assert_refused();
	fe_web_fuzz_client_free(c);
}

static void test_alias_refused(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	from_terminal("/alias u unload web");
	from_web(c, "/u");
	assert_refused();
	/* an alias the web client made itself */
	from_web(c, "/alias u2 unload $0");
	from_web(c, "/u2 web");
	assert_refused();
	from_terminal("/unalias u");
	from_terminal("/unalias u2");
	fe_web_fuzz_client_free(c);
}

static void cmd_emit_unload(void)
{
	signal_emit("command unload", 3, "web", NULL, NULL);
}

/* the other ways to the same command: /EVAL, an abbreviation, // (no
 * aliases), ^ (no output), another command character */
static void test_indirect_refused(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	from_web(c, "/eval unload web");
	assert_refused();
	from_web(c, "/eval echo x; unload web");
	assert_refused();
	from_web(c, "/unl web");
	assert_refused();
	from_web(c, "//unload web");
	assert_refused();
	from_web(c, "/^unload web");
	g_assert_false(reached);

	settings_set_str("cmdchars", "/!");
	from_web(c, "!unload web");
	assert_refused();
	settings_set_str("cmdchars", "/");

	/* a script that a web client's command runs, and that emits
	 * "command unload" itself (Irssi::signal_emit) */
	command_bind("test_emit_unload", NULL, (SIGNAL_FUNC) cmd_emit_unload);
	from_web(c, "/test_emit_unload");
	assert_refused();
	/* the same outside a web client's command */
	reset();
	signal_emit("command unload", 3, "web", NULL, NULL);
	assert_reached("web");
	command_unbind("test_emit_unload", (SIGNAL_FUNC) cmd_emit_unload);
	fe_web_fuzz_client_free(c);
}

static void test_other_module_allowed(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	from_web(c, "/unload perl");
	assert_reached("perl");
	/* not a module name (fe-web is "web"): cmd_unload() says it is not
	 * loaded */
	from_web(c, "/unload fe_web");
	assert_reached("fe_web");
	fe_web_fuzz_client_free(c);
}

/* a module "web" with a file that is not fe-web: only the files that
 * would be unloaded count */
static void test_other_submodule_allowed(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);
	MODULE_FILE_REC *other = module_register_full("web", "core", "web-core");

	from_web(c, "/unload web core");
	assert_reached("web core");
	from_web(c, "/unload web nosuch");
	assert_reached("web nosuch");
	from_web(c, "/unload web");
	assert_refused();
	from_web(c, "/unload web fe");
	assert_refused();

	/* the file is static: module_file_unload() only forgets it */
	module_file_unload(other);
	g_assert_nonnull(module_find("web"));

	/* a module none of whose files is fe-web */
	other = module_register_full("notweb", "core", "notweb-core");
	from_web(c, "/unload notweb");
	assert_reached("notweb");
	module_file_unload(other);
	g_assert_null(module_find("notweb"));
	fe_web_fuzz_client_free(c);
}

static void test_terminal_allowed(void)
{
	from_terminal("/unload web");
	assert_reached("web");
	from_terminal("/unl WEB fe");
	assert_reached("WEB fe");
}

/* after a web client's command, the terminal's own runs again */
static void test_depth_restored(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	from_web(c, "/unload web");
	assert_refused();
	from_web(c, "/echo hello");
	from_terminal("/unload web");
	assert_reached("web");
	fe_web_fuzz_client_free(c);
}

/* the guard goes with fe-web's deinit */
static void test_deinit_unbinds(void)
{
	WEB_CLIENT_REC *c = fe_web_fuzz_client_new_peer("p", TRUE, FALSE);

	fe_web_client_commands_deinit();
	from_web(c, "/unload web");
	assert_reached("web");
	fe_web_client_commands_init();
	from_web(c, "/unload web");
	assert_refused();
	fe_web_fuzz_client_free(c);
}

int main(int argc, char **argv)
{
	int ret;

	test_init(&argc, &argv);
	fe_web_fuzz_allow_commands(TRUE);
	fe_web_client_commands_init();
	/* at the priority of cmd_unload() and bound after fe-web's guard, as a
	 * script's handler would be: newer handlers of one priority run first,
	 * so only a guard of a higher priority runs before it */
	signal_add("command unload", (SIGNAL_FUNC) probe);

	g_test_add_func("/fe-web/unload/web-client-refused", test_web_client_refused);
	g_test_add_func("/fe-web/unload/frame-refused", test_frame_refused);
	g_test_add_func("/fe-web/unload/submodule-refused", test_submodule_refused);
	g_test_add_func("/fe-web/unload/case-insensitive", test_case_insensitive);
	g_test_add_func("/fe-web/unload/alias-refused", test_alias_refused);
	g_test_add_func("/fe-web/unload/indirect-refused", test_indirect_refused);
	g_test_add_func("/fe-web/unload/other-module-allowed", test_other_module_allowed);
	g_test_add_func("/fe-web/unload/other-submodule-allowed",
	                test_other_submodule_allowed);
	g_test_add_func("/fe-web/unload/terminal-allowed", test_terminal_allowed);
	g_test_add_func("/fe-web/unload/depth-restored", test_depth_restored);
	g_test_add_func("/fe-web/unload/deinit-unbinds", test_deinit_unbinds);

	ret = g_test_run();

	signal_remove("command unload", (SIGNAL_FUNC) probe);
	fe_web_client_commands_deinit();
	fe_web_fuzz_allow_commands(FALSE);
	g_free(reached_data);
	return ret;
}
