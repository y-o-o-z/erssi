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

 The statusbar works the same way: a config holds a copy of the default
 config's bars once it was saved, and a bar's item list in the config
 replaces the built-in one. The info bar of such a copy gets the items
 erssi added to it since (the e2e item) once - only when its list is
 exactly one erssi shipped (erssi/statusbar_version marks it done); a list
 the user changed is the user's.

 Each value and each statusbar change belongs to the version of its
 marker that brought it, and applies only to a config whose marker is
 lower: a later version brings only its own changes.
*/

#include "module.h"
#include <irssi/src/core/settings.h>
#include <irssi/src/fe-common/core/shellter-defaults.h>

/* the C defaults of erssi Shellter Edition before 2026.10.0. `since' is the
   defaults_version that brought the row: a config whose marker is lower
   gets it. A later version adds its rows with its own number and leaves
   these alone (a key the user removed after version 1 stays removed). */
static const SHELLTER_DEFAULT_REC shellter_defaults[] = {
	{ 1, "fe-ansi", "colors_ansi_24bit", "yes" },
	{ 1, "fe-common/irc", "print_whois_rpl_in_server_window", "yes" },
	{ 1, "fe-common/core", "theme", "shellter" },
	{ 1, "fe-common/core", "show_names_on_join_limit", "1" },
	{ 1, "fe-common/core", "show_names_on_join_alone", "no" },
	{ 1, "fe-common/core", "nick_column_width", "13" },
	{ 1, "fe-common/core", "nick_hash_colors",
	  "#f59e0b #fcd34d #fdba74 #79c0ff #a5d6ff #d2a8ff #3fb950 #7ee787" },
	{ 1, "fe-common/core", "nick_mode_color_owner", "%ZFF7B72%_" },
	{ 1, "fe-common/core", "nick_mode_color_op", "%ZF59E0B%_" },
	{ 1, "fe-common/core", "nick_mode_color_halfop", "%Z3FB950%_" },
	{ 1, "fe-common/core", "nick_mode_color_voice", "%Z79C0FF%_" },
	{ 1, "fe-common/core", "hilight_color", "%ZFCD34D" },
	{ 1, "fe-common/core", "hilight_act_color", "%ZFF7B72" },
	{ 1, "fe-common/core", "hidden_settings", "translate_deepl_key" },
};

static int migrated_count;
static int migrated_state;
static char *save_error;	/* why the write failed (SAVE_FAILED) */

/* TRUE when `parent' has no node `key' or a block of that name. A node of
   another shape would be replaced by lib-config (with a g_critical, fatal
   while erssi starts), so it is left alone. */
static int block_or_missing(CONFIG_NODE *parent, const char *key)
{
	CONFIG_NODE *node;

	node = config_node_find(parent, key);
	return node == NULL || node->type == NODE_TYPE_BLOCK;
}

int shellter_defaults_migrate_rows(CONFIG_REC *config, const SHELLTER_DEFAULT_REC *rows,
                                   int rows_count, int version)
{
	CONFIG_NODE *marker, *node, *settings, *section;
	int i, count, stored;

	g_return_val_if_fail(config != NULL, 0);
	g_return_val_if_fail(rows != NULL || rows_count == 0, 0);

	/* without a place for the marker it would run at every start (a
	   defaults_version that is not a scalar is such a case, below) */
	if (!block_or_missing(config->mainnode, "erssi") ||
	    !block_or_missing(config->mainnode, "settings"))
		return 0;

	marker = config_node_find(config->mainnode, "erssi");
	node = marker == NULL ? NULL : config_node_find(marker, "defaults_version");
	if (node != NULL && !has_node_value(node))
		return 0;
	stored = marker == NULL ? 0 : config_node_get_int(marker, "defaults_version", 0);
	if (stored >= version)
		return 0;

	/* module names contain '/', so the sections are found by node, not by
	   path. "settings" is a block or missing (checked above), so this
	   returns a block. */
	settings = config_node_traverse(config, "settings", TRUE);
	count = 0;
	for (i = 0; i < rows_count; i++) {
		const SHELLTER_DEFAULT_REC *rec = &rows[i];

		/* the config had this row's version already */
		if (rec->since <= stored || rec->since > version)
			continue;
		if (!block_or_missing(settings, rec->module))
			continue;
		section = config_node_section(config, settings, rec->module, NODE_TYPE_BLOCK);
		if (section == NULL || config_node_find(section, rec->key) != NULL)
			continue;

		config_node_set_str(config, section, rec->key, rec->value);
		count++;
	}

	config_set_int(config, "erssi", "defaults_version", version);
	return count;
}

int shellter_defaults_migrate(CONFIG_REC *config)
{
	return shellter_defaults_migrate_rows(config, shellter_defaults,
	                                      G_N_ELEMENTS(shellter_defaults),
	                                      SHELLTER_DEFAULTS_VERSION);
}

/* the statusbar version that added the e2e item to the info bar */
#define STATUSBAR_E2E_SINCE 1

/* statusbar.default.info.items of every irssi.conf erssi shipped since the
   info bar became the default (53b2a8b1) up to the one with the e2e item
   (2026.10.0): its items in order, each "<name>[ <key>=<value>...]". All
   of them shipped the same list. */
static const char *const shipped_info_items[][7] = {
	{ "time", "user", "window", "lag", "act", "more alignment=right", NULL },
};

/* "<name>[ <key>=<value>...]" of an item of a bar, NULL when it is no
   block of scalars */
static char *item_text(CONFIG_NODE *item)
{
	GString *str;
	GSList *tmp;

	if (item->key == NULL || item->type != NODE_TYPE_BLOCK)
		return NULL;
	str = g_string_new(item->key);
	for (tmp = config_node_first(item->value); tmp != NULL; tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;

		if (node->key == NULL || !has_node_value(node)) {
			g_string_free(str, TRUE);
			return NULL;
		}
		g_string_append_printf(str, " %s=%s", node->key, (char *) node->value);
	}
	return g_string_free(str, FALSE);
}

/* items holds exactly the items of one list erssi shipped */
static int shipped_info_list(CONFIG_NODE *items)
{
	int i, j;

	for (i = 0; i < (int) G_N_ELEMENTS(shipped_info_items); i++) {
		GSList *tmp = config_node_first(items->value);
		int same = TRUE;

		for (j = 0; same && shipped_info_items[i][j] != NULL; j++) {
			char *text = tmp == NULL ? NULL : item_text(tmp->data);

			same = g_strcmp0(text, shipped_info_items[i][j]) == 0;
			g_free(text);
			if (tmp != NULL)
				tmp = config_node_next(tmp);
		}
		if (same && tmp == NULL)
			return TRUE;
	}
	return FALSE;
}

/* the block at mainnode/statusbar/default/info/items, NULL when the config
   has none (the built-in bar is used then) or one of another shape */
static CONFIG_NODE *info_items(CONFIG_REC *config)
{
	static const char *const path[] = { "statusbar", "default", "info", "items" };
	CONFIG_NODE *node = config->mainnode;
	int i;

	for (i = 0; node != NULL && i < (int) G_N_ELEMENTS(path); i++) {
		node = config_node_find(node, path[i]);
		if (node != NULL && node->type != NODE_TYPE_BLOCK)
			return NULL;
	}
	return node;
}

int shellter_statusbar_migrate_to(CONFIG_REC *config, int version)
{
	CONFIG_NODE *marker, *node, *items, *window;
	int added = FALSE, stored;

	g_return_val_if_fail(config != NULL, FALSE);

	/* no place for the marker: it would run at every start */
	if (!block_or_missing(config->mainnode, "erssi"))
		return FALSE;
	marker = config_node_find(config->mainnode, "erssi");
	node = marker == NULL ? NULL : config_node_find(marker, "statusbar_version");
	if (node != NULL && !has_node_value(node))
		return FALSE;
	stored = marker == NULL ? 0 : config_node_get_int(marker, "statusbar_version", 0);
	if (stored >= version)
		return FALSE;

	/* Each change belongs to the statusbar version that brought it and
	   applies only below that version: a later version adds its own
	   change here, under its own check, and leaves the e2e item of a
	   config already at version 1 alone (the user may have removed it).
	   shipped_info_items are the lists before version 1. */
	items = info_items(config);
	if (stored < STATUSBAR_E2E_SINCE && items != NULL && shipped_info_list(items) &&
	    config_node_find(items, "e2e") == NULL) {
		window = config_node_find(items, "window");
		/* every shipped list has window, as a block */
		config_node_section_index(config, items, "e2e",
		                          g_slist_index(items->value, window) + 1,
		                          NODE_TYPE_BLOCK);
		added = TRUE;
	}

	config_set_int(config, "erssi", "statusbar_version", version);
	return added;
}

int shellter_statusbar_migrate(CONFIG_REC *config)
{
	return shellter_statusbar_migrate_to(config, SHELLTER_STATUSBAR_VERSION);
}

int shellter_save_at_start(char **reason)
{
	int ok;

	if (reason != NULL)
		*reason = NULL;
	/* the callers save only a config read without errors: what is
	   cleared below can only be the error of this write */
	g_return_val_if_fail(config_last_error(mainconfig) == NULL, FALSE);

	/* lib-config's rename path returns success with the error set, so
	   an error after the write is a failed write too */
	ok = settings_save(NULL, TRUE) && config_last_error(mainconfig) == NULL;
	if (!ok) {
		/* The file is intact, but the reason left in last_error would
		   look like a parse error for the rest of the run (erssi
		   --setup, the other migrations). lib-config has no public
		   call to clear it. */
		if (reason != NULL)
			*reason = g_strdup(config_last_error(mainconfig) != NULL ?
			                   config_last_error(mainconfig) : "unknown error");
		g_free_and_null(mainconfig->last_error);
	}
	return ok;
}

void shellter_defaults_init(void)
{
	int counter;

	g_free_and_null(save_error);
	counter = mainconfig->modifycounter;
	migrated_state = SHELLTER_DEFAULTS_SAVED;
	migrated_count = shellter_defaults_migrate(mainconfig);
	/* the item shows itself; no note */
	shellter_statusbar_migrate(mainconfig);
	if (mainconfig->modifycounter == counter)
		return;

	if (mainconfig->fname == NULL ||
	    !g_file_test(mainconfig->fname, G_FILE_TEST_IS_REGULAR)) {
		/* the user has no config file yet: the config was read from
		   elsewhere (SYSCONFDIR/irssi.conf, which may be irssi's,
		   without the markers). Like an upstream first run: the
		   values apply, nothing is said, and the counter is put back
		   so that settings_autosave does not write the file at quit
		   because of this. */
		mainconfig->modifycounter = counter;
		migrated_state = SHELLTER_DEFAULTS_NO_FILE;
	} else if (config_last_error(mainconfig) != NULL) {
		/* the file was read only in part: writing it would lose the
		   rest. The values apply for this run; the counter is put back
		   so that settings_autosave does not write the file either. */
		mainconfig->modifycounter = counter;
		migrated_state = SHELLTER_DEFAULTS_BROKEN;
	} else if (!shellter_save_at_start(&save_error)) {
		/* saved at once, so that /RELOAD or settings_autosave OFF
		   keep it - but the write failed (a full quota, a read-only
		   directory); the reason is kept for the note */
		migrated_state = SHELLTER_DEFAULTS_SAVE_FAILED;
	}
}

/* `text' for printtext: a % is not a colour code */
static char *escape_percent(const char *text)
{
	char **parts, *escaped;

	parts = g_strsplit(text, "%", -1);
	escaped = g_strjoinv("%%", parts);
	g_strfreev(parts);
	return escaped;
}

char *shellter_defaults_format_note(int count, int state, const char *fname,
                                    const char *reason)
{
	char *path, *why, *note;

	if (count == 0 || state == SHELLTER_DEFAULTS_NO_FILE)
		return NULL;

	path = escape_percent(fname != NULL ? fname : "the config");
	switch (state) {
	case SHELLTER_DEFAULTS_SAVED:
		note = g_strdup_printf("%d settings keep their Shellter Edition values, "
		                       "now written in %s (they were built-in defaults "
		                       "before 2026.10.0)", count, path);
		break;
	case SHELLTER_DEFAULTS_BROKEN:
		/* no /SAVE here: it would write the part that was read */
		note = g_strdup_printf("%d settings keep their Shellter Edition values "
		                       "for this run only: %s has errors and was not "
		                       "changed; fix it and restart erssi", count, path);
		break;
	default:
		why = escape_percent(reason != NULL ? reason : "unknown error");
		note = g_strdup_printf("%d settings keep their Shellter Edition values "
		                       "for now (built-in defaults before 2026.10.0), but "
		                       "writing %s failed: %s - /SAVE to retry",
		                       count, path, why);
		g_free(why);
		break;
	}
	g_free(path);
	return note;
}

char *shellter_defaults_note(void)
{
	return shellter_defaults_format_note(migrated_count, migrated_state,
	                                     mainconfig->fname, save_error);
}
