/*
 log-search.c : bounded streaming search over autolog files (pure)

    Copyright (C) 2026 the Shellter team

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
	GPtrArray *segs; /* GPatternSpec *, one per path component below the base */
	GArray *found;
	int entries;
	gboolean truncated;
} WALK;

int log_search_match_cap = LOG_SEARCH_MAX_MATCHES;

static int found_cmp(gconstpointer a, gconstpointer b)
{
	const FOUND_FILE *x = a, *y = b;

	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? 1 : -1; /* newest first */
	return strcmp(x->path, y->path);
}

/* sort newest first and drop everything beyond KEEP */
static void found_trim(WALK *w, guint keep)
{
	guint i;

	g_array_sort(w->found, found_cmp);
	if (w->found->len <= keep)
		return;
	for (i = keep; i < w->found->len; i++)
		g_free(g_array_index(w->found, FOUND_FILE, i).path);
	g_array_set_size(w->found, keep);
	w->truncated = TRUE;
}

static void walk_dir(WALK *w, const char *dir, guint idx)
{
	GDir *d;
	const char *name;
	gboolean last = idx + 1 == w->segs->len;

	if ((d = g_dir_open(dir, 0, NULL)) == NULL)
		return;

	while ((name = g_dir_read_name(d)) != NULL) {
		char *path;
		struct stat st;

		if (!g_pattern_spec_match_string(w->segs->pdata[idx], name))
			continue;
		if (++w->entries > LOG_SEARCH_MAX_ENTRIES) {
			w->truncated = TRUE;
			break;
		}
		path = g_build_filename(dir, name, NULL);
		/* lstat: a symlink (to a file or to a directory) is never followed, so the
		   walk cannot leave the base through a link planted inside it */
		if (lstat(path, &st) != 0) {
			g_free(path);
			continue;
		}
		if (!last && S_ISDIR(st.st_mode)) {
			walk_dir(w, path, idx + 1);
			g_free(path);
		} else if (last && S_ISREG(st.st_mode)) {
			FOUND_FILE ff = {path, st.st_mtime};

			g_array_append_val(w->found, ff);
			/* keep the newest log_search_match_cap, trimming in batches */
			if (w->found->len >= 2 * (guint)log_search_match_cap)
				found_trim(w, log_search_match_cap);
		} else {
			g_free(path);
		}
	}
	g_dir_close(d);
}

GPtrArray *log_search_files(const char *glob, int max_files, gboolean *truncated)
{
	GPtrArray *res = g_ptr_array_new_with_free_func(g_free);
	WALK w;
	GString *base = g_string_new(NULL);
	char **parts;
	GPtrArray *comp = g_ptr_array_new(); /* borrowed from PARTS */
	guint i, first_wild = G_MAXUINT, n;

	if (truncated != NULL)
		*truncated = FALSE;
	/* a relative glob would depend on the working directory: refuse it */
	if (max_files <= 0 || glob[0] != '/') {
		g_string_free(base, TRUE);
		g_ptr_array_free(comp, TRUE);
		return res;
	}

	parts = g_strsplit(glob, "/", 0);
	/* empty components ("//") are dropped */
	for (i = 0; parts[i] != NULL; i++) {
		if (parts[i][0] == '\0')
			continue;
		if (parts[i][strcspn(parts[i], "*?")] != '\0' && first_wild == G_MAXUINT)
			first_wild = comp->len;
		g_ptr_array_add(comp, parts[i]);
	}
	n = comp->len;

	w.found = g_array_new(FALSE, FALSE, sizeof(FOUND_FILE));
	w.segs = g_ptr_array_new_with_free_func((GDestroyNotify)g_pattern_spec_free);
	w.entries = 0;
	w.truncated = FALSE;

	if (first_wild == G_MAXUINT) {
		struct stat st;

		for (i = 0; i < n; i++)
			g_string_append_printf(base, "/%s", (char *)comp->pdata[i]);
		if (n > 0 && lstat(base->str, &st) == 0 && S_ISREG(st.st_mode)) {
			FOUND_FILE ff = {g_strdup(base->str), st.st_mtime};

			g_array_append_val(w.found, ff);
		}
	} else if (n - first_wild <= LOG_SEARCH_MAX_DEPTH) {
		g_string_append_c(base, '/');
		for (i = 0; i < first_wild; i++) {
			g_string_append(base, comp->pdata[i]);
			if (i + 1 < first_wild)
				g_string_append_c(base, '/');
		}
		for (i = first_wild; i < n; i++)
			g_ptr_array_add(w.segs, g_pattern_spec_new(comp->pdata[i]));
		walk_dir(&w, base->str, 0);
	}

	found_trim(&w, log_search_match_cap);
	found_trim(&w, max_files);
	for (i = 0; i < w.found->len; i++)
		g_ptr_array_add(res, g_array_index(w.found, FOUND_FILE, i).path);
	if (truncated != NULL)
		*truncated = w.truncated;
	g_array_free(w.found, TRUE);
	g_ptr_array_free(w.segs, TRUE);
	g_string_free(base, TRUE);
	g_strfreev(parts);
	g_ptr_array_free(comp, TRUE);
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

#define LINE_MAX_BYTES (LOG_SEARCH_LINE_BUF - 1)

typedef struct {
	Regex *re;
	RING *ring;
	const char *label;
	char *date;
} SCAN;

static void scan_line(SCAN *sc, char *buf, gsize len)
{
	char *nul;

	/* the regex API takes a C string: a NUL inside a line becomes a space */
	while ((nul = memchr(buf, '\0', len)) != NULL)
		*nul = ' ';
	while (len > 0 && buf[len - 1] == '\r')
		len--;
	buf[len] = '\0';

	if (buf[0] == '-' &&
	    (strncmp(buf, "--- Log opened ", 15) == 0 || strncmp(buf, "--- Day changed ", 16) == 0)) {
		g_free(sc->date);
		sc->date = cut_line(buf + 4, len - 4);
	}

	if (i_regex_match(sc->re, buf, 0, NULL)) {
		LOG_SEARCH_HIT *hit = g_new0(LOG_SEARCH_HIT, 1);

		hit->file_label = g_strdup(sc->label);
		hit->date = g_strdup(sc->date);
		hit->line = cut_line(buf, len);
		ring_add(sc->ring, hit);
	}
}

/* Read at most LIMIT bytes from F, split on '\n' (NUL-safe); a line is kept up to
   64 KiB - 1 bytes, the rest of a longer line is skipped. DISCARD_FIRST drops the
   (partial) first line. */
static void scan_file(FILE *f, const char *label, Regex *re, RING *ring, gint64 limit,
                      gboolean discard_first)
{
	char *rbuf = g_malloc(LOG_SEARCH_LINE_BUF);
	char *line = g_malloc(LOG_SEARCH_LINE_BUF);
	gsize llen = 0;
	gboolean pending = FALSE, discard = discard_first;
	SCAN sc = {re, ring, label, g_strdup("")};

	while (limit > 0) {
		gsize want = limit < LOG_SEARCH_LINE_BUF ? (gsize)limit : LOG_SEARCH_LINE_BUF;
		gsize n = fread(rbuf, 1, want, f), pos = 0;

		if (n == 0)
			break;
		limit -= n;
		while (pos < n) {
			char *nl = memchr(rbuf + pos, '\n', n - pos);
			gsize end = nl != NULL ? (gsize)(nl - rbuf) : n, seg = end - pos;

			if (discard) {
				if (nl != NULL)
					discard = FALSE;
			} else {
				gsize take = MIN(seg, LINE_MAX_BYTES - llen);

				memcpy(line + llen, rbuf + pos, take);
				llen += take;
				pending = TRUE;
				if (nl != NULL) {
					scan_line(&sc, line, llen);
					llen = 0;
					pending = FALSE;
				}
			}
			pos = nl != NULL ? end + 1 : n;
		}
	}
	if (pending && !discard)
		scan_line(&sc, line, llen);
	g_free(sc.date);
	g_free(line);
	g_free(rbuf);
}

#define LOG_SEARCH_MIN_TAIL (64 * 1024)

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
		gint64 take, budget = max_bytes - res->bytes;
		gboolean tail = FALSE, discard = FALSE;

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
		take = st.st_size;
		if (take > budget) {
			/* does not fit: search its last BUDGET bytes, from a line start,
			   and stop afterwards */
			res->size_limit = TRUE;
			if (budget < LOG_SEARCH_MIN_TAIL) {
				fclose(f);
				break;
			}
			if (fseeko(f, st.st_size - budget - 1, SEEK_SET) != 0) {
				fclose(f);
				break;
			}
			discard = getc(f) != '\n';
			take = budget;
			tail = TRUE;
		}

		ring.size = max_hits;
		ring.count = ring.next = 0;
		ring.slot = g_new0(LOG_SEARCH_HIT *, max_hits);
		label = g_path_get_basename(path);
		scan_file(f, label, re, &ring, take, discard);
		g_free(label);
		fclose(f);

		res->files++;
		res->bytes += take;

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
		if (tail)
			break;
	}
}

/* strip trailing CR/LF only: trailing spaces and TABs are data */
static void chomp_eol(char *s)
{
	gsize len = strlen(s);

	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = '\0';
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
	chomp_eol(copy);
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
	chomp_eol(copy);
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
