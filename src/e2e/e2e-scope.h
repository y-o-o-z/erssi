#ifndef IRSSI_E2E_SCOPE_H
#define IRSSI_E2E_SCOPE_H

/*
 The network scope of a context, and CASEMAPPING folding.

 A context ("#chan", "@ident@host") on the wire is the bare name. In the
 keyring it is stored scoped to the network it belongs to:

   "<label>\x1f<wire ctx>"

 The label is the server's chatnet name if it has one, otherwise the
 server tag. C0 controls, DEL, C1 controls (U+0080-U+009F), '|' and every
 byte that is not part of valid UTF-8 become '_', so a label is valid
 UTF-8 and never holds the separator. A key without the separator is a
 legacy (unscoped) row, from rpe2e.pl or from before the scope: it matches
 any network. The wire (c= field, AAD, HKDF info, signatures) always uses
 the bare context, e2e_ctx_wire().

 A wire context that itself holds 0x1F cannot be told apart from a scoped
 one, so it is refused: e2e_ctx_scoped() and e2e_ctx_rescope() return NULL
 for it, and the callers refuse such a context (fail closed). A legacy
 row whose name holds 0x1F reads as scoped; callers flag such rows rather
 than reinterpret them.

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

/* label from chatnet (if non-empty) or tag (if non-empty), else NULL;
   casemap from the CASEMAPPING value (NULL when the server sent none) */
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
  - UNICODE, valid UTF-8, in this order:
      1. drop the invisible characters (general category Cf, variation
         selectors U+FE00-U+FE0F and U+E0100-U+E01EF, U+034F, the Hangul
         fillers U+115F, U+1160, U+3164, U+FFA0), as NFKC_Casefold does;
      2. NFD, full case folding, NFKD, full case folding: Unicode's
         compatibility caseless match (D146);
      3. NFKC, for one representative;
      4. the RFC1459 specials.
    Full case folding is g_utf8_casefold() followed by a fix for GLib's
    Cherokee: a code point whose fold is a single other code point folding
    back to it becomes the smaller of the two (CaseFolding.txt's target).
    The result is valid UTF-8 and holds none of the characters of step 1.
  - UNICODE, invalid UTF-8: the byte-wise RFC1459 fold.
 Bytes above 0x7f are left alone by the exact maps. fold(fold(x)) == fold(x).
 U+0130 and U+0131 follow full case folding (U+0130 -> i U+0307), not the
 simple case mapping to and from plain i.
*/
char *e2e_casefold(const char *name, E2E_CASEMAP map);
/* e2e_casefold(), and *exact (if not NULL) says whether the fold is the
   one map asks for: FALSE when UNICODE fell back to the byte-wise RFC1459
   fold (invalid UTF-8, or GLib refused to normalise) and for a NULL name;
   TRUE otherwise, the exact maps always */
char *e2e_casefold_ex(const char *name, E2E_CASEMAP map, gboolean *exact);

/* "<label>\x1f<wire>"; a bare copy when net is NULL; NULL when net has no
   label (never a bare key in its place), and for a NULL wire or one that
   holds 0x1F */
char *e2e_ctx_scoped(const E2E_NET *net, const char *wire);
/* wire scoped to the same network as like (a bare copy if like is bare or
   NULL); NULL for a NULL wire or one that holds 0x1F */
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
