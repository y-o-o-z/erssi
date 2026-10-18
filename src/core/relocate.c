/*
 relocate.c : the installation prefix, found at run time in a relocatable
              build (see relocate.h)

    Copyright (C) 2026 the Shellter team

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
#include <irssi/src/core/relocate.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>

#ifdef ERSSI_RELOCATABLE
#include <limits.h>
#include <stdlib.h>
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

static char *install_prefix;
static GHashTable *install_paths;

static void reloc_debug(const char *what, const char *detail)
{
	g_log("erssi", G_LOG_LEVEL_DEBUG, "relocatable: %s (%s) - using the compiled prefix %s",
	      what, detail, ERSSI_PREFIX);
}

/* the path of the running executable as the kernel knows it */
static char *exe_path(void)
{
#if defined(__linux__)
	GError *error = NULL;
	char *path = g_file_read_link("/proc/self/exe", &error);

	if (path == NULL) {
		reloc_debug("cannot read /proc/self/exe", error->message);
		g_error_free(error);
	}
	return path;
#elif defined(__FreeBSD__)
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
	char buf[PATH_MAX];
	size_t len = sizeof(buf);

	if (sysctl(mib, 4, buf, &len, NULL, 0) != 0 || len == 0 || buf[0] == '\0') {
		reloc_debug("sysctl KERN_PROC_PATHNAME failed", g_strerror(errno));
		return NULL;
	}
	return g_strndup(buf, len);
#else
	reloc_debug("no way to find the executable on this system", "unsupported");
	return NULL;
#endif
}

/* dirname(dirname(realpath(executable))), or NULL */
static char *find_prefix(void)
{
	char *exe, *real, *bin, *prefix;

	exe = exe_path();
	if (exe == NULL)
		return NULL;
	/* a link (the installer's bin/erssi -> ../current/bin/erssi) or an
	   executable deleted since it started (" (deleted)" on Linux) */
	real = realpath(exe, NULL);
	if (real == NULL) {
		reloc_debug(exe, g_strerror(errno));
		g_free(exe);
		return NULL;
	}
	g_free(exe);
	bin = g_path_get_dirname(real);
	prefix = g_path_get_dirname(bin);
	free(real);
	g_free(bin);
	return prefix;
}

const char *get_install_prefix(void)
{
	if (install_prefix == NULL) {
		install_prefix = find_prefix();
		if (install_prefix == NULL)
			install_prefix = g_strdup(ERSSI_PREFIX);
	}
	return install_prefix;
}

const char *get_install_path(const char *compiled)
{
	const char *prefix, *rest;
	size_t len;
	char *path;

	prefix = get_install_prefix();
	/* the compiled prefix without its trailing slashes: "/" is "" */
	len = strlen(ERSSI_PREFIX);
	while (len > 0 && ERSSI_PREFIX[len - 1] == '/')
		len--;
	if (strcmp(prefix, ERSSI_PREFIX) == 0 || strncmp(compiled, ERSSI_PREFIX, len) != 0 ||
	    (compiled[len] != '/' && compiled[len] != '\0'))
		return compiled; /* not under the prefix, or not moved */
	rest = compiled + len;

	if (install_paths == NULL)
		install_paths = g_hash_table_new(g_str_hash, g_str_equal);
	path = g_hash_table_lookup(install_paths, compiled);
	if (path == NULL) {
		/* a prefix of "/" and a path under it: no double slash */
		path = g_strconcat(strcmp(prefix, "/") == 0 && *rest == '/' ? "" : prefix, rest, NULL);
		g_hash_table_insert(install_paths, g_strdup(compiled), path);
	}
	return path;
}

/* PATH is DIR or under it */
static gboolean path_in(const char *path, const char *dir)
{
	size_t len = strlen(dir);

	return strncmp(path, dir, len) == 0 && (path[len] == '/' || path[len] == '\0');
}

/* ENTRY, a path under RELEASES/<another version>, as the same path under
   PREFIX (this release); NULL when it is not one */
static char *entry_moved(const char *entry, const char *releases, const char *prefix)
{
	const char *version, *rest;

	if (!path_in(entry, releases) || path_in(entry, prefix))
		return NULL;
	version = entry + strlen(releases);
	if (*version != '/' || version[1] == '\0' || version[1] == '/')
		return NULL; /* <releases> itself, or with an empty version */
	rest = strchr(version + 1, '/');
	return g_strconcat(prefix, rest == NULL ? "" : rest, NULL);
}

const char *get_install_setting(const char *key, const char *seps)
{
	/* the entries warned about, "key old": the config can be read again
	   from the file during the run, one line each is enough */
	static GHashTable *warned;
	const char *value, *prefix;
	char *releases, *name, **entries, *moved, *msg, *id, *joined, sep[2];
	gboolean changed = FALSE;
	int i;

	value = settings_get_str(key);
	prefix = get_install_prefix();
	releases = g_path_get_dirname(prefix);
	name = g_path_get_basename(releases);
	if (strcmp(name, "releases") != 0) {
		g_free(releases);
		g_free(name);
		return value;
	}

	/* each entry of the list on its own; the others stay as they are,
	   in their order */
	entries = g_strsplit_set(value, seps, -1);
	for (i = 0; entries[i] != NULL; i++) {
		moved = entry_moved(entries[i], releases, prefix);
		if (moved == NULL)
			continue;
		if (warned == NULL)
			warned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		id = g_strconcat(key, " ", entries[i], NULL);
		if (!g_hash_table_contains(warned, id)) {
			msg = g_strdup_printf("%s: %s is in another release - using %s of this one",
			                      key, entries[i], moved);
			signal_emit("gui dialog", 2, "warning", msg);
			g_free(msg);
			g_hash_table_add(warned, id);
		} else {
			g_free(id);
		}
		g_free(entries[i]);
		entries[i] = moved;
		changed = TRUE;
	}
	if (changed) {
		sep[0] = seps[0];
		sep[1] = '\0';
		joined = g_strjoinv(sep, entries);
		/* the default itself is not written into the config */
		if (strcmp(joined, settings_get_record(key)->default_value.v_string) == 0)
			settings_reset(key);
		else
			settings_set_str(key, joined);
		g_free(joined);
		value = settings_get_str(key);
	}
	g_strfreev(entries);
	g_free(releases);
	g_free(name);
	return value;
}

#else /* not relocatable: the compiled paths */

const char *get_install_prefix(void)
{
	return ERSSI_PREFIX;
}

const char *get_install_path(const char *compiled)
{
	return compiled;
}

const char *get_install_setting(const char *key, const char *seps)
{
	(void) seps;
	return settings_get_str(key);
}

#endif
