#ifndef IRSSI_FE_ANSI_TERM_HYPERLINK_H
#define IRSSI_FE_ANSI_TERM_HYPERLINK_H

#include <glib.h>

/* Clickable links through the terminal: OSC 8 hyperlinks
 * (ESC ] 8 ; id=ID ; URI ESC \ ... ESC ] 8 ; ; ESC \).
 *
 * Pure helpers: finding the addresses in a formatted line, turning one into
 * a URI that is safe inside the escape sequence, the id that ties the pieces
 * of one wrapped address together, and the choice of auto. Nothing here
 * touches the terminal or any global state except the salt of the ids.
 *
 * The text of a line comes from other people. A link is only ever made of
 * text that is shown, and its target is that same text: never a hidden
 * address behind other words. */

/* longer URIs (after percent-encoding) stay plain text */
#define TERM_LINK_URI_MAX 2048
/* 'e' and 16 hex digits */
#define TERM_LINK_ID_SIZE 18

typedef struct {
	/* byte offsets of the link in the formatted line text: start is
	 * its first character, end the byte after its last one; format
	 * sequences between them belong to it */
	gsize start, end;
	/* only bytes 0x21..0x7e, never ESC, BEL or anything that ends the
	 * sequence */
	char *uri;
	/* only [A-Za-z0-9] */
	char id[TERM_LINK_ID_SIZE];
} TermLinkSpan;

typedef struct _TermLinkList {
	int count;
	TermLinkSpan spans[1]; /* count of them */
} TermLinkList;

/* The links in a formatted line (the text view's line text: \004 format
 * sequences, '\n' between parts). charset is how the text view reads it:
 * TERM_TYPE_UTF8, or else TERM_TYPE_8BIT or TERM_TYPE_BIG5, where a byte
 * of 0x80 or more (a Big5 character: both of its bytes) ends an address.
 * NULL when there is none: a line without "://" costs one strstr(). The
 * ids are left empty. */
TermLinkList *term_link_scan(const char *text, int charset);
void term_link_list_free(TermLinkList *list);

/* The URI for an address as it is shown: code points that may not appear
 * in a URI (non-ASCII, and \ ^ ` { | }) as %XX of their UTF-8 bytes, and
 * every control, space, DEL, < > " refused. NULL when it is refused or
 * longer than TERM_LINK_URI_MAX. */
char *term_link_encode_uri(const char *text, gsize len);

/* TRUE when uri can be written inside the sequence as it is */
gboolean term_link_uri_is_safe(const char *uri);
gboolean term_link_id_is_safe(const char *id);

/* The id of link number index of a line, from key (something that tells
 * the line apart: its address and time) and the salt. */
void term_link_make_id(guint64 key, guint index, char id[TERM_LINK_ID_SIZE]);
/* The salt mixed into every id; 0 until set (the tests). */
void term_link_set_salt(guint64 salt);

/* The environment that identifies the terminal; NULL when unset. tty is
 * the name of the terminal device erssi runs on (ttyname()). */
typedef struct {
	const char *term, *term_program, *lc_terminal, *kitty_window_id, *ghostty_resources_dir,
	    *wezterm_pane, *vte_version, *konsole_version, *wt_session, *inside_emacs, *tmux,
	    *sty, *tty;
} TermLinkEnv;

void term_link_env_from_process(TermLinkEnv *env);

/* What term_hyperlinks auto decides: TRUE only for terminals known to show
 * the links or, inside tmux, to be given them only when the outer terminal
 * can (tmux 3.4 and later; older tmux drops them). FALSE under GNU screen,
 * on a console (TERM linux, a console tty), for dumb, vt*, Emacs and every
 * terminal not known: one that does not understand OSC could print it. */
gboolean term_link_detect(const TermLinkEnv *env);

/* The sequences, with ST (ESC backslash) at their end; the URI comes last, so a
 * ';' in it needs nothing. Open is printf format for id and uri. */
#define TERM_LINK_OPEN_FMT "\033]8;id=%s;%s\033\\"
#define TERM_LINK_CLOSE_SEQ "\033]8;;\033\\"

#endif
