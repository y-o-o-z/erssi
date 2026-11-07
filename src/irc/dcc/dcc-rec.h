int type;
int orig_type; /* original DCC type that was sent to us - same as type except GET and SEND are swapped */
time_t created;

IRC_SERVER_REC *server;
char *servertag; /* for resetting server later if we get disconnected */
char *mynick; /* my current nick */
char *nick;

CHAT_DCC_REC *chat; /* if the request came through DCC chat */
char *target; /* who the request was sent to - your nick, channel or NULL if you sent the request */
char *arg;

IPADDR addr; /* address we're connected in */
char addrstr[MAX_IP_LEN]; /* in readable form */
int port; /* port we're connected in */

GIOChannel *handle; /* socket handle */
int tagconn, tagread, tagwrite;
time_t starttime; /* transfer start time */
uoff_t transfd; /* bytes transferred */

int pasv_id; /* DCC Id for passive DCCs. <0 means a passive DCC, >=0 means a standard DCC */

unsigned int destroyed:1; /* We're about to destroy this DCC recond */

/* SDCC (DCC SCHAT, DCC SSEND): TLS on the connection */
unsigned int secure:1;
unsigned int tls_done:1; /* the TLS handshake is over */
char *tls_info; /* version and cipher, after the handshake */
char *tls_fingerprint; /* SHA-256 of the other side's certificate or NULL */
void (*tls_ready)(void *dcc); /* what goes on after the handshake */

GHashTable *module_data;
