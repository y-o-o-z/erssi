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
#include <signal.h>
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
		/* both are printed to the terminal */
		if (!firstrun_printable(node->key) || !firstrun_printable(address))
			continue;

		n = g_new0(FIRSTRUN_NETWORK, 1);
		n->chatnet = g_strdup(node->key);
		n->address = g_strdup(address);
		n->port = config_node_get_int(server, "port", 6667);
		n->tls = config_node_get_bool(server, "use_tls", FALSE) ||
		         config_node_get_bool(server, "use_ssl", FALSE);
		/* the precedence of servers-setup.c */
		n->tls_verify = config_node_find(server, "tls_verify") != NULL ?
		                    config_node_get_bool(server, "tls_verify", TRUE) :
		                    config_node_get_bool(server, "ssl_verify", TRUE);
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

gboolean firstrun_printable(const char *s)
{
	if (s == NULL)
		return FALSE;
	for (; *s != '\0'; s++)
		if ((unsigned char) *s < 0x20 || (unsigned char) *s > 0x7e)
			return FALSE;
	return TRUE;
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

static gboolean account_valid(const char *s)
{
	for (; *s != '\0'; s++)
		if ((unsigned char) *s < 0x20 || *s == 0x7f)
			return FALSE;
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
	/* printed in the prompts: a default that is not printable is no default */
	if (!firstrun_printable(default_nick))
		default_nick = NULL;
	if (!firstrun_printable(default_theme))
		default_theme = NULL;

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
			       n->tls ? (n->tls_verify ? " TLS" : " TLS (unverified)") : "");
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

	/* SASL: only over verified TLS, so the password never travels where a MITM can read it */
	if (chosen != NULL && chosen->tls && chosen->tls_verify) {
		for (tries = 0; tries < FIRSTRUN_MAX_TRIES; tries++) {
			PROMPT(out, "SASL account on %s (empty = none): ", chosen->chatnet);
			line = read(ctx, FALSE);
			if (line == NULL)
				goto skipped;
			trim(line);
			if (*line == '\0') {
				g_free(line);
				break;
			}
			if (!account_valid(line)) {
				g_free(line);
				PROMPT(out, "  not a valid account (no control characters)\n");
				continue;
			}
			answers->sasl_user = line;
			break;
		}
		if (answers->sasl_user != NULL) {
			PROMPT(out, "%s", FIRSTRUN_SECRET_PROMPT);
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

/* Switches the terminal at once, then drops its pending input: nothing
 * typed before the switch is read after it. Not TCSAFLUSH, which also waits
 * until the terminal has read everything written to it: on the BSDs that
 * blocks for good on a pty whose master has stopped reading (^S, a stalled
 * ssh). 0, or -1 with errno. */
static int tty_switch(int fd, const struct termios *t)
{
	int rc;

	do {
		rc = tcsetattr(fd, TCSANOW, t);
	} while (rc != 0 && errno == EINTR);
	if (rc != 0)
		return rc;
	do {
		rc = tcflush(fd, TCIFLUSH);
	} while (rc != 0 && errno == EINTR);
	return rc;
}

/* sets up a no-echo, no-signal, byte-at-a-time terminal; FALSE = could not, fail closed */
static gboolean secret_tty_begin(int fd, struct termios *saved)
{
	struct termios quiet, check;

	if (tcgetattr(fd, saved) != 0)
		return FALSE;
	quiet = *saved;
	quiet.c_lflag &= ~(tcflag_t) (ECHO | ECHONL | ISIG | ICANON);
	quiet.c_cc[VMIN] = 1;
	quiet.c_cc[VTIME] = 0;
	/* anything typed ahead of the prompt is dropped */
	if (tty_switch(fd, &quiet) != 0) {
		tcsetattr(fd, TCSANOW, saved);
		return FALSE;
	}
	/* tcsetattr reports success if only part of the change was applied */
	if (tcgetattr(fd, &check) != 0 || (check.c_lflag & (ECHO | ISIG | ICANON)) != 0) {
		tcsetattr(fd, TCSANOW, saved);
		return FALSE;
	}
	return TRUE;
}

static gboolean is_cc(const struct termios *t, int idx, unsigned char c)
{
	return t->c_cc[idx] != _POSIX_VDISABLE && t->c_cc[idx] == c;
}

static volatile sig_atomic_t *abort_flag;

void firstrun_set_abort_flag(volatile sig_atomic_t *flag)
{
	abort_flag = flag;
}

static gboolean aborted(void)
{
	return abort_flag != NULL && *abort_flag != 0;
}

/* to the terminal being read, else stdout */
static void write_tty(int fd, const char *text)
{
	size_t len = strlen(text);

	if (write(fd, text, len) < 0 && write(STDOUT_FILENO, text, len) < 0) {
		/* nothing more to try */
	}
}

char *firstrun_read_fd(int fd, int timeout_ms, gboolean secret)
{
	char buf[FIRSTRUN_LINE_MAX + 1];
	size_t len = 0;
	char *result = NULL;
	struct termios saved;
	gboolean raw = FALSE;
	size_t late = 0;
	unsigned char c = 0;
	enum { ESC_NONE, ESC_START, ESC_CSI, ESC_SS3 } esc;
	int raise_sig;
	gboolean resumed = FALSE;
	gint64 deadline = timeout_ms >= 0 ? g_get_monotonic_time() + (gint64) timeout_ms * 1000 : 0;

restart:
	esc = ESC_NONE;
	raise_sig = 0;
	/* before the terminal is touched: a stopped job that was killed (SIGTERM, then
	 * SIGCONT) would stop again on tcsetattr from the background */
	if (aborted())
		return NULL;
	if (secret && isatty(fd)) {
		if (!secret_tty_begin(fd, &saved))
			return NULL;
		raw = TRUE;
		if (resumed)
			write_tty(fd, FIRSTRUN_SECRET_PROMPT);
	}

	for (;;) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int wait = -1, r;
		ssize_t n;
		gboolean eof = FALSE;

		if (aborted())
			break;

		if (timeout_ms >= 0) {
			gint64 left = deadline - g_get_monotonic_time();

			if (left <= 0) {
				/* the deadline holds even for a stream without a newline; only
				 * "0 = poll once" may still take what is already there */
				if (timeout_ms > 0 || ++late > FIRSTRUN_LINE_MAX * 4)
					break;
				wait = 0;
			} else {
				wait = (int) ((left + 999) / 1000);
			}
		}
		r = poll(&pfd, 1, wait);
		if (r < 0)
			break; /* EINTR aborts, see the header */
		if (r == 0)
			break; /* timeout */

		n = read(fd, &c, 1);
		if (n < 0) {
			if (errno == EAGAIN)
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
		if (raw) {
			/* an escape sequence (an arrow key: ESC [ D, ESC O A; Alt+key:
			 * ESC x) is dropped whole; a control byte ends it and counts */
			if (esc != ESC_NONE && c >= 0x20 && c != 0x7f) {
				if (esc == ESC_START)
					esc = c == '[' ? ESC_CSI : c == 'O' ? ESC_SS3 : ESC_NONE;
				else if (esc == ESC_SS3 || (c >= 0x40 && c <= 0x7e))
					esc = ESC_NONE; /* the final byte */
				else if (c >= 0x80)
					esc = ESC_NONE; /* not a sequence byte: ends it, counts */
				if (esc != ESC_NONE || c < 0x80)
					continue;
			}
			esc = ESC_NONE;
			/* ISIG is off: these keys give the signal the terminal would have
			 * sent, after the terminal is restored (see the header) */
			if (is_cc(&saved, VINTR, c) || is_cc(&saved, VQUIT, c) ||
			    is_cc(&saved, VSUSP, c)) {
				raise_sig = is_cc(&saved, VINTR, c)   ? SIGINT :
				            is_cc(&saved, VQUIT, c)   ? SIGQUIT :
				                                        SIGTSTP;
				break;
			}
			if (is_cc(&saved, VEOF, c)) {
				if (len > 0)
					result = g_strndup(buf, len);
				break;
			}
			if (is_cc(&saved, VERASE, c) || c == 0x7f || c == '\b') {
				/* a whole UTF-8 character: drop continuation bytes and the lead */
				while (len > 0) {
					unsigned char b = buf[--len];

					if ((b & 0xC0) != 0x80)
						break;
				}
				continue;
			}
			if (is_cc(&saved, VKILL, c)) {
				len = 0;
				continue;
			}
			if (c == '\r')
				c = '\n';
			else if (c == 0x1b) {
				esc = ESC_START;
				continue;
			} else if (c < 0x20 && c != '\n')
				continue; /* ^W, ESC, TAB, ...: never part of a secret */
			if (c == '\n') {
				result = g_strndup(buf, len);
				break;
			}
			if (len >= FIRSTRUN_LINE_MAX)
				break; /* a secret that does not fit is rejected, not truncated */
		}
		if (c == '\n') {
			result = g_strndup(buf, len);
			break;
		}
		if (len < FIRSTRUN_LINE_MAX)
			buf[len++] = c;
		/* else: the rest of an overlong (non-secret) line is discarded */
	}

	if (raw) {
		/* no half-typed password stays queued */
		tty_switch(fd, &saved);
		write_tty(fd, "\n");
		raw = FALSE;
	}
	/* wiped before any signal: a core dump (SIGQUIT) holds nothing of it */
	secure_zero(buf, sizeof(buf));
	secure_zero(&c, sizeof(c));
	if (raise_sig == SIGTSTP) {
		/* suspended like at a cooked prompt: on resume the prompt is shown
		 * again and the secret read from the start, within the same deadline */
		len = 0;
		resumed = TRUE;
		raise(SIGTSTP);
		goto restart;
	}
	if (raise_sig != 0)
		raise(raise_sig);

	if (result != NULL) {
		size_t l = strlen(result);

		while (l > 0 && result[l - 1] == '\r')
			result[--l] = '\0';
	}
	return result;
}
