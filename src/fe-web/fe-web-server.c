/* Enable GNU extensions (strcasestr) */
#define _GNU_SOURCE

/*
 fe-web-server.c : TCP / Unix socket WebSocket server for fe-web

    Copyright (C) 2025

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "module.h"
#include "fe-web.h"
#include "fe-web-ssl.h"
#include "fe-web-crypto.h"
#include "fe-web-pacing.h"

#include <irssi/src/core/network.h>
#include <irssi/src/core/net-sendbuffer.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/fe-common/core/printtext.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

static GIOChannel *listen_channel = NULL;
static int listen_port = -1;
static int listen_tag = -1;

/* Unix socket mode (fe_web_socket): the path listened on, and the device
 * and inode of the socket file made there - only that file is removed when
 * the server stops, never one something else has put in its place. */
static char *listen_path = NULL;
static dev_t listen_dev;
static ino_t listen_ino;
/* flock() on "<socket>.lock", held for as long as the socket listens, so
 * that another erssi can tell a busy erssi's socket from a stale one
 * (socket_lock()); -1 when not held */
static int listen_lock_fd = -1;

/* Linux reports a listener whose queue is full with EAGAIN; the BSDs and
 * macOS report ECONNREFUSED, as for no listener */
#ifdef __linux__
static gboolean econnrefused_means_no_listener = TRUE;
#else
static gboolean econnrefused_means_no_listener = FALSE;
#endif

/* Limits for clients that have not logged in yet: one password guess per
 * connection, a few KB of request and a few seconds to send it. */
#define FE_WEB_MAX_CLIENTS 16
#define FE_WEB_MAX_REQUEST 16384
#define FE_WEB_HANDSHAKE_TIMEOUT 10
static guint relisten_tag = 0;
static time_t refused_logged = 0;
/* After FE_WEB_MAX_FAILS wrong passwords within a minute from one peer,
 * logins from that peer are paced: one password is checked every
 * FE_WEB_CHECK_INTERVAL seconds and requests in between are refused
 * unchecked (429), until a minute passes without a wrong password
 * (fe-web-pacing.h). Not a full lockout, and per peer: fe-web usually
 * listens on a loopback port every user of a shared box can reach, and
 * none of them may keep the owner's web client out. The owner's own
 * processes (own_peer_key) are never paced: they can read the password in
 * the config anyway. Both are made on the first login.
 *
 * A proxy that runs as the owner's user (a reverse proxy, socat, an ssh -L
 * tunnel others can use) connects as the owner, so every client behind it
 * counts as the owner and is never paced: such a proxy must sign its own
 * users in, as a web client that keeps the fe-web password on its side
 * and has its own sign-in does. */
static FE_WEB_PACING *login_pacing = NULL;
static char *own_peer_key = NULL;
/* the pacing notice is printed once a minute at most, the peers paced in
 * between are counted */
static time_t paced_logged = 0;
static int paced_unlogged = 0;
/* /proc/net/tcp lookups of loopback peers (fe_web_tcp_peer_uid()) */
static FE_WEB_PROC_BUDGET proc_budget;

/* Connections still logging in have their own limit (FE_WEB_MAX_PENDING,
 * fe-web-pacing.h), so they can never take the places of logged-in
 * clients; when it is reached one of them makes room
 * (fe_web_pending_victim(): idle connections cannot keep new logins out). */
static int refused_unlogged = 0;

/* Output a client does not take yet is queued up to this much; a client
 * that stops reading for longer is closed instead of eating memory. */
#ifndef FE_WEB_MAX_OUTPUT
#define FE_WEB_MAX_OUTPUT (32 * 1024 * 1024)
#endif
/* How long a client being closed gets to take its last frame (a close
 * frame, a 401 or 429) */
#define FE_WEB_LINGER_TIMEOUT 5

/* Forward declarations */
static void sig_listen(void);
static void client_input(WEB_CLIENT_REC *client);
static void log_refused(WEB_CLIENT_REC *client, const char *reason);
static void log_refused_addr(const char *addr, const char *reason);

/* Close client connection */
static void fe_web_close_client(WEB_CLIENT_REC *client)
{
	if (client == NULL) {
		return;
	}

	if (client->handshake_tag != 0) {
		g_source_remove(client->handshake_tag);
		client->handshake_tag = 0;
	}

	if (client->close_tag != 0) {
		g_source_remove(client->close_tag);
		client->close_tag = 0;
	}

	/* Remove input and output handlers */
	if (client->recv_tag != -1) {
		g_source_remove(client->recv_tag);
		client->recv_tag = -1;
	}
	if (client->send_tag != -1) {
		g_source_remove(client->send_tag);
		client->send_tag = -1;
	}

	/* Free SSL channel if exists. With output still queued a TLS write
	 * may be half done: no close_notify then. */
	if (client->ssl_channel != NULL) {
		if (client->output_buffer->len > client->output_pos)
			client->ssl_channel->failed = 1;
		fe_web_ssl_channel_free(client->ssl_channel);
		client->ssl_channel = NULL;
	}

	/* Close socket */
	if (client->handle != NULL) {
		net_sendbuffer_destroy(client->handle, TRUE);
		client->handle = NULL;
	}

	/* Destroy client record */
	fe_web_client_destroy(client);
}

/* Output: every byte for a client goes through fe_web_client_send_raw().
 * The socket is non-blocking and a client may read slowly (a phone on
 * mobile data, a large state dump), so what the socket does not take is
 * queued in order and written when the socket is writable again. */

static gsize output_pending(WEB_CLIENT_REC *client)
{
	return client->output_buffer->len - client->output_pos;
}

/* One write to the socket: bytes written, 0 when the socket is full,
 * FE_WEB_SSL_WANT_READ when TLS has to read first, -1 on error. */
static int output_write(WEB_CLIENT_REC *client, const guchar *data, gsize len)
{
	int n = len > G_MAXINT ? G_MAXINT : (int) len;
	int ret;

	if (client->use_ssl) {
		if (client->ssl_channel == NULL)
			return -1;
		ret = fe_web_ssl_write(client->ssl_channel, (const char *) data, n);
		return ret == FE_WEB_SSL_WANT_WRITE ? 0 : ret;
	}
	if (client->handle == NULL)
		return -1;
	return net_transmit(net_sendbuffer_handle(client->handle), (const char *) data, n);
}

static void output_ready(WEB_CLIENT_REC *client);

/* A writable watch exactly while there is output the socket can take -
 * not while TLS waits for a read: the socket is writable then and the
 * watch would spin; client_input() retries after reading. */
static void output_watch_update(WEB_CLIENT_REC *client)
{
	gboolean want = output_pending(client) > 0 && !client->output_want_read &&
	                !client->output_failed && client->handle != NULL;

	if (want && client->send_tag == -1) {
		client->send_tag = i_input_add(net_sendbuffer_handle(client->handle),
		                               I_INPUT_WRITE, (GInputFunction) output_ready,
		                               client);
	} else if (!want && client->send_tag != -1) {
		g_source_remove(client->send_tag);
		client->send_tag = -1;
	}
}

/* Write what is queued, as far as the socket takes it. FALSE on a write
 * error. */
static gboolean output_flush(WEB_CLIENT_REC *client)
{
	GByteArray *buf = client->output_buffer;
	int ret;

	client->output_want_read = FALSE;
	while (output_pending(client) > 0) {
		ret = output_write(client, buf->data + client->output_pos, output_pending(client));
		if (ret > 0) {
			client->output_pos += ret;
		} else if (ret == 0) {
			break;
		} else if (ret == FE_WEB_SSL_WANT_READ) {
			client->output_want_read = TRUE;
			break;
		} else {
			return FALSE;
		}
	}

	if (output_pending(client) == 0) {
		g_byte_array_set_size(buf, 0);
		client->output_pos = 0;
	} else if (client->output_pos >= 65536 && client->output_pos >= buf->len / 2) {
		/* the written part is dropped once it is most of the buffer, not
		 * after every write. The unwritten bytes keep their contents,
		 * which is all a TLS retry needs. */
		g_byte_array_remove_range(buf, 0, client->output_pos);
		client->output_pos = 0;
	}
	output_watch_update(client);
	return TRUE;
}

static gboolean close_later(gpointer data)
{
	WEB_CLIENT_REC *client = data;

	client->close_tag = 0;
	fe_web_close_client(client);
	return FALSE;
}

/* Output to this client failed or overflowed: nothing more is sent to it
 * and it is closed from the main loop - not here, where the caller may be
 * walking web_clients or still using the client. */
static void output_fail(WEB_CLIENT_REC *client)
{
	if (client->ssl_channel != NULL && output_pending(client) > 0)
		client->ssl_channel->failed = 1; /* no close_notify after a cut record */
	client->output_failed = TRUE;
	g_byte_array_set_size(client->output_buffer, 0);
	client->output_pos = 0;
	output_watch_update(client);
	if (client->close_tag != 0)
		g_source_remove(client->close_tag);
	/* a timeout, not an idle: it has the priority of the input watches,
	 * so a busy socket cannot keep it from running */
	client->close_tag = g_timeout_add(0, close_later, client);
}

/* Writable watch */
static void output_ready(WEB_CLIENT_REC *client)
{
	if (!output_flush(client)) {
		fe_web_close_client(client);
		return;
	}
	if (client->closing && output_pending(client) == 0)
		fe_web_close_client(client);
}

/* Send bytes to a client, in order after everything sent before. What the
 * socket does not take now is queued. Never closes the client itself:
 * FALSE when it can no longer be sent to (it is closed shortly). */
gboolean fe_web_client_send_raw(WEB_CLIENT_REC *client, const void *data, gsize len)
{
	const guchar *p = data;
	gboolean queued_before;
	int ret;

	if (client == NULL || client->output_failed || client->closing)
		return FALSE;
	if (len == 0)
		return TRUE;

	queued_before = output_pending(client) > 0;
	if (!queued_before && !client->output_want_read) {
		/* nothing queued: straight to the socket */
		while (len > 0) {
			ret = output_write(client, p, len);
			if (ret > 0) {
				p += ret;
				len -= ret;
			} else if (ret == 0) {
				break;
			} else if (ret == FE_WEB_SSL_WANT_READ) {
				client->output_want_read = TRUE;
				break;
			} else {
				output_fail(client);
				return FALSE;
			}
		}
		if (len == 0)
			return TRUE;
	}

	if (output_pending(client) + len > FE_WEB_MAX_OUTPUT) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: [%s] %s does not read what is sent to it "
		          "(over %d MB queued) - disconnected",
		          client->id, client->addr, FE_WEB_MAX_OUTPUT >> 20);
		output_fail(client);
		return FALSE;
	}
	g_byte_array_append(client->output_buffer, p, len);

	/* with output queued before, the socket may have room again by now */
	if (queued_before && !client->output_want_read && !output_flush(client)) {
		output_fail(client);
		return FALSE;
	}
	output_watch_update(client);
	return TRUE;
}

/* Close the client once its queued output (a last frame, an HTTP error)
 * is written, or after FE_WEB_LINGER_TIMEOUT. Nothing is read from it or
 * sent to it any more. */
static void fe_web_close_client_flushed(WEB_CLIENT_REC *client)
{
	if (output_pending(client) == 0 || client->output_failed || client->handle == NULL) {
		fe_web_close_client(client);
		return;
	}

	client->closing = TRUE;
	if (client->recv_tag != -1) {
		g_source_remove(client->recv_tag);
		client->recv_tag = -1;
	}
	if (client->handshake_tag != 0) {
		g_source_remove(client->handshake_tag);
		client->handshake_tag = 0;
	}
	if (client->close_tag != 0)
		g_source_remove(client->close_tag);
	client->close_tag = g_timeout_add_seconds(FE_WEB_LINGER_TIMEOUT, close_later, client);
	output_watch_update(client);
}

/* Verify password from handshake request
 * Password MUST be provided in query parameter: GET /?password=secret HTTP/1.1
 */
static int fe_web_verify_password(const char *data)
{
	const char *configured_password;
	char *password_param;
	char *password_start;
	char *password_end;
	char *password = NULL;
	gboolean from_header = FALSE;
	int result = 0;

	configured_password = settings_get_str("fe_web_password");

	/* Password is REQUIRED - reject if not configured */
	if (configured_password == NULL || *configured_password == '\0') {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: REJECTED - No password configured! Use /SET fe_web_password <password>");
		return 0;
	}

	/* "Authorization: Bearer <password>" header - the password stays out of
	 * the request line (and of anything that logs URLs) */
	{
		const char *headers = strstr(data, "\r\n");
		const char *h = headers;

		while (h != NULL && password == NULL) {
			h += 2;
			if (strncmp(h, "\r\n", 2) == 0)
				break;  /* end of headers */
			if (g_ascii_strncasecmp(h, "Authorization:", 14) == 0) {
				const char *v = h + 14;
				const char *end = strstr(v, "\r\n");

				while (*v == ' ' || *v == '\t')
					v++;
				if (g_ascii_strncasecmp(v, "Bearer ", 7) == 0 && end != NULL) {
					/* RFC 9110 5.5: whitespace at the end of a field
					 * value is not part of it (a proxy may add some) */
					while (end > v + 7 && (end[-1] == ' ' || end[-1] == '\t'))
						end--;
					if (end > v + 7) {
						password = g_strndup(v + 7, end - (v + 7));
						from_header = TRUE;
					}
				}
			}
			h = strstr(h, "\r\n");
		}
	}

	/* Otherwise the query parameter (?password=...), as older web clients send */
	password_param = password != NULL ? NULL : strstr(data, "?password=");
	if (password_param != NULL) {
		password_start = password_param + strlen("?password=");
		password_end = strpbrk(password_start, " &\r\n");
		if (password_end != NULL) {
			password = g_strndup(password_start, password_end - password_start);
		} else {
			password = g_strdup(password_start);
		}
	}

	/* Verify password: compare digests, in time independent of where
	 * the first difference is */
	if (password != NULL) {
		char *want, *got;
		unsigned char diff = 0;
		gsize want_len = strlen(configured_password);
		int i;

		/* a header cannot carry whitespace at the end of the password
		 * (see above): such a password is compared without it */
		while (from_header && want_len > 0 &&
		       (configured_password[want_len - 1] == ' ' ||
		        configured_password[want_len - 1] == '\t'))
			want_len--;
		want = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
		                                   (const guchar *) configured_password, want_len);
		got = g_compute_checksum_for_string(G_CHECKSUM_SHA256, password, -1);
		for (i = 0; want[i] != '\0'; i++)
			diff |= want[i] ^ got[i];
		result = diff == 0;
		memset(password, 0, strlen(password));
		g_free(password);
		g_free(want);
		g_free(got);
	}

	return result;
}

/* Notice that logins from peer are paced now; 'overflow': from every peer
 * of the overflow bucket; 'shared': the peer is an address other users of
 * this machine have too */
static void log_paced(const char *peer, gboolean overflow, gboolean shared)
{
	time_t now = time(NULL);

	/* a clock that went back counts as a minute passed */
	if (now >= paced_logged && now - paced_logged < 60) {
		paced_unlogged++;
		return;
	}
	if (paced_unlogged > 0)
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: logins from %d more %s paced since the last notice",
		          paced_unlogged, paced_unlogged == 1 ? "peer" : "peers");
	if (overflow)
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: %d wrong passwords within a minute from new peers, last %s "
		          "(more than %d peers, counted together) - checking one login from "
		          "all of them every %d s",
		          FE_WEB_MAX_FAILS, peer, FE_WEB_PACING_MAX_PEERS, FE_WEB_CHECK_INTERVAL);
	else
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: %d wrong passwords within a minute from %s - "
		          "checking one login from there every %d s",
		          FE_WEB_MAX_FAILS, peer, FE_WEB_CHECK_INTERVAL);
	if (shared)
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: other users of this machine can reach this port - "
		          "/SET fe_web_socket ~/.erssi/fe-web.sock lets only your own "
		          "processes in");
	paced_logged = now;
	paced_unlogged = 0;
}

/* Who the client counts as for login pacing */
static const char *client_peer(WEB_CLIENT_REC *client)
{
	return client->peer_key != NULL ? client->peer_key : "unknown";
}

/* Handle WebSocket handshake (RFC 6455) */
static int fe_web_handle_handshake(WEB_CLIENT_REC *client, const char *data)
{
	char *key_line;
	char *key_start;
	char *key_end;
	char *accept_key;
	GString *response;
	time_t now;
	const char *peer;
	gboolean paced;

	/* Look for Sec-WebSocket-Key header */
	key_line = strstr(data, "Sec-WebSocket-Key:");
	if (key_line == NULL) {
		/* Try case-insensitive search */
		key_line = strcasestr(data, "Sec-WebSocket-Key:");
		if (key_line == NULL) {
			return 0; /* Not complete handshake yet */
		}
	}

	/* Check for end of headers */
	if (strstr(data, "\r\n\r\n") == NULL) {
		return 0; /* Headers not complete */
	}

	/* Paced after too many wrong passwords from this peer: one check per
	 * interval. A client without a peer key counts as one peer with all
	 * the others that have none. */
	now = time(NULL);
	if (login_pacing == NULL)
		login_pacing = fe_web_pacing_new();
	if (own_peer_key == NULL)
		own_peer_key = fe_web_peer_key_uid((unsigned long) getuid());
	peer = client_peer(client);
	/* the owner's uid is exempt - and so is whatever connects through a
	 * proxy running as the owner (see login_pacing) */
	paced = strcmp(peer, own_peer_key) != 0;
	if (paced && !fe_web_pacing_may_check(login_pacing, peer, now)) {
		static const char busy[] = "HTTP/1.1 429 Too Many Requests\r\n"
		                           "Retry-After: " G_STRINGIFY(FE_WEB_CHECK_INTERVAL) "\r\n"
		                           "Content-Length: 0\r\n\r\n";
		fe_web_client_send_raw(client, busy, sizeof(busy) - 1);
		log_refused(client, "too many wrong passwords, logins paced");
		return -1;
	}

	/* Verify password */
	if (!fe_web_verify_password(data)) {
		/* the owner's wrong passwords count nowhere: not in the
		 * table, not towards the ceiling */
		if (paced && fe_web_pacing_failed(login_pacing, peer, now))
			log_paced(peer, fe_web_pacing_in_overflow(login_pacing, peer),
			          client->peer_shared);
		log_refused(client, "wrong or missing password");

		/* 401 Unauthorized; the caller closes once it is written */
		response = g_string_new("");
		g_string_append(response, "HTTP/1.1 401 Unauthorized\r\n");
		g_string_append(response, "Content-Type: text/plain\r\n");
		g_string_append(response, "Content-Length: 13\r\n");
		g_string_append(response, "\r\n");
		g_string_append(response, "Unauthorized\n");
		fe_web_client_send_raw(client, response->str, response->len);
		g_string_free(response, TRUE);
		return -1; /* Authentication failed */
	}

	/* Extract key value */
	key_start = key_line + strlen("Sec-WebSocket-Key:");
	while (*key_start == ' ' || *key_start == '\t') {
		key_start++;
	}

	key_end = strchr(key_start, '\r');
	if (key_end == NULL) {
		key_end = strchr(key_start, '\n');
	}

	if (key_end == NULL) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: [%s] Invalid WebSocket key format",
		          client->id);
		return 0;
	}

	/* Store key */
	if (client->websocket_key != NULL) {
		g_free(client->websocket_key);
	}
	client->websocket_key = g_strndup(key_start, key_end - key_start);

	/* Compute accept key */
	accept_key = fe_web_websocket_compute_accept(client->websocket_key);

	/* Build handshake response */
	response = g_string_new("");
	g_string_append(response, "HTTP/1.1 101 Switching Protocols\r\n");
	g_string_append(response, "Upgrade: websocket\r\n");
	g_string_append(response, "Connection: Upgrade\r\n");
	g_string_append_printf(response, "Sec-WebSocket-Accept: %s\r\n", accept_key);
	g_string_append(response, "\r\n");

	/* Send response (queued if the socket is full) */
	if (!fe_web_client_send_raw(client, response->str, response->len)) {
		g_free(accept_key);
		g_string_free(response, TRUE);
		return -1;
	}

	g_free(accept_key);
	g_string_free(response, TRUE);

	client->handshake_done = TRUE;
	return 1;
}

/* Handle WebSocket data */
static void fe_web_handle_websocket_data(WEB_CLIENT_REC *client)
{
	int fin;
	int opcode;
	int masked;
	guint64 payload_len;
	guchar mask_key[4];
	const guchar *payload;
	int ret;
	guchar *unmasked_payload;
	gsize frame_total_len;

	/* output_failed: the client is closed shortly, nothing it asks for
	 * can be answered */
	while (client->input_buffer->len > 0 && !client->output_failed) {
		/* RFC 6455 5.1: a client masks every frame, and a server closes
		 * the connection on one that is not masked. Refused on its
		 * header (the mask bit of byte 2), whatever the opcode, before
		 * its payload is waited for or anything of it is handled; the
		 * frames after it in the buffer are not handled either.
		 * 1002 = protocol error. */
		if (client->input_buffer->len >= 2 &&
		    (client->input_buffer->data[1] & 0x80) == 0) {
			static const guchar status[2] = { 0x03, 0xEA };
			guchar *close_frame;
			gsize close_len;

			close_frame = fe_web_websocket_create_frame(WS_OPCODE_CLOSE, status, 2,
			                                            &close_len);
			fe_web_client_send_raw(client, close_frame, close_len);
			g_free(close_frame);
			fe_web_close_client_flushed(client);
			return;
		}

		/* Try to parse frame */
		ret = fe_web_websocket_parse_frame(client->input_buffer->data,
		                                    client->input_buffer->len,
		                                    &fin, &opcode, &masked,
		                                    &payload_len, mask_key, &payload);

		if (ret == 0) {
			/* Incomplete frame - wait for more data */
			break;
		}

		if (ret < 0) {
			/* Invalid frame - close connection */
			fe_web_close_client(client);
			return;
		}

		/* Calculate total frame length */
		frame_total_len = (payload - client->input_buffer->data) + payload_len;

		/* Handle different opcodes */
		if (opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY) { /* binary = encrypted */
			/* every frame here is masked */
			unmasked_payload = g_malloc(payload_len + 1);
			memcpy(unmasked_payload, payload, payload_len);
			fe_web_websocket_unmask(unmasked_payload, payload_len, mask_key);
			unmasked_payload[payload_len] = '\0';

			/* Binary frame = encrypted data */
			if (opcode == WS_OPCODE_BINARY) {
				unsigned char *decrypted;
				int decrypted_len;
				const unsigned char *key;

				if (!client->encryption_enabled) {
					printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
					          "fe-web: [%s] Received encrypted data but encryption not enabled", client->id);
					g_free(unmasked_payload);
					fe_web_close_client(client);
					return;
				}

				key = fe_web_crypto_get_key();
				if (key == NULL) {
					printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
					          "fe-web: [%s] Encryption key not available", client->id);
					g_free(unmasked_payload);
					fe_web_close_client(client);
					return;
				}

				/* Allocate buffer for decrypted data */
				decrypted = g_malloc(payload_len);

				/* Decrypt */
				if (!fe_web_crypto_decrypt(unmasked_payload, payload_len, key, decrypted, &decrypted_len)) {
					printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
					          "fe-web: [%s] Decryption failed - wrong password or tampered data", client->id);
					g_free(unmasked_payload);
					g_free(decrypted);
					fe_web_close_client(client);
					return;
				}

				/* Null-terminate decrypted JSON */
				g_free(unmasked_payload);
				unmasked_payload = g_malloc(decrypted_len + 1);
				memcpy(unmasked_payload, decrypted, decrypted_len);
				unmasked_payload[decrypted_len] = '\0';
				g_free(decrypted);
			}

			/* Handle JSON message */
			fe_web_client_handle_message(client, (const char *)unmasked_payload);
			g_free(unmasked_payload);
		} else if (opcode == WS_OPCODE_CLOSE) {
			/* RFC 6455 5.5.1: answer with a Close frame (echoing the
			 * status code), then close - the client then sees an orderly
			 * close instead of waiting for its timeout. A client's close
			 * payload is masked; only the 2-byte status code is echoed. */
			guchar status[2] = { 0x03, 0xE8 }; /* 1000 normal closure */
			guchar *close_frame;
			gsize close_len;

			if (payload_len >= 2) {
				status[0] = payload[0] ^ mask_key[0];
				status[1] = payload[1] ^ mask_key[1];
			}
			close_frame = fe_web_websocket_create_frame(WS_OPCODE_CLOSE, status, 2, &close_len);
			fe_web_client_send_raw(client, close_frame, close_len);
			g_free(close_frame);

			/* after what is queued before it, the close frame too */
			fe_web_close_client_flushed(client);
			return;
		} else if (opcode == WS_OPCODE_PING) {
			/* Send pong, in order behind what is queued */
			guchar *pong_frame;
			guchar *pong_data;
			gsize pong_len;

			/* the pong carries the ping's data, unmasked */
			pong_data = g_malloc(payload_len + 1);
			memcpy(pong_data, payload, payload_len);
			fe_web_websocket_unmask(pong_data, payload_len, mask_key);
			pong_frame = fe_web_websocket_create_frame(WS_OPCODE_PONG, pong_data, payload_len, &pong_len);
			g_free(pong_data);

			fe_web_client_send_raw(client, pong_frame, pong_len);
			g_free(pong_frame);
		}
		/* WS_OPCODE_PONG - nothing to do */

		/* Remove processed frame from buffer */
		g_byte_array_remove_range(client->input_buffer, 0, frame_total_len);
	}
}

/* Read data from client */
static void client_input_once(WEB_CLIENT_REC *client);

/* Read until nothing is left: OpenSSL may hold already-decrypted data (a
 * record larger than one read, several frames in one record) that never
 * makes the socket readable again, so stopping after one read would leave
 * those frames waiting for the next packet */
static void client_input(WEB_CLIENT_REC *client)
{
	int rounds = 0;

	do {
		client_input_once(client);
	} while (g_slist_find(web_clients, client) != NULL && !client->closing &&
	         !client->output_failed && client->use_ssl &&
	         client->ssl_channel != NULL && client->ssl_channel->ssl != NULL &&
	         client->ssl_channel->handshake_done &&
	         SSL_pending(client->ssl_channel->ssl) > 0 && ++rounds < 1024);

	if (g_slist_find(web_clients, client) == NULL)
		return;
	if (client->output_failed) {
		/* top level here: no need to wait for close_later() */
		fe_web_close_client(client);
	} else if (client->output_want_read && !client->closing) {
		/* TLS has read what it waited for (renegotiation, key update):
		 * the queued output can go on */
		if (!output_flush(client))
			fe_web_close_client(client);
	}
}

static void client_input_once(WEB_CLIENT_REC *client)
{
	guchar buffer[8192];
	int ret;
	GIOChannel *channel;

	if (client == NULL || client->handle == NULL || client->closing) {
		return;
	}
	if (client->output_failed) {
		fe_web_close_client(client);
		return;
	}

	/* SSL handshake if needed */
	if (client->use_ssl && client->ssl_channel != NULL && !client->ssl_channel->handshake_done) {
		ret = fe_web_ssl_accept(client->ssl_channel);

		if (ret == 0) {
			/* Need more data */
			return;
		} else if (ret < 0) {
			/* Handshake failed: not TLS, a port scan, an old client */
			log_refused(client, "TLS handshake failed");
			fe_web_close_client(client);
			return;
		}

		/* Handshake complete */
	}

	/* Read from socket (SSL or plain) */
	if (client->use_ssl && client->ssl_channel != NULL) {
		ret = fe_web_ssl_read(client->ssl_channel, (char *)buffer, sizeof(buffer));

		if (ret == -2) {
			/* SSL wants read - wait for more data */
			return;
		}
	} else {
		channel = net_sendbuffer_handle(client->handle);
		if (channel == NULL) {
			return;
		}
		ret = net_receive(channel, (char *)buffer, sizeof(buffer));
	}

	if (ret <= 0) {
		/* Connection closed or error */
		if (ret < 0) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
			          "fe-web: [%s] Connection error (ret=%d, errno=%d: %s)",
			          client->id, ret, errno, strerror(errno));
		}
		fe_web_close_client(client);
		return;
	}

	/* Append to input buffer */
	g_byte_array_append(client->input_buffer, buffer, ret);

	/* Handle handshake first */
	if (!client->handshake_done) {
		/* Null-terminate for string operations */
		g_byte_array_append(client->input_buffer, (guchar *)"\0", 1);

		ret = fe_web_handle_handshake(client, (const char *)client->input_buffer->data);
		if (ret < 0 || (ret == 0 && client->input_buffer->len > FE_WEB_MAX_REQUEST)) {
			/* wrong password (after the 401 or 429 is written), failed
			 * write or an endless request */
			fe_web_close_client_flushed(client);
			return;
		}
		if (ret > 0) {
			/* Handshake complete - send auth_ok */
			WEB_MESSAGE_REC *msg;

			if (client->handshake_tag != 0) {
				g_source_remove(client->handshake_tag);
				client->handshake_tag = 0;
			}

			msg = fe_web_message_new(WEB_MSG_AUTH_OK);
			msg->id = fe_web_generate_message_id();
			fe_web_send_message(client, msg);
			fe_web_message_free(msg);

			client->authenticated = TRUE;
			/* the owner is never paced: it needs no place among
			 * the trusted peers */
			if (login_pacing != NULL && own_peer_key != NULL &&
			    strcmp(client_peer(client), own_peer_key) != 0)
				fe_web_pacing_succeeded(login_pacing, client_peer(client));

			/* Clear input buffer */
			g_byte_array_set_size(client->input_buffer, 0);
		} else {
			/* Remove null terminator */
			g_byte_array_set_size(client->input_buffer, client->input_buffer->len - 1);
		}
		return;
	}

	/* Handle WebSocket frames */
	fe_web_handle_websocket_data(client);
}

/* Refused connections are reported once a minute at most, so a client
 * guessing passwords or a port scan cannot flood the windows. */
static void log_refused(WEB_CLIENT_REC *client, const char *reason)
{
	log_refused_addr(client->addr, reason);
}

static void log_refused_addr(const char *addr, const char *reason)
{
	time_t now = time(NULL);

	/* a clock that went back counts as a minute passed */
	if (now >= refused_logged && now - refused_logged < 60) {
		refused_unlogged++;
		return;
	}
	if (refused_unlogged > 0)
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: %d more connections refused", refused_unlogged);
	printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
	          "fe-web: connection from %s refused: %s", addr, reason);
	refused_logged = now;
	refused_unlogged = 0;
}

static gboolean handshake_timeout(gpointer data)
{
	WEB_CLIENT_REC *client = data;

	client->handshake_tag = 0;
	if (!client->handshake_done)
		fe_web_close_client(client);
	return FALSE;
}

static gboolean relisten(gpointer data)
{
	relisten_tag = 0;
	if (listen_channel != NULL && listen_tag == -1)
		listen_tag = i_input_add(listen_channel, I_INPUT_READ,
		                         (GInputFunction) sig_listen, NULL);
	return FALSE;
}

/* The uid of the process at the other end of a Unix socket connection.
 * Returns 1 when known, 0 when it could not be read and -1 when this system
 * has no way to tell (then the socket file's 0600 mode is the only guard). */
static int socket_peer_uid(int fd, uid_t *uid)
{
#if defined(__linux__) && defined(SO_PEERCRED)
	struct ucred cred;
	socklen_t len = sizeof(cred);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
		return 0;
	if (len != sizeof(cred)) {
		errno = EPROTO;
		return 0;
	}
	*uid = cred.uid;
	return 1;
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__APPLE__)
	gid_t gid;

	return getpeereid(fd, uid, &gid) == 0 ? 1 : 0;
#else
	(void) fd;
	(void) uid;
	return -1;
#endif
}

/* Accept a connection on the Unix socket */
static GIOChannel *socket_accept(GIOChannel *channel)
{
	struct sockaddr_un sa;
	socklen_t len = sizeof(sa);
	int fd;

	fd = accept(g_io_channel_unix_get_fd(channel), (struct sockaddr *) &sa, &len);
	if (fd < 0)
		return NULL;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	/* /exec children must not inherit web clients' connections */
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return i_io_channel_new(fd);
}

/* Accept new connection */
static void sig_listen(void)
{
	IPADDR ip;
	int port;
	GIOChannel *handle;
	char host[MAX_IP_LEN];
	char *addr, *peer_key;
	gboolean peer_shared = FALSE;
	WEB_CLIENT_REC *client;
	NET_SENDBUF_REC *sendbuf;

	/* Accept connection */
	if (listen_path != NULL)
		handle = socket_accept(listen_channel);
	else
		handle = net_accept(listen_channel, &ip, &port);
	if (handle != NULL) {
		/* not inherited by /exec children */
		fcntl(g_io_channel_unix_get_fd(handle), F_SETFD, FD_CLOEXEC);
	}
	if (handle == NULL) {
		/* Out of file descriptors: the connection stays queued and the
		 * socket stays readable - pause instead of spinning. */
		if ((errno == EMFILE || errno == ENFILE) && listen_tag != -1) {
			g_source_remove(listen_tag);
			listen_tag = -1;
			relisten_tag = g_timeout_add_seconds(1, relisten, NULL);
		}
		return;
	}

	if (listen_path != NULL) {
		uid_t uid = 0;
		int known;

		addr = g_strdup_printf("unix:%s", listen_path);
		/* only processes of the user running erssi */
		known = socket_peer_uid(g_io_channel_unix_get_fd(handle), &uid);
		if (known == 0 || (known > 0 && uid != getuid())) {
			char *reason = known == 0 ?
			    g_strdup_printf("cannot read peer credentials: %s", g_strerror(errno)) :
			    g_strdup_printf("process of uid %lu, not ours", (unsigned long) uid);

			log_refused_addr(addr, reason);
			g_free(reason);
			g_free(addr);
			net_disconnect(handle);
			return;
		}
		/* no way to tell on this system: the socket file's mode lets
		 * only our own processes in, but they are paced all the same.
		 * "uid:unknown" can never be own_peer_key, which is "uid:"
		 * and digits (fe_web_peer_key_uid()), so it is never exempt;
		 * Linux and the BSDs always know the uid, so no test reaches
		 * this. */
		peer_key = known > 0 ? fe_web_peer_key_uid((unsigned long) uid) :
		                       g_strdup("uid:unknown");
	} else {
		uid_t uid = 0;

		net_ip2host(&ip, host);
		addr = g_strdup_printf("%s:%d", host, port);
		/* Every user of the machine can reach a loopback port, and
		 * all of them have the same address there: on Linux the
		 * peer counts as the uid of its socket (sock_diag, or
		 * /proc/net/tcp within proc_budget). Any other peer, or one
		 * whose uid is not known, counts as its address; every
		 * loopback address is the one shared "ip:loopback". */
		if (fe_web_tcp_peer_uid(g_io_channel_unix_get_fd(handle), &proc_budget, time(NULL),
		                        &uid) > 0) {
			peer_key = fe_web_peer_key_uid((unsigned long) uid);
		} else {
			peer_key = fe_web_peer_key_ip(&ip);
			peer_shared = fe_web_ip_is_loopback(&ip);
		}
	}

	{
		WEB_CLIENT_REC *victim;
		int logged_in = 0;
		GSList *tmp;

		for (tmp = web_clients; tmp != NULL; tmp = tmp->next) {
			WEB_CLIENT_REC *c = tmp->data;
			if (c->authenticated)
				logged_in++;
		}
		if (logged_in >= FE_WEB_MAX_CLIENTS) {
			log_refused_addr(addr, "too many web clients logged in");
			g_free(addr);
			g_free(peer_key);
			net_disconnect(handle);
			return;
		}
		/* The queue is full: the busiest peer loses its oldest
		 * connection, so a loop of new connections, from one peer or
		 * from many, closes its own. The owner's uid keeps its
		 * connections while anyone else is waiting; a trusted peer
		 * keeps only its oldest pending one, and its lingering
		 * connections go first (fe_web_pending_victim()). */
		if (own_peer_key == NULL)
			own_peer_key = fe_web_peer_key_uid((unsigned long) getuid());
		victim = fe_web_pending_victim(web_clients, FE_WEB_MAX_PENDING, own_peer_key,
		                               login_pacing);
		if (victim != NULL) {
			log_refused(victim, "too many connections logging in, oldest closed");
			fe_web_close_client(victim);
		}
	}

	/* Increase the send buffer to 2MB for large state dumps. FreeBSD
	 * refuses one above kern.ipc.maxsockbuf (by default 2MB including its
	 * bookkeeping) with ENOBUFS: the largest of 1MB, 512KB, ... it takes */
	{
		int fd = g_io_channel_unix_get_fd(handle);
		int bufsize = 2 * 1024 * 1024; /* 2MB */
		int rc;

		for (;;) {
			rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
			if (rc == 0 || errno != ENOBUFS || bufsize <= 64 * 1024)
				break;
			bufsize /= 2;
		}
		if (rc < 0) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTCRAP,
			          "fe-web: Warning: Failed to set SO_SNDBUF to %d bytes: %s",
			          bufsize, strerror(errno));
		}
	}

	/* Create client record */
	client = fe_web_client_create(g_io_channel_unix_get_fd(handle), addr);
	client->peer_key = peer_key;
	client->peer_shared = peer_shared;

	/* Create send buffer */
	sendbuf = net_sendbuffer_create(handle, 0);
	client->handle = sendbuf;

	/* SSL is ALWAYS enabled - no option to disable */
	client->ssl_channel = fe_web_ssl_channel_create(handle);
	client->use_ssl = TRUE;
	if (client->ssl_channel == NULL) {
		/* never a plain text connection instead */
		fe_web_close_client(client);
		g_free(addr);
		return;
	}

	/* Encryption is ALWAYS enabled - no option to disable */
	client->encryption_enabled = TRUE;

	/* Add input handler */
	client->recv_tag = i_input_add(handle, I_INPUT_READ,
	                               (GInputFunction) client_input, client);
	client->handshake_tag = g_timeout_add_seconds(FE_WEB_HANDSHAKE_TIMEOUT,
	                                              handshake_timeout, client);

	g_free(addr);
}

/* fe_web_socket: "~/" is the home directory, a relative path is relative
 * to the irssi directory */
static char *socket_path_expand(const char *value)
{
	char *path, *full;

	path = convert_home(value);
	if (!g_path_is_absolute(path)) {
		full = g_build_filename(get_irssi_dir(), path, NULL);
		g_free(path);
		path = full;
	}
	return path;
}

/* No one else may be able to swap any part of the socket path: every
 * component is owned by the user or root, a directory writable by others
 * only if sticky (like /tmp), and a symbolic link only one owned by the
 * user or root (e.g. /home -> /usr/home on FreeBSD; another user's link in
 * /tmp could later be pointed elsewhere). Returns NULL when trusted, else
 * the offending component (to be freed). */
static char *socket_path_untrusted(const char *dir)
{
	char **parts, *prefix, *existing = NULL;
	gboolean link_seen = FALSE;
	int i;
	uid_t me = getuid();

	if (!g_path_is_absolute(dir))
		return g_strdup(dir);
	parts = g_strsplit(dir, "/", -1);
	prefix = g_strdup("/");
	for (i = 0; parts[i] != NULL; i++) {
		struct stat st;
		char *next;

		if (*parts[i] == '\0')
			continue;
		next = g_build_filename(prefix, parts[i], NULL);
		g_free(prefix);
		prefix = next;
		if (lstat(prefix, &st) < 0) {
			if (errno == ENOENT)
				break;	/* the rest is created 0700 by us */
			g_strfreev(parts);
			return prefix;
		}
		g_free(existing);
		existing = g_strdup(prefix);
		if (st.st_uid != me && st.st_uid != 0)
			goto bad;
		if (S_ISLNK(st.st_mode)) {
			link_seen = TRUE;
			continue;
		}
		if (!S_ISDIR(st.st_mode))
			goto bad;
		if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (st.st_mode & S_ISVTX) == 0)
			goto bad;
		continue;
bad:
		g_strfreev(parts);
		g_free(existing);
		return prefix;
	}
	g_strfreev(parts);
	g_free(prefix);
	if (link_seen) {
		/* a link of ours may still lead through a directory someone else
		 * can change - check where the existing part really is */
		char *real = realpath(existing, NULL), *bad;

		if (real == NULL)
			return existing;
		bad = strcmp(real, existing) == 0 ? NULL : socket_path_untrusted(real);
		free(real);
		g_free(existing);
		return bad;
	}
	g_free(existing);
	return NULL;
}

/* The socket's directory must be the user's own and writable only by the
 * user, and no one else may be able to swap any directory above it -
 * otherwise someone else could replace the socket with their own and
 * receive the web client's password. The directory itself must be a real
 * directory, not a symbolic link (lstat). A missing directory is made 0700. */
static gboolean socket_dir_check(const char *path)
{
	struct stat st;
	char *dir, *parent, *bad;
	gboolean ok = FALSE;

	dir = g_path_get_dirname(path);
	parent = g_path_get_dirname(dir);
	/* the directory itself is checked below, with a message for each case */
	bad = socket_path_untrusted(parent);
	g_free(parent);
	if (bad != NULL) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: %s in the fe_web_socket path can be changed by "
		          "another user (owner, mode or a link that is not yours) - put the socket "
		          "in a directory of your own, e.g. ~/.erssi/fe-web.sock",
		          bad);
		g_free(bad);
		g_free(dir);
		return FALSE;
	}
	if (lstat(dir, &st) < 0) {
		if (errno != ENOENT || (errno = 0, g_mkdir_with_parents(dir, 0700) < 0) ||
		    lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
		    chmod(dir, 0700) < 0 || lstat(dir, &st) < 0) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
			          "fe-web: Not starting: directory %s of fe_web_socket: %s", dir,
			          errno ? g_strerror(errno) : "not created as a directory of yours");
			g_free(dir);
			return FALSE;
		}
		/* someone else may have made a missing directory above it
		 * between the check and mkdir - check the path again */
		parent = g_path_get_dirname(dir);
		bad = socket_path_untrusted(parent);
		g_free(parent);
		if (bad != NULL) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
			          "fe-web: Not starting: %s in the fe_web_socket path can be changed by "
			          "another user (it appeared while erssi created the directory)",
			          bad);
			g_free(bad);
			g_free(dir);
			return FALSE;
		}
	}

	if (S_ISLNK(st.st_mode)) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: directory %s of fe_web_socket is a symbolic link - "
		          "use the real directory",
		          dir);
	} else if (!S_ISDIR(st.st_mode)) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: %s is not a directory", dir);
	} else if (st.st_uid != getuid()) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: directory %s of fe_web_socket is not owned by you",
		          dir);
	} else if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		/* printtext() knows no %o */
		char *mode = g_strdup_printf("%04o", (unsigned int) (st.st_mode & 07777));

		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: directory %s of fe_web_socket is writable by "
		          "others (mode %s) - use a directory only you can write to, "
		          "e.g. chmod 700 %s",
		          dir, mode, dir);
		g_free(mode);
	} else {
		ok = TRUE;
	}
	g_free(dir);
	return ok;
}

static void socket_addr_set(struct sockaddr_un *sa, const char *path)
{
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	strncpy(sa->sun_path, path, sizeof(sa->sun_path) - 1);
}

/* Lock "<path>.lock" (made 0600 when missing), so that only one erssi
 * uses the socket path. flock(), on Linux and the BSDs: the kernel drops
 * the lock when the process dies, so a lock file left by a killed erssi
 * holds no start, and the lock belongs to the open file - an fcntl() lock
 * is dropped by any close() of that file in the process, and one process
 * never conflicts with itself. The lock file is never removed: an erssi
 * that opened it just before could then lock a file no one else sees.
 * A symbolic link is refused, not followed (O_NOFOLLOW), and the file must
 * be a regular file of the user's own that no one else may open (a lock
 * held by another user would keep erssi from starting); not inherited by
 * /exec children.
 * Returns the fd; -1 when another process holds the lock; -2 when the
 * file system has no locks; -3 on any other error (errno set). */
static int socket_lock(const char *path)
{
	struct stat st;
	char *lock_path;
	int fd, err;

	lock_path = g_strconcat(path, ".lock", NULL);
	/* O_RDWR: Linux emulates flock() on NFS with fcntl() locks, and an
	 * exclusive one of those needs the file open for writing.
	 * O_NONBLOCK: a FIFO put there must not hang erssi in open();
	 * O_NOCTTY: nor may a terminal there become erssi's */
	fd = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY,
	          0600);
	err = errno;
	g_free(lock_path);
	if (fd < 0) {
		errno = err;
		return -3;
	}
	if (fstat(fd, &st) < 0)
		err = errno;
	else if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0)
		err = EPERM;
	else
		err = 0;
	if (err != 0) {
		close(fd);
		errno = err;
		return -3;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		err = errno;
		close(fd);
		if (err == EWOULDBLOCK || err == EAGAIN)
			return -1;
		if (err == EOPNOTSUPP || err == ENOLCK || err == EINVAL)
			return -2;
		errno = err;
		return -3;
	}
	return fd;
}

/* Why socket_lock() failed with 'err', for the message */
static void socket_lock_failed(const char *path, int err)
{
	struct stat st;
	char *lock_path, *why;

	lock_path = g_strconcat(path, ".lock", NULL);
	if (lstat(lock_path, &st) < 0)
		why = g_strdup(g_strerror(err));
	else if (S_ISLNK(st.st_mode))
		why = g_strdup("it is a symbolic link - remove it");
	else if (!S_ISREG(st.st_mode) || st.st_uid != getuid())
		why = g_strdup("it is not a regular file of yours - remove it");
	else if ((st.st_mode & 077) != 0)
		/* printtext() knows no %o */
		why = g_strdup_printf("others may open it (mode %04o) - chmod 600 %s",
		                      (unsigned int) (st.st_mode & 07777), lock_path);
	else
		why = g_strdup(g_strerror(err));
	printtext(NULL, NULL, MSGLEVEL_CLIENTERROR, "fe-web: Not starting: cannot lock %s: %s",
	          lock_path, why);
	g_free(why);
	g_free(lock_path);
}

/* A socket file left by an erssi that did not stop cleanly is removed - but
 * only a socket of the user's own that nothing listens on any more. With
 * 'locked' this erssi holds the lock of the path (socket_lock()), so no
 * other erssi uses it. */
static gboolean socket_stale_remove(const char *path, gboolean locked)
{
	struct sockaddr_un sa;
	struct stat st;
	int fd, ret, err;

	if (lstat(path, &st) < 0) {
		if (errno == ENOENT)
			return TRUE;
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Cannot use %s: %s", path, g_strerror(errno));
		return FALSE;
	}
	if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: %s exists and is not a socket of yours - "
		          "remove it or choose another fe_web_socket",
		          path);
		return FALSE;
	}

	/* non-blocking: a listener with a full queue must not hang erssi */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Cannot create a socket: %s", g_strerror(errno));
		return FALSE;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	socket_addr_set(&sa, path);
	ret = connect(fd, (struct sockaddr *) &sa, sizeof(sa));
	err = errno;
	close(fd);

	if (ret == 0 || err == EAGAIN || err == EINPROGRESS) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: another program is listening on unix:%s", path);
		return FALSE;
	}
	if (err != ECONNREFUSED) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: cannot check the old socket %s: %s", path,
		          g_strerror(err));
		return FALSE;
	}
	/* outside Linux this may be an erssi whose queue is full: only the
	 * lock tells it is not */
	if (!locked && !econnrefused_means_no_listener) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: cannot tell whether unix:%s is still in use - "
		          "remove it if no erssi uses it",
		          path);
		return FALSE;
	}
	if (unlink(path) < 0 && errno != ENOENT) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Cannot remove the old socket %s: %s", path, g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

/* Bind and listen. The socket file is made 0600 from the start (umask), so
 * there is no moment in which another user could connect. */
static GIOChannel *socket_listen(const char *path)
{
	struct sockaddr_un sa;
	struct stat st;
	mode_t old_umask;
	int fd, ret, err;

	if (strlen(path) >= sizeof(sa.sun_path)) {
		errno = ENAMETOOLONG;
		return NULL;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	socket_addr_set(&sa, path);
	old_umask = umask(077);
	ret = bind(fd, (struct sockaddr *) &sa, sizeof(sa));
	err = errno;
	umask(old_umask);
	if (ret < 0) {
		close(fd);
		errno = err;
		return NULL;
	}

	if (chmod(path, 0600) < 0 || lstat(path, &st) < 0 ||
	    listen(fd, FE_WEB_MAX_CLIENTS) < 0) {
		err = errno;
		unlink(path);
		close(fd);
		errno = err;
		return NULL;
	}
	listen_dev = st.st_dev;
	listen_ino = st.st_ino;
	return i_io_channel_new(fd);
}

/* Remove the socket file - if it still is the one this server made */
static void socket_remove(void)
{
	struct stat st;

	if (listen_path == NULL)
		return;
	if (lstat(listen_path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_dev == listen_dev &&
	    st.st_ino == listen_ino)
		unlink(listen_path);
	g_free(listen_path);
	listen_path = NULL;
}

static void server_listen_unix(const char *value)
{
	char *path;
	int lock_fd;

	path = socket_path_expand(value);
	{
		struct sockaddr_un sa;

		if (strlen(path) >= sizeof(sa.sun_path)) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
			          "fe-web: Not starting: fe_web_socket path %s is too long "
			          "(at most %d bytes)",
			          path, (int) sizeof(sa.sun_path) - 1);
			g_free(path);
			return;
		}
	}
	if (!socket_dir_check(path)) {
		g_free(path);
		return;
	}
	/* the lock first: only its holder may decide that the socket there
	 * is stale and remove it */
	lock_fd = socket_lock(path);
	if (lock_fd == -1) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Not starting: another erssi is using unix:%s", path);
		g_free(path);
		return;
	}
	if (lock_fd == -3) {
		socket_lock_failed(path, errno);
		g_free(path);
		return;
	}
	if (!socket_stale_remove(path, lock_fd >= 0)) {
		if (lock_fd >= 0)
			close(lock_fd);
		g_free(path);
		return;
	}

	listen_channel = socket_listen(path);
	if (listen_channel == NULL) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Failed to listen on unix:%s: %s", path, g_strerror(errno));
		if (lock_fd >= 0)
			close(lock_fd);
		g_free(path);
		return;
	}
	listen_path = path;
	listen_lock_fd = lock_fd >= 0 ? lock_fd : -1;

	listen_tag = i_input_add(listen_channel, I_INPUT_READ,
	                         (GInputFunction) sig_listen, NULL);

	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "fe-web: WebSocket server listening on unix:%s (SSL + AES-256-GCM)", path);
	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "fe-web: Security: SSL/TLS enabled, Application-level encryption enabled, "
	          "socket 0600, only your own processes");
}

/* Initialize server */
void fe_web_server_init(void)
{
	IPADDR *bind_ip;
	const char *bind_addr;
	const char *password;
	const char *socket_path;
	int port;

	/* Check if already running */
	if (listen_channel != NULL) {
		return;
	}

	/* SECURITY: Verify password is set */
	password = settings_get_str("fe_web_password");
	if (password == NULL || *password == '\0') {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: FATAL: Cannot start server without password!");
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Please set password: /SET fe_web_password <strong-password>");
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Use a long random secret, e.g. the output of: openssl rand -base64 32");
		return;
	}

	/* SECURITY: TLS context (key and certificate made at the first start;
	 * the Unix socket uses TLS too) */
	if (!fe_web_ssl_prepare()) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: FATAL: SSL/TLS not initialized!");
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: SSL certificate generation failed. Check OpenSSL installation.");
		return;
	}

	/* SECURITY: Verify encryption is initialized */
	if (!fe_web_crypto_is_enabled()) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: FATAL: Encryption not initialized!");
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Encryption key derivation failed. Check password setting.");
		return;
	}

	/* A Unix socket instead of TCP */
	socket_path = settings_get_str("fe_web_socket");
	if (socket_path != NULL && *socket_path != '\0') {
		server_listen_unix(socket_path);
		return;
	}

	/* Get settings */
	port = settings_get_int("fe_web_port");
	bind_addr = settings_get_str("fe_web_bind");

	/* Parse bind address */
	bind_ip = g_new0(IPADDR, 1);
	if (net_host2ip(bind_addr, bind_ip) != 0) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Invalid bind address: %s", bind_addr);
		g_free(bind_ip);
		return;
	}

	/* Create listening socket */
	listen_channel = net_listen(bind_ip, &port);
	if (listen_channel != NULL)
		fcntl(g_io_channel_unix_get_fd(listen_channel), F_SETFD, FD_CLOEXEC);
	g_free(bind_ip);

	if (listen_channel == NULL) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "fe-web: Failed to bind to %s:%d: %s",
		          bind_addr, port, strerror(errno));
		return;
	}

	listen_port = port;

	/* Add input handler */
	listen_tag = i_input_add(listen_channel, I_INPUT_READ,
	                         (GInputFunction) sig_listen, NULL);

	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "fe-web: WebSocket server listening on wss://%s:%d (SSL + AES-256-GCM)",
	          bind_addr, port);
	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "fe-web: Security: SSL/TLS enabled, Application-level encryption enabled");
}

/* Deinitialize server */
void fe_web_server_deinit(void)
{
	GSList *tmp;
	GSList *next;

	/* Close all clients */
	for (tmp = web_clients; tmp != NULL; tmp = next) {
		WEB_CLIENT_REC *client = tmp->data;
		next = tmp->next;
		fe_web_close_client(client);
	}

	if (relisten_tag != 0) {
		g_source_remove(relisten_tag);
		relisten_tag = 0;
	}

	/* Close listening socket */
	if (listen_tag != -1) {
		g_source_remove(listen_tag);
		listen_tag = -1;
	}

	if (listen_channel != NULL) {
		net_disconnect(listen_channel);
		listen_channel = NULL;
	}
	socket_remove();
	/* releases the lock; the lock file stays (socket_lock()) */
	if (listen_lock_fd >= 0) {
		close(listen_lock_fd);
		listen_lock_fd = -1;
	}

	listen_port = -1;

	fe_web_pacing_free(login_pacing);
	login_pacing = NULL;
	g_free(own_peer_key);
	own_peer_key = NULL;

	printtext(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
	          "fe-web: WebSocket server stopped");
}
