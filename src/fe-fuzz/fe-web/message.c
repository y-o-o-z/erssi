/*
 message.c : fuzz the JSON messages of a logged-in web client

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The input is one message as fe-web gets it from a text frame (or from a
 decrypted binary frame): fe_web_client_handle_message() with its JSON
 lookups, and the handlers of each message type (network_add,
 server_list, mark_read, ...). Commands are swallowed, not run.
*/

#include <irssi/src/fe-fuzz/fe-web/fe-web-fuzz.h>

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void) argc;
	(void) argv;
	fe_web_fuzz_init(FALSE);
	return 0;
}

static const char *const keys[] = {
	"type", "id", "server", "command", "target", "nick", "channel", "name",
	"address", "port", "chatnet", "password", "network", "",
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	WEB_CLIENT_REC *client;
	char *json;
	gsize i;

	json = g_strndup((const char *) data, size);

	/* the lookups on their own, and the escaping of what they return */
	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		char *value = fe_web_json_get_string(json, keys[i]);
		char *escaped = fe_web_escape_json(value);

		if (value != NULL && strlen(escaped) < strlen(value))
			abort();
		fe_web_json_get_int(json, keys[i], -1);
		fe_web_json_has_key(json, keys[i]);
		g_free(escaped);
		g_free(value);
	}

	client = fe_web_fuzz_client_new(TRUE);
	fe_web_client_handle_message(client, json);
	/* a network_list/server_list now lists what the message added */
	fe_web_client_handle_message(client, "{\"type\":\"network_list\",\"id\":\"1\"}");
	fe_web_client_handle_message(client, "{\"type\":\"server_list\",\"id\":\"2\"}");
	fe_web_fuzz_client_free(client);

	fe_web_fuzz_reset_setup();
	g_free(json);
	return 0;
}
