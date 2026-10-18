/*
 test-config-write.c : config_write() fails, and keeps the old file, when
                       a write fails at the end

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

/* config_write() writes a temporary file through a buffered GIOChannel and
 * renames it over the configuration. The last part of the buffer reaches
 * the file only when the channel is flushed: that write, too, must be done
 * and checked before the file is synced and renamed. Here write() and
 * fsync() of the process come through the test (Linux), which makes the
 * disk "full" from a given byte on, or after the fsync. */

#include <irssi/src/common.h>
#include <irssi/src/lib-config/iconfig.h>

#include "../test-tmpdir.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#ifdef __linux__
static gboolean watching, synced, fail_after_sync;
static gint64 written, fail_from = -1;

/* only the writes of config_write() are watched (watching), to files */
ssize_t write(int fd, const void *buf, size_t count)
{
	if (watching && fd > 2) {
		if ((fail_after_sync && synced) ||
		    (fail_from >= 0 && written + (gint64) count > fail_from)) {
			errno = ENOSPC;
			return -1;
		}
		written += count;
	}
	return syscall(SYS_write, fd, buf, count);
}

int fsync(int fd)
{
	if (watching)
		synced = TRUE;
	return syscall(SYS_fsync, fd);
}

static void watch(gint64 from, gboolean after_sync)
{
	watching = TRUE;
	synced = FALSE;
	written = 0;
	fail_from = from;
	fail_after_sync = after_sync;
}

static void unwatch(void)
{
	watching = FALSE;
}

typedef struct {
	char *dir;
	char *path;
	CONFIG_REC *config;
	char *old;	/* the file before the failing write */
} Fixture;

/* a configuration of about 13 KB: many times the channel's buffer */
static void fixture_setup(Fixture *f, gconstpointer data)
{
	CONFIG_NODE *node;
	int i;

	f->dir = g_dir_make_tmp("config-write-XXXXXX", NULL);
	g_assert_nonnull(f->dir);
	f->path = g_build_filename(f->dir, "config", NULL);
	g_assert_true(g_file_set_contents(f->path, "old = \"config\";\n", -1, NULL));
	f->old = g_strdup("old = \"config\";\n");
	f->config = config_open(f->path, 0600);
	g_assert_nonnull(f->config);
	node = config_node_traverse(f->config, "settings", TRUE);
	for (i = 0; i < 300; i++) {
		char *key = g_strdup_printf("k%d", i);

		config_node_set_str(f->config, node, key, "some value that is long enough");
		g_free(key);
	}
}

static void fixture_teardown(Fixture *f, gconstpointer data)
{
	GDir *dir;
	const char *name;

	unwatch();
	config_close(f->config);
	dir = g_dir_open(f->dir, 0, NULL);
	while ((name = g_dir_read_name(dir)) != NULL) {
		char *p = g_build_filename(f->dir, name, NULL);

		g_unlink(p);
		g_free(p);
	}
	g_dir_close(dir);
	g_rmdir(f->dir);
	g_free(f->dir);
	g_free(f->path);
	g_free(f->old);
}

static int dir_entries(const char *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	int n = 0;

	while (g_dir_read_name(dir) != NULL)
		n++;
	g_dir_close(dir);
	return n;
}

static void assert_old_file_kept(Fixture *f)
{
	char *now = NULL;

	g_assert_true(g_file_get_contents(f->path, &now, NULL, NULL));
	g_assert_cmpstr(now, ==, f->old);
	g_free(now);
	/* no temporary file left */
	g_assert_cmpint(dir_entries(f->dir), ==, 1);
}

/* the size config_write() writes, measured with a write that succeeds */
static gint64 full_size(Fixture *f)
{
	char *other = g_build_filename(f->dir, "measure", NULL);
	struct stat st;

	g_assert_cmpint(config_write(f->config, other, 0600), ==, 0);
	g_assert_cmpint(stat(other, &st), ==, 0);
	g_unlink(other);
	g_free(other);
	return st.st_size;
}

static void test_success(Fixture *f, gconstpointer data)
{
	gint64 size = full_size(f);
	struct stat st;

	g_assert_cmpint(size, >, 8192);
	watch(-1, FALSE);
	g_assert_cmpint(config_write(f->config, NULL, 0600), ==, 0);
	unwatch();
	g_assert_null(config_last_error(f->config));
	g_assert_cmpint(stat(f->path, &st), ==, 0);
	g_assert_cmpint(st.st_size, ==, size);
	g_assert_cmpint(written, ==, size);
}

/* the disk is full for the last bytes, in the part the buffer holds until
 * the end: a failure, the old file stays */
static void test_full_at_the_end(Fixture *f, gconstpointer data)
{
	gint64 size = full_size(f);

	watch(size - 100, FALSE);
	g_assert_cmpint(config_write(f->config, NULL, 0600), ==, -1);
	unwatch();
	g_assert_nonnull(config_last_error(f->config));
	assert_old_file_kept(f);
}

/* nothing is written after the fsync: everything has reached the file
 * before it is synced */
static void test_nothing_after_fsync(Fixture *f, gconstpointer data)
{
	gint64 size = full_size(f);
	struct stat st;

	watch(-1, TRUE);
	g_assert_cmpint(config_write(f->config, NULL, 0600), ==, 0);
	unwatch();
	g_assert_true(synced);
	g_assert_cmpint(stat(f->path, &st), ==, 0);
	g_assert_cmpint(st.st_size, ==, size);
}

/* the rename fails (the target is a directory): a failure */
static void test_rename_fails(Fixture *f, gconstpointer data)
{
	char *sub = g_build_filename(f->dir, "sub", NULL);

	g_assert_cmpint(g_mkdir(sub, 0700), ==, 0);
	g_assert_cmpint(config_write(f->config, sub, 0600), ==, -1);
	g_assert_nonnull(config_last_error(f->config));
	g_assert_true(g_file_test(sub, G_FILE_TEST_IS_DIR));
	g_rmdir(sub);
	g_free(sub);
}

#define ADD(name, func) \
	g_test_add(name, Fixture, NULL, fixture_setup, func, fixture_teardown)
#endif

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* every temporary file in one directory, removed also after a failure */
	test_tmpdir_setup();
#ifdef __linux__
	ADD("/config-write/success", test_success);
	ADD("/config-write/full-at-the-end", test_full_at_the_end);
	ADD("/config-write/nothing-after-fsync", test_nothing_after_fsync);
	ADD("/config-write/rename-fails", test_rename_fails);
#endif
	return g_test_run();
}
