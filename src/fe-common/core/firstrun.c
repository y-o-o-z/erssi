/*
 firstrun.c : first-run setup (network, nick, SASL, theme), pure logic

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#include "module.h"
#include <irssi/src/fe-common/core/firstrun.h>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define FIRSTRUN_MAX_TRIES 3
#define FIRSTRUN_LINE_MAX 511

/* a volatile store loop the compiler cannot drop, unlike a memset before free */
static void secure_zero(void *p, size_t n)
{
	volatile unsigned char *v = p;

	while (n-- > 0)
		*v++ = 0;
}

static void wipe_string(char *s)
{
	if (s != NULL)
		secure_zero(s, strlen(s));
}

gboolean firstrun_wanted(gboolean config_exists, gboolean tty, gboolean forced, gboolean scripted)
{
	return tty && (forced || (!config_exists && !scripted));
}

static void network_free(gpointer data)
{
	FIRSTRUN_NETWORK *n = data;

	g_free(n->chatnet);
	g_free(n->address);
	g_free(n);
}

static int network_cmp(gconstpointer a, gconstpointer b)
{
	const FIRSTRUN_NETWORK *const *na = a;
	const FIRSTRUN_NETWORK *const *nb = b;

	return g_ascii_strcasecmp((*na)->chatnet, (*nb)->chatnet);
}

/* first server node of CHATNET in the servers list, or NULL */
static CONFIG_NODE *first_server_of(CONFIG_REC *config, const char *chatnet)
{
	CONFIG_NODE *servers = config_node_traverse(config, "servers", FALSE);
	GSList *tmp;

	if (servers == NULL || servers->value == NULL)
		return NULL;
	for (tmp = config_node_first(servers->value); tmp != NULL; tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;
		const char *net = config_node_get_str(node, "chatnet", NULL);

		if (node->type == NODE_TYPE_BLOCK && net != NULL &&
		    g_ascii_strcasecmp(net, chatnet) == 0)
			return node;
	}
	return NULL;
}

GPtrArray *firstrun_networks(CONFIG_REC *config)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(network_free);
	CONFIG_NODE *chatnets = config_node_traverse(config, "chatnets", FALSE);
	GSList *tmp;

	if (chatnets == NULL || chatnets->value == NULL)
		return list;

	for (tmp = config_node_first(chatnets->value); tmp != NULL; tmp = config_node_next(tmp)) {
		CONFIG_NODE *node = tmp->data;
		CONFIG_NODE *server;
		FIRSTRUN_NETWORK *n;
		const char *type, *address;

		if (node->type != NODE_TYPE_BLOCK || node->key == NULL)
			continue;
		type = config_node_get_str(node, "type", NULL);
		if (type == NULL || g_ascii_strcasecmp(type, "IRC") != 0)
			continue;
		server = first_server_of(config, node->key);
		if (server == NULL)
			continue;
		address = config_node_get_str(server, "address", NULL);
		if (address == NULL || *address == '\0')
			continue;

		n = g_new0(FIRSTRUN_NETWORK, 1);
		n->chatnet = g_strdup(node->key);
		n->address = g_strdup(address);
		n->port = config_node_get_int(server, "port", 6667);
		n->tls = config_node_get_bool(server, "use_tls", FALSE) ||
		         config_node_get_bool(server, "use_ssl", FALSE);
		g_ptr_array_add(list, n);
	}
	g_ptr_array_sort(list, network_cmp);
	return list;
}

void firstrun_answers_clear(FIRSTRUN_ANSWERS *answers)
{
	if (answers == NULL)
		return;
	g_free(answers->chatnet);
	g_free(answers->nick);
	g_free(answers->sasl_user);
	wipe_string(answers->sasl_pass);
	g_free(answers->sasl_pass);
	g_free(answers->theme);
	memset(answers, 0, sizeof(*answers));
}

gboolean firstrun_nick_valid(const char *nick)
{
	size_t i, len;

	if (nick == NULL)
		return FALSE;
	len = strlen(nick);
	if (len < 1 || len > 30)
		return FALSE;
	for (i = 0; i < len; i++) {
		unsigned char c = nick[i];
		gboolean letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
		gboolean special = c == '[' || c == ']' || c == '\\' || c == '`' || c == '_' ||
		                   c == '^' || c == '{' || c == '|' || c == '}';

		if (letter || special)
			continue;
		if (i > 0 && ((c >= '0' && c <= '9') || c == '-'))
			continue;
		return FALSE;
	}
	return TRUE;
}

/* strips surrounding whitespace in place */
static char *trim(char *s)
{
	return g_strstrip(s);
}

/* TRUE and *index (1-based) when TEXT is a plain number in 0..MAX */
static gboolean parse_choice(const char *text, int max, int *value)
{
	char *end;
	gint64 n;

	if (*text == '\0' || !g_ascii_isdigit(*text))
		return FALSE;
	n = g_ascii_strtoll(text, &end, 10);
	if (*end != '\0' || n < 0 || n > max)
		return FALSE;
	*value = (int) n;
	return TRUE;
}

#define PROMPT(out, ...)                                                                           \
	do {                                                                                       \
		fprintf((out), __VA_ARGS__);                                                       \
		fflush(out);                                                                       \
	} while (0)

FIRSTRUN_RESULT firstrun_ask(FILE *out, FIRSTRUN_READ_FUNC read, void *ctx, GPtrArray *networks,
                             char **themes, const char *default_nick,
                             const char *default_theme, FIRSTRUN_ANSWERS *answers)
{
	FIRSTRUN_NETWORK *chosen = NULL;
	int ntheme = 0, def_net = 1, choice = 0, tries;
	char *line;
	guint i;

	memset(answers, 0, sizeof(*answers));
	while (themes != NULL && themes[ntheme] != NULL)
		ntheme++;

	PROMPT(out, "erssi first-run setup: network, nick, SASL and theme. Enter keeps the "
	            "[default].\n");
	PROMPT(out, "Set up now? [Y/n] ");
	line = read(ctx, FALSE);
	if (line == NULL)
		return FIRSTRUN_SKIPPED;
	trim(line);
	if (*line == 'n' || *line == 'N') {
		g_free(line);
		return FIRSTRUN_SKIPPED;
	}
	g_free(line);

	/* network */
	if (networks != NULL && networks->len > 0) {
		PROMPT(out, "Network:\n");
		for (i = 0; i < networks->len; i++) {
			const FIRSTRUN_NETWORK *n = g_ptr_array_index(networks, i);

			if (g_ascii_strcasecmp(n->chatnet, "IRCnet") == 0 && def_net == 1)
				def_net = i + 1;
			PROMPT(out, "  %u) %-12s %s:%d%s\n", i + 1, n->chatnet, n->address, n->port,
			       n->tls ? " TLS" : "");
		}
		PROMPT(out, "  0) none - connect later with /CONNECT\n");
		choice = def_net;
		for (tries = 0; tries < FIRSTRUN_MAX_TRIES; tries++) {
			int value;

			PROMPT(out, "Network [%d]: ", def_net);
			line = read(ctx, FALSE);
			if (line == NULL)
				goto skipped;
			trim(line);
			if (*line == '\0') {
				g_free(line);
				break;
			}
			if (parse_choice(line, networks->len, &value)) {
				choice = value;
				g_free(line);
				break;
			}
			g_free(line);
			PROMPT(out, "  not a valid choice\n");
		}
		if (choice > 0) {
			chosen = g_ptr_array_index(networks, choice - 1);
			answers->chatnet = g_strdup(chosen->chatnet);
		}
	}

	/* nick */
	answers->nick = g_strdup(default_nick);
	for (tries = 0; tries < FIRSTRUN_MAX_TRIES; tries++) {
		PROMPT(out, "Nick [%s]: ", default_nick != NULL ? default_nick : "");
		line = read(ctx, FALSE);
		if (line == NULL)
			goto skipped;
		trim(line);
		if (*line == '\0') {
			g_free(line);
			break;
		}
		if (firstrun_nick_valid(line)) {
			g_free(answers->nick);
			answers->nick = line;
			break;
		}
		g_free(line);
		PROMPT(out, "  not a valid nick (letters, digits, - _ [ ] \\ ` ^ { | }, up to 30)\n");
	}

	/* SASL: only over TLS, so the password never travels in clear text */
	if (chosen != NULL && chosen->tls) {
		PROMPT(out, "SASL account on %s (empty = none): ", chosen->chatnet);
		line = read(ctx, FALSE);
		if (line == NULL)
			goto skipped;
		trim(line);
		if (*line == '\0') {
			g_free(line);
		} else {
			answers->sasl_user = line;
			PROMPT(out, "SASL password: ");
			line = read(ctx, TRUE);
			if (line == NULL)
				goto skipped;
			if (*line == '\0') {
				/* an account without a password is no SASL */
				g_free(line);
				g_free(answers->sasl_user);
				answers->sasl_user = NULL;
			} else {
				answers->sasl_pass = line;
			}
		}
	}

	/* theme */
	answers->theme = g_strdup(default_theme);
	if (ntheme > 0) {
		PROMPT(out, "Theme: ");
		for (i = 0; i < (guint) ntheme; i++)
			PROMPT(out, "%s%u) %s", i > 0 ? "  " : "", i + 1, themes[i]);
		PROMPT(out, "\n");
		for (tries = 0; tries < FIRSTRUN_MAX_TRIES; tries++) {
			int value, found = -1;

			PROMPT(out, "Theme [%s]: ", default_theme != NULL ? default_theme : "");
			line = read(ctx, FALSE);
			if (line == NULL)
				goto skipped;
			trim(line);
			if (*line == '\0') {
				g_free(line);
				break;
			}
			if (parse_choice(line, ntheme, &value) && value > 0)
				found = value - 1;
			else
				for (i = 0; i < (guint) ntheme; i++)
					if (strcmp(themes[i], line) == 0)
						found = i;
			g_free(line);
			if (found >= 0) {
				g_free(answers->theme);
				answers->theme = g_strdup(themes[found]);
				break;
			}
			PROMPT(out, "  not a valid choice\n");
		}
	}
	return FIRSTRUN_DONE;

skipped:
	firstrun_answers_clear(answers);
	return FIRSTRUN_SKIPPED;
}

void firstrun_apply_config(CONFIG_REC *config, const FIRSTRUN_ANSWERS *answers)
{
	CONFIG_NODE *chatnets, *net, *server;

	if (answers == NULL || answers->chatnet == NULL)
		return;

	server = first_server_of(config, answers->chatnet);
	if (server != NULL)
		config_node_set_bool(config, server, "autoconnect", TRUE);

	if (answers->sasl_user == NULL || answers->sasl_pass == NULL)
		return;
	chatnets = config_node_traverse(config, "chatnets", FALSE);
	net = chatnets != NULL ? config_node_find(chatnets, answers->chatnet) : NULL;
	if (net == NULL)
		return;
	config_node_set_str(config, net, "sasl_mechanism", "PLAIN");
	config_node_set_str(config, net, "sasl_username", answers->sasl_user);
	config_node_set_str(config, net, "sasl_password", answers->sasl_pass);
}

char *firstrun_read_fd(int fd, int timeout_ms, gboolean secret)
{
	char buf[FIRSTRUN_LINE_MAX + 1];
	size_t len = 0;
	char *result = NULL;
	struct termios saved, quiet;
	gboolean restore = FALSE;
	gint64 deadline = timeout_ms >= 0 ? g_get_monotonic_time() + (gint64) timeout_ms * 1000 : 0;

	if (secret && isatty(fd) && tcgetattr(fd, &saved) == 0) {
		quiet = saved;
		quiet.c_lflag &= ~(tcflag_t) ECHO;
		restore = tcsetattr(fd, TCSANOW, &quiet) == 0;
	}

	for (;;) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int wait = -1, r;
		unsigned char c;
		ssize_t n;
		gboolean eof = FALSE;

		if (timeout_ms >= 0) {
			gint64 left = deadline - g_get_monotonic_time();

			if (left <= 0)
				break; /* timeout */
			wait = (int) ((left + 999) / 1000);
		}
		r = poll(&pfd, 1, wait);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break; /* timeout */

		n = read(fd, &c, 1);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			if (errno != EIO) /* EIO: the other end of a pty went away */
				break;
			eof = TRUE;
		} else if (n == 0) {
			eof = TRUE;
		}
		if (eof) {
			if (len > 0)
				result = g_strndup(buf, len);
			break;
		}
		if (c == '\n') {
			result = g_strndup(buf, len);
			break;
		}
		if (len < FIRSTRUN_LINE_MAX)
			buf[len++] = c;
		/* else: an overlong line, the rest is discarded up to the newline */
	}

	if (restore) {
		tcsetattr(fd, TCSANOW, &saved);
		if (write(fd, "\n", 1) < 0 && write(STDOUT_FILENO, "\n", 1) < 0) {
			/* nothing more to try */
		}
	}
	secure_zero(buf, sizeof(buf));

	if (result != NULL) {
		size_t l = strlen(result);

		while (l > 0 && result[l - 1] == '\r')
			result[--l] = '\0';
	}
	return result;
}
