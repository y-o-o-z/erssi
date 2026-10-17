/*
 * Tests for the encrypted form of core/credential.c and credential-crypto.c:
 * an encrypted value carries the "erssi-enc1:" prefix and anything without
 * it is plain text, so a password with ':' in it is neither left
 * unencrypted nor taken for an encrypted value. The value is AES-256-GCM,
 * so a changed value or a wrong master password does not decrypt. Values
 * written by older versions (AES-256-CBC, salt_hex:iv_hex:base64 without
 * the prefix) are still read, and written again in the current form.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/credential.h>
#include <irssi/src/lib-config/iconfig.h>

#include <string.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#define PREFIX "erssi-enc1:"
#define MASTER "master secret"

/* written by erssi 2026.10.2 (and by `openssl enc -aes-256-cbc` with the
 * same key): "old:secret" under MASTER */
#define LEGACY_VECTOR \
	"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f:" \
	"6465666768696a6b6c6d6e6f70717273:WJyhOjhJzUeXuRAamdBfOQ=="

/* what versions before 2026.10.3 wrote: PBKDF2-SHA256 (100000 rounds) of
 * the master password, AES-256-CBC, salt_hex:iv_hex:base64, no prefix */
static char *legacy_encrypt(const char *plain)
{
	unsigned char salt[32], iv[16], key[32], out[256];
	char salt_hex[65], iv_hex[33], *b64, *legacy;
	EVP_CIPHER_CTX *ctx;
	int len, out_len, i;

	g_assert_cmpuint(strlen(plain), <, sizeof(out) - 16);
	g_assert_cmpint(RAND_bytes(salt, sizeof(salt)), ==, 1);
	g_assert_cmpint(RAND_bytes(iv, sizeof(iv)), ==, 1);
	g_assert_cmpint(PKCS5_PBKDF2_HMAC(MASTER, strlen(MASTER), salt, sizeof(salt), 100000,
	                                  EVP_sha256(), sizeof(key), key), ==, 1);
	ctx = EVP_CIPHER_CTX_new();
	g_assert_cmpint(EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv), ==, 1);
	g_assert_cmpint(EVP_EncryptUpdate(ctx, out, &len, (const unsigned char *) plain,
	                                  strlen(plain)), ==, 1);
	out_len = len;
	g_assert_cmpint(EVP_EncryptFinal_ex(ctx, out + out_len, &len), ==, 1);
	out_len += len;
	EVP_CIPHER_CTX_free(ctx);

	for (i = 0; i < 32; i++)
		g_snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
	for (i = 0; i < 16; i++)
		g_snprintf(iv_hex + i * 2, 3, "%02x", iv[i]);
	b64 = g_base64_encode(out, out_len);
	legacy = g_strdup_printf("%s:%s:%s", salt_hex, iv_hex, b64);
	g_free(b64);
	g_assert_false(g_str_has_prefix(legacy, PREFIX));
	return legacy;
}

static void test_is_encrypted(void)
{
	static const char *const plain[] = {
		"", "hunter2", "pa:ss", "a:b:c", "user:pass:extra:more",
		"/msg NickServ identify me pa:ss",
		/* the shape of an old value, but not quite */
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:"
		"0123456789abcdef0123456789abcdef:YWJj",           /* 3 bytes */
		"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef:"
		"0123456789abcdef0123456789abcdef:AAAAAAAAAAAAAAAAAAAAAA==", /* upper case */
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde:"
		"0123456789abcdef0123456789abcdef:AAAAAAAAAAAAAAAAAAAAAA==", /* short salt */
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:"
		"0123456789abcdef0123456789abcdef:AAAAAAAAAAAAAAAAAAAAAA=",  /* bad padding */
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:"
		"0123456789abcdef0123456789abcdef:AAAAAAAAAAAAAAA:AAAAAA==", /* not base64 */
	};
	char *value;
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS(plain); i++) {
		if (credential_is_encrypted(plain[i]))
			g_test_message("taken as encrypted: \"%s\"", plain[i]);
		g_assert_false(credential_is_encrypted(plain[i]));
	}
	g_assert_false(credential_is_encrypted(NULL));

	g_assert_true(credential_is_encrypted(
	    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:"
	    "0123456789abcdef0123456789abcdef:AAAAAAAAAAAAAAAAAAAAAA=="));

	value = credential_encrypt("pa:ss", MASTER);
	g_assert_true(g_str_has_prefix(value, PREFIX));
	g_assert_true(credential_is_encrypted(value));
	g_free(value);

	value = legacy_encrypt("pa:ss");
	g_assert_true(credential_is_encrypted(value));
	g_free(value);
}

static void test_decrypt(void)
{
	static const char *const passwords[] = { "hunter2", "pa:ss", ":", "a:b:c:", "zażółć:gęślą" };
	int i;

	/* the first value read in the old form says once that it cannot be
	 * checked */
	{
		char *plain;

		g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*before 2026.10.3*cannot tell*HELP CREDENTIAL*");
		plain = credential_decrypt(LEGACY_VECTOR, MASTER);
		g_test_assert_expected_messages();
		g_assert_cmpstr(plain, ==, "old:secret");
		g_free(plain);
	}

	for (i = 0; i < (int) G_N_ELEMENTS(passwords); i++) {
		char *value, *plain;

		value = credential_encrypt(passwords[i], MASTER);
		plain = credential_decrypt(value, MASTER);
		g_assert_cmpstr(plain, ==, passwords[i]);
		g_free(plain);
		g_free(value);

		/* written by an older version */
		value = legacy_encrypt(passwords[i]);
		plain = credential_decrypt(value, MASTER);
		g_assert_cmpstr(plain, ==, passwords[i]);
		g_free(plain);
		g_free(value);

		/* plain text stays as it is */
		plain = credential_decrypt(passwords[i], MASTER);
		g_assert_cmpstr(plain, ==, passwords[i]);
		g_free(plain);
	}

	/* a wrong master password does not decrypt */
	{
		char *value = credential_encrypt("pa:ss", MASTER);

		g_assert_null(credential_decrypt(value, "another one"));
		g_assert_null(credential_decrypt(value, ""));
		g_free(value);
	}

	/* an empty password round-trips too */
	{
		char *value = credential_encrypt("", MASTER);
		char *plain = credential_decrypt(value, MASTER);

		g_assert_cmpstr(plain, ==, "");
		g_free(plain);
		g_free(value);
	}
}

/* a value written by an older version decrypts to what it held */
static void test_legacy_vector(void)
{
	char *plain;

	g_assert_true(credential_is_encrypted(LEGACY_VECTOR));
	g_assert_true(credential_is_legacy_encrypted(LEGACY_VECTOR));
	plain = credential_decrypt(LEGACY_VECTOR, MASTER);
	g_assert_cmpstr(plain, ==, "old:secret");
	g_free(plain);

	/* the current form is not the old one */
	plain = credential_encrypt("old:secret", MASTER);
	g_assert_false(credential_is_legacy_encrypted(plain));
	g_assert_false(credential_is_legacy_encrypted(plain + strlen(PREFIX)));
	g_free(plain);
	g_assert_false(credential_is_legacy_encrypted("old:secret"));
	g_assert_false(credential_is_legacy_encrypted(NULL));
}

static void flip_base64_byte(char *b64, gsize index)
{
	guchar *data;
	gsize len;
	char *again;

	data = g_base64_decode(b64, &len);
	g_assert_cmpuint(index, <, len);
	data[index] ^= 0x01;
	again = g_base64_encode(data, len);
	g_assert_cmpuint(strlen(again), ==, strlen(b64));
	memcpy(b64, again, strlen(again));
	g_free(again);
	g_free(data);
}

static void flip_hex_digit(char *hex)
{
	*hex = *hex == '0' ? '1' : '0';
}

/* a change to any part of an encrypted value makes it fail to decrypt,
 * not decrypt to other text */
static void test_tamper(void)
{
	const char *plain_text = "pa:ss-and-some-more";
	char *value, *copy, *salt, *nonce, *b64, *plain;
	gsize data_len;
	guchar *data;

	value = credential_encrypt(plain_text, MASTER);
	g_assert_nonnull(value);
	salt = value + strlen(PREFIX);
	nonce = strchr(salt, ':') + 1;
	b64 = strchr(nonce, ':') + 1;
	g_assert_cmpuint(nonce - salt, ==, 64 + 1);
	g_assert_cmpuint(b64 - nonce, ==, 24 + 1);
	data = g_base64_decode(b64, &data_len);
	g_assert_cmpuint(data_len, ==, strlen(plain_text) + 16);
	g_free(data);

	plain = credential_decrypt(value, MASTER);
	g_assert_cmpstr(plain, ==, plain_text);
	g_free(plain);

#define TAMPERED(change) do { \
		copy = g_strdup(value); \
		change; \
		g_assert_cmpstr(copy, !=, value); \
		g_assert_true(credential_is_encrypted(copy)); \
		g_assert_null(credential_decrypt(copy, MASTER)); \
		g_free(copy); \
	} while (0)

	/* the salt (gives another key), the nonce, the ciphertext, the tag */
	TAMPERED(flip_hex_digit(copy + (salt - value)));
	TAMPERED(flip_hex_digit(copy + (salt - value) + 63));
	TAMPERED(flip_hex_digit(copy + (nonce - value)));
	TAMPERED(flip_hex_digit(copy + (nonce - value) + 23));
	TAMPERED(flip_base64_byte(copy + (b64 - value), 0));
	TAMPERED(flip_base64_byte(copy + (b64 - value), strlen(plain_text) - 1));
	TAMPERED(flip_base64_byte(copy + (b64 - value), strlen(plain_text)));
	TAMPERED(flip_base64_byte(copy + (b64 - value), data_len - 1));
	/* cut short, and the tag alone without the ciphertext */
	TAMPERED(copy[strlen(copy) - 4] = '\0');
	TAMPERED(copy[b64 - value] = '\0');
	/* the salt in upper case is not the form that was written */
	TAMPERED(copy[salt - value] = 'A');
#undef TAMPERED

	g_assert_null(credential_decrypt(PREFIX, MASTER));
	g_assert_null(credential_decrypt(PREFIX "00:00:AAAA", MASTER));
	g_free(value);
}

static const char *stored_value(const char *network, CredentialContext context)
{
	GSList *list, *tmp;
	const char *value = NULL;

	list = credential_list();
	for (tmp = list; tmp != NULL; tmp = tmp->next) {
		CREDENTIAL_REC *rec = tmp->data;

		if (rec->context == context && strcmp(rec->network, network) == 0)
			value = rec->encrypted_value;
	}
	g_slist_free(list);
	return value;
}

/* a plain password with ':' round-trips through credential_set() and
 * credential_get(), with encryption off and on */
static void test_set_get(void)
{
	char *value;

	credential_config_encrypt = FALSE;
	credential_clear_master_password();
	g_assert_true(credential_set("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD, "pa:ss"));
	g_assert_cmpstr(stored_value("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD), ==, "pa:ss");
	value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	g_assert_cmpstr(value, ==, "pa:ss");
	g_free(value);

	/* a master password alone does not decrypt plain text */
	credential_set_master_password(MASTER);
	value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	g_assert_cmpstr(value, ==, "pa:ss");
	g_free(value);

	/* with encryption on the value is stored encrypted */
	credential_config_encrypt = TRUE;
	g_assert_true(credential_set("Net", CREDENTIAL_CONTEXT_SASL_USERNAME, "user:name"));
	g_assert_true(g_str_has_prefix(stored_value("Net", CREDENTIAL_CONTEXT_SASL_USERNAME),
	                               PREFIX));
	value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_USERNAME);
	g_assert_cmpstr(value, ==, "user:name");
	g_free(value);

	/* a value encrypted by an older version is read, and kept in the
	 * current form */
	{
		char *legacy = legacy_encrypt("old:secret");
		const char *stored;

		g_assert_true(credential_set("Old", CREDENTIAL_CONTEXT_SERVER_PASSWORD, legacy));
		stored = stored_value("Old", CREDENTIAL_CONTEXT_SERVER_PASSWORD);
		g_assert_true(g_str_has_prefix(stored, PREFIX));
		value = credential_get("Old", CREDENTIAL_CONTEXT_SERVER_PASSWORD);
		g_assert_cmpstr(value, ==, "old:secret");
		g_free(value);
		g_free(legacy);
	}

	/* without the master password it is kept as it is */
	{
		credential_clear_master_password();
		g_assert_true(credential_set("Old2", CREDENTIAL_CONTEXT_SERVER_PASSWORD,
		                             LEGACY_VECTOR));
		g_assert_cmpstr(stored_value("Old2", CREDENTIAL_CONTEXT_SERVER_PASSWORD), ==,
		                LEGACY_VECTOR);
		credential_set_master_password(MASTER);
		value = credential_get("Old2", CREDENTIAL_CONTEXT_SERVER_PASSWORD);
		g_assert_cmpstr(value, ==, "old:secret");
		g_free(value);
		credential_remove("Old2", CREDENTIAL_CONTEXT_SERVER_PASSWORD);
	}

	credential_remove("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	credential_remove("Net", CREDENTIAL_CONTEXT_SASL_USERNAME);
	credential_remove("Old", CREDENTIAL_CONTEXT_SERVER_PASSWORD);
	credential_config_encrypt = FALSE;
	credential_clear_master_password();
}

/* the config hooks encrypt a plain password with ':' on save and decrypt
 * it again */
static void test_config_hooks(void)
{
	CONFIG_REC *config;
	CONFIG_NODE *node;
	const char *value;
	char *legacy;

	config = config_open(NULL, -1);
	g_assert_cmpint(config_parse_data(config,
	                                  "servers = ( { address = \"irc.example.net\"; "
	                                  "chatnet = \"Net\"; password = \"srv:pass\"; } );\n"
	                                  "chatnets = { Net = { sasl_username = \"me\"; "
	                                  "sasl_password = \"pa:ss\"; }; };\n",
	                                  "test"),
	                ==, 0);

	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_config_encrypt = TRUE;
	credential_set_master_password(MASTER);

	legacy = legacy_encrypt("/msg NickServ identify old:secret");
	node = config_node_traverse(config, "chatnets/Net", FALSE);
	config_node_set_str(config, node, "autosendcmd", legacy);

	/* nothing confirms the master password yet: the old value stays */
	{
		CONFIG_REC *unconfirmed = config_open(NULL, -1);

		g_assert_cmpint(config_parse_data(unconfirmed, "chatnets = { Net = {}; };\n",
		                                  "test"), ==, 0);
		node = config_node_traverse(unconfirmed, "chatnets/Net", FALSE);
		config_node_set_str(unconfirmed, node, "autosendcmd", legacy);
		credential_config_write_hook(unconfirmed);
		g_assert_cmpstr(config_node_get_str(node, "autosendcmd", NULL), ==, legacy);
		config_close(unconfirmed);
	}
	g_free(legacy);

	/* a value in the current form that decrypts confirms it */
	{
		char *current = credential_encrypt("other secret", MASTER);

		node = config_node_traverse(config, "chatnets", FALSE);
		node = config_node_section(config, node, "Other", NODE_TYPE_BLOCK);
		config_node_set_str(config, node, "sasl_password", current);
		g_free(current);
	}

	credential_config_write_hook(config);
	node = config_node_traverse(config, "chatnets/Net", FALSE);
	g_assert_nonnull(node);
	value = config_node_get_str(node, "sasl_password", NULL);
	g_assert_true(g_str_has_prefix(value, PREFIX));
	g_assert_true(g_str_has_prefix(config_node_get_str(node, "sasl_username", NULL), PREFIX));
	/* the value an older version encrypted is written in the current form */
	g_assert_true(g_str_has_prefix(config_node_get_str(node, "autosendcmd", NULL), PREFIX));
	node = config_node_traverse(config, "servers", FALSE);
	node = config_node_nth(node, 0);
	g_assert_true(g_str_has_prefix(config_node_get_str(node, "password", NULL), PREFIX));

	credential_config_read_hook(config);
	node = config_node_traverse(config, "chatnets/Net", FALSE);
	g_assert_cmpstr(config_node_get_str(node, "autosendcmd", NULL), ==,
	                "/msg NickServ identify old:secret");
	g_assert_cmpstr(config_node_get_str(node, "sasl_password", NULL), ==, "pa:ss");
	g_assert_cmpstr(config_node_get_str(node, "sasl_username", NULL), ==, "me");
	node = config_node_nth(config_node_traverse(config, "servers", FALSE), 0);
	g_assert_cmpstr(config_node_get_str(node, "password", NULL), ==, "srv:pass");

	credential_config_encrypt = FALSE;
	credential_clear_master_password();
	config_close(config);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/credential/is_encrypted", test_is_encrypted);
	g_test_add_func("/credential/decrypt", test_decrypt);
	g_test_add_func("/credential/legacy_vector", test_legacy_vector);
	g_test_add_func("/credential/tamper", test_tamper);
	g_test_add_func("/credential/set_get", test_set_get);
	g_test_add_func("/credential/config_hooks", test_config_hooks);

	return g_test_run();
}
