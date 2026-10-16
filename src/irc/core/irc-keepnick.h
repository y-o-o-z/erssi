#ifndef IRSSI_IRC_CORE_IRC_KEEPNICK_H
#define IRSSI_IRC_CORE_IRC_KEEPNICK_H

#include <irssi/src/irc/core/irc-chatnets.h>
#include <irssi/src/irc/core/irc-servers.h>

/* Keeps `nick' on the network `net' (NULL: stops keeping): the network's
   keepnick option, in the configuration as well. Servers of the network
   look at once. */
void keepnick_set(IRC_CHATNET_REC *net, const char *nick);

/* Keeping is paused on the network after a manual /NICK away from the
   kept nick. keepnick_resume() ends the pause, TRUE if there was one. */
gboolean keepnick_is_paused(const char *chatnet);
gboolean keepnick_resume(const char *chatnet);

/* The nick kept for this connection only (keepnick_auto: the nick the
   server was asked to use was taken at connect), NULL if none. `paused'
   (when not NULL) tells if a manual /NICK paused it. */
const char *keepnick_auto_nick(IRC_SERVER_REC *server, gboolean *paused);
/* Stops keeping that nick; TRUE if there was one */
gboolean keepnick_auto_stop(IRC_SERVER_REC *server);

/* A Perl script named keepnick (keepnick.pl) is loaded: the built-in one
   does nothing, its commands are the script's. */
gboolean keepnick_script_loaded(void);

/* TRUE while the reply to keepnick's own NICK is processed: the nick
   change is keepnick's, not the user's */
gboolean keepnick_regaining(SERVER_REC *server);

void irc_keepnick_init(void);
void irc_keepnick_deinit(void);

#endif
