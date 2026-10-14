/*
 * Every theme erssi installs parses without an error: the theme loader
 * (theme_read() in themes.c) reads it with lib-config and prints "Ignored
 * errors in theme" at every start when config_parse() reports one. The
 * themes come as arguments from tests/fe-common/core/meson.build, which
 * passes the list themes/meson.build installs.
 */
#include <irssi/src/common.h>
#include <irssi/src/lib-config/iconfig.h>

#include <string.h>

/* the top-level keys theme_read() reads; a block closed too early leaves the
 * format blocks of the modules after it at the top level, where erssi never
 * looks (nexus.theme keeps a few loose single values there, which erssi
 * ignores as well; only a block out of place is an error here) */
static const char *const THEME_KEYS[] = {
	"default_color", "info_eol", "replaces", "abstracts", "formats",
};

static gboolean theme_key_known(const char *key)
{
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS(THEME_KEYS); i++)
		if (strcmp(key, THEME_KEYS[i]) == 0)
			return TRUE;
	return FALSE;
}

static void test_theme_parses(gconstpointer data)
{
	const char *path = data;
	CONFIG_REC *config;
	CONFIG_NODE *formats;
	GSList *tmp;

	config = config_open(path, -1);
	g_assert_nonnull(config);
	if (config == NULL)
		return;
	config_parse(config);
	if (config_last_error(config) != NULL)
		g_test_message("Ignored errors in theme %s:\n%s", path,
		               config_last_error(config));
	g_assert_null(config_last_error(config));

	for (tmp = config_node_first(config->mainnode->value); tmp != NULL;
	     tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;
		gboolean lost;

		lost = node->type == NODE_TYPE_BLOCK && node->key != NULL &&
		       !theme_key_known(node->key);
		if (lost)
			g_test_message("%s: block \"%s\" outside formats", path, node->key);
		g_assert_false(lost);
	}

	/* a theme may have no formats at all (colorless.theme) */
	formats = config_node_traverse(config, "formats", FALSE);
	g_assert_true(formats == NULL || formats->type == NODE_TYPE_BLOCK);
	config_close(config);
}

int main(int argc, char **argv)
{
	int i;

	g_test_init(&argc, &argv, NULL);
	/* report every broken theme, not only the first one */
	g_test_set_nonfatal_assertions();

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
