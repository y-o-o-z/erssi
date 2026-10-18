/*
 e2e-outq.c : the module's send queue

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "e2e-outq.h"
#include "e2e-crypto.h"

#include <string.h>

struct _E2E_OUTQ {
	GQueue lines;		/* E2E_OUT_LINE * */
};

static E2E_OUT_LINE *out_line_new(E2E_OUT_KIND kind, const char *line)
{
	E2E_OUT_LINE *l = g_new0(E2E_OUT_LINE, 1);

	l->kind = kind;
	l->line = g_strdup(line);
	return l;
}

void e2e_out_line_free(E2E_OUT_LINE *line)
{
	if (line == NULL)
		return;
	if (line->line != NULL) {
		e2e_wipe(line->line, strlen(line->line));
		g_free(line->line);
	}
	g_free(line);
}

E2E_OUTQ *e2e_outq_new(void)
{
	E2E_OUTQ *q = g_new0(E2E_OUTQ, 1);

	g_queue_init(&q->lines);
	return q;
}

void e2e_outq_free(E2E_OUTQ *q)
{
	if (q == NULL)
		return;
	e2e_outq_drop(q, NULL);
	g_free(q);
}

gboolean e2e_outq_empty(const E2E_OUTQ *q)
{
	g_return_val_if_fail(q != NULL, TRUE);
	return q->lines.length == 0;
}

guint e2e_outq_length(const E2E_OUTQ *q)
{
	g_return_val_if_fail(q != NULL, 0);
	return q->lines.length;
}

void e2e_outq_push_front(E2E_OUTQ *q, E2E_OUT_KIND kind, const GPtrArray *lines)
{
	guint i;

	g_return_if_fail(q != NULL);
	g_return_if_fail(lines != NULL);
	/* all or nothing */
	for (i = 0; i < lines->len; i++)
		g_return_if_fail(g_ptr_array_index(lines, i) != NULL);
	/* last first, so the batch keeps its order ahead of everything */
	for (i = lines->len; i > 0; i--)
		g_queue_push_head(&q->lines,
		                  out_line_new(kind, g_ptr_array_index(lines, i - 1)));
}

void e2e_outq_push_back(E2E_OUTQ *q, E2E_OUT_KIND kind, const char *line)
{
	g_return_if_fail(q != NULL);
	g_return_if_fail(line != NULL);
	g_queue_push_tail(&q->lines, out_line_new(kind, line));
}

E2E_OUT_LINE *e2e_outq_pop(E2E_OUTQ *q)
{
	g_return_val_if_fail(q != NULL, NULL);
	return g_queue_pop_head(&q->lines);
}

guint e2e_outq_drop(E2E_OUTQ *q, guint *plain)
{
	E2E_OUT_LINE *l;
	guint n = 0, p = 0;

	if (plain != NULL)
		*plain = 0;
	g_return_val_if_fail(q != NULL, 0);
	while ((l = g_queue_pop_head(&q->lines)) != NULL) {
		n++;
		if (l->kind == E2E_OUT_PLAIN)
			p++;
		e2e_out_line_free(l);
	}
	if (plain != NULL)
		*plain = p;
	return n;
}

/* ---- strangers' handshake NOTICEs ---- */

struct _E2E_STRANGERQ {
	GQueue lines;
	guint max;
};

E2E_STRANGERQ *e2e_strangerq_new(guint max)
{
	E2E_STRANGERQ *q = g_new0(E2E_STRANGERQ, 1);

	g_queue_init(&q->lines);
	q->max = max > 0 ? max : 1;
	return q;
}

void e2e_stranger_line_free(E2E_STRANGER_LINE *l)
{
	if (l == NULL)
		return;
	e2e_wipe(l->line, strlen(l->line));
	g_free(l->line);
	g_free(l->nick);
	g_free(l->keyrsp_wire);
	g_free(l->keyreq_wire);
	g_free(l->handle);
	g_free(l);
}

void e2e_strangerq_free(E2E_STRANGERQ *q)
{
	E2E_STRANGER_LINE *l;

	if (q == NULL)
		return;
	while ((l = g_queue_pop_head(&q->lines)) != NULL)
		e2e_stranger_line_free(l);
	g_free(q);
}

gboolean e2e_strangerq_empty(const E2E_STRANGERQ *q)
{
	return q == NULL || g_queue_is_empty((GQueue *) &q->lines);
}

guint e2e_strangerq_length(const E2E_STRANGERQ *q)
{
	return q == NULL ? 0 : g_queue_get_length((GQueue *) &q->lines);
}

static E2E_STRANGER_LINE *stranger_line(const char *nick, const char *line, const char *wire)
{
	E2E_STRANGER_LINE *l = g_new0(E2E_STRANGER_LINE, 1);

	l->nick = g_strdup(nick);
	l->line = g_strdup(line);
	l->keyrsp_wire = g_strdup(wire);
	return l;
}

GPtrArray *e2e_strangerq_push(E2E_STRANGERQ *q, const char *nick, const char *line,
                              const char *keyrsp_wire, const char *keyreq_wire,
                              const char *handle)
{
	GPtrArray *dropped = NULL;
	E2E_STRANGER_LINE *l;

	g_return_val_if_fail(q != NULL && nick != NULL && line != NULL, NULL);
	while (g_queue_get_length(&q->lines) >= q->max) {
		if (dropped == NULL)
			dropped = g_ptr_array_new();
		g_ptr_array_add(dropped, g_queue_pop_head(&q->lines));
	}
	l = stranger_line(nick, line, keyrsp_wire);
	l->keyreq_wire = g_strdup(keyreq_wire);
	l->handle = g_strdup(handle);
	g_queue_push_tail(&q->lines, l);
	return dropped;
}

gboolean e2e_strangerq_push_after_keyrsp(E2E_STRANGERQ *q, const char *nick, const char *wire,
                                         const char *line)
{
	GList *tmp, *last = NULL;

	if (q == NULL)
		return FALSE;
	for (tmp = q->lines.head; tmp != NULL; tmp = tmp->next) {
		E2E_STRANGER_LINE *l = tmp->data;

		if (l->keyrsp_wire != NULL && strcmp(l->keyrsp_wire, wire) == 0 &&
		    g_ascii_strcasecmp(l->nick, nick) == 0)
			last = tmp;
	}
	if (last == NULL)
		return FALSE;
	g_queue_insert_after(&q->lines, last, stranger_line(nick, line, NULL));
	return TRUE;
}

E2E_STRANGER_LINE *e2e_strangerq_pop(E2E_STRANGERQ *q)
{
	return q == NULL ? NULL : g_queue_pop_head(&q->lines);
}

gboolean e2e_stranger_may_send(gboolean irssi_queue_empty, gboolean own_queue_empty,
                               gint64 now_us, gint64 wait_cmd_us, gint64 last_cmd_us,
                               int cmd_queue_speed_ms, int cmdcount, int max_cmds_at_once)
{
	/* irssi waits at most 2 s + 1 s per 100 bytes of a 512-byte line */
	const gint64 longest = 7 * G_USEC_PER_SEC;
	gint64 wait = wait_cmd_us - now_us, since = now_us - last_cmd_us;

	if (!irssi_queue_empty || !own_queue_empty)
		return FALSE;
	if (wait > 0 && wait <= longest)
		return FALSE;
	if (since >= 0 && since < (gint64) cmd_queue_speed_ms * 1000 && cmd_queue_speed_ms > 0)
		return FALSE;
	return cmdcount + 2 < MAX(max_cmds_at_once, 3);
}
