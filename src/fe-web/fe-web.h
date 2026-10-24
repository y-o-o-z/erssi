#ifndef IRSSI_FE_WEB_FE_WEB_H
#define IRSSI_FE_WEB_FE_WEB_H

#include <irssi/src/common.h>
#include <irssi/src/core/network.h>
#include <irssi/src/irc/core/irc.h>
#include <irssi/src/irc/core/irc-servers.h>
#include <irssi/src/irc/core/irc-chatnets.h>
#include <irssi/src/irc/core/irc-servers-setup.h>
#include <irssi/src/core/servers-setup.h>
#include <irssi/src/core/chatnets.h>

/* Forward declaration for SSL channel */
typedef struct _FE_WEB_SSL_CHANNEL FE_WEB_SSL_CHANNEL;

/* Message types for WebSocket protocol (from PROTOCOL.md) */
typedef enum {
	WEB_MSG_AUTH_OK = 1,
	WEB_MSG_MESSAGE,
	WEB_MSG_SERVER_STATUS,
	WEB_MSG_CHANNEL_JOIN,
	WEB_MSG_CHANNEL_PART,
	WEB_MSG_CHANNEL_KICK,
	WEB_MSG_USER_QUIT,
	WEB_MSG_TOPIC,
	WEB_MSG_CHANNEL_MODE,
	WEB_MSG_NICKLIST,
	WEB_MSG_NICKLIST_UPDATE,
	WEB_MSG_NICK_CHANGE,
	WEB_MSG_USER_MODE,
	WEB_MSG_AWAY,
	WEB_MSG_WHOIS,
	WEB_MSG_CHANNEL_LIST,
	WEB_MSG_STATE_DUMP,
	WEB_MSG_ERROR,
	WEB_MSG_PONG,
	WEB_MSG_QUERY_OPENED,
	WEB_MSG_QUERY_CLOSED,
	WEB_MSG_ACTIVITY_UPDATE,       /* Activity level changed (unread markers) */
	WEB_MSG_MARK_READ,             /* Mark channel as read (from client) */
	WEB_MSG_NETWORK_LIST,          /* Request: list all networks */
	WEB_MSG_NETWORK_LIST_RESPONSE, /* Response: network list */
	WEB_MSG_SERVER_LIST,           /* Request: list all servers */
	WEB_MSG_SERVER_LIST_RESPONSE,  /* Response: server list */
	WEB_MSG_NETWORK_ADD,           /* Request: add/modify network */
	WEB_MSG_NETWORK_REMOVE,        /* Request: remove network */
	WEB_MSG_SERVER_ADD,            /* Request: add/modify server */
	WEB_MSG_SERVER_REMOVE,         /* Request: remove server */
	WEB_MSG_COMMAND_RESULT,        /* Response: operation result */
	WEB_MSG_E2E_STATE              /* E2E state of a channel or query (docs/fe-web-e2e.md) */
} WEB_MESSAGE_TYPE;

/* WebSocket client connection record */
typedef struct {
	int fd;
	char *id;   /* UUID or timestamp-counter */
	/* different for every record made since start: a closed client's
	 * memory may hold the next one */
	guint64 serial;
	char *addr; /* Client IP address (for logging) */
	time_t connected_at;
	/* who the client counts as for login pacing: "uid:1000" or
	 * "ip:192.0.2.7" (fe-web-pacing.h) */
	char *peer_key;
	/* peer_key is an address other users of this machine share */
	unsigned int peer_shared : 1;

	/* WebSocket state */
	unsigned int authenticated : 1;
	unsigned int handshake_done : 1;
	char *websocket_key;

	/* irssi context - per-client server assignment */
	IRC_SERVER_REC *server;  /* Assigned server (or NULL) */
	GSList *synced_channels; /* List of channel names (char *) */
	unsigned int wants_all_servers : 1;

	/* Network */
	NET_SENDBUF_REC *handle;
	/* What the socket did not take yet, in order: output_buffer->data +
	 * output_pos up to its end. fe_web_client_send_raw() is the only
	 * writer to the socket. */
	GByteArray *output_buffer;
	gsize output_pos;
	int send_tag;             /* writable watch while output is queued */
	guint close_tag;          /* deferred or lingering close */
	unsigned int output_want_read : 1; /* TLS has to read before it can write */
	unsigned int output_failed : 1;    /* write error or too much queued: closing */
	unsigned int closing : 1;          /* last frame queued, closing once written */
	GByteArray *input_buffer; /* For incomplete WebSocket frames */
	/* A fragmented message (RFC 6455 5.4) being put together: its
	 * unmasked data so far, NULL when none is open; its opcode (text or
	 * binary) and how many frames it had */
	GByteArray *fragments;
	int fragments_opcode;
	unsigned int fragments_count;
	int recv_tag;
	guint handshake_tag; /* closes the client if it does not log in in time */

	/* SSL/TLS */
	FE_WEB_SSL_CHANNEL *ssl_channel; /* SSL wrapper (if SSL enabled) */
	unsigned int use_ssl : 1;        /* Whether this connection uses SSL */

	/* Encryption */
	unsigned int encryption_enabled : 1; /* Whether this connection uses encryption */

	/* Statistics */
	unsigned long messages_sent;
	unsigned long messages_received;

	/* Request tracking (for WHOIS, ban list, etc.) */
	GHashTable *pending_requests; /* request_id -> response_type mapping */
} WEB_CLIENT_REC;

/* WHOIS data collection structure */
typedef struct {
	char *nick;
	char *user;
	char *host;
	char *realname;
	char *server;
	char *server_info;
	char *idle;
	char *signon;
	char *channels;
	char *account;
	unsigned int secure : 1;
	unsigned int oper : 1;
	time_t timestamp;
	GSList *special; /* List of special/non-standard WHOIS lines */
} WHOIS_REC;

/* Message structure for internal use */
typedef struct {
	char *id; /* Message ID (UUID or timestamp) */
	WEB_MESSAGE_TYPE type;
	char *server_tag;
	char *target; /* Channel or nick */
	char *nick;
	char *text;
	int level; /* MSGLEVEL_* */
	time_t timestamp;
	unsigned int is_own : 1;
	unsigned int is_highlight : 1; /* Message is a highlight (mentions user) */

	/* Additional data (for complex messages like WHOIS, channel_list) */
	GHashTable *extra_data; /* key -> value string pairs */

	/* Response tracking */
	char *response_to; /* Request ID this responds to */
} WEB_MESSAGE_REC;

/* Global clients list */
extern GSList *web_clients;

/* Module initialization */
void fe_web_init(void);
void fe_web_deinit(void);

/* Server functions; fe_web_server_init() returns TRUE when the server
 * listens */
gboolean fe_web_server_init(void);
void fe_web_server_deinit(void);

/* Client functions */
WEB_CLIENT_REC *fe_web_client_create(int fd, const char *addr);
void fe_web_client_destroy(WEB_CLIENT_REC *client);
void fe_web_client_handle_message(WEB_CLIENT_REC *client, const char *json);
void fe_web_client_sync_server(WEB_CLIENT_REC *client, const char *server_tag);
void fe_web_client_execute_command(WEB_CLIENT_REC *client, const char *command, WI_ITEM_REC *item);

/* The guard for what a web client's command must not do while fe-web runs
 * it (/UNLOAD of fe-web): fe_web_init() and fe_web_deinit() call these */
void fe_web_client_commands_init(void);
void fe_web_client_commands_deinit(void);

/* Signal handlers */
void fe_web_signals_init(void);
void fe_web_signals_deinit(void);

/* Nicklist helpers */
void fe_web_send_nicklist_for_channel(IRC_SERVER_REC *server, IRC_CHANNEL_REC *channel);

/* Message creation/destruction */
WEB_MESSAGE_REC *fe_web_message_new(WEB_MESSAGE_TYPE type);
void fe_web_message_free(WEB_MESSAGE_REC *msg);

/* Message sending */
gboolean fe_web_client_send_raw(WEB_CLIENT_REC *client, const void *data, gsize len);
void fe_web_send_message(WEB_CLIENT_REC *client, WEB_MESSAGE_REC *msg);
void fe_web_send_to_server_clients(IRC_SERVER_REC *server, WEB_MESSAGE_REC *msg);
void fe_web_send_to_all_clients(WEB_MESSAGE_REC *msg);

/* JSON utilities */
char *fe_web_message_to_json(WEB_MESSAGE_REC *msg);
char *fe_web_escape_json(const char *str);
char *fe_web_generate_message_id(void);

/* Members of the top-level object of a client message (fe-web-json.c) */
char *fe_web_json_get_string(const char *json, const char *key);
int fe_web_json_get_int(const char *json, const char *key, int default_value);
int fe_web_json_has_key(const char *json, const char *key);

/* JSON building for network/server management. Passwords and the
 * network's autosendcmd go out as FE_WEB_MASK; network_add and server_add
 * keep the stored value when a client sends the mask back. */
#define FE_WEB_MASK "***"

GString *fe_web_build_network_json(IRC_CHATNET_REC *rec);
GString *fe_web_build_server_json(IRC_SERVER_SETUP_REC *rec);
GString *fe_web_build_command_result_json(gboolean success, const char *message,
                                          const char *error_code);

/* Network/Server management handlers */
void fe_web_handle_network_list(WEB_CLIENT_REC *client, const char *json);
void fe_web_handle_server_list(WEB_CLIENT_REC *client, const char *json);
void fe_web_handle_network_add(WEB_CLIENT_REC *client, const char *json);
void fe_web_handle_network_remove(WEB_CLIENT_REC *client, const char *json);
void fe_web_handle_server_add(WEB_CLIENT_REC *client, const char *json);
void fe_web_handle_server_remove(WEB_CLIENT_REC *client, const char *json);

/* State dump */
void fe_web_dump_state(WEB_CLIENT_REC *client);

/* E2E state and key-request actions (fe-web-e2e.c, docs/fe-web-e2e.md).
 * fe_web_signals_init() and fe_web_signals_deinit() call init/deinit. */
void fe_web_e2e_init(void);
void fe_web_e2e_deinit(void);
/* the state of target on server, for this client only: "e2e state
 * request", which the e2e module answers at once (nothing without it) */
void fe_web_e2e_dump(WEB_CLIENT_REC *client, IRC_SERVER_REC *server, const char *target);
/* an e2e_action message: answered with a command_result; the action goes
 * to the e2e module ("e2e action") only when it is valid - with its
 * optional fingerprint, 16 lowercase hex digits */
void fe_web_handle_e2e_action(WEB_CLIENT_REC *client, const char *json);
/* an ident@host an action may name: 3..255 bytes, exactly one '@' and not
 * at either end, no byte <= 0x20, no DEL, ',' or '!' */
gboolean fe_web_e2e_handle_valid(const char *handle);
/* the e2e object of an e2e_state message: one JSON object, valid UTF-8,
 * that starts with {"v": and has nothing after it - anything else is
 * never embedded in a message */
gboolean fe_web_e2e_json_valid(const char *json);

/* WebSocket protocol (RFC 6455) */
char *fe_web_websocket_compute_accept(const char *client_key);
/* the longest upgrade request (the login, headers included), and how
 * much one read of a client takes: the input buffer of a new client
 * holds both without a reallocation, which would leave a copy of the
 * password behind (fe-web-client.c) */
#define FE_WEB_MAX_REQUEST 16384
#define FE_WEB_READ_SIZE 8192

/* Largest WebSocket frame accepted from a client, and the largest
 * message (the frames of a fragmented one together) */
#define FE_WEB_MAX_FRAME (16 * 1024 * 1024)
/* Most frames one fragmented message may have */
#define FE_WEB_MAX_FRAGMENTS 4096

/* The header of the frame at 'data': 1 when all of it is there (then
 * 'header_len' is its length; the payload follows, maybe not yet read),
 * 0 when more is needed, -1 when it breaks the rules for control frames
 * (more than 125 bytes, fragmented) */
int fe_web_websocket_parse_header(const guchar *data, gsize data_len, int *fin, int *opcode,
                                  int *masked, guint64 *payload_len, guchar mask_key[4],
                                  gsize *header_len);
/* The whole frame at 'data': 1 when it is there ('payload' points into
 * 'data'), 0 when more is needed, -1 as for the header and for a payload
 * over FE_WEB_MAX_FRAME */

int fe_web_websocket_parse_frame(const guchar *data, gsize data_len, int *fin, int *opcode,
                                 int *masked, guint64 *payload_len, guchar mask_key[4],
                                 const guchar **payload);
void fe_web_websocket_unmask(guchar *payload, guint64 payload_len, const guchar mask_key[4]);
/* WebSocket opcodes (RFC 6455, section 5.2) */
#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT 0x1
#define WS_OPCODE_BINARY 0x2
#define WS_OPCODE_CLOSE 0x8
#define WS_OPCODE_PING 0x9
#define WS_OPCODE_PONG 0xA

guchar *fe_web_websocket_create_frame(int opcode, const guchar *payload, guint64 payload_len,
                                      gsize *frame_len);

#endif /* IRSSI_FE_WEB_FE_WEB_H */
