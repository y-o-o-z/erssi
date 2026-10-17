/*
 credential-crypto.c : Cryptographic functions for credential management

    Copyright (C) 2024 Irssi Project
    Copyright (C) 2024-2025 erssi-org team
    Lead Developer: Jerzy (kofany) Dąbrowski <https://github.com/kofany>

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
#include <irssi/src/core/credential.h>

#include <string.h>
#include <glib.h>

/* OpenSSL is always available in Irssi (dependency in meson.build) */
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

/* Cryptographic constants */
#define CREDENTIAL_SALT_SIZE 32
#define CREDENTIAL_KEY_SIZE 32
#define CREDENTIAL_IV_SIZE 16
#define CREDENTIAL_NONCE_SIZE 12
#define CREDENTIAL_TAG_SIZE 16
#define CREDENTIAL_PBKDF2_ITERATIONS 100000

/* An encrypted value is "erssi-enc1:" + salt_hex:nonce_hex:base64, where
 * the base64 holds the AES-256-GCM ciphertext followed by its 16-byte tag.
 * The tag covers the ciphertext and, as additional data, everything before
 * it ("erssi-enc1:salt_hex:nonce_hex"), so a value that was changed, or a
 * wrong master password, fails to decrypt instead of giving other text.
 * Without the prefix a value is plain text, even when it holds ':' (a
 * password may).
 *
 * Versions before 2026.10.3 wrote salt_hex:iv_hex:base64 of AES-256-CBC
 * with no prefix and no tag. Such a value is still read when it has
 * exactly that shape (see credential_is_legacy_encrypted()), which a
 * password does not by chance, but a change to it cannot be detected;
 * credential_set() and the config write hook encrypt it again in the
 * current form when the master password decrypts it. */
#define CREDENTIAL_PREFIX "erssi-enc1:"

/* === Funkcje pomocnicze === */

static char *bytes_to_hex(const unsigned char *bytes, size_t len)
{
	char *hex;
	size_t i;
	
	hex = g_malloc(len * 2 + 1);
	for (i = 0; i < len; i++) {
		g_snprintf(hex + i * 2, 3, "%02x", bytes[i]);
	}
	hex[len * 2] = '\0';
	
	return hex;
}

/* the first hex_len characters of hex, which must be hex digits */
static unsigned char *hex_to_bytes_n(const char *hex, size_t hex_len, size_t *out_len)
{
	size_t i;
	unsigned char *bytes;

	g_return_val_if_fail(hex != NULL, NULL);
	g_return_val_if_fail(out_len != NULL, NULL);

	if (hex_len % 2 != 0) {
		return NULL;
	}

	*out_len = hex_len / 2;
	bytes = g_malloc(*out_len);

	for (i = 0; i < *out_len; i++) {
		int hi = g_ascii_xdigit_value(hex[i * 2]);
		int lo = g_ascii_xdigit_value(hex[i * 2 + 1]);

		if (hi < 0 || lo < 0) {
			g_free(bytes);
			return NULL;
		}
		bytes[i] = (hi << 4) | lo;
	}

	return bytes;
}

static unsigned char *hex_to_bytes(const char *hex, size_t *out_len)
{
	g_return_val_if_fail(hex != NULL, NULL);
	return hex_to_bytes_n(hex, strlen(hex), out_len);
}

/* === Funkcje kryptograficzne OpenSSL === */

static gboolean generate_salt_openssl(unsigned char *salt, size_t salt_len)
{
	return RAND_bytes(salt, salt_len) == 1;
}

static gboolean derive_key_openssl(const char *password, const unsigned char *salt, 
                                   size_t salt_len, unsigned char *key, size_t key_len)
{
	return PKCS5_PBKDF2_HMAC(password, strlen(password), salt, salt_len,
	                         CREDENTIAL_PBKDF2_ITERATIONS, EVP_sha256(),
	                         key_len, key) == 1;
}

/* AES-256-GCM over plaintext with aad as additional data; the result is
 * the base64 of ciphertext + tag */
static char *encrypt_aes256_gcm_openssl(const char *plaintext, const unsigned char *key,
                                        const unsigned char *nonce, const char *aad)
{
	EVP_CIPHER_CTX *ctx;
	unsigned char *out;
	char *result = NULL;
	int len, out_len;
	size_t plaintext_len;

	plaintext_len = strlen(plaintext);
	out = g_malloc(plaintext_len + CREDENTIAL_TAG_SIZE);

	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		g_free(out);
		return NULL;
	}

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CREDENTIAL_NONCE_SIZE, NULL) != 1 ||
	    EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1 ||
	    EVP_EncryptUpdate(ctx, NULL, &len, (const unsigned char *) aad, strlen(aad)) != 1 ||
	    EVP_EncryptUpdate(ctx, out, &len, (const unsigned char *) plaintext,
	                      plaintext_len) != 1)
		goto out;
	out_len = len;
	if (EVP_EncryptFinal_ex(ctx, out + out_len, &len) != 1)
		goto out;
	out_len += len;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CREDENTIAL_TAG_SIZE,
	                        out + out_len) != 1)
		goto out;
	out_len += CREDENTIAL_TAG_SIZE;

	result = g_base64_encode(out, out_len);

out:
	EVP_CIPHER_CTX_free(ctx);
	memset(out, 0, plaintext_len + CREDENTIAL_TAG_SIZE);
	g_free(out);
	return result;
}

/* the reverse of encrypt_aes256_gcm_openssl(): NULL when the tag does not
 * match, that is when anything covered by it was changed or the key is
 * wrong */
static char *decrypt_aes256_gcm_openssl(const char *data_b64, const unsigned char *key,
                                        const unsigned char *nonce, const char *aad)
{
	EVP_CIPHER_CTX *ctx;
	unsigned char *data, *plaintext;
	char *result = NULL;
	int len, plaintext_len;
	gsize data_len, ciphertext_len;

	data = g_base64_decode(data_b64, &data_len);
	if (data == NULL || data_len < CREDENTIAL_TAG_SIZE || data_len > G_MAXINT) {
		g_free(data);
		return NULL;
	}
	ciphertext_len = data_len - CREDENTIAL_TAG_SIZE;
	plaintext = g_malloc(ciphertext_len + 1);

	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL)
		goto out;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CREDENTIAL_NONCE_SIZE, NULL) != 1 ||
	    EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1 ||
	    EVP_DecryptUpdate(ctx, NULL, &len, (const unsigned char *) aad, strlen(aad)) != 1 ||
	    EVP_DecryptUpdate(ctx, plaintext, &len, data, ciphertext_len) != 1)
		goto out;
	plaintext_len = len;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, CREDENTIAL_TAG_SIZE,
	                        data + ciphertext_len) != 1)
		goto out;
	/* checks the tag */
	if (EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len, &len) != 1)
		goto out;
	plaintext_len += len;

	/* a password with a NUL in it would be cut short where it is used */
	if (memchr(plaintext, '\0', plaintext_len) != NULL)
		goto out;
	plaintext[plaintext_len] = '\0';
	result = g_strdup((char *) plaintext);

out:
	EVP_CIPHER_CTX_free(ctx);
	memset(plaintext, 0, ciphertext_len + 1);
	g_free(plaintext);
	g_free(data);
	return result;
}

/* Only reads values written by versions before 2026.10.3. */
static char *decrypt_aes256_cbc_openssl(const char *ciphertext_b64, const unsigned char *key,
                                        const unsigned char *iv)
{
	EVP_CIPHER_CTX *ctx;
	unsigned char *ciphertext, *plaintext;
	char *result;
	int len, plaintext_len;
	gsize ciphertext_len;
	
	g_return_val_if_fail(ciphertext_b64 != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);
	g_return_val_if_fail(iv != NULL, NULL);

	/* Decode from base64 */
	ciphertext = g_base64_decode(ciphertext_b64, &ciphertext_len);
	if (ciphertext == NULL) {
		return NULL;
	}
	
	plaintext = g_malloc(ciphertext_len + 16);
	
	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		g_free(ciphertext);
		g_free(plaintext);
		return NULL;
	}
	
	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		g_free(ciphertext);
		g_free(plaintext);
		return NULL;
	}
	
	if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		g_free(ciphertext);
		g_free(plaintext);
		return NULL;
	}
	plaintext_len = len;
	
	if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		g_free(ciphertext);
		g_free(plaintext);
		return NULL;
	}
	plaintext_len += len;
	
	EVP_CIPHER_CTX_free(ctx);
	g_free(ciphertext);

	/* Null-terminate and return as string */
	result = g_malloc(plaintext_len + 1);
	memcpy(result, plaintext, plaintext_len);
	result[plaintext_len] = '\0';
	g_free(plaintext);

	return result;
}

static gboolean is_lower_hex(const char *str, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (!g_ascii_isxdigit(str[i]) || g_ascii_isupper(str[i]))
			return FALSE;
	}
	return TRUE;
}

/* A value written by an older version: the salt and IV in lower-case hex
 * of their exact sizes, then the AES-256-CBC ciphertext in padded base64,
 * which decodes to a whole number of 16-byte blocks. */
gboolean credential_is_legacy_encrypted(const char *value)
{
	const char *b64;
	size_t len, pad, i, decoded;

	if (value == NULL)
		return FALSE;
	if (strlen(value) < CREDENTIAL_SALT_SIZE * 2 + 1 + CREDENTIAL_IV_SIZE * 2 + 1)
		return FALSE;
	if (!is_lower_hex(value, CREDENTIAL_SALT_SIZE * 2) ||
	    value[CREDENTIAL_SALT_SIZE * 2] != ':')
		return FALSE;
	value += CREDENTIAL_SALT_SIZE * 2 + 1;
	if (!is_lower_hex(value, CREDENTIAL_IV_SIZE * 2) || value[CREDENTIAL_IV_SIZE * 2] != ':')
		return FALSE;
	b64 = value + CREDENTIAL_IV_SIZE * 2 + 1;

	len = strlen(b64);
	if (len == 0 || len % 4 != 0)
		return FALSE;
	pad = 0;
	if (b64[len - 1] == '=')
		pad = b64[len - 2] == '=' ? 2 : 1;
	for (i = 0; i < len - pad; i++) {
		if (!g_ascii_isalnum(b64[i]) && b64[i] != '+' && b64[i] != '/')
			return FALSE;
	}
	decoded = len / 4 * 3 - pad;
	return decoded >= 16 && decoded % 16 == 0;
}

/* === Public functions === */

gboolean credential_is_encrypted(const char *value)
{
	if (value == NULL)
		return FALSE;
	return g_str_has_prefix(value, CREDENTIAL_PREFIX) || credential_is_legacy_encrypted(value);
}

char *credential_encrypt(const char *plaintext, const char *password)
{
	unsigned char salt[CREDENTIAL_SALT_SIZE];
	unsigned char key[CREDENTIAL_KEY_SIZE];
	unsigned char nonce[CREDENTIAL_NONCE_SIZE];
	char *salt_hex, *nonce_hex, *header, *encrypted, *result;

	g_return_val_if_fail(plaintext != NULL, NULL);
	g_return_val_if_fail(password != NULL, NULL);

	/* A fresh salt gives a fresh key, and a fresh random nonce with it */
	if (!generate_salt_openssl(salt, sizeof(salt)) ||
	    !generate_salt_openssl(nonce, sizeof(nonce))) {
		return NULL;
	}

	/* Derive key from password */
	if (!derive_key_openssl(password, salt, sizeof(salt), key, sizeof(key))) {
		memset(key, 0, sizeof(key));
		return NULL;
	}

	salt_hex = bytes_to_hex(salt, sizeof(salt));
	nonce_hex = bytes_to_hex(nonce, sizeof(nonce));
	header = g_strdup_printf(CREDENTIAL_PREFIX "%s:%s", salt_hex, nonce_hex);

	encrypted = encrypt_aes256_gcm_openssl(plaintext, key, nonce, header);
	result = encrypted == NULL ? NULL : g_strdup_printf("%s:%s", header, encrypted);

	/* Secure cleanup */
	memset(key, 0, sizeof(key));
	g_free(salt_hex);
	g_free(nonce_hex);
	g_free(header);
	g_free(encrypted);

	return result;
}

/* a value in the current form: "erssi-enc1:" salt_hex:nonce_hex:base64 */
static char *decrypt_current(const char *encrypted_data, const char *password)
{
	const char *salt_hex, *nonce_hex, *data_b64;
	unsigned char *salt, *nonce;
	unsigned char key[CREDENTIAL_KEY_SIZE];
	size_t salt_len, nonce_len;
	char *header, *result;

	salt_hex = encrypted_data + strlen(CREDENTIAL_PREFIX);
	if (strlen(salt_hex) < CREDENTIAL_SALT_SIZE * 2 + 1 + CREDENTIAL_NONCE_SIZE * 2 + 1)
		return NULL;
	nonce_hex = salt_hex + CREDENTIAL_SALT_SIZE * 2 + 1;
	data_b64 = nonce_hex + CREDENTIAL_NONCE_SIZE * 2 + 1;
	if (!is_lower_hex(salt_hex, CREDENTIAL_SALT_SIZE * 2) ||
	    salt_hex[CREDENTIAL_SALT_SIZE * 2] != ':' ||
	    !is_lower_hex(nonce_hex, CREDENTIAL_NONCE_SIZE * 2) ||
	    nonce_hex[CREDENTIAL_NONCE_SIZE * 2] != ':')
		return NULL;

	header = g_strndup(encrypted_data, data_b64 - 1 - encrypted_data);
	salt = hex_to_bytes_n(salt_hex, CREDENTIAL_SALT_SIZE * 2, &salt_len);
	nonce = hex_to_bytes_n(nonce_hex, CREDENTIAL_NONCE_SIZE * 2, &nonce_len);

	result = NULL;
	if (salt != NULL && nonce != NULL &&
	    derive_key_openssl(password, salt, salt_len, key, sizeof(key)))
		result = decrypt_aes256_gcm_openssl(data_b64, key, nonce, header);

	memset(key, 0, sizeof(key));
	g_free(header);
	g_free(salt);
	g_free(nonce);
	return result;
}

/* a value written by a version before 2026.10.3: salt_hex:iv_hex:base64
 * of AES-256-CBC, without a tag */
static char *decrypt_legacy(const char *encrypted_data, const char *password)
{
	static gboolean warned = FALSE;
	char **parts;
	unsigned char *salt, *iv;
	unsigned char key[CREDENTIAL_KEY_SIZE];
	size_t salt_len, iv_len;
	char *result;

	/* Parse salt:iv:encrypted */
	parts = g_strsplit(encrypted_data, ":", 3);
	if (!parts[0] || !parts[1] || !parts[2]) {
		g_strfreev(parts);
		return NULL;
	}

	/* Convert hex to bytes */
	salt = hex_to_bytes(parts[0], &salt_len);
	iv = hex_to_bytes(parts[1], &iv_len);

	if (salt == NULL || iv == NULL ||
	    salt_len != CREDENTIAL_SALT_SIZE ||
	    iv_len != CREDENTIAL_IV_SIZE) {
		g_strfreev(parts);
		g_free(salt);
		g_free(iv);
		return NULL;
	}

	/* Derive key from password */
	if (!derive_key_openssl(password, salt, salt_len, key, sizeof(key))) {
		memset(key, 0, sizeof(key));
		g_strfreev(parts);
		g_free(salt);
		g_free(iv);
		return NULL;
	}

	/* Decrypt */
	result = decrypt_aes256_cbc_openssl(parts[2], key, iv);

	/* Secure cleanup */
	memset(key, 0, sizeof(key));
	g_strfreev(parts);
	g_free(salt);
	g_free(iv);

	if (result != NULL && !warned) {
		warned = TRUE;
		g_warning("A credential is stored in the encrypted form of erssi "
		          "versions before 2026.10.3, which cannot tell whether it was "
		          "changed.");
	}
	return result;
}

char *credential_decrypt(const char *encrypted_data, const char *password)
{
	g_return_val_if_fail(encrypted_data != NULL, NULL);
	g_return_val_if_fail(password != NULL, NULL);

	if (g_str_has_prefix(encrypted_data, CREDENTIAL_PREFIX))
		return decrypt_current(encrypted_data, password);
	if (credential_is_legacy_encrypted(encrypted_data))
		return decrypt_legacy(encrypted_data, password);
	/* a value that is not encrypted is plain text, ':' or not */
	return g_strdup(encrypted_data);
}

gboolean credential_crypto_init(void)
{
	/* OpenSSL doesn't require special initialization in newer versions */
	return TRUE;
}

void credential_crypto_deinit(void)
{
	/* OpenSSL doesn't require special cleanup */
}