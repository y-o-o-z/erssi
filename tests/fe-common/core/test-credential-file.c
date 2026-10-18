/*
 * The external credentials file (credential_external_file, by default
 * ~/.erssi/.credentials) holds passwords at a path a setting names. It is
 * read only when it is a regular file: a symbolic link is not followed (the
 * passwords would come from the file it points to), a FIFO does not hang
 * the start, a directory or a device is no file. It is written as a whole
 * with mode 0600, never through a link. Every test works with real files
 * in the temporary home; an open() that waits on a FIFO would hang the
 * test, so alarm() ends it and the test fails.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/credential.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/lib-config/iconfig.h>

#include "../../test-home.h"

#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define HANG_SECONDS 5
#define FILE_NAME "creds-test"
#define CONTENT "chatnets = { Net = { sasl_password = \"from-the-file\"; }; };\n"

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
	g_free(credential_external_file);
	credential_external_file = g_strdup(FILE_NAME);
	credential_remove("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	alarm(HANG_SECONDS);
}

static void teardown(void)
{
	char *path = path_of(FILE_NAME);

	alarm(0);
	remove_path(path);
	g_free(path);
	credential_remove("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
}

/* the file with a password in it, outside ~/.erssi */
static char *write_elsewhere(void)
{
	char *path = g_build_filename(g_get_home_dir(), "elsewhere-credentials", NULL);

	g_assert_true(g_file_set_contents(path, CONTENT, -1, NULL));
	return path;
}

static void assert_not_loaded(void)
{
	char *value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);

	g_assert_null(value);
}

static void test_regular_read(void)
{
	char *path = path_of(FILE_NAME), *value;

	setup();
	g_assert_true(g_file_set_contents(path, CONTENT, -1, NULL));
	g_assert_true(credential_external_load());
	value = credential_get("Net", CREDENTIAL_CONTEXT_SASL_PASSWORD);
	g_assert_cmpstr(value, ==, "from-the-file");
	g_free(value);
	teardown();
	g_free(path);
}

/* a link to a file elsewhere (put there by someone who can write the
 * directory): not followed, nothing is loaded from it */
static void test_symlink_not_followed(void)
{
	char *path = path_of(FILE_NAME), *target;
	CONFIG_REC *config;

	setup();
	target = write_elsewhere();
	g_assert_cmpint(symlink(target, path), ==, 0);
	g_assert_false(credential_external_load());
	assert_not_loaded();
	config = credential_file_open(path, TRUE);
	g_assert_null(config);
	teardown();
	g_unlink(target);
	g_free(target);
	g_free(path);
}

/* a FIFO: the load returns at once instead of waiting for a writer */
static void test_fifo_does_not_hang(void)
{
	char *path = path_of(FILE_NAME);

	setup();
	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	g_assert_false(credential_external_load());
	g_assert_null(credential_file_open(path, TRUE));
	assert_not_loaded();
	teardown();
	g_free(path);
}

static void test_directory_refused(void)
{
	char *path = path_of(FILE_NAME);

	setup();
	g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
	g_assert_false(credential_external_load());
	g_assert_null(credential_file_open(path, TRUE));
	teardown();
	g_free(path);
}

static void test_dev_null_link_refused(void)
{
	char *path = path_of(FILE_NAME);

	setup();
	g_assert_cmpint(symlink("/dev/null", path), ==, 0);
	g_assert_false(credential_external_load());
	g_assert_null(credential_file_open(path, TRUE));
	teardown();
	g_free(path);
}

/* missing: an empty file when that is asked for, not created on disk */
static void test_missing(void)
{
	char *path = path_of(FILE_NAME);
	CONFIG_REC *config;

	setup();
	g_assert_null(credential_file_open(path, FALSE));
	config = credential_file_open(path, TRUE);
	g_assert_nonnull(config);
	config_close(config);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	teardown();
	g_free(path);
}

/* written with mode 0600 whatever the umask; a link at the path is not
 * written through, the file it points to stays as it is */
static void test_write(void)
{
	char *path = path_of(FILE_NAME), *target, *content;
	struct stat st;
	CONFIG_REC *config;
	mode_t old_umask;

	setup();
	config = credential_file_open(path, TRUE);
	g_assert_nonnull(config);
	config_node_set_str(config, config->mainnode, "x", "y");
	old_umask = umask(022);
	g_assert_cmpint(credential_file_write(config, NULL), ==, 0);
	umask(old_umask);
	g_assert_cmpint(lstat(path, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpuint(st.st_mode & 07777, ==, 0600);
	g_unlink(path);

	target = write_elsewhere();
	g_assert_cmpint(symlink(target, path), ==, 0);
	g_assert_cmpint(credential_file_write(config, NULL), ==, -1);
	g_assert_true(g_file_get_contents(target, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, CONTENT);
	g_free(content);
	g_unlink(path);

	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	g_assert_cmpint(credential_file_write(config, NULL), ==, -1);

	config_close(config);
	teardown();
	g_unlink(target);
	g_free(target);
	g_free(path);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	test_home_setup();
	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	core_init();

	g_test_add_func("/credential/file/regular_read", test_regular_read);
	g_test_add_func("/credential/file/symlink_not_followed", test_symlink_not_followed);
	g_test_add_func("/credential/file/fifo_does_not_hang", test_fifo_does_not_hang);
	g_test_add_func("/credential/file/directory_refused", test_directory_refused);
	g_test_add_func("/credential/file/dev_null_link_refused", test_dev_null_link_refused);
	g_test_add_func("/credential/file/missing", test_missing);
	g_test_add_func("/credential/file/write", test_write);

	return g_test_run();
}
