/*
 * Tests for shellter-defaults.c: a config made before 2026.10.0 gets the
 * Shellter Edition values of the former C defaults once, only for the keys
 * it lacks.
 */
#include <irssi/src/common.h>
#include <glib/gstdio.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/lib-config/iconfig.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/shellter-defaults.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void fe_whois_init(void);

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

/* what a new ~/.erssi gets: UPGRADED with the hilight colour of irssi.conf */
static const DEFAULT_ROW FRESH[] = {
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
	{ "fe-common/core", "hilight_color", "%Zbb9af7" },
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

static void test_builtin_config_fresh_values(void)
{
	CONFIG_REC *c;
	int i, counter;

	c = cfg_parse(default_config);
	g_assert_cmpint(G_N_ELEMENTS(FRESH), ==, UPGRADED_COUNT);
	for (i = 0; i < (int) G_N_ELEMENTS(FRESH); i++)
		g_assert_cmpstr(cfg_get(c, FRESH[i].module, FRESH[i].key), ==, FRESH[i].value);
	g_assert_cmpint(config_get_int(c, "erssi", "defaults_version", 0), ==, 1);
	counter = c->modifycounter;
	g_assert_cmpint(shellter_defaults_migrate(c), ==, 0);
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);
}

/* the info bar as every irssi.conf before 2026.10.0 shipped it */
#define SHIPPED_INFO \
	"statusbar = { default = { info = {\n" \
	"  items = {\n" \
	"    time = { };\n" \
	"    user = { };\n" \
	"    window = { };\n" \
	"    lag = { };\n" \
	"    act = { };\n" \
	"    more = { alignment = \"right\"; };\n" \
	"  };\n" \
	"  position = \"0\";\n" \
	"  placement = \"bottom\";\n" \
	"}; }; };\n"

/* "time user window ..." of a bar's items, NULL when it has none */
static char *bar_items(CONFIG_REC *c, const char *bar)
{
	CONFIG_NODE *node;
	GString *str;
	GSList *tmp;

	node = config_node_traverse(c, "statusbar", FALSE);
	node = node == NULL ? NULL : config_node_find(node, "default");
	node = node == NULL ? NULL : config_node_find(node, bar);
	node = node == NULL ? NULL : config_node_find(node, "items");
	if (node == NULL)
		return NULL;
	str = g_string_new(NULL);
	for (tmp = config_node_first(node->value); tmp != NULL; tmp = config_node_next(tmp))
		g_string_append_printf(str, "%s%s", str->len > 0 ? " " : "",
		                       ((CONFIG_NODE *) tmp->data)->key);
	return g_string_free(str, FALSE);
}

static void assert_bar(CONFIG_REC *c, const char *bar, const char *items)
{
	char *got = bar_items(c, bar);

	g_assert_cmpstr(got, ==, items);
	g_free(got);
}

/* the shipped list gets e2e after window, the rest of the bar stays */
static void test_statusbar_shipped_list(void)
{
	CONFIG_REC *c;

	c = cfg_parse(SHIPPED_INFO);
	g_assert_true(shellter_statusbar_migrate(c));
	assert_bar(c, "info", "time user window e2e lag act more");
	g_assert_cmpstr(config_get_str(c, "statusbar/default/info/items/more", "alignment", NULL),
	                ==, "right");
	g_assert_cmpstr(config_get_str(c, "statusbar/default/info", "position", NULL), ==, "0");
	g_assert_cmpint(config_get_int(c, "erssi", "statusbar_version", 0), ==, 1);
	/* the marker does not touch the settings' one */
	g_assert_cmpint(config_get_int(c, "erssi", "defaults_version", 0), ==, 0);
	config_close(c);
}

/* a list the user changed, and other bars, are never touched */
static void test_statusbar_custom_untouched(void)
{
	static const char *const custom[] = {
		/* an item removed */
		"statusbar = { default = { info = { items = { time = { }; user = { }; "
		"window = { }; act = { }; more = { alignment = \"right\"; }; }; }; }; };",
		/* an item added */
		"statusbar = { default = { info = { items = { time = { }; user = { }; "
		"window = { }; lag = { }; act = { }; more = { alignment = \"right\"; }; "
		"usercount = { }; }; }; }; };",
		/* the same names in another order */
		"statusbar = { default = { info = { items = { user = { }; time = { }; "
		"window = { }; lag = { }; act = { }; more = { alignment = \"right\"; }; }; }; }; };",
		/* an attribute changed */
		"statusbar = { default = { info = { items = { time = { }; user = { }; "
		"window = { }; lag = { }; act = { }; more = { }; }; }; }; };",
		"statusbar = { default = { info = { items = { time = { priority = \"5\"; }; "
		"user = { }; window = { }; lag = { }; act = { }; "
		"more = { alignment = \"right\"; }; }; }; }; };",
	};
	CONFIG_REC *c;
	char *before, *after;
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS(custom); i++) {
		c = cfg_parse(custom[i]);
		before = bar_items(c, "info");
		g_assert_false(shellter_statusbar_migrate(c));
		after = bar_items(c, "info");
		g_assert_cmpstr(after, ==, before);
		g_assert_null(strstr(after, "e2e"));
		g_assert_cmpint(config_get_int(c, "erssi", "statusbar_version", 0), ==, 1);
		g_free(before);
		g_free(after);
		config_close(c);
	}

	/* the shipped list on another bar: not the info bar */
	c = cfg_parse("statusbar = { default = { info2 = { items = { time = { }; user = { }; "
	              "window = { }; lag = { }; act = { }; more = { alignment = \"right\"; }; "
	              "}; }; }; };");
	g_assert_false(shellter_statusbar_migrate(c));
	assert_bar(c, "info2", "time user window lag act more");
	config_close(c);
}

/* once: an e2e the user removed later is not added back */
static void test_statusbar_runs_once(void)
{
	CONFIG_REC *c;
	CONFIG_NODE *items;
	int counter;

	c = cfg_parse(SHIPPED_INFO);
	g_assert_true(shellter_statusbar_migrate(c));
	items = config_node_traverse(c, "statusbar/default/info/items", FALSE);
	g_assert_nonnull(items);
	config_node_remove(c, items, config_node_find(items, "e2e"));
	assert_bar(c, "info", "time user window lag act more");
	counter = c->modifycounter;
	g_assert_false(shellter_statusbar_migrate(c));
	assert_bar(c, "info", "time user window lag act more");
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);

	/* a newer marker */
	c = cfg_parse(SHIPPED_INFO "erssi = { statusbar_version = \"2\"; };");
	g_assert_false(shellter_statusbar_migrate(c));
	assert_bar(c, "info", "time user window lag act more");
	config_close(c);
}

/* no statusbar section (the built-in bars are used), or one of an odd
   shape: nothing to do, no g_critical */
static void test_statusbar_no_section(void)
{
	static const char *const odd[] = {
		"statusbar = \"odd\";",
		"statusbar = { default = ( \"x\" ); };",
		"statusbar = { default = { info = \"x\"; }; };",
		"statusbar = { default = { info = { items = ( \"time\" ); }; }; };",
		"statusbar = { default = { info = { position = \"3\"; }; }; };",
	};
	CONFIG_REC *c;
	int i, counter;

	c = cfg_parse("settings = { core = { nick = \"alice\"; }; };");
	g_assert_false(shellter_statusbar_migrate(c));
	g_assert_null(config_node_find(c->mainnode, "statusbar"));
	g_assert_cmpint(config_get_int(c, "erssi", "statusbar_version", 0), ==, 1);
	config_close(c);

	c = config_open(NULL, -1);
	g_assert_false(shellter_statusbar_migrate(c));
	g_assert_cmpint(config_get_int(c, "erssi", "statusbar_version", 0), ==, 1);
	config_close(c);

	for (i = 0; i < (int) G_N_ELEMENTS(odd); i++) {
		c = cfg_parse(odd[i]);
		g_assert_false(shellter_statusbar_migrate(c));
		g_assert_cmpint(config_get_int(c, "erssi", "statusbar_version", 0), ==, 1);
		config_close(c);
	}

	/* no place for the marker: nothing changes */
	c = cfg_parse(SHIPPED_INFO "erssi = \"odd\";");
	counter = c->modifycounter;
	g_assert_false(shellter_statusbar_migrate(c));
	assert_bar(c, "info", "time user window lag act more");
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);
	c = cfg_parse(SHIPPED_INFO "erssi = { statusbar_version = { }; };");
	g_assert_false(shellter_statusbar_migrate(c));
	assert_bar(c, "info", "time user window lag act more");
	config_close(c);
}

/* the built-in irssi.conf has the item and the marker: nothing changes */
static void test_statusbar_builtin_config(void)
{
	CONFIG_REC *c;
	int counter;

	c = cfg_parse(default_config);
	assert_bar(c, "info", "time user window e2e lag act more");
	g_assert_cmpint(config_get_int(c, "erssi", "statusbar_version", 0), ==, 1);
	counter = c->modifycounter;
	g_assert_false(shellter_statusbar_migrate(c));
	g_assert_cmpint(c->modifycounter, ==, counter);
	config_close(c);
}

static void test_note_saved(void)
{
	char *note;

	note = shellter_defaults_format_note(12, SHELLTER_DEFAULTS_SAVED, "/home/a/.erssi/config", NULL);
	g_assert_cmpstr(note, ==, "12 settings keep their Shellter Edition values, now written "
	                "in /home/a/.erssi/config (they were built-in defaults before 2026.10.0)");
	g_free(note);
}

static void test_note_broken(void)
{
	char *note;

	note = shellter_defaults_format_note(14, SHELLTER_DEFAULTS_BROKEN, "/home/a/.erssi/config", NULL);
	g_assert_cmpstr(note, ==, "14 settings keep their Shellter Edition values for this run "
	                "only: /home/a/.erssi/config has errors and was not changed; fix it and "
	                "restart erssi");
	g_assert_null(strstr(note, "/SAVE"));
	g_free(note);
}

static void test_note_save_failed(void)
{
	char *note;

	note = shellter_defaults_format_note(14, SHELLTER_DEFAULTS_SAVE_FAILED,
	                                     "/home/a/.erssi/config", "Disk quota exceeded");
	g_assert_cmpstr(note, ==, "14 settings keep their Shellter Edition values for now "
	                "(built-in defaults before 2026.10.0), but writing /home/a/.erssi/config "
	                "failed: Disk quota exceeded - /SAVE to retry");
	g_free(note);

	/* the reason is text too */
	note = shellter_defaults_format_note(2, SHELLTER_DEFAULTS_SAVE_FAILED, "/tmp/a%b/config",
	                                     "100% full");
	g_assert_cmpstr(note, ==, "2 settings keep their Shellter Edition values for now "
	                "(built-in defaults before 2026.10.0), but writing /tmp/a%%b/config "
	                "failed: 100%% full - /SAVE to retry");
	g_free(note);
}

/* no file of the user's yet: nothing to say */
static void test_note_no_file(void)
{
	g_assert_null(shellter_defaults_format_note(14, SHELLTER_DEFAULTS_NO_FILE,
	                                            "/home/a/.erssi/config", NULL));
}

static void test_note_percent_and_none(void)
{
	char *note;

	note = shellter_defaults_format_note(3, SHELLTER_DEFAULTS_SAVED, "/tmp/a%b/config", NULL);
	g_assert_cmpstr(note, ==, "3 settings keep their Shellter Edition values, now written "
	                "in /tmp/a%%b/config (they were built-in defaults before 2026.10.0)");
	g_free(note);

	g_assert_null(shellter_defaults_format_note(0, SHELLTER_DEFAULTS_SAVED, "/x/config", NULL));
}

/* Integration: a real start of core and fe-common in a child process, with
   its own --home. */

/* the home of the test, made by the parent for the child process */
#define TEST_HOME_ENV "ERSSI_TEST_HOME"

static void remove_tree(const char *path)
{
	GDir *dir;
	const char *name;

	if (g_file_test(path, G_FILE_TEST_IS_DIR) &&
	    !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
		dir = g_dir_open(path, 0, NULL);
		if (dir != NULL) {
			while ((name = g_dir_read_name(dir)) != NULL) {
				char *child = g_build_filename(path, name, NULL);
				remove_tree(child);
				g_free(child);
			}
			g_dir_close(dir);
		}
	}
	g_remove(path);
}

/* In the parent: makes a home with `config' (none when NULL), read-only
   when `read_only', runs the test in a child process with it, removes the
   home (also when the child failed) and returns FALSE. In the child:
   returns TRUE. A home that stays writable when it is made read-only (a
   test run by root) skips the test. */
static gboolean run_in_child_mode(const char *config, gboolean read_only)
{
	GError *error = NULL;
	char *home, *fname;
	int fd;

	if (g_test_subprocess())
		return TRUE;

	home = g_dir_make_tmp("erssi-defaults-XXXXXX", &error);
	g_assert_no_error(error);
	if (config != NULL) {
		fname = g_build_filename(home, "config", NULL);
		g_file_set_contents(fname, config, -1, &error);
		g_free(fname);
		if (error != NULL)
			remove_tree(home);
		g_assert_no_error(error);
	}

	if (read_only) {
		g_assert_cmpint(g_chmod(home, 0500), ==, 0);
		fname = g_build_filename(home, "probe", NULL);
		fd = g_open(fname, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd != -1) {
			close(fd);
			g_remove(fname);
		}
		g_free(fname);
		if (fd != -1) {
			g_chmod(home, 0700);
			remove_tree(home);
			g_free(home);
			g_test_skip("a read-only directory is still writable here");
			return FALSE;
		}
	}

	g_setenv(TEST_HOME_ENV, home, TRUE);
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_unsetenv(TEST_HOME_ENV);
	if (read_only)
		g_chmod(home, 0700);
	remove_tree(home);
	g_free(home);
	g_test_trap_assert_passed();
	return FALSE;
}

static gboolean run_in_child(const char *config)
{
	return run_in_child_mode(config, FALSE);
}

/* in the child: starts erssi's core in the home of the test; returns the
   home */
static char *start_core_in_home(void)
{
	const char *home;
	char *argv[] = { "test-shellter-defaults", "--home", NULL, NULL };

	home = g_getenv(TEST_HOME_ENV);
	g_assert_nonnull(home);
	argv[2] = (char *) home;
	core_register_options();
	fe_common_core_register_options();
	args_execute(3, argv);
	core_preinit(argv[0]);
	core_init();
	return g_strdup(home);
}

/* in the child: starts erssi's core and fe-common in the home of the test;
   returns the home */
static char *start_in_home(void)
{
	char *home;

	home = start_core_in_home();
	fe_common_core_init();
	fe_whois_init();
	return home;
}

static void assert_effective(const DEFAULT_ROW *rows, int count)
{
	SETTINGS_REC *rec;
	int i;

	for (i = 0; i < count; i++) {
		/* colors_ansi_24bit: fe-ansi is not started here, see
		   tests/fe-ansi/test-term-defaults.c */
		if (g_strcmp0(rows[i].module, "fe-ansi") == 0)
			continue;
		rec = settings_get_record(rows[i].key);
		g_assert_nonnull(rec);
		g_assert_cmpstr(rec->module, ==, rows[i].module);
		switch (rec->type) {
		case SETTING_TYPE_BOOLEAN:
			g_assert_cmpint(settings_get_bool(rows[i].key), ==,
			                g_strcmp0(rows[i].value, "yes") == 0);
			break;
		case SETTING_TYPE_INT:
			g_assert_cmpint(settings_get_int(rows[i].key), ==, atoi(rows[i].value));
			break;
		case SETTING_TYPE_STRING:
			g_assert_cmpstr(settings_get_str(rows[i].key), ==, rows[i].value);
			break;
		default:
			g_assert_not_reached();
		}
	}
}

static void assert_default_str(const char *key, const char *value)
{
	SETTINGS_REC *rec;

	rec = settings_get_record(key);
	g_assert_nonnull(rec);
	g_assert_cmpint(rec->type, ==, SETTING_TYPE_STRING);
	g_assert_cmpstr(rec->default_value.v_string, ==, value);
}

static void assert_default_int(const char *key, int value)
{
	SETTINGS_REC *rec;

	rec = settings_get_record(key);
	g_assert_nonnull(rec);
	g_assert_cmpint(rec->type, ==, SETTING_TYPE_INT);
	g_assert_cmpint(rec->default_value.v_int, ==, value);
}

static void assert_default_bool(const char *key, int value)
{
	SETTINGS_REC *rec;

	rec = settings_get_record(key);
	g_assert_nonnull(rec);
	g_assert_cmpint(rec->type, ==, SETTING_TYPE_BOOLEAN);
	g_assert_cmpint(rec->default_value.v_bool, ==, value);
}

/* a new home: the Shellter values come from the built-in irssi.conf, the C
   defaults are erssi's */
static void test_fresh_home(void)
{
	char *home, *fname, *note;

	if (!g_test_subprocess() &&
	    g_file_test(SYSCONFDIR "/" IRSSI_GLOBAL_CONFIG, G_FILE_TEST_EXISTS)) {
		g_test_skip(SYSCONFDIR "/" IRSSI_GLOBAL_CONFIG " exists: a home without a "
		            "config reads it, not the built-in irssi.conf");
		return;
	}
	if (!run_in_child(NULL))
		return;

	home = start_in_home();
	assert_effective(FRESH, G_N_ELEMENTS(FRESH));
	g_assert_cmpstr(settings_get_str("theme"), ==, "shellter");
	g_assert_cmpint(settings_get_int("show_names_on_join_limit"), ==, 1);
	g_assert_false(settings_get_bool("show_names_on_join_alone"));
	g_assert_cmpint(settings_get_int("nick_column_width"), ==, 13);
	g_assert_true(settings_get_bool("print_whois_rpl_in_server_window"));
	g_assert_cmpstr(settings_get_str("hilight_color"), ==, "%Zbb9af7");

	note = shellter_defaults_note();
	g_assert_null(note);
	fname = g_build_filename(home, "config", NULL);
	g_assert_false(g_file_test(fname, G_FILE_TEST_EXISTS));

	assert_default_str("theme", "default");
	assert_default_int("show_names_on_join_limit", 18);
	assert_default_bool("show_names_on_join_alone", TRUE);
	assert_default_int("nick_column_width", 10);
	assert_default_str("nick_hash_colors", "g r b m c y G C");
	assert_default_str("nick_mode_color_owner", "");
	assert_default_str("nick_mode_color_op", "");
	assert_default_str("nick_mode_color_halfop", "");
	assert_default_str("nick_mode_color_voice", "");
	assert_default_str("nick_mode_color_normal", "");
	assert_default_str("hilight_color", "%Y");
	assert_default_str("hilight_act_color", "%M");
	assert_default_str("hidden_settings", "");
	assert_default_bool("print_whois_rpl_in_server_window", FALSE);

	g_free(fname);
	g_free(home);
}

/* a config from before 2026.10.0: the former defaults are written in it at
   once, what the user set is kept */
static void test_upgraded_home(void)
{
	char *home, *fname, *contents, *note, *expected;
	GError *error = NULL;

	if (!run_in_child("settings = { core = { nick = \"alice\"; }; "
	                  "\"fe-text\" = { colors_ansi_24bit = \"no\"; };\n"
	                  "  \"fe-common/core\" = { hilight_color = \"%Zbb9af7\"; "
	                  "nick_column_width = \"10\"; }; };\n"))
		return;

	home = start_in_home();
	g_assert_cmpstr(settings_get_str("theme"), ==, "shellter");
	g_assert_cmpint(settings_get_int("nick_column_width"), ==, 10);
	g_assert_cmpstr(settings_get_str("hilight_color"), ==, "%Zbb9af7");
	g_assert_true(settings_get_bool("print_whois_rpl_in_server_window"));
	g_assert_cmpint(settings_get_int("show_names_on_join_limit"), ==, 1);
	g_assert_false(settings_get_bool("show_names_on_join_alone"));
	g_assert_cmpstr(settings_get_str("hidden_settings"), ==, "translate_deepl_key");

	fname = g_build_filename(home, "config", NULL);
	g_file_get_contents(fname, &contents, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(contents, "defaults_version = \"1\";"));
	g_assert_nonnull(strstr(contents, "theme = \"shellter\";"));

	note = shellter_defaults_note();
	expected = g_strdup_printf("12 settings keep their Shellter Edition values, now "
	                           "written in %s (they were built-in defaults before "
	                           "2026.10.0)", fname);
	g_assert_cmpstr(note, ==, expected);

	g_free(expected);
	g_free(note);
	g_free(contents);
	g_free(fname);
	g_free(home);
}

/* a config written from an older irssi.conf: its info bar gets the e2e
   item, in the file at once */
static void test_upgraded_home_statusbar(void)
{
	CONFIG_REC *c;
	char *home, *fname;

	if (!run_in_child("settings = { core = { nick = \"alice\"; }; };\n" SHIPPED_INFO
	                  "erssi = { defaults_version = \"1\"; };\n"))
		return;

	home = start_in_home();
	fname = g_build_filename(home, "config", NULL);
	c = config_open(fname, -1);
	g_assert_nonnull(c);
	g_assert_cmpint(config_parse(c), ==, 0);
	assert_bar(c, "info", "time user window e2e lag act more");
	g_assert_cmpint(config_get_int(c, "erssi", "statusbar_version", 0), ==, 1);
	/* the settings had their marker: no note about them */
	g_assert_null(shellter_defaults_note());
	config_close(c);
	g_free(fname);
	g_free(home);
}

/* a config with errors is not written: the part not read would be lost */
static void test_broken_config(void)
{
	static const char config[] =
		"settings = { core = { nick = \"alice\"; }; };\nservers = ( { address = ";
	char *home, *fname, *contents, *note, *expected;
	gsize length;
	GError *error = NULL;

	if (!run_in_child(config))
		return;

	home = start_in_home();
	fname = g_build_filename(home, "config", NULL);
	g_file_get_contents(fname, &contents, &length, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(contents, length, config, strlen(config));
	g_assert_cmpstr(settings_get_str("theme"), ==, "shellter");

	/* the part read has only core.nick, so all the rows are in memory */
	note = shellter_defaults_note();
	expected = g_strdup_printf("14 settings keep their Shellter Edition values for this "
	                           "run only: %s has errors and was not changed; fix it "
	                           "and restart erssi", fname);
	g_assert_cmpstr(note, ==, expected);

	g_free(expected);
	g_free(note);
	g_free(contents);
	g_free(fname);
	g_free(home);
}

/* The config's directory is read-only, so the config cannot be written:
   the values apply for this run, the note says that the write failed and
   why, and no error is left in mainconfig that looks like a parse error
   (erssi --setup and the mentions migration check it). */
static void test_save_failed(void)
{
	static const char config[] = "settings = { core = { nick = \"alice\"; }; };\n";
	char *home, *fname, *contents, *note, *expected;
	gsize length;
	GError *error = NULL;

	if (!run_in_child_mode(config, TRUE))
		return;

	home = start_in_home();
	g_assert_null(config_last_error(mainconfig));
	g_assert_cmpstr(settings_get_str("theme"), ==, "shellter");
	g_assert_cmpint(settings_get_int("nick_column_width"), ==, 13);
	g_assert_cmpstr(settings_get_str("hidden_settings"), ==, "translate_deepl_key");

	fname = g_build_filename(home, "config", NULL);
	g_file_get_contents(fname, &contents, &length, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(contents, length, config, strlen(config));

	note = shellter_defaults_note();
	expected = g_strdup_printf("14 settings keep their Shellter Edition values for now "
	                           "(built-in defaults before 2026.10.0), but writing %s "
	                           "failed: %s - /SAVE to retry", fname, g_strerror(EACCES));
	g_assert_cmpstr(note, ==, expected);

	g_free(expected);
	g_free(note);
	g_free(contents);
	g_free(fname);
	g_free(home);
}

/* No config file of the user's yet, and the one read instead (for example
   SYSCONFDIR/irssi.conf of irssi) has no markers: the values apply in
   memory, like at an upstream first run - no note, and the modify counter
   is as before, so settings_autosave does not write the file at quit. */
static void test_no_user_file(void)
{
	CONFIG_NODE *section;
	char *home, *fname;
	int counter;

	if (!run_in_child(NULL))
		return;

	home = start_core_in_home();
	fname = g_build_filename(home, "config", NULL);
	g_assert_cmpstr(mainconfig->fname, ==, fname);
	g_assert_false(g_file_test(fname, G_FILE_TEST_EXISTS));

	/* a config read from elsewhere, without the markers and a theme */
	config_node_set_str(mainconfig, mainconfig->mainnode, "erssi", NULL);
	section = cfg_section(mainconfig, "fe-common/core");
	if (section != NULL)
		config_node_set_str(mainconfig, section, "theme", NULL);
	g_assert_null(cfg_get(mainconfig, "fe-common/core", "theme"));
	counter = mainconfig->modifycounter;

	shellter_defaults_init();
	g_assert_cmpstr(cfg_get(mainconfig, "fe-common/core", "theme"), ==, "shellter");
	g_assert_cmpint(config_get_int(mainconfig, "erssi", "defaults_version", 0), ==, 1);
	g_assert_cmpint(mainconfig->modifycounter, ==, counter);
	g_assert_null(shellter_defaults_note());
	g_assert_null(config_last_error(mainconfig));
	g_assert_false(g_file_test(fname, G_FILE_TEST_EXISTS));

	g_free(fname);
	g_free(home);
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
	g_test_add_func("/shellter_defaults/builtin_config_fresh_values",
	                test_builtin_config_fresh_values);
	g_test_add_func("/shellter_defaults/note_saved", test_note_saved);
	g_test_add_func("/shellter_defaults/note_broken", test_note_broken);
	g_test_add_func("/shellter_defaults/note_save_failed", test_note_save_failed);
	g_test_add_func("/shellter_defaults/note_no_file", test_note_no_file);
	g_test_add_func("/shellter_defaults/note_percent_and_none", test_note_percent_and_none);
	g_test_add_func("/shellter_defaults/fresh_home", test_fresh_home);
	g_test_add_func("/shellter_defaults/upgraded_home", test_upgraded_home);
	g_test_add_func("/shellter_defaults/broken_config", test_broken_config);
	g_test_add_func("/shellter_defaults/save_failed", test_save_failed);
	g_test_add_func("/shellter_defaults/no_user_file", test_no_user_file);
	g_test_add_func("/shellter_defaults/statusbar_shipped_list", test_statusbar_shipped_list);
	g_test_add_func("/shellter_defaults/statusbar_custom_untouched",
	                test_statusbar_custom_untouched);
	g_test_add_func("/shellter_defaults/statusbar_runs_once", test_statusbar_runs_once);
	g_test_add_func("/shellter_defaults/statusbar_no_section", test_statusbar_no_section);
	g_test_add_func("/shellter_defaults/statusbar_builtin_config",
	                test_statusbar_builtin_config);
	g_test_add_func("/shellter_defaults/upgraded_home_statusbar", test_upgraded_home_statusbar);

	return g_test_run();
}
