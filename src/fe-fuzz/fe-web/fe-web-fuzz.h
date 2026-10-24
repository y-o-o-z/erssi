#ifndef IRSSI_FE_FUZZ_FE_WEB_FE_WEB_FUZZ_H
#define IRSSI_FE_FUZZ_FE_WEB_FE_WEB_FUZZ_H

/* Shared setup for the fe-web fuzz targets.
 *
 * A web client is driven exactly as fe-web drives it: the bytes go into
 * one end of a socketpair and the real client_input_once() of
 * fe-web-server.c reads and handles them from the other end (plain socket
 * instead of TLS - the TLS layer is OpenSSL's, fe-web parses what comes out
 * of it). Everything fe-web sends back is read, and kept for the unit
 * tests until the next client is made. */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <irssi/src/fe-web/fe-web.h>

/* password the fuzz targets configure as fe_web_password */
#define FE_WEB_FUZZ_PASSWORD "fuzz-password"

/* input is cut into separate reads at this marker */
#define FE_WEB_FUZZ_SPLIT "@@READ@@"

/* Initialize irssi core, irc, fe-common and the parts of fe-web the
 * targets use. Commands sent by web clients are swallowed, never run, and
 * irssi's home is a fresh temporary directory. settings_autosave is OFF,
 * so the network and server edits of web clients are not written to the
 * config file (a test that wants them written sets it ON). */
void fe_web_fuzz_init(gboolean with_irc_signals);

/* The temporary directory fe_web_fuzz_init() made irssi's home (and
 * HOME); NULL before */
const char *fe_web_fuzz_home(void);

/* New web client on a socketpair; logged in already if 'logged_in' */
WEB_CLIENT_REC *fe_web_fuzz_client_new(gboolean logged_in);

/* Send data from the browser side, as one read */
void fe_web_fuzz_client_send(WEB_CLIENT_REC *client, const guchar *data, gsize len);

/* Send data cut into reads at FE_WEB_FUZZ_SPLIT */
void fe_web_fuzz_client_send_split(WEB_CLIENT_REC *client, const guchar *data, gsize len);

/* Is the client still connected (fe-web closes clients on bad input).
 * 'client' is the one fe_web_fuzz_client_new*() made last (anything else
 * is a test error), and FALSE once fe-web closed it, even when a new
 * client got the same memory. NULL: FALSE. */
gboolean fe_web_fuzz_client_alive(WEB_CLIENT_REC *client);

/* Close the client if fe-web did not already, and the browser end; the
 * next fe_web_fuzz_client_new*() may follow. NULL: only the browser end. */
void fe_web_fuzz_client_free(WEB_CLIENT_REC *client);

/* Forget login failures (and the count of new connections) so every
 * input gets its password checked */
void fe_web_fuzz_reset_login_state(void);

/* The login pacing of fe-web-server.c (made if there is none yet) */
struct _FE_WEB_PACING *fe_web_fuzz_login_pacing(void);

/* Remove networks and servers that web clients added */
void fe_web_fuzz_reset_setup(void);

/* An IRC server with this tag on /dev/null (what it sends goes nowhere),
 * past server_connect_finished(): it is in the servers list, and
 * "connected" only once a 001 comes in; its lines come in with
 * signal_emit("server incoming", ...). fe_web_fuzz_server_free()
 * disconnects it (NULL: nothing). */
IRC_SERVER_REC *fe_web_fuzz_server_new(const char *tag);
void fe_web_fuzz_server_free(IRC_SERVER_REC *server);

/* fe_web_verify_password() of fe-web-server.c, on a NUL-terminated
 * request */
int fe_web_fuzz_verify_password(const char *request);

/* for the unit tests in tests/fe-web */

/* New web client on a socketpair, as fe_web_fuzz_client_new(); with 'tls'
 * it has use_ssl set and a stand-in TLS channel, which is the plain socket
 * (see fe_web_fuzz_tls_write_script()). 'peer_key' is the peer the client
 * counts as for login pacing (client->peer_key, e.g. "uid:1001");
 * fe_web_fuzz_client_new() uses "fuzz". Clears fe_web_fuzz_received() and
 * the write script. */
WEB_CLIENT_REC *fe_web_fuzz_client_new_peer(const char *peer_key, gboolean logged_in,
                                            gboolean tls);

/* Everything the browser end read for the current client */
const GByteArray *fe_web_fuzz_received(void);

/* Read at most 'max' bytes (0: until nothing is left) at the browser end,
 * into fe_web_fuzz_received(); returns how many */
gsize fe_web_fuzz_read_peer(gsize max);

/* What the writable watch of the client does: output_ready() when it has
 * one (send_tag != -1) */
void fe_web_fuzz_client_writable(WEB_CLIENT_REC *client);

/* What the input watch of the client does: client_input() */
void fe_web_fuzz_client_input(WEB_CLIENT_REC *client);

/* The browser end writes; fe-web does not read it yet */
void fe_web_fuzz_peer_write(const void *data, gsize len);

/* Results for the next writes of the stand-in TLS: n > 0 writes at most
 * n bytes, FE_WEB_SSL_WANT_WRITE, FE_WEB_SSL_WANT_READ or -1 (an error).
 * Once the script is used up, writes go to the socket (a full socket gives
 * FE_WEB_SSL_WANT_WRITE). */
void fe_web_fuzz_tls_write_script(const int *results, gsize n);

/* After a WANT result the stand-in TLS checks the OpenSSL retry rule: the
 * next write on that channel offers at least the same bytes again, or the
 * test fails (g_error). */

/* The failed flag (no close_notify) of the last TLS channel freed since
 * fe_web_fuzz_client_new_peer(): 0 or 1, -1 when none was freed */
int fe_web_fuzz_tls_freed_failed(void);

/* Unix sockets: with 'on' the peer of every accepted connection counts as
 * a process of uid getuid() + 1 (SO_PEERCRED / getpeereid) */
void fe_web_fuzz_fake_peer_other_uid(gboolean on);

/* The next g_mkdir_with_parents() of fe-web-server.c first creates 'dir'
 * with 'mode' (chmod after mkdir): another user who wins the race */
void fe_web_fuzz_mkdir_first(const char *dir, int mode);

/* After the next g_mkdir_with_parents() of fe-web-server.c succeeds, 'fn'
 * runs with its directory: something that swaps it right after */
void fe_web_fuzz_after_mkdir(void (*fn)(const char *dir));

/* connect() of fe-web-server.c fails with 'err' (the check of an old
 * socket file); 0 means the real connect() */
void fe_web_fuzz_fake_connect_errno(int err);

/* flock() of fe-web-server.c (the lock file of fe_web_socket) fails with
 * 'err'; 0 means the real flock() */
void fe_web_fuzz_fake_flock_errno(int err);

/* Does ECONNREFUSED from connect() mean that nothing listens (TRUE on
 * Linux; elsewhere a listener with a full queue says so too) */
void fe_web_fuzz_set_econnrefused_reliable(gboolean reliable);

/* Back to fe-web-server.c's own value of it */
void fe_web_fuzz_reset_econnrefused_reliable(void);

/* Use up the /proc lookups fe-web-server.c may make in this second of
 * its time(): a TCP loopback peer whose uid sock_diag does not give is
 * then ip:loopback */
void fe_web_fuzz_spend_proc_budget(void);

/* time() in fe-web-server.c; 0 means the real clock */
void fe_web_fuzz_set_clock(time_t now);

/* The text, without formatting codes, of every "print text" since the
 * last call, one line each (to be freed) */
char *fe_web_fuzz_take_printed(void);

/* With TRUE, commands sent by web clients run (the unit tests of what a
 * command may do); FALSE swallows them again, as fe_web_fuzz_init() does. */
void fe_web_fuzz_allow_commands(gboolean allow);

/* Run the default main context for 'ms' milliseconds */
void fe_web_fuzz_iterate(int ms);

/* The port fe-web-server.c listens on (-1: none) */
int fe_web_fuzz_listen_port(void);

/* the libFuzzer entry points every target defines */
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif
