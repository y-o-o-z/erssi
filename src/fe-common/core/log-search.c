/*
 log-search.c : bounded streaming search over autolog files (pure)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#include "module.h"
#include <irssi/src/fe-common/core/fe-log.h>
#include <irssi/src/fe-common/core/log-search.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_SEARCH_MAX_DEPTH 4
#define LOG_SEARCH_LINE_BUF (64 * 1024)
#define LOG_SEARCH_MAX_CHARS 400
/* hard bounds on the directory walk, whatever max_files says */
#define LOG_SEARCH_MAX_MATCHES 50000
#define LOG_SEARCH_MAX_ENTRIES 500000

static gboolean is_name_char(char c)
{
	return g_ascii_isalnum(c) || c == '_';
}

char *log_search_glob(const char *autolog_path, const char *home, const char *tag,
                      const char *target)
{
	GString *out = g_string_new(NULL);
	const char *p = autolog_path;

	if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
		g_string_append(out, home != NULL ? home : "");
		p++;
	}

	while (*p != '\0') {
		if (*p == '$') {
			GString *name = g_string_new(NULL);

			p++;
			if (*p == '{') {
				const char *end = strchr(p, '}');

				if (end != NULL) {
					g_string_append_len(name, p + 1, end - p - 1);
					p = end + 1;
				}
			} else if (g_ascii_isdigit(*p)) {
				g_string_append_c(name, *p++);
			} else {
				while (is_name_char(*p))
					g_string_append_c(name, *p++);
			}

			if ((strcmp(name->str, "tag") == 0 || strcmp(name->str, "1") == 0) &&
			    tag != NULL) {
				g_string_append(out, tag);
			} else if ((strcmp(name->str, "0") == 0) && target != NULL) {
				char *esc = fe_log_escape_target(target);
				char *q;

				for (q = esc; *q != '\0'; q++) {
					*q = g_ascii_tolower(*q);
					/* autolog doubled '%' for strftime(), the file name has one */
					if (q[0] == '%' && q[1] == '%')
						memmove(q, q + 1, strlen(q));
				}
				g_string_append(out, esc);
				g_free(esc);
			} else {
				g_string_append_c(out, '*');
			}
			g_string_free(name, TRUE);
		} else if (*p == '%') {
			if (p[1] == '%') {
				g_string_append_c(out, '%');
				p += 2;
			} else {
				g_string_append_c(out, '*');
				p += (p[1] != '\0') ? 2 : 1;
			}
		} else {
			g_string_append_c(out, *p++);
		}
	}

	return g_string_free(out, FALSE);
}

typedef struct {
	char *path;
	time_t mtime;
} FOUND_FILE;

typedef struct {
	GPatternSpec *spec;
	GArray *found;
	int entries;
} WALK;

static void walk_dir(WALK *w, const char *dir, int depth)
{
	GDir *d;
	const char *name;

	if (depth > LOG_SEARCH_MAX_DEPTH || (d = g_dir_open(dir, 0, NULL)) == NULL)
		return;

	while ((name = g_dir_read_name(d)) != NULL) {
		char *path;
		struct stat st;

		if (++w->entries > LOG_SEARCH_MAX_ENTRIES)
			break;
		path = g_build_filename(dir, name, NULL);
		/* lstat: a symlink (to a file or to a directory) is never followed, so the
		   walk cannot leave the base through a link planted inside it */
		if (lstat(path, &st) != 0) {
			g_free(path);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			walk_dir(w, path, depth + 1);
			g_free(path);
		} else if (S_ISREG(st.st_mode) && g_pattern_spec_match_string(w->spec, path) &&
		           w->found->len < LOG_SEARCH_MAX_MATCHES) {
			FOUND_FILE ff = {path, st.st_mtime};

			g_array_append_val(w->found, ff);
		} else {
			g_free(path);
		}
	}
	g_dir_close(d);
}

static int found_cmp(gconstpointer a, gconstpointer b)
{
	const FOUND_FILE *x = a, *y = b;

	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? 1 : -1; /* newest first */
	return strcmp(x->path, y->path);
}

GPtrArray *log_search_files(const char *glob, int max_files)
{
	GPtrArray *res = g_ptr_array_new_with_free_func(g_free);
	WALK w;
	char *base;
	const char *wild = glob + strcspn(glob, "*?");
	guint i;

	if (max_files <= 0 || glob[0] == '\0')
		return res;

	w.spec = g_pattern_spec_new(glob);
	w.found = g_array_new(FALSE, FALSE, sizeof(FOUND_FILE));
	w.entries = 0;

	if (*wild == '\0') {
		struct stat st;

		if (lstat(glob, &st) == 0 && S_ISREG(st.st_mode)) {
			FOUND_FILE ff = {g_strdup(glob), st.st_mtime};

			g_array_append_val(w.found, ff);
		}
	} else {
		const char *slash = wild;

		while (slash > glob && slash[-1] != '/')
			slash--;
		base = slash > glob ? g_strndup(glob, slash - glob - 1) : g_strdup("/");
		if (base[0] == '\0') {
			g_free(base);
			base = g_strdup("/");
		}
		walk_dir(&w, base, 1);
		g_free(base);
	}

	g_array_sort(w.found, found_cmp);
	for (i = 0; i < w.found->len; i++) {
		FOUND_FILE *ff = &g_array_index(w.found, FOUND_FILE, i);

		if (i < (guint)max_files)
			g_ptr_array_add(res, ff->path);
		else
			g_free(ff->path);
	}
	g_array_free(w.found, TRUE);
	g_pattern_spec_free(w.spec);
	return res;
}

void log_search_hit_free(LOG_SEARCH_HIT *hit)
{
	if (hit == NULL)
		return;
	g_free(hit->file_label);
	g_free(hit->date);
	g_free(hit->line);
	g_free(hit);
}

void log_search_result_clear(LOG_SEARCH_RESULT *res)
{
	if (res->hits != NULL)
		g_ptr_array_free(res->hits, TRUE);
	res->hits = NULL;
}

static char *cut_line(const char *buf, gsize len)
{
	char *valid = g_utf8_make_valid(buf, len);
	char *cut;

	if (g_utf8_strlen(valid, -1) > LOG_SEARCH_MAX_CHARS) {
		char *end = g_utf8_offset_to_pointer(valid, LOG_SEARCH_MAX_CHARS);

		char *head = g_strndup(valid, end - valid);

		cut = g_strconcat(head, "\xe2\x80\xa6", NULL);
		g_free(head);
		g_free(valid);
		return cut;
	}
	return valid;
}

/* hits of one file, the newest max_hits, in a ring */
typedef struct {
	LOG_SEARCH_HIT **slot;
	int size, count, next;
} RING;

static void ring_add(RING *r, LOG_SEARCH_HIT *hit)
{
	if (r->slot[r->next] != NULL)
		log_search_hit_free(r->slot[r->next]);
	r->slot[r->next] = hit;
	r->next = (r->next + 1) % r->size;
	if (r->count < r->size)
		r->count++;
}

/* append the ring, oldest first, to out */
static void ring_flush(RING *r, GPtrArray *out)
{
	int i, start = (r->next - r->count + r->size) % r->size;

	for (i = 0; i < r->count; i++) {
		int idx = (start + i) % r->size;

		g_ptr_array_add(out, r->slot[idx]);
		r->slot[idx] = NULL;
	}
	r->count = r->next = 0;
}

static void scan_file(FILE *f, const char *label, Regex *re, RING *ring)
{
	char *buf = g_malloc(LOG_SEARCH_LINE_BUF);
	char *date = g_strdup("");
	gboolean skipping = FALSE;

	while (fgets(buf, LOG_SEARCH_LINE_BUF, f) != NULL) {
		gsize len = strlen(buf);
		gboolean complete = len > 0 && buf[len - 1] == '\n';

		if (skipping) {
			/* the tail of a line longer than the buffer */
			skipping = !complete;
			continue;
		}
		skipping = !complete && !feof(f);
		while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
			buf[--len] = '\0';

		if (buf[0] == '-' && (strncmp(buf, "--- Log opened ", 15) == 0 ||
		                      strncmp(buf, "--- Day changed ", 16) == 0)) {
			g_free(date);
			date = cut_line(buf + 4, len - 4);
		}

		if (i_regex_match(re, buf, 0, NULL)) {
			LOG_SEARCH_HIT *hit = g_new0(LOG_SEARCH_HIT, 1);

			hit->file_label = g_strdup(label);
			hit->date = g_strdup(date);
			hit->line = cut_line(buf, len);
			ring_add(ring, hit);
		}
	}
	g_free(date);
	g_free(buf);
}

void log_search_run(GPtrArray *files, Regex *re, int max_hits, gint64 max_bytes,
                    LOG_SEARCH_RESULT *res)
{
	guint i;

	memset(res, 0, sizeof(*res));
	res->hits = g_ptr_array_new_with_free_func((GDestroyNotify) log_search_hit_free);
	if (max_hits <= 0)
		return;

	for (i = 0; i < files->len; i++) {
		const char *path = files->pdata[i];
		RING ring;
		struct stat st;
		FILE *f;
		int fd;
		char *label;
		GPtrArray *older;

		if ((int)res->hits->len >= max_hits)
			break;

		/* O_NONBLOCK: opening a FIFO must not wait for a writer; regular files
		   ignore it. O_NOFOLLOW: a path swapped for a symlink after the walk. */
		fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
		    (f = fdopen(fd, "r")) == NULL) {
			close(fd);
			continue;
		}
		if (res->bytes + st.st_size > max_bytes) {
			res->size_limit = TRUE;
			fclose(f);
			break;
		}
		setvbuf(f, NULL, _IOFBF, LOG_SEARCH_LINE_BUF);

		ring.size = max_hits;
		ring.count = ring.next = 0;
		ring.slot = g_new0(LOG_SEARCH_HIT *, max_hits);
		label = g_path_get_basename(path);
		scan_file(f, label, re, &ring);
		g_free(label);
		fclose(f);

		res->files++;
		res->bytes += st.st_size;

		/* this file is older than everything collected so far: its hits go
		   in front, then the oldest overflow is dropped */
		older = g_ptr_array_new();
		ring_flush(&ring, older);
		g_free(ring.slot);
		g_ptr_array_extend(older, res->hits, NULL, NULL);
		/* the pointers moved to OLDER: empty HITS without freeing them */
		g_ptr_array_set_free_func(res->hits, NULL);
		g_ptr_array_set_size(res->hits, 0);
		g_ptr_array_set_free_func(res->hits, (GDestroyNotify) log_search_hit_free);
		{
			guint skip = older->len > (guint)max_hits ? older->len - max_hits : 0;
			guint j;

			for (j = 0; j < older->len; j++) {
				if (j < skip)
					log_search_hit_free(older->pdata[j]);
				else
					g_ptr_array_add(res->hits, older->pdata[j]);
			}
		}
		g_ptr_array_free(older, TRUE);
	}
}

static char *clean_field(const char *s)
{
	char *c = g_strdup(s != NULL ? s : "");
	char *p;

	for (p = c; *p != '\0'; p++)
		if (*p == '\t' || *p == '\r' || *p == '\n')
			*p = ' ';
	return c;
}

char *log_search_encode(const LOG_SEARCH_HIT *hit)
{
	char *l = clean_field(hit->file_label), *d = clean_field(hit->date),
	     *t = clean_field(hit->line);
	char *out = g_strdup_printf("H\t%s\t%s\t%s\n", l, d, t);

	g_free(l);
	g_free(d);
	g_free(t);
	return out;
}

gboolean log_search_decode(const char *line, LOG_SEARCH_HIT *hit)
{
	char *copy, **parts;
	gboolean ok;

	if (line == NULL || line[0] != 'H' || line[1] != '\t')
		return FALSE;
	copy = g_strdup(line + 2);
	g_strchomp(copy);
	parts = g_strsplit(copy, "\t", 3);
	ok = g_strv_length(parts) == 3;
	if (ok) {
		hit->file_label = g_strdup(parts[0]);
		hit->date = g_strdup(parts[1]);
		hit->line = g_strdup(parts[2]);
	}
	g_strfreev(parts);
	g_free(copy);
	return ok;
}

char *log_search_encode_summary(const LOG_SEARCH_RESULT *r)
{
	return g_strdup_printf("S\t%u\t%d\t%" G_GINT64_FORMAT "\t%d\t%d\n",
	                       r->hits != NULL ? r->hits->len : 0, r->files, r->bytes,
	                       r->size_limit ? 1 : 0, r->file_limit ? 1 : 0);
}

gboolean log_search_decode_summary(const char *line, int *hits, LOG_SEARCH_RESULT *r)
{
	char *copy;
	char **p;
	gint64 v[5];
	int i;
	gboolean ok = FALSE;

	if (line == NULL || line[0] != 'S' || line[1] != '\t')
		return FALSE;
	copy = g_strdup(line + 2);
	g_strchomp(copy);
	p = g_strsplit(copy, "\t", 0);
	if (g_strv_length(p) == 5) {
		ok = TRUE;
		for (i = 0; i < 5 && ok; i++)
			ok = g_ascii_string_to_signed(p[i], 10, 0, G_MAXINT64, &v[i], NULL);
	}
	if (ok) {
		*hits = (int)v[0];
		r->files = (int)v[1];
		r->bytes = v[2];
		r->size_limit = v[3] != 0;
		r->file_limit = v[4] != 0;
	}
	g_strfreev(p);
	g_free(copy);
	return ok;
}
