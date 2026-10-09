/*
 irssi.c : irssi

    Copyright (C) 1999-2000 Timo Sirainen

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
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <irssi/src/fe-ansi/module-formats.h>
#include "default-files-known.h"
#include <irssi/src/core/modules-load.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/session.h>
#include <irssi/src/core/servers.h>

#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/fe-settings.h>
#include <irssi/src/fe-common/core/themes.h>

#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/gui-entry.h>
#include <irssi/src/fe-ansi/mainwindows.h>
#include <irssi/src/fe-ansi/sidepanels-render.h>
#include <irssi/src/fe-ansi/gui-printtext.h>
#include <irssi/src/fe-ansi/gui-readline.h>
#include <irssi/src/fe-ansi/statusbar.h>
#include <irssi/src/fe-ansi/gui-windows.h>
#include <irssi/irssi-version.h>
#include <irssi/src/fe-ansi/sidepanels.h>
#include <irssi/src/fe-ansi/gui-mouse.h>
#include <irssi/src/fe-ansi/gui-gestures.h>
#include <irssi/src/fe-ansi/resize-debug.h>

#ifdef HAVE_IMAGE_PREVIEW
#include <irssi/src/image-preview/image-preview.h>
#endif

#include <signal.h>
#include <locale.h>

void gui_expandos_init(void);
void gui_expandos_deinit(void);

void textbuffer_commands_init(void);
void textbuffer_commands_deinit(void);

void textbuffer_formats_init(void);
void textbuffer_formats_deinit(void);

void lastlog_init(void);
void lastlog_deinit(void);

void mainwindow_activity_init(void);
void mainwindow_activity_deinit(void);

void mainwindows_layout_init(void);
void mainwindows_layout_deinit(void);

void sidepanels_init(void);
void sidepanels_deinit(void);

static int dirty, full_redraw;

static GMainLoop *main_loop;
int quitting;

static int display_firsttimer = FALSE;
static GSList *default_notes;	/* about ~/.erssi files, printed after the banner */
static unsigned int user_settings_changed = 0;

static void sig_exit(void)
{
	quitting = TRUE;
}

static void sig_settings_userinfo_changed(gpointer changedp)
{
	user_settings_changed = GPOINTER_TO_UINT(changedp);
}

static void sig_autoload_modules(void)
{
	char **list, **module;
	list = g_strsplit_set(settings_get_str("autoload_modules"), " ,", -1);
	for (module = list; *module != NULL; module++) {
		char *tmp;
		if ((tmp = strchr(*module, ':')) != NULL)
			*tmp = ' ';
		tmp = g_strdup_printf("-silent %s", *module);
		signal_emit("command load", 1, tmp);
		g_free(tmp);
	}
	g_strfreev(list);
}

/* redraw irssi's screen.. */
void irssi_redraw(void)
{
	dirty = TRUE;
	full_redraw = TRUE;
}

void irssi_set_dirty(void)
{
	dirty = TRUE;
}

static void dirty_check(void)
{
	if (!dirty)
		return;

	resize_debug_log("DIRTY_CHECK", "dirty_check() called, full_redraw=%d", full_redraw);

	/* Freeze terminal output - all drawing operations will be buffered
	 * and flushed once at the end. This eliminates flicker from multiple
	 * intermediate flushes during window/statusbar redraws. */
	term_refresh_freeze();
	resize_debug_log("DIRTY_CHECK", "term_refresh_freeze() done");

	term_resize_dirty();
	resize_debug_log("DIRTY_CHECK", "term_resize_dirty() done");

	if (full_redraw) {
		full_redraw = FALSE;
		resize_debug_log("DIRTY_CHECK", "FULL REDRAW starting");

		/* first clear the screen so curses will be
		   forced to redraw the screen */
		term_clear();
		resize_debug_log("DIRTY_CHECK", "term_clear() done");

		mainwindows_redraw();
		resize_debug_log("DIRTY_CHECK", "mainwindows_redraw() done");
		sidepanels_invalidate_caches(); /* the screen under them was cleared */
		redraw_both_panels_only("screen_clear"); /* Redraw only sidepanels after full screen clear */
		resize_debug_log("DIRTY_CHECK", "redraw_both_panels_only() done");
		statusbar_redraw(NULL, TRUE);
		resize_debug_log("DIRTY_CHECK", "statusbar_redraw() done");

		/* Draw horizontal separator above statusbar for notcurses
		 * (terminfo uses scroll regions for protection) */
		if (screen_reserved_bottom > 0) {
			term_draw_statusbar_separator(term_height - screen_reserved_bottom - 1);
		}
		resize_debug_log("DIRTY_CHECK", "FULL REDRAW complete");
	}

	mainwindows_redraw_dirty();
	statusbar_redraw_dirty();

	/* Redraw statusbar separator during dirty updates as well */
	if (screen_reserved_bottom > 0) {
		term_draw_statusbar_separator(term_height - screen_reserved_bottom - 1);
	}

	/* Thaw and flush all buffered output in one operation */
	term_refresh_thaw();
	resize_debug_log("DIRTY_CHECK", "term_refresh_thaw() done, dirty_check complete");

	dirty = FALSE;
}

static void textui_init(void)
{
#ifdef SIGTRAP
	struct sigaction act;

	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = SIG_IGN;
	sigaction(SIGTRAP, &act, NULL);
#endif

	irssi_gui = IRSSI_GUI_TEXT;
	core_init();
	fe_common_core_init();

	theme_register(gui_text_formats);
	signal_add("settings userinfo changed", (SIGNAL_FUNC) sig_settings_userinfo_changed);
	signal_add("module autoload", (SIGNAL_FUNC) sig_autoload_modules);
	signal_add_last("gui exit", (SIGNAL_FUNC) sig_exit);
}

static int critical_fatal_section_begin(void)
{
	return g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
}

static void critical_fatal_section_end(int loglev)
{
	g_log_set_always_fatal(loglev);
}

static void textui_finish_init(void)
{
	int loglev;
	quitting = FALSE;

	term_refresh_freeze();
	textbuffer_init();
	textbuffer_view_init();
	textbuffer_commands_init();
	textbuffer_formats_init();
	gui_expandos_init();
	gui_printtext_init();
	gui_readline_init();
	gui_entry_init();
	lastlog_init();
	mainwindows_init();
	mainwindow_activity_init();
	mainwindows_layout_init();
	gui_windows_init();
	gui_mouse_init();
	gui_gestures_init();
	sidepanels_init();
#ifdef HAVE_IMAGE_PREVIEW
	image_preview_init();
#endif
	/* Temporarily raise the fatal level to abort on config errors. */
	loglev = critical_fatal_section_begin();
	statusbar_init();
	critical_fatal_section_end(loglev);

	resize_debug_init();
	settings_check();

	module_register("core", "fe-text");

	dirty_check();

	/* Temporarily raise the fatal level to abort on config errors. */
	loglev = critical_fatal_section_begin();
	fe_common_core_finish_init();
	critical_fatal_section_end(loglev);
	term_refresh_thaw();

	signal_emit("irssi init finished", 0);
	statusbar_redraw(NULL, TRUE);

	/* every start, also when servers connect at once (they are still being
	 * looked up), but not after /UPGRADE (its servers are restored) */
	if (servers == NULL)
		printformat(NULL, NULL, MSGLEVEL_CRAP | MSGLEVEL_NO_ACT, TXT_IRSSI_BANNER);
	while (default_notes != NULL) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE, "%s", (char *) default_notes->data);
		g_free(default_notes->data);
		default_notes = g_slist_delete_link(default_notes, default_notes);
	}

	if (display_firsttimer) {
		printformat(NULL, NULL, MSGLEVEL_CRAP | MSGLEVEL_NO_ACT, TXT_WELCOME_FIRSTTIME);
	}

	/* First run: show local account info and safe defaults for privacy.
	 * See irc-servers-setup.c:init_userinfo for details. */
	if (user_settings_changed & USER_SETTINGS_FIRST_RUN) {
		const char *sys_user, *sys_real;
		const char *irc_nick, *irc_user, *irc_real;

		/* Get system-detected values */
		sys_user = g_get_user_name();
		sys_real = g_get_real_name();

		/* Get current IRC defaults from config */
		irc_nick = settings_get_str("nick");
		irc_user = settings_get_str("user_name");
		irc_real = settings_get_str("real_name");

		/* Display local account info */
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_LOCAL_HEADER);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_LOCAL_SETTING, "user_name:", sys_user);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_LOCAL_SETTING, "real_name:", sys_real);

		/* Display IRC safe defaults */
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_IRC_HEADER);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_IRC_SETTING, "nick:", irc_nick ? irc_nick : "");
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_IRC_SETTING, "user_name:", irc_user ? irc_user : "");
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_IRC_SETTING, "real_name:", irc_real ? irc_real : "");

		/* Display how to change */
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
		            TXT_FIRSTRUN_HOWTO);
	}

	term_environment_check();
}

static void textui_deinit(void)
{
	signal(SIGINT, SIG_DFL);

	term_refresh_freeze();
	while (modules != NULL)
		module_unload(modules->data);

	dirty_check(); /* one last time to print any quit messages */
	signal_remove("settings userinfo changed", (SIGNAL_FUNC) sig_settings_userinfo_changed);
	signal_remove("module autoload", (SIGNAL_FUNC) sig_autoload_modules);
	signal_remove("gui exit", (SIGNAL_FUNC) sig_exit);

	resize_debug_deinit();
	lastlog_deinit();
	statusbar_deinit();
#ifdef HAVE_IMAGE_PREVIEW
	image_preview_deinit();
#endif
	sidepanels_deinit();
	gui_gestures_deinit();
	gui_mouse_deinit();
	gui_entry_deinit();
	gui_printtext_deinit();
	gui_readline_deinit();
	gui_windows_deinit();
	mainwindows_layout_deinit();
	mainwindow_activity_deinit();
	mainwindows_deinit();
	gui_expandos_deinit();
	textbuffer_formats_deinit();
	textbuffer_commands_deinit();
	textbuffer_view_deinit();
	textbuffer_deinit();

	term_refresh_thaw();
	term_deinit();

	theme_unregister();

	fe_common_core_deinit();
	core_deinit();
}

/* Files erssi ships into ~/.erssi (themes, startup) are updated like
 * configuration files of a package: replaced by the new version only while
 * the user has not changed them - their sha256 is the one erssi recorded
 * when it installed them (default-files.sha256) or one of a version erssi
 * ever shipped under that name (known_default_files). A changed file is
 * never touched;
 * when a new version ships, one note says where it is. Symbolic links and
 * anything that is not a regular file are left alone. */
#define DEFAULT_FILES_SHA256 "default-files.sha256"

static GHashTable *default_hashes;	/* file name -> recorded sha256 */
static gboolean default_hashes_changed;
static gboolean default_themes_changed;

static void default_hashes_load(const char *irssi_dir)
{
	char *path, *contents, **lines, **line;

	default_hashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	path = g_strdup_printf("%s/" DEFAULT_FILES_SHA256, irssi_dir);
	if (g_file_get_contents(path, &contents, NULL, NULL)) {
		/* "<sha256>  <name>", as sha256sum writes it */
		lines = g_strsplit(contents, "\n", -1);
		for (line = lines; *line != NULL; line++) {
			if (strlen(*line) > 66 && (*line)[64] == ' ' && (*line)[65] == ' ')
				g_hash_table_replace(default_hashes, g_strdup(*line + 66),
				                     g_strndup(*line, 64));
		}
		g_strfreev(lines);
		g_free(contents);
	}
	g_free(path);
}

static void default_hashes_save(const char *irssi_dir)
{
	GString *out;
	GList *names, *tmp;
	char *path;

	if (default_hashes_changed) {
		out = g_string_new(NULL);
		names = g_list_sort(g_hash_table_get_keys(default_hashes), (GCompareFunc) strcmp);
		for (tmp = names; tmp != NULL; tmp = tmp->next) {
			g_string_append_printf(out, "%s  %s\n",
			                       (char *) g_hash_table_lookup(default_hashes, tmp->data),
			                       (char *) tmp->data);
		}
		g_list_free(names);
		path = g_strdup_printf("%s/" DEFAULT_FILES_SHA256, irssi_dir);
		if (g_file_set_contents(path, out->str, out->len, NULL))
			chmod(path, 0600);
		g_free(path);
		g_string_free(out, TRUE);
	}
	g_hash_table_destroy(default_hashes);
	default_hashes = NULL;
}

static void default_hash_record(const char *name, const char *hash)
{
	const char *old = g_hash_table_lookup(default_hashes, name);

	if (old == NULL || strcmp(old, hash) != 0) {
		g_hash_table_replace(default_hashes, g_strdup(name), g_strdup(hash));
		default_hashes_changed = TRUE;
	}
}

static gboolean default_hash_known(const char *name, const char *hash)
{
	int i;

	for (i = 0; known_default_files[i][0] != NULL; i++) {
		if (strcmp(known_default_files[i][0], name) == 0 &&
		    strcmp(known_default_files[i][1], hash) == 0)
			return TRUE;
	}
	return FALSE;
}

/* ~/.erssi/<name> from shipped_path: copied when missing, replaced while
 * unchanged by the user, otherwise kept (one note per new shipped version). */
/* The contents of a regular file, never through a symbolic link: opened
 * once with O_NOFOLLOW and checked on that descriptor, so a link swapped in
 * after the lstat() is not followed */
static gboolean read_regular_file(const char *path, char **data, gsize *len)
{
	GString *buf;
	struct stat st;
	char chunk[8192];
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return FALSE;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return FALSE;
	}
	buf = g_string_sized_new(st.st_size + 1);
	while ((n = read(fd, chunk, sizeof(chunk))) > 0 ||
	       (n < 0 && errno == EINTR)) {
		if (n > 0)
			g_string_append_len(buf, chunk, n);
	}
	close(fd);
	if (n < 0) {
		g_string_free(buf, TRUE);
		return FALSE;
	}
	*len = buf->len;
	*data = g_string_free(buf, FALSE);
	return TRUE;
}

/* Replaces path with data: a new file with the given mode from the start
 * (no window with the umask's mode), renamed over the old one */
static gboolean write_file_mode(const char *path, const char *data, gsize len, int mode)
{
	char *tmp = g_strconcat(path, ".XXXXXX", NULL);
	gsize done = 0;
	int fd, ok;

	fd = g_mkstemp_full(tmp, O_WRONLY | O_CLOEXEC, mode);
	if (fd < 0) {
		g_free(tmp);
		return FALSE;
	}
	ok = fchmod(fd, mode) == 0;
	while (ok && done < len) {
		ssize_t n = write(fd, data + done, len - done);
		if (n < 0 && errno == EINTR)
			continue;
		ok = n > 0;
		if (ok)
			done += n;
	}
	ok = close(fd) == 0 && ok;
	ok = ok && rename(tmp, path) == 0;
	if (!ok)
		unlink(tmp);
	g_free(tmp);
	return ok;
}

static void update_default_file(const char *shipped_path, const char *irssi_dir,
                                const char *name, gboolean theme)
{
	char *shipped, *current, *dst_path, *shipped_hash, *current_hash;
	const char *recorded;
	gsize shipped_len, current_len;
	struct stat st;

	if (!g_file_get_contents(shipped_path, &shipped, &shipped_len, NULL))
		return;
	shipped_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *) shipped, shipped_len);
	recorded = g_hash_table_lookup(default_hashes, name);
	dst_path = g_strdup_printf("%s/%s", irssi_dir, name);

	if (g_lstat(dst_path, &st) != 0) {
		if (errno == ENOENT && write_file_mode(dst_path, shipped, shipped_len, 0600))
			default_hash_record(name, shipped_hash);
	} else if (S_ISREG(st.st_mode) &&
	           read_regular_file(dst_path, &current, &current_len)) {
		current_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *) current, current_len);
		if (strcmp(current_hash, shipped_hash) == 0) {
			default_hash_record(name, shipped_hash);
		} else if ((recorded != NULL && strcmp(current_hash, recorded) == 0) ||
		           default_hash_known(name, current_hash)) {
			/* not changed since erssi installed it: the new version */
			if (write_file_mode(dst_path, shipped, shipped_len, st.st_mode & 07777)) {
				default_hash_record(name, shipped_hash);
				default_themes_changed |= theme;
			}
		} else {
			/* the user's own version: kept, with a note once per
			 * shipped version (also the first time, without a record) */
			if (recorded == NULL || strcmp(recorded, shipped_hash) != 0) {
				char *note = g_strdup_printf(
					"%s: kept as you changed it; the version shipped "
					"with erssi is %s", dst_path, shipped_path);
				char **parts = g_strsplit(note, "%", -1);

				/* printed as text: a % in a path is not a colour code */
				default_notes = g_slist_append(default_notes, g_strjoinv("%%", parts));
				g_strfreev(parts);
				g_free(note);
			}
			default_hash_record(name, shipped_hash);
		}
		g_free(current_hash);
		g_free(current);
	}
	g_free(dst_path);
	g_free(shipped_hash);
	g_free(shipped);
}

static void copy_default_files(void)
{
	struct stat statbuf;
	char *themes_dir, *startup_file, *src_path;
	const char *irssi_dir = get_irssi_dir();
	
	/* Only copy files if this is erssi (.erssi directory) */
	if (!g_str_has_suffix(irssi_dir, ".erssi"))
		return;
	default_hashes_load(irssi_dir);
		
	/* Copy themes */
	themes_dir = g_strdup(THEMESDIR);
	if (stat(themes_dir, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
		GDir *dir = g_dir_open(themes_dir, 0, NULL);
		if (dir) {
			const char *filename;
			while ((filename = g_dir_read_name(dir)) != NULL) {
				if (g_str_has_suffix(filename, ".theme")) {
					src_path = g_strdup_printf("%s/%s", themes_dir, filename);
					update_default_file(src_path, irssi_dir, filename, TRUE);
					g_free(src_path);
				}
			}
			g_dir_close(dir);
		}
	}
	g_free(themes_dir);
	
	startup_file = g_strdup_printf("%s/startup", PKGDATADIR);
	update_default_file(startup_file, irssi_dir, "startup", FALSE);
	g_free(startup_file);

	default_hashes_save(irssi_dir);
	/* themes were read already: show a replaced one at once (before
	 * term_init - "theme changed" handlers do not touch the terminal) */
	if (default_themes_changed)
		themes_reload();
}

static void check_files(void)
{
	struct stat statbuf;

	if (stat(get_irssi_dir(), &statbuf) != 0) {
		/* ~/.irssi doesn't exist, first time running irssi */
		display_firsttimer = TRUE;
	}
}

int main(int argc, char **argv)
{
	static int version = 0, update = 0, check_update = 0;
	static GOptionEntry options[] = {
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &version, "Display Irssi version", NULL },
		{ "update", 0, 0, G_OPTION_ARG_NONE, &update,
		  "Update erssi to the newest signed release", NULL },
		{ "check-update", 0, 0, G_OPTION_ARG_NONE, &check_update,
		  "Show whether a newer signed release is available", NULL },
		{ NULL }
	};
	int loglev;

	core_register_options();
	fe_common_core_register_options();
	args_register(options);
	args_execute(argc, argv);

	if (version) {
		printf(PACKAGE_TARNAME " " PACKAGE_VERSION " (%d %04d)\n", IRSSI_VERSION_DATE,
		       IRSSI_VERSION_TIME);
		return 0;
	}

	if (update || check_update) {
		/* the installer of this release, installed with it: it finds the
		 * newest release signed by the key it carries, builds and installs
		 * it into this prefix (or runs the updater named in its config) */
		const char *installer = PKGDATADIR "/shellter-install.sh";

		if (access(installer, R_OK) != 0) {
			fprintf(stderr, "erssi: the installer %s is missing - update with "
			        "shellter-install.sh, the way erssi was installed\n", installer);
			return 1;
		}
		execl("/bin/sh", "sh", installer, "--update", "--prefix", ERSSI_PREFIX,
		      check_update ? "--check" : (char *) NULL, (char *) NULL);
		fprintf(stderr, "erssi: cannot run %s: %s\n", installer, g_strerror(errno));
		return 1;
	}

	srand(time(NULL));

	quitting = FALSE;
	core_preinit(argv[0]);

	check_files();

	/* setlocale() must be called at the beginning before any calls that
	   affect it, especially regexps seem to break if they're generated
	   before this call.

	   locales aren't actually used for anything else than autodetection
	   of UTF-8 currently..

	   furthermore to get the users's charset with g_get_charset() properly
	   you have to call setlocale(LC_ALL, "") */
	setlocale(LC_ALL, "");

	/* Temporarily raise the fatal level to abort on config errors. */
	loglev = critical_fatal_section_begin();
	textui_init();
	
	/* Copy default files (themes, startup) for erssi after directories are created */
	copy_default_files();

	if (!term_init()) {
		fprintf(stderr, "Can't initialize screen handling.\n");
		return 1;
	}

	critical_fatal_section_end(loglev);

	textui_finish_init();
	main_loop = g_main_loop_new(NULL, TRUE);

	/* Does the same as g_main_run(main_loop), except we
	   can call our dirty-checker after each iteration */
	while (!quitting) {
		if (sigterm_received) {
			sigterm_received = FALSE;
			signal_emit("gui exit", 0);
		}

		if (sighup_received) {
			sighup_received = FALSE;

			if (settings_get_bool("quit_on_hup")) {
				signal_emit("gui exit", 0);
			} else {
				signal_emit("command reload", 1, "");
			}
		}

		dirty_check();

		term_refresh_freeze();
		g_main_context_iteration(NULL, TRUE);
		term_refresh_thaw();
	}

	g_main_loop_unref(main_loop);
	textui_deinit();

	session_upgrade(); /* if we /UPGRADEd, start the new process */
	return 0;
}
