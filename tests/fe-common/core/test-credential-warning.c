/*
 * The warning printed once at startup when a configuration still uses the
 * old credential encryption, the external credentials file, or holds
 * encrypted values (credential_legacy_warning()): it says that the
 * configuration file itself is plain text, which values may be encrypted,
 * and names the commands that go back to plain storage. A configuration
 * that uses none of it gets no warning.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/credential.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/lib-config/iconfig.h>

#include "../../test-home.h"

#include <string.h>
#include <sys/stat.h>

/* the lines as one text, NULL for no warning */
static char *warning_text(void)
{
	char **lines = credential_legacy_warning();
	char *text;

	if (lines == NULL)
		return NULL;
	text = g_strjoinv("\n", lines);
	g_strfreev(lines);
	return text;
}

static void set_sasl_password(const char *value)
{
	CONFIG_NODE *node = config_node_traverse(mainconfig, "chatnets", TRUE);

	node = config_node_section(mainconfig, node, "Net", NODE_TYPE_BLOCK);
	config_node_set_str(mainconfig, node, "sasl_password", value);
}

static void reset(void)
{
	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_config_encrypt = FALSE;
	set_sasl_password("plain password");
}

static void assert_protection_said(const char *text)
{
	g_assert_nonnull(strstr(text, "is a plain-text file, readable"));
	g_assert_nonnull(strstr(text, mainconfig->fname));
	g_assert_nonnull(strstr(text, "To keep passwords out of the file"));
}

static void test_plain_no_warning(void)
{
	reset();
	g_assert_null(credential_legacy_warning());
}

static void test_encryption_on(void)
{
	char *text;

	reset();
	credential_config_encrypt = TRUE;
	text = warning_text();
	g_assert_nonnull(text);
	assert_protection_said(text);
	g_assert_nonnull(strstr(text, "Nothing in it is encrypted."));
	g_assert_nonnull(strstr(text, "To go back to plain storage: /CREDENTIAL PASSWD "
	                              "<master password>, /CREDENTIAL DECRYPT, /SAVE."));
	g_free(text);
	reset();
}

static void test_external_mode(void)
{
	char *text;

	reset();
	credential_storage_mode = CREDENTIAL_STORAGE_EXTERNAL;
	text = warning_text();
	g_assert_nonnull(text);
	assert_protection_said(text);
	g_assert_nonnull(strstr(text, "external file are plain text in it unless"));
	g_assert_nonnull(strstr(text, "To go back to plain storage: /CREDENTIAL MIGRATE CONFIG, "
	                              "/SAVE."));
	g_free(text);

	credential_config_encrypt = TRUE;
	text = warning_text();
	g_assert_nonnull(strstr(text, "/CREDENTIAL PASSWD <master password>, /CREDENTIAL DECRYPT, "
	                              "/CREDENTIAL MIGRATE CONFIG, /SAVE."));
	g_free(text);
	reset();
}

/* the feature off, but values left encrypted: nothing decrypts them any
 * more, the passwords are entered again */
static void test_encrypted_values_left(void)
{
	char *value, *text;

	reset();
	value = credential_encrypt("secret", "master");
	g_assert_nonnull(value);
	set_sasl_password(value);
	g_free(value);
	text = warning_text();
	g_assert_nonnull(text);
	assert_protection_said(text);
	g_assert_nonnull(strstr(text, "Only the passwords that the old encryption encrypted"));
	g_assert_nonnull(strstr(text, "enter each encrypted password again"));
	g_assert_nonnull(strstr(text, "/NETWORK ADD -sasl_password"));
	g_free(text);
	reset();
	g_assert_null(credential_legacy_warning());
}

/* the mode the file has now: a copy made by hand may be readable by others */
static void test_file_mode_said(void)
{
	char *text;

	reset();
	credential_config_encrypt = TRUE;
	g_assert_true(settings_save(NULL, FALSE));
	text = warning_text();
	g_assert_nonnull(strstr(text, "readable only by you (mode 0600)"));
	g_free(text);
	g_assert_cmpint(chmod(mainconfig->fname, 0644), ==, 0);
	text = warning_text();
	g_assert_nonnull(strstr(text, "readable by other users too (mode 0644; /SAVE writes it "
	                              "with mode 0600)"));
	g_free(text);
	g_assert_cmpint(chmod(mainconfig->fname, 0600), ==, 0);
	reset();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	test_home_setup();
	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	core_init();

	g_test_add_func("/credential/warning/plain_no_warning", test_plain_no_warning);
	g_test_add_func("/credential/warning/encryption_on", test_encryption_on);
	g_test_add_func("/credential/warning/external_mode", test_external_mode);
	g_test_add_func("/credential/warning/encrypted_values_left", test_encrypted_values_left);
	g_test_add_func("/credential/warning/file_mode_said", test_file_mode_said);

	return g_test_run();
}
