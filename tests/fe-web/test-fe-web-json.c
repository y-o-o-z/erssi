/*
 test-fe-web-json.c : the JSON lookups in web client messages

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 Every field fe-web reads from a client message (type, id, server,
 command, target, ...) is a member of the message's top-level object:
 keys of nested objects and arrays, and text inside string values, must
 never be taken for one.
*/

#define MODULE_NAME "test-fe-web-json"

#include "fe-web-test.h"

#include <irssi/src/core/signals.h>

#include <string.h>

static void assert_string(const char *json, const char *key, const char *expected)
{
	char *value = fe_web_json_get_string(json, key);

	if (g_strcmp0(value, expected) != 0)
		g_error("fe_web_json_get_string(%s, \"%s\") = %s%s%s, not %s%s%s", json, key,
		        value != NULL ? "\"" : "", value != NULL ? value : "NULL",
		        value != NULL ? "\"" : "", expected != NULL ? "\"" : "",
		        expected != NULL ? expected : "NULL", expected != NULL ? "\"" : "");
	g_free(value);
}

static void test_nested_keys(void)
{
	assert_string("{\"metadata\":{\"type\":\"command\"},\"type\":\"ping\"}", "type", "ping");
	assert_string("{\"type\":\"ping\",\"metadata\":{\"type\":\"command\"}}", "type", "ping");
	assert_string("{\"metadata\":{\"type\":\"command\"}}", "type", NULL);
	assert_string("{\"list\":[{\"type\":\"command\"},\"type\"],\"id\":\"7\"}", "type", NULL);
	assert_string("{\"list\":[{\"type\":\"command\"},\"type\"],\"id\":\"7\"}", "id", "7");
	assert_string("{\"a\":{\"b\":{\"target\":\"#x\"}},\"target\":\"#y\"}", "target", "#y");
	/* the value of a nested object is no string */
	assert_string("{\"type\":{\"type\":\"ping\"}}", "type", NULL);

	g_assert_false(fe_web_json_has_key("{\"x\":{\"port\":1}}", "port"));
	g_assert_true(fe_web_json_has_key("{\"x\":{\"port\":1},\"port\":2}", "port"));
	g_assert_cmpint(fe_web_json_get_int("{\"x\":{\"port\":1}}", "port", -1), ==, -1);
	g_assert_cmpint(fe_web_json_get_int("{\"x\":[{\"port\":1}],\"port\":6697}", "port", -1),
	                ==, 6697);
}

static void test_keys_in_strings(void)
{
	/* a key written inside a string value is only text */
	assert_string("{\"text\":\"\\\"type\\\":\\\"command\\\"\",\"type\":\"ping\"}", "type",
	              "ping");
	assert_string("{\"text\":\"\\\"type\\\":\\\"command\\\"\"}", "type", NULL);
	assert_string("{\"text\":\"type\"}", "type", NULL);
	g_assert_false(fe_web_json_has_key("{\"text\":\"\\\"port\\\":1\"}", "port"));
	g_assert_cmpint(fe_web_json_get_int("{\"text\":\"\\\"port\\\":1\"}", "port", -1), ==, -1);
	/* nor a value taken for a key */
	assert_string("{\"name\":\"type\",\"value\":\"x\"}", "type", NULL);
}

static void test_escaped_quotes(void)
{
	assert_string("{\"text\":\"a\\\"b\",\"type\":\"ping\"}", "text", "a\"b");
	assert_string("{\"text\":\"a\\\"b\",\"type\":\"ping\"}", "type", "ping");
	/* an escaped backslash, then the closing quote */
	assert_string("{\"text\":\"\\\\\",\"type\":\"ping\"}", "text", "\\");
	assert_string("{\"text\":\"\\\\\",\"type\":\"ping\"}", "type", "ping");
	assert_string("{\"text\":\"\\\\\\\"\",\"type\":\"ping\"}", "text", "\\\"");
	/* a quote in a key does not end it */
	assert_string("{\"t\\\"ype\":\"x\",\"type\":\"y\"}", "type", "y");
	assert_string("{\"t\\\"ype\":\"x\"}", "t\"ype", "x");
	/* a key with escapes is what it means, as to any JSON parser */
	assert_string("{\"\\u0074ype\":\"ping\"}", "type", "ping");
}

static void test_duplicate_keys(void)
{
	/* the last one wins, as with the browser's JSON.parse() */
	assert_string("{\"type\":\"command\",\"type\":\"ping\"}", "type", "ping");
	assert_string("{\"type\":\"ping\",\"x\":1,\"type\":\"command\"}", "type", "command");
	assert_string("{\"type\":\"ping\",\"type\":7}", "type", NULL);
	g_assert_cmpint(fe_web_json_get_int("{\"port\":1,\"port\":2}", "port", -1), ==, 2);
	g_assert_cmpint(fe_web_json_get_int("{\"port\":1,\"port\":\"2\"}", "port", -1), ==, -1);
}

static void test_malformed(void)
{
	static const char *const bad[] = {
		"",
		"\"type\":\"ping\"",
		"[\"type\",\"ping\"]",
		"{\"type\":\"ping\"",
		"{\"type\":\"ping\",}",
		"{\"type\":\"ping\"}x",
		"{\"type\":\"ping\"}{\"type\":\"command\"}",
		"{\"type\" \"ping\"}",
		"{\"type\":\"ping\" \"id\":\"1\"}",
		"{\"x\":tru,\"type\":\"ping\"}",
		"{\"x\":01,\"type\":\"ping\"}",
		"{\"x\":-,\"type\":\"ping\"}",
		"{\"x\":1.,\"type\":\"ping\"}",
		"{\"x\":[1,],\"type\":\"ping\"}",
		"{\"x\":[1}],\"type\":\"ping\"}",
		"{\"x\":{\"a\"},\"type\":\"ping\"}",
		"{\"x\":\"open,\"type\":\"ping\"}",
		"{\"type\":\"ping\\\"}",
		"{type:\"ping\"}",
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		assert_string(bad[i], "type", NULL);
		g_assert_false(fe_web_json_has_key(bad[i], "type"));
	}

	/* what is well-formed */
	assert_string(" \r\n\t{ \"type\" : \"ping\" } \n", "type", "ping");
	assert_string("{\"x\":[],\"y\":{},\"z\":[null,true,false,-0.5e+3,\"\"],\"type\":\"ping\"}",
	              "type", "ping");
	g_assert_true(fe_web_json_has_key("{\"type\":null}", "type"));
	assert_string("{\"type\":null}", "type", NULL);
}

/* {"a":[[...[0]...]],"type":"ping"} with 'arrays' arrays */
static char *nested(int arrays)
{
	GString *json = g_string_new("{\"a\":");
	int i;

	for (i = 0; i < arrays; i++)
		g_string_append_c(json, '[');
	g_string_append_c(json, '0');
	for (i = 0; i < arrays; i++)
		g_string_append_c(json, ']');
	g_string_append(json, ",\"type\":\"ping\"}");
	return g_string_free(json, FALSE);
}

static void test_depth_limit(void)
{
	char *json;

	/* 64 levels with the message's own object */
	json = nested(63);
	assert_string(json, "type", "ping");
	g_free(json);

	json = nested(64);
	assert_string(json, "type", NULL);
	g_free(json);

	/* far too deep is turned away, without running out of stack */
	json = nested(1000000);
	assert_string(json, "type", NULL);
	g_free(json);
}

static GPtrArray *sent_commands;

static void sig_send_command(const char *command)
{
	g_ptr_array_add(sent_commands, g_strdup(command));
}

static gboolean received_has(const char *text)
{
	const GByteArray *data = fe_web_fuzz_received();

	return g_strstr_len((const char *) data->data, data->len, text) != NULL;
}

/* Whole messages: a ping with a "type" nested in it is a ping, and a
 * command runs only what its own "command" member says */
static void test_message_type(void)
{
	WEB_CLIENT_REC *c;

	sent_commands = g_ptr_array_new_with_free_func(g_free);
	signal_add_first("send command", (SIGNAL_FUNC) sig_send_command);

	/* plain text frames, so that the pong can be seen */
	c = fe_web_fuzz_client_new(TRUE);
	c->encryption_enabled = FALSE;
	fe_web_client_handle_message(
	    c, "{\"metadata\":{\"type\":\"command\"},\"command\":\"echo nested\",\"type\":\"ping\"}");
	fe_web_fuzz_read_peer(0);
	g_assert_cmpuint(sent_commands->len, ==, 0);
	g_assert_true(received_has("\"pong\""));
	fe_web_fuzz_client_free(c);

	c = fe_web_fuzz_client_new(TRUE);
	c->encryption_enabled = FALSE;
	fe_web_client_handle_message(
	    c, "{\"metadata\":{\"command\":\"echo nested\"},\"type\":\"command\"}");
	g_assert_cmpuint(sent_commands->len, ==, 0);
	fe_web_client_handle_message(
	    c, "{\"metadata\":{\"type\":\"ping\"},\"type\":\"command\",\"command\":\"echo top\"}");
	g_assert_cmpuint(sent_commands->len, ==, 1);
	g_assert_cmpstr(sent_commands->pdata[0], ==, "echo top");
	fe_web_fuzz_read_peer(0);
	g_assert_false(received_has("\"pong\""));
	fe_web_fuzz_client_free(c);

	signal_remove("send command", (SIGNAL_FUNC) sig_send_command);
	g_ptr_array_free(sent_commands, TRUE);
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);

	g_test_add_func("/fe-web/json/nested-keys", test_nested_keys);
	g_test_add_func("/fe-web/json/keys-in-strings", test_keys_in_strings);
	g_test_add_func("/fe-web/json/escaped-quotes", test_escaped_quotes);
	g_test_add_func("/fe-web/json/duplicate-keys", test_duplicate_keys);
	g_test_add_func("/fe-web/json/malformed", test_malformed);
	g_test_add_func("/fe-web/json/depth-limit", test_depth_limit);
	g_test_add_func("/fe-web/json/message-type", test_message_type);

	return g_test_run();
}
