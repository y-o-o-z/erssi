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

 Old (unscoped) rows: a keyring of rpe2e.pl, or of the module before the
 scope, holds bare contexts. e2e_ctx_lookup() decides what a wire context
 is on a network. While erssi uses one network (E2E_NET.legacy_ok) old
 rows are LEGACY: e2e_ctx_find() uses them where they are (incoming
 traffic, commands), and only the user's own encrypted line takes them
 over, e2e_ctx_resolve() / e2e_adopt_legacy(), which marks the name as
 that network's (R-E18: a stranger's KEYREQ or wire must not bind it).
 On several networks an old context is BLOCKED - the gate refuses it,
 nothing is decrypted or answered with it - until /e2e adopt binds it to
 one network, or /e2e off writes a scoped config for this one; a name
 another network took over is BLOCKED the same way.

 Everything here is pure: no irssi; the lookups read and change an
 in-memory keyring only. Strings returned as char * are newly allocated
 (g_free).
*/

#include <glib.h>

#include "e2e-json.h"

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
  - UNICODE, pure ASCII: the RFC1459 fold (what the steps below give).
  - UNICODE, valid UTF-8, in this order:
      1. drop the invisible characters (general category Cf, variation
         selectors U+FE00-U+FE0F and U+E0100-U+E01EF, the Mongolian free
         variation selectors U+180B-U+180D and U+180F, the Khmer inherent
         vowels U+17B4 and U+17B5, U+034F, the Hangul fillers U+115F,
         U+1160, U+3164, U+FFA0), as NFKC_Casefold does;
      2. NFD, full case folding, NFKD, full case folding, NFKD: Unicode's
         compatibility caseless match (D146);
      3. drop the run of combining marks (g_unichar_ismark) directly
         before and directly after every U+03B9: U+0345 folds to that iota,
         and servers that case fold before normalising leave the marks on
         the iota where D146 leaves them on the letter;
      4. the Turkish i: U+0131 becomes i, and a U+0307 in the run of
         combining marks directly after an i is dropped;
      5. NFKC, for one representative;
      6. the RFC1459 specials.
    Full case folding is g_utf8_casefold() followed by a fix for GLib's
    Cherokee: a code point whose fold is a single other code point folding
    back to it becomes the smaller of the two (CaseFolding.txt's target).
    The result is valid UTF-8 and holds none of the characters of step 1.
  - UNICODE, invalid UTF-8: the byte-wise RFC1459 fold.
 Bytes above 0x7f are left alone by the exact maps. fold(fold(x)) == fold(x).
 Step 4 covers servers that map case per code point (towlower, Go's
 unicode.ToLower, an upper-case irccmp), which take U+0130 to plain i and
 U+0131 to I: dotted, dotless and plain i fold to one letter (U+1D6A4
 too, through NFKD).

 Never persist the output of e2e_casefold() (it may change with the
 Unicode tables or this fold): persist the original name and fold both
 sides at comparison time.
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

/* ---- the keyring: legacy (unscoped) rows ---- */

typedef enum {
	E2E_CTX_NEW,		/* nothing stored for it on this network yet */
	E2E_CTX_SCOPED,		/* rows stored for this network */
	E2E_CTX_LEGACY,		/* only old unscoped rows, which may be used here */
	E2E_CTX_BLOCKED		/* old rows of unknown network, or no scoped name */
} E2E_CTX_KIND;

/*
 What wire (a bare "#chan" or "@ident@host") is on net. A context EXISTS
 when channels[ctx] or outgoing[ctx] is there, an incoming key is
 "<handle>|<ctx>" or a pending key (a request of ours) is "<ctx>|<handle>"
 or "<ctx>"; the handle of a key is its row's "handle" member, else
 everything up to the first '|' (incoming) or after the last '|'
 (pending), so "#a" is never a row of "#a|b".

   the bare wire exists, net->legacy_ok     LEGACY,  *ctx = wire
   else scoped "<label>\x1f<wire>" exists   SCOPED,  *ctx = scoped
   else the bare wire exists                BLOCKED, *ctx = wire
   else adopted by another network          BLOCKED, *ctx = wire
   else                                     NEW,     *ctx = scoped

 "Adopted by another network": a channels or outgoing row
 "<other label>\x1f<wire>" with the E2E_ADOPTED_MEMBER marker, and no row
 of this network for wire. It also keeps LEGACY from taking newer old
 rows of that name over here. A pending row without a "handle" member
 belongs to the context of its "channel" member.
   wire holds 0x1F, or net has no label     BLOCKED, *ctx = wire
   net NULL (unscoped, as before)           SCOPED if wire exists, else
                                            NEW;     *ctx = wire
   wire NULL                                BLOCKED, *ctx = NULL

 On the only network (legacy_ok) old rows are taken over even next to
 scoped ones, which win row by row (e2e_adopt_legacy): one scoped row of
 a context (a request of ours) must not hide the old sessions of others.
 On several networks a scoped row hides the old ones; they stay.

 A wire holding 0x1F cannot be scoped, and an old row under that name
 reads as another network's (or as one of a network named like its first
 part): BLOCKED whether a row exists or not, so callers never encrypt
 under it. *ctx is g_free'd by the caller.
*/
E2E_CTX_KIND e2e_ctx_lookup(const E2E_JSON *kr, const E2E_NET *net, const char *wire, char **ctx);
/* the storage context of wire on net: e2e_ctx_lookup(), and for LEGACY the
   old rows are taken over (e2e_adopt_legacy(), *changed set: the caller
   saves). NULL for BLOCKED (nothing changed). changed may be NULL. */
char *e2e_ctx_resolve(E2E_JSON *kr, const E2E_NET *net, const char *wire, gboolean *changed);
/* the storage context of wire on net WITHOUT taking anything over: for
   LEGACY the bare name (the old rows used where they are) - the scoped
   name when this network has rows of its own as well -, NULL for
   BLOCKED. For everything that is not the user's own outgoing line:
   incoming messages and handshakes, commands (R-E18). */
char *e2e_ctx_find(const E2E_JSON *kr, const E2E_NET *net, const char *wire);
/*
 Moves every bare row of wire to "<label>\x1f<wire>": channels, outgoing,
 accepted "*|w", incoming "*|w", seen_rekeys "*|w", pending_inbound
 "*|w", pending "w|*" and "w", outgoing_recipients "w|*" (the rows'
 "channel" member too, when it names wire), pending_trust_change[] with
 channel w. A scoped row that already exists wins and the bare one is
 dropped (seen_rekeys: the used nonces of both are kept). The scoped
 channels and outgoing rows of wire get E2E_ADOPTED_MEMBER = label when
 any old row was found. Returns the rows moved; 0 for a label or wire
 that cannot make a scoped name.
*/
guint e2e_adopt_legacy(E2E_JSON *kr, const char *label, const char *wire);

/* the member of a scoped config or key row ("channels", "outgoing") whose
   old rows were taken over: the label. While one exists, the same wire
   name on any other network without rows of its own is BLOCKED, not NEW.
   Writers of those rows keep it. */
#define E2E_ADOPTED_MEMBER "adopted"

#endif
