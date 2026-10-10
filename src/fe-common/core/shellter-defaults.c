/*
 shellter-defaults.c : older configs keep the Shellter values of former defaults

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 Before 2026.10.0 erssi Shellter Edition had its own look built into the C
 defaults of these settings. The C defaults are erssi's again and the
 Shellter values come from the shipped config (irssi.conf). A config made
 before that has no erssi/defaults_version marker: it gets the former
 values once, only for the keys it does not have, so nothing the user set
 is changed and a key the user removes later is not added back.
*/

#include "module.h"
#include <irssi/src/core/settings.h>
#include <irssi/src/fe-common/core/shellter-defaults.h>

typedef struct {
	const char *module;
	const char *key;
	const char *value;
} SHELLTER_DEFAULT_REC;

/* the C defaults of erssi Shellter Edition before 2026.10.0 */
static const SHELLTER_DEFAULT_REC shellter_defaults[] = {
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

static int migrated_count;
static int migrated_saved;

/* TRUE when `parent' has no node `key' or a block of that name. A node of
   another shape would be replaced by lib-config (with a g_critical, fatal
   while erssi starts), so it is left alone. */
static int block_or_missing(CONFIG_NODE *parent, const char *key)
{
	CONFIG_NODE *node;

	node = config_node_find(parent, key);
	return node == NULL || node->type == NODE_TYPE_BLOCK;
}

int shellter_defaults_migrate(CONFIG_REC *config)
{
	CONFIG_NODE *marker, *settings, *section;
	int i, count;

	g_return_val_if_fail(config != NULL, 0);

	/* without a place for the marker it would run at every start */
	if (!block_or_missing(config->mainnode, "erssi") ||
	    !block_or_missing(config->mainnode, "settings"))
		return 0;

	marker = config_node_find(config->mainnode, "erssi");
	if (marker != NULL &&
	    config_node_get_int(marker, "defaults_version", 0) >= SHELLTER_DEFAULTS_VERSION)
		return 0;

	/* module names contain '/', so the sections are found by node, not by path */
	settings = config_node_traverse(config, "settings", TRUE);
	count = 0;
	for (i = 0; i < (int) G_N_ELEMENTS(shellter_defaults); i++) {
		const SHELLTER_DEFAULT_REC *rec = &shellter_defaults[i];

		if (settings == NULL || !block_or_missing(settings, rec->module))
			continue;
		section = config_node_section(config, settings, rec->module, NODE_TYPE_BLOCK);
		if (section == NULL || config_node_find(section, rec->key) != NULL)
			continue;

		config_node_set_str(config, section, rec->key, rec->value);
		count++;
	}

	config_set_int(config, "erssi", "defaults_version", SHELLTER_DEFAULTS_VERSION);
	return count;
}

void shellter_defaults_init(void)
{
	int counter;

	counter = mainconfig->modifycounter;
	migrated_saved = FALSE;
	migrated_count = shellter_defaults_migrate(mainconfig);
	if (mainconfig->modifycounter == counter)
		return;

	if (config_last_error(mainconfig) != NULL) {
		/* the file was read only in part: writing it would lose the
		   rest. The values apply for this run; the counter is put back
		   so that settings_autosave does not write the file either. */
		mainconfig->modifycounter = counter;
	} else if (mainconfig->fname != NULL &&
	           g_file_test(mainconfig->fname, G_FILE_TEST_IS_REGULAR)) {
		/* saved at once: /RELOAD or settings_autosave OFF keep it */
		migrated_saved = settings_save(NULL, TRUE);
	}
}

char *shellter_defaults_note(void)
{
	char *note, *text, **parts;

	if (migrated_count == 0)
		return NULL;

	note = g_strdup_printf("erssi: %d settings keep their Shellter Edition values, "
	                       "now written in %s (they were built-in defaults "
	                       "before 2026.10.0)%s",
	                       migrated_count,
	                       mainconfig->fname != NULL ? mainconfig->fname : "the config",
	                       migrated_saved ? "" : " - /SAVE to keep them");

	/* printed as text: a % in the path is not a colour code */
	parts = g_strsplit(note, "%", -1);
	text = g_strjoinv("%%", parts);
	g_strfreev(parts);
	g_free(note);
	return text;
}
