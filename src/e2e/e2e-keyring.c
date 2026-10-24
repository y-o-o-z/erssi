/*
 e2e-keyring.c : keyring.json on disk, as rpe2e.pl 0.2.2 keeps it

    Copyright (C) 2026 the Shellter team
    Ported from rpe2e.pl, Copyright (c) 2026 outragedevs, MIT licence
    (contrib/rpe2e/LICENSE).

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The load/save rules of rpe2e.pl 0.2.2 (its change "erssi/y-o-o-z (7)"),
 with its messages: an existing file that cannot be read is never
 replaced, a corrupt one is kept aside, a write never leaves a truncated
 keyring behind. One rule more: after a corrupt keyring was moved aside,
 the missing file does not mean "no E2E anywhere" (rpe2e.pl started fresh
 and the gate let everything out in clear text) - the keyring is LOST
 until /e2e reset starts a new one (e2e_keyring_start_fresh).
*/

#include "e2e-keyring.h"
#include "e2e-crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define E2E_REASON_SYMLINK "a symbolic link (not followed)"

static const char *const object_members[] = {
	"peers", "outgoing", "incoming", "channels", "pending",
	"outgoing_recipients", "pending_inbound",
	/* this client's own (rpe2e.pl keeps members it does not know) */
	"accepted", "seen_rekeys", NULL
};
static const char *const array_members[] = {
	"autotrust", "pending_trust_change", NULL
};

E2E_JSON *e2e_keyring_new(void)
{
	E2E_JSON *kr = e2e_json_new(E2E_JSON_OBJECT);
	int i;

	e2e_json_set(kr, "identity", e2e_json_new(E2E_JSON_NULL));
	for (i = 0; object_members[i] != NULL; i++)
		e2e_json_set(kr, object_members[i], e2e_json_new(E2E_JSON_OBJECT));
	for (i = 0; array_members[i] != NULL; i++)
		e2e_json_set(kr, array_members[i], e2e_json_new(E2E_JSON_ARRAY));
	return kr;
}

/* missing members are added as rpe2e.pl's "//= {}" does; a member of the
   wrong type would make every later lookup guess, so such a file is not
   used (and, like an unreadable one, never overwritten) */
static gboolean normalize(E2E_JSON *kr, char **error)
{
	E2E_JSON *node;
	GPtrArray *keys;
	int i;

	node = e2e_json_get(kr, "identity");
	if (node == NULL)
		e2e_json_set(kr, "identity", e2e_json_new(E2E_JSON_NULL));
	else if (node->type != E2E_JSON_NULL && node->type != E2E_JSON_OBJECT) {
		*error = g_strdup("unexpected layout: identity is not an object");
		return FALSE;
	}
	for (i = 0; object_members[i] != NULL; i++) {
		node = e2e_json_get(kr, object_members[i]);
		if (node == NULL || node->type == E2E_JSON_NULL)
			e2e_json_set(kr, object_members[i], e2e_json_new(E2E_JSON_OBJECT));
		else if (node->type != E2E_JSON_OBJECT) {
			*error = g_strdup_printf("unexpected layout: %s is not an object",
			                         object_members[i]);
			return FALSE;
		}
	}
	/* a channel config that is no object: rpe2e.pl failed (and refused to
	   send) on it; reading it as "off" would send in clear text */
	keys = e2e_json_keys(e2e_json_get(kr, "channels"));
	for (i = 0; i < (int) keys->len; i++) {
		node = e2e_json_get(e2e_json_get(kr, "channels"), g_ptr_array_index(keys, i));
		if (node->type != E2E_JSON_OBJECT && node->type != E2E_JSON_NULL) {
			*error = g_strdup_printf("unexpected layout: channels/%s is not an object",
			                         (char *) g_ptr_array_index(keys, i));
			g_ptr_array_unref(keys);
			return FALSE;
		}
	}
	g_ptr_array_unref(keys);
	for (i = 0; array_members[i] != NULL; i++) {
		node = e2e_json_get(kr, array_members[i]);
		if (node == NULL || node->type == E2E_JSON_NULL)
			e2e_json_set(kr, array_members[i], e2e_json_new(E2E_JSON_ARRAY));
		else if (node->type != E2E_JSON_ARRAY) {
			*error = g_strdup_printf("unexpected layout: %s is not a list",
			                         array_members[i]);
			return FALSE;
		}
	}
	return TRUE;
}

static const char *file_kind(mode_t mode)
{
	if (S_ISDIR(mode))
		return "a directory";
	if (S_ISFIFO(mode))
		return "a FIFO";
	if (S_ISCHR(mode) || S_ISBLK(mode))
		return "a device";
	if (S_ISSOCK(mode))
		return "a socket";
	return "a special file";
}

/* path opened for reading only when it is a regular file: a symlink is
   not followed, and a FIFO or a device neither blocks the client nor is
   read (O_NONBLOCK until the type is known). The type is checked on the
   descriptor itself, so nothing can swap the file after a check. own:
   it must also belong to this user (the files in ~/.erssi/rpe2e). -1
   with *error and errno set on failure. */
static int open_regular(const char *path, gboolean own, struct stat *st, char **error)
{
	struct stat lst;
	int fd, flags, saved;

	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		saved = errno;
		/* ELOOP on Linux, EMLINK on FreeBSD */
		if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode))
			*error = g_strdup(E2E_REASON_SYMLINK);
		else
			*error = g_strdup(g_strerror(saved));
		errno = saved;
		return -1;
	}
	if (fstat(fd, st) != 0) {
		saved = errno;
		*error = g_strdup(g_strerror(saved));
		goto fail;
	}
	if (!S_ISREG(st->st_mode)) {
		saved = EINVAL;
		*error = g_strdup_printf("not a regular file (%s)", file_kind(st->st_mode));
		goto fail;
	}
	if (own && st->st_uid != geteuid()) {
		saved = EPERM;
		*error = g_strdup_printf("it belongs to another user (uid %ld)", (long) st->st_uid);
		goto fail;
	}
	/* a regular file: reads block as they always did */
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
		saved = errno;
		*error = g_strdup(g_strerror(saved));
		goto fail;
	}
	return fd;
fail:
	close(fd);
	errno = saved;
	return -1;
}

char *e2e_read_regular_file(const char *path, gsize max, gboolean own, gsize *len, char **error)
{
	struct stat st;
	GString *data;
	char buf[8192];
	ssize_t n;
	int fd, saved;

	fd = open_regular(path, own, &st, error);
	if (fd < 0)
		return NULL;
	if (st.st_size > 0 && (guint64) st.st_size > max) {
		close(fd);
		*error = g_strdup_printf("larger than %" G_GSIZE_FORMAT " bytes", max);
		errno = EFBIG;
		return NULL;
	}
	/* sized up front: a grown buffer would leave copies of the keys behind
	   in freed memory */
	data = g_string_sized_new(st.st_size > 0 ? (gsize) st.st_size + 1 : 8192);
	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			saved = errno;
			*error = g_strdup(g_strerror(saved));
			goto fail;
		}
		if (n == 0)
			break;
		g_string_append_len(data, buf, n);
		/* it grew while read */
		if (data->len > max) {
			saved = EFBIG;
			*error = g_strdup_printf("larger than %" G_GSIZE_FORMAT " bytes", max);
			goto fail;
		}
	}
	close(fd);
	e2e_wipe(buf, sizeof(buf));
	*len = data->len;
	return g_string_free(data, FALSE);
fail:
	close(fd);
	e2e_wipe(buf, sizeof(buf));
	e2e_wipe(data->str, data->len);
	g_string_free(data, TRUE);
	errno = saved;
	return NULL;
}

/* a keyring.json.corrupt-* next to path: one was moved aside */
static gboolean corrupt_one_aside(const char *path)
{
	char *dir = g_path_get_dirname(path), *base = g_path_get_basename(path), *prefix;
	const char *name;
	gboolean found = FALSE;
	GDir *d;

	prefix = g_strconcat(base, ".corrupt-", NULL);
	d = g_dir_open(dir, 0, NULL);
	while (d != NULL && !found && (name = g_dir_read_name(d)) != NULL)
		found = g_str_has_prefix(name, prefix);
	if (d != NULL)
		g_dir_close(d);
	g_free(prefix);
	g_free(base);
	g_free(dir);
	return found;
}

gboolean e2e_keyring_lost(const char *path)
{
	struct stat st;

	return lstat(path, &st) != 0 && errno == ENOENT && corrupt_one_aside(path);
}

E2E_JSON *e2e_keyring_parse_checked(const char *data, size_t len, E2E_LOAD_STATUS *status,
                                    char **error)
{
	const char *perr = NULL;
	E2E_JSON *kr;

	*status = E2E_LOAD_OK;
	kr = e2e_json_parse(data, len, &perr);
	if (kr == NULL) {
		*status = E2E_LOAD_PARSE_ERROR;
		*error = g_strdup("not valid JSON");
		return e2e_keyring_new();
	}
	if (!normalize(kr, error)) {
		*status = E2E_LOAD_READ_ERROR;
		e2e_json_free(kr);
		return e2e_keyring_new();
	}
	return kr;
}

E2E_JSON *e2e_keyring_load_checked(const char *path, E2E_LOAD_STATUS *status, char **error)
{
	struct stat st;
	E2E_JSON *kr;
	char *data;
	gsize len;

	*status = E2E_LOAD_OK;
	/* lstat: a symlink, dangling or not, is no "absent" keyring that the
	   next save would replace */
	if (lstat(path, &st) != 0) {
		if (errno == ENOENT && corrupt_one_aside(path)) {
			*status = E2E_LOAD_LOST;
			*error = g_strdup("the corrupt keyring was moved aside");
			return e2e_keyring_new();
		}
		if (errno == ENOENT)
			return e2e_keyring_new();
		/* rpe2e.pl's -e would call this "absent"; a path that cannot even
		   be looked at is reported instead */
		*status = E2E_LOAD_READ_ERROR;
		*error = g_strdup(g_strerror(errno));
		return e2e_keyring_new();
	}
	data = e2e_read_regular_file(path, E2E_MAX_KEYRING_SIZE, TRUE, &len, error);
	if (data == NULL) {
		*status = E2E_LOAD_READ_ERROR;
		return e2e_keyring_new();
	}
	kr = e2e_keyring_parse_checked(data, len, status, error);
	e2e_wipe(data, len);
	g_free(data);
	return kr;
}

E2E_JSON *e2e_keyring_load(const char *path, char **notice)
{
	E2E_LOAD_STATUS status;

	return e2e_keyring_load_status(path, &status, notice);
}

E2E_JSON *e2e_keyring_load_status(const char *path, E2E_LOAD_STATUS *status, char **notice)
{
	char *error = NULL, *aside;
	E2E_JSON *kr;
	int i;

	*notice = NULL;
	kr = e2e_keyring_load_checked(path, status, &error);
	if (*status == E2E_LOAD_PARSE_ERROR) {
		/* repartee's script started fresh here and the next save wrote a
		   new identity over the real file; keep it for the user */
		aside = g_strdup_printf("%s.corrupt-%ld", path, (long) time(NULL));
		for (i = 1; g_file_test(aside, G_FILE_TEST_EXISTS); i++) {
			g_free(aside);
			aside = g_strdup_printf("%s.corrupt-%ld.%d", path, (long) time(NULL), i);
		}
		if (rename(path, aside) == 0) {
			*status = E2E_LOAD_LOST;
			*notice = g_strdup_printf("keyring %s is not valid JSON — moved aside to %s. " E2E_LOST_HINT,
			                          path, aside);
		} else {
			*notice = g_strdup_printf("keyring %s is not valid JSON and cannot be moved aside (%s) — not saving until it is fixed",
			                          path, g_strerror(errno));
		}
		g_free(aside);
	} else if (*status == E2E_LOAD_LOST) {
		*notice = g_strdup_printf("keyring %s: the corrupt keyring was moved aside (%s.corrupt-*). " E2E_LOST_HINT,
		                          path, path);
	} else if (*status == E2E_LOAD_READ_ERROR) {
		/* a link shared with another client worked before: say how to
		   keep sharing it */
		*notice = g_strdup_printf("cannot read keyring %s: %s — E2E state not loaded, nothing is saved until it is readable%s",
		                          path, error,
		                          strcmp(error, E2E_REASON_SYMLINK) == 0 ?
		                          ". keyring.json must be a regular file: replace the link with the file, or link the whole rpe2e directory instead" : "");
	}
	g_free(error);
	return kr;
}

gboolean e2e_same_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
	       a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

void e2e_keyring_cache_clear(E2E_KEYRING_CACHE *cache)
{
	e2e_json_free(cache->kr);
	cache->kr = NULL;
}

/* kept only when the file is the same before and after the read (a
   writer that replaced it meanwhile is read again next time), and older
   than the current second: a write in place within one tick of the
   file system's clock could leave size and times as they were (as the
   status bar's and fe-web's caches do) */
E2E_JSON *e2e_keyring_cache_load(E2E_KEYRING_CACHE *cache, const char *path,
                                 E2E_LOAD_STATUS *status, char **notice)
{
	struct stat before, after;
	gboolean have = lstat(path, &before) == 0 && S_ISREG(before.st_mode);
	E2E_JSON *kr;

	if (have && cache->kr != NULL && e2e_same_file(&before, &cache->st) &&
	    before.st_mode == cache->st.st_mode && before.st_uid == cache->st.st_uid) {
		*status = E2E_LOAD_OK;
		*notice = NULL;
		return e2e_json_copy(cache->kr);
	}
	e2e_keyring_cache_clear(cache);
	cache->reads++;
	kr = e2e_keyring_load_status(path, status, notice);
	if (have && *status == E2E_LOAD_OK && lstat(path, &after) == 0 &&
	    e2e_same_file(&before, &after) && before.st_mode == after.st_mode &&
	    before.st_uid == after.st_uid && after.st_mtim.tv_sec < time(NULL)) {
		cache->kr = e2e_json_copy(kr);
		cache->st = after;
	}
	return kr;
}

void e2e_keyring_cache_saved(E2E_KEYRING_CACHE *cache, const char *path, const E2E_JSON *kr)
{
	struct stat st;

	e2e_keyring_cache_clear(cache);
	/* the file as the rename left it (ours, whatever its times);
	   another writer in between is the race every load-change-save of
	   the keyring has */
	if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		cache->kr = e2e_json_copy(kr);
		cache->st = st;
	}
}

static gboolean write_all(int fd, const char *data, size_t len)
{
	ssize_t n;

	while (len > 0) {
		n = write(fd, data, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			return FALSE;
		}
		data += n;
		len -= n;
	}
	return TRUE;
}

static void fsync_dir(const char *path)
{
	char *dir = g_path_get_dirname(path);
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC);

	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	g_free(dir);
}

/* a new file (O_EXCL, never through a symlink) with data, fsync'ed */
static gboolean write_new(const char *path, const char *data, size_t len, int *err)
{
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		*err = errno;
		return FALSE;
	}
	if (!write_all(fd, data, len) || fsync(fd) != 0) {
		*err = errno;
		close(fd);
		unlink(path);
		return FALSE;
	}
	if (close(fd) != 0) {
		*err = errno;
		unlink(path);
		return FALSE;
	}
	return TRUE;
}

gboolean e2e_write_file_atomic(const char *path, const char *data, size_t len,
                               const char *what, char **error)
{
	char *tmp;
	int fd, saved;

	tmp = g_strdup_printf("%s.tmp.%ld.%ld", path, (long) getpid(), (long) time(NULL));
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		*error = g_strdup_printf("cannot write %s: %s", what, g_strerror(errno));
		g_free(tmp);
		return FALSE;
	}
	/* every write is checked and the data is on disk before the rename:
	   a full disk can no longer put a truncated file in place */
	if (!write_all(fd, data, len) || fsync(fd) != 0) {
		saved = errno;
		close(fd);
		goto fail;
	}
	if (close(fd) != 0) {
		saved = errno;
		goto fail;
	}
	/* rename replaces whatever is at path (a symlink too), never follows it */
	if (rename(tmp, path) != 0) {
		saved = errno;
		unlink(tmp);
		*error = g_strdup_printf("cannot replace %s: %s", what, g_strerror(saved));
		g_free(tmp);
		return FALSE;
	}
	fsync_dir(path);
	g_free(tmp);
	return TRUE;
fail:
	unlink(tmp);
	*error = g_strdup_printf("cannot write %s (%s) — the previous %s is kept",
	                         what, g_strerror(saved), what);
	g_free(tmp);
	return FALSE;
}

static gboolean keyring_write(const char *path, const E2E_JSON *kr, gboolean fresh,
                              char **error)
{
	struct stat st;
	E2E_LOAD_STATUS status;
	E2E_JSON *current;
	char *data, *err = NULL;
	gboolean ok;
	size_t len;

	if (lstat(path, &st) == 0) {
		if (fresh) {
			*error = g_strdup_printf("%s exists — there is nothing to reset", path);
			return FALSE;
		}
	} else if (errno == ENOENT && !fresh && corrupt_one_aside(path)) {
		/* a new keyring here would hide that the state was lost */
		*error = g_strdup("keyring NOT saved: the corrupt keyring was moved aside and E2E state is lost — /e2e reset starts a new one");
		return FALSE;
	}
	if (lstat(path, &st) == 0 || errno != ENOENT) {
		current = e2e_keyring_load_checked(path, &status, &err);
		e2e_json_free(current);
		if (status != E2E_LOAD_OK) {
			*error = g_strdup_printf("keyring NOT saved: %s exists but cannot be read (%s) — fix it or move it aside",
			                         path, err);
			g_free(err);
			return FALSE;
		}
	}

	data = e2e_json_encode(kr, FALSE);
	len = strlen(data);
	ok = e2e_write_file_atomic(path, data, len, "keyring", error);
	e2e_wipe(data, len);
	g_free(data);
	return ok;
}

gboolean e2e_keyring_save(const char *path, const E2E_JSON *kr, char **error)
{
	return keyring_write(path, kr, FALSE, error);
}

gboolean e2e_keyring_start_fresh(const char *path, const E2E_JSON *kr, char **error)
{
	return keyring_write(path, kr, TRUE, error);
}

char *e2e_keyring_snapshot(const E2E_JSON *kr)
{
	return e2e_json_encode(kr, FALSE);
}

char *e2e_keyring_backup(const char *path, char **error)
{
	struct stat st;
	char *data, *bak;
	gsize len;
	int err = 0, n;

	if (lstat(path, &st) != 0 && errno == ENOENT)
		return g_strdup("");
	data = e2e_read_regular_file(path, E2E_MAX_KEYRING_SIZE, TRUE, &len, error);
	if (data == NULL)
		return NULL;
	bak = g_strdup_printf("%s.bak-%ld", path, (long) time(NULL));
	/* two in one second (a reset, then the import of its backup): the
	   later one gets a number, nothing is overwritten */
	for (n = 1; !write_new(bak, data, len, &err); n++) {
		g_free(bak);
		bak = NULL;
		if (err != EEXIST || n >= 100) {
			*error = g_strdup(g_strerror(err));
			break;
		}
		bak = g_strdup_printf("%s.bak-%ld.%d", path, (long) time(NULL), n);
	}
	e2e_wipe(data, len);
	g_free(data);
	return bak;
}

gboolean e2e_write_new_file(const char *path, const char *data, size_t len, char **error)
{
	int err;

	if (write_new(path, data, len, &err))
		return TRUE;
	*error = g_strdup(g_strerror(err));
	return FALSE;
}
