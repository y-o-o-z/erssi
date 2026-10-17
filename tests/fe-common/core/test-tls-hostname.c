/*
 * The check of a server's TLS certificate against the address erssi
 * connected to (irssi_ssl_verify_hostname() in core/network-openssl.c,
 * compiled into this test), with certificates made here: a server given
 * by name matches a dNSName (or, without any, the CommonName), one given
 * as an IPv4 or IPv6 address matches an iPAddress subjectAltName, and
 * everything else is refused.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/network-openssl.c>

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

static EVP_PKEY *key;

/* a self-signed certificate with the CommonName cn (or none) and the
 * subjectAltName san in openssl.cnf syntax (or none) */
static X509 *make_cert(const char *cn, const char *san)
{
	X509 *cert;
	X509_NAME *name;
	X509V3_CTX v3;

	cert = X509_new();
	g_assert_nonnull(cert);
	g_assert_cmpint(X509_set_version(cert, 2), ==, 1);
	ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
	X509_gmtime_adj(X509_getm_notBefore(cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
	g_assert_cmpint(X509_set_pubkey(cert, key), ==, 1);

	name = X509_get_subject_name(cert);
	if (cn != NULL)
		g_assert_cmpint(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
		                                           (const unsigned char *) cn, -1, -1, 0),
		                ==, 1);
	g_assert_cmpint(X509_set_issuer_name(cert, name), ==, 1);

	if (san != NULL) {
		X509_EXTENSION *ext;

		X509V3_set_ctx_nodb(&v3);
		X509V3_set_ctx(&v3, cert, cert, NULL, NULL, 0);
		ext = X509V3_EXT_nconf_nid(NULL, &v3, NID_subject_alt_name, san);
		g_assert_nonnull(ext);
		g_assert_cmpint(X509_add_ext(cert, ext, -1), ==, 1);
		X509_EXTENSION_free(ext);
	}

	g_assert_cmpint(X509_sign(cert, key, NULL), >, 0);
	return cert;
}

typedef struct {
	const char *host;
	gboolean match;
} HostCase;

static void check(const char *cn, const char *san, const HostCase *cases, int count)
{
	X509 *cert = make_cert(cn, san);
	int i;

	for (i = 0; i < count; i++) {
		gboolean matched = irssi_ssl_verify_hostname(cert, cases[i].host);

		if (matched != cases[i].match)
			g_test_message("CN=%s SAN=%s host %s: %s", cn ? cn : "-", san ? san : "-",
			               cases[i].host, matched ? "matched" : "refused");
		g_assert_cmpint(matched, ==, cases[i].match);
	}
	X509_free(cert);
}

static void test_dns_only(void)
{
	static const HostCase cases[] = {
		{ "irc.example.net", TRUE },
		{ "IRC.Example.NET", TRUE },
		{ "irc2.example.net", FALSE },
		{ "example.net", FALSE },
		{ "irc.example.net.evil.org", FALSE },
		{ "127.0.0.1", FALSE },
		{ "::1", FALSE },
		/* the CommonName does not count next to a dNSName */
		{ "cn.example.org", FALSE },
	};

	check("cn.example.org", "DNS:irc.example.net", cases, G_N_ELEMENTS(cases));
}

static void test_wildcard(void)
{
	static const HostCase cases[] = {
		{ "irc.example.net", TRUE },
		{ "a.b.example.net", FALSE },
		{ "example.net", FALSE },
	};
	static const HostCase partial[] = {
		{ "irc.example.net", FALSE },
	};
	static const HostCase top_level[] = {
		{ "example.com", FALSE },
	};

	check(NULL, "DNS:*.example.net", cases, G_N_ELEMENTS(cases));
	/* only a whole leftmost label may be a wildcard */
	check(NULL, "DNS:i*.example.net", partial, G_N_ELEMENTS(partial));
	/* nor stand for a whole domain under a top-level one */
	check(NULL, "DNS:*.com", top_level, G_N_ELEMENTS(top_level));
}

static void test_ip_only(void)
{
	static const HostCase cases[] = {
		{ "127.0.0.1", TRUE },
		{ "127.0.0.2", FALSE },
		{ "::ffff:127.0.0.1", FALSE },
		{ "localhost", FALSE },
		/* without a dNSName the CommonName stands for the name (RFC 6125
		 * 6.4.4) */
		{ "cn.example.org", TRUE },
	};

	check("cn.example.org", "IP:127.0.0.1", cases, G_N_ELEMENTS(cases));
}

static void test_ipv6(void)
{
	static const HostCase cases[] = {
		{ "::1", TRUE },
		{ "0:0:0:0:0:0:0:1", TRUE },
		{ "2001:db8::7", TRUE },
		{ "2001:DB8:0:0:0:0:0:7", TRUE },
		{ "2001:db8::8", FALSE },
		{ "127.0.0.1", FALSE },
	};

	check(NULL, "IP:::1,IP:2001:db8::7", cases, G_N_ELEMENTS(cases));
}

static void test_both(void)
{
	static const HostCase cases[] = {
		{ "irc.example.net", TRUE },
		{ "192.0.2.7", TRUE },
		{ "2001:db8::7", TRUE },
		{ "192.0.2.8", FALSE },
		{ "other.example.net", FALSE },
	};

	check("irc.example.net", "DNS:irc.example.net,IP:192.0.2.7,IP:2001:db8::7",
	      cases, G_N_ELEMENTS(cases));
}

/* without any subjectAltName the CommonName is used, also when it is the
 * address itself (as earlier versions accepted); an address in the
 * CommonName does not count when the certificate lists iPAddress names */
static void test_common_name(void)
{
	static const HostCase name[] = {
		{ "irc.example.net", TRUE },
		{ "irc2.example.net", FALSE },
	};
	static const HostCase address[] = {
		{ "192.0.2.7", TRUE },
		{ "192.0.2.8", FALSE },
	};
	static const HostCase listed[] = {
		{ "192.0.2.7", FALSE },
		{ "192.0.2.9", TRUE },
	};
	static const HostCase none[] = {
		{ "irc.example.net", FALSE },
		{ "192.0.2.7", FALSE },
	};

	check("irc.example.net", NULL, name, G_N_ELEMENTS(name));
	check("192.0.2.7", NULL, address, G_N_ELEMENTS(address));
	check("192.0.2.7", "IP:192.0.2.9", listed, G_N_ELEMENTS(listed));
	check(NULL, NULL, none, G_N_ELEMENTS(none));
}

int main(int argc, char **argv)
{
	EVP_PKEY_CTX *ctx;
	int ret;

	g_test_init(&argc, &argv, NULL);
	/* a refused certificate is logged with g_warning(), which is no
	 * failure here */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
	g_assert_nonnull(ctx);
	g_assert_cmpint(EVP_PKEY_keygen_init(ctx), ==, 1);
	g_assert_cmpint(EVP_PKEY_keygen(ctx, &key), ==, 1);
	EVP_PKEY_CTX_free(ctx);

	g_test_add_func("/tls/hostname/dns_only", test_dns_only);
	g_test_add_func("/tls/hostname/wildcard", test_wildcard);
	g_test_add_func("/tls/hostname/ip_only", test_ip_only);
	g_test_add_func("/tls/hostname/ipv6", test_ipv6);
	g_test_add_func("/tls/hostname/dns_and_ip", test_both);
	g_test_add_func("/tls/hostname/common_name", test_common_name);

	ret = g_test_run();
	EVP_PKEY_free(key);
	return ret;
}
