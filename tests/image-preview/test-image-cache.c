/*
 * The image preview cache (image-preview/image-preview-cache.c, compiled
 * into this test) keeps to image_preview_cache_size: the oldest files are
 * removed until the cache fits, the image just downloaded and the files
 * written within the download time limit (downloads in progress, images
 * not shown yet) stay, and only the cache's own regular files directly in
 * its directory are touched;
 * other files, directories and symlinks (and what they point to) are left
 * alone.
 */
#include <irssi/src/common.h>
#include <irssi/src/image-preview/image-preview-cache.c>

#include <glib/gstdio.h>
#include <utime.h>

static char *home;
static char *outside;
static int size_limit;

const char *get_irssi_dir(void)
{
	return home;
}

int settings_get_size(const char *key)
{
	g_assert_cmpstr(key, ==, IMAGE_PREVIEW_CACHE_SIZE);
	return size_limit;
}

/* image_preview_timeout 60s: files written in the last 120 seconds stay */
int settings_get_time(const char *key)
{
	g_assert_cmpstr(key, ==, IMAGE_PREVIEW_TIMEOUT);
	return 60 * 1000;
}

void image_preview_debug_print(const char *fmt, ...)
{
}

void printtext(void *server, const char *target, int level, const char *text, ...)
{
}

/* 64 hex digits */
#define HASH(c) c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c \
	c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c

static char *cache_file(const char *name)
{
	return g_build_filename(cache_dir, name, NULL);
}

/* a file of size bytes in the cache, last modified age seconds ago */
static void make_file(const char *name, gsize size, int age)
{
	char *path = cache_file(name);
	char *data = g_malloc0(size);
	struct utimbuf times;

	g_assert_true(g_file_set_contents(path, data, size, NULL));
	times.actime = times.modtime = time(NULL) - age;
	g_assert_cmpint(utime(path, &times), ==, 0);
	g_free(data);
	g_free(path);
}

static gboolean exists(const char *name)
{
	char *path = cache_file(name);
	struct stat st;
	gboolean ret = lstat(path, &st) == 0;

	g_free(path);
	return ret;
}

static void setup(void)
{
	home = g_dir_make_tmp("image-cache-XXXXXX", NULL);
	g_assert_nonnull(home);
	outside = g_build_filename(home, "outside", NULL);
	g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
	size_limit = 100 * 1024 * 1024;
	g_free(cache_dir);
	cache_dir = NULL;
	g_assert_true(ensure_cache_dir());
}

static void remove_tree(const char *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	const char *name;

	if (dir != NULL) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			char *child = g_build_filename(path, name, NULL);

			if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
			    !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
				remove_tree(child);
			else
				g_unlink(child);
			g_free(child);
		}
		g_dir_close(dir);
	}
	g_rmdir(path);
}

static void teardown(void)
{
	remove_tree(home);
	g_free(home);
	g_free(outside);
	g_free(cache_dir);
	cache_dir = NULL;
}

/* the oldest files go first, until the cache is within the limit */
static void test_size_limit(void)
{
	setup();
	make_file(HASH("1") ".png", 1024, 3000);
	make_file(HASH("2") ".jpg", 1024, 2000);
	make_file(HASH("3") ".img", 1024, 1000);

	size_limit = 4096;
	image_cache_cleanup();
	g_assert_true(exists(HASH("1") ".png"));
	g_assert_cmpint(cache_stats.total_size, ==, 3072);
	g_assert_cmpint(cache_stats.entry_count, ==, 3);

	size_limit = 2048;
	image_cache_cleanup();
	g_assert_false(exists(HASH("1") ".png"));
	g_assert_true(exists(HASH("2") ".jpg"));
	g_assert_true(exists(HASH("3") ".img"));
	g_assert_cmpint(cache_stats.total_size, ==, 2048);
	g_assert_cmpint(cache_stats.entry_count, ==, 2);

	size_limit = 1500;
	image_cache_cleanup();
	g_assert_false(exists(HASH("2") ".jpg"));
	g_assert_true(exists(HASH("3") ".img"));
	g_assert_cmpint(cache_stats.total_size, ==, 1024);

	/* a single file over the limit goes too */
	size_limit = 1000;
	image_cache_cleanup();
	g_assert_false(exists(HASH("3") ".img"));
	g_assert_cmpint(cache_stats.total_size, ==, 0);
	g_assert_cmpint(cache_stats.entry_count, ==, 0);
	teardown();
}

/* the image that was just downloaded stays, even alone over the limit, and
 * so do other new files: only older ones make room */
static void test_keep_new_file(void)
{
	char *path;

	setup();
	make_file(HASH("1") ".gif", 1024, 1000);
	make_file(HASH("2") ".gif", 1024, 900);
	/* another download still being written, an image not shown yet */
	make_file(HASH("3") ".gif", 1024, 100);
	make_file(HASH("4") ".gif", 1024, 5);
	/* just downloaded */
	make_file(HASH("a") ".gif", 2048, 0);
	size_limit = 1024;

	path = cache_file(HASH("a") ".gif");
	image_cache_enforce_limit(path);
	g_free(path);
	g_assert_true(exists(HASH("a") ".gif"));
	g_assert_false(exists(HASH("1") ".gif"));
	g_assert_false(exists(HASH("2") ".gif"));
	g_assert_true(exists(HASH("3") ".gif"));
	g_assert_true(exists(HASH("4") ".gif"));
	g_assert_cmpint(cache_stats.total_size, ==, 4096);
	g_assert_cmpint(cache_stats.entry_count, ==, 3);
	teardown();

	/* the kept file stays when it is the oldest one too */
	setup();
	make_file(HASH("a") ".gif", 2048, 1000);
	make_file(HASH("b") ".gif", 1024, 500);
	size_limit = 1024;
	path = cache_file(HASH("a") ".gif");
	image_cache_enforce_limit(path);
	g_free(path);
	g_assert_true(exists(HASH("a") ".gif"));
	g_assert_false(exists(HASH("b") ".gif"));

	/* a path outside the cache keeps nothing in it */
	path = g_build_filename(outside, HASH("a") ".gif", NULL);
	image_cache_enforce_limit(path);
	g_free(path);
	g_assert_false(exists(HASH("a") ".gif"));
	teardown();
}

/* the periodic cleanup leaves new files alone too, and removes them for
 * size once they are old enough */
static void test_recent_files(void)
{
	struct utimbuf times;
	char *path;

	setup();
	make_file(HASH("5") ".png", 2048, 110);
	make_file(HASH("6") ".png", 2048, 30);
	size_limit = 1024;
	image_cache_cleanup();
	g_assert_true(exists(HASH("5") ".png"));
	g_assert_true(exists(HASH("6") ".png"));
	g_assert_cmpint(cache_stats.total_size, ==, 4096);

	/* the first one, a few minutes later */
	path = cache_file(HASH("5") ".png");
	times.actime = times.modtime = time(NULL) - 130;
	g_assert_cmpint(utime(path, &times), ==, 0);
	g_free(path);
	image_cache_cleanup();
	g_assert_false(exists(HASH("5") ".png"));
	g_assert_true(exists(HASH("6") ".png"));
	teardown();
}

/* files older than 7 days are removed even under the limit */
static void test_age(void)
{
	setup();
	make_file(HASH("c") ".webp", 200, 8 * 24 * 3600);
	make_file(HASH("d") ".webp", 200, 6 * 24 * 3600);
	image_cache_cleanup();
	g_assert_false(exists(HASH("c") ".webp"));
	g_assert_true(exists(HASH("d") ".webp"));
	teardown();
}

/* nothing but the cache's own regular files is removed or counted */
static void test_foreign_entries(void)
{
	static const char *const foreign[] = {
		"README", "notes.txt", ".hidden", HASH("e"), HASH("E") ".png",
		HASH("e") ".toolong", "0" HASH("e") ".png",
	};
	char *target, *link_path, *subdir;
	int i;

	setup();
	for (i = 0; i < (int) G_N_ELEMENTS(foreign); i++)
		make_file(foreign[i], 4096, 30 * 24 * 3600);

	/* a symlink with a cache name to a big old file outside the cache */
	target = g_build_filename(outside, "precious.png", NULL);
	{
		char *data = g_malloc0(8192);

		g_assert_true(g_file_set_contents(target, data, 8192, NULL));
		g_free(data);
	}
	link_path = cache_file(HASH("f") ".png");
	g_assert_cmpint(symlink(target, link_path), ==, 0);

	/* a directory with a cache name */
	subdir = cache_file(HASH("9") ".img");
	g_assert_cmpint(g_mkdir(subdir, 0700), ==, 0);

	make_file(HASH("0") ".png", 1024, 1000);

	size_limit = 512;
	image_cache_cleanup();

	for (i = 0; i < (int) G_N_ELEMENTS(foreign); i++) {
		if (!exists(foreign[i]))
			g_test_message("removed: %s", foreign[i]);
		g_assert_true(exists(foreign[i]));
	}
	g_assert_true(exists(HASH("f") ".png"));
	g_assert_true(g_file_test(target, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(subdir, G_FILE_TEST_IS_DIR));
	g_assert_false(exists(HASH("0") ".png"));
	g_assert_cmpint(cache_stats.total_size, ==, 0);

	/* /IMAGE CLEAR leaves them alone too */
	make_file(HASH("1") ".png", 100, 0);
	image_cache_clear_all();
	g_assert_false(exists(HASH("1") ".png"));
	for (i = 0; i < (int) G_N_ELEMENTS(foreign); i++)
		g_assert_true(exists(foreign[i]));
	g_assert_true(exists(HASH("f") ".png"));
	g_assert_true(g_file_test(target, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(subdir, G_FILE_TEST_IS_DIR));

	g_free(target);
	g_free(link_path);
	g_free(subdir);
	teardown();
}

/* PNG magic and enough bytes to pass for an image */
static void write_png(const char *path)
{
	char data[200];

	memset(data, 0, sizeof(data));
	memcpy(data, "\x89PNG\r\n\x1a\n", 8);
	g_assert_true(g_file_set_contents(path, data, sizeof(data), NULL));
}

/* a cached image is the cache's own regular file: a link with the URL's
 * cache name is no cached image (it would show any file it points to);
 * neither the link nor what it points to is removed */
static void test_get_symlink(void)
{
	const char *url = "https://example.invalid/linked.png";
	char *path, *target;

	setup();
	path = generate_cache_path(url);
	target = g_build_filename(outside, "other.png", NULL);
	write_png(target);
	g_assert_cmpint(symlink(target, path), ==, 0);
	g_assert_null(image_cache_get(url));
	g_assert_false(image_cache_has(url));
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
	g_assert_true(g_file_test(target, G_FILE_TEST_IS_REGULAR));

	/* the same image as the cache's own file is found */
	g_unlink(path);
	write_png(path);
	g_assert_true(image_cache_has(url));
	g_free(path);
	path = image_cache_get(url);
	g_assert_nonnull(path);
	g_free(path);
	g_free(target);
	teardown();
}

/* a FIFO or a link to /dev/null under the cache name: not an image, and
 * looking at it does not hang (alarm() ends a hanging test) */
static void test_get_fifo_dev_null(void)
{
	const char *url = "https://example.invalid/fifo.png";
	char *path;

	setup();
	alarm(5);
	path = generate_cache_path(url);
	g_assert_cmpint(mkfifo(path, 0600), ==, 0);
	g_assert_null(image_cache_get(url));
	g_assert_false(validate_image_file(path));
	g_unlink(path);
	g_assert_cmpint(symlink("/dev/null", path), ==, 0);
	g_assert_null(image_cache_get(url));
	g_assert_false(validate_image_file(path));
	alarm(0);
	g_free(path);
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/image-cache/size_limit", test_size_limit);
	g_test_add_func("/image-cache/keep_new_file", test_keep_new_file);
	g_test_add_func("/image-cache/recent_files", test_recent_files);
	g_test_add_func("/image-cache/age", test_age);
	g_test_add_func("/image-cache/foreign_entries", test_foreign_entries);
	g_test_add_func("/image-cache/get_symlink", test_get_symlink);
	g_test_add_func("/image-cache/get_fifo_dev_null", test_get_fifo_dev_null);

	return g_test_run();
}
