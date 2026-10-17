/*
 * Tests for the encrypted form of core/credential.c and credential-crypto.c:
 * an encrypted value carries the "erssi-enc1:" prefix and anything without
 * it is plain text, so a password with ':' in it is neither left
 * unencrypted nor taken for an encrypted value. Values written by older
 * versions (salt_hex:iv_hex:base64 without the prefix) are still read.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/credential.h>
#include <irssi/src/lib-config/iconfig.h>

#include <string.h>

#define PREFIX "erssi-enc1:"
#define MASTER "master secret"

/* what versions before 2026.10.3 wrote: an encrypted value without the
 * prefix */
static char *legacy_encrypt(const char *plain)
{
	char *value, *legacy;

	value = credential_encrypt(plain, MASTER);
	g_assert_nonnull(value);
	g_assert_true(g_str_has_prefix(value, PREFIX));
	legacy = g_strdup(value + strlen(PREFIX));
	g_free(value);
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
		char *plain = credential_decrypt(value, "another one");

		g_assert_cmpstr(plain, !=, "pa:ss");
		g_free(plain);
		g_free(value);
	}
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

	/* a value encrypted by an older version is read */
	{
		char *legacy = legacy_encrypt("old:secret");
		const char *stored;

		g_assert_true(credential_set("Old", CREDENTIAL_CONTEXT_SERVER_PASSWORD, legacy));
		stored = stored_value("Old", CREDENTIAL_CONTEXT_SERVER_PASSWORD);
		g_assert_cmpstr(stored, ==, legacy); /* not encrypted again */
		value = credential_get("Old", CREDENTIAL_CONTEXT_SERVER_PASSWORD);
		g_assert_cmpstr(value, ==, "old:secret");
		g_free(value);
		g_free(legacy);
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

	credential_config_write_hook(config);
	node = config_node_traverse(config, "chatnets/Net", FALSE);
	g_assert_nonnull(node);
	value = config_node_get_str(node, "sasl_password", NULL);
	g_assert_true(g_str_has_prefix(value, PREFIX));
	g_assert_true(g_str_has_prefix(config_node_get_str(node, "sasl_username", NULL), PREFIX));
	node = config_node_traverse(config, "servers", FALSE);
	node = config_node_nth(node, 0);
	g_assert_true(g_str_has_prefix(config_node_get_str(node, "password", NULL), PREFIX));

	credential_config_read_hook(config);
	node = config_node_traverse(config, "chatnets/Net", FALSE);
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
	g_test_add_func("/credential/set_get", test_set_get);
	g_test_add_func("/credential/config_hooks", test_config_hooks);

	return g_test_run();
}
