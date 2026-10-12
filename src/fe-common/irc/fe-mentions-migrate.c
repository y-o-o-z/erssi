/*
 fe-mentions-migrate.c : mentions.pl users move to the built-in Mentions window

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "module.h"
#include <irssi/src/core/commands.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/lib-config/iconfig.h>

#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/irc/fe-mentions.h>
#include <irssi/src/perl/perl-core.h>

#include <sys/stat.h>

/* where the settings of the script were kept */
#define OLD_SETTINGS_MODULE "perl/core/scripts"

static guint unload_tag; /* idle source that unloads the script */
static gboolean deiniting;

/* Move one setting of the script to the built-in one, unless the built-in
   one is set. Returns TRUE if the key was set in the new place (before or
   now). */
static gboolean migrate_key(const char *key, gboolean is_bool)
{
	CONFIG_NODE *settings, *old_mod, *new_mod, *old;
	gboolean had_new;

	settings = iconfig_node_traverse("settings", FALSE);
	if (settings == NULL)
		return FALSE;

	new_mod = iconfig_node_section(settings, MODULE_NAME, -1);
	had_new = new_mod != NULL && config_node_find(new_mod, key) != NULL;
	old_mod = iconfig_node_section(settings, OLD_SETTINGS_MODULE, -1);
	old = old_mod == NULL ? NULL : config_node_find(old_mod, key);
	if (old == NULL)
		return had_new;

	if (!had_new) {
		/* the string points into the old node: settings_set_str copies it
		   before the node is removed below */
		if (is_bool)
			settings_set_bool(key, config_node_get_bool(old_mod, key, FALSE));
		else
			settings_set_str(key, config_node_get_str(old_mod, key, ""));
	}
	iconfig_node_remove(old_mod, old);
	return TRUE;
}

/* the script autoruns: the file or symlink exists, even a dangling one */
static gboolean script_autoruns(void)
{
	struct stat st;
	char *path;
	gboolean found;

	path = g_build_filename(get_irssi_dir(), "scripts", "autorun", "mentions.pl", NULL);
	found = lstat(path, &st) == 0;
	g_free(path);
	return found;
}

static void migrate_settings(void)
{
	gboolean log_set;
	int counter;

	counter = mainconfig->modifycounter;

	migrate_key("mentions_window", FALSE);
	log_set = migrate_key("mentions_log", TRUE);
	migrate_key("mentions_log_file", FALSE);

	/* the script logged by default */
	if (!log_set && script_autoruns())
		settings_set_bool("mentions_log", TRUE);

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
		settings_save(NULL, TRUE);
	}
}

static gboolean unload_script(gpointer data)
{
	unload_tag = 0;
	if (deiniting)
		return FALSE;

	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE, "%s",
	          "mentions.pl is no longer needed: erssi has a built-in Mentions window (the same "
	          "window, settings and log file; /HELP MENTIONS). The script was unloaded - remove "
	          "it from <irssi dir>/scripts/autorun to stop this message.");
	signal_emit("command script unload", 3, "mentions", NULL, NULL);
	return FALSE;
}

/* The script name is known before its code runs; unloading waits for the
   load to end. */
static void sig_script_created(PERL_SCRIPT_REC *script)
{
	if (deiniting || script == NULL || script->name == NULL ||
	    g_ascii_strcasecmp(script->name, "mentions") != 0)
		return;
	if (unload_tag == 0)
		unload_tag = g_idle_add(unload_script, NULL);
}

/* the script went away before the idle job ran (it failed to compile, or
   was unloaded by hand): nothing to unload, nothing to tell */
static void sig_script_destroyed(PERL_SCRIPT_REC *script)
{
	if (script == NULL || script->name == NULL ||
	    g_ascii_strcasecmp(script->name, "mentions") != 0)
		return;
	if (unload_tag != 0) {
		g_source_remove(unload_tag);
		unload_tag = 0;
	}
}

void fe_mentions_migrate_init(void)
{
	deiniting = FALSE;
	migrate_settings();
	signal_add("script created", (SIGNAL_FUNC) sig_script_created);
	signal_add("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);
}

void fe_mentions_migrate_deinit(void)
{
	deiniting = TRUE;
	signal_remove("script created", (SIGNAL_FUNC) sig_script_created);
	signal_remove("script destroyed", (SIGNAL_FUNC) sig_script_destroyed);
	if (unload_tag != 0) {
		g_source_remove(unload_tag);
		unload_tag = 0;
	}
}
