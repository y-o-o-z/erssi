/*
 image-preview-cache.c : Image cache management for erssi-nc

    Copyright (C) 2024 erssi team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "module.h"
#include "image-preview.h"

#include <irssi/src/core/settings.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/safe-file.h>
#include <irssi/src/fe-common/core/printtext.h>
#include <irssi/src/core/levels.h>

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* Cache directory path */
static char *cache_dir = NULL;

/* Periodic cleanup timer */
static guint cleanup_timer_tag = 0;

/* Cache statistics */
static struct {
	gint64 total_size;
	int entry_count;
	int hits;
	int misses;
} cache_stats = {0};

/* Minimum file size for a valid image (100 bytes - magic + some data) */
#define MIN_IMAGE_SIZE 100

/* Validate that a file contains an actual image (not HTML or other garbage).
 * The cache's own file only: opened once, not through a symbolic link, and
 * checked on that descriptor (a FIFO put there does not hang erssi). */
static gboolean validate_image_file(const char *path)
{
	unsigned char magic[12];
	ssize_t nread;
	struct stat st;
	int fd;

	fd = safe_file_open_read(path, 0, &st);
	if (fd < 0) {
		image_preview_debug_print("CACHE: validate_image_file - can't open %s: %s", path,
		                          safe_file_strerror(errno));
		return FALSE;
	}

	if (st.st_size < MIN_IMAGE_SIZE) {
		image_preview_debug_print("CACHE: validate_image_file - file too small (%lld bytes): %s",
		                          (long long)st.st_size, path);
		close(fd);
		return FALSE;
	}

	do
		nread = read(fd, magic, sizeof(magic));
	while (nread < 0 && errno == EINTR);
	close(fd);

	if (nread < 4)
		return FALSE;

	/* Check for image magic bytes */
	/* JPEG: FF D8 FF */
	if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF)
		return TRUE;

	/* PNG: 89 50 4E 47 (0x89 P N G) */
	if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47)
		return TRUE;

	/* GIF: 47 49 46 (G I F) */
	if (magic[0] == 0x47 && magic[1] == 0x49 && magic[2] == 0x46)
		return TRUE;

	/* WEBP: RIFF....WEBP (needs 12 bytes) */
	if (nread >= 12 &&
	    magic[0] == 0x52 && magic[1] == 0x49 && magic[2] == 0x46 && magic[3] == 0x46 &&
	    magic[8] == 0x57 && magic[9] == 0x45 && magic[10] == 0x42 && magic[11] == 0x50)
		return TRUE;

	/* BMP: 42 4D (B M) */
	if (magic[0] == 0x42 && magic[1] == 0x4D)
		return TRUE;

	/* Not a recognized image format - probably HTML or corrupted */
	image_preview_debug_print("CACHE: file %s failed magic validation (first bytes: %02x %02x %02x %02x)",
	                          path, magic[0], magic[1], magic[2], magic[3]);
	return FALSE;
}

/* Ensure cache directory exists */
static gboolean ensure_cache_dir(void)
{
	struct stat st;

	if (cache_dir == NULL) {
		cache_dir = g_strdup_printf("%s/%s", get_irssi_dir(), IMAGE_CACHE_DIR);
	}

	if (stat(cache_dir, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			return TRUE;
		}
		g_warning("image-cache: %s exists but is not a directory", cache_dir);
		return FALSE;
	}

	/* Create directory */
	if (mkdir(cache_dir, 0700) != 0) {
		g_warning("image-cache: Failed to create cache directory %s: %s",
		          cache_dir, strerror(errno));
		return FALSE;
	}

	return TRUE;
}

/* Generate cache path for URL (SHA256 hash + extension) */
static char *generate_cache_path(const char *url)
{
	GChecksum *checksum;
	const char *hash;
	const char *ext;
	char *path;

	if (!ensure_cache_dir())
		return NULL;

	checksum = g_checksum_new(G_CHECKSUM_SHA256);
	g_checksum_update(checksum, (guchar *)url, strlen(url));
	hash = g_checksum_get_string(checksum);

	/* Extract extension from URL */
	ext = strrchr(url, '.');
	if (ext != NULL) {
		/* Check if it's a valid image extension and not too long */
		if (strlen(ext) <= 6 && strchr(ext, '/') == NULL &&
		    (g_ascii_strcasecmp(ext, ".jpg") == 0 ||
		     g_ascii_strcasecmp(ext, ".jpeg") == 0 ||
		     g_ascii_strcasecmp(ext, ".png") == 0 ||
		     g_ascii_strcasecmp(ext, ".gif") == 0 ||
		     g_ascii_strcasecmp(ext, ".webp") == 0)) {
			/* Keep extension */
		} else {
			ext = ".img";
		}
	} else {
		ext = ".img";
	}

	path = g_strdup_printf("%s/%s%s", cache_dir, hash, ext);
	g_checksum_free(checksum);

	return path;
}

/* Check if URL is cached (and valid) */
gboolean image_cache_has(const char *url)
{
	char *path;
	struct stat st;
	gboolean valid;

	if (url == NULL)
		return FALSE;

	path = generate_cache_path(url);
	if (path == NULL)
		return FALSE;

	valid = FALSE;
	/* lstat: a symbolic link is no entry of the cache (nor removed) */
	if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		/* Also validate the file content */
		if (validate_image_file(path)) {
			valid = TRUE;
			cache_stats.hits++;
		} else {
			/* Invalid file - remove it */
			image_preview_debug_print("CACHE: has() removing invalid file: %s", path);
			unlink(path);
			cache_stats.misses++;
		}
	} else {
		cache_stats.misses++;
	}

	g_free(path);
	return valid;
}

/* Get cached file path (or NULL if not cached) */
char *image_cache_get(const char *url)
{
	char *path;
	struct stat st;

	if (url == NULL)
		return NULL;

	path = generate_cache_path(url);
	if (path == NULL)
		return NULL;

	if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		/* Validate that the cached file is actually an image */
		if (!validate_image_file(path)) {
			/* Corrupted cache entry (HTML, empty, etc.) - remove it */
			image_preview_debug_print("CACHE: removing invalid cached file: %s", path);
			unlink(path);
			g_free(path);
			cache_stats.misses++;
			return NULL;
		}
		cache_stats.hits++;
		return path;
	}

	cache_stats.misses++;
	g_free(path);
	return NULL;
}

/* TRUE for the name of a file the cache writes: the SHA-256 of the URL in
 * lower-case hex and a short extension (see generate_cache_path() and
 * image_preview_queue_fetch()). Nothing else in the directory is deleted,
 * even when the directory is not the one erssi made. */
static gboolean is_cache_file_name(const char *name)
{
	int i;

	for (i = 0; i < 64; i++) {
		if (!g_ascii_isxdigit(name[i]) || g_ascii_isupper(name[i]))
			return FALSE;
	}
	return name[64] == '.' && name[65] != '\0' && strlen(name + 64) <= 6;
}

/* Opens the cache directory for reading entries relative to it */
static DIR *open_cache_dir(void)
{
	int fd;
	DIR *dir;

	if (cache_dir == NULL || !ensure_cache_dir())
		return NULL;

	fd = open(cache_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	dir = fdopendir(fd);
	if (dir == NULL)
		close(fd);
	return dir;
}

/* Clear all cached images */
void image_cache_clear_all(void)
{
	DIR *dir;
	struct dirent *entry;
	struct stat st;

	dir = open_cache_dir();
	if (dir == NULL)
		return;

	while ((entry = readdir(dir)) != NULL) {
		if (!is_cache_file_name(entry->d_name))
			continue;
		/* a regular file only: a symlink is not followed, nor removed */
		if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
		    S_ISREG(st.st_mode))
			unlinkat(dirfd(dir), entry->d_name, 0);
	}

	closedir(dir);

	cache_stats.total_size = 0;
	cache_stats.entry_count = 0;
}

typedef struct {
	char *name;
	time_t mtime;
	gint64 size;
} CACHE_FILE_REC;

static int cache_file_cmp_age(gconstpointer a, gconstpointer b)
{
	const CACHE_FILE_REC *fa = a, *fb = b;

	if (fa->mtime != fb->mtime)
		return fa->mtime < fb->mtime ? -1 : 1;
	return strcmp(fa->name, fb->name);
}

static void cache_file_free(CACHE_FILE_REC *rec)
{
	g_free(rec->name);
	g_free(rec);
}

/* Removes files older than 7 days, then the oldest files until the cache
 * is no larger than image_preview_cache_size. keep_name, when not NULL, is
 * a file that stays (the image that was just downloaded to be shown).
 *
 * For the size limit only files last written longer ago than a download
 * may take (image_preview_timeout) and one minute more are removed. Newer
 * ones are downloads still in progress, or images downloaded a moment ago
 * and not shown yet (a shown image's file is removed 30 seconds after it
 * was displayed): they stay, even when that leaves the cache over the
 * limit, which a later cleanup then brings down. So a new image larger
 * than the limit is kept and shown, and only older files make room.
 *
 * Only regular files with the cache's own names directly in the cache
 * directory are looked at; symlinks are neither followed nor removed. */
static void cache_cleanup(const char *keep_name)
{
	DIR *dir;
	struct dirent *entry;
	struct stat st;
	time_t now;
	int max_age_seconds;
	gint64 max_size_bytes, total;
	time_t recent;
	GSList *files = NULL;
	GSList *l;
	int count;

	dir = open_cache_dir();
	if (dir == NULL)
		return;

	now = time(NULL);
	max_age_seconds = 7 * 24 * 60 * 60;  /* 7 days default */
	max_size_bytes = settings_get_size(IMAGE_PREVIEW_CACHE_SIZE);
	recent = now - settings_get_time(IMAGE_PREVIEW_TIMEOUT) / 1000 - 60;

	/* Collect all files with their stats */
	total = 0;
	count = 0;
	while ((entry = readdir(dir)) != NULL) {
		CACHE_FILE_REC *rec;

		if (!is_cache_file_name(entry->d_name))
			continue;
		if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
		    !S_ISREG(st.st_mode))
			continue;

		/* Check age - remove if too old */
		if ((now - st.st_mtime) > max_age_seconds &&
		    g_strcmp0(entry->d_name, keep_name) != 0) {
			unlinkat(dirfd(dir), entry->d_name, 0);
			continue;
		}

		rec = g_new0(CACHE_FILE_REC, 1);
		rec->name = g_strdup(entry->d_name);
		rec->mtime = st.st_mtime;
		rec->size = st.st_size;
		files = g_slist_prepend(files, rec);
		total += rec->size;
		count++;
	}

	/* If over the size limit, remove the oldest files first */
	files = g_slist_sort(files, cache_file_cmp_age);
	for (l = files; l != NULL && total > max_size_bytes; l = l->next) {
		CACHE_FILE_REC *rec = l->data;

		/* sorted by age: everything from here on is newer */
		if (rec->mtime >= recent)
			break;
		if (g_strcmp0(rec->name, keep_name) == 0)
			continue;
		if (unlinkat(dirfd(dir), rec->name, 0) == 0 || errno == ENOENT) {
			image_preview_debug_print("CACHE: over the size limit, removed %s (%" G_GINT64_FORMAT " bytes)",
			                          rec->name, rec->size);
			total -= rec->size;
			count--;
		}
	}

	closedir(dir);

	cache_stats.total_size = total;
	cache_stats.entry_count = count;

	g_slist_free_full(files, (GDestroyNotify) cache_file_free);
}

/* Cleanup old cache entries */
void image_cache_cleanup(void)
{
	cache_cleanup(NULL);
}

/* image_preview_cache_size is a soft limit (see cache_cleanup()): this is
 * the hard one. No new download starts while the cache, counted again
 * now, holds more than IMAGE_CACHE_HARD_FACTOR times the setting - files
 * that are too new to be removed. With at most MAX_CONCURRENT_FETCHES
 * downloads at a time (image-preview-fetch.c), each at most
 * image_preview_max_file_size, the cache never holds more than that
 * factor times the setting plus those downloads. */
gboolean image_cache_full(void)
{
	cache_cleanup(NULL);
	return cache_stats.total_size >
	       (gint64) IMAGE_CACHE_HARD_FACTOR * settings_get_size(IMAGE_PREVIEW_CACHE_SIZE);
}

/* Keeps the cache within its size limit after path was written to it;
 * path itself stays */
void image_cache_enforce_limit(const char *path)
{
	char *dir_name;
	const char *name = NULL;

	if (path != NULL && cache_dir != NULL) {
		dir_name = g_path_get_dirname(path);
		if (strcmp(dir_name, cache_dir) == 0)
			name = path + strlen(cache_dir) + 1;
		g_free(dir_name);
	}
	cache_cleanup(name);
}

/* Periodic cleanup callback */
static gboolean cleanup_timer_cb(gpointer data)
{
	image_cache_cleanup();
	return TRUE;  /* Continue timer */
}

/* Print cache statistics */
void image_cache_print_stats(void)
{
	char *size_str;

	if (cache_stats.total_size >= 1024 * 1024) {
		size_str = g_strdup_printf("%.1f MB", cache_stats.total_size / (1024.0 * 1024.0));
	} else if (cache_stats.total_size >= 1024) {
		size_str = g_strdup_printf("%.1f KB", cache_stats.total_size / 1024.0);
	} else {
		size_str = g_strdup_printf("%" G_GINT64_FORMAT " bytes", cache_stats.total_size);
	}

	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "Image cache statistics:");
	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "  Directory: %s", cache_dir ? cache_dir : "(not initialized)");
	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "  Entries: %d", cache_stats.entry_count);
	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "  Total size: %s", size_str);
	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "  Cache hits: %d", cache_stats.hits);
	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "  Cache misses: %d", cache_stats.misses);

	g_free(size_str);
}

/* Initialize cache system */
void image_cache_init(void)
{
	if (!ensure_cache_dir()) {
		g_warning("image-cache: Failed to initialize cache directory");
		return;
	}

	/* Initial cleanup */
	image_cache_cleanup();

	/* Start periodic cleanup timer (every 30 minutes) */
	cleanup_timer_tag = g_timeout_add(30 * 60 * 1000, cleanup_timer_cb, NULL);
}

/* Deinitialize cache system */
void image_cache_deinit(void)
{
	if (cleanup_timer_tag != 0) {
		g_source_remove(cleanup_timer_tag);
		cleanup_timer_tag = 0;
	}

	g_free(cache_dir);
	cache_dir = NULL;
}
