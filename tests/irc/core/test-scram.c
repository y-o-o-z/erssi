/*
 * SCRAM-SHA-* (irc/core/scram.c) against a server side computed here as
 * RFC 5802 describes it: a correct exchange logs in, and the client
 * refuses an iteration count that is not a plain decimal number in its
 * range (it would otherwise compute PBKDF2 for as long as the server
 * asks, or go on after the computation failed), a server-final-message
 * that does not start with "v=" or carries the wrong signature, a server
 * error "e=", a nonce that does not continue its own, and a malformed
 * salt.
 */
#include <irssi/src/common.h>
#include <irssi/src/irc/core/scram.h>

#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#define USER "user"
#define PASSWORD "pencil"
#define SALT_B64 "W22ZaJ0SNY7soEsUEjb6gQ=="

typedef struct {
	const char *digest;
	SCRAM_SESSION_REC *session;
	char *client_first_bare;
	char *nonce;          /* client nonce + server part */
	char *server_first;
	unsigned int iterations;
} Exchange;

/* step 0: the client-first-message */
static void exchange_start(Exchange *ex, const char *digest)
{
	char *output = NULL, *r;
	size_t output_len = 0;

	memset(ex, 0, sizeof(*ex));
	ex->digest = digest;
	ex->session = scram_session_create(digest, USER, PASSWORD);
	g_assert_nonnull(ex->session);
	g_assert_cmpint(scram_process(ex->session, NULL, &output, &output_len), ==,
	                SCRAM_IN_PROGRESS);
	g_assert_true(g_str_has_prefix(output, "n,,n=" USER ",r="));
	g_assert_cmpuint(output_len, ==, strlen(output));
	ex->client_first_bare = g_strdup(output + 3);
	r = strstr(output, ",r=") + 3;
	ex->nonce = g_strconcat(r, "srvNonce42", NULL);
	g_free(output);
}

static void exchange_free(Exchange *ex)
{
	scram_session_free(ex->session);
	g_free(ex->client_first_bare);
	g_free(ex->nonce);
	g_free(ex->server_first);
}

/* step 1 with a server-first-message; returns the status and, when the
 * client answered, its client-final-message */
static scram_status exchange_server_first(Exchange *ex, const char *server_first,
                                          char **client_final)
{
	size_t output_len = 0;
	scram_status status;

	*client_final = NULL;
	g_free(ex->server_first);
	ex->server_first = g_strdup(server_first);
	status = scram_process(ex->session, server_first, client_final, &output_len);
	if (status != SCRAM_IN_PROGRESS)
		*client_final = NULL;
	return status;
}

static void hmac(const EVP_MD *md, const unsigned char *key, size_t key_len,
                 const void *data, size_t data_len, unsigned char *out)
{
	unsigned int len;

	g_assert_nonnull(HMAC(md, key, key_len, data, data_len, out, &len));
	g_assert_cmpuint(len, ==, EVP_MD_size(md));
}

/* checks the client's proof as a server does and returns the
 * server-final-message "v=..." */
static char *server_verify(Exchange *ex, const char *client_final)
{
	const EVP_MD *md = EVP_get_digestbyname(ex->digest);
	int size = EVP_MD_size(md);
	unsigned char salted[EVP_MAX_MD_SIZE], client_key[EVP_MAX_MD_SIZE];
	unsigned char stored_key[EVP_MAX_MD_SIZE], signature[EVP_MAX_MD_SIZE];
	unsigned char server_key[EVP_MAX_MD_SIZE], proof_expected[EVP_MAX_MD_SIZE];
	unsigned int stored_len;
	guchar *salt, *proof;
	gsize salt_len, proof_len;
	char *without_proof, *auth_message, *expected, *result;
	const char *p;
	int i;

	p = strstr(client_final, ",p=");
	g_assert_nonnull(p);
	without_proof = g_strndup(client_final, p - client_final);
	expected = g_strdup_printf("c=biws,r=%s", ex->nonce);
	g_assert_cmpstr(without_proof, ==, expected);
	g_free(expected);

	salt = g_base64_decode(SALT_B64, &salt_len);
	g_assert_cmpint(PKCS5_PBKDF2_HMAC(PASSWORD, strlen(PASSWORD), salt, salt_len,
	                                  ex->iterations, md, size, salted), ==, 1);
	g_free(salt);
	auth_message = g_strdup_printf("%s,%s,%s", ex->client_first_bare, ex->server_first,
	                               without_proof);

	hmac(md, salted, size, "Client Key", 10, client_key);
	g_assert_cmpint(EVP_Digest(client_key, size, stored_key, &stored_len, md, NULL), ==, 1);
	hmac(md, stored_key, stored_len, auth_message, strlen(auth_message), signature);
	for (i = 0; i < size; i++)
		proof_expected[i] = client_key[i] ^ signature[i];
	proof = g_base64_decode(p + 3, &proof_len);
	g_assert_cmpuint(proof_len, ==, size);
	g_assert_cmpmem(proof, proof_len, proof_expected, size);
	g_free(proof);

	hmac(md, salted, size, "Server Key", 10, server_key);
	hmac(md, server_key, size, auth_message, strlen(auth_message), signature);
	expected = g_base64_encode(signature, size);
	result = g_strconcat("v=", expected, NULL);

	g_free(expected);
	g_free(auth_message);
	g_free(without_proof);
	return result;
}

static char *server_first_message(Exchange *ex, const char *iterations)
{
	return g_strdup_printf("r=%s,s=" SALT_B64 ",i=%s", ex->nonce, iterations);
}

/* a whole exchange, as a server that knows the password answers it */
static void test_login(gconstpointer data)
{
	const char *digest = data;
	Exchange ex;
	char *server_first, *client_final, *server_final;
	size_t len = 0;
	char *out = NULL;

	exchange_start(&ex, digest);
	ex.iterations = 4096;
	server_first = server_first_message(&ex, "4096");
	g_assert_cmpint(exchange_server_first(&ex, server_first, &client_final), ==,
	                SCRAM_IN_PROGRESS);
	server_final = server_verify(&ex, client_final);
	g_assert_cmpint(scram_process(ex.session, server_final, &out, &len), ==, SCRAM_SUCCESS);
	g_assert_null(ex.session->error);

	g_free(server_first);
	g_free(client_final);
	g_free(server_final);
	exchange_free(&ex);
}

/* the count Atheme uses by default logs in too */
static void test_iterations_atheme(gconstpointer data)
{
	const char *count = data;
	Exchange ex;
	char *server_first, *client_final, *server_final;
	size_t len = 0;
	char *out = NULL;

	exchange_start(&ex, "SHA256");
	ex.iterations = atoi(count);
	server_first = server_first_message(&ex, count);
	g_assert_cmpint(exchange_server_first(&ex, server_first, &client_final), ==,
	                SCRAM_IN_PROGRESS);
	server_final = server_verify(&ex, client_final);
	g_assert_cmpint(scram_process(ex.session, server_final, &out, &len), ==, SCRAM_SUCCESS);

	g_free(server_first);
	g_free(client_final);
	g_free(server_final);
	exchange_free(&ex);
}

/* the largest count Atheme can be set to (5000000) and the upper bound
 * itself are taken. Checked on the parser alone: a whole login at 5000000
 * rounds computes PBKDF2 twice (client and test server), which took the
 * test past its time limit on a loaded sanitizer build. */
static void test_iterations_accepted(void)
{
	static const char *const counts[] = {
		"4096", "64000", "5000000", "10000000",
	};
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS(counts); i++)
		g_assert_cmpuint(scram_parse_iteration_count(counts[i]), ==,
		                 (unsigned int) atoi(counts[i]));
	g_assert_cmpuint(scram_parse_iteration_count("10000001"), ==, 0);
	g_assert_cmpuint(scram_parse_iteration_count("4095"), ==, 0);
}

static void test_iterations_refused(void)
{
	static const char *const counts[] = {
		"", "0", "1", "4095", "10000001", "2147483648", "4294967297",
		"99999999999999999999999", "-4096", "+4096", " 4096", "4096 ",
		"4096x", "0x1000", "4e3", "abc",
	};
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS(counts); i++) {
		Exchange ex;
		char *server_first, *client_final;
		gint64 start = g_get_monotonic_time();

		exchange_start(&ex, "SHA256");
		server_first = server_first_message(&ex, counts[i]);
		g_test_message("i=%s", counts[i]);
		g_assert_cmpint(exchange_server_first(&ex, server_first, &client_final), ==,
		                SCRAM_ERROR);
		g_assert_null(client_final);
		g_assert_nonnull(ex.session->error);
		g_assert_nonnull(strstr(ex.session->error, "iteration count"));
		/* refused at once, without computing anything */
		g_assert_cmpint(g_get_monotonic_time() - start, <, G_USEC_PER_SEC);
		g_free(server_first);
		exchange_free(&ex);
	}

	/* no i= at all */
	{
		Exchange ex;
		char *server_first, *client_final;

		exchange_start(&ex, "SHA256");
		server_first = g_strdup_printf("r=%s,s=" SALT_B64 ",x=1", ex.nonce);
		g_assert_cmpint(exchange_server_first(&ex, server_first, &client_final), ==,
		                SCRAM_ERROR);
		g_assert_nonnull(ex.session->error);
		g_free(server_first);
		exchange_free(&ex);
	}
}

/* a server-first-message the client cannot use */
static void test_server_first_refused(void)
{
	static const char *const formats[] = {
		/* a nonce that is not the client's own followed by the server's */
		"r=someoneElsesNonce,s=" SALT_B64 ",i=4096",
		/* a salt that is not base64 */
		"r=%s,s=W22Z*J0SNY7soEsUEjb6gQ==,i=4096",
		"r=%s,s=W22ZaJ0,i=4096",
		"r=%s,s=,i=4096",
		/* a mandatory extension */
		"m=ext,r=%s,s=" SALT_B64 ",i=4096",
	};
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS(formats); i++) {
		Exchange ex;
		char *server_first, *client_final;

		exchange_start(&ex, "SHA256");
		server_first = g_strdup_printf(formats[i], ex.nonce);
		g_test_message("%s", server_first);
		g_assert_cmpint(exchange_server_first(&ex, server_first, &client_final), ==,
		                SCRAM_ERROR);
		g_assert_null(client_final);
		g_assert_nonnull(ex.session->error);
		g_free(server_first);
		exchange_free(&ex);
	}
}

/* only "v=" with the right signature logs in */
static void test_server_final(void)
{
	char *finals[6];
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS(finals); i++) {
		Exchange ex;
		char *server_first, *client_final, *right, *out = NULL;
		size_t len = 0;

		exchange_start(&ex, "SHA256");
		ex.iterations = 4096;
		server_first = server_first_message(&ex, "4096");
		g_assert_cmpint(exchange_server_first(&ex, server_first, &client_final), ==,
		                SCRAM_IN_PROGRESS);
		right = server_verify(&ex, client_final);

		switch (i) {
		case 0: /* the right signature after another key */
			finals[i] = g_strconcat("x=", right + 2, NULL);
			break;
		case 1:
			finals[i] = g_strconcat("vv", right + 2, NULL);
			break;
		case 2: /* one bit off */
			finals[i] = g_strdup(right);
			finals[i][2] = finals[i][2] == 'A' ? 'B' : 'A';
			break;
		case 3: /* cut short */
			finals[i] = g_strndup(right, strlen(right) - 4);
			break;
		case 4:
			finals[i] = g_strdup("e=invalid-proof");
			break;
		case 5:
			finals[i] = g_strdup("v=");
			break;
		}
		g_test_message("server-final %s", finals[i]);
		g_assert_cmpint(scram_process(ex.session, finals[i], &out, &len), ==, SCRAM_ERROR);
		g_assert_nonnull(ex.session->error);
		if (i == 4)
			g_assert_nonnull(strstr(ex.session->error, "invalid-proof"));

		g_free(finals[i]);
		g_free(right);
		g_free(server_first);
		g_free(client_final);
		exchange_free(&ex);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_data_func("/scram/login/sha1", "SHA1", test_login);
	g_test_add_data_func("/scram/login/sha256", "SHA256", test_login);
	g_test_add_data_func("/scram/login/sha512", "SHA512", test_login);
	g_test_add_data_func("/scram/iterations/atheme_default", "64000",
	                     test_iterations_atheme);
	g_test_add_func("/scram/iterations/accepted", test_iterations_accepted);
	g_test_add_func("/scram/iterations/refused", test_iterations_refused);
	g_test_add_func("/scram/server_first/refused", test_server_first_refused);
	g_test_add_func("/scram/server_final", test_server_final);

	return g_test_run();
}
