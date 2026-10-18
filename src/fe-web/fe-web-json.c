/*
 fe-web-json.c : Simple JSON parser for fe-web

    Copyright (C) 2025

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "module.h"
#include "fe-web.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

/* Value of the 4 hex digits at p, or -1 */
static int json_hex4(const char *p)
{
	int i, value = 0;

	for (i = 0; i < 4; i++) {
		int digit = g_ascii_xdigit_value(p[i]);

		if (digit < 0)
			return -1;
		value = value * 16 + digit;
	}
	return value;
}

/* Helper: Unescape JSON string (\\, \", \n, \r, \t, \b, \f, \uXXXX) */
static char *fe_web_json_unescape(const char *str)
{
	GString *result;
	const char *p;

	if (str == NULL) {
		return NULL;
	}

	result = g_string_new("");
	p = str;

	while (*p != '\0') {
		if (*p == '\\' && *(p + 1) != '\0') {
			p++; /* Skip backslash */
			switch (*p) {
			case '"':
				g_string_append_c(result, '"');
				break;
			case '\\':
				g_string_append_c(result, '\\');
				break;
			case '/':
				g_string_append_c(result, '/');
				break;
			case 'b':
				g_string_append_c(result, '\b');
				break;
			case 'f':
				g_string_append_c(result, '\f');
				break;
			case 'n':
				g_string_append_c(result, '\n');
				break;
			case 'r':
				g_string_append_c(result, '\r');
				break;
			case 't':
				g_string_append_c(result, '\t');
				break;
			case 'u': {
				/* \uXXXX is a UTF-16 unit: a surrogate pair is one
				 * character, and a lone surrogate or a NUL (which a
				 * C string cannot hold) becomes U+FFFD */
				int code = json_hex4(p + 1);

				if (code < 0) {
					/* not 4 hex digits: keep it as it is */
					g_string_append_c(result, 'u');
					break;
				}
				p += 4;
				if (code >= 0xD800 && code <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
					int low = json_hex4(p + 3);

					if (low >= 0xDC00 && low <= 0xDFFF) {
						code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
						p += 6;
					}
				}
				if (code == 0 || (code >= 0xD800 && code <= 0xDFFF))
					code = 0xFFFD;
				g_string_append_unichar(result, code);
				break;
			}
			default:
				/* Unknown escape - keep as-is */
				g_string_append_c(result, *p);
				break;
			}
			p++;
		} else {
			g_string_append_c(result, *p);
			p++;
		}
	}

	return g_string_free(result, FALSE);
}

/*
 Lookups in a client message read only the members of its top-level
 object. A message is first checked to be one well-formed JSON object
 (a stray character after it, an unclosed string or bracket: no member
 is found at all), and a key is only ever matched at the top level, so
 {"meta":{"type":"command"},"type":"ping"} is a ping and a key written
 inside a string value is just text. A name given twice counts with its
 last value, as in the browser's JSON.parse() and e2e-json.c.
*/

/* How deeply objects and arrays may nest in a message: far more than any
   client sends, and it bounds the recursion of json_skip_value() */
#define JSON_MAX_DEPTH 64

static const char *json_skip_value(const char *p, int depth);

static const char *json_skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* p is at the opening quote of a string: where it ends (after the
 * closing quote), or NULL when it is not closed. An escape is skipped
 * whole, so \" does not end the string; what it means is left to
 * fe_web_json_unescape(). */
static const char *json_skip_string(const char *p)
{
	for (p++; *p != '"'; p++) {
		if (*p == '\0')
			return NULL;
		if (*p == '\\' && *++p == '\0')
			return NULL;
	}
	return p + 1;
}

static const char *json_skip_digits(const char *p)
{
	if (!g_ascii_isdigit(*p))
		return NULL;
	while (g_ascii_isdigit(*p))
		p++;
	return p;
}

/* -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
static const char *json_skip_number(const char *p)
{
	if (*p == '-')
		p++;
	if (*p == '0')
		p++;
	else if ((p = json_skip_digits(p)) == NULL)
		return NULL;
	if (*p == '.' && (p = json_skip_digits(p + 1)) == NULL)
		return NULL;
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-')
			p++;
		p = json_skip_digits(p);
	}
	return p;
}

/* Is the string that starts at the quote at p the name key */
static gboolean json_key_is(const char *p, const char *end, const char *key)
{
	gsize len = end - p - 2;
	char *raw, *name;
	gboolean same;

	/* a name with escapes is compared as what it means ("\u0074ype" is
	 * "type" to a JSON parser too) */
	if (memchr(p + 1, '\\', len) == NULL)
		return strlen(key) == len && memcmp(p + 1, key, len) == 0;
	raw = g_strndup(p + 1, len);
	name = fe_web_json_unescape(raw);
	same = strcmp(name, key) == 0;
	g_free(name);
	g_free(raw);
	return same;
}

/* p is at the '{' of an object: where it ends (after the '}'), or NULL
 * when it is not well-formed. With a key, *value is set to the start of
 * the value of its last member of that name; members of the objects and
 * arrays inside are skipped, never matched. */
static const char *json_walk_object(const char *p, int depth, const char *key,
                                    const char **value)
{
	p = json_skip_ws(p + 1);
	if (*p == '}')
		return p + 1;
	for (;;) {
		const char *name = p;
		gboolean match;

		if (*p != '"' || (p = json_skip_string(p)) == NULL)
			return NULL;
		match = key != NULL && json_key_is(name, p, key);

		p = json_skip_ws(p);
		if (*p != ':')
			return NULL;
		p = json_skip_ws(p + 1);
		if (match)
			*value = p;
		if ((p = json_skip_value(p, depth)) == NULL)
			return NULL;
		p = json_skip_ws(p);
		if (*p == '}')
			return p + 1;
		if (*p != ',')
			return NULL;
		p = json_skip_ws(p + 1);
	}
}

static const char *json_walk_array(const char *p, int depth)
{
	p = json_skip_ws(p + 1);
	if (*p == ']')
		return p + 1;
	for (;;) {
		if ((p = json_skip_value(p, depth)) == NULL)
			return NULL;
		p = json_skip_ws(p);
		if (*p == ']')
			return p + 1;
		if (*p != ',')
			return NULL;
		p = json_skip_ws(p + 1);
	}
}

/* p is at a value inside a container nested 'depth' deep: where the value
 * ends, or NULL when it is not well-formed */
static const char *json_skip_value(const char *p, int depth)
{
	switch (*p) {
	case '"':
		return json_skip_string(p);
	case '{':
	case '[':
		if (depth >= JSON_MAX_DEPTH)
			return NULL;
		return *p == '{' ? json_walk_object(p, depth + 1, NULL, NULL) :
		                   json_walk_array(p, depth + 1);
	case 't':
		return strncmp(p, "true", 4) == 0 ? p + 4 : NULL;
	case 'f':
		return strncmp(p, "false", 5) == 0 ? p + 5 : NULL;
	case 'n':
		return strncmp(p, "null", 4) == 0 ? p + 4 : NULL;
	default:
		return json_skip_number(p);
	}
}

/* Where the value of the top-level member key starts, or NULL when the
 * object has no such member or json is not one well-formed object */
static const char *json_find_value(const char *json, const char *key)
{
	const char *p, *value = NULL;

	if (json == NULL || key == NULL)
		return NULL;
	p = json_skip_ws(json);
	if (*p != '{')
		return NULL;
	p = json_walk_object(p, 1, key, &value);
	if (p == NULL || *json_skip_ws(p) != '\0')
		return NULL;
	return value;
}

/* The string value of a top-level member, unescaped; NULL when there is
 * none or it is no string */
char *fe_web_json_get_string(const char *json, const char *key)
{
	const char *value, *end;
	char *escaped, *result;

	value = json_find_value(json, key);
	if (value == NULL || *value != '"')
		return NULL;
	end = json_skip_string(value);
	escaped = g_strndup(value + 1, end - value - 2);
	result = fe_web_json_unescape(escaped);
	g_free(escaped);
	return result;
}

/* The integer value of a top-level member (true is 1, false 0) */
int fe_web_json_get_int(const char *json, const char *key, int default_value)
{
	const char *value;
	char *end;
	long number;

	value = json_find_value(json, key);
	if (value == NULL)
		return default_value;

	/* JSON booleans, as server_list sends them (use_tls, autoconnect) */
	if (strncmp(value, "true", 4) == 0)
		return 1;
	if (strncmp(value, "false", 5) == 0)
		return 0;

	/* Parse integer: one that does not fit an int is not taken
	 * (sscanf("%d") is undefined for it) */
	errno = 0;
	number = strtol(value, &end, 10);
	if (end == value || errno == ERANGE || number < INT_MIN || number > INT_MAX)
		return default_value;
	return (int) number;
}

/* Does the top-level object have a member key */
int fe_web_json_has_key(const char *json, const char *key)
{
	return json_find_value(json, key) != NULL;
}

/* Append "key":"value", (escaped) or "key":null, */
static void json_append_string(GString *json, const char *key, const char *value)
{
	char *escaped;

	if (value == NULL) {
		g_string_append_printf(json, "\"%s\":null,", key);
		return;
	}
	escaped = fe_web_escape_json(value);
	g_string_append_printf(json, "\"%s\":\"%s\",", key, escaped);
	g_free(escaped);
}

/* Build JSON string for a network (IRC_CHATNET_REC) */
GString *fe_web_build_network_json(IRC_CHATNET_REC *rec)
{
	GString *json;

	if (rec == NULL) {
		return NULL;
	}

	json = g_string_new("{");

	/* Required fields */
	json_append_string(json, "name", rec->name != NULL ? rec->name : "");
	g_string_append(json, "\"chat_type\":\"IRC\",");

	/* Optional fields */
	json_append_string(json, "nick", rec->nick);
	json_append_string(json, "alternate_nick", rec->alternate_nick);
	json_append_string(json, "username", rec->username);
	json_append_string(json, "realname", rec->realname);
	json_append_string(json, "own_host", rec->own_host);
	/* masked: it often holds a NickServ password (network_add keeps
	 * the stored value when it gets the mask back) */
	if (rec->autosendcmd != NULL && *rec->autosendcmd != '\0') {
		g_string_append(json, "\"autosendcmd\":\"" FE_WEB_MASK "\",");
	} else {
		json_append_string(json, "autosendcmd", rec->autosendcmd);
	}
	json_append_string(json, "usermode", rec->usermode);

	/* SASL fields */
	json_append_string(json, "sasl_mechanism", rec->sasl_mechanism);
	json_append_string(json, "sasl_username", rec->sasl_username);

	/* Mask password - never send actual password */
	if (rec->sasl_password != NULL) {
		g_string_append(json, "\"sasl_password\":\"" FE_WEB_MASK "\",");
	} else {
		g_string_append(json, "\"sasl_password\":null,");
	}

	/* Numeric settings */
	g_string_append_printf(json, "\"max_kicks\":%d,", rec->max_kicks);
	g_string_append_printf(json, "\"max_msgs\":%d,", rec->max_msgs);
	g_string_append_printf(json, "\"max_modes\":%d,", rec->max_modes);
	g_string_append_printf(json, "\"max_whois\":%d,", rec->max_whois);
	g_string_append_printf(json, "\"max_cmds_at_once\":%d,", rec->max_cmds_at_once);
	g_string_append_printf(json, "\"cmd_queue_speed\":%d,", rec->cmd_queue_speed);
	g_string_append_printf(json, "\"max_query_chans\":%d", rec->max_query_chans);

	g_string_append(json, "}");

	return json;
}

/* Build JSON string for a server (IRC_SERVER_SETUP_REC) */
GString *fe_web_build_server_json(IRC_SERVER_SETUP_REC *rec)
{
	GString *json;

	if (rec == NULL) {
		return NULL;
	}

	json = g_string_new("{");

	/* Required fields */
	json_append_string(json, "address", rec->address != NULL ? rec->address : "");
	g_string_append_printf(json, "\"port\":%d,", rec->port);

	/* Optional fields */
	json_append_string(json, "chatnet", rec->chatnet);

	/* Password - never send actual password */
	if (rec->password != NULL) {
		g_string_append(json, "\"password\":\"" FE_WEB_MASK "\",");
	} else {
		g_string_append(json, "\"password\":null,");
	}

	/* Boolean flags */
	g_string_append_printf(json, "\"autoconnect\":%s,", rec->autoconnect ? "true" : "false");
	g_string_append_printf(json, "\"use_tls\":%s,", rec->use_tls ? "true" : "false");
	g_string_append_printf(json, "\"tls_verify\":%s,", rec->tls_verify ? "true" : "false");

	/* TLS certificate fields */
	json_append_string(json, "tls_cert", rec->tls_cert);
	json_append_string(json, "tls_pkey", rec->tls_pkey);

	/* Mask TLS password */
	if (rec->tls_pass != NULL) {
		g_string_append(json, "\"tls_pass\":\"" FE_WEB_MASK "\",");
	} else {
		g_string_append(json, "\"tls_pass\":null,");
	}

	json_append_string(json, "tls_cafile", rec->tls_cafile);
	json_append_string(json, "tls_capath", rec->tls_capath);
	json_append_string(json, "tls_ciphers", rec->tls_ciphers);
	json_append_string(json, "tls_pinned_cert", rec->tls_pinned_cert);
	json_append_string(json, "tls_pinned_pubkey", rec->tls_pinned_pubkey);

	/* Network binding */
	json_append_string(json, "own_host", rec->own_host);

	/* Numeric settings */
	g_string_append_printf(json, "\"family\":%d,", rec->family);
	g_string_append_printf(json, "\"max_cmds_at_once\":%d,", rec->max_cmds_at_once);
	g_string_append_printf(json, "\"cmd_queue_speed\":%d,", rec->cmd_queue_speed);
	g_string_append_printf(json, "\"max_query_chans\":%d,", rec->max_query_chans);
	g_string_append_printf(json, "\"starttls\":%d,", rec->starttls);

	/* More boolean flags */
	g_string_append_printf(json, "\"no_cap\":%s,", rec->no_cap ? "true" : "false");
	g_string_append_printf(json, "\"no_proxy\":%s,", rec->no_proxy ? "true" : "false");
	g_string_append_printf(json, "\"last_failed\":%s,", rec->last_failed ? "true" : "false");
	g_string_append_printf(json, "\"banned\":%s,", rec->banned ? "true" : "false");
	g_string_append_printf(json, "\"dns_error\":%s", rec->dns_error ? "true" : "false");

	g_string_append(json, "}");

	return json;
}

/* Build command result JSON */
GString *fe_web_build_command_result_json(gboolean success, const char *message,
                                          const char *error_code)
{
	GString *json;
	char *escaped_msg;

	json = g_string_new("{");
	g_string_append_printf(json, "\"success\":%s,", success ? "true" : "false");

	if (message != NULL) {
		escaped_msg = fe_web_escape_json(message);
		g_string_append_printf(json, "\"message\":\"%s\",", escaped_msg);
		g_free(escaped_msg);
	} else {
		g_string_append(json, "\"message\":null,");
	}

	if (error_code != NULL) {
		char *escaped_code = fe_web_escape_json(error_code);
		g_string_append_printf(json, "\"error_code\":\"%s\"", escaped_code);
		g_free(escaped_code);
	} else {
		g_string_append(json, "\"error_code\":null");
	}

	g_string_append(json, "}");

	return json;
}
