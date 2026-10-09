#ifndef IRSSI_E2E_CORE_H
#define IRSSI_E2E_CORE_H

/* what the /e2e commands share with the signal handlers of e2e-core.c */

#include "module.h"
#include "e2e-json.h"
#include "e2e-proto.h"

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
/* drops the PRIVMSGs waiting in the flood queues; how many */
int e2e_drop_queued_privmsgs(void);
gboolean e2e_save(const E2E_JSON *kr);
/* ensure_identity: creates and stores one when the keyring has none */
gboolean e2e_ensure_identity(E2E_JSON *kr, E2E_IDENTITY *id);

/* live ident@host of nick (query, common channel), then the keyring */
char *e2e_resolve_dm_handle(SERVER_REC *server, const E2E_JSON *kr, const char *nick);
/* our own peer-visible ident@host on server, NULL while unknown */
const char *e2e_own_handle(SERVER_REC *server);
/* "@<own>" for a DM context, the channel itself, NULL while unknown */
char *e2e_incoming_ctx_for(SERVER_REC *server, const char *ctx);
void e2e_send_notice(SERVER_REC *server, const char *nick, const char *body);
/* the 30 s limiter of the KEYREQs we send */
GHashTable *e2e_keyreq_out_stamps(void);
/* FALSE (after telling the user once) while rpe2e.pl is loaded */
gboolean e2e_native_active(void);

void e2e_commands_init(void);
void e2e_commands_deinit(void);

#endif
