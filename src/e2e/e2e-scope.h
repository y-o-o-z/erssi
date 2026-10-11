#ifndef IRSSI_E2E_SCOPE_H
#define IRSSI_E2E_SCOPE_H

/*
 The network scope of a context, and CASEMAPPING folding.

 A context ("#chan", "@ident@host") on the wire is the bare name. In the
 keyring it is stored scoped to the network it belongs to:

   "<label>\x1f<wire ctx>"

 The label is the server's chatnet name if it has one, otherwise the
 server tag; every byte below 0x20 and every '|' in it becomes '_', so a
 label never holds the separator. A key without the separator is a legacy
 (unscoped) row, from rpe2e.pl or from before the scope: it matches any
 network. The wire (c= field, AAD, HKDF info, signatures) always uses the
 bare context, e2e_ctx_wire().

 CASEMAPPING: ascii, rfc1459 and strict-rfc1459 are folded exactly; no
 value means rfc1459; every other value (rfc7613, rfc8265, utf8, unknown,
 empty) gets the superset fold E2E_CASEMAP_UNICODE, which can only equate
 more names than the server does, never fewer.

 Everything here is pure: no irssi, no keyring. Strings returned as
 char * are newly allocated (g_free).
*/

#include <glib.h>

#define E2E_SCOPE_SEP '\x1f'

typedef enum {
	E2E_CASEMAP_RFC1459,
	E2E_CASEMAP_STRICT_RFC1459,
	E2E_CASEMAP_ASCII,
	E2E_CASEMAP_UNICODE
} E2E_CASEMAP;

typedef struct {
	char *label;		/* NULL when the server has no chatnet and no tag */
	E2E_CASEMAP casemap;
	gboolean legacy_ok;	/* whether unscoped rows may be used here */
} E2E_NET;

/* label from chatnet (if non-empty) or tag (if non-empty), casemap from
   the CASEMAPPING value (NULL when the server sent none) */
void e2e_net_init(E2E_NET *net, const char *chatnet, const char *tag, const char *casemapping,
                  gboolean legacy_ok);
/* frees the label; safe to call twice */
void e2e_net_clear(E2E_NET *net);

/* ASCII case-insensitive; NULL -> RFC1459, anything unknown -> UNICODE */
E2E_CASEMAP e2e_casemap_parse(const char *value);
/*
 The folded form of name under map (NULL for NULL):
  - ASCII: A-Z -> a-z;
  - STRICT_RFC1459: ASCII and [ ] \ -> { } |;
  - RFC1459: STRICT_RFC1459 and ~ -> ^;
  - UNICODE, valid UTF-8: NFKC (g_utf8_normalize), then full case folding
    (g_utf8_casefold), then a code point whose fold is a single other code
    point folding back to it (GLib's Cherokee) becomes the smaller of the
    two, then the RFC1459 specials. The result is valid UTF-8.
  - UNICODE, invalid UTF-8: the byte-wise RFC1459 fold.
 Bytes above 0x7f are left alone by the exact maps. fold(fold(x)) == fold(x).
*/
char *e2e_casefold(const char *name, E2E_CASEMAP map);

/* "<label>\x1f<wire>"; a bare copy when net or its label is NULL; NULL for
   a NULL wire */
char *e2e_ctx_scoped(const E2E_NET *net, const char *wire);
/* wire scoped to the same network as like (a bare copy if like is bare) */
char *e2e_ctx_rescope(const char *like, const char *wire);
/* the part after the first 0x1F, or ctx itself; points into ctx */
const char *e2e_ctx_wire(const char *ctx);
/* the label of a scoped ctx; NULL when unscoped */
char *e2e_ctx_network(const char *ctx);
/* TRUE when ctx is scoped to net's label, or unscoped (a legacy row); a
   scoped ctx is on no net without a label, and a NULL ctx on none */
gboolean e2e_ctx_on_net(const char *ctx, const E2E_NET *net);
/* "IRCnet:#chan" for a scoped ctx, a bare one unchanged; NULL for NULL */
char *e2e_ctx_display(const char *ctx);

#endif
