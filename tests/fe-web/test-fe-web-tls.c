/*
 test-fe-web-tls.c : the TLS layer of fe-web (fe-web-ssl.c), for real

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The other fe-web suites stand in for the TLS layer; these run
 fe-web-ssl.c itself with OpenSSL, in the temporary irssi directory of the
 harness. The key and the self-signed certificate are made at the first
 start and kept in ~/.erssi/fe-web-key.pem (mode 0600) and
 fe-web-cert.pem, so a web client can pin the certificate: they must stay
 the same across restarts. A key that others can read, a key or a
 certificate that is a link, a FIFO or anything but a regular file is not
 used (and a FIFO does not hang the start): a new pair is made, written to
 a temporary file and renamed into place. A client that pins the certificate completes a handshake with
 the names 127.0.0.1 and localhost, and data goes both ways.
*/

#include "fe-web-test.h"

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define NEW_KEY_NOTICE "can be read by other users or is not yours - making a new key"

static char *key_path, *cert_path;

static char *read_file(const char *path)
{
	char *data = NULL;

	if (!g_file_get_contents(path, &data, NULL, NULL))
		return NULL;
	return data;
}

/* Does the file at 'path' hold exactly 'want' */
static gboolean file_is(const char *path, const char *want)
{
	char *data = read_file(path);
	gboolean same = data != NULL && strcmp(data, want) == 0;

	g_free(data);
	return same;
}

/* Back to no key and no certificate, the TLS layer not prepared */
static void reset(void)
{
	fe_web_ssl_deinit();
	unlink(key_path);
	unlink(cert_path);
	g_free(fe_web_fuzz_take_printed());
}

/* fe_web_ssl_prepare() after a restart (the context made again); returns
 * what it printed (to be freed) */
static char *restart(void)
{
	fe_web_ssl_deinit();
	g_assert_true(fe_web_ssl_prepare());
	g_assert_nonnull(fe_web_ssl_ctx);
	return fe_web_fuzz_take_printed();
}

static void assert_private_file(const char *path)
{
	struct stat st;

	g_assert_cmpint(lstat(path, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpint(st.st_mode & 07777, ==, 0600);
	g_assert_cmpuint(st.st_uid, ==, getuid());
}

/* nothing of the temporary-file-and-rename save is left over: no file
 * named after the key or the certificate with something after it */
static void assert_no_temporary_files(void)
{
	GDir *dir = g_dir_open(fe_web_fuzz_home(), 0, NULL);
	const char *name;

	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL) {
		if (g_str_has_prefix(name, "fe-web-key.pem.") ||
		    g_str_has_prefix(name, "fe-web-cert.pem."))
			g_error("temporary file left: %s", name);
	}
	g_dir_close(dir);
}

static void test_made_at_first_start(void)
{
	char *out;

	reset();
	g_assert_false(g_file_test(key_path, G_FILE_TEST_EXISTS));
	g_assert_true(fe_web_ssl_prepare());
	g_assert_true(fe_web_ssl_is_enabled());
	assert_private_file(key_path);
	g_assert_true(g_file_test(cert_path, G_FILE_TEST_IS_REGULAR));
	assert_no_temporary_files();
	g_assert_cmpint(SSL_CTX_get_min_proto_version(fe_web_ssl_ctx), ==, TLS1_2_VERSION);
	out = fe_web_fuzz_take_printed();
	g_assert_cmpstr(out, ==, "");
	g_free(out);
	/* prepared once: a second call changes nothing */
	g_assert_true(fe_web_ssl_prepare());
}

/* the pin of a web client depends on it */
static void test_reused_after_restart(void)
{
	char *cert, *key, *out;

	reset();
	g_assert_true(fe_web_ssl_prepare());
	cert = read_file(cert_path);
	key = read_file(key_path);
	g_assert_nonnull(cert);
	g_assert_nonnull(key);

	out = restart();
	g_assert_cmpstr(out, ==, "");
	g_free(out);
	g_assert_true(file_is(cert_path, cert));
	g_assert_true(file_is(key_path, key));
	g_free(cert);
	g_free(key);
}

/* A key other users could read is not used: a new pair, saved 0600 */
static void test_readable_key_replaced(void)
{
	static const int modes[] = { 0644, 0640, 0604, 0601 };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(modes); i++) {
		char *cert, *out;

		reset();
		g_assert_true(fe_web_ssl_prepare());
		cert = read_file(cert_path);
		g_assert_cmpint(chmod(key_path, modes[i]), ==, 0);

		out = restart();
		if (strstr(out, NEW_KEY_NOTICE) == NULL)
			g_error("mode %o: expected the notice, got:\n%s", modes[i], out);
		g_free(out);
		assert_private_file(key_path);
		assert_no_temporary_files();
		g_assert_false(file_is(cert_path, cert));
		g_free(cert);
	}
}

/* A link at the key's path is not followed: a new key replaces the link,
 * the file it pointed to stays as it was */
static void test_key_link_not_followed(void)
{
	char *elsewhere, *key, *cert;

	reset();
	g_assert_true(fe_web_ssl_prepare());
	key = read_file(key_path);
	cert = read_file(cert_path);
	elsewhere = g_build_filename(fe_web_fuzz_home(), "elsewhere.pem", NULL);
	g_assert_cmpint(rename(key_path, elsewhere), ==, 0);
	g_assert_cmpint(symlink(elsewhere, key_path), ==, 0);

	g_free(restart());
	assert_private_file(key_path);
	g_assert_false(file_is(cert_path, cert));
	g_assert_true(file_is(elsewhere, key));
	unlink(elsewhere);
	g_free(elsewhere);
	g_free(key);
	g_free(cert);
}

/* A FIFO at the key's or the certificate's path: the start does not wait
 * for a writer (alarm() ends a test that hangs), a new pair replaces it */
static void test_fifo_not_read(void)
{
	char *both[2];
	int i;

	both[0] = key_path;
	both[1] = cert_path;
	for (i = 0; i < 2; i++) {
		reset();
		g_assert_true(fe_web_ssl_prepare());
		g_assert_cmpint(unlink(both[i]), ==, 0);
		g_assert_cmpint(mkfifo(both[i], 0600), ==, 0);
		alarm(5);
		g_free(restart());
		alarm(0);
		assert_private_file(key_path);
		g_assert_true(g_file_test(cert_path, G_FILE_TEST_IS_REGULAR));
		g_assert_false(g_file_test(cert_path, G_FILE_TEST_IS_SYMLINK));
		assert_no_temporary_files();
	}
}

/* A link at the certificate's path, even to the right certificate, is not
 * followed: a new pair replaces the link, the file it pointed to stays */
static void test_cert_link_not_followed(void)
{
	char *elsewhere, *cert;

	reset();
	g_assert_true(fe_web_ssl_prepare());
	cert = read_file(cert_path);
	elsewhere = g_build_filename(fe_web_fuzz_home(), "elsewhere-cert.pem", NULL);
	g_assert_cmpint(rename(cert_path, elsewhere), ==, 0);
	g_assert_cmpint(symlink(elsewhere, cert_path), ==, 0);

	g_free(restart());
	g_assert_true(g_file_test(cert_path, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(cert_path, G_FILE_TEST_IS_SYMLINK));
	g_assert_false(file_is(cert_path, cert));
	g_assert_true(file_is(elsewhere, cert));
	unlink(elsewhere);
	g_free(elsewhere);
	g_free(cert);
}

/* a link to /dev/null as the key: not read, a new key in its place */
static void test_key_dev_null_link(void)
{
	reset();
	g_assert_cmpint(symlink("/dev/null", key_path), ==, 0);
	g_assert_true(fe_web_ssl_prepare());
	assert_private_file(key_path);
	g_free(fe_web_fuzz_take_printed());
}

/* a damaged certificate, or one of another key: a new pair */
static void test_damaged_files_replaced(void)
{
	char *cert;

	reset();
	g_assert_true(fe_web_ssl_prepare());
	g_assert_true(g_file_set_contents(cert_path, "not a certificate\n", -1, NULL));
	g_free(restart());
	cert = read_file(cert_path);
	g_assert_nonnull(strstr(cert, "-----BEGIN CERTIFICATE-----"));
	assert_private_file(key_path);

	/* the certificate of another key */
	g_assert_cmpint(rename(cert_path, "other-cert.tmp"), ==, 0);
	reset();
	g_assert_true(fe_web_ssl_prepare());
	g_assert_cmpint(rename("other-cert.tmp", cert_path), ==, 0);
	g_free(restart());
	g_assert_false(file_is(cert_path, cert));
	g_free(cert);
}

/* The files cannot be written: TLS works all the same, with a pair made
 * for this start, and the user is told */
static void test_save_failure(void)
{
	char *out;

	if (getuid() == 0) {
		g_test_skip("root writes into a read-only directory");
		return;
	}
	reset();
	g_assert_cmpint(chmod(fe_web_fuzz_home(), 0500), ==, 0);
	g_assert_true(fe_web_ssl_prepare());
	out = fe_web_fuzz_take_printed();
	g_assert_cmpint(chmod(fe_web_fuzz_home(), 0700), ==, 0);
	if (strstr(out, "Could not save the certificate") == NULL)
		g_error("expected the save error, got:\n%s", out);
	g_free(out);
	g_assert_false(g_file_test(key_path, G_FILE_TEST_EXISTS));
	assert_no_temporary_files();
}

/* A TLS client on one end of a socketpair, fe-web-ssl.c on the other */
typedef struct {
	SSL_CTX *ctx;
	SSL *ssl;
	int fd;
	FE_WEB_SSL_CHANNEL *server;
	GIOChannel *server_io;
} TLS_PAIR;

/* A client that trusts only the certificate in 'pin_path' and checks
 * 'name' (an address or a host name) */
static void pair_new(TLS_PAIR *p, const char *pin_path, const char *name)
{
	X509_VERIFY_PARAM *param;
	int sv[2];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
	p->fd = sv[1];
	p->server_io = g_io_channel_unix_new(sv[0]);
	g_io_channel_set_close_on_unref(p->server_io, TRUE);
	p->server = fe_web_ssl_channel_create(p->server_io);
	g_assert_nonnull(p->server);

	p->ctx = SSL_CTX_new(TLS_client_method());
	g_assert_nonnull(p->ctx);
	g_assert_cmpint(SSL_CTX_load_verify_locations(p->ctx, pin_path, NULL), ==, 1);
	SSL_CTX_set_verify(p->ctx, SSL_VERIFY_PEER, NULL);
	p->ssl = SSL_new(p->ctx);
	param = SSL_get0_param(p->ssl);
	if (X509_VERIFY_PARAM_set1_ip_asc(param, name) != 1)
		g_assert_cmpint(X509_VERIFY_PARAM_set1_host(param, name, 0), ==, 1);
	SSL_set_fd(p->ssl, p->fd);
	SSL_set_connect_state(p->ssl);
}

static void pair_free(TLS_PAIR *p)
{
	fe_web_ssl_channel_free(p->server);
	g_io_channel_unref(p->server_io);
	SSL_free(p->ssl);
	SSL_CTX_free(p->ctx);
	if (p->fd != -1)
		close(p->fd);
	ERR_clear_error();
}

/* Run both ends until the handshake is done or one of them gives up;
 * TRUE when it is done */
static gboolean pair_handshake(TLS_PAIR *p)
{
	gboolean client_done = FALSE, server_done = FALSE;
	int i;

	for (i = 0; i < 1000 && !(client_done && server_done); i++) {
		if (!client_done) {
			int ret = SSL_do_handshake(p->ssl);

			if (ret == 1) {
				client_done = TRUE;
			} else {
				int err = SSL_get_error(p->ssl, ret);

				if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
					return FALSE;
			}
		}
		if (!server_done) {
			int ret = fe_web_ssl_accept(p->server);

			if (ret < 0)
				return FALSE;
			server_done = ret == 1;
		}
	}
	return client_done && server_done;
}

/* The client reads 'len' bytes (in as many tries as TLS needs) */
static void client_read(TLS_PAIR *p, char *buf, int len)
{
	int got = 0, i;

	for (i = 0; i < 1000 && got < len; i++) {
		int ret = SSL_read(p->ssl, buf + got, len - got);

		if (ret > 0)
			got += ret;
		else
			g_assert_cmpint(SSL_get_error(p->ssl, ret), ==, SSL_ERROR_WANT_READ);
	}
	g_assert_cmpint(got, ==, len);
}

/* The server end reads 'len' bytes */
static void server_read(TLS_PAIR *p, char *buf, int len)
{
	int got = 0, i;

	for (i = 0; i < 1000 && got < len; i++) {
		int ret = fe_web_ssl_read(p->server, buf + got, len - got);

		if (ret > 0)
			got += ret;
		else
			g_assert_cmpint(ret, ==, -2);
	}
	g_assert_cmpint(got, ==, len);
}

static void test_handshake_with_pin(void)
{
	static const char *names[] = { "127.0.0.1", "::1", "localhost" };
	gsize n;

	reset();
	g_assert_true(fe_web_ssl_prepare());
	for (n = 0; n < G_N_ELEMENTS(names); n++) {
		TLS_PAIR p;
		char buf[6] = "";
		char *out;
		int ret, i;

		pair_new(&p, cert_path, names[n]);
		if (!pair_handshake(&p))
			g_error("%s: no handshake", names[n]);
		g_assert_cmpint(SSL_get_verify_result(p.ssl), ==, X509_V_OK);
		g_assert_cmpint(SSL_version(p.ssl), >=, TLS1_2_VERSION);

		/* client to server and back */
		g_assert_cmpint(SSL_write(p.ssl, "hello", 5), ==, 5);
		server_read(&p, buf, 5);
		g_assert_cmpstr(buf, ==, "hello");
		g_assert_cmpint(fe_web_ssl_write(p.server, "world", 5), ==, 5);
		client_read(&p, buf, 5);
		g_assert_cmpstr(buf, ==, "world");

		/* the client goes away without a close_notify: an ordinary
		 * disconnect, read as 0 without a notice, and no SSL_shutdown
		 * after it. On a Unix socket (as here and with fe_web_socket)
		 * the alert OpenSSL then tries to send fails at once with
		 * EPIPE. */
		close(p.fd);
		p.fd = -1;
		ret = -2;
		for (i = 0; i < 1000 && ret == -2; i++)
			ret = fe_web_ssl_read(p.server, buf, sizeof(buf));
		g_assert_cmpint(ret, ==, 0);
		g_assert_true(p.server->failed);
		out = fe_web_fuzz_take_printed();
		g_assert_cmpstr(out, ==, "");
		g_free(out);
		pair_free(&p);
	}
	g_free(fe_web_fuzz_take_printed());
}

/* A client pinned to the certificate of an earlier key is refused once
 * the key was replaced: the pin holds only while the pair is kept */
static void test_old_pin_refused_after_new_key(void)
{
	char *old_pin;
	TLS_PAIR p;

	reset();
	g_assert_true(fe_web_ssl_prepare());
	old_pin = g_build_filename(fe_web_fuzz_home(), "old-pin.pem", NULL);
	g_assert_cmpint(link(cert_path, old_pin), ==, 0);
	g_assert_cmpint(chmod(key_path, 0644), ==, 0);
	g_free(restart());

	pair_new(&p, old_pin, "127.0.0.1");
	g_assert_false(pair_handshake(&p));
	pair_free(&p);

	/* the new certificate is trusted */
	pair_new(&p, cert_path, "127.0.0.1");
	g_assert_true(pair_handshake(&p));
	pair_free(&p);
	unlink(old_pin);
	g_free(old_pin);
	g_free(fe_web_fuzz_take_printed());
}

int main(int argc, char **argv)
{
	int ret;

	test_init(&argc, &argv);
	/* a write to a client end that is gone is an error, not a signal */
	signal(SIGPIPE, SIG_IGN);
	fe_web_ssl_init();
	key_path = g_build_filename(fe_web_fuzz_home(), "fe-web-key.pem", NULL);
	cert_path = g_build_filename(fe_web_fuzz_home(), "fe-web-cert.pem", NULL);
	/* relative paths in the tests are in the irssi directory */
	g_assert_cmpint(chdir(fe_web_fuzz_home()), ==, 0);

	g_test_add_func("/fe-web/tls/made-at-first-start", test_made_at_first_start);
	g_test_add_func("/fe-web/tls/reused-after-restart", test_reused_after_restart);
	g_test_add_func("/fe-web/tls/readable-key-replaced", test_readable_key_replaced);
	g_test_add_func("/fe-web/tls/key-link-not-followed", test_key_link_not_followed);
	g_test_add_func("/fe-web/tls/fifo-not-read", test_fifo_not_read);
	g_test_add_func("/fe-web/tls/cert-link-not-followed", test_cert_link_not_followed);
	g_test_add_func("/fe-web/tls/key-dev-null-link", test_key_dev_null_link);
	g_test_add_func("/fe-web/tls/damaged-files-replaced", test_damaged_files_replaced);
	g_test_add_func("/fe-web/tls/save-failure", test_save_failure);
	g_test_add_func("/fe-web/tls/handshake-with-pin", test_handshake_with_pin);
	g_test_add_func("/fe-web/tls/old-pin-refused-after-new-key",
	                test_old_pin_refused_after_new_key);

	ret = g_test_run();
	reset();
	g_free(key_path);
	g_free(cert_path);
	return ret;
}
