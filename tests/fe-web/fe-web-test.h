#ifndef IRSSI_TESTS_FE_WEB_FE_WEB_TEST_H
#define IRSSI_TESTS_FE_WEB_FE_WEB_TEST_H

/* Helpers of the fe-web unit tests */

#include <irssi/src/fe-fuzz/fe-web/fe-web-fuzz.h>
#include <irssi/src/fe-web/fe-web-ssl.h>

/* g_test_init(), with warnings not fatal (net_transmit() warns when the
 * peer went away) and criticals still fatal, then fe_web_fuzz_init() */
void test_init(int *argc, char ***argv);

/* A WebSocket frame (FIN set); 'masked' as a browser sends it, with the
 * mask 11 22 33 44. Free with g_byte_array_unref(). */
GByteArray *test_frame(int opcode, const void *payload, gsize len, gboolean masked);

/* A WebSocket upgrade request with "Authorization: Bearer <password>"
 * (no Authorization header when password is NULL); to be freed */
char *test_request(const char *password);

/* Does data start with prefix */
gboolean test_starts_with(const GByteArray *data, const char *prefix);

/* Output queued for the client and not written yet */
gsize test_pending(WEB_CLIENT_REC *client);

#endif
