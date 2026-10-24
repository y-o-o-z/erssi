/*
 scram.c : irssi

    Copyright (C) 2023 Patrick Okraku

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
#include <irssi/src/irc/core/scram.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#define NONCE_LENGTH 18
#define CLIENT_KEY "Client Key"
#define SERVER_KEY "Server Key"

/* The iteration count i= of the server-first-message. RFC 5802 and 7677
 * ask for at least 4096, which Ergo uses; Atheme (the services behind
 * SASL on Libera.Chat and many networks running Solanum, InspIRCd or
 * UnrealIRCd, which leave SCRAM to the services) defaults to 64000 and
 * lets a network set anything from 10000 to 5000000. The upper bound here
 * is 10000000, so that every Atheme setting logs in, with room to spare.
 * The client computes PBKDF2 over i rounds in the main loop, so a hostile
 * or broken server can still make one login stall erssi: 10000000 rounds
 * take about 5 seconds with SHA-1 or SHA-256 and about 15 seconds with
 * SHA-512 on a slow machine. It cannot ask for more than that, nor make
 * the count wrap around or turn negative. Anything outside this range is
 * refused. */
#define SCRAM_MIN_ITERATIONS 4096
#define SCRAM_MAX_ITERATIONS 10000000

// EVP_MD_CTX_create() and EVP_MD_CTX_destroy() were renamed in OpenSSL 1.1.0
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
#define EVP_MD_CTX_new(ctx) EVP_MD_CTX_create(ctx)
#define EVP_MD_CTX_free(ctx) EVP_MD_CTX_destroy(ctx)
#endif

SCRAM_SESSION_REC *scram_session_create(const char *digest, const char *username,
                                        const char *password)
{
	SCRAM_SESSION_REC *session;
	const EVP_MD *md;
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
	OpenSSL_add_all_algorithms();
#endif
	md = EVP_get_digestbyname(digest);

	if (md == NULL) {
		// Unknown message digest
		return NULL;
	}

	session = g_new0(SCRAM_SESSION_REC, 1);
	session->digest = md;
	session->digest_size = EVP_MD_size(md);
	session->username = g_strdup(username);
	session->password = g_strdup(password);
	return session;
}

void scram_session_free(SCRAM_SESSION_REC *session)
{
	if (session == NULL) {
		return;
	}

	g_free(session->username);
	g_free(session->password);
	g_free(session->client_nonce_b64);
	g_free(session->client_first_message_bare);
	g_free(session->salted_password);
	g_free(session->auth_message);
	g_free(session->error);

	g_free(session);
}

static int create_nonce(void *buffer, size_t length)
{
	return RAND_bytes(buffer, length);
}

static int create_SHA(SCRAM_SESSION_REC *session, const unsigned char *input, size_t input_len,
                      unsigned char *output, unsigned int *output_len)
{
	EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();

	if (md_ctx == NULL) {
		session->error = g_strdup("Message digest allocation failed");
		return SCRAM_ERROR;
	}

	if (!EVP_DigestInit_ex(md_ctx, session->digest, NULL)) {
		session->error = g_strdup("Message digest initialization failed");
		EVP_MD_CTX_free(md_ctx);
		return SCRAM_ERROR;
	}

	if (!EVP_DigestUpdate(md_ctx, input, input_len)) {
		session->error = g_strdup("Message digest update failed");
		EVP_MD_CTX_free(md_ctx);
		return SCRAM_ERROR;
	}

	if (!EVP_DigestFinal_ex(md_ctx, output, output_len)) {
		session->error = g_strdup("Message digest finalization failed");
		EVP_MD_CTX_free(md_ctx);
		return SCRAM_ERROR;
	}

	EVP_MD_CTX_free(md_ctx);
	return SCRAM_IN_PROGRESS;
}

static scram_status process_client_first(SCRAM_SESSION_REC *session, char **output,
                                         size_t *output_len)
{
	char nonce[NONCE_LENGTH];

	if (create_nonce(nonce, NONCE_LENGTH) != 1) {
		session->error = g_strdup("Could not create client nonce");
		return SCRAM_ERROR;
	}

	session->client_nonce_b64 = g_base64_encode((guchar *) nonce, NONCE_LENGTH);
	*output = g_strdup_printf("n,,n=%s,r=%s", session->username, session->client_nonce_b64);
	*output_len = strlen(*output);
	session->client_first_message_bare = g_strdup(*output + 3);
	session->step++;
	return SCRAM_IN_PROGRESS;
}

/* The value of i=: decimal digits only, nothing else, within the range
 * above; 0 when it is anything else. */
unsigned int scram_parse_iteration_count(const char *str)
{
	unsigned long value = 0;
	const char *p;

	if (*str == '\0')
		return 0;
	for (p = str; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p))
			return 0;
		value = value * 10 + (*p - '0');
		if (value > SCRAM_MAX_ITERATIONS)
			return 0;
	}
	if (value < SCRAM_MIN_ITERATIONS)
		return 0;
	return value;
}

/* TRUE when str is padded base64 that decodes to at least one byte */
static gboolean is_base64(const char *str)
{
	size_t len, pad, i;

	len = strlen(str);
	if (len == 0 || len % 4 != 0)
		return FALSE;
	pad = 0;
	if (str[len - 1] == '=')
		pad = str[len - 2] == '=' ? 2 : 1;
	for (i = 0; i < len - pad; i++) {
		if (!g_ascii_isalnum(str[i]) && str[i] != '+' && str[i] != '/')
			return FALSE;
	}
	return len / 4 * 3 - pad > 0;
}

/* HMAC(key, data) into out, which holds digest_size bytes */
static gboolean scram_hmac(SCRAM_SESSION_REC *session, const unsigned char *key,
                           size_t key_len, const unsigned char *data, size_t data_len,
                           unsigned char *out)
{
	unsigned int out_len = 0;

	if (HMAC(session->digest, key, key_len, data, data_len, out, &out_len) == NULL ||
	    out_len != session->digest_size) {
		session->error = g_strdup("HMAC computation failed");
		return FALSE;
	}
	return TRUE;
}

static scram_status process_server_first(SCRAM_SESSION_REC *session, const char *data,
                                         char **output, size_t *output_len)
{
	char **params, *client_final_message_without_proof, *salt, *server_nonce_b64,
	    *client_proof_b64, *iterations;
	unsigned char *client_key, stored_key[EVP_MAX_MD_SIZE], *client_signature, *client_proof;
	unsigned int i, param_count, iteration_count, stored_key_len;
	gsize salt_len = 0;
	size_t client_nonce_len;
	scram_status status = SCRAM_ERROR;

	params = g_strsplit(data, ",", -1);
	param_count = g_strv_length(params);

	if (param_count < 3) {
		/* Invalid server-first-message */
		session->error = g_strdup_printf("%s", data);
		g_strfreev(params);
		return SCRAM_ERROR;
	}

	server_nonce_b64 = NULL;
	salt = NULL;
	iterations = NULL;

	for (i = 0; i < param_count; i++) {
		if (!strncmp(params[i], "m=", 2)) {
			/* a mandatory extension this client does not know */
			session->error = g_strdup_printf("Unsupported SCRAM extension in "
			                                 "server-first-message: %s", data);
			g_strfreev(params);
			g_free(server_nonce_b64);
			g_free(salt);
			g_free(iterations);
			return SCRAM_ERROR;
		} else if (!strncmp(params[i], "r=", 2)) {
			g_free(server_nonce_b64);
			server_nonce_b64 = g_strdup(params[i] + 2);
		} else if (!strncmp(params[i], "s=", 2)) {
			g_free(salt);
			salt = g_strdup(params[i] + 2);
		} else if (!strncmp(params[i], "i=", 2)) {
			g_free(iterations);
			iterations = g_strdup(params[i] + 2);
		}
	}

	g_strfreev(params);

	if (server_nonce_b64 == NULL || *server_nonce_b64 == '\0' || salt == NULL ||
	    !is_base64(salt) || iterations == NULL) {
		session->error = g_strdup_printf("Invalid server-first-message: %s", data);
		g_free(server_nonce_b64);
		g_free(salt);
		g_free(iterations);
		return SCRAM_ERROR;
	}

	iteration_count = scram_parse_iteration_count(iterations);
	if (iteration_count == 0) {
		session->error = g_strdup_printf("Refusing SCRAM iteration count i=%s: it must be "
		                                 "a number from %d to %d",
		                                 iterations, SCRAM_MIN_ITERATIONS,
		                                 SCRAM_MAX_ITERATIONS);
		g_free(server_nonce_b64);
		g_free(salt);
		g_free(iterations);
		return SCRAM_ERROR;
	}
	g_free(iterations);

	client_nonce_len = strlen(session->client_nonce_b64);

	// The server can append his nonce to the client's nonce
	if (strlen(server_nonce_b64) < client_nonce_len ||
	    strncmp(server_nonce_b64, session->client_nonce_b64, client_nonce_len)) {
		session->error = g_strdup_printf("Invalid server nonce: %s", server_nonce_b64);
		g_free(server_nonce_b64);
		g_free(salt);
		return SCRAM_ERROR;
	}

	g_base64_decode_inplace((gchar *) salt, &salt_len);

	// SaltedPassword := Hi(Normalize(password), salt, i)
	session->salted_password = g_malloc(session->digest_size);

	if (PKCS5_PBKDF2_HMAC(session->password, strlen(session->password), (unsigned char *) salt,
	                      salt_len, iteration_count, session->digest, session->digest_size,
	                      session->salted_password) != 1) {
		session->error = g_strdup("PBKDF2 computation failed");
		g_free(server_nonce_b64);
		g_free(salt);
		return SCRAM_ERROR;
	}

	// AuthMessage := client-first-message-bare + "," +
	//                server-first-message + "," +
	//                client-final-message-without-proof
	client_final_message_without_proof = g_strdup_printf("c=biws,r=%s", server_nonce_b64);

	session->auth_message = g_strdup_printf("%s,%s,%s", session->client_first_message_bare,
	                                        data, client_final_message_without_proof);

	client_key = g_malloc0(session->digest_size);
	client_signature = g_malloc0(session->digest_size);
	client_proof = g_malloc0(session->digest_size);

	// ClientKey := HMAC(SaltedPassword, "Client Key")
	if (!scram_hmac(session, session->salted_password, session->digest_size,
	                (unsigned char *) CLIENT_KEY, strlen(CLIENT_KEY), client_key))
		goto out;

	// StoredKey := H(ClientKey)
	if (!create_SHA(session, client_key, session->digest_size, stored_key, &stored_key_len))
		goto out;

	// ClientSignature := HMAC(StoredKey, AuthMessage)
	if (!scram_hmac(session, stored_key, stored_key_len,
	                (unsigned char *) session->auth_message, strlen(session->auth_message),
	                client_signature))
		goto out;

	// ClientProof := ClientKey XOR ClientSignature
	for (i = 0; i < session->digest_size; i++) {
		client_proof[i] = client_key[i] ^ client_signature[i];
	}

	client_proof_b64 = g_base64_encode((guchar *) client_proof, session->digest_size);

	*output = g_strdup_printf("%s,p=%s", client_final_message_without_proof, client_proof_b64);
	*output_len = strlen(*output);
	g_free(client_proof_b64);

	session->step++;
	status = SCRAM_IN_PROGRESS;

out:
	g_free(server_nonce_b64);
	g_free(salt);
	g_free(client_final_message_without_proof);
	g_free(client_key);
	g_free(client_signature);
	g_free(client_proof);
	return status;
}

static scram_status process_server_final(SCRAM_SESSION_REC *session, const char *data)
{
	char *verifier;
	unsigned char *server_key, *server_signature;
	gsize verifier_len = 0;
	scram_status status = SCRAM_ERROR;

	if (g_str_has_prefix(data, "e=")) {
		session->error = g_strdup_printf("Server reported an error: %s", data + 2);
		return SCRAM_ERROR;
	}
	if (!g_str_has_prefix(data, "v=") || !is_base64(data + 2)) {
		session->error = g_strdup_printf("Invalid server-final-message: %s", data);
		return SCRAM_ERROR;
	}

	verifier = g_strdup(data + 2);
	g_base64_decode_inplace(verifier, &verifier_len);

	server_key = g_malloc0(session->digest_size);
	server_signature = g_malloc0(session->digest_size);

	// ServerKey := HMAC(SaltedPassword, "Server Key")
	// ServerSignature := HMAC(ServerKey, AuthMessage)
	if (scram_hmac(session, session->salted_password, session->digest_size,
	               (unsigned char *) SERVER_KEY, strlen(SERVER_KEY), server_key) &&
	    scram_hmac(session, server_key, session->digest_size,
	               (unsigned char *) session->auth_message, strlen(session->auth_message),
	               server_signature)) {
		if (verifier_len == session->digest_size &&
		    CRYPTO_memcmp(verifier, server_signature, verifier_len) == 0)
			status = SCRAM_SUCCESS;
		else
			session->error = g_strdup("The server signature does not match: the "
			                          "server does not know the password");
	}

	g_free(verifier);
	g_free(server_key);
	g_free(server_signature);
	return status;
}

scram_status scram_process(SCRAM_SESSION_REC *session, const char *input, char **output,
                           size_t *output_len)
{
	scram_status status;

	switch (session->step) {
	case 0:
		status = process_client_first(session, output, output_len);
		break;
	case 1:
		status = process_server_first(session, input, output, output_len);
		break;
	case 2:
		status = process_server_final(session, input);
		break;
	default:
		*output = NULL;
		*output_len = 0;
		status = SCRAM_ERROR;
		break;
	}

	return status;
}
