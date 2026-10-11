/*
 * Tests for fe-settings.c: a setting holding a secret is not shown by /SET
 * or offered by tab completion.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/fe-settings.h>
#include <irssi/src/fe-common/core/printtext.h>

#include <string.h>
#include "../../test-home.h"

#define MODULE_NAME "tests"
#define MASK "(secret, set - not shown)"

static GString *printed;

static void sig_print_text(TEXT_DEST_REC *dest, const char *text, const char *stripped)
{
	g_string_append(printed, stripped);
	g_string_append_c(printed, '\n');
}

/* the core is brought up once: it is not meant to be re-initialised */
static void init_once(void)
{
	args_execute(0, NULL);
	core_init();
	fe_common_core_init();
	signal_emit("irssi init finished", 0);

	settings_add_str("tests", "fe_web_password", "s3cret");
	settings_add_str("tests", "bot_token", "TOKEN-1");
	settings_add_str("tests", "my_api_key", "APIKEY-1");
	settings_add_str("tests", "histsearch_key", "^R");
	settings_add_str("tests", "empty_password", "");
	settings_add_str("tests", "translate_deepl_key", "DEEPL-1");

	printed = g_string_new("");
	signal_add("print text", (SIGNAL_FUNC) sig_print_text);
}

static void set_up(void)
{
	settings_set_str("fe_web_password", "s3cret");
	settings_set_str("bot_token", "TOKEN-1");
	settings_set_str("my_api_key", "APIKEY-1");
	settings_set_str("histsearch_key", "^R");
	settings_set_str("empty_password", "");
	settings_set_str("translate_deepl_key", "DEEPL-1");
	/* what is hidden by name is the user's config, not the C default */
	settings_set_str("hidden_settings", "");
}

static void tear_down(void)
{
}

static void run_set(const char *cmd)
{
	g_string_truncate(printed, 0);
	signal_emit("send command", 3, cmd, NULL, NULL);
}

/* the printed line of a setting: it starts with the key (the theme decides
 * the separator), so another line that only mentions the key does not count */
static char *line_of(const char *key)
{
	char **lines = g_strsplit(printed->str, "\n", -1);
	char *found = NULL;
	size_t len = strlen(key);
	int i;

	for (i = 0; lines[i] != NULL && found == NULL; i++) {
		const char *l = lines[i];

		while (*l == ' ')
			l++;
		if (strncmp(l, key, len) == 0 && (l[len] == ' ' || l[len] == '=' || l[len] == '\0'))
			found = g_strdup(l);
	}
	g_strfreev(lines);
	g_assert_nonnull(found);
	return found;
}

static void test_words(void)
{
	g_assert_true(fe_settings_is_secret("fe_web_password"));
	g_assert_true(fe_settings_is_secret("sasl_passwd"));
	g_assert_true(fe_settings_is_secret("my_secret_x"));
	g_assert_true(fe_settings_is_secret("bot_token"));
	g_assert_true(fe_settings_is_secret("my_api_key"));
	g_assert_false(fe_settings_is_secret("api_keys"));
	g_assert_false(fe_settings_is_secret("histsearch_key"));
	g_assert_false(fe_settings_is_secret("nick"));
	g_assert_false(fe_settings_is_secret(NULL));
}

static void test_hidden_settings(void)
{
	settings_set_str("hidden_settings", "translate_deepl_key, Other_Key");
	g_assert_true(fe_settings_is_secret("translate_deepl_key"));
	g_assert_true(fe_settings_is_secret("other_key"));
	g_assert_false(fe_settings_is_secret("deepl"));
}

static void test_set_masks_value(void)
{
	char *line;

	run_set("/set fe_web_password");
	line = line_of("fe_web_password");
	g_assert_nonnull(strstr(line, MASK));
	g_assert_null(strstr(printed->str, "s3cret"));
	g_free(line);
}

static void test_set_new_value_masked(void)
{
	char *line;

	run_set("/set bot_token NEWTOKEN");
	line = line_of("bot_token");
	g_assert_nonnull(strstr(line, MASK));
	g_assert_null(strstr(printed->str, "NEWTOKEN"));
	g_assert_cmpstr(settings_get_str("bot_token"), ==, "NEWTOKEN");
	g_free(line);
}

static void test_set_empty_secret_shown_empty(void)
{
	char *line;

	run_set("/set empty_password");
	line = line_of("empty_password");
	g_assert_null(strstr(line, "not shown"));
	/* what follows the key is an empty value: nothing but separators */
	g_assert_cmpstr(g_strstrip(g_strdelimit(line + strlen("empty_password"), "=", ' ')), ==, "");
	g_free(line);
}

static void test_set_pattern_masks(void)
{
	char *line;

	run_set("/set token");
	line = line_of("bot_token");
	g_assert_nonnull(strstr(line, MASK));
	g_assert_null(strstr(printed->str, "TOKEN-1"));
	g_free(line);
}

static void test_set_plain_visible(void)
{
	char *line;

	run_set("/set histsearch_key");
	line = line_of("histsearch_key");
	g_assert_nonnull(strstr(line, "^R"));
	g_assert_null(strstr(line, "not shown"));
	g_free(line);
}

static void test_complete_secret_value_not_offered(void)
{
	GList *list = NULL;
	int want_space = 0;

	signal_emit("complete command set", 5, &list, NULL, "", "bot_token", &want_space);
	g_assert_null(list);

	signal_emit("complete command set", 5, &list, NULL, "", "histsearch_key", &want_space);
	g_assert_nonnull(list);
	g_assert_cmpstr(list->data, ==, "^R");
	g_list_free_full(list, g_free);
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

	add("/fe-settings/is-secret/words", test_words);
	add("/fe-settings/is-secret/hidden-settings", test_hidden_settings);
	add("/fe-settings/set/masks-value", test_set_masks_value);
	add("/fe-settings/set/new-value-masked", test_set_new_value_masked);
	add("/fe-settings/set/empty-secret-shown-empty", test_set_empty_secret_shown_empty);
	add("/fe-settings/set/pattern-masks", test_set_pattern_masks);
	add("/fe-settings/set/plain-visible", test_set_plain_visible);
	add("/fe-settings/complete/secret-value-not-offered", test_complete_secret_value_not_offered);

	test_home_setup();

	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	init_once();

	return g_test_run();
}
