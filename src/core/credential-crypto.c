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
#define CREDENTIAL_PBKDF2_ITERATIONS 100000

/* An encrypted value is "erssi-enc1:" + salt_hex:iv_hex:ciphertext_base64.
 * Without the prefix a value is plain text, even when it holds ':' (a
 * password may). Versions before 2026.10.3 wrote the same value without
 * the prefix; it is still read when it has exactly that shape (see
 * credential_legacy_encrypted()), which a password does not by chance. */
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

static unsigned char *hex_to_bytes(const char *hex, size_t *out_len)
{
	size_t hex_len, i;
	unsigned char *bytes;
	
	g_return_val_if_fail(hex != NULL, NULL);
	g_return_val_if_fail(out_len != NULL, NULL);
	
	hex_len = strlen(hex);
	if (hex_len % 2 != 0) {
		return NULL;
	}
	
	*out_len = hex_len / 2;
	bytes = g_malloc(*out_len);
	
	for (i = 0; i < *out_len; i++) {
		if (sscanf(hex + i * 2, "%2hhx", &bytes[i]) != 1) {
			g_free(bytes);
			return NULL;
		}
	}
	
	return bytes;
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

static char *encrypt_aes256_cbc_openssl(const char *plaintext, const unsigned char *key,
                                        const unsigned char *iv)
{
	EVP_CIPHER_CTX *ctx;
	unsigned char *ciphertext;
	char *result;
	int len, ciphertext_len;
	size_t plaintext_len;
	
	g_return_val_if_fail(plaintext != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);
	g_return_val_if_fail(iv != NULL, NULL);
	
	plaintext_len = strlen(plaintext);
	ciphertext = g_malloc(plaintext_len + 16); /* AES block size */
	
	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		g_free(ciphertext);
		return NULL;
	}
	
	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		g_free(ciphertext);
		return NULL;
	}
	
	if (EVP_EncryptUpdate(ctx, ciphertext, &len, (unsigned char*)plaintext, plaintext_len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		g_free(ciphertext);
		return NULL;
	}
	ciphertext_len = len;
	
	if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		g_free(ciphertext);
		return NULL;
	}
	ciphertext_len += len;
	
	EVP_CIPHER_CTX_free(ctx);

	/* Convert to base64 */
	result = g_base64_encode(ciphertext, ciphertext_len);
	g_free(ciphertext);

	return result;
}

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
static gboolean credential_legacy_encrypted(const char *value)
{
	const char *b64;
	size_t len, pad, i, decoded;

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
	return g_str_has_prefix(value, CREDENTIAL_PREFIX) || credential_legacy_encrypted(value);
}

char *credential_encrypt(const char *plaintext, const char *password)
{
	unsigned char salt[CREDENTIAL_SALT_SIZE];
	unsigned char key[CREDENTIAL_KEY_SIZE];
	unsigned char iv[CREDENTIAL_IV_SIZE];
	char *salt_hex, *iv_hex, *encrypted, *result;
	
	g_return_val_if_fail(plaintext != NULL, NULL);
	g_return_val_if_fail(password != NULL, NULL);

	/* Generate salt and IV */
	if (!generate_salt_openssl(salt, sizeof(salt)) ||
	    !generate_salt_openssl(iv, sizeof(iv))) {
		return NULL;
	}

	/* Derive key from password */
	if (!derive_key_openssl(password, salt, sizeof(salt), key, sizeof(key))) {
		return NULL;
	}

	/* Encrypt */
	encrypted = encrypt_aes256_cbc_openssl(plaintext, key, iv);
	if (encrypted == NULL) {
		return NULL;
	}

	/* Convert salt and IV to hex */
	salt_hex = bytes_to_hex(salt, sizeof(salt));
	iv_hex = bytes_to_hex(iv, sizeof(iv));

	result = g_strdup_printf(CREDENTIAL_PREFIX "%s:%s:%s", salt_hex, iv_hex, encrypted);

	/* Secure cleanup */
	memset(key, 0, sizeof(key));
	g_free(salt_hex);
	g_free(iv_hex);
	g_free(encrypted);
	
	return result;
}

char *credential_decrypt(const char *encrypted_data, const char *password)
{
	char **parts;
	unsigned char *salt, *iv;
	unsigned char key[CREDENTIAL_KEY_SIZE];
	size_t salt_len, iv_len;
	char *result;
	
	g_return_val_if_fail(encrypted_data != NULL, NULL);
	g_return_val_if_fail(password != NULL, NULL);

	/* a value that is not encrypted is plain text, ':' or not */
	if (!credential_is_encrypted(encrypted_data))
		return g_strdup(encrypted_data);
	if (g_str_has_prefix(encrypted_data, CREDENTIAL_PREFIX))
		encrypted_data += strlen(CREDENTIAL_PREFIX);

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
	
	return result;
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