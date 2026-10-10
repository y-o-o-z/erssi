/*
 * Tests for shellter-defaults.c: a config made before 2026.10.0 gets the
 * Shellter Edition values of the former C defaults once, only for the keys
 * it lacks.
 */
#include <irssi/src/common.h>
#include <irssi/src/lib-config/iconfig.h>
#include <irssi/src/fe-common/core/shellter-defaults.h>

typedef struct {
	const char *module;
	const char *key;
	const char *value;
} DEFAULT_ROW;

static const DEFAULT_ROW UPGRADED[] = {
	{ "fe-ansi", "colors_ansi_24bit", "yes" },
	{ "fe-common/irc", "print_whois_rpl_in_server_window", "yes" },
	{ "fe-common/core", "theme", "shellter" },
	{ "fe-common/core", "show_names_on_join_limit", "1" },
	{ "fe-common/core", "show_names_on_join_alone", "no" },
	{ "fe-common/core", "nick_column_width", "13" },
	{ "fe-common/core", "nick_hash_colors",
	  "#f59e0b #fcd34d #fdba74 #79c0ff #a5d6ff #d2a8ff #3fb950 #7ee787" },
	{ "fe-common/core", "nick_mode_color_owner", "%ZFF7B72%_" },
	{ "fe-common/core", "nick_mode_color_op", "%ZF59E0B%_" },
	{ "fe-common/core", "nick_mode_color_halfop", "%Z3FB950%_" },
	{ "fe-common/core", "nick_mode_color_voice", "%Z79C0FF%_" },
	{ "fe-common/core", "hilight_color", "%ZFCD34D" },
	{ "fe-common/core", "hilight_act_color", "%ZFF7B72" },
	{ "fe-common/core", "hidden_settings", "translate_deepl_key" },
};

#define UPGRADED_COUNT ((int) G_N_ELEMENTS(UPGRADED))

static CONFIG_NODE *cfg_section(CONFIG_REC *c, const char *module)
{
	CONFIG_NODE *node;

	node = config_node_find(c->mainnode, "settings");
	if (node == NULL || node->type != NODE_TYPE_BLOCK)
		return NULL;
	node = config_node_find(node, module);
	return node == NULL || node->type != NODE_TYPE_BLOCK ? NULL : node;
}

static const char *cfg_get(CONFIG_REC *c, const char *module, const char *key)
{
	CONFIG_NODE *node;

	node = cfg_section(c, module);
	return node == NULL ? NULL : config_node_get_str(node, key, NULL);
}

static CONFIG_REC *cfg_parse(const char *text)
{
	CONFIG_REC *c;

	c = config_open(NULL, -1);
	g_assert_nonnull(c);
	g_assert_cmpint(config_parse_data(c, text, "test"), ==, 0);
	g_assert_null(config_last_error(c));
	return c;
}

static void test_old_config_gets_values(void)
{
	CONFIG_REC *c;
	int i;

	c = cfg_parse("settings = { core = { nick = \"alice\"; }; };");
	g_assert_cmpint(shellter_defaults_migrate(c), ==, UPGRADED_COUNT);
	g_assert_cmpint(UPGRADED_COUNT, ==, 14);
	for (i = 0; i < UPGRADED_COUNT; i++)
		g_assert_cmpstr(cfg_get(c, UPGRADED[i].module, UPGRADED[i].key), ==,
		                UPGRADED[i].value);
	g_assert_cmpint(config_get_int(c, "erssi", "defaults_version", 0), ==, 1);
	g_assert_cmpstr(cfg_get(c, "core", "nick"), ==, "alice");
	config_close(c);
}

static void test_user_values_kept(void)
{
	CONFIG_REC *c;

	c = cfg_parse("settings = {\n"
	              "  \"fe-common/core\" = { theme = \"default\"; nick_column_width = \"10\"; };\n"
	              "  \"fe-ansi\" = { colors_ansi_24bit = \"no\"; };\n"
	              "};");
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 11);
	g_assert_cmpstr(cfg_get(c, "fe-common/core", "theme"), ==, "default");
	g_assert_cmpstr(cfg_get(c, "fe-common/core", "nick_column_width"), ==, "10");
	g_assert_cmpstr(cfg_get(c, "fe-ansi", "colors_ansi_24bit"), ==, "no");
	g_assert_cmpstr(cfg_get(c, "fe-common/core", "hilight_color"), ==, "%ZFCD34D");
	g_assert_cmpint(config_get_int(c, "erssi", "defaults_version", 0), ==, 1);
	config_close(c);
}

static void test_legacy_fe_text_section(void)
{
	CONFIG_REC *c;

	c = cfg_parse("settings = { \"fe-text\" = { colors_ansi_24bit = \"no\"; }; };");
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 14);
	g_assert_cmpstr(cfg_get(c, "fe-ansi", "colors_ansi_24bit"), ==, "yes");
	g_assert_cmpstr(cfg_get(c, "fe-text", "colors_ansi_24bit"), ==, "no");
	config_close(c);
}

static void test_runs_once(void)
{
	CONFIG_REC *c;
	CONFIG_NODE *section;
	int counter;

	c = cfg_parse("settings = { core = { nick = \"alice\"; }; };");
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 14);

	section = cfg_section(c, "fe-common/core");
	g_assert_nonnull(section);
	config_node_set_str(c, section, "theme", NULL);
	g_assert_null(cfg_get(c, "fe-common/core", "theme"));
	counter = c->modifycounter;

	g_assert_cmpint(shellter_defaults_migrate(c), ==, 0);
	g_assert_null(cfg_get(c, "fe-common/core", "theme"));
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);
}

static void test_newer_marker(void)
{
	CONFIG_REC *c;
	int counter;

	c = cfg_parse("erssi = { defaults_version = \"2\"; };");
	counter = c->modifycounter;
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 0);
	g_assert_null(cfg_get(c, "fe-common/core", "theme"));
	g_assert_cmpint(config_get_int(c, "erssi", "defaults_version", 0), ==, 2);
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);
}

static void test_empty_config(void)
{
	CONFIG_REC *c;

	c = config_open(NULL, -1);
	g_assert_nonnull(c);
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 14);
	g_assert_cmpint(config_get_int(c, "erssi", "defaults_version", 0), ==, 1);
	config_close(c);
}

/* A section of an unexpected shape is left alone, and no g_critical (fatal
   in tests and while erssi starts) is raised. */
static void test_odd_shapes_untouched(void)
{
	CONFIG_REC *c;
	int counter;

	c = cfg_parse("settings = { \"fe-ansi\" = \"odd\"; \"fe-common/irc\" = ( \"x\" ); };");
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 12);
	g_assert_cmpstr(config_node_get_str(config_node_find(c->mainnode, "settings"),
	                                    "fe-ansi", NULL), ==, "odd");
	g_assert_cmpint(config_node_find(config_node_find(c->mainnode, "settings"),
	                                 "fe-common/irc")->type, ==, NODE_TYPE_LIST);
	g_assert_cmpstr(cfg_get(c, "fe-common/core", "theme"), ==, "shellter");
	g_assert_cmpint(config_get_int(c, "erssi", "defaults_version", 0), ==, 1);
	config_close(c);

	/* the marker cannot be written: nothing is changed */
	c = cfg_parse("erssi = \"odd\";");
	counter = c->modifycounter;
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 0);
	g_assert_null(cfg_get(c, "fe-common/core", "theme"));
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);

	/* a marker that is not a scalar: config_set_int would replace it */
	c = cfg_parse("erssi = { defaults_version = { }; };");
	counter = c->modifycounter;
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 0);
	g_assert_null(cfg_get(c, "fe-common/core", "theme"));
	g_assert_cmpint(config_node_find(config_node_find(c->mainnode, "erssi"),
	                                 "defaults_version")->type, ==, NODE_TYPE_BLOCK);
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);

	c = cfg_parse("erssi = { defaults_version = ( \"1\" ); };");
	counter = c->modifycounter;
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 0);
	g_assert_null(cfg_get(c, "fe-common/core", "theme"));
	g_assert_cmpint(config_node_find(config_node_find(c->mainnode, "erssi"),
	                                 "defaults_version")->type, ==, NODE_TYPE_LIST);
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);

	c = cfg_parse("settings = \"odd\";");
	counter = c->modifycounter;
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 0);
	g_assert_cmpstr(config_node_get_str(c->mainnode, "settings", NULL), ==, "odd");
	g_assert_null(config_node_find(c->mainnode, "erssi"));
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);
}

static void test_note_saved(void)
{
	char *note;

	note = shellter_defaults_format_note(12, SHELLTER_DEFAULTS_SAVED, "/home/a/.erssi/config");
	g_assert_cmpstr(note, ==, "12 settings keep their Shellter Edition values, now written "
	                "in /home/a/.erssi/config (they were built-in defaults before 2026.10.0)");
	g_free(note);
}

static void test_note_broken(void)
{
	char *note;

	note = shellter_defaults_format_note(14, SHELLTER_DEFAULTS_BROKEN, "/home/a/.erssi/config");
	g_assert_cmpstr(note, ==, "14 settings keep their Shellter Edition values for this run "
	                "only: /home/a/.erssi/config has errors and was not changed; fix it and "
	                "restart erssi");
	g_assert_null(strstr(note, "/SAVE"));
	g_free(note);
}

static void test_note_unsaved(void)
{
	char *note;

	note = shellter_defaults_format_note(14, SHELLTER_DEFAULTS_UNSAVED, "/home/a/.erssi/config");
	g_assert_cmpstr(note, ==, "14 settings keep their Shellter Edition values (built-in "
	                "defaults before 2026.10.0) - /SAVE to keep them in /home/a/.erssi/config");
	g_free(note);
}

static void test_note_percent_and_none(void)
{
	char *note;

	note = shellter_defaults_format_note(3, SHELLTER_DEFAULTS_SAVED, "/tmp/a%b/config");
	g_assert_cmpstr(note, ==, "3 settings keep their Shellter Edition values, now written "
	                "in /tmp/a%%b/config (they were built-in defaults before 2026.10.0)");
	g_free(note);

	g_assert_null(shellter_defaults_format_note(0, SHELLTER_DEFAULTS_SAVED, "/x/config"));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/shellter_defaults/old_config_gets_values", test_old_config_gets_values);
	g_test_add_func("/shellter_defaults/user_values_kept", test_user_values_kept);
	g_test_add_func("/shellter_defaults/legacy_fe_text_section", test_legacy_fe_text_section);
	g_test_add_func("/shellter_defaults/runs_once", test_runs_once);
	g_test_add_func("/shellter_defaults/newer_marker", test_newer_marker);
	g_test_add_func("/shellter_defaults/empty_config", test_empty_config);
	g_test_add_func("/shellter_defaults/odd_shapes_untouched", test_odd_shapes_untouched);
	g_test_add_func("/shellter_defaults/note_saved", test_note_saved);
	g_test_add_func("/shellter_defaults/note_broken", test_note_broken);
	g_test_add_func("/shellter_defaults/note_unsaved", test_note_unsaved);
	g_test_add_func("/shellter_defaults/note_percent_and_none", test_note_percent_and_none);

	return g_test_run();
}
