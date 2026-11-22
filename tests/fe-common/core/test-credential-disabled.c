/*
 * Credential encryption and the external credentials file are disabled in
 * this version: turning either on interactively is refused, the setting is
 * put back, and the user is told why (CREDENTIAL_DISABLED_MSG). A
 * configuration that already uses them at startup is left as it is, so it
 * keeps working. These tests lock that contract in both directions so the
 * subsystem cannot silently regress to re-enabled.
 *
 * core/credential.c is compiled into this test so the refusal path and its
 * file-static state (credential_settings_ready, the "setup changed"
 * handlers) can be driven directly; the rest resolves from libcore.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>

#include <irssi/src/core/credential.c>

#include "../../test-home.h"

#include <string.h>

/* the last "error"/"info" dialog the refusal emits, captured below */
static char *last_error;
static char *last_note;

static void sig_gui_dialog(const char *type, const char *text)
{
	char **last = g_strcmp0(type, "error") == 0 ? &last_error :
	              g_strcmp0(type, "info") == 0 ? &last_note : NULL;

	if (last != NULL) {
		g_free(*last);
		*last = g_strdup(text);
	}
}

static void reset(void)
{
	g_free(last_error); last_error = NULL;
	g_free(last_note);  last_note = NULL;

	/* a baseline that uses neither feature, as a fresh configuration has */
	credential_settings_ready = TRUE;
	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_config_encrypt = FALSE;
	settings_set_str("credential_storage_mode", "config");
	settings_set_bool("credential_config_encrypt", FALSE);
}

/* /SET credential_storage_mode external is refused: the mode stays "config",
 * the setting is put back, and an error dialog says why. */
static void test_external_mode_refused(void)
{
	reset();

	settings_set_str("credential_storage_mode", "external");
	signal_emit("setup changed", 0);

	g_assert_cmpint(credential_storage_mode, ==, CREDENTIAL_STORAGE_CONFIG);
	g_assert_cmpstr(settings_get_str("credential_storage_mode"), ==, "config");
	g_assert_cmpstr(last_error, ==, CREDENTIAL_DISABLED_MSG);
}

/* /SET credential_config_encrypt on is refused the same way. */
static void test_config_encrypt_refused(void)
{
	reset();

	settings_set_bool("credential_config_encrypt", TRUE);
	signal_emit("setup changed", 0);

	g_assert_false(credential_config_encrypt);
	g_assert_false(settings_get_bool("credential_config_encrypt"));
	g_assert_cmpstr(last_error, ==, CREDENTIAL_DISABLED_MSG);
}

/* The other direction must NOT be refused: a configuration that is read with
 * the feature already on (credential_settings_ready is still FALSE, as it is
 * while settings load at startup) keeps it on, with no error dialog. */
static void test_startup_setting_preserved(void)
{
	reset();

	credential_settings_ready = FALSE;
	settings_set_bool("credential_config_encrypt", TRUE);
	credential_config_encrypt_changed();

	g_assert_true(credential_config_encrypt);
	g_assert_true(settings_get_bool("credential_config_encrypt"));
	g_assert_null(last_error);

	reset();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	test_home_setup();
	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	core_init();

	signal_add_first("gui dialog", (SIGNAL_FUNC) sig_gui_dialog);

	g_test_add_func("/credential/disabled/external_mode_refused",
	                test_external_mode_refused);
	g_test_add_func("/credential/disabled/config_encrypt_refused",
	                test_config_encrypt_refused);
	g_test_add_func("/credential/disabled/startup_setting_preserved",
	                test_startup_setting_preserved);

	return g_test_run();
}
