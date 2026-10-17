/*
 * The message formats of every theme erssi installs, printed through the
 * theme engine as erssi prints them: each theme (the arguments, from
 * `shipped_themes' of the top-level meson.build) is loaded with
 * theme_load() and the formats get the arguments fe-messages.c,
 * fe-irc-messages.c and fe-ignore.c give them. test-shipped-themes.c checks
 * the files' structure; this checks what they print:
 * - a message printed where its channel is not the active one shows the
 *   channel, and a message to a channel's ops (STATUSMSG) shows @#chan; a
 *   public notice shows its target;
 * - the nick is shown in every combination of nick_hash_color_enabled and
 *   nick_column_enabled, with a mode and without;
 * - with the nick column on, the separator of a theme that has one is right
 *   after the column (nick_column_width cells);
 * - no format uses an argument its module does not pass (ignore_line used
 *   $4 of four arguments, $0-$3).
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/recode.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/utf8.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/fe-expandos.h>
#include <irssi/src/fe-common/core/module-formats.h>
#include <irssi/src/fe-common/core/themes.h>
#include <irssi/src/fe-common/irc/module-formats.h>

#include <string.h>
#include "../../test-home.h"

#define SEPARATOR " │"

static GPtrArray *theme_names;

/* one format of a theme with these arguments, without colours; the nick
 * and mode are those of the message, for $nicktrunc and the like */
static char *render(const char *theme_name, const char *module, int formatnum,
                    const char *nick, const char *mode, const char *const *args)
{
	THEME_REC *theme;
	TEXT_DEST_REC dest;
	char *text, *plain;

	theme = theme_load(theme_name);
	g_assert_nonnull(theme);
	memset(&dest, 0, sizeof(dest));
	dest.level = MSGLEVEL_PUBLIC;

	update_nick_context(nick, mode);
	text = format_get_text_theme_charargs(theme, module, &dest, formatnum, (char **) args);
	clear_nick_context();
	g_assert_nonnull(text);
	plain = strip_codes(text);
	g_free(text);
	return plain;
}

typedef struct {
	const char *what;
	const char *module;
	int formatnum;
	int nick_arg, target_arg, text_arg, mode_arg;
	int params;
	/* the target is checked only in the Shellter themes: the default
	 * notice_public, upstream irssi's since 1.4, leaves it out */
	gboolean target_shellter_only;
} MESSAGE_FORMAT;

/* where fe-messages.c and fe-irc-messages.c put the nick, the target, the
 * text and the mode of each format */
static const MESSAGE_FORMAT CHANNEL_FORMATS[] = {
	{ "own_msg_channel", "fe-common/core", TXT_OWN_MSG_CHANNEL, 0, 1, 2, 3, 4 },
	{ "pubmsg_channel", "fe-common/core", TXT_PUBMSG_CHANNEL, 0, 1, 2, 3, 4 },
	{ "pubmsg_me_channel", "fe-common/core", TXT_PUBMSG_ME_CHANNEL, 0, 1, 2, 3, 4 },
	/* $0 is the hilight colour */
	{ "pubmsg_hilight_channel", "fe-common/core", TXT_PUBMSG_HILIGHT_CHANNEL, 1, 2, 3, 4, 5 },
	{ "notice_public", "fe-common/irc", IRCTXT_NOTICE_PUBLIC, 0, 1, 2, 3, 4, TRUE },
};

static const MESSAGE_FORMAT PLAIN_FORMATS[] = {
	{ "own_msg", "fe-common/core", TXT_OWN_MSG, 0, -1, 1, 2, 3 },
	{ "pubmsg", "fe-common/core", TXT_PUBMSG, 0, -1, 1, 2, 3 },
	{ "pubmsg_me", "fe-common/core", TXT_PUBMSG_ME, 0, -1, 1, 2, 3 },
	{ "pubmsg_hilight", "fe-common/core", TXT_PUBMSG_HILIGHT, 1, -1, 2, 3, 4 },
};

static char *render_message(const char *theme_name, const MESSAGE_FORMAT *f, const char *nick,
                            const char *target, const char *mode)
{
	const char *args[MAX_FORMAT_PARAMS + 1] = { NULL };
	int i;

	for (i = 0; i < f->params; i++)
		args[i] = "";
	args[f->nick_arg] = nick;
	if (f->target_arg >= 0)
		args[f->target_arg] = target;
	args[f->text_arg] = "hello world";
	args[f->mode_arg] = mode;
	return render(theme_name, f->module, f->formatnum, nick, mode, args);
}

static void set_nick_settings(gboolean hash, gboolean column)
{
	settings_set_bool("nick_hash_color_enabled", hash);
	settings_set_bool("nick_column_enabled", column);
	settings_set_int("nick_column_width", 13);
}

/* the cells before the separator, or -1 when the line has none */
static int separator_column(const char *line)
{
	const char *sep = strstr(line, SEPARATOR);
	char *before;
	int cells;

	if (sep == NULL)
		return -1;
	before = g_strndup(line, sep - line);
	cells = string_width(before, TREAT_STRING_AS_UTF8);
	g_free(before);
	return cells;
}

/* the channel and its STATUSMSG prefix are shown */
static void test_channel_target(gconstpointer data)
{
	const char *theme_name = data;
	static const char *const targets[] = { "#chan", "@#chan", "+#chan" };
	int i, t, hash, column;

	for (hash = 0; hash < 2; hash++) {
		for (column = 0; column < 2; column++) {
			set_nick_settings(hash, column);
			for (i = 0; i < (int) G_N_ELEMENTS(CHANNEL_FORMATS); i++) {
				for (t = 0; t < (int) G_N_ELEMENTS(targets); t++) {
					const MESSAGE_FORMAT *f = &CHANNEL_FORMATS[i];
					gboolean check_target;
					char *line;
					int sep;

					check_target = !f->target_shellter_only ||
					               g_str_has_prefix(theme_name, "shellter");
					line = render_message(theme_name, f, "alice", targets[t], "@");
					if ((check_target && strstr(line, targets[t]) == NULL) ||
					    strstr(line, "alice") == NULL ||
					    strstr(line, "hello world") == NULL)
						g_test_message("%s, %s to %s: \"%s\"", theme_name, f->what,
						               targets[t], line);
					if (check_target)
						g_assert_nonnull(strstr(line, targets[t]));
					g_assert_nonnull(strstr(line, "alice"));
					g_assert_nonnull(strstr(line, "hello world"));
					/* the target is not in the nick column */
					sep = separator_column(line);
					if (sep >= 0 && column) {
						g_assert_cmpint(sep, ==, 13);
						g_assert_true(strstr(line, targets[t]) >
						              strstr(line, SEPARATOR));
					}
					g_free(line);
				}
			}
		}
	}
	set_nick_settings(TRUE, TRUE);
}

/* the nick and its mode in every combination of the nick settings */
static void test_nick_shown(gconstpointer data)
{
	const char *theme_name = data;
	static const char *const modes[] = { "@", "+", "" };
	int i, m, hash, column;

	for (hash = 0; hash < 2; hash++) {
		for (column = 0; column < 2; column++) {
			set_nick_settings(hash, column);
			for (m = 0; m < (int) G_N_ELEMENTS(modes); m++) {
				for (i = 0; i < (int) G_N_ELEMENTS(PLAIN_FORMATS) +
				                    (int) G_N_ELEMENTS(CHANNEL_FORMATS);
				     i++) {
					const MESSAGE_FORMAT *f =
					    i < (int) G_N_ELEMENTS(PLAIN_FORMATS) ?
					        &PLAIN_FORMATS[i] :
					        &CHANNEL_FORMATS[i - G_N_ELEMENTS(PLAIN_FORMATS)];
					char *line, *with_mode;
					int sep;

					line = render_message(theme_name, f, "alice", "#chan", modes[m]);
					/* a notice of the Shellter themes shows the nick
					 * without its mode */
					with_mode = g_strconcat(strcmp(f->module, "fe-common/irc") == 0 ?
					                            "" :
					                            modes[m],
					                        "alice", NULL);
					if (strstr(line, with_mode) == NULL)
						g_test_message("%s, %s, hash %d, column %d, mode \"%s\": "
						               "\"%s\"",
						               theme_name, f->what, hash, column, modes[m],
						               line);
					g_assert_nonnull(strstr(line, with_mode));
					sep = separator_column(line);
					if (sep >= 0 && column)
						g_assert_cmpint(sep, ==, 13);
					g_free(with_mode);
					g_free(line);
				}
			}
		}
	}
	set_nick_settings(TRUE, TRUE);
}

/* the highest argument a format refers to ($3, $[-4]3, ${3}, $1-3), -1
 * for none; expandos ($nicktrunc, $N) do not count */
static int highest_argument(const char *format)
{
	const char *p;
	int max = -1;

	for (p = format; *p != '\0'; p++) {
		if (*p == '%' && p[1] != '\0') {
			p++;
			continue;
		}
		if (*p != '$')
			continue;
		p++;
		if (*p == '$')
			continue;
		if (*p == '[') {
			while (*p != '\0' && *p != ']')
				p++;
			if (*p == '\0')
				break;
			p++;
		}
		if (*p == '{' && g_ascii_isdigit(p[1]) && p[2] == '}') {
			max = MAX(max, p[1] - '0');
			p += 2;
		} else if (g_ascii_isdigit(*p)) {
			max = MAX(max, *p - '0');
			if (p[1] == '-' && g_ascii_isdigit(p[2])) {
				max = MAX(max, p[2] - '0');
				p += 2;
			}
		} else {
			p--;
		}
	}
	return max;
}

typedef struct {
	const char *theme;
	GString *errors;
} ARG_CHECK;

static void check_module_args(const char *module, FORMAT_REC *formats, ARG_CHECK *check)
{
	THEME_REC *theme;
	MODULE_THEME_REC *rec;
	int i;

	theme = theme_load(check->theme);
	rec = g_hash_table_lookup(theme->modules, module);
	if (rec == NULL)
		return;
	/* formats[0] is the module, a tag NULL a section heading */
	for (i = 1; i < rec->count; i++) {
		int max;

		if (formats[i].tag == NULL || formats[i].def == NULL ||
		    rec->expanded_formats[i] == NULL)
			continue;
		max = highest_argument(rec->expanded_formats[i]);
		if (max >= formats[i].params)
			g_string_append_printf(check->errors,
			                       "%s: %s uses $%d, the format has %d arguments: %s\n",
			                       module, formats[i].tag, max, formats[i].params,
			                       rec->expanded_formats[i]);
	}
}

/* no format of the theme, or default format of a module, refers to an
 * argument it is not given */
static void test_arguments(gconstpointer data)
{
	ARG_CHECK check = { data, g_string_new(NULL) };

	g_hash_table_foreach(default_formats, (GHFunc) check_module_args, &check);
	if (check.errors->len > 0)
		g_test_message("%s:\n%s", check.theme, check.errors->str);
	g_assert_cmpstr(check.errors->str, ==, "");
	g_string_free(check.errors, TRUE);
}

/* the parser of the check above */
static void test_highest_argument(void)
{
	g_assert_cmpint(highest_argument("plain"), ==, -1);
	g_assert_cmpint(highest_argument("$nicktrunc $N $* $$5 %$7"), ==, -1);
	g_assert_cmpint(highest_argument("$0 $1"), ==, 1);
	g_assert_cmpint(highest_argument("%#$[-4]0 $1: $2 $3 $4"), ==, 4);
	g_assert_cmpint(highest_argument("x${3}y"), ==, 3);
	g_assert_cmpint(highest_argument("$1-"), ==, 1);
	g_assert_cmpint(highest_argument("$1-5"), ==, 5);
	g_assert_cmpint(highest_argument("$[!10]2"), ==, 2);
}

/* the themes from the command line, copied into the test home, where
 * theme_load() finds them before the installed ones */
static void install_themes(int argc, char **argv)
{
	int i;

	theme_names = g_ptr_array_new_with_free_func(g_free);
	for (i = 1; i < argc; i++) {
		char *base = g_path_get_basename(argv[i]);
		char *dest = g_build_filename(test_home_dir, base, NULL);
		char *contents;
		gsize len;

		g_assert_true(g_file_get_contents(argv[i], &contents, &len, NULL));
		g_assert_true(g_file_set_contents(dest, contents, len, NULL));
		g_assert_true(g_str_has_suffix(base, ".theme"));
		base[strlen(base) - strlen(".theme")] = '\0';
		g_ptr_array_add(theme_names, base);
		g_free(contents);
		g_free(dest);
	}
}

int main(int argc, char **argv)
{
	guint i;

	g_test_init(&argc, &argv, NULL);
	/* report every theme and format that fails, not only the first one */
	g_test_set_nonfatal_assertions();
	g_assert_cmpint(argc, >, 1);

	test_home_setup();
	install_themes(argc, argv);
	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	args_execute(0, NULL);
	core_init();
	fe_common_core_init();
	theme_register_module("fe-common/irc", fecommon_irc_formats);
	signal_emit("irssi init finished", 0);
	settings_set_str("term_charset", "UTF-8");
	recode_update_charset();

	g_test_add_func("/theme_formats/highest_argument", test_highest_argument);
	for (i = 0; i < theme_names->len; i++) {
		const char *name = theme_names->pdata[i];
		char *path;

		path = g_strdup_printf("/theme_formats/%s/channel_target", name);
		g_test_add_data_func(path, name, test_channel_target);
		g_free(path);
		path = g_strdup_printf("/theme_formats/%s/nick_shown", name);
		g_test_add_data_func(path, name, test_nick_shown);
		g_free(path);
		path = g_strdup_printf("/theme_formats/%s/arguments", name);
		g_test_add_data_func(path, name, test_arguments);
		g_free(path);
	}

	return g_test_run();
}
