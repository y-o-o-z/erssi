#ifndef IRSSI_CORE_RELOCATE_H
#define IRSSI_CORE_RELOCATE_H

/* The installation prefix and the paths under it.

   A build made with meson -Drelocatable=true finds its prefix when it
   runs: the directory two levels above the real path of its own
   executable (<prefix>/bin/erssi). Every compiled-in path under the
   compiled prefix (MODULEDIR, HELPDIR, THEMESDIR, SCRIPTDIR, PKGDATADIR,
   SYSCONFDIR, PERL_USE_LIB) is then taken under that prefix instead, so
   the installed tree can be moved or unpacked anywhere. When the
   executable cannot be found, the compiled paths are used (G_MESSAGES_DEBUG
   shows why). Other builds always use the compiled paths. */

/* The prefix (no trailing slash). Never freed. */
const char *get_install_prefix(void);

/* COMPILED (one of the paths above) as it is in this installation. The
   string returned lives as long as the process. */
const char *get_install_path(const char *compiled);

/* settings_get_str(KEY) of a setting whose default is a path under the
   prefix: a list of paths separated by any of SEPS, the first of them
   joining it again (help_path ":", perl_use_lib " \t"). In the binary
   installer's layout (<p>/releases/<version>/bin/erssi), an entry under
   <p>/releases/<another version> (one /SET -default wrote before
   2026.10.4, say) would name an old or removed tree: it becomes the same
   path under this release, with one warning line naming both. The other
   entries stay as they are, in their order. A list that is then the
   default is removed from the config. */
const char *get_install_setting(const char *key, const char *seps);

#endif
