/*
 e2e-scope.c : network scope of contexts and CASEMAPPING folding

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The superset fold (E2E_CASEMAP_UNICODE) is for servers whose CASEMAPPING
 is not one of the three exact ones. Folding more than the server does
 can only make two names look like one channel - a refusal, or a
 decryption attempt that the AEAD rejects - while folding less could
 treat one channel as two, so every step only adds equalities.
*/

#include "e2e-scope.h"

#include <string.h>

void e2e_net_init(E2E_NET *net, const char *chatnet, const char *tag, const char *casemapping,
                  gboolean legacy_ok)
{
	const char *name;
	char *p;

	if (chatnet != NULL && *chatnet != '\0')
		name = chatnet;
	else if (tag != NULL && *tag != '\0')
		name = tag;
	else
		name = NULL;

	net->label = g_strdup(name);
	for (p = net->label; p != NULL && *p != '\0'; p++) {
		if ((unsigned char) *p < 0x20 || *p == '|')
			*p = '_';
	}
	net->casemap = e2e_casemap_parse(casemapping);
	net->legacy_ok = legacy_ok;
}

void e2e_net_clear(E2E_NET *net)
{
	g_free(net->label);
	net->label = NULL;
}

E2E_CASEMAP e2e_casemap_parse(const char *value)
{
	if (value == NULL)
		return E2E_CASEMAP_RFC1459;
	if (g_ascii_strcasecmp(value, "rfc1459") == 0)
		return E2E_CASEMAP_RFC1459;
	if (g_ascii_strcasecmp(value, "strict-rfc1459") == 0)
		return E2E_CASEMAP_STRICT_RFC1459;
	if (g_ascii_strcasecmp(value, "ascii") == 0)
		return E2E_CASEMAP_ASCII;
	return E2E_CASEMAP_UNICODE;
}

/* the exact maps, byte by byte, in place; UNICODE takes RFC1459's */
static void fold_bytes(char *s, E2E_CASEMAP map)
{
	for (; *s != '\0'; s++) {
		char c = *s;

		if (c >= 'A' && c <= 'Z')
			*s = c - 'A' + 'a';
		else if (map == E2E_CASEMAP_ASCII)
			continue;
		else if (c == '[')
			*s = '{';
		else if (c == ']')
			*s = '}';
		else if (c == '\\')
			*s = '|';
		else if (c == '~' && map != E2E_CASEMAP_STRICT_RFC1459)
			*s = '^';
	}
}

/* the full case fold of one code point, when it is one code point */
static gboolean fold_single(gunichar c, gunichar *out)
{
	char buf[8];
	char *folded;
	int len;
	gboolean single;

	len = g_unichar_to_utf8(c, buf);
	folded = g_utf8_casefold(buf, len);
	single = *folded != '\0' && *g_utf8_next_char(folded) == '\0';
	if (single)
		*out = g_utf8_get_char(folded);
	g_free(folded);
	return single;
}

/*
 GLib's g_utf8_casefold() folds what CaseFolding.txt lists and lowercases
 everything else. For Cherokee, CaseFolding.txt folds the small letters to
 the capitals while the fallback lowercases the capitals, so the two fold
 into each other and never meet. A code point c whose fold is a single
 other code point f folding back to c is replaced by the smaller of c and
 f, which is CaseFolding.txt's own target for these pairs.
*/
static char *fold_cycles(const char *s)
{
	GString *out = g_string_sized_new(strlen(s));
	const char *p;

	for (p = s; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p), f, back;

		if (c >= 0x80 && fold_single(c, &f) && f != c && fold_single(f, &back) &&
		    back == c)
			c = MIN(c, f);
		g_string_append_unichar(out, c);
	}
	return g_string_free(out, FALSE);
}

static char *fold_unicode(const char *name)
{
	char *nfkc, *folded, *result;

	if (!g_utf8_validate(name, -1, NULL))
		return NULL;
	nfkc = g_utf8_normalize(name, -1, G_NORMALIZE_NFKC);
	if (nfkc == NULL)
		return NULL;
	folded = g_utf8_casefold(nfkc, -1);
	g_free(nfkc);
	result = fold_cycles(folded);
	g_free(folded);
	/* the specials are ASCII, so this keeps the string valid UTF-8 */
	fold_bytes(result, E2E_CASEMAP_RFC1459);
	return result;
}

char *e2e_casefold(const char *name, E2E_CASEMAP map)
{
	char *result;

	if (name == NULL)
		return NULL;
	if (map == E2E_CASEMAP_UNICODE) {
		result = fold_unicode(name);
		if (result != NULL)
			return result;
		/* not UTF-8: no Unicode fold can be decided, keep rfc1459's */
		map = E2E_CASEMAP_RFC1459;
	}
	result = g_strdup(name);
	fold_bytes(result, map);
	return result;
}

char *e2e_ctx_scoped(const E2E_NET *net, const char *wire)
{
	if (wire == NULL)
		return NULL;
	if (net == NULL || net->label == NULL)
		return g_strdup(wire);
	return g_strdup_printf("%s%c%s", net->label, E2E_SCOPE_SEP, wire);
}

char *e2e_ctx_rescope(const char *like, const char *wire)
{
	const char *sep;

	if (wire == NULL)
		return NULL;
	sep = like != NULL ? strchr(like, E2E_SCOPE_SEP) : NULL;
	if (sep == NULL)
		return g_strdup(wire);
	return g_strdup_printf("%.*s%c%s", (int) (sep - like), like, E2E_SCOPE_SEP, wire);
}

const char *e2e_ctx_wire(const char *ctx)
{
	const char *sep;

	if (ctx == NULL)
		return NULL;
	sep = strchr(ctx, E2E_SCOPE_SEP);
	return sep != NULL ? sep + 1 : ctx;
}

char *e2e_ctx_network(const char *ctx)
{
	const char *sep;

	if (ctx == NULL)
		return NULL;
	sep = strchr(ctx, E2E_SCOPE_SEP);
	return sep != NULL ? g_strndup(ctx, sep - ctx) : NULL;
}

gboolean e2e_ctx_on_net(const char *ctx, const E2E_NET *net)
{
	const char *sep;
	size_t len;

	if (ctx == NULL)
		return FALSE;
	sep = strchr(ctx, E2E_SCOPE_SEP);
	if (sep == NULL)
		return TRUE;
	if (net == NULL || net->label == NULL)
		return FALSE;
	len = sep - ctx;
	return strlen(net->label) == len && memcmp(ctx, net->label, len) == 0;
}

char *e2e_ctx_display(const char *ctx)
{
	const char *sep;

	if (ctx == NULL)
		return NULL;
	sep = strchr(ctx, E2E_SCOPE_SEP);
	if (sep == NULL)
		return g_strdup(ctx);
	return g_strdup_printf("%.*s:%s", (int) (sep - ctx), ctx, sep + 1);
}
