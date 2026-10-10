/*
 fe-web-fuzz.c : shared setup for the fe-web fuzz targets

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/* fe-web-server.c defines it too, the same way */
#define _GNU_SOURCE

/* Everything fe-web-server.c includes, before the stand-ins below are
 * defined: its include guards then keep the macros out of the system and
 * irssi headers. */
#include <irssi/src/fe-web/module.h>
#include <irssi/src/fe-web/fe-web.h>
#include <irssi/src/fe-web/fe-web-ssl.h>
#include <irssi/src/fe-web/fe-web-crypto.h>

#include <irssi/src/core/network.h>
#include <irssi/src/core/net-sendbuffer.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/fe-common/core/printtext.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Stand-ins for the TLS layer and the clock of fe-web-server.c. The TLS
 * stand-in is the plain socket, with scripted write results for the unit
 * tests (tests/fe-web); the fuzz targets never make TLS clients, so for
 * them nothing changes. The clock is the real one unless a test sets it. */
static int fe_web_fuzz_ssl_prepare(void);
static FE_WEB_SSL_CHANNEL *fe_web_fuzz_ssl_channel_create(GIOChannel *plain_channel);
static void fe_web_fuzz_ssl_channel_free(FE_WEB_SSL_CHANNEL *chan);
static int fe_web_fuzz_ssl_accept(FE_WEB_SSL_CHANNEL *chan);
static int fe_web_fuzz_ssl_read(FE_WEB_SSL_CHANNEL *chan, char *buf, int len);
static int fe_web_fuzz_ssl_write(FE_WEB_SSL_CHANNEL *chan, const char *data, int len);
static time_t fe_web_fuzz_time(time_t *t);
static int fe_web_fuzz_getsockopt(int fd, int level, int name, void *val, socklen_t *len);
static int fe_web_fuzz_mkdir_with_parents(const char *dir, int mode);
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__APPLE__)
static int fe_web_fuzz_getpeereid(int fd, uid_t *uid, gid_t *gid);
#define getpeereid fe_web_fuzz_getpeereid
#endif

#define fe_web_ssl_prepare fe_web_fuzz_ssl_prepare
#define fe_web_ssl_channel_create fe_web_fuzz_ssl_channel_create
#define fe_web_ssl_channel_free fe_web_fuzz_ssl_channel_free
#define fe_web_ssl_accept fe_web_fuzz_ssl_accept
#define fe_web_ssl_read fe_web_fuzz_ssl_read
#define fe_web_ssl_write fe_web_fuzz_ssl_write
#define time(t) fe_web_fuzz_time(t)
#define getsockopt fe_web_fuzz_getsockopt
#define g_mkdir_with_parents fe_web_fuzz_mkdir_with_parents

/* The input path of a web client (client_input_once(), the handshake, the
 * password check, the frame loop) is static in fe-web-server.c. It is
 * compiled into the fuzz targets from here, unchanged, so they run the
 * real code without fe-web having to export anything for them. This file
 * must stay the only one that includes it. */
#include <irssi/src/fe-web/fe-web-server.c>

#undef fe_web_ssl_prepare
#undef fe_web_ssl_channel_create
#undef fe_web_ssl_channel_free
#undef fe_web_ssl_accept
#undef fe_web_ssl_read
#undef fe_web_ssl_write
#undef time
#undef getsockopt
#undef g_mkdir_with_parents
#ifdef getpeereid
#undef getpeereid
#endif

#include <irssi/src/fe-fuzz/fe-web/fe-web-fuzz.h>
#include <irssi/src/fe-fuzz/null-logger.h>

#include <irssi/src/core/core.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/chatnets.h>
#include <irssi/src/core/servers-setup.h>
#include <irssi/src/fe-common/core/fe-common-core.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-ansi/sidepanels-activity.h>
#include <irssi/src/fe-ansi/sidepanels-render.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* irc-core.c */
void irc_core_init(void);
/* fe-common-irc.c */
void fe_common_irc_init(void);

/* fe-web.c registers these in fe_web_init(), which also creates the TLS
 * key and certificate in irssi's home - not wanted here */
static void fe_web_fuzz_settings(void)
{
	settings_add_bool("lookandfeel", "fe_web_enabled", FALSE);
	settings_add_int("lookandfeel", "fe_web_port", 9001);
	settings_add_str("lookandfeel", "fe_web_bind", "127.0.0.1");
	settings_add_str("lookandfeel", "fe_web_password", "");
	settings_add_str("lookandfeel", "fe_web_socket", "");
	settings_set_str("fe_web_password", FE_WEB_FUZZ_PASSWORD);
}

/* The sidebar of the terminal frontend, which fe-web clears on mark_read */
void reset_window_priority(WINDOW_REC *win)
{
	(void) win;
}

void redraw_left_panels_only(const char *event_name)
{
	(void) event_name;
}

/* Web clients may run any command: never let a fuzzer do that */
static void sig_swallow(void)
{
	signal_stop();
}

/* stripped text of every "print text" since fe_web_fuzz_take_printed() */
static GString *printed = NULL;

static void sig_capture_printed(TEXT_DEST_REC *dest, const char *text)
{
	char *stripped;

	(void) dest;
	stripped = strip_codes(text);
	g_string_append(printed, stripped);
	g_string_append_c(printed, '\n');
	g_free(stripped);
}

static char *fuzz_home = NULL;
/* "--home=..." for args_execute(), which takes it out of argv: freed at
 * exit (a pointer that is only written could be optimised away, and
 * LeakSanitizer then reports it) */
static char *home_arg = NULL;
static gboolean verbose = FALSE;

static void remove_tree(const char *path)
{
	GDir *dir;
	const char *name;

	dir = g_dir_open(path, 0, NULL);
	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			char *child = g_build_filename(path, name, NULL);
			if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
			    !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
				remove_tree(child);
			else
				unlink(child);
			g_free(child);
		}
		g_dir_close(dir);
	}
	rmdir(path);
}

/* The home of a run that was killed or aborted is left behind; the next
 * run removes it. Only directories named erssi-fe-web-fuzz-<pid>-*, owned
 * by this user, real directories (not links) and whose process is gone. */
#define FUZZ_HOME_PREFIX "erssi-fe-web-fuzz-"

static void remove_stale_homes(void)
{
	const char *tmp = g_get_tmp_dir();
	const char *name;
	GDir *dir;

	dir = g_dir_open(tmp, 0, NULL);
	if (dir == NULL)
		return;
	while ((name = g_dir_read_name(dir)) != NULL) {
		const char *p = name + strlen(FUZZ_HOME_PREFIX);
		char *end, *path;
		struct stat st;
		long pid;

		if (strncmp(name, FUZZ_HOME_PREFIX, strlen(FUZZ_HOME_PREFIX)) != 0)
			continue;
		errno = 0;
		pid = strtol(p, &end, 10);
		if (errno != 0 || end == p || *end != '-' || pid <= 0 || pid == getpid())
			continue;
		if (kill((pid_t) pid, 0) == 0 || errno != ESRCH)
			continue; /* still running, or not ours to ask about */
		path = g_build_filename(tmp, name, NULL);
		if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid())
			remove_tree(path);
		g_free(path);
	}
	g_dir_close(dir);
}

static void free_tls_retry(void);

static void remove_fuzz_home(void)
{
	if (fuzz_home != NULL)
		remove_tree(fuzz_home);
	g_free(home_arg);
	home_arg = NULL;
	if (printed != NULL)
		g_string_free(printed, TRUE);
	printed = NULL;
	free_tls_retry();
}

void fe_web_fuzz_init(gboolean with_irc_signals)
{
	char *argv[3];

	verbose = g_getenv("FE_WEB_FUZZ_VERBOSE") != NULL;
	if (!verbose)
		g_log_set_null_logger();

	remove_stale_homes();
	{
		char *template = g_strdup_printf(FUZZ_HOME_PREFIX "%ld-XXXXXX", (long) getpid());

		fuzz_home = g_dir_make_tmp(template, NULL);
		g_free(template);
	}
	if (fuzz_home == NULL)
		abort();
	atexit(remove_fuzz_home);
	/* also HOME: settings such as autolog_path ("~/.erssi/logs/...") are
	 * relative to the home directory, not to --home */
	g_setenv("HOME", fuzz_home, TRUE);

	argv[0] = "fe-web-fuzz";
	argv[1] = home_arg = g_strconcat("--home=", fuzz_home, NULL);
	argv[2] = NULL;

	core_register_options();
	fe_common_core_register_options();
	args_execute(2, argv);
	core_preinit(argv[0]);
	core_init();
	irc_core_init();
	fe_common_core_init();
	fe_common_irc_init();
	module_register("web", "fe");

	signal_add_first("send command", (SIGNAL_FUNC) sig_swallow);
	signal_add_first("save config", (SIGNAL_FUNC) sig_swallow);
	printed = g_string_new(NULL);
	signal_add_first("print text", (SIGNAL_FUNC) sig_capture_printed);

	fe_web_fuzz_settings();
	fe_web_crypto_init();
	if (fe_web_crypto_get_key() == NULL)
		abort();
	if (with_irc_signals)
		fe_web_signals_init();

	/* start from no networks and servers, so each input sees the same */
	fe_web_fuzz_reset_setup();
}

/* browser end of the socketpair of the current client */
static int peer_fd = -1;
/* what it got and did not show yet, and whether the HTTP part is over */
static GByteArray *sent = NULL;
static gboolean sent_http_done;
/* everything it got (for the unit tests) */
static GByteArray *received = NULL;

/* results for the next TLS writes, see fe_web_fuzz_tls_write_script() */
#define TLS_SCRIPT_MAX 64
static int tls_script[TLS_SCRIPT_MAX];
static gsize tls_script_len = 0, tls_script_pos = 0;
/* What the last TLS write offered when it returned a WANT result: OpenSSL
 * needs the next write on that channel to offer at least those bytes
 * again (fe-web-ssl.h) */
static FE_WEB_SSL_CHANNEL *tls_retry_chan = NULL;
static GByteArray *tls_retry = NULL;
/* the failed flag of the last TLS channel freed, -1 for none */
static int tls_freed_failed = -1;

WEB_CLIENT_REC *fe_web_fuzz_client_new_peer(const char *peer_key, gboolean logged_in,
                                            gboolean tls)
{
	WEB_CLIENT_REC *client;
	int sv[2];

	if (peer_fd != -1 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		abort();
	fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
	fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
	peer_fd = sv[1];
	if (sent == NULL)
		sent = g_byte_array_new();
	g_byte_array_set_size(sent, 0);
	sent_http_done = logged_in;
	if (received == NULL)
		received = g_byte_array_new();
	g_byte_array_set_size(received, 0);
	tls_script_len = tls_script_pos = 0;
	tls_freed_failed = -1;

	client = fe_web_client_create(sv[0], "fuzz:0");
	client->peer_key = g_strdup(peer_key);
	client->handle = net_sendbuffer_create(i_io_channel_new(sv[0]), 0);
	if (tls) {
		client->use_ssl = TRUE;
		client->ssl_channel =
		    fe_web_fuzz_ssl_channel_create(net_sendbuffer_handle(client->handle));
	} else {
		client->use_ssl = FALSE;
		client->ssl_channel = NULL;
	}
	client->encryption_enabled = TRUE;
	if (logged_in) {
		client->handshake_done = TRUE;
		client->authenticated = TRUE;
	}
	return client;
}

WEB_CLIENT_REC *fe_web_fuzz_client_new(gboolean logged_in)
{
	return fe_web_fuzz_client_new_peer("fuzz", logged_in, FALSE);
}

gboolean fe_web_fuzz_client_alive(WEB_CLIENT_REC *client)
{
	return g_slist_find(web_clients, client) != NULL;
}

/* FE_WEB_FUZZ_VERBOSE: what the browser got, the HTTP response and then
 * each frame, encrypted ones decrypted */
static void show_sent(void)
{
	for (;;) {
		int fin, opcode, masked, ret, len;
		guint64 payload_len;
		guchar mask_key[4];
		const guchar *payload;
		char *text;

		if (!sent_http_done) {
			const guchar *end = memmem(sent->data, sent->len, "\r\n\r\n", 4);

			if (end == NULL)
				return;
			len = end + 4 - sent->data;
			fprintf(stderr, "fe-web sent: %.*s", len, (const char *) sent->data);
			g_byte_array_remove_range(sent, 0, len);
			sent_http_done = TRUE;
			continue;
		}
		ret = fe_web_websocket_parse_frame(sent->data, sent->len, &fin, &opcode, &masked,
		                                   &payload_len, mask_key, &payload);
		if (ret <= 0)
			return;
		len = payload - sent->data + payload_len;
		if (opcode == WS_OPCODE_BINARY) {
			guchar *plain = g_malloc(payload_len + 1);
			int plain_len = 0;

			if (fe_web_crypto_decrypt(payload, payload_len, fe_web_crypto_get_key(),
			                          plain, &plain_len))
				fprintf(stderr, "fe-web sent: %.*s\n", plain_len, (char *) plain);
			else
				fprintf(stderr, "fe-web sent: undecryptable frame\n");
			g_free(plain);
		} else {
			text = g_strndup((const char *) payload, payload_len);
			fprintf(stderr, "fe-web sent opcode %d: %s\n", opcode, text);
			g_free(text);
		}
		g_byte_array_remove_range(sent, 0, len);
	}
}

gsize fe_web_fuzz_read_peer(gsize max)
{
	char buf[16384];
	gsize total = 0;
	ssize_t n;

	if (peer_fd == -1)
		return 0;
	while (max == 0 || total < max) {
		gsize want = sizeof(buf);

		if (max != 0 && max - total < want)
			want = max - total;
		n = read(peer_fd, buf, want);
		if (n <= 0)
			break;
		total += n;
		g_byte_array_append(received, (const guchar *) buf, n);
		if (verbose) {
			g_byte_array_append(sent, (const guchar *) buf, n);
			show_sent();
		}
	}
	return total;
}

/* read what fe-web sent to the browser */
static void drain_peer(void)
{
	fe_web_fuzz_read_peer(0);
}

void fe_web_fuzz_client_send(WEB_CLIENT_REC *client, const guchar *data, gsize len)
{
	gsize off = 0;
	int rounds = 0;

	while (fe_web_fuzz_client_alive(client) && rounds++ < 100000) {
		int avail = 0;

		drain_peer();
		/* there is no main loop here: what the writable watch of fe-web
		 * does once the browser has read */
		if (client->send_tag != -1) {
			output_ready(client);
			continue;
		}
		if (off < len) {
			ssize_t n = write(peer_fd, data + off, len - off);
			if (n > 0)
				off += n;
		}
		if (ioctl(client->fd, FIONREAD, &avail) < 0)
			abort();
		if (avail == 0) {
			if (off >= len)
				break;
			continue;
		}
		/* readable: what the input watch of fe-web reacts to */
		client_input_once(client);
	}
	drain_peer();
}

void fe_web_fuzz_client_send_split(WEB_CLIENT_REC *client, const guchar *data, gsize len)
{
	const gsize mlen = sizeof(FE_WEB_FUZZ_SPLIT) - 1;
	const guchar *p = data, *end = data + len;

	while (p <= end && fe_web_fuzz_client_alive(client)) {
		const guchar *m = memmem(p, end - p, FE_WEB_FUZZ_SPLIT, mlen);
		const guchar *chunk_end = m != NULL ? m : end;

		if (chunk_end > p)
			fe_web_fuzz_client_send(client, p, chunk_end - p);
		if (m == NULL)
			break;
		p = m + mlen;
	}
}

void fe_web_fuzz_client_free(WEB_CLIENT_REC *client)
{
	drain_peer();
	if (fe_web_fuzz_client_alive(client))
		fe_web_close_client(client);
	if (peer_fd != -1) {
		close(peer_fd);
		peer_fd = -1;
	}
}

void fe_web_fuzz_reset_login_state(void)
{
	if (login_pacing != NULL)
		fe_web_pacing_reset(login_pacing);
	paced_logged = 0;
	paced_unlogged = 0;
	refused_logged = 0;
	refused_unlogged = 0;
}

FE_WEB_PACING *fe_web_fuzz_login_pacing(void)
{
	if (login_pacing == NULL)
		login_pacing = fe_web_pacing_new();
	return login_pacing;
}

void fe_web_fuzz_reset_setup(void)
{
	while (setupservers != NULL)
		server_setup_remove(setupservers->data);
	while (chatnets != NULL)
		chatnet_remove(chatnets->data);
}

int fe_web_fuzz_verify_password(const char *request)
{
	return fe_web_verify_password(request);
}

/* for the unit tests in tests/fe-web */

const GByteArray *fe_web_fuzz_received(void)
{
	if (received == NULL)
		received = g_byte_array_new();
	return received;
}

void fe_web_fuzz_client_writable(WEB_CLIENT_REC *client)
{
	if (client->send_tag != -1)
		output_ready(client);
}

void fe_web_fuzz_client_input(WEB_CLIENT_REC *client)
{
	client_input(client);
}

void fe_web_fuzz_peer_write(const void *data, gsize len)
{
	const char *p = data;

	while (len > 0) {
		ssize_t n = write(peer_fd, p, len);

		if (n <= 0)
			abort(); /* the tests write less than the socket takes */
		p += n;
		len -= n;
	}
}

void fe_web_fuzz_tls_write_script(const int *results, gsize n)
{
	if (n > TLS_SCRIPT_MAX)
		abort();
	memcpy(tls_script, results, n * sizeof(*results));
	tls_script_len = n;
	tls_script_pos = 0;
}

static time_t fake_now = 0;

/* Unix socket seams: the peer's uid, and another user winning the race
 * for a directory */
static gboolean fake_peer_other = FALSE;
static char *mkdir_first_dir = NULL;
static int mkdir_first_mode = 0;

void fe_web_fuzz_fake_peer_other_uid(gboolean on)
{
	fake_peer_other = on;
}

void fe_web_fuzz_mkdir_first(const char *dir, int mode)
{
	g_free(mkdir_first_dir);
	mkdir_first_dir = g_strdup(dir);
	mkdir_first_mode = mode;
}

static int fe_web_fuzz_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
	int ret = getsockopt(fd, level, name, val, len);

#if defined(__linux__) && defined(SO_PEERCRED)
	if (ret == 0 && fake_peer_other && level == SOL_SOCKET && name == SO_PEERCRED &&
	    *len == sizeof(struct ucred))
		((struct ucred *) val)->uid = getuid() + 1;
#endif
	return ret;
}

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__APPLE__)
static int fe_web_fuzz_getpeereid(int fd, uid_t *uid, gid_t *gid)
{
	int ret = getpeereid(fd, uid, gid);

	if (ret == 0 && fake_peer_other)
		*uid = getuid() + 1;
	return ret;
}
#endif

static int fe_web_fuzz_mkdir_with_parents(const char *dir, int mode)
{
	if (mkdir_first_dir != NULL) {
		char *first = mkdir_first_dir;

		mkdir_first_dir = NULL;
		g_mkdir_with_parents(first, mkdir_first_mode);
		chmod(first, mkdir_first_mode);
		g_free(first);
	}
	return g_mkdir_with_parents(dir, mode);
}

void fe_web_fuzz_set_clock(time_t now)
{
	fake_now = now;
}

char *fe_web_fuzz_take_printed(void)
{
	char *text;

	if (printed == NULL)
		return g_strdup("");
	text = g_string_free(printed, FALSE);
	printed = g_string_new(NULL);
	return text;
}

static gboolean iterate_done(gpointer data)
{
	*(gboolean *) data = TRUE;
	return FALSE;
}

void fe_web_fuzz_iterate(int ms)
{
	gboolean done = FALSE;
	int rounds;

	g_timeout_add(ms, iterate_done, &done);
	while (!done)
		g_main_context_iteration(NULL, TRUE);
	/* and what became ready at the same time */
	for (rounds = 0; rounds < 100 && g_main_context_iteration(NULL, FALSE); rounds++)
		;
}

static void free_tls_retry(void)
{
	if (tls_retry != NULL)
		g_byte_array_free(tls_retry, TRUE);
	tls_retry = NULL;
	tls_retry_chan = NULL;
}

int fe_web_fuzz_tls_freed_failed(void)
{
	return tls_freed_failed;
}

int fe_web_fuzz_listen_port(void)
{
	return listen_port;
}

/* The stand-ins */

static int fe_web_fuzz_ssl_prepare(void)
{
	return TRUE;
}

static FE_WEB_SSL_CHANNEL *fe_web_fuzz_ssl_channel_create(GIOChannel *plain_channel)
{
	FE_WEB_SSL_CHANNEL *chan = g_new0(FE_WEB_SSL_CHANNEL, 1);

	chan->plain_channel = plain_channel;
	chan->fd = g_io_channel_unix_get_fd(plain_channel);
	chan->ssl = NULL;
	chan->ssl_enabled = TRUE;
	chan->handshake_done = TRUE;
	return chan;
}

static void fe_web_fuzz_ssl_channel_free(FE_WEB_SSL_CHANNEL *chan)
{
	if (chan == NULL)
		return;
	tls_freed_failed = chan->failed;
	if (tls_retry_chan == chan)
		tls_retry_chan = NULL;
	g_free(chan);
}

static int fe_web_fuzz_ssl_accept(FE_WEB_SSL_CHANNEL *chan)
{
	return chan != NULL && chan->handshake_done ? 1 : -1;
}

static int fe_web_fuzz_ssl_read(FE_WEB_SSL_CHANNEL *chan, char *buf, int len)
{
	ssize_t n = read(chan->fd, buf, len);

	if (n >= 0)
		return n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return -2;
	chan->failed = 1;
	return -1;
}

static int fe_web_fuzz_ssl_send(FE_WEB_SSL_CHANNEL *chan, const char *data, int len)
{
	ssize_t n = send(chan->fd, data, len, MSG_NOSIGNAL);

	if (n >= 0)
		return n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return FE_WEB_SSL_WANT_WRITE;
	chan->failed = 1;
	return -1;
}

static int fe_web_fuzz_ssl_write_once(FE_WEB_SSL_CHANNEL *chan, const char *data, int len)
{
	int result;

	if (tls_script_pos >= tls_script_len)
		return fe_web_fuzz_ssl_send(chan, data, len);
	result = tls_script[tls_script_pos++];
	if (result > 0)
		return fe_web_fuzz_ssl_send(chan, data, MIN(result, len));
	if (result == -1)
		chan->failed = 1;
	return result;
}

static int fe_web_fuzz_ssl_write(FE_WEB_SSL_CHANNEL *chan, const char *data, int len)
{
	int result;

	if (tls_retry_chan == chan &&
	    ((gsize) len < tls_retry->len || memcmp(data, tls_retry->data, tls_retry->len) != 0)) {
		/* OpenSSL fails such a write with "bad write retry" */
		g_error("fe-web-fuzz: TLS write retry offers %d bytes, not the %u "
		        "offered before%s",
		        len, tls_retry->len,
		        (gsize) len < tls_retry->len ? "" : " (other contents)");
	}
	tls_retry_chan = NULL;

	result = fe_web_fuzz_ssl_write_once(chan, data, len);
	if (result == FE_WEB_SSL_WANT_WRITE || result == FE_WEB_SSL_WANT_READ) {
		if (tls_retry == NULL)
			tls_retry = g_byte_array_new();
		g_byte_array_set_size(tls_retry, 0);
		g_byte_array_append(tls_retry, (const guchar *) data, len);
		tls_retry_chan = chan;
	}
	return result;
}

static time_t fe_web_fuzz_time(time_t *t)
{
	if (fake_now == 0)
		return time(t);
	if (t != NULL)
		*t = fake_now;
	return fake_now;
}
