#ifndef IRSSI_E2E_OUTQ_H
#define IRSSI_E2E_OUTQ_H

/*
 The module's send queue: lines waiting for the server's flood limit.

 A line is a raw IRC line without CR LF. CIPHER lines are ciphertext,
 ready to go out; PLAIN lines have not been through the gate yet (they
 are decided when they leave the queue). push_front puts a batch ahead of
 everything, in its own order; push_back appends one line.

 The queue owns copies of its lines and wipes each one when it frees it:
 e2e_outq_free(), e2e_outq_drop() and e2e_out_line_free() on a popped
 line. Pure: no irssi.

 A NULL queue, a NULL line, or a batch holding a NULL line is a
 programming error (g_return_if_fail): nothing is queued. Only
 e2e_outq_free() and e2e_out_line_free() take NULL as a no-op.
*/

#include <glib.h>

typedef enum {
	E2E_OUT_CIPHER,
	E2E_OUT_PLAIN		/* not gated yet */
} E2E_OUT_KIND;

typedef struct {
	E2E_OUT_KIND kind;
	char *line;		/* without CR LF */
} E2E_OUT_LINE;

typedef struct _E2E_OUTQ E2E_OUTQ;

E2E_OUTQ *e2e_outq_new(void);
/* wipes every line; NULL is a no-op */
void e2e_outq_free(E2E_OUTQ *q);
gboolean e2e_outq_empty(const E2E_OUTQ *q);
guint e2e_outq_length(const E2E_OUTQ *q);
/* copies of lines (char *), in order, ahead of all queued lines */
void e2e_outq_push_front(E2E_OUTQ *q, E2E_OUT_KIND kind, const GPtrArray *lines);
/* a copy of line, behind all queued lines */
void e2e_outq_push_back(E2E_OUTQ *q, E2E_OUT_KIND kind, const char *line);
/* the first line, which the caller frees with e2e_out_line_free(); NULL
   when empty */
E2E_OUT_LINE *e2e_outq_pop(E2E_OUTQ *q);
/* wipes the text; NULL is a no-op */
void e2e_out_line_free(E2E_OUT_LINE *line);
/* empties the queue, wiping every line; returns the lines dropped and,
   when plain is not NULL, sets *plain to the PLAIN ones among them */
guint e2e_outq_drop(E2E_OUTQ *q, guint *plain);

/*
 The handshake NOTICEs to people the user did not confirm (KEYRSP,
 automatic and reciprocal KEYREQ, and the REKEY of a peer whose KEYRSP
 still waits here): never in irssi's queue while anything else waits.
 They wait here, at most max of them (the oldest goes when another
 comes), and leave one at a time only while irssi's queue and the
 module's own are empty and irssi's flood rule leaves room for the
 user (e2e_stranger_may_send). So the user's lines, the module's chunks,
 REKEYs and the answers to confirmed peers never wait behind them - at
 most behind the one in flight.
*/
typedef struct {
	char *nick;		/* the NOTICE goes to */
	char *line;		/* "NOTICE <nick> :..." */
	char *keyrsp_wire;	/* a KEYRSP: the bare context, else NULL */
	char *keyreq_wire;	/* a KEYREQ of ours: the bare context, else NULL */
	char *rekey_wire;	/* a REKEY behind a KEYRSP: its context, else NULL */
	char *handle;		/* the address it is for (a KEYREQ's), or NULL */
} E2E_STRANGER_LINE;

typedef struct _E2E_STRANGERQ E2E_STRANGERQ;

#define E2E_STRANGERQ_MAX 8
E2E_STRANGERQ *e2e_strangerq_new(guint max);
void e2e_strangerq_free(E2E_STRANGERQ *q);
gboolean e2e_strangerq_empty(const E2E_STRANGERQ *q);
guint e2e_strangerq_length(const E2E_STRANGERQ *q);
/* a copy, behind the others; the oldest that went for it (the caller
   frees them, e2e_stranger_line_free), NULL when none. A KEYRSP that goes
   takes the REKEYs queued behind it along (they would reach a peer with
   no session) */
GPtrArray *e2e_strangerq_push(E2E_STRANGERQ *q, const char *nick, const char *line,
                              const char *keyrsp_wire, const char *keyreq_wire,
                              const char *handle);
/* a KEYRSP or a REKEY to nick (ASCII case) for wire waits here: line (a
   REKEY) goes right behind the last such one, in order (past the cap, it
   pushes nothing out) and TRUE; else FALSE, nothing queued */
gboolean e2e_strangerq_push_after_keyrsp(E2E_STRANGERQ *q, const char *nick, const char *wire,
                                         const char *line);
/* the first one, freed by the caller (e2e_stranger_line_free); NULL */
E2E_STRANGER_LINE *e2e_strangerq_pop(E2E_STRANGERQ *q);
void e2e_stranger_line_free(E2E_STRANGER_LINE *l);
/* irssi's flood rule lets a stranger's NOTICE out now: nothing waits in
   irssi's queue or the module's, the wait after the last line is over
   (times in microseconds; a wait longer than one long line takes is a
   clock set back, and over), cmd_queue_speed has passed since the last
   one, and the burst leaves two lines of cmds_max_at_once to the user */
gboolean e2e_stranger_may_send(gboolean irssi_queue_empty, gboolean own_queue_empty,
                               gint64 now_us, gint64 wait_cmd_us, gint64 last_cmd_us,
                               int cmd_queue_speed_ms, int cmdcount, int max_cmds_at_once);

#endif
