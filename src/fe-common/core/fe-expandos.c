/*
 fe-expandos.c : irssi

    Copyright (C) 2000 Timo Sirainen

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
    
    ---
    
    Modifications for erssi:
    Copyright (C) 2024-2025 erssi-org team
    Lead Developer: Jerzy (kofany) Dąbrowski <https://github.com/kofany>
*/

#include "module.h"
#include <irssi/src/fe-common/core/fe-expandos.h>
#include <irssi/src/core/expandos.h>
#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/commands.h>
#include <irssi/src/core/recode.h>
#include <irssi/src/core/utf8.h>
#include <time.h>

/* Nick column context variables */
static char *current_nick = NULL;
static char *current_mode = NULL;
static gboolean nick_context_valid = FALSE;

/* Hash-based nick coloring system */
typedef struct {
	char *nick;
	int color_index;
	time_t last_seen;
} nick_color_entry;

typedef struct {
	GHashTable *nick_colors; /* nick -> nick_color_entry */
	char *channel_name;
	char *server_tag;
} channel_color_context;

static GHashTable *channel_contexts = NULL; /* "server:channel" -> channel_color_context */

/* Window ref# */
static char *expando_winref(SERVER_REC *server, void *item, int *free_ret)
{
	if (active_win == NULL)
		return "";

	*free_ret = TRUE;
	return g_strdup_printf("%d", active_win->refnum);
}

/* Window name */
static char *expando_winname(SERVER_REC *server, void *item, int *free_ret)
{
	if (active_win == NULL)
		return "";

	return active_win->name;
}

/* One character of a nick and the cells it takes, as the text view draws
 * it (textbuffer_read_unichar() in fe-ansi): in a UTF-8 terminal a
 * character with its combining marks, of its width (2 for CJK), and a byte
 * that is not UTF-8 as one cell (the view shows U+FFFD); in another
 * terminal one byte, one cell. Moves *p past it. */
static int nick_advance(const char **p, gboolean utf8)
{
	if (utf8 && (g_utf8_get_char_validated(*p, -1) & 0x80000000) == 0)
		return string_advance(p, TREAT_STRING_AS_UTF8);
	(*p)++;
	return 1;
}

static int nick_cells(const char *str, gboolean utf8)
{
	int cells = 0;

	while (*str != '\0')
		cells += nick_advance(&str, utf8);
	return cells;
}

/* The cells of the mode before the nick: one even without a mode, as
 * $nickmode gives a space then. */
static int nick_mode_cells(void)
{
	int cells;

	cells = current_mode == NULL ? 0 : nick_cells(current_mode, is_utf8());
	return cells > 0 ? cells : 1;
}

/* The nick as the nick column shows it: when the mode and the nick are
 * wider than nick_column_width, the nick is cut to what fits with a "+"
 * after it. Widths are terminal cells and the cut never splits a
 * character, so a wide character that does not fit is left out whole and
 * the column is one cell narrower (*cells says so, $nickalign pads it).
 * Returns NULL when the nick fits as it is. */
static char *nick_column_cut(int *cells)
{
	gboolean utf8 = is_utf8();
	const char *p, *next;
	int room, used, w;

	*cells = nick_cells(current_nick, utf8);
	room = settings_get_int("nick_column_width") - nick_mode_cells();
	if (*cells <= room)
		return NULL;

	room--; /* for the "+" */
	used = 0;
	for (p = current_nick; *p != '\0'; p = next) {
		next = p;
		w = nick_advance(&next, utf8);
		if (used + w > room)
			break;
		used += w;
	}
	*cells = used + 1;
	return g_strdup_printf("%.*s+", (int) (p - current_nick), current_nick);
}

/* Nick column align - returns only padding spaces */
static char *expando_nickalign(SERVER_REC *server, void *item, int *free_ret)
{
	int padding, cells;

	if (!settings_get_bool("nick_column_enabled") || !nick_context_valid ||
	    current_nick == NULL)
		return "";

	g_free(nick_column_cut(&cells));
	padding = settings_get_int("nick_column_width") - nick_mode_cells() - cells;
	if (padding <= 0)
		return "";

	*free_ret = TRUE;
	return g_strnfill(padding, ' ');
}

/* The nick of the message, cut to the nick column when it is on */
static char *expando_nicktrunc(SERVER_REC *server, void *item, int *free_ret)
{
	char *cut;
	int cells;

	if (!nick_context_valid || current_nick == NULL)
		return "";
	if (!settings_get_bool("nick_column_enabled"))
		return current_nick;

	cut = nick_column_cut(&cells);
	if (cut == NULL)
		return current_nick;
	*free_ret = TRUE;
	return cut;
}

/* Update nick context for expandos */
void update_nick_context(const char *nick, const char *mode)
{
	g_free(current_nick);
	g_free(current_mode);
	current_nick = g_strdup(nick);
	current_mode = g_strdup(mode ? mode : "");
	nick_context_valid = TRUE;
}

/* Clear nick context */
void clear_nick_context(void)
{
	nick_context_valid = FALSE;
}

/* Hash-based nick coloring functions */

static void free_nick_color_entry(nick_color_entry *entry)
{
	if (entry) {
		g_free(entry->nick);
		g_free(entry);
	}
}

static void free_channel_color_context(channel_color_context *ctx)
{
	if (ctx) {
		if (ctx->nick_colors) {
			g_hash_table_destroy(ctx->nick_colors);
		}
		g_free(ctx->channel_name);
		g_free(ctx->server_tag);
		g_free(ctx);
	}
}

/* Normalize one nick_hash_colors entry to the code that follows '%' in a
 * format string, or NULL when it is not a colour. Accepted forms:
 *   g  G  %G           - the 16 classic colours (krgybmcw / KRGYBMCW)
 *   #f59e0b  Zf59e0b   - 24-bit foreground (written as %Zf59e0b)
 *   %Zf59e0b
 */
char *nick_palette_entry_normalize(const char *entry)
{
	const char *p = entry;
	int i;

	if (p == NULL)
		return NULL;
	if (*p == '%')
		p++;
	if (strlen(p) == 1 && strchr("krgybmcwKRGYBMCW", *p) != NULL)
		return g_strdup(p);
	if ((*p == '#' || *p == 'Z') && strlen(p) == 7) {
		for (i = 1; i < 7; i++) {
			if (!g_ascii_isxdigit(p[i]))
				return NULL;
		}
		return g_strdup_printf("Z%s", p + 1);
	}
	return NULL;
}

/* Parse nick_hash_colors into a NULL-terminated array holding ONLY valid,
 * normalized entries, so palette[index] is always a usable colour for every
 * index < *count. (Previously the array kept invalid tokens and empty
 * strings from double spaces while *count counted only valid ones, so a
 * hash could land on junk.) Falls back to the default palette. */
gchar **parse_color_palette(const char *colors_str, int *count)
{
	GPtrArray *valid;
	gchar **tokens;
	int i;

	valid = g_ptr_array_new();
	tokens = g_strsplit(colors_str != NULL ? colors_str : "", " ", -1);
	for (i = 0; tokens[i] != NULL; i++) {
		char *code = nick_palette_entry_normalize(tokens[i]);
		if (code != NULL)
			g_ptr_array_add(valid, code);
	}
	g_strfreev(tokens);

	if (valid->len == 0) {
		g_ptr_array_free(valid, TRUE);
		*count = 8;
		return g_strsplit("g r b m c y G C", " ", -1);
	}

	*count = valid->len;
	g_ptr_array_add(valid, NULL);
	return (gchar **) g_ptr_array_free(valid, FALSE);
}

static int hash_nick_to_color_index(const char *nick, const char *channel_key, int palette_size)
{
	char *hash_input;
	unsigned int hash;
	int result;
	
	hash_input = g_strdup_printf("%s:%s", channel_key, nick);
	hash = g_str_hash(hash_input);
	result = hash % palette_size;
	g_free(hash_input);
	return result;
}

static int generate_random_color_index(int old_color, int palette_size)
{
	int new_color;
	
	if (palette_size <= 1)
		return old_color;
	
	/* Generate random color different from old_color */
	do {
		new_color = (time(NULL) + rand()) % palette_size;
	} while (new_color == old_color);
	
	return new_color;
}

static int get_persistent_nick_color(const char *nick, void *item, SERVER_REC *server)
{
	WI_ITEM_REC *witem;
	const char *channel;
	const char *server_tag;
	int palette_size;
	gchar **palette;
	char *channel_key;
	channel_color_context *ctx;
	nick_color_entry *entry;
	int color_index;
	
	
	witem = (WI_ITEM_REC *)item;
	channel = witem ? witem->visible_name : "query";
	server_tag = server ? server->tag : "unknown";
	
	/* Parse color palette */
	palette = parse_color_palette(settings_get_str("nick_hash_colors"), &palette_size);
	
	/* Create channel key */
	channel_key = g_strdup_printf("%s:%s", server_tag, channel);
	
	/* Initialize channel_contexts if needed */
	if (!channel_contexts) {
		channel_contexts = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                         g_free, (GDestroyNotify)free_channel_color_context);
	}
	
	/* Get or create channel context */
	ctx = g_hash_table_lookup(channel_contexts, channel_key);
	if (!ctx) {
		ctx = g_new0(channel_color_context, 1);
		ctx->channel_name = g_strdup(channel);
		ctx->server_tag = g_strdup(server_tag);
		ctx->nick_colors = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                         g_free, (GDestroyNotify)free_nick_color_entry);
		g_hash_table_insert(channel_contexts, g_strdup(channel_key), ctx);
	}
	
	/* Get or create nick color entry */
	entry = g_hash_table_lookup(ctx->nick_colors, nick);
	if (!entry) {
		/* Nick not found - use hash color for consistency */
		entry = g_new0(nick_color_entry, 1);
		entry->nick = g_strdup(nick);
		
		/* Use hash-based color for new nicks */
		entry->color_index = hash_nick_to_color_index(nick, channel_key, palette_size);
		entry->last_seen = time(NULL);
		g_hash_table_insert(ctx->nick_colors, g_strdup(nick), entry);
	} else {
		entry->last_seen = time(NULL);
		/* the palette may have shrunk since this colour was picked */
		if (entry->color_index < 0 || entry->color_index >= palette_size)
			entry->color_index = hash_nick_to_color_index(nick, channel_key, palette_size);
	}
	
	color_index = entry->color_index;
	g_free(channel_key);
	g_strfreev(palette);
	return color_index;
}

/* The nick of the message in its hash colour, cut to the nick column when
 * that is on (as $nicktrunc). With nick_hash_color_enabled off it is the
 * same nick without a colour: a theme that shows the nick with
 * $nickcolored must not lose it when the colouring is switched off. */
static char *expando_nickcolored(SERVER_REC *server, void *item, int *free_ret)
{
	char *display_nick;
	int nick_free = FALSE;
	int palette_size;
	gchar **palette;
	int color_index;
	GString *raw;
	const char *p;
	char *result;

	if (!nick_context_valid || current_nick == NULL)
		return "";

	display_nick = expando_nicktrunc(server, item, &nick_free);
	if (!settings_get_bool("nick_hash_color_enabled")) {
		*free_ret = nick_free;
		return display_nick;
	}

	palette = parse_color_palette(settings_get_str("nick_hash_colors"), &palette_size);
	color_index = get_persistent_nick_color(current_nick, item, server);
	if (color_index < 0 || color_index >= palette_size) {
		g_strfreev(palette);
		*free_ret = nick_free;
		return display_nick;
	}

	raw = g_string_new(NULL);
	g_string_append_printf(raw, "%%%s%%_", palette[color_index]);
	for (p = display_nick; *p != '\0'; p++) {
		if (*p == '%')
			g_string_append_c(raw, '%'); /* literal %, not a format code */
		g_string_append_c(raw, *p);
	}
	g_string_append(raw, "%_%n");
	result = format_string_expand(raw->str, NULL);

	g_string_free(raw, TRUE);
	g_strfreev(palette);
	if (nick_free)
		g_free(display_nick);
	*free_ret = TRUE;
	return result;
}

/* Nick mode prefix colored by rank: nick_mode_color_owner (~ &), _op (@),
 * _halfop (%), _voice (+), _normal (no mode). The mode comes from the
 * message context like $nickalign; a nick without a mode gets one space, so
 * the nick column keeps its width. */
static char *expando_nickmode(SERVER_REC *server, void *item, int *free_ret)
{
	const char *mode, *setting, *p;
	GString *raw;
	char *result;

	if (!nick_context_valid || current_nick == NULL)
		return "";

	mode = current_mode != NULL && *current_mode != '\0' ? current_mode : " ";
	switch (*mode) {
	case '~':
	case '&':
		setting = "nick_mode_color_owner";
		break;
	case '@':
		setting = "nick_mode_color_op";
		break;
	case '%':
		setting = "nick_mode_color_halfop";
		break;
	case '+':
		setting = "nick_mode_color_voice";
		break;
	default:
		setting = "nick_mode_color_normal";
		break;
	}

	raw = g_string_new(settings_get_str(setting));
	for (p = mode; *p != '\0'; p++) {
		if (*p == '%')
			g_string_append_c(raw, '%'); /* literal %, not a format code */
		g_string_append_c(raw, *p);
	}
	g_string_append(raw, "%n");

	*free_ret = TRUE;
	result = format_string_expand(raw->str, NULL);
	g_string_free(raw, TRUE);
	return result;
}

/* Reset event parser and helper functions */

static gboolean should_reset_on_event(const char *event)
{
	const char *setting;
	
	setting = settings_get_str("nick_hash_reset_event");
	if (!setting || !*setting)
		setting = "quit";
	
	return strstr(setting, event) != NULL;
}

static void reset_nick_color(const char *server_tag, const char *channel, const char *nick)
{
	char *channel_key;
	channel_color_context *ctx;
	nick_color_entry *entry;
	int old_color, new_color, palette_size;
	gchar **palette;
	
	if (!channel_contexts)
		return;
		
	channel_key = g_strdup_printf("%s:%s", server_tag, channel);
	ctx = g_hash_table_lookup(channel_contexts, channel_key);
	
	if (!ctx || !ctx->nick_colors) {
		g_free(channel_key);
		return;
	}
	
	entry = g_hash_table_lookup(ctx->nick_colors, nick);
	if (!entry) {
		g_free(channel_key);
		return;
	}
	
	/* Get palette and generate new color different from current */
	palette = parse_color_palette(settings_get_str("nick_hash_colors"), &palette_size);
	old_color = entry->color_index;
	new_color = generate_random_color_index(old_color, palette_size);
	
	entry->color_index = new_color;
	entry->last_seen = time(NULL);
	
	g_strfreev(palette);
	g_free(channel_key);
}

static void reset_nick_on_server(const char *server_tag, const char *nick)
{
	GHashTableIter iter;
	gpointer key, value;
	channel_color_context *ctx;
	nick_color_entry *entry;
	char *server_prefix;
	int old_color, new_color, palette_size;
	gchar **palette;
	
	if (!channel_contexts)
		return;
		
	/* Get palette for color generation */
	palette = parse_color_palette(settings_get_str("nick_hash_colors"), &palette_size);
	server_prefix = g_strdup_printf("%s:", server_tag);
	
	
	g_hash_table_iter_init(&iter, channel_contexts);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (g_str_has_prefix((char*)key, server_prefix)) {
			ctx = (channel_color_context*)value;
			if (ctx && ctx->nick_colors) {
				entry = g_hash_table_lookup(ctx->nick_colors, nick);
				if (entry) {
					old_color = entry->color_index;
					new_color = generate_random_color_index(old_color, palette_size);
					entry->color_index = new_color;
					entry->last_seen = time(NULL);
				}
			}
		}
	}
	
	g_strfreev(palette);
	g_free(server_prefix);
}


/* Signal handlers for cleanup events */

static void cleanup_nick_on_quit(SERVER_REC *server, const char *nick, const char *address, const char *reason)
{
	if (!should_reset_on_event("quit") || !server || !nick)
		return;
		
	reset_nick_on_server(server->tag, nick);
}

static void cleanup_nick_on_part(SERVER_REC *server, const char *channel, const char *nick, const char *address, const char *reason)
{
	if (!should_reset_on_event("part") || !server || !channel || !nick)
		return;
		
	reset_nick_color(server->tag, channel, nick);
}

static void cleanup_nick_on_nickchange(SERVER_REC *server, const char *new_nick, const char *old_nick, const char *address)
{
	if (!should_reset_on_event("nickchange") || !server || !old_nick)
		return;
		
	reset_nick_on_server(server->tag, old_nick);
}

/* /nickhash command handler */
/* SYNTAX: NICKHASH SHIFT <channel> <nick> */

static void cmd_nickhash(const char *data, SERVER_REC *server, WI_ITEM_REC *item)
{
	char **params;
	char *subcmd, *channel, *nick;
	
	if (!data || !*data) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTCRAP, "Usage: /nickhash shift <#channel> <nick>");
		return;
	}
	
	params = g_strsplit(data, " ", 3);
	if (!params[0]) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTCRAP, "Usage: /nickhash shift <#channel> <nick>");
		g_strfreev(params);
		return;
	}
	
	subcmd = g_strstrip(params[0]);
	
	if (g_ascii_strcasecmp(subcmd, "shift") == 0) {
		if (!params[1] || !params[2]) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTCRAP, "Usage: /nickhash shift <#channel> <nick>");
			g_strfreev(params);
			return;
		}
		
		channel = g_strstrip(params[1]);
		nick = g_strstrip(params[2]);
		
		if (!server) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTCRAP, "Not connected to server");
			g_strfreev(params);
			return;
		}
		
		/* DON'T remove # prefix - keep it consistent with get_persistent_nick_color */
			
		reset_nick_color(server->tag, channel, nick);
		printtext(NULL, NULL, MSGLEVEL_CLIENTCRAP, "Shifted color for nick %s in %s", nick, channel);
		
	} else {
		printtext(NULL, NULL, MSGLEVEL_CLIENTCRAP, "Unknown subcommand: %s", subcmd);
	}
	
	g_strfreev(params);
}

/* Nick colours are kept per channel so a nick keeps its colour; nicks not
   seen for a week (and channels left with none) are forgotten, or the
   tables grow for as long as erssi runs. */
#define NICK_COLOR_KEEP (7 * 24 * 3600)
static guint nick_color_prune_tag = 0;

static gboolean nick_color_entry_old(gpointer key, gpointer value, gpointer cutoff)
{
	return ((nick_color_entry *) value)->last_seen < *(time_t *) cutoff;
}

static gboolean channel_context_prune(gpointer key, gpointer value, gpointer cutoff)
{
	channel_color_context *ctx = value;

	g_hash_table_foreach_remove(ctx->nick_colors, nick_color_entry_old, cutoff);
	return g_hash_table_size(ctx->nick_colors) == 0;
}

static gboolean nick_colors_prune(gpointer data)
{
	time_t cutoff = time(NULL) - NICK_COLOR_KEEP;

	if (channel_contexts != NULL)
		g_hash_table_foreach_remove(channel_contexts, channel_context_prune, &cutoff);
	return TRUE;
}

void fe_expandos_init(void)
{
	expando_create("winref", expando_winref, "window changed", EXPANDO_ARG_NONE,
	               "window refnum changed", EXPANDO_ARG_WINDOW, NULL);
	expando_create("winname", expando_winname, "window changed", EXPANDO_ARG_NONE,
	               "window name changed", EXPANDO_ARG_WINDOW, NULL);
	expando_create("nickalign", expando_nickalign, "message public", EXPANDO_ARG_NONE,
	               "message own_public", EXPANDO_ARG_NONE, NULL);
	expando_create("nicktrunc", expando_nicktrunc, "message public", EXPANDO_ARG_NONE,
	               "message own_public", EXPANDO_ARG_NONE, NULL);
	expando_create("nickcolored", expando_nickcolored, "message public", EXPANDO_ARG_NONE,
	               "message own_public", EXPANDO_ARG_NONE, NULL);
	expando_create("nickmode", expando_nickmode, "message public", EXPANDO_ARG_NONE,
	               "message own_public", EXPANDO_ARG_NONE, NULL);
	
	/* Register signal handlers for nick cleanup */
	signal_add("message quit", (SIGNAL_FUNC) cleanup_nick_on_quit);
	signal_add("message part", (SIGNAL_FUNC) cleanup_nick_on_part);
	signal_add("message nick", (SIGNAL_FUNC) cleanup_nick_on_nickchange);
	
	/* Register command */
	command_bind("nickhash", NULL, (SIGNAL_FUNC) cmd_nickhash);

	nick_color_prune_tag = g_timeout_add_seconds(3600, nick_colors_prune, NULL);
}

void fe_expandos_deinit(void)
{
	expando_destroy("winref", expando_winref);
	expando_destroy("winname", expando_winname);
	expando_destroy("nickalign", expando_nickalign);
	expando_destroy("nicktrunc", expando_nicktrunc);
	expando_destroy("nickcolored", expando_nickcolored);
	expando_destroy("nickmode", expando_nickmode);
	
	/* Unregister signal handlers */
	signal_remove("message quit", (SIGNAL_FUNC) cleanup_nick_on_quit);
	signal_remove("message part", (SIGNAL_FUNC) cleanup_nick_on_part);
	signal_remove("message nick", (SIGNAL_FUNC) cleanup_nick_on_nickchange);
	
	/* Unregister command */
	command_unbind("nickhash", (SIGNAL_FUNC) cmd_nickhash);
	
	if (nick_color_prune_tag != 0) {
		g_source_remove(nick_color_prune_tag);
		nick_color_prune_tag = 0;
	}

	/* Clean up hash coloring data */
	if (channel_contexts) {
		g_hash_table_destroy(channel_contexts);
		channel_contexts = NULL;
	}
}
