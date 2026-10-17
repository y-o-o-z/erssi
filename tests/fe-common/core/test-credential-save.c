/*
 * /SAVE (settings_save(), the function behind it and the autosave) writes
 * a credential that an erssi version before 2026.10.3 encrypted again in
 * the current, authenticated form, when the credentials are kept in the
 * configuration file and the master password is set and confirmed: a
 * value already in the current form must decrypt with it. A wrong master
 * password can decrypt an old value to random text (about once in 256
 * tries), and that must never replace the real value.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/credential.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/lib-config/iconfig.h>

#include "../../test-home.h"

#include <string.h>

#define PREFIX "erssi-enc1:"
#define MASTER "master secret"

/* written by erssi 2026.10.2: "old:secret" under MASTER */
#define LEGACY_VECTOR \
	"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f:" \
	"6465666768696a6b6c6d6e6f70717273:WJyhOjhJzUeXuRAamdBfOQ=="
/* master passwords that pass the CBC padding check of LEGACY_VECTOR
 * although they are wrong (found by trying "wrong 0", "wrong 1", ...):
 * the first gives text with a NUL in it, the second random bytes */
#define WRONG_NUL "wrong 344"
#define WRONG_RANDOM "wrong 586"

static CONFIG_NODE *chatnet_node(CONFIG_REC *config, const char *name)
{
	CONFIG_NODE *node = config_node_traverse(config, "chatnets", TRUE);

	return config_node_section(config, node, name, NODE_TYPE_BLOCK);
}

/* the configuration as it is: Net with the old value, and, when
 * confirm_with is not NULL, Other with a value in the current form under
 * that password */
static void setup_config(const char *confirm_with)
{
	CONFIG_NODE *node;

	config_node_traverse(mainconfig, "chatnets", TRUE);
	node = config_node_traverse(mainconfig, "chatnets", FALSE);
	config_node_clear(mainconfig, node);
	config_node_set_str(mainconfig, chatnet_node(mainconfig, "Net"), "sasl_password",
	                    LEGACY_VECTOR);
	if (confirm_with != NULL) {
		char *current = credential_encrypt("other secret", confirm_with);

		config_node_set_str(mainconfig, chatnet_node(mainconfig, "Other"),
		                    "sasl_password", current);
		g_free(current);
	}
}

/* /SAVE, then the Net value as the file holds it */
static char *save_and_read_back(void)
{
	CONFIG_REC *written;
	CONFIG_NODE *node;
	char *value;

	g_assert_true(settings_save(NULL, FALSE));
	written = config_open(mainconfig->fname, -1);
	g_assert_nonnull(written);
	g_assert_cmpint(config_parse(written), ==, 0);
	node = config_node_traverse(written, "chatnets/Net", FALSE);
	g_assert_nonnull(node);
	value = g_strdup(config_node_get_str(node, "sasl_password", NULL));
	config_close(written);

	/* what is in memory is what was written */
	g_assert_cmpstr(config_node_get_str(chatnet_node(mainconfig, "Net"), "sasl_password",
	                                    NULL), ==, value);
	return value;
}

static void test_upgrade_confirmed(void)
{
	char *value, *plain;

	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_set_master_password(MASTER);
	setup_config(MASTER);

	value = save_and_read_back();
	g_assert_true(g_str_has_prefix(value, PREFIX));
	plain = credential_decrypt(value, MASTER);
	g_assert_cmpstr(plain, ==, "old:secret");
	g_free(plain);
	g_free(value);

	/* saving again changes nothing more */
	value = save_and_read_back();
	g_assert_true(g_str_has_prefix(value, PREFIX));
	g_free(value);
	credential_clear_master_password();
}

/* with only old values nothing shows the password is right */
static void test_not_confirmed(void)
{
	char *value;

	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_set_master_password(MASTER);
	setup_config(NULL);
	value = save_and_read_back();
	g_assert_cmpstr(value, ==, LEGACY_VECTOR);
	g_free(value);

	/* nor does a value in the current form under another password */
	setup_config("another master password");
	value = save_and_read_back();
	g_assert_cmpstr(value, ==, LEGACY_VECTOR);
	g_free(value);

	/* and without a master password nothing is done */
	credential_clear_master_password();
	setup_config(MASTER);
	value = save_and_read_back();
	g_assert_cmpstr(value, ==, LEGACY_VECTOR);
	g_free(value);
}

/* the credentials are kept in the external file: the configuration is
 * written as it is */
static void test_external_mode(void)
{
	char *value;

	credential_storage_mode = CREDENTIAL_STORAGE_EXTERNAL;
	credential_set_master_password(MASTER);
	setup_config(MASTER);
	value = save_and_read_back();
	g_assert_cmpstr(value, ==, LEGACY_VECTOR);
	g_free(value);
	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_clear_master_password();
}

/* a wrong master password, even one that values in the current form were
 * encrypted with (the password was changed), never replaces the old value
 * with what it decrypts to */
static void test_wrong_password(void)
{
	char *value, *plain;

	/* text with a NUL in it is no password: the decryption fails */
	g_assert_null(credential_decrypt(LEGACY_VECTOR, WRONG_NUL));
	/* random bytes are not UTF-8 text without control characters */
	plain = credential_decrypt(LEGACY_VECTOR, WRONG_RANDOM);
	g_assert_nonnull(plain);
	g_assert_cmpstr(plain, !=, "old:secret");
	g_free(plain);

	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_set_master_password(WRONG_RANDOM);
	setup_config(WRONG_RANDOM);
	value = save_and_read_back();
	g_assert_cmpstr(value, ==, LEGACY_VECTOR);
	g_free(value);

	credential_set_master_password(WRONG_NUL);
	setup_config(WRONG_NUL);
	value = save_and_read_back();
	g_assert_cmpstr(value, ==, LEGACY_VECTOR);
	g_free(value);

	/* credential_set() keeps it as well */
	credential_set_master_password(WRONG_RANDOM);
	g_assert_true(credential_set("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD, LEGACY_VECTOR));
	g_assert_true(credential_set("Other", CREDENTIAL_CONTEXT_SASL_PASSWORD, "plain"));
	{
		GSList *list = credential_list(), *tmp;

		for (tmp = list; tmp != NULL; tmp = tmp->next) {
			CREDENTIAL_REC *rec = tmp->data;

			if (strcmp(rec->network, "Net") == 0)
				g_assert_cmpstr(rec->encrypted_value, ==, LEGACY_VECTOR);
		}
		g_slist_free(list);
	}
	credential_remove("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	credential_remove("Other", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	credential_clear_master_password();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* reading an old value logs a warning once; it is no failure here */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	test_home_setup();
	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	core_init();

	g_test_add_func("/credential/save/upgrade_confirmed", test_upgrade_confirmed);
	g_test_add_func("/credential/save/not_confirmed", test_not_confirmed);
	g_test_add_func("/credential/save/external_mode", test_external_mode);
	g_test_add_func("/credential/save/wrong_password", test_wrong_password);

	return g_test_run();
}
