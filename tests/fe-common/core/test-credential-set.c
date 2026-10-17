/*
 * credential_set() when encrypting the new value fails: the record keeps
 * its old value (or, for a new credential, none is added), and nothing is
 * freed twice. core/credential.c is compiled into this test with
 * credential_encrypt() replaced by a wrapper that can be told to fail,
 * which the real function only does when OpenSSL does.
 */
#include <irssi/src/common.h>

char *test_credential_encrypt(const char *plaintext, const char *password);
#define credential_encrypt test_credential_encrypt
#include <irssi/src/core/credential.c>
#undef credential_encrypt

/* the real one, from credential-crypto.c; its prototype was renamed above */
char *credential_encrypt(const char *plaintext, const char *password);

#include <string.h>

#define PREFIX "erssi-enc1:"

static gboolean encrypt_fails;

char *test_credential_encrypt(const char *plaintext, const char *password)
{
	if (encrypt_fails)
		return NULL;
	return credential_encrypt(plaintext, password);
}

static void encryption_on(void)
{
	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_config_encrypt = TRUE;
	g_free(master_password);
	master_password = g_strdup("master secret");
	encrypt_fails = FALSE;
}

static void encryption_off(void)
{
	credential_config_encrypt = FALSE;
	g_free(master_password);
	master_password = NULL;
	encrypt_fails = FALSE;
}

/* an existing credential keeps its old value when the new one cannot be
 * encrypted, and can still be read and removed */
static void test_replace_fails(void)
{
	CREDENTIAL_REC *rec;
	char *old_stored, *value;

	encryption_on();
	g_assert_true(credential_set("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD, "old-password"));
	rec = credential_find("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	g_assert_nonnull(rec);
	g_assert_true(g_str_has_prefix(rec->encrypted_value, PREFIX));
	old_stored = g_strdup(rec->encrypted_value);

	encrypt_fails = TRUE;
	g_assert_false(credential_set("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD, "new-password"));
	encrypt_fails = FALSE;

	g_assert_true(credential_find("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD) == rec);
	g_assert_cmpstr(rec->encrypted_value, ==, old_stored);
	value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	g_assert_cmpstr(value, ==, "old-password");
	g_free(value);

	/* a later attempt that works replaces it */
	g_assert_true(credential_set("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD, "new-password"));
	value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	g_assert_cmpstr(value, ==, "new-password");
	g_free(value);

	g_assert_true(credential_remove("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD));
	g_assert_null(credential_find("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD));
	g_free(old_stored);
	encryption_off();
}

/* a new credential that cannot be encrypted is not added */
static void test_new_fails(void)
{
	guint count;

	encryption_on();
	count = g_slist_length(credentials);
	encrypt_fails = TRUE;
	g_assert_false(credential_set("Other", CREDENTIAL_CONTEXT_SERVER_PASSWORD, "secret"));
	encrypt_fails = FALSE;
	g_assert_null(credential_find("Other", CREDENTIAL_CONTEXT_SERVER_PASSWORD));
	g_assert_cmpuint(g_slist_length(credentials), ==, count);
	encryption_off();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/credential/set/replace_fails", test_replace_fails);
	g_test_add_func("/credential/set/new_fails", test_new_fails);

	return g_test_run();
}
