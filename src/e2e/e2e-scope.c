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

/*
 The label from a chatnet name or tag, which may hold anything: C0 and C1
 controls, DEL and '|' become '_', and so does every byte that is not part
 of a valid UTF-8 character, so the label is valid UTF-8 and never holds
 the separator.
*/
static char *label_from(const char *name)
{
	GString *out = g_string_sized_new(strlen(name));
	const char *p = name;

	while (*p != '\0') {
		gunichar c = g_utf8_get_char_validated(p, -1);

		if (!g_unichar_validate(c)) { /* also (gunichar) -1 and -2 */
			g_string_append_c(out, '_');
			p++;
			continue;
		}
		if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == '|')
			g_string_append_c(out, '_');
		else
			g_string_append_len(out, p, g_utf8_next_char(p) - p);
		p = g_utf8_next_char(p);
	}
	return g_string_free(out, FALSE);
}

void e2e_net_init(E2E_NET *net, const char *chatnet, const char *tag, const char *casemapping,
                  gboolean legacy_ok)
{
	if (chatnet != NULL && *chatnet != '\0')
		net->label = label_from(chatnet);
	else if (tag != NULL && *tag != '\0')
		net->label = label_from(tag);
	else
		net->label = NULL;
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

/*
 Invisible characters, dropped as NFKC_Casefold drops Default_Ignorable
 code points: format characters (Cf: soft hyphen, zero width space and
 joiners, bidi marks, word joiner, BOM, tags), the variation selectors,
 the Mongolian free variation selectors, the Khmer inherent vowels, the
 combining grapheme joiner and the Hangul fillers.
*/
static gboolean is_ignorable(gunichar c)
{
	return g_unichar_type(c) == G_UNICODE_FORMAT || (c >= 0xfe00 && c <= 0xfe0f) ||
	       (c >= 0xe0100 && c <= 0xe01ef) || (c >= 0x180b && c <= 0x180d) || c == 0x180f ||
	       c == 0x17b4 || c == 0x17b5 || c == 0x34f || c == 0x115f || c == 0x1160 || c == 0x3164 || c == 0xffa0;
}

static char *drop_ignorables(const char *s)
{
	GString *out = g_string_sized_new(strlen(s));
	const char *p;

	for (p = s; *p != '\0'; p = g_utf8_next_char(p)) {
		if (!is_ignorable(g_utf8_get_char(p)))
			g_string_append_len(out, p, g_utf8_next_char(p) - p);
	}
	return g_string_free(out, FALSE);
}

/* full case folding, with GLib's Cherokee cycles closed */
static char *casefold_full(const char *s)
{
	char *folded = g_utf8_casefold(s, -1);
	char *result = fold_cycles(folded);

	g_free(folded);
	return result;
}

/* g_utf8_normalize(s, mode), freeing s; NULL for NULL or if GLib refuses */
static char *normalize_take(char *s, GNormalizeMode mode)
{
	char *result = s != NULL ? g_utf8_normalize(s, -1, mode) : NULL;

	g_free(s);
	return result;
}

/* casefold_full(s), freeing s; NULL for NULL */
static char *casefold_take(char *s)
{
	char *result = s != NULL ? casefold_full(s) : NULL;

	g_free(s);
	return result;
}

/*
 U+0345 (ypogegrammeni) is the one combining mark whose case fold is a base
 character, iota. NFD sorts it after the other marks of its letter, a
 server that case folds first puts the iota before them, so the marks end
 on the letter in one and on the iota in the other. Servers disagree, so
 the run of marks directly before and directly after every iota is
 dropped. That over-folds only marks next to a Greek iota.
*/
static char *drop_marks_by_iota(const char *s)
{
	glong i, j, len;
	gunichar *cps = g_utf8_to_ucs4_fast(s, -1, &len);
	gboolean *drop = g_new0(gboolean, len + 1);
	GString *out = g_string_sized_new(strlen(s));

	for (i = 0; i < len; i++) {
		if (cps[i] != 0x3b9)
			continue;
		for (j = i - 1; j >= 0 && g_unichar_ismark(cps[j]); j--)
			drop[j] = TRUE;
		for (j = i + 1; j < len && g_unichar_ismark(cps[j]); j++)
			drop[j] = TRUE;
	}
	for (i = 0; i < len; i++) {
		if (!drop[i])
			g_string_append_unichar(out, cps[i]);
	}
	g_free(cps);
	g_free(drop);
	return g_string_free(out, FALSE);
}

/* drop_marks_by_iota(s), freeing s; NULL for NULL */
static char *drop_marks_by_iota_take(char *s)
{
	char *result = s != NULL ? drop_marks_by_iota(s) : NULL;

	g_free(s);
	return result;
}

/*
 The Turkish i. Full case folding takes U+0130 to i U+0307 and keeps
 U+0131, while servers that map case per code point (C towlower and
 towupper, Go's unicode.ToLower and ToUpper, an upper-case irccmp) take
 U+0130 to plain i and U+0131 to I. So U+0131 becomes i, and a U+0307 in
 the run of marks directly after an i goes: dotted, dotless and plain i
 are one letter.
*/
static char *fold_turkish_i(const char *s)
{
	GString *out = g_string_sized_new(strlen(s));
	gboolean after_i = FALSE;
	const char *p;

	for (p = s; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c == 0x131)
			c = 'i';
		if (after_i && g_unichar_ismark(c)) {
			if (c != 0x307)
				g_string_append_unichar(out, c);
			continue;
		}
		after_i = c == 'i';
		g_string_append_unichar(out, c);
	}
	return g_string_free(out, FALSE);
}

/* fold_turkish_i(s), freeing s; NULL for NULL */
static char *fold_turkish_i_take(char *s)
{
	char *result = s != NULL ? fold_turkish_i(s) : NULL;

	g_free(s);
	return result;
}

/*
 Unicode's compatibility caseless match (D146),
 NFKD(casefold(NFKD(casefold(NFD(x))))), with the invisible characters
 dropped first and NFKC at the end for one representative. Folding a
 composed form instead (NFKC first) under-folds: U+1FB3 U+0342 and
 U+1FBC U+0342 compose around U+0345 differently and fold apart, and
 casefold leaves marks out of canonical order (U+01F0 U+0338 folds to
 j U+030C U+0338), which only a normalisation after it puts right.
 Then the marks around an iota go (drop_marks_by_iota), the Turkish i
 is merged with i (fold_turkish_i), and NFKC.
*/
static char *fold_unicode(const char *name)
{
	char *s;

	if (!g_utf8_validate(name, -1, NULL))
		return NULL;
	s = drop_ignorables(name);
	s = normalize_take(s, G_NORMALIZE_NFD);
	s = casefold_take(s);
	s = normalize_take(s, G_NORMALIZE_NFKD);
	s = casefold_take(s);
	s = normalize_take(s, G_NORMALIZE_NFKD);
	s = drop_marks_by_iota_take(s);
	s = fold_turkish_i_take(s);
	s = normalize_take(s, G_NORMALIZE_NFKC);
	if (s == NULL)
		return NULL;
	/* the specials are ASCII, so this keeps the string valid UTF-8 */
	fold_bytes(s, E2E_CASEMAP_RFC1459);
	return s;
}

static gboolean is_ascii(const char *s)
{
	for (; *s != '\0'; s++) {
		if ((unsigned char) *s >= 0x80)
			return FALSE;
	}
	return TRUE;
}

char *e2e_casefold_ex(const char *name, E2E_CASEMAP map, gboolean *exact)
{
	char *result;

	if (exact != NULL)
		*exact = FALSE;
	if (name == NULL)
		return NULL;
	if (map == E2E_CASEMAP_UNICODE && is_ascii(name))
		map = E2E_CASEMAP_RFC1459; /* the Unicode fold of ASCII, faster */
	if (map == E2E_CASEMAP_UNICODE) {
		result = fold_unicode(name);
		if (result != NULL) {
			if (exact != NULL)
				*exact = TRUE;
			return result;
		}
		/* not UTF-8: no Unicode fold can be decided, keep rfc1459's
		   and say so */
		result = g_strdup(name);
		fold_bytes(result, E2E_CASEMAP_RFC1459);
		return result;
	}
	result = g_strdup(name);
	fold_bytes(result, map);
	if (exact != NULL)
		*exact = TRUE;
	return result;
}

char *e2e_casefold(const char *name, E2E_CASEMAP map)
{
	return e2e_casefold_ex(name, map, NULL);
}

char *e2e_ctx_scoped(const E2E_NET *net, const char *wire)
{
	if (wire == NULL || strchr(wire, E2E_SCOPE_SEP) != NULL)
		return NULL;
	if (net == NULL)
		return g_strdup(wire);
	if (net->label == NULL)
		return NULL;
	return g_strdup_printf("%s%c%s", net->label, E2E_SCOPE_SEP, wire);
}

char *e2e_ctx_rescope(const char *like, const char *wire)
{
	const char *sep;

	if (wire == NULL || strchr(wire, E2E_SCOPE_SEP) != NULL)
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
