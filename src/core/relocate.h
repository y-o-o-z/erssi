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

#endif
