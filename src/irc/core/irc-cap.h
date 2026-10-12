#ifndef IRSSI_IRC_CORE_IRC_CAP_H
#define IRSSI_IRC_CORE_IRC_CAP_H

/* The most entries cap_supported and cap_active take from a server. The
   IRCv3 registry, drafts and vendor caps included, has well under 100
   names and big networks offer 20-40; 256 leaves room for years and still
   bounds what a hostile server can make us keep (each entry is at most a
   line, so at most about 128 KB per list). */
#define CAP_MAX_ENTRIES 256

void irc_cap_init(void);
void irc_cap_deinit(void);
int irc_cap_toggle (IRC_SERVER_REC *server, char *cap, int enable);
void irc_cap_finish_negotiation (IRC_SERVER_REC *server);

#endif
