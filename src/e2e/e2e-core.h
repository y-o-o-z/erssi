#ifndef IRSSI_E2E_CORE_H
#define IRSSI_E2E_CORE_H

/* what the /e2e commands share with the signal handlers of e2e-core.c */

#include "module.h"
#include "e2e-json.h"
#include "e2e-proto.h"
#include "e2e-state.h"

#include <irssi/src/core/servers.h>

/* "[E2E] text" in a window item (NULL: the active window) or in the
   window of target on server */
void e2e_print_item(void *item, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void e2e_print_debug(SERVER_REC *server, const char *ctx, const char *nick,
                     const char *fmt, ...) G_GNUC_PRINTF(4, 5);
/* the window item of a context (a channel) or of a nick (a query) */
void *e2e_notice_item(SERVER_REC *server, const char *ctx, const char *nick);

const char *e2e_keyring_file(void);
/* load_keyring of rpe2e.pl: problems are printed */
E2E_JSON *e2e_load(void);
/* the same; *lost: the keyring is lost (a corrupt one was moved aside),
   the one returned is empty and must not be used for anything */
E2E_JSON *e2e_load_full(gboolean *lost);
/* a corrupt keyring was moved aside and /e2e reset not run yet */
gboolean e2e_keyring_is_lost(void);
/* drops the PRIVMSGs waiting in irssi's flood queues and every line
   of the module's send queues; how many */
int e2e_drop_queued_privmsgs(void);
gboolean e2e_save(const E2E_JSON *kr);
/* ensure_identity: creates and stores one when the keyring has none */
gboolean e2e_ensure_identity(E2E_JSON *kr, E2E_IDENTITY *id);

/* live ident@host of nick (query, common channel), then the keyring */
char *e2e_resolve_dm_handle(SERVER_REC *server, const E2E_JSON *kr, const char *nick);
/* our own peer-visible ident@host on server, NULL while unknown */
const char *e2e_own_handle(SERVER_REC *server);
/* "@<own>" (scoped like ctx) for a DM context, the channel itself, NULL
   while unknown */
char *e2e_incoming_ctx_for(SERVER_REC *server, const char *ctx);
/* the network of server: the label is its chatnet (as configured), else
   its tag; the casemap its CASEMAPPING; legacy_ok e2e_legacy_ok(). The
   caller calls e2e_net_clear(). */
void e2e_server_net(SERVER_REC *server, E2E_NET *net);
/* erssi uses at most one network: the labels of the servers connected,
   connecting and waiting to reconnect, and of the autoconnect servers
   (one without a chatnet by its address, unless it is up) */
gboolean e2e_legacy_ok(void);
void e2e_send_notice(SERVER_REC *server, const char *nick, const char *body);
/* the 30 s limiter of the KEYREQs we send */
GHashTable *e2e_keyreq_out_stamps(void);
/* FALSE (after telling the user once) while rpe2e.pl is loaded; a change
   of that is an e2e_state_changed() */
gboolean e2e_native_active(void);
/* rpe2e.pl is loaded - only looked up: nothing told, nothing remembered */
gboolean e2e_script_loaded(void);

/* the E2E state of the window of item on item's own server (server is
   not used): the keyring read checked, nothing saved, nothing taken
   over, nothing remembered; FALSE (out cleared) for an item that is no
   channel or query, or without a server */
gboolean e2e_window_info_for(SERVER_REC *server, WI_ITEM_REC *item, E2E_WIN_INFO *out);
/* the same from a keyring read already (kr_ok: e2e_keyring_load_checked()
   read it; kr is not changed) */
gboolean e2e_window_info_with(const E2E_JSON *kr, gboolean kr_ok, WI_ITEM_REC *item,
                              E2E_WIN_INFO *out);
/* the keyring or a window's state changed (also: rpe2e.pl loaded or
   unloaded, our own address learned or forgotten): "e2e state changed" is
   emitted once, when the main loop is idle (the statusbar, fe-web) */
void e2e_state_changed(void);
/* "Pending key exchange from <nick> (<handle>) for <network>:<ctx>" in
   the window of ctx (or of nick); have_theirs: their key is here
   already */
void e2e_print_pending(SERVER_REC *server, const char *ctx, const char *nick, const char *handle,
                       gboolean have_theirs);
/* /e2e accept and /e2e decline [<nick>|<ident@host>] in the window of
   item (on item's server; server only for no item, the status window):
   without who (NULL or "") the only request waiting there; a nick that
   names several is refused (they are listed); an ident@host names the
   request of that address - the channel's of a channel window, else its
   query's on this server */
void e2e_do_accept(SERVER_REC *server, WI_ITEM_REC *item, const char *who);
void e2e_do_decline(SERVER_REC *server, WI_ITEM_REC *item, const char *who);

/* e2e_rotate_for_damaged() found entries (pairs "<handle>", "<ctx>"):
   told in item's window (NULL: the active one) */
void e2e_tell_damaged(void *item, const GPtrArray *found);

void e2e_commands_init(void);
void e2e_commands_deinit(void);

#endif
