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
#include <unistd.h>
#include <irssi/src/fe-ansi/module-formats.h>
#include <irssi/src/fe-ansi/default-files.h>
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
#include <irssi/src/fe-common/core/firstrun.h>
#include <irssi/src/fe-common/core/fe-settings.h>
#include <irssi/src/fe-common/core/shellter-defaults.h>
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
#include <sys/stat.h>

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

void gui_notify_init(void);
void gui_notify_deinit(void);

static int dirty, full_redraw;

static GMainLoop *main_loop;
int quitting;

static int display_firsttimer = FALSE;
static GSList *default_notes;	/* about ~/.erssi files, printed after the banner */
static unsigned int user_settings_changed = 0;
static gboolean config_existed;	/* the config file was there when erssi started */
static gboolean firstrun_ran;	/* the first-run setup was asked this start */

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
	gui_notify_init();
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

	/* a later start that kept the nick of the built-in default config */
	if (!firstrun_ran && config_existed) {
		char *shipped = settings_get_builtin_str("core", "nick");

		if (shipped != NULL && *shipped != '\0' &&
		    g_strcmp0(settings_get_str("nick"), shipped) == 0)
			printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE | MSGLEVEL_NO_ACT,
			          "Your nick is still %s: /SET nick <nick>, or run erssi --setup.",
			          shipped);
		g_free(shipped);
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
	gui_notify_deinit();
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

/* Files erssi ships into ~/.erssi (themes, startup): see default-files.h */
static void copy_default_files(void)
{
	struct stat statbuf;
	char *themes_dir, *startup_file, *src_path;
	const char *irssi_dir = get_irssi_dir();
	DEFAULT_FILES_REC *rec;
	gboolean themes_changed;
	
	/* Only copy files if this is erssi (.erssi directory) */
	if (!g_str_has_suffix(irssi_dir, ".erssi"))
		return;
	rec = default_files_open(irssi_dir, known_default_files);
		
	/* Copy themes */
	themes_dir = g_strdup(THEMESDIR);
	if (stat(themes_dir, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
		GDir *dir = g_dir_open(themes_dir, 0, NULL);
		if (dir) {
			const char *filename;
			while ((filename = g_dir_read_name(dir)) != NULL) {
				if (g_str_has_suffix(filename, ".theme")) {
					src_path = g_strdup_printf("%s/%s", themes_dir, filename);
					default_files_update(rec, src_path, filename, TRUE);
					g_free(src_path);
				}
			}
			g_dir_close(dir);
		}
	}
	g_free(themes_dir);
	
	startup_file = g_strdup_printf("%s/startup", PKGDATADIR);
	default_files_update(rec, startup_file, "startup", FALSE);
	g_free(startup_file);

	/* the notes are printed after the banner */
	default_notes = g_slist_concat(default_notes, rec->notes);
	rec->notes = NULL;
	themes_changed = rec->themes_changed;
	default_files_close(rec);
	/* themes were read already: show a replaced one at once (before
	 * term_init - "theme changed" handlers do not touch the terminal) */
	if (themes_changed)
		themes_reload();
}

/* each answer waits at most 5 minutes; no answer = skipped */
#define FIRSTRUN_ANSWER_TIMEOUT_MS 300000

/* SIGINT, SIGQUIT, SIGTERM and SIGHUP during the setup end erssi without
 * saving anything. The handler only records the signal: the read that is
 * waiting returns (EINTR), or sees the flag (firstrun_set_abort_flag) before
 * it starts, between bytes and after a resume, and restores the terminal. */
static const int firstrun_signals[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP };
static volatile sig_atomic_t firstrun_signal;
static gboolean firstrun_last_secret; /* a secret read ends with its own newline */

static void sig_firstrun(int signo)
{
	firstrun_signal = signo;
}

static char *firstrun_read_stdin(void *ctx, gboolean secret)
{
	firstrun_last_secret = secret;
	return firstrun_read_fd(STDIN_FILENO, FIRSTRUN_ANSWER_TIMEOUT_MS, secret);
}

static gint firstrun_theme_cmp(gconstpointer a, gconstpointer b)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* adds the names of the *.theme files in DIR that LIST does not have yet; no dotfiles,
 * only regular files (or links to them), only names that are printable */
static void firstrun_add_themes(GPtrArray *list, const char *dir)
{
	const char *fname;
	GDir *d;
	guint i;

	if (dir == NULL || (d = g_dir_open(dir, 0, NULL)) == NULL)
		return;
	while ((fname = g_dir_read_name(d)) != NULL) {
		size_t len = strlen(fname);
		gboolean dup = FALSE, regular;
		char *name, *path;

		if (len <= 6 || *fname == '.' || !g_str_has_suffix(fname, ".theme") ||
		    !firstrun_printable(fname))
			continue;
		path = g_build_filename(dir, fname, NULL);
		regular = g_file_test(path, G_FILE_TEST_IS_REGULAR);
		g_free(path);
		if (!regular)
			continue;
		name = g_strndup(fname, len - 6);
		for (i = 0; i < list->len && !dup; i++)
			dup = strcmp(g_ptr_array_index(list, i), name) == 0;
		if (dup)
			g_free(name);
		else
			g_ptr_array_add(list, name);
	}
	g_dir_close(d);
}

/* The first-run questions on the terminal, before the screen is set up
 * (stdio, cooked tty). Nothing here goes through printtext, so nothing
 * reaches the logs. */
static void firstrun_run_tty(void)
{
	struct sigaction act, old[G_N_ELEMENTS(firstrun_signals)];
	FIRSTRUN_ANSWERS a;
	FIRSTRUN_RESULT result;
	GPtrArray *networks, *theme_list;
	char **themes;
	char *current_theme;
	guint i;

	if (config_last_error(mainconfig) != NULL) {
		/* the file was read only in part: saving would write the
		   mis-parsed tree over it */
		printf("erssi: %s has errors - fix it first, the setup did not change it.\n",
		       mainconfig->fname);
		fflush(stdout);
		return;
	}

	networks = firstrun_networks(mainconfig);
	theme_list = g_ptr_array_new();
	firstrun_add_themes(theme_list, get_irssi_dir());
	firstrun_add_themes(theme_list, THEMESDIR);
	g_ptr_array_sort(theme_list, firstrun_theme_cmp);
	g_ptr_array_add(theme_list, NULL);
	themes = (char **) g_ptr_array_free(theme_list, FALSE);
	current_theme = g_strdup(settings_get_str("theme"));

	/* no SA_RESTART: the signal interrupts the read */
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = sig_firstrun;
	firstrun_signal = 0;
	for (i = 0; i < G_N_ELEMENTS(firstrun_signals); i++) {
		sigaction(firstrun_signals[i], NULL, &old[i]);
		if (old[i].sa_handler != SIG_IGN)
			sigaction(firstrun_signals[i], &act, NULL);
	}

	firstrun_ran = TRUE;
	firstrun_set_abort_flag(&firstrun_signal);
	result = firstrun_ask(stdout, firstrun_read_stdin, NULL, networks, themes,
	                      settings_get_str("nick"), current_theme, &a);
	firstrun_set_abort_flag(NULL);

	for (i = 0; i < G_N_ELEMENTS(firstrun_signals); i++)
		sigaction(firstrun_signals[i], &old[i], NULL);

	if (firstrun_signal != 0) {
		int signo = firstrun_signal;

		/* interrupted: nothing is saved, the next start asks again */
		firstrun_answers_clear(&a);
		if (!firstrun_last_secret)
			fputs("\n", stdout); /* after the ^C the terminal echoed */
		fflush(stdout);
		signal(signo, SIG_DFL);
		raise(signo);
		_exit(128 + signo);
	}

	if (result == FIRSTRUN_DONE) {
		firstrun_apply_config(mainconfig, &a);
		if (a.nick != NULL)
			settings_set_str("nick", a.nick);
		if (a.theme != NULL && g_strcmp0(a.theme, current_theme) != 0) {
			settings_set_str("theme", a.theme);
			themes_reload();
		}
	}
	firstrun_answers_clear(&a);

	if (!settings_save(NULL, FALSE))
		printf("Could not save %s: erssi --setup asks again.\n", mainconfig->fname);
	else if (result == FIRSTRUN_DONE)
		printf("Saved to %s. Later: /NETWORK, /SET nick, /SET theme, or erssi --setup.\n",
		       mainconfig->fname);
	else
		printf("Skipped. erssi --setup asks again.\n");
	fflush(stdout);

	g_free(current_theme);
	g_strfreev(themes);
	g_ptr_array_free(networks, TRUE);
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
	static int version = 0, update = 0, check_update = 0, setup = 0;
	static GOptionEntry options[] = {
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &version, "Display Irssi version", NULL },
		{ "update", 0, 0, G_OPTION_ARG_NONE, &update,
		  "Update erssi to the newest signed release", NULL },
		{ "check-update", 0, 0, G_OPTION_ARG_NONE, &check_update,
		  "Show whether a newer signed release is available", NULL },
		{ "setup", 0, 0, G_OPTION_ARG_NONE, &setup, "Ask the first-run setup questions",
		  NULL },
		{ NULL }
	};
	struct stat statbuf;
	char *note, *home_config;
	gboolean tty, scripted;
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

	tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
	      g_strcmp0(g_getenv("TERM"), "dumb") != 0;
	if (setup && !tty) {
		fprintf(stderr, "erssi: --setup needs a terminal\n");
		return 1;
	}

	srand(time(NULL));

	quitting = FALSE;
	core_preinit(argv[0]);
	config_existed = stat(get_irssi_config(), &statbuf) == 0;

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
	note = shellter_defaults_note();
	if (note != NULL)
		default_notes = g_slist_append(default_notes, note);
	
	/* Copy default files (themes, startup) for erssi after directories are created */
	copy_default_files();

	/* the first-run setup, on a terminal, before the screen is set up */
	home_config = g_strdup_printf("%s/" IRSSI_HOME_CONFIG, get_irssi_dir());
	scripted = fe_common_core_scripted_start() ||
	           g_strcmp0(get_irssi_config(), home_config) != 0;
	g_free(home_config);
	if (firstrun_wanted(config_existed, tty, setup, scripted))
		firstrun_run_tty();

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
