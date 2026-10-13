#ifndef IRSSI_FE_COMMON_CORE_FIRSTRUN_H
#define IRSSI_FE_COMMON_CORE_FIRSTRUN_H

#include <glib.h>
#include <stdio.h>
#include <irssi/src/lib-config/iconfig.h>

/* One network the first-run setup can offer: the first server of an IRC chatnet. */
typedef struct {
	char *chatnet, *address;
	int port;
	gboolean tls;
	gboolean tls_verify; /* same rule as core: tls_verify, else ssl_verify, default yes */
} FIRSTRUN_NETWORK;

/* What the user answered. chatnet NULL = no network (connect later). sasl_user and
 * sasl_pass are both set or both NULL. theme may be NULL (keep the current one). */
typedef struct {
	char *chatnet;
	char *nick;
	char *sasl_user;
	char *sasl_pass;
	char *theme;
} FIRSTRUN_ANSWERS;

/* Returns a newly allocated line (g_free it; the caller wipes secrets), or NULL on
 * EOF/timeout. SECRET is TRUE for the SASL password: the reader must not echo it. */
typedef char *(*FIRSTRUN_READ_FUNC)(void *ctx, gboolean secret);

typedef enum { FIRSTRUN_DONE, FIRSTRUN_SKIPPED } FIRSTRUN_RESULT;

/* tty && (forced || (!config_exists && !scripted)) */
gboolean firstrun_wanted(gboolean config_exists, gboolean tty, gboolean forced,
                         gboolean scripted);

/* One entry per chatnet of type IRC that has at least one server; the entry takes the
 * first server of that chatnet in list order. Sorted by name, case-insensitively. The
 * array has a free func; g_ptr_array_free(array, TRUE) releases it. */
GPtrArray *firstrun_networks(CONFIG_REC *config);

/* Asks the questions on OUT, reading answers through READ. THEMES is a NULL terminated
 * list (NULL or empty = no theme question). Invalid input is asked again, up to 3 times
 * per question, then the default is used. "n" at "Set up now?" or NULL from READ at any
 * point gives FIRSTRUN_SKIPPED with ANSWERS wiped. The SASL questions are only asked
 * when the chosen network's server has TLS with certificate verification (never a
 * password over plaintext or unverified TLS); an empty account, an empty password or
 * a control byte in the account (asked again) means no SASL. ANSWERS is initialised here. */
FIRSTRUN_RESULT firstrun_ask(FILE *out, FIRSTRUN_READ_FUNC read, void *ctx, GPtrArray *networks,
                             char **themes, const char *default_nick,
                             const char *default_theme, FIRSTRUN_ANSWERS *answers);

/* If a chatnet is set: autoconnect = yes on its first server. If sasl_user is set:
 * sasl_mechanism = PLAIN, sasl_username and sasl_password in chatnets/<chatnet>.
 * Nothing else changes. */
void firstrun_apply_config(CONFIG_REC *config, const FIRSTRUN_ANSWERS *answers);

/* 1-30 bytes; first char [A-Za-z[]\`_^{|}], the rest also [0-9-]. */
gboolean firstrun_nick_valid(const char *nick);

/* Reads one line from FD, waiting at most TIMEOUT_MS in total (negative = no limit,
 * 0 = poll once). Reads byte by byte, so nothing past the line is consumed. At most 511
 * bytes are returned; the rest of a longer line is discarded. Trailing \r\n is stripped.
 * Returns NULL on timeout and on EOF with nothing read. Rule for a partial line (no
 * newline yet): EOF returns the text read so far, timeout returns NULL.
 * A signal that interrupts the wait (EINTR; core handles SIGTERM/SIGHUP this way) aborts
 * the read: NULL, so the caller treats it as "skipped" and then honours core's flag.
 * If SECRET and FD is a terminal, echo, ISIG and ICANON are off during the read (like
 * getpass, with erase/kill/EOF handled here) and the terminal is restored with
 * TCSAFLUSH on every exit path, so no half-typed password stays queued; a newline is
 * printed afterwards. Erase removes a whole UTF-8 character; an escape sequence (an
 * arrow key: ESC [ ... final byte, ESC O x; Alt+key) is dropped whole; other control
 * bytes are ignored and never stored; a secret longer than 511 bytes is rejected (NULL). The
 * overall deadline holds even for a stream with no newline (with TIMEOUT_MS 0 at most a
 * few KiB already queued are taken). The interrupt, quit and suspend characters abort the read (NULL).
 * If the terminal cannot be switched, a secret is not read at all (NULL).
 * Internal buffers are wiped. Only erssi's own copies are wiped: lib-config and the
 * saved config hold the password in plaintext by design. */
char *firstrun_read_fd(int fd, int timeout_ms, gboolean secret);

/* Frees everything and zeroes the struct; the password is wiped before it is freed. */
void firstrun_answers_clear(FIRSTRUN_ANSWERS *answers);

#endif
