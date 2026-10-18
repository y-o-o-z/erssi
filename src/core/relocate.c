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

#else /* not relocatable: the compiled paths */

const char *get_install_prefix(void)
{
	return ERSSI_PREFIX;
}

const char *get_install_path(const char *compiled)
{
	return compiled;
}

#endif
