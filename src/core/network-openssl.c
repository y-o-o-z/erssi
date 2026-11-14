/*
 network-ssl.c : SSL support

    Copyright (C) 2002 vjt

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
#include <irssi/src/core/network.h>
#include <irssi/src/core/network-openssl.h>
#include <irssi/src/core/net-sendbuffer.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/tls.h>
#include <irssi/src/core/safe-file.h>

#include <openssl/crypto.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/rand.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* OpenSSL 1.1.0 introduced some backward-incompatible changes to the api */
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && \
    (!defined(LIBRESSL_VERSION_NUMBER) || LIBRESSL_VERSION_NUMBER < 0x2070000fL)
/* The two functions below could be already defined if OPENSSL_API_COMPAT is
 * below the 1.1.0 version so let's do a clean start */
#undef  X509_get_notBefore
#undef  X509_get_notAfter
#define X509_get_notBefore(x)     X509_get0_notBefore(x)
#define X509_get_notAfter(x)      X509_get0_notAfter(x)
#define ASN1_STRING_data(x)       ASN1_STRING_get0_data(x)
#endif

/* OpenSSL 1.1.0 also introduced some useful additions to the api */
#if (OPENSSL_VERSION_NUMBER >= 0x10002000L)
#if (OPENSSL_VERSION_NUMBER < 0x10100000L) || \
    (defined (LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
static int X509_STORE_up_ref(X509_STORE *vfy)
{
    int n;

    n = CRYPTO_add(&vfy->references, 1, CRYPTO_LOCK_X509_STORE);
    g_assert(n > 1);

    return (n > 1) ? 1 : 0;
}
#endif
#endif

/* ssl i/o channel object */
typedef struct
{
	GIOChannel pad;
	gint fd;
	GIOChannel *giochan;
	SSL *ssl;
	SSL_CTX *ctx;
	unsigned int verify:1;
	SERVER_REC *server;
	int port;
} GIOSSLChannel;

static int ssl_inited = FALSE;
/* https://github.com/irssi/irssi/issues/820 */
#if (OPENSSL_VERSION_NUMBER >= 0x10002000L)
static X509_STORE *store = NULL;
#endif

static void irssi_ssl_free(GIOChannel *handle)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
	g_io_channel_unref(chan->giochan);
	SSL_free(chan->ssl);
	SSL_CTX_free(chan->ctx);
	g_free(chan);
}

/* Checks if the given string has internal NUL characters. */
static gboolean has_internal_nul(const char* str, int len) {
	/* Remove trailing nul characters. They would give false alarms */
	while (len > 0 && str[len-1] == 0)
		len--;
	return strlen(str) != len;
}

/* tls_text_name - extract certificate property value by name */
static char *tls_text_name(X509_NAME *name, int nid)
{
	int     pos;
	X509_NAME_ENTRY *entry;
	ASN1_STRING *entry_str;
	int     utf8_length;
	unsigned char *utf8_value;
	char *result;

	if (name == 0 || (pos = X509_NAME_get_index_by_NID(name, nid, -1)) < 0) {
		return NULL;
	}

	entry = X509_NAME_get_entry(name, pos);
	g_return_val_if_fail(entry != NULL, NULL);
	entry_str = X509_NAME_ENTRY_get_data(entry);
	g_return_val_if_fail(entry_str != NULL, NULL);

	/* Convert everything into UTF-8. It's up to OpenSSL to do something
	   reasonable when converting ASCII formats that contain non-ASCII
	   content. */
	if ((utf8_length = ASN1_STRING_to_UTF8(&utf8_value, entry_str)) < 0) {
		g_warning("Error decoding ASN.1 type=%d", ASN1_STRING_type(entry_str));
		return NULL;
	}

	if (has_internal_nul((char *)utf8_value, utf8_length)) {
		g_warning("NUL character in hostname in certificate");
		OPENSSL_free(utf8_value);
		return NULL;
	}

	result = g_strdup((char *) utf8_value);
	OPENSSL_free(utf8_value);
	return result;
}


/* TRUE when host is an IPv4 or IPv6 address rather than a name */
static gboolean host_is_ip_literal(const char *host)
{
	unsigned char buf[sizeof(struct in6_addr)];

	return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

/* TRUE when the certificate has an iPAddress subjectAltName */
static gboolean cert_has_ip_san(X509 *cert)
{
	STACK_OF(GENERAL_NAME) *gens;
	gboolean found = FALSE;
	int i;

	gens = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
	if (gens == NULL)
		return FALSE;
	for (i = 0; i < sk_GENERAL_NAME_num(gens) && !found; i++)
		found = sk_GENERAL_NAME_value(gens, i)->type == GEN_IPADD;
	sk_GENERAL_NAME_pop_free(gens, GENERAL_NAME_free);
	return found;
}

/* Checks the certificate against the address we connected to, as RFC 6125
 * says: a server given by name must match a dNSName subjectAltName (a
 * leading "*." matches exactly one label), or the CommonName when the
 * certificate has no dNSName at all; a server given as an IPv4 or IPv6
 * address must match an iPAddress subjectAltName, compared as an address,
 * so "::1" and "0:0:0:0:0:0:0:1" are the same. A certificate without any
 * iPAddress name is, as in earlier versions, also accepted for an address
 * when its dNSName or CommonName is that address written out. */
static gboolean irssi_ssl_verify_hostname(X509 *cert, const char *hostname)
{
	const unsigned int flags = X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS;
	char *cert_subject_cn;

	if (host_is_ip_literal(hostname)) {
		if (X509_check_ip_asc(cert, hostname, 0) == 1)
			return TRUE;
		if (!cert_has_ip_san(cert) &&
		    X509_check_host(cert, hostname, strlen(hostname),
		                    flags | X509_CHECK_FLAG_NO_WILDCARDS, NULL) == 1)
			return TRUE;
		g_warning("None of the IP addresses in the certificate match '%s'", hostname);
		return FALSE;
	}

	/* X509_check_host() takes a name that starts with '.' for "any name
	 * under it", so ".example.net" would match evil.example.net */
	if (*hostname == '.') {
		g_warning("Refusing to check the certificate against host name '%s'", hostname);
		return FALSE;
	}

	if (X509_check_host(cert, hostname, strlen(hostname), flags, NULL) == 1)
		return TRUE;

	cert_subject_cn = tls_text_name(X509_get_subject_name(cert), NID_commonName);
	if (cert_subject_cn != NULL && *cert_subject_cn != '\0') {
		g_warning("The certificate (common name '%s') does not match host name '%s'",
		          cert_subject_cn, hostname);
	} else {
		g_warning("The certificate does not match host name '%s'", hostname);
	}
	g_free(cert_subject_cn);
	return FALSE;
}

static gboolean irssi_ssl_verify(SSL *ssl, SSL_CTX *ctx, const char* hostname, int port, X509 *cert, SERVER_REC *server, TLS_REC *tls_rec)
{
	long result;

	result = SSL_get_verify_result(ssl);
	if (result != X509_V_OK) {
		g_warning("Could not verify TLS servers certificate: %s", X509_verify_cert_error_string(result));
		return FALSE;
	} else if (! irssi_ssl_verify_hostname(cert, hostname)){
		return FALSE;
	}
	return TRUE;
}

static GIOStatus irssi_ssl_read(GIOChannel *handle, gchar *buf, gsize len, gsize *ret, GError **gerr)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
	gint ret1, err;
	const char *errstr;
	gchar *errmsg;

	ERR_clear_error();
	ret1 = SSL_read(chan->ssl, buf, len);
	if(ret1 <= 0)
	{
		*ret = 0;
		err = SSL_get_error(chan->ssl, ret1);
		if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
			return G_IO_STATUS_AGAIN;
		else if(err == SSL_ERROR_ZERO_RETURN)
			return G_IO_STATUS_EOF;
		else if (err == SSL_ERROR_SYSCALL)
		{
			errstr = ERR_reason_error_string(ERR_get_error());
			if (errstr == NULL && ret1 == -1)
				errstr = strerror(errno);
			if (errstr == NULL)
				errstr = "server closed connection unexpectedly";
		}
		else
		{
			errstr = ERR_reason_error_string(ERR_get_error());
			if (errstr == NULL)
				errstr = "unknown SSL error";
		}
		errmsg = g_strdup_printf("SSL read error: %s", errstr);
		*gerr = g_error_new_literal(G_IO_CHANNEL_ERROR, G_IO_CHANNEL_ERROR_FAILED,
					    errmsg);
		g_free(errmsg);
		return G_IO_STATUS_ERROR;
	}
	else
	{
		*ret = ret1;
		return G_IO_STATUS_NORMAL;
	}
	/*UNREACH*/
	return G_IO_STATUS_ERROR;
}

static GIOStatus irssi_ssl_write(GIOChannel *handle, const gchar *buf, gsize len, gsize *ret, GError **gerr)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
	gint ret1, err;
	const char *errstr;
	gchar *errmsg;

	ERR_clear_error();
	ret1 = SSL_write(chan->ssl, (const char *)buf, len);
	if(ret1 <= 0)
	{
		*ret = 0;
		err = SSL_get_error(chan->ssl, ret1);
		if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
			return G_IO_STATUS_AGAIN;
		else if(err == SSL_ERROR_ZERO_RETURN)
			errstr = "server closed connection";
		else if (err == SSL_ERROR_SYSCALL)
		{
			errstr = ERR_reason_error_string(ERR_get_error());
			if (errstr == NULL && ret1 == -1)
				errstr = strerror(errno);
			if (errstr == NULL)
				errstr = "server closed connection unexpectedly";
		}
		else
		{
			errstr = ERR_reason_error_string(ERR_get_error());
			if (errstr == NULL)
				errstr = "unknown SSL error";
		}
		errmsg = g_strdup_printf("SSL write error: %s", errstr);
		*gerr = g_error_new_literal(G_IO_CHANNEL_ERROR, G_IO_CHANNEL_ERROR_FAILED,
					    errmsg);
		g_free(errmsg);
		return G_IO_STATUS_ERROR;
	}
	else
	{
		*ret = ret1;
		return G_IO_STATUS_NORMAL;
	}
	/*UNREACH*/
	return G_IO_STATUS_ERROR;
}

static GIOStatus irssi_ssl_seek(GIOChannel *handle, gint64 offset, GSeekType type, GError **gerr)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;

	return chan->giochan->funcs->io_seek(handle, offset, type, gerr);
}

static GIOStatus irssi_ssl_close(GIOChannel *handle, GError **gerr)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;

	return chan->giochan->funcs->io_close(handle, gerr);
}

static GSource *irssi_ssl_create_watch(GIOChannel *handle, GIOCondition cond)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;

	return chan->giochan->funcs->io_create_watch(handle, cond);
}

static GIOStatus irssi_ssl_set_flags(GIOChannel *handle, GIOFlags flags, GError **gerr)
{
    GIOSSLChannel *chan = (GIOSSLChannel *)handle;

    return chan->giochan->funcs->io_set_flags(handle, flags, gerr);
}

static GIOFlags irssi_ssl_get_flags(GIOChannel *handle)
{
    GIOSSLChannel *chan = (GIOSSLChannel *)handle;

    return chan->giochan->funcs->io_get_flags(handle);
}

static GIOFuncs irssi_ssl_channel_funcs = {
    irssi_ssl_read,
    irssi_ssl_write,
    irssi_ssl_seek,
    irssi_ssl_close,
    irssi_ssl_create_watch,
    irssi_ssl_free,
    irssi_ssl_set_flags,
    irssi_ssl_get_flags
};

gboolean irssi_ssl_init(void)
{
#if (OPENSSL_VERSION_NUMBER >= 0x10002000L)
	int success;
#endif

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
	if (!OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT, NULL)) {
		g_error("Could not initialize OpenSSL");
		return FALSE;
	}
#else
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
#endif

#if (OPENSSL_VERSION_NUMBER >= 0x10002000L)
	store = X509_STORE_new();
	if (store == NULL) {
		g_error("Could not initialize OpenSSL: X509_STORE_new() failed");
		return FALSE;
	}

	success = X509_STORE_set_default_paths(store);
	if (success == 0) {
		g_warning("Could not load default certificates");
		X509_STORE_free(store);
		store = NULL;
		/* Don't return an error; the user might have their own cafile/capath. */
	}
#endif

	ssl_inited = TRUE;

	return TRUE;
}

static int get_pem_password_callback(char *buffer, int max_length, int rwflag, void *pass)
{
	char *password;
	size_t length;

	if (pass == NULL)
		return 0;

	password = (char *)pass;
	length = strlen(pass);

	if (length > max_length)
		return 0;

	memcpy(buffer, password, length + 1);
	return length;
}

static GIOChannel *irssi_ssl_get_iochannel(GIOChannel *handle, int port, SERVER_REC *server)
{
	GIOSSLChannel *chan;
	GIOChannel *gchan;
	int fd;
	SSL *ssl;
	SSL_CTX *ctx = NULL;

	const char *mycert = server->connrec->tls_cert;
	const char *mypkey = server->connrec->tls_pkey;
	const char *mypass = server->connrec->tls_pass;
	const char *cafile = server->connrec->tls_cafile;
	const char *capath = server->connrec->tls_capath;
	const char *ciphers = server->connrec->tls_ciphers;
	gboolean verify = server->connrec->tls_verify;

	g_return_val_if_fail(handle != NULL, NULL);

	if(!ssl_inited && !irssi_ssl_init())
		return NULL;

	if(!(fd = g_io_channel_unix_get_fd(handle)))
		return NULL;

	ERR_clear_error();
	ctx = SSL_CTX_new(SSLv23_client_method());
	if (ctx == NULL) {
		g_error("Could not allocate memory for SSL context");
		return NULL;
	}
	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
	SSL_CTX_set_default_passwd_cb(ctx, get_pem_password_callback);
	SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *)mypass);

	if (ciphers != NULL && ciphers[0] != '\0') {
		if (SSL_CTX_set_cipher_list(ctx, ciphers) != 1)
			g_warning("No valid SSL cipher suite could be selected");
	}

	if (mycert && *mycert) {
		char *scert = NULL, *spkey = NULL;
		FILE *fp;
		scert = convert_home(mycert);
		if (mypkey && *mypkey)
			spkey = convert_home(mypkey);

		if ((fp = fopen(scert, "r"))) {
			X509 *cert;
			/* Let's parse the certificate by hand instead of using
			 * SSL_CTX_use_certificate_file so that we can validate
			 * some parts of it. */
			cert = PEM_read_X509(fp, NULL, get_pem_password_callback, (void *)mypass);
			if (cert != NULL) {
				/* Only the expiration date is checked right now */
				if (X509_cmp_current_time(X509_get_notAfter(cert))  <= 0 ||
				    X509_cmp_current_time(X509_get_notBefore(cert)) >= 0)
					g_warning("The client certificate is expired");

				ERR_clear_error();
				if (! SSL_CTX_use_certificate(ctx, cert))
					g_warning("Loading of client certificate '%s' failed: %s", mycert, ERR_reason_error_string(ERR_get_error()));
				else if (! SSL_CTX_use_PrivateKey_file(ctx, spkey ? spkey : scert, SSL_FILETYPE_PEM))
					g_warning("Loading of private key '%s' failed: %s", mypkey ? mypkey : mycert, ERR_reason_error_string(ERR_get_error()));
				else if (! SSL_CTX_check_private_key(ctx))
					g_warning("Private key does not match the certificate");

				X509_free(cert);
			} else
				g_warning("Loading of client certificate '%s' failed: %s", mycert, ERR_reason_error_string(ERR_get_error()));

			fclose(fp);
		} else
			g_warning("Could not find client certificate '%s'", scert);
		g_free(scert);
		g_free(spkey);
	}

	if ((cafile && *cafile) || (capath && *capath)) {
		char *scafile = NULL;
		char *scapath = NULL;
		if (cafile && *cafile)
			scafile = convert_home(cafile);
		if (capath && *capath)
			scapath = convert_home(capath);
		if (! SSL_CTX_load_verify_locations(ctx, scafile, scapath)) {
			g_warning("Could not load CA list for verifying TLS server certificate");
			g_free(scafile);
			g_free(scapath);
			SSL_CTX_free(ctx);
			return NULL;
		}
		g_free(scafile);
		g_free(scapath);
		verify = TRUE;
	}
#if (OPENSSL_VERSION_NUMBER >= 0x10002000L)
	  else if (store != NULL) {
		/* Make sure to increment the refcount every time the store is
		 * used, that's essential not to get it free'd by OpenSSL when
		 * the SSL_CTX is destroyed. */
		X509_STORE_up_ref(store);
		SSL_CTX_set_cert_store(ctx, store);
	}
#else
	  else {
		if (!SSL_CTX_set_default_verify_paths(ctx))
			g_warning("Could not load default certificates");
	}
#endif

	if(!(ssl = SSL_new(ctx)))
	{
		g_warning("Failed to allocate SSL structure");
		SSL_CTX_free(ctx);
		return NULL;
	}

	if(!SSL_set_fd(ssl, fd))
	{
		g_warning("Failed to associate socket to SSL stream");
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		return NULL;
	}

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	/* SNI carries a host name only, never an address (RFC 6066) */
	if (!host_is_ip_literal(server->connrec->address))
		SSL_set_tlsext_host_name(ssl, server->connrec->address);
#endif

	SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE |
			SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	chan = g_new0(GIOSSLChannel, 1);
	chan->fd = fd;
	chan->giochan = handle;
	chan->ssl = ssl;
	chan->ctx = ctx;
	chan->server = server;
	chan->port = port;
	chan->verify = verify;

	gchan = (GIOChannel *)chan;
	gchan->funcs = &irssi_ssl_channel_funcs;
	g_io_channel_init(gchan);
	gchan->is_readable = gchan->is_writeable = TRUE;
	gchan->use_buffer = FALSE;

	return gchan;
}

static void set_cipher_info(TLS_REC *tls, SSL *ssl)
{
	g_return_if_fail(tls != NULL);
	g_return_if_fail(ssl != NULL);

	tls_rec_set_protocol_version(tls, SSL_get_version(ssl));

	tls_rec_set_cipher(tls, SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
	tls_rec_set_cipher_size(tls, SSL_get_cipher_bits(ssl, NULL));
}

static gboolean set_pubkey_info(TLS_REC *tls, X509 *cert, unsigned char *cert_fingerprint, size_t cert_fingerprint_size, unsigned char *public_key_fingerprint, size_t public_key_fingerprint_size)
{
	gboolean ret = TRUE;
	EVP_PKEY *pubkey = NULL;
	char *cert_fingerprint_hex = NULL;
	char *public_key_fingerprint_hex = NULL;

	BIO *bio = NULL;
	char buffer[128];
	ssize_t length;

	g_return_val_if_fail(tls != NULL, FALSE);
	g_return_val_if_fail(cert != NULL, FALSE);

	pubkey = X509_get_pubkey(cert);

	cert_fingerprint_hex = binary_to_hex(cert_fingerprint, cert_fingerprint_size);
	tls_rec_set_certificate_fingerprint(tls, cert_fingerprint_hex);
	tls_rec_set_certificate_fingerprint_algorithm(tls, "SHA256");

	/* Show algorithm. */
	switch (EVP_PKEY_id(pubkey)) {
		case EVP_PKEY_RSA:
			tls_rec_set_public_key_algorithm(tls, "RSA");
			break;

		case EVP_PKEY_DSA:
			tls_rec_set_public_key_algorithm(tls, "DSA");
			break;

		case EVP_PKEY_EC:
			tls_rec_set_public_key_algorithm(tls, "EC");
			break;

		default:
			tls_rec_set_public_key_algorithm(tls, "Unknown");
			break;
	}

	public_key_fingerprint_hex = binary_to_hex(public_key_fingerprint, public_key_fingerprint_size);
	tls_rec_set_public_key_fingerprint(tls, public_key_fingerprint_hex);
	tls_rec_set_public_key_size(tls, EVP_PKEY_bits(pubkey));
	tls_rec_set_public_key_fingerprint_algorithm(tls, "SHA256");

	/* Read the NotBefore timestamp. */
	bio = BIO_new(BIO_s_mem());
	ASN1_TIME_print(bio, X509_get_notBefore(cert));
	length = BIO_read(bio, buffer, sizeof(buffer));
	if (length < 0) {
		ret = FALSE;
		BIO_free(bio);
		goto done;
	}
	buffer[length] = '\0';
	BIO_free(bio);
	tls_rec_set_not_before(tls, buffer);

	/* Read the NotAfter timestamp. */
	bio = BIO_new(BIO_s_mem());
	ASN1_TIME_print(bio, X509_get_notAfter(cert));
	length = BIO_read(bio, buffer, sizeof(buffer));
	if (length < 0) {
		ret = FALSE;
		BIO_free(bio);
		goto done;
	}
	buffer[length] = '\0';
	BIO_free(bio);
	tls_rec_set_not_after(tls, buffer);

done:
	g_free(cert_fingerprint_hex);
	g_free(public_key_fingerprint_hex);
	EVP_PKEY_free(pubkey);

	return ret;
}

static void set_peer_cert_chain_info(TLS_REC *tls, SSL *ssl)
{
	int nid;
	char *key = NULL;
	char *value = NULL;
	STACK_OF(X509) *chain = NULL;
	int i;
	int j;
	TLS_CERT_REC *cert_rec = NULL;
	X509_NAME *name = NULL;
	X509_NAME_ENTRY *entry = NULL;
	TLS_CERT_ENTRY_REC *tls_cert_entry_rec = NULL;
	ASN1_STRING *data = NULL;

	g_return_if_fail(tls != NULL);
	g_return_if_fail(ssl != NULL);

	chain = SSL_get_peer_cert_chain(ssl);

	if (chain == NULL)
		return;

	for (i = 0; i < sk_X509_num(chain); i++) {
		cert_rec = tls_cert_create_rec();

		/* Subject. */
		name = X509_get_subject_name(sk_X509_value(chain, i));

		for (j = 0; j < X509_NAME_entry_count(name); j++) {
			entry = X509_NAME_get_entry(name, j);

			nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(entry));
			key = (char *)OBJ_nid2sn(nid);

			if (key == NULL)
				key = (char *)OBJ_nid2ln(nid);

			data = X509_NAME_ENTRY_get_data(entry);
			value = (char *)ASN1_STRING_data(data);

			tls_cert_entry_rec = tls_cert_entry_create_rec(key, value);
			tls_cert_rec_append_subject_entry(cert_rec, tls_cert_entry_rec);
		}

		/* Issuer. */
		name = X509_get_issuer_name(sk_X509_value(chain, i));

		for (j = 0; j < X509_NAME_entry_count(name); j++) {
			entry = X509_NAME_get_entry(name, j);

			nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(entry));
			key = (char *)OBJ_nid2sn(nid);

			if (key == NULL)
				key = (char *)OBJ_nid2ln(nid);

			data = X509_NAME_ENTRY_get_data(entry);
			value = (char *)ASN1_STRING_data(data);

			tls_cert_entry_rec = tls_cert_entry_create_rec(key, value);
			tls_cert_rec_append_issuer_entry(cert_rec, tls_cert_entry_rec);
		}

		tls_rec_append_cert(tls, cert_rec);
	}
}

static void set_server_temporary_key_info(TLS_REC *tls, SSL *ssl)
{
#ifdef SSL_get_server_tmp_key
	/* Show ephemeral key information. */
	EVP_PKEY *ephemeral_key = NULL;
	char *ephemeral_key_algorithm = NULL;

	g_return_if_fail(tls != NULL);
	g_return_if_fail(ssl != NULL);

	if (SSL_get_server_tmp_key(ssl, &ephemeral_key)) {
		int keytype = EVP_PKEY_id(ephemeral_key);
		switch (keytype) {
		case EVP_PKEY_DH:
			tls_rec_set_ephemeral_key_algorithm(tls, "DH");
			tls_rec_set_ephemeral_key_size(tls, EVP_PKEY_bits(ephemeral_key));
			break;

			/* OPENSSL_NO_EC is for solaris 11.3 (2016), github ticket #598 */
#ifndef OPENSSL_NO_EC
		case EVP_PKEY_EC: {
#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
			char cname[50];
			EVP_PKEY_get_group_name(ephemeral_key, cname, sizeof(cname), NULL);
#else
			EC_KEY *ec_key = NULL;
			char *cname = NULL;
			int nid;

			ec_key = EVP_PKEY_get1_EC_KEY(ephemeral_key);
			nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec_key));
			EC_KEY_free(ec_key);
			cname = (char *) OBJ_nid2sn(nid);
#endif
			ephemeral_key_algorithm = g_strdup_printf("ECDH: %s", cname);

			tls_rec_set_ephemeral_key_algorithm(tls, ephemeral_key_algorithm);
			tls_rec_set_ephemeral_key_size(tls, EVP_PKEY_bits(ephemeral_key));

			g_free_and_null(ephemeral_key_algorithm);
			break;
		}
#endif

		default:
			tls_rec_set_ephemeral_key_algorithm(tls, OBJ_nid2ln(keytype));
			tls_rec_set_ephemeral_key_size(tls, EVP_PKEY_bits(ephemeral_key));
			break;
		}

		EVP_PKEY_free(ephemeral_key);
	}
#endif /* SSL_get_server_tmp_key. */
}

/* TLS on a DCC connection (SDCC: DCC SCHAT, DCC SSEND). The peer is a
   person, not a server with a certificate a CA signed: the certificate is
   never verified, its SHA-256 is only shown so that both sides can compare
   it some other way. The side that accepted the TCP connection is the TLS
   server, as in Eggdrop and KVIrc. */

static int dcc_tls_accept_any(int ok, X509_STORE_CTX *store_ctx)
{
	(void) ok;
	(void) store_ctx;
	return 1;
}

static char *tls_error_text(const char *fallback)
{
	unsigned long code = ERR_get_error();
	const char *reason = code != 0 ? ERR_reason_error_string(code) : NULL;

	ERR_clear_error();
	return g_strdup(reason != NULL ? reason : fallback);
}

static gboolean is_ssl_channel(GIOChannel *handle)
{
	return handle != NULL && handle->funcs == &irssi_ssl_channel_funcs;
}

/* Load the key and certificate in cert_file into ctx. The PEM is read once,
   through safe-file (a regular file of the user's own, opened non-blocking
   so a FIFO cannot hang the main loop, not through a symbolic link), and
   parsed from that buffer: the path is not opened again under us the way
   SSL_CTX_use_certificate_chain_file() would. The first certificate is the
   leaf, any that follow are the chain, and the key may come in any order.
   FALSE with *error set on failure. */
static int dcc_ctx_use_cert(SSL_CTX *ctx, const char *cert_file, char **error)
{
	char *data = NULL;
	gsize len = 0;
	BIO *bio = NULL;
	X509 *cert = NULL, *ca;
	EVP_PKEY *pkey = NULL;
	int ok = FALSE;

	if (!safe_file_read(cert_file, SAFE_FILE_OWN, 1024 * 1024, &data, &len)) {
		*error = g_strdup_printf("the certificate %s: %s", cert_file,
					 safe_file_strerror(errno));
		return FALSE;
	}

	ERR_clear_error();
	bio = BIO_new_mem_buf(data, len);
	if (bio == NULL) {
		*error = tls_error_text("no memory for the certificate");
		goto out;
	}
	cert = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
	if (cert == NULL || SSL_CTX_use_certificate(ctx, cert) != 1) {
		*error = tls_error_text("the certificate could not be loaded");
		goto out;
	}
	/* further certificates in the file are the chain */
	SSL_CTX_clear_chain_certs(ctx);
	while ((ca = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
		if (SSL_CTX_add0_chain_cert(ctx, ca) != 1) {
			X509_free(ca);
			*error = tls_error_text("a chain certificate could not be added");
			goto out;
		}
	}
	ERR_clear_error(); /* the loop ends on a benign "no more objects" */

	BIO_free(bio);
	bio = BIO_new_mem_buf(data, len);
	if (bio == NULL) {
		*error = tls_error_text("no memory for the private key");
		goto out;
	}
	pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
	if (pkey == NULL || SSL_CTX_use_PrivateKey(ctx, pkey) != 1 ||
	    SSL_CTX_check_private_key(ctx) != 1) {
		*error = g_strdup_printf("the certificate %s: the key and certificate do not match",
					 cert_file);
		goto out;
	}
	ok = TRUE;

out:
	if (!ok)
		ERR_clear_error();
	if (bio != NULL)
		BIO_free(bio);
	X509_free(cert);
	EVP_PKEY_free(pkey);
	if (data != NULL) {
		OPENSSL_cleanse(data, len); /* the buffer held the private key */
		g_free(data);
	}
	return ok;
}

GIOChannel *net_tls_dcc_wrap(GIOChannel *handle, int accept_side,
			     const char *cert_file, int min_version,
			     char **error)
{
	GIOSSLChannel *chan;
	GIOChannel *gchan;
	SSL_CTX *ctx;
	SSL *ssl;
	int fd, proto;

	g_return_val_if_fail(handle != NULL, NULL);
	g_return_val_if_fail(error != NULL, NULL);
	*error = NULL;

	if (!ssl_inited && !irssi_ssl_init()) {
		*error = g_strdup("OpenSSL could not be initialised");
		return NULL;
	}
	fd = g_io_channel_unix_get_fd(handle);

	if (min_version >= 13) {
#ifdef TLS1_3_VERSION
		proto = TLS1_3_VERSION;
#else
		*error = g_strdup("this OpenSSL has no TLS 1.3");
		return NULL;
#endif
	} else {
		proto = TLS1_2_VERSION;
	}

	ERR_clear_error();
	ctx = SSL_CTX_new(TLS_method());
	if (ctx == NULL) {
		*error = tls_error_text("no memory for a TLS context");
		return NULL;
	}
	if (!SSL_CTX_set_min_proto_version(ctx, proto)) {
		*error = tls_error_text("the TLS version could not be set");
		SSL_CTX_free(ctx);
		return NULL;
	}
	/* ask for the peer's certificate (a client sends one only when it
	   has one), accept whatever comes: see above */
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, dcc_tls_accept_any);
#if (OPENSSL_VERSION_NUMBER >= 0x10101000L) && !defined(LIBRESSL_VERSION_NUMBER)
	/* a DCC is never resumed: no session tickets after the handshake */
	SSL_CTX_set_num_tickets(ctx, 0);
#endif
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

	if (cert_file != NULL && *cert_file != '\0') {
		if (!dcc_ctx_use_cert(ctx, cert_file, error)) {
			SSL_CTX_free(ctx);
			return NULL;
		}
	} else if (accept_side) {
		*error = g_strdup("a TLS server needs a certificate");
		SSL_CTX_free(ctx);
		return NULL;
	}

	ssl = SSL_new(ctx);
	if (ssl == NULL || !SSL_set_fd(ssl, fd)) {
		*error = tls_error_text("the TLS connection could not be set up");
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		return NULL;
	}
	if (accept_side)
		SSL_set_accept_state(ssl);
	else
		SSL_set_connect_state(ssl);
	SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE |
			SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	chan = g_new0(GIOSSLChannel, 1);
	chan->fd = fd;
	chan->giochan = handle;
	chan->ssl = ssl;
	chan->ctx = ctx;
	chan->server = NULL;
	chan->port = 0;
	chan->verify = FALSE;

	gchan = (GIOChannel *)chan;
	gchan->funcs = &irssi_ssl_channel_funcs;
	g_io_channel_init(gchan);
	gchan->is_readable = gchan->is_writeable = TRUE;
	gchan->use_buffer = FALSE;
	return gchan;
}

int net_tls_dcc_handshake(GIOChannel *handle, char **error)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
	int ret, err;

	g_return_val_if_fail(is_ssl_channel(handle), -1);
	g_return_val_if_fail(error != NULL, -1);
	*error = NULL;

	ERR_clear_error();
	ret = SSL_do_handshake(chan->ssl);
	if (ret == 1)
		return 0;

	err = SSL_get_error(chan->ssl, ret);
	switch (err) {
	case SSL_ERROR_WANT_READ:
		return 1;
	case SSL_ERROR_WANT_WRITE:
		return 3;
	case SSL_ERROR_ZERO_RETURN:
		*error = g_strdup("the other side closed the connection");
		break;
	case SSL_ERROR_SYSCALL:
		if (ERR_peek_error() == 0) {
			*error = g_strdup(ret == -1 && errno != 0 ? g_strerror(errno) :
					  "the other side closed the connection");
			break;
		}
		/* fall through */
	default:
		*error = tls_error_text("unknown TLS error");
		break;
	}
	return -1;
}

int net_tls_pending(GIOChannel *handle)
{
	if (!is_ssl_channel(handle))
		return 0;
	return SSL_pending(((GIOSSLChannel *)handle)->ssl);
}

char *net_tls_dcc_info(GIOChannel *handle)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;

	if (!is_ssl_channel(handle))
		return NULL;
	return g_strdup_printf("%s %s", SSL_get_version(chan->ssl),
			       SSL_CIPHER_get_name(SSL_get_current_cipher(chan->ssl)));
}

static char *cert_sha256(X509 *cert)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int len;

	if (!X509_digest(cert, EVP_sha256(), md, &len))
		return NULL;
	return binary_to_hex(md, len);
}

char *net_tls_dcc_peer_fingerprint(GIOChannel *handle)
{
	X509 *cert;
	char *ret;

	if (!is_ssl_channel(handle))
		return NULL;
	cert = SSL_get_peer_certificate(((GIOSSLChannel *)handle)->ssl);
	if (cert == NULL)
		return NULL;
	ret = cert_sha256(cert);
	X509_free(cert);
	return ret;
}

char *net_tls_dcc_own_fingerprint(GIOChannel *handle)
{
	X509 *cert;

	if (!is_ssl_channel(handle))
		return NULL;
	/* the certificate this side actually loaded for this connection, not
	   whatever is on disk now: a regenerated or swapped PEM under the same
	   path must not make us show an old fingerprint. SSL_get_certificate
	   returns a certificate owned by the SSL, so it is not freed here. */
	cert = SSL_get_certificate(((GIOSSLChannel *)handle)->ssl);
	if (cert == NULL)
		return NULL;
	return cert_sha256(cert);
}

/* How cert_file looks to us before a DCC: whether it is a key and
   certificate we can use, and when not, why. */
enum dcc_cert_state {
	DCC_CERT_USABLE,  /* a regular file of ours, mode 0600, holding a key
			     and a certificate that load and match */
	DCC_CERT_MISSING, /* nothing at that path */
	DCC_CERT_UNSAFE,  /* a FIFO, device, directory, symlink, a file another
			     user owns, or one others may read (*why set) */
	DCC_CERT_BROKEN,  /* a regular file of ours we could read, but not an
			     empty/truncated/mismatched/hand-edited PEM */
};

/* Inspect cert_file. The file is opened once through safe-file: with
   O_NONBLOCK so a FIFO put in its place cannot hang the main loop, checked
   on the descriptor with fstat() (anything but a regular file of the user's
   own is refused), and never through a symbolic link. The private key is
   read, then - rewound - the certificate, so a valid PEM is accepted
   whatever the order of its key and certificate. */
static enum dcc_cert_state dcc_cert_inspect(const char *cert_file, char **why)
{
	struct stat st;
	FILE *fp;
	int fd;
	EVP_PKEY *pkey;
	X509 *cert;
	int ok;

	if (why != NULL)
		*why = NULL;

	fd = safe_file_open_read(cert_file, SAFE_FILE_OWN, &st);
	if (fd == -1) {
		if (errno == ENOENT)
			return DCC_CERT_MISSING;
		if (why != NULL)
			*why = g_strdup(safe_file_strerror(errno));
		return DCC_CERT_UNSAFE;
	}
	/* a private key readable by other users is unsafe whatever it holds */
	if (st.st_mode & (S_IRWXG | S_IRWXO)) {
		close(fd);
		if (why != NULL)
			*why = g_strdup("readable by other users, needs mode 0600");
		return DCC_CERT_UNSAFE;
	}

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		close(fd);
		return DCC_CERT_BROKEN;
	}
	pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
	rewind(fp);
	cert = PEM_read_X509(fp, NULL, NULL, NULL);
	fclose(fp);

	ok = pkey != NULL && cert != NULL &&
	     X509_check_private_key(cert, pkey) == 1;
	if (!ok)
		ERR_clear_error();

	EVP_PKEY_free(pkey);
	X509_free(cert);
	return ok ? DCC_CERT_USABLE : DCC_CERT_BROKEN;
}

/* Remove "<cert_file>.tmp-XXXXXX" temporaries left behind when erssi was
   killed between g_mkstemp_full() and the link() below (each holds a
   private key, so they are worth clearing). The ".tmp-" marker is a name
   the user is very unlikely to pick, so an unrelated neighbour such as
   "<cert_file>.backup" is not matched; on top of that only a regular file
   of ours, no symlink, with no other hard link, mode 0600 and older than
   30s is removed - never a file another erssi is still writing, never one
   we do not own. Called only for the managed certificate and only when a
   fresh one is about to be made. */
static void dcc_cert_remove_stale_temps(const char *cert_file)
{
	char *dir, *base, *prefix;
	const char *name;
	GDir *d;
	size_t prefixlen;
	time_t now;

	base = g_path_get_basename(cert_file);
	prefix = g_strconcat(base, ".tmp-", NULL);
	prefixlen = strlen(prefix);
	dir = g_path_get_dirname(cert_file);

	d = g_dir_open(dir, 0, NULL);
	if (d != NULL) {
		now = time(NULL);
		while ((name = g_dir_read_name(d)) != NULL) {
			struct stat st;
			char *path;

			if (strncmp(name, prefix, prefixlen) != 0 ||
			    strlen(name) != prefixlen + 6)
				continue;

			path = g_build_filename(dir, name, NULL);
			if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
			    st.st_uid == geteuid() && st.st_nlink == 1 &&
			    (st.st_mode & 07777) == 0600 &&
			    now - st.st_mtime > 30)
				unlink(path);
			g_free(path);
		}
		g_dir_close(d);
	}

	g_free(dir);
	g_free(base);
	g_free(prefix);
}

/* Make sure cert_file is a key and certificate the DCC TLS server can use.

   managed is the certificate erssi keeps for the user (the default
   ~/.erssi/dcc-tls.pem, when dcc_tls_cert is unset): a missing, broken or
   unsafe one is replaced with a fresh self-signed P-256 key and
   certificate (ten years). A world-readable managed key may already have
   leaked, and a chmod would not undo that, so it too is renewed rather
   than kept. The new pair is written 0600 under a temporary name and
   linked into place, so a second erssi making one at the same moment
   never replaces the first, and an interrupted write never removes the
   last good copy.

   When managed is false cert_file is the certificate the user configured
   with dcc_tls_cert: it is only ever validated, never created, deleted,
   regenerated or chmod'd. If it is missing, unsafe or not a usable PEM the
   DCC is refused with a message that says why, and the file is left exactly
   as it is. */
int net_tls_dcc_make_cert(const char *cert_file, int managed, char **error)
{
	EVP_PKEY_CTX *pctx = NULL;
	EVP_PKEY *pkey = NULL;
	X509 *cert = NULL;
	X509_NAME *name;
	BIGNUM *bn = NULL;
	unsigned char serial[16];
	char *tmp = NULL;
	FILE *fp = NULL;
	int fd = -1, ok = FALSE;
	enum dcc_cert_state state;
	char *why = NULL;

	g_return_val_if_fail(cert_file != NULL, FALSE);
	g_return_val_if_fail(error != NULL, FALSE);
	*error = NULL;

	if (!ssl_inited && !irssi_ssl_init()) {
		*error = g_strdup("OpenSSL could not be initialised");
		return FALSE;
	}

	state = dcc_cert_inspect(cert_file, &why);
	if (state == DCC_CERT_USABLE) {
		g_free(why);
		return TRUE;
	}
	if (!managed) {
		/* the user's own certificate: say what is wrong and stop,
		   touching nothing */
		switch (state) {
		case DCC_CERT_MISSING:
			*error = g_strdup_printf("the certificate %s does not exist", cert_file);
			break;
		case DCC_CERT_UNSAFE:
			*error = g_strdup_printf("the certificate %s is %s", cert_file,
						 why != NULL ? why : "unsafe to use");
			break;
		default: /* DCC_CERT_BROKEN */
			*error = g_strdup_printf("the certificate %s is not a usable key and certificate",
						 cert_file);
			break;
		}
		g_free(why);
		return FALSE;
	}
	g_free(why);

	/* the managed certificate: clear our own leftover temporaries and make
	   a fresh pair */
	dcc_cert_remove_stale_temps(cert_file);

	ERR_clear_error();
	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
	if (pctx == NULL || EVP_PKEY_keygen_init(pctx) <= 0 ||
	    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0 ||
	    EVP_PKEY_keygen(pctx, &pkey) <= 0)
		goto fail;

	cert = X509_new();
	if (cert == NULL || !X509_set_version(cert, 2))
		goto fail;
	if (RAND_bytes(serial, sizeof(serial)) != 1)
		goto fail;
	serial[0] &= 0x7f; /* a positive serial number */
	bn = BN_bin2bn(serial, sizeof(serial), NULL);
	if (bn == NULL || BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert)) == NULL)
		goto fail;
	if (X509_gmtime_adj(X509_getm_notBefore(cert), -86400L) == NULL ||
	    X509_gmtime_adj(X509_getm_notAfter(cert), 3650L * 86400L) == NULL ||
	    !X509_set_pubkey(cert, pkey))
		goto fail;
	name = X509_get_subject_name(cert);
	if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
					(const unsigned char *) "erssi DCC", -1, -1, 0) ||
	    !X509_set_issuer_name(cert, name) ||
	    !X509_sign(cert, pkey, EVP_sha256()))
		goto fail;

	tmp = g_strconcat(cert_file, ".tmp-XXXXXX", NULL);
	fd = g_mkstemp_full(tmp, O_WRONLY, 0600);
	if (fd == -1) {
		*error = g_strdup_printf("%s: %s", tmp, g_strerror(errno));
		goto out;
	}
	fp = fdopen(fd, "w");
	if (fp == NULL) {
		*error = g_strdup_printf("%s: %s", tmp, g_strerror(errno));
		goto out;
	}
	fd = -1;
	if (!PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, NULL, NULL) ||
	    !PEM_write_X509(fp, cert))
		goto fail;
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
		*error = g_strdup_printf("%s: %s", tmp, g_strerror(errno));
		goto out;
	}
	fclose(fp);
	fp = NULL;
	if (link(tmp, cert_file) != 0) {
		/* Something is already at the path, or the filesystem has no
		   hard links (FAT, some FUSE). If another erssi won the race
		   and wrote a good certificate, keep it; otherwise what is
		   there is the unusable or unsafe file we set out to replace
		   (we only reach here when the existing one is not usable), so
		   rename over it atomically. */
		if (errno == EEXIST &&
		    dcc_cert_inspect(cert_file, NULL) == DCC_CERT_USABLE) {
			ok = TRUE;
			goto out;
		}
		if (rename(tmp, cert_file) != 0) {
			*error = g_strdup_printf("%s: %s", cert_file, g_strerror(errno));
			goto out;
		}
	}
	ok = TRUE;
	goto out;

fail:
	if (*error == NULL)
		*error = tls_error_text("the certificate could not be made");
out:
	if (fp != NULL)
		fclose(fp);
	if (fd != -1)
		close(fd);
	if (tmp != NULL) {
		unlink(tmp);
		g_free(tmp);
	}
	BN_free(bn);
	X509_free(cert);
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(pctx);
	return ok;
}

GIOChannel *net_connect_ip_ssl(IPADDR *ip, int port, IPADDR *my_ip, SERVER_REC *server)
{
	GIOChannel *handle, *ssl_handle;

	handle = net_connect_ip(ip, port, my_ip);
	if (handle == NULL)
		return NULL;
	ssl_handle  = irssi_ssl_get_iochannel(handle, port, server);
	if (ssl_handle == NULL)
		g_io_channel_unref(handle);
	return ssl_handle;
}

GIOChannel *net_start_ssl(SERVER_REC *server)
{
	GIOChannel *handle, *ssl_handle;

	g_return_val_if_fail(server != NULL, NULL);

	handle = net_sendbuffer_handle(server->handle);
	if (handle == NULL)
		return NULL;

	ssl_handle  = irssi_ssl_get_iochannel(handle, server->connrec->port, server);
	return ssl_handle;
}


int irssi_ssl_handshake(GIOChannel *handle)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
	int ret, err;
	const char *errstr = NULL;
	X509 *cert = NULL;
	X509_PUBKEY *pubkey = NULL;
	int pubkey_size = 0;
	unsigned char *pubkey_der = NULL;
	unsigned char *pubkey_der_tmp = NULL;
	unsigned char pubkey_fingerprint[EVP_MAX_MD_SIZE];
	unsigned int pubkey_fingerprint_size;
	unsigned char cert_fingerprint[EVP_MAX_MD_SIZE];
	unsigned int cert_fingerprint_size;
	const char *pinned_cert_fingerprint = chan->server->connrec->tls_pinned_cert;
	const char *pinned_pubkey_fingerprint = chan->server->connrec->tls_pinned_pubkey;
	TLS_REC *tls = NULL;

	ERR_clear_error();
	ret = SSL_connect(chan->ssl);
	if (ret <= 0) {
		err = SSL_get_error(chan->ssl, ret);
		switch (err) {
			case SSL_ERROR_WANT_READ:
				return 1;
			case SSL_ERROR_WANT_WRITE:
				return 3;
			case SSL_ERROR_ZERO_RETURN:
				g_warning("SSL handshake failed: %s", "server closed connection");
				return -1;
			case SSL_ERROR_SYSCALL:
				errstr = ERR_reason_error_string(ERR_get_error());
				if (errstr == NULL && ret == -1 && errno)
					errstr = strerror(errno);
				g_warning("SSL handshake failed: %s", errstr != NULL ? errstr : "server closed connection unexpectedly");
				return -1;
			default:
				errstr = ERR_reason_error_string(ERR_get_error());
				g_warning("SSL handshake failed: %s", errstr != NULL ? errstr : "unknown SSL error");
				return -1;
		}
	}

	cert = SSL_get_peer_certificate(chan->ssl);
	if (cert == NULL) {
		g_warning("TLS server supplied no certificate");
		ret = 0;
		goto done;
	}

	pubkey = X509_get_X509_PUBKEY(cert);
	if (pubkey == NULL) {
		g_warning("TLS server supplied no certificate public key");
		ret = 0;
		goto done;
	}

	if (! X509_digest(cert, EVP_sha256(), cert_fingerprint, &cert_fingerprint_size)) {
		g_warning("Unable to generate certificate fingerprint");
		ret = 0;
		goto done;
	}

	pubkey_size = i2d_X509_PUBKEY(pubkey, NULL);
	pubkey_der = pubkey_der_tmp = g_new(unsigned char, pubkey_size);
	i2d_X509_PUBKEY(pubkey, &pubkey_der_tmp);

	EVP_Digest(pubkey_der, pubkey_size, pubkey_fingerprint, &pubkey_fingerprint_size, EVP_sha256(), 0);

	tls = tls_create_rec();
	set_cipher_info(tls, chan->ssl);
	if (! set_pubkey_info(tls, cert, cert_fingerprint, cert_fingerprint_size, pubkey_fingerprint, pubkey_fingerprint_size)) {
		g_warning("Couldn't set pubkey information");
		ret = 0;
		goto done;
	}
	set_peer_cert_chain_info(tls, chan->ssl);
	set_server_temporary_key_info(tls, chan->ssl);

	/* Emit the TLS rec. */
	signal_emit("tls handshake finished", 2, chan->server, tls);

	ret = 1;

	if (pinned_cert_fingerprint != NULL && pinned_cert_fingerprint[0] != '\0') {
		ret = g_ascii_strcasecmp(pinned_cert_fingerprint, tls->certificate_fingerprint) == 0;

		if (! ret) {
			g_warning("  Pinned certificate mismatch");
			goto done;
		}
	}

	if (pinned_pubkey_fingerprint != NULL && pinned_pubkey_fingerprint[0] != '\0') {
		ret = g_ascii_strcasecmp(pinned_pubkey_fingerprint, tls->public_key_fingerprint) == 0;

		if (! ret) {
			g_warning("  Pinned public key mismatch");
			goto done;
		}
	}

	if (chan->verify) {
		ret = irssi_ssl_verify(chan->ssl, chan->ctx, chan->server->connrec->address, chan->port, cert, chan->server, tls);

		if (! ret) {
			/* irssi_ssl_verify emits a warning itself. */
			goto done;
		}
	}

done:
	tls_rec_free(tls);
	X509_free(cert);
	g_free(pubkey_der);

	return ret ? 0 : -1;
}
