/*
 * debug_log_open(): the debug logs (sidepanel_debug, resize_debug) are
 * files of the user's own. A new one is made readable by the user only,
 * whatever the umask; a symbolic link or a FIFO in its place is refused
 * (the link's target is not touched, the open does not hang); an old log
 * made with the umask mode is appended to and made private.
 */
#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/resize-debug.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *dir;

static char *path_in_dir(const char *name)
{
	return g_build_filename(dir, name, NULL);
}

static char *read_file(const char *path)
{
	char *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static void test_new_file_private(void)
{
	char *path = path_in_dir("new.log");
	struct stat st;
	mode_t old;
	FILE *fp;
	char *text;

	old = umask(0);
	fp = debug_log_open(path);
	umask(old);
	g_assert_nonnull(fp);
	fputs("one\n", fp);
	fclose(fp);

	g_assert_cmpint(lstat(path, &st), ==, 0);
	g_assert_true(S_ISREG(st.st_mode));
	g_assert_cmpint(st.st_mode & 07777, ==, 0600);
	text = read_file(path);
	g_assert_cmpstr(text, ==, "one\n");
	g_free(text);

	/* opened again: appended to */
	fp = debug_log_open(path);
	g_assert_nonnull(fp);
	fputs("two\n", fp);
	fclose(fp);
	text = read_file(path);
	g_assert_cmpstr(text, ==, "one\ntwo\n");
	g_free(text);
	g_free(path);
}

static void test_symlink_refused(void)
{
	char *target = path_in_dir("target");
	char *link = path_in_dir("link.log");
	char *text;

	g_assert_true(g_file_set_contents(target, "secret\n", -1, NULL));
	g_assert_cmpint(symlink(target, link), ==, 0);

	g_assert_null(debug_log_open(link));
	text = read_file(target);
	g_assert_cmpstr(text, ==, "secret\n");
	g_free(text);

	/* a dangling link: no file is made where it points */
	g_assert_cmpint(unlink(target), ==, 0);
	g_assert_null(debug_log_open(link));
	g_assert_false(g_file_test(target, G_FILE_TEST_EXISTS));
	g_free(target);
	g_free(link);
}

static void test_fifo_refused(void)
{
	char *path = path_in_dir("fifo.log");

	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	/* no reader: a blocking open would wait for one forever */
	g_assert_null(debug_log_open(path));
	g_free(path);
}

static void test_old_log_made_private(void)
{
	char *path = path_in_dir("old.log");
	struct stat st;
	FILE *fp;
	char *text;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(fchmod(fd, 0644), ==, 0);
	g_assert_cmpint(write(fd, "old\n", 4), ==, 4);
	close(fd);

	fp = debug_log_open(path);
	g_assert_nonnull(fp);
	fputs("new\n", fp);
	fclose(fp);
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 07777, ==, 0600);
	text = read_file(path);
	g_assert_cmpstr(text, ==, "old\nnew\n");
	g_free(text);
	g_free(path);
}

static void test_missing_dir(void)
{
	char *path = g_build_filename(dir, "no-such-dir", "x.log", NULL);

	g_assert_null(debug_log_open(path));
	g_free(path);
}

int main(int argc, char **argv)
{
	int ret;

	g_test_init(&argc, &argv, NULL);
	dir = g_dir_make_tmp("test-debug-log.XXXXXX", NULL);
	g_assert_nonnull(dir);

	g_test_add_func("/debug_log/new_file_private", test_new_file_private);
	g_test_add_func("/debug_log/symlink_refused", test_symlink_refused);
	g_test_add_func("/debug_log/fifo_refused", test_fifo_refused);
	g_test_add_func("/debug_log/old_log_made_private", test_old_log_made_private);
	g_test_add_func("/debug_log/missing_dir", test_missing_dir);

	ret = g_test_run();
	{
		const char *names[] = { "new.log", "link.log", "fifo.log", "old.log", "target", NULL };
		int i;

		for (i = 0; names[i] != NULL; i++) {
			char *p = path_in_dir(names[i]);
			unlink(p);
			g_free(p);
		}
	}
	rmdir(dir);
	g_free(dir);
	return ret;
}
