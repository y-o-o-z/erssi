/*
 * The external credentials file (credential_external_file, by default
 * ~/.erssi/.credentials) holds passwords at a path the user's own setting
 * names in the user's own ~/.erssi. A symbolic link there is followed (a
 * dotfile manager's, or the secrets on an encrypted volume), but only to a
 * regular file of the user's own: a FIFO does not hang the start, a
 * directory or a device is no file. Whatever fails is said with the
 * reason, and no password is ever lost: a value is removed from the
 * configuration only once the external file holds it, and a migration
 * that fails leaves the mode as it was. Every test works with real files
 * in the temporary home; an open() that waits on a FIFO would hang the
 * test, so alarm() ends it and the test fails.
 */
#define MODULE_NAME "test-credential-file"

#include <irssi/src/common.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/chatnets.h>
#include <irssi/src/core/credential.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/lib-config/iconfig.h>

#include "../../test-home.h"

#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define HANG_SECONDS 5
#define FILE_NAME "creds-test"
#define CONTENT "chatnets = { Net = { sasl_password = \"from-the-file\"; }; };\n"

static char *last_error;

static void sig_gui_dialog(const char *type, const char *text)
{
	if (g_strcmp0(type, "error") == 0) {
		g_free(last_error);
		last_error = g_strdup(text);
	}
}

static char *path_of(const char *name)
{
	return g_build_filename(get_irssi_dir(), name, NULL);
}

static void remove_path(const char *path)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode))
		g_rmdir(path);
	else
		g_unlink(path);
}

static void setup(void)
{
	char *path = path_of(FILE_NAME);

	remove_path(path);
	g_free(path);
	/* the setting too: "setup changed" reads it again */
	settings_set_str("credential_external_file", FILE_NAME);
	g_free(credential_external_file);
	credential_external_file = g_strdup(FILE_NAME);
	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	credential_remove("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	g_free(last_error);
	last_error = NULL;
	alarm(HANG_SECONDS);
}

static void teardown(void)
{
	char *path = path_of(FILE_NAME);

	alarm(0);
	remove_path(path);
	g_free(path);
	credential_storage_mode = CREDENTIAL_STORAGE_CONFIG;
	settings_set_str("credential_storage_mode", "config");
	credential_remove("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
}

/* a file outside ~/.erssi, with a password in it */
static char *write_elsewhere(const char *content)
{
	char *path = g_build_filename(g_get_home_dir(), "elsewhere-credentials", NULL);

	g_assert_true(g_file_set_contents(path, content, -1, NULL));
	return path;
}

static char *read_back(const char *path)
{
	char *data = NULL;

	g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
	return data;
}

static void assert_loaded(const char *want)
{
	char *value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);

	g_assert_cmpstr(value, ==, want);
	g_free(value);
}

/* the reason of the last error printed */
static void assert_error_said(const char *what)
{
	g_assert_nonnull(last_error);
	if (strstr(last_error, what) == NULL)
		g_error("expected \"%s\" in: %s", what, last_error);
}

static void test_regular_read(void)
{
	char *path = path_of(FILE_NAME);

	setup();
	g_assert_true(g_file_set_contents(path, CONTENT, -1, NULL));
	g_assert_true(credential_external_load());
	assert_loaded("from-the-file");
	teardown();
	g_free(path);
}

/* a link to the user's own file elsewhere: read through it, and written
 * through it - the link stays */
static void test_symlink_followed(void)
{
	char *path = path_of(FILE_NAME), *target, *content, *error = NULL;
	CONFIG_REC *config;
	struct stat st;

	setup();
	target = write_elsewhere(CONTENT);
	g_assert_cmpint(symlink(target, path), ==, 0);
	g_assert_true(credential_external_load());
	assert_loaded("from-the-file");

	config = credential_file_open(path, FALSE, &error);
	g_assert_nonnull(config);
	config_node_set_str(config, config->mainnode, "x", "written");
	g_assert_cmpint(credential_file_write(config, NULL, &error), ==, 0);
	config_close(config);
	g_assert_cmpint(lstat(path, &st), ==, 0);
	g_assert_true(S_ISLNK(st.st_mode));
	content = read_back(target);
	g_assert_nonnull(strstr(content, "written"));
	g_free(content);
	g_assert_cmpint(stat(target, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
	teardown();
	g_unlink(target);
	g_free(target);
	g_free(path);
}

/* a FIFO, also behind a link: the load returns at once instead of waiting
 * for a writer, and says why */
static void test_fifo_does_not_hang(void)
{
	char *path = path_of(FILE_NAME), *fifo, *error = NULL;

	setup();
	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	g_assert_false(credential_external_load());
	assert_error_said("not a regular file");
	assert_error_said("no credentials were loaded from it");
	g_assert_null(credential_file_open(path, TRUE, &error));
	g_assert_cmpstr(error, ==, "not a regular file");
	g_free(error);
	error = NULL;

	fifo = g_build_filename(g_get_home_dir(), "credentials-fifo", NULL);
	g_assert_cmpint(rename(path, fifo), ==, 0);
	g_assert_cmpint(symlink(fifo, path), ==, 0);
	g_assert_null(credential_file_open(path, TRUE, &error));
	g_assert_cmpstr(error, ==, "not a regular file");
	g_free(error);
	teardown();
	g_unlink(fifo);
	g_free(fifo);
	g_free(path);
}

static void test_directory_refused(void)
{
	char *path = path_of(FILE_NAME), *error = NULL;

	setup();
	g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
	g_assert_false(credential_external_load());
	assert_error_said("a directory");
	g_assert_null(credential_file_open(path, TRUE, &error));
	g_free(error);
	teardown();
	g_free(path);
}

static void test_dev_null_link_refused(void)
{
	char *path = path_of(FILE_NAME), *error = NULL;

	setup();
	g_assert_cmpint(symlink("/dev/null", path), ==, 0);
	g_assert_false(credential_external_load());
	assert_error_said("not a regular file");
	g_assert_null(credential_file_open(path, TRUE, &error));
	g_free(error);
	teardown();
	g_free(path);
}

/* missing: an empty file when that is asked for, not created on disk */
static void test_missing(void)
{
	char *path = path_of(FILE_NAME), *error = NULL;
	CONFIG_REC *config;

	setup();
	g_assert_null(credential_file_open(path, FALSE, &error));
	g_assert_nonnull(error);
	g_free(error);
	config = credential_file_open(path, TRUE, NULL);
	g_assert_nonnull(config);
	config_close(config);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	teardown();
	g_free(path);
}

/* written with mode 0600 whatever the umask; a link that leads nowhere or
 * a FIFO is not written to, with the reason */
static void test_write(void)
{
	char *path = path_of(FILE_NAME), *error = NULL;
	struct stat st;
	CONFIG_REC *config;
	mode_t old_umask;

	setup();
	config = credential_file_open(path, TRUE, NULL);
	g_assert_nonnull(config);
	config_node_set_str(config, config->mainnode, "x", "y");
	old_umask = umask(022);
	g_assert_cmpint(credential_file_write(config, NULL, NULL), ==, 0);
	umask(old_umask);
	g_assert_cmpint(lstat(path, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
	g_unlink(path);

	g_assert_cmpint(symlink("nowhere/at/all", path), ==, 0);
	g_assert_cmpint(credential_file_write(config, NULL, &error), ==, -1);
	g_assert_cmpstr(error, ==, "a symbolic link that leads to no file");
	g_free(error);
	error = NULL;
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
	g_unlink(path);

	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	g_assert_cmpint(credential_file_write(config, NULL, &error), ==, -1);
	g_assert_cmpstr(error, ==, "not a regular file");
	g_free(error);

	config_close(config);
	teardown();
	g_free(path);
}

static CONFIG_NODE *net_node(void)
{
	CONFIG_NODE *node = config_node_traverse(mainconfig, "chatnets", TRUE);

	return config_node_section(mainconfig, node, "Net", NODE_TYPE_BLOCK);
}

/* /NETWORK ADD -sasl_password in external mode: the password moves to
 * the external file - and stays in the configuration when that file
 * cannot be written, with the reason said */
static void test_save_keeps_password(void)
{
	char *path = path_of(FILE_NAME), *content;
	CHATNET_REC *net = g_new0(CHATNET_REC, 1);
	CONFIG_NODE *node;

	setup();
	net->name = g_strdup("Net");
	credential_storage_mode = CREDENTIAL_STORAGE_EXTERNAL;
	g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
	node = net_node();
	config_node_set_str(mainconfig, node, "sasl_password", "new-secret");
	signal_emit("chatnet saved", 2, net, node);
	g_assert_cmpstr(config_node_get_str(node, "sasl_password", NULL), ==, "new-secret");
	assert_error_said("cannot write");
	assert_error_said("a directory");

	/* the file can be written: moved there */
	g_rmdir(path);
	signal_emit("chatnet saved", 2, net, node);
	g_assert_null(config_node_get_str(node, "sasl_password", NULL));
	content = read_back(path);
	g_assert_nonnull(strstr(content, "new-secret"));
	g_free(content);

	config_node_set_str(mainconfig, config_node_traverse(mainconfig, "chatnets", TRUE),
	                    "Net", NULL);
	g_free(net->name);
	g_free(net);
	teardown();
	g_free(path);
}

/* /CREDENTIAL MIGRATE CONFIG (credential_storage_mode config) with an
 * external file that cannot be read: the reason is said, the mode stays
 * external, nothing is lost; once it can be read, the credentials move */
static void test_migrate_failure_keeps_mode(void)
{
	char *path = path_of(FILE_NAME);

	setup();
	settings_set_str("credential_storage_mode", "external");
	credential_storage_mode = CREDENTIAL_STORAGE_EXTERNAL;
	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	settings_set_str("credential_storage_mode", "config");
	signal_emit("setup changed", 0);
	g_assert_cmpint(credential_storage_mode, ==, CREDENTIAL_STORAGE_EXTERNAL);
	g_assert_cmpstr(settings_get_str("credential_storage_mode"), ==, "external");
	assert_error_said("not a regular file");
	assert_error_said("credential_storage_mode stays external");

	g_unlink(path);
	g_assert_true(g_file_set_contents(path, CONTENT, -1, NULL));
	settings_set_str("credential_storage_mode", "config");
	signal_emit("setup changed", 0);
	g_assert_cmpint(credential_storage_mode, ==, CREDENTIAL_STORAGE_CONFIG);
	g_assert_cmpstr(config_node_get_str(net_node(), "sasl_password", NULL), ==,
	                "from-the-file");
	config_node_set_str(mainconfig, config_node_traverse(mainconfig, "chatnets", TRUE),
	                    "Net", NULL);
	teardown();
	g_free(path);
}

/* The start in external mode, in a process of its own: the configuration
 * and the external file are there before core_init(). */
static void start_with(const char *external_link_target, gboolean fifo)
{
	char *path;

	test_home_setup();
	path = g_build_filename(test_home_dir, "config", NULL);
	g_assert_true(g_file_set_contents(path, "settings = { core = { "
	                                        "credential_storage_mode = \"external\"; }; };\n",
	                                  -1, NULL));
	g_free(path);
	path = g_build_filename(test_home_dir, ".credentials", NULL);
	if (fifo)
		g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	else
		g_assert_cmpint(symlink(external_link_target, path), ==, 0);
	g_free(path);
	alarm(HANG_SECONDS);
	core_preinit("test");
	irssi_gui = IRSSI_GUI_NONE;
	core_init();
	alarm(0);
}

/* a start in external mode reads the file, through a link, and leaves it
 * as it is (it was replaced by the empty credentials of the
 * configuration at every start) */
static void test_start_external_link(void)
{
	if (g_test_subprocess()) {
		char *target = g_build_filename(g_get_tmp_dir(), "erssi-test-creds-XXXXXX", NULL);
		int fd = g_mkstemp(target);
		char *content;

		g_assert_cmpint(fd, >=, 0);
		close(fd);
		g_assert_true(g_file_set_contents(target, CONTENT, -1, NULL));
		start_with(target, FALSE);
		assert_loaded("from-the-file");
		content = read_back(target);
		g_assert_cmpstr(content, ==, CONTENT);
		g_free(content);
		g_unlink(target);
		g_free(target);
		return;
	}
	g_test_trap_subprocess(NULL, 20 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
}

/* a FIFO at the start: no hang, and the startup warning says why nothing
 * was loaded */
static void test_start_external_fifo(void)
{
	if (g_test_subprocess()) {
		char **lines;

		start_with(NULL, TRUE);
		lines = credential_legacy_warning();
		g_assert_nonnull(lines);
		g_assert_nonnull(strstr(lines[0], "cannot read"));
		g_assert_nonnull(strstr(lines[0], "not a regular file"));
		g_strfreev(lines);
		return;
	}
	g_test_trap_subprocess(NULL, 20 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_passed();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	/* the start tests make their own home and start in a child */
	if (!g_test_subprocess()) {
		test_home_setup();
		core_preinit(*argv);
		irssi_gui = IRSSI_GUI_NONE;
		core_init();
		signal_add_first("gui dialog", (SIGNAL_FUNC) sig_gui_dialog);
	}

	g_test_add_func("/credential/file/regular_read", test_regular_read);
	g_test_add_func("/credential/file/symlink_followed", test_symlink_followed);
	g_test_add_func("/credential/file/fifo_does_not_hang", test_fifo_does_not_hang);
	g_test_add_func("/credential/file/directory_refused", test_directory_refused);
	g_test_add_func("/credential/file/dev_null_link_refused", test_dev_null_link_refused);
	g_test_add_func("/credential/file/missing", test_missing);
	g_test_add_func("/credential/file/write", test_write);
	g_test_add_func("/credential/file/save_keeps_password", test_save_keeps_password);
	g_test_add_func("/credential/file/migrate_failure_keeps_mode",
	                test_migrate_failure_keeps_mode);
	g_test_add_func("/credential/file/start_external_link", test_start_external_link);
	g_test_add_func("/credential/file/start_external_fifo", test_start_external_fifo);

	return g_test_run();
}
