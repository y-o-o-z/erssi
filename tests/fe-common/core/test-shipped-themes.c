/*
 * Every theme erssi installs parses without an error and has the shape the
 * theme loader reads: theme_read() in themes.c reads it with lib-config and
 * prints "Ignored errors in theme" at every start when config_parse()
 * reports one. The themes come as arguments from
 * tests/fe-common/core/meson.build, which passes `shipped_themes' of the
 * top-level meson.build (the list themes/meson.build installs).
 *
 * A misplaced brace does not always unbalance the file. A block closed too
 * early or too late can leave entries in places where erssi never looks,
 * and lib-config parses that without an error:
 * - "formats" closed early: the module blocks after it at the top level;
 * - "abstracts" closed early: the abstracts after it at the top level;
 * - a module block closed early: its formats directly under "formats";
 * - a module block closed late: the next module nested inside it.
 * The structure check finds all of them; /themes/structure/broken shows
 * that it does, on small broken themes.
 */
#include <irssi/src/common.h>
#include <irssi/src/lib-config/iconfig.h>

#include <string.h>

/* the top-level keys theme_read() reads */
static const char *const THEME_BLOCKS[] = { "replaces", "abstracts", "formats" };
static const char *const THEME_SCALARS[] = { "default_color", "info_eol" };

/* nexus.theme closes "abstracts" early since its first version (1.0.0), so
 * these abstracts sit at the top level, where erssi never reads them. They
 * are allowed in that theme only, until it is fixed; any other loose key,
 * or one of these in another theme, is an error. */
static const char *const NEXUS_LOOSE_KEYS[] = {
	"sb_background", "window_border", "sb_topic_bg", "sb", "sbmode",
	"sbservertag", "sbmore", "sblag", "sb_default_bg", "sb_act_sep",
	"sb_act_text", "sb_act_msg", "sb_act_hilight", "sb_act_hilight_color",
	"sb_info1_bg", "sb_window_bg", "sb_window2_bg", "sb_info2_bg", "sbstart",
	"sbend", "topicsbstart", "topicsbend", "prompt", "sbaway", "sbnickmode",
	"sb_usercount", "sb_uc_normal", "sb_uc_ops", "sb_uc_voices", "whois_field",
	"whois_label", "whois_chan_op", "whois_chan_voice", "whois_chan_normal",
	"whois_chan_owner", "whois_chan_admin", "whois_nick", "whois_userhost",
	"whois_realname",
};

static gboolean in_list(const char *key, const char *const *list, int count)
{
	int i;

	for (i = 0; i < count; i++)
		if (strcmp(key, list[i]) == 0)
			return TRUE;
	return FALSE;
}

/* every entry of block is a single value (abstracts, replaces, the formats
 * of one module) */
static void check_values(GString *errors, CONFIG_NODE *block, const char *where)
{
	GSList *tmp;

	for (tmp = config_node_first(block->value); tmp != NULL; tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;

		if (node->key == NULL || !has_node_value(node))
			g_string_append_printf(errors, "%s: \"%s\" is not a single value\n", where,
			                       node->key != NULL ? node->key : "(list item)");
	}
}

/* every entry of formats is a module block that holds only single values */
static void check_modules(GString *errors, CONFIG_NODE *formats)
{
	GSList *tmp;

	for (tmp = config_node_first(formats->value); tmp != NULL; tmp = config_node_next(tmp)) {
		CONFIG_NODE *module = tmp->data;
		char *where;

		if (module->key == NULL || module->type != NODE_TYPE_BLOCK) {
			g_string_append_printf(errors,
			                       "formats: \"%s\" is not a module block (a module "
			                       "closed too early)\n",
			                       module->key != NULL ? module->key : "(list item)");
			continue;
		}
		/* a block in a module block: the next module, nested because
		 * this one was closed too late */
		where = g_strdup_printf("formats.\"%s\"", module->key);
		check_values(errors, module, where);
		g_free(where);
	}
}

/* The problems of a parsed theme, one per line; empty when there are none.
 * name: the theme's file name (nexus.theme has its known loose keys). */
static GString *theme_problems(CONFIG_REC *config, const char *name)
{
	GString *errors = g_string_new(NULL);
	gboolean nexus = strcmp(name, "nexus.theme") == 0;
	CONFIG_NODE *formats;
	GSList *tmp;

	if (config_last_error(config) != NULL)
		g_string_append_printf(errors, "parse error: %s\n", config_last_error(config));

	for (tmp = config_node_first(config->mainnode->value); tmp != NULL;
	     tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;
		const char *key = node->key;

		if (key == NULL) {
			g_string_append(errors, "top level: an entry without a name\n");
		} else if (node->type == NODE_TYPE_BLOCK &&
		           in_list(key, THEME_BLOCKS, G_N_ELEMENTS(THEME_BLOCKS))) {
			if (strcmp(key, "formats") != 0)
				check_values(errors, node, key);
		} else if (has_node_value(node) &&
		           (in_list(key, THEME_SCALARS, G_N_ELEMENTS(THEME_SCALARS)) ||
		            (nexus && in_list(key, NEXUS_LOOSE_KEYS,
		                              G_N_ELEMENTS(NEXUS_LOOSE_KEYS))))) {
			/* read by theme_read(), or a known loose key of nexus */
		} else {
			g_string_append_printf(errors, "top level: \"%s\" is out of place (%s)\n",
			                       key, node->type == NODE_TYPE_BLOCK ?
			                                "a block outside formats" :
			                                "a value outside its block");
		}
	}

	/* a theme may have no formats at all (colorless.theme) */
	formats = config_node_find(config->mainnode, "formats");
	if (formats != NULL && formats->type != NODE_TYPE_BLOCK)
		g_string_append(errors, "formats is not a block\n");
	else if (formats != NULL)
		check_modules(errors, formats);
	return errors;
}

static void test_theme_parses(gconstpointer data)
{
	const char *path = data;
	CONFIG_REC *config;
	GString *errors;
	char *base;

	config = config_open(path, -1);
	g_assert_nonnull(config);
	if (config == NULL)
		return;
	config_parse(config);

	base = g_path_get_basename(path);
	errors = theme_problems(config, base);
	if (errors->len > 0)
		g_test_message("%s:\n%s", path, errors->str);
	g_assert_cmpstr(errors->str, ==, "");
	g_string_free(errors, TRUE);
	g_free(base);
	config_close(config);
}

/* a theme from a string, as config_parse() reads a file */
static GString *problems_of(const char *text, const char *name)
{
	CONFIG_REC *config;
	GString *errors;

	config = config_open(NULL, -1);
	config_parse_data(config, text, name);
	errors = theme_problems(config, name);
	config_close(config);
	return errors;
}

typedef struct {
	const char *what;
	const char *name;
	const char *text;
	const char *problem; /* a part of the expected report */
} BROKEN_THEME;

#define GOOD_ABSTRACTS "abstracts = { line_start = \"x\"; timestamp = \"y\"; };\n"
#define GOOD_CORE "\"fe-common/core\" = { join = \"j\"; part = \"p\"; };\n"
#define GOOD_IRC "\"fe-common/irc\" = { netsplit = \"n\"; };\n"

static const BROKEN_THEME BROKEN[] = {
	{ "an extra closing brace", "test.theme",
	  GOOD_ABSTRACTS "formats = {\n" GOOD_CORE "};\n};\n" GOOD_IRC "};\n", "parse error" },
	{ "formats closed early", "test.theme",
	  GOOD_ABSTRACTS "formats = {\n" GOOD_CORE "};\n" GOOD_IRC,
	  "\"fe-common/irc\" is out of place (a block outside formats)" },
	{ "abstracts closed early", "test.theme",
	  "abstracts = { line_start = \"x\"; };\ntimestamp = \"y\";\n"
	  "formats = {\n" GOOD_CORE "};\n",
	  "\"timestamp\" is out of place (a value outside its block)" },
	{ "a module closed early", "test.theme",
	  GOOD_ABSTRACTS "formats = {\n\"fe-common/core\" = { join = \"j\"; };\n"
	  "part = \"p\";\n" GOOD_IRC "};\n",
	  "formats: \"part\" is not a module block" },
	{ "a module closed late (the next one nested in it)", "test.theme",
	  GOOD_ABSTRACTS "formats = {\n\"fe-common/core\" = { join = \"j\";\n" GOOD_IRC
	  "};\n};\n",
	  "formats.\"fe-common/core\": \"fe-common/irc\" is not a single value" },
	{ "formats nested in abstracts", "test.theme",
	  "abstracts = { line_start = \"x\";\nformats = {\n" GOOD_CORE "};\n};\n",
	  "abstracts: \"formats\" is not a single value" },
	{ "a loose key of nexus.theme in another theme", "test.theme",
	  GOOD_ABSTRACTS "sb_background = \"%8\";\nformats = {\n" GOOD_CORE "};\n",
	  "\"sb_background\" is out of place" },
	{ "a loose key nexus.theme does not have", "nexus.theme",
	  GOOD_ABSTRACTS "sb_new = \"%8\";\nformats = {\n" GOOD_CORE "};\n",
	  "\"sb_new\" is out of place" },
};

static void test_broken_themes(void)
{
	GString *errors;
	int i;

	/* the well-formed theme the broken ones are made from */
	errors = problems_of("default_color = \"-1\";\ninfo_eol = \"false\";\n"
	                     "replaces = { \"[]\" = \"%K$*%n\"; };\n" GOOD_ABSTRACTS
	                     "formats = {\n" GOOD_CORE GOOD_IRC "};\n",
	                     "test.theme");
	g_assert_cmpstr(errors->str, ==, "");
	g_string_free(errors, TRUE);

	/* nexus.theme's known loose keys pass in nexus.theme */
	errors = problems_of(GOOD_ABSTRACTS "sb_background = \"%8\";\nformats = {\n" GOOD_CORE
	                     "};\n",
	                     "nexus.theme");
	g_assert_cmpstr(errors->str, ==, "");
	g_string_free(errors, TRUE);

	for (i = 0; i < (int) G_N_ELEMENTS(BROKEN); i++) {
		errors = problems_of(BROKEN[i].text, BROKEN[i].name);
		if (strstr(errors->str, BROKEN[i].problem) == NULL)
			g_test_message("%s: expected \"%s\", got:\n%s", BROKEN[i].what,
			               BROKEN[i].problem, errors->str);
		g_assert_nonnull(strstr(errors->str, BROKEN[i].problem));
		g_string_free(errors, TRUE);
	}
}

int main(int argc, char **argv)
{
	int i;

	g_test_init(&argc, &argv, NULL);
	/* report every broken theme, not only the first one */
	g_test_set_nonfatal_assertions();

	g_test_add_func("/themes/structure/broken", test_broken_themes);

	/* g_test_init() leaves the arguments it does not know: the themes */
	g_assert_cmpint(argc, >, 1);
	for (i = 1; i < argc; i++) {
		char *base = g_path_get_basename(argv[i]);
		char *name = g_strdup_printf("/themes/parse/%s", base);

		g_test_add_data_func(name, argv[i], test_theme_parses);
		g_free(name);
		g_free(base);
	}

	return g_test_run();
}
