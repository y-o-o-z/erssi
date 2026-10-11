/*
 test-e2e-scope.c : network scope of contexts and CASEMAPPING folding

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <glib.h>
#include <string.h>

#include <irssi/src/e2e/e2e-json.h>
#include <irssi/src/e2e/e2e-scope.h>

static const E2E_CASEMAP all_maps[] = {
	E2E_CASEMAP_RFC1459,
	E2E_CASEMAP_STRICT_RFC1459,
	E2E_CASEMAP_ASCII,
	E2E_CASEMAP_UNICODE,
};

static void assert_fold(const char *a, const char *b, E2E_CASEMAP map, gboolean same)
{
	char *fa, *fb;

	fa = e2e_casefold(a, map);
	fb = e2e_casefold(b, map);
	g_assert_nonnull(fa);
	g_assert_nonnull(fb);
	if (same)
		g_assert_cmpstr(fa, ==, fb);
	else
		g_assert_cmpstr(fa, !=, fb);
	g_free(fa);
	g_free(fb);
}

#define SAME(a, b, m) assert_fold((a), (b), (m), TRUE)
#define DIFF(a, b, m) assert_fold((a), (b), (m), FALSE)

static void test_casemap_parse(void)
{
	g_assert_cmpint(e2e_casemap_parse(NULL), ==, E2E_CASEMAP_RFC1459);
	g_assert_cmpint(e2e_casemap_parse("ascii"), ==, E2E_CASEMAP_ASCII);
	g_assert_cmpint(e2e_casemap_parse("RFC1459"), ==, E2E_CASEMAP_RFC1459);
	g_assert_cmpint(e2e_casemap_parse("rfc1459"), ==, E2E_CASEMAP_RFC1459);
	g_assert_cmpint(e2e_casemap_parse("strict-rfc1459"), ==, E2E_CASEMAP_STRICT_RFC1459);
	g_assert_cmpint(e2e_casemap_parse("rfc7613"), ==, E2E_CASEMAP_UNICODE);
	g_assert_cmpint(e2e_casemap_parse("rfc8265"), ==, E2E_CASEMAP_UNICODE);
	g_assert_cmpint(e2e_casemap_parse("utf8"), ==, E2E_CASEMAP_UNICODE);
	g_assert_cmpint(e2e_casemap_parse("future-thing"), ==, E2E_CASEMAP_UNICODE);
	/* an empty or truncated value is unknown, so it gets the superset fold */
	g_assert_cmpint(e2e_casemap_parse(""), ==, E2E_CASEMAP_UNICODE);
	g_assert_cmpint(e2e_casemap_parse("rfc145"), ==, E2E_CASEMAP_UNICODE);
	g_assert_cmpint(e2e_casemap_parse("ascii2"), ==, E2E_CASEMAP_UNICODE);
}

/* every string the casefold test uses, for the idempotence check */
static const char *const fold_inputs[] = {
	"#Secret", "#secret", "#a[b]\\~", "#a{b}|^", "#a[b", "#a{b", "#a[", "#a{",
	"#a~", "#a^",
	"#\xc5\xbb" "aba", "#\xc5\xbc" "aba", "#z\xcc\x87" "aba", "#\xff",
	"#A\xff[", "#\xef\xbc\xbb" "X",
	"#\xe1\x8e\xa0", "#\xea\xad\xb0", "#\xe1\x8f\xb8", "#\xe1\x8f\xb0",
	"#Stra\xc3\x9f" "e", "#STRASSE", "#\xc4\xb0", "#i\xcc\x87",
	"#\xc7\xb0\xcc\xb8", "#J\xcc\xb8\xcc\x8c", "#\xc4\xb0\xcc\xa7", "#i\xcc\xa7\xcc\x87",
	"#a\xc2\xad" "b", "#a\xcd\x8f\xcc\x81\xcc\x80", "#\xe1\xbe\xb3\xcd\x82", "#\xe1\xbe\xbc\xcd\x82", "",
};

static void test_casefold(void)
{
	gsize i, j;
	char *s;

	SAME("#Secret", "#secret", E2E_CASEMAP_ASCII);
	SAME("#a[b]\\~", "#a{b}|^", E2E_CASEMAP_RFC1459);
	DIFF("#a[b", "#a{b", E2E_CASEMAP_ASCII);
	SAME("#a[", "#a{", E2E_CASEMAP_STRICT_RFC1459);
	DIFF("#a~", "#a^", E2E_CASEMAP_STRICT_RFC1459);
	SAME("#\xc5\xbb" "aba", "#\xc5\xbc" "aba", E2E_CASEMAP_UNICODE); /* #Żaba / #żaba */
	DIFF("#\xc5\xbb" "aba", "#\xc5\xbc" "aba", E2E_CASEMAP_ASCII);
	DIFF("#\xc5\xbb" "aba", "#\xc5\xbc" "aba", E2E_CASEMAP_RFC1459);
	SAME("#z\xcc\x87" "aba", "#\xc5\xbc" "aba", E2E_CASEMAP_UNICODE); /* NFD vs NFC */
	SAME("#\xff", "#\xff", E2E_CASEMAP_UNICODE); /* invalid UTF-8: byte-wise, no crash */

	/* each exact map, letter by letter */
	SAME("#ABCXYZ", "#abcxyz", E2E_CASEMAP_ASCII);
	DIFF("#a]", "#a}", E2E_CASEMAP_ASCII);
	DIFF("#a\\", "#a|", E2E_CASEMAP_ASCII);
	DIFF("#a~", "#a^", E2E_CASEMAP_ASCII);
	SAME("#a]", "#a}", E2E_CASEMAP_STRICT_RFC1459);
	SAME("#a\\", "#a|", E2E_CASEMAP_STRICT_RFC1459);
	SAME("#a~", "#a^", E2E_CASEMAP_RFC1459);
	/* the exact maps never touch bytes above 0x7f */
	DIFF("#\xc3\x80", "#\xc3\xa0", E2E_CASEMAP_RFC1459);

	/* the superset fold: rfc1459's specials, NFKC, full case folding */
	SAME("#a[b]\\~", "#A{B}|^", E2E_CASEMAP_UNICODE);
	SAME("#\xef\xbc\xbb" "X", "#{x", E2E_CASEMAP_UNICODE); /* fullwidth [ */
	SAME("#Stra\xc3\x9f" "e", "#STRASSE", E2E_CASEMAP_UNICODE);
	/* invalid UTF-8 still gets rfc1459's byte-wise fold */
	SAME("#A\xff[", "#a\xff{", E2E_CASEMAP_UNICODE);
	/* Cherokee: CaseFolding.txt folds the small letters to the capitals,
	   GLib's fallback folds the capitals to the small letters */
	SAME("#\xe1\x8e\xa0", "#\xea\xad\xb0", E2E_CASEMAP_UNICODE); /* U+13A0 / U+AB70 */
	SAME("#\xe1\x8f\xb0", "#\xe1\x8f\xb8", E2E_CASEMAP_UNICODE); /* U+13F0 / U+13F8 */
	SAME("#\xc4\xb0", "#i\xcc\x87", E2E_CASEMAP_UNICODE); /* U+0130: full case folding */
	/* the representative of a Cherokee pair is CaseFolding.txt's, the capital */
	s = e2e_casefold("#\xea\xad\xb0", E2E_CASEMAP_UNICODE);
	g_assert_cmpstr(s, ==, "#\xe1\x8e\xa0");
	g_free(s);
	s = e2e_casefold("#\xe1\x8f\xb8", E2E_CASEMAP_UNICODE);
	g_assert_cmpstr(s, ==, "#\xe1\x8f\xb0");
	g_free(s);
	/* casefold output is normalised again: these are one name under full
	   case folding but come out of casefold in different mark orders */
	SAME("#\xc7\xb0\xcc\xb8", "#J\xcc\xb8\xcc\x8c", E2E_CASEMAP_UNICODE);  /* U+01F0 U+0338 */
	SAME("#\xc4\xb0\xcc\xa7", "#i\xcc\xa7\xcc\x87", E2E_CASEMAP_UNICODE);  /* U+0130 U+0327 */
	/* folding a composed form under-folds: U+0345 joins another base */
	SAME("#\xe1\xbe\xb3\xcd\x82", "#\xe1\xbe\xbc\xcd\x82", E2E_CASEMAP_UNICODE); /* U+1FB3/U+1FBC U+0342 */
	SAME("#\xe1\xbe\xb7", "#\xce\x91\xcd\x82\xce\xb9", E2E_CASEMAP_UNICODE);    /* U+1FB7 */
	/* servers that casefold first put U+0345's iota before the other
	   marks, NFD puts it after them: the marks next to an iota go */
	SAME("#\xe1\xbe\x88\xcc\xaf", "#\xe1\xbc\x88\xce\x99\xcc\xaf", E2E_CASEMAP_UNICODE); /* U+1F88 U+032F */
	SAME("#\xe1\xbf\xbc\xcc\xa8", "#\xcf\x89\xce\xb9\xcc\xa8", E2E_CASEMAP_UNICODE);     /* U+1FFC U+0328 */
	SAME("#\xcf\x80\xce\xbf\xce\xaf\xce\xb7\xce\xbc\xce\xb1", "#\xcf\x80\xce\xbf\xce\xb9\xce\xb7\xce\xbc\xce\xb1",
	     E2E_CASEMAP_UNICODE); /* the over-fold this costs */
	DIFF("#\xce\xb1\xcc\x81", "#\xce\xb1", E2E_CASEMAP_UNICODE); /* marks elsewhere stay */
	SAME("#\xce\xb9\xcc\x81\xcc\x80", "#\xce\xb9", E2E_CASEMAP_UNICODE);  /* marks at the end */
	SAME("\xcc\x81\xcc\x80\xce\xb9", "\xce\xb9", E2E_CASEMAP_UNICODE);    /* marks at the start */
	SAME("#a\xcc\x81\xcd\x85", "#a\xce\xb9", E2E_CASEMAP_UNICODE);        /* a U+0345 */
	DIFF("#a\xcc\x81\xcd\x85", "#\xce\xb9", E2E_CASEMAP_UNICODE);         /* the base stays */
	s = e2e_casefold("#\xc7\xb0\xcc\xb8", E2E_CASEMAP_UNICODE);
	g_assert_cmpstr(s, ==, "#\xc7\xb0\xcc\xb8"); /* NFKC of j U+030C U+0338 */
	g_free(s);
	/* a plain result for the exact maps too */
	s = e2e_casefold("#A[]\\~\xc3\x80", E2E_CASEMAP_RFC1459);
	g_assert_cmpstr(s, ==, "#a{}|^\xc3\x80");
	g_free(s);

	g_assert_null(e2e_casefold(NULL, E2E_CASEMAP_RFC1459));

	/* idempotence: fold(fold(x)) == fold(x) */
	for (i = 0; i < G_N_ELEMENTS(fold_inputs); i++) {
		for (j = 0; j < G_N_ELEMENTS(all_maps); j++) {
			char *once = e2e_casefold(fold_inputs[i], all_maps[j]);
			char *twice = e2e_casefold(once, all_maps[j]);

			g_assert_cmpstr(once, ==, twice);
			g_free(once);
			g_free(twice);
		}
	}
}

/* invisible characters the superset fold drops, as NFKC_Casefold does */
static const char *const ignorables[] = {
	"\xc2\xad",         /* U+00AD soft hyphen (Cf) */
	"\xd8\x80",         /* U+0600 (Cf) */
	"\xe2\x80\x8b",     /* U+200B zero width space (Cf) */
	"\xe2\x80\x8d",     /* U+200D zero width joiner (Cf) */
	"\xe2\x80\x8f",     /* U+200F (Cf) */
	"\xe2\x81\xa0",     /* U+2060 word joiner (Cf) */
	"\xef\xbb\xbf",     /* U+FEFF (Cf) */
	"\xf3\xa0\x80\x81", /* U+E0001 language tag (Cf) */
	"\xef\xb8\x80",     /* U+FE00 variation selector 1 */
	"\xef\xb8\x8f",     /* U+FE0F variation selector 16 */
	"\xf3\xa0\x84\x80", /* U+E0100 variation selector 17 */
	"\xf3\xa0\x87\xaf", /* U+E01EF variation selector 256 */
	"\xcd\x8f",         /* U+034F combining grapheme joiner */
	"\xe1\xa0\x8b",     /* U+180B Mongolian free variation selector 1 */
	"\xe1\xa0\x8c",     /* U+180C Mongolian free variation selector 2 */
	"\xe1\xa0\x8d",     /* U+180D Mongolian free variation selector 3 */
	"\xe1\xa0\x8f",     /* U+180F Mongolian free variation selector 4 */
	"\xe1\x9e\xb4",     /* U+17B4 Khmer vowel inherent aq */
	"\xe1\x9e\xb5",     /* U+17B5 Khmer vowel inherent aa */
	"\xe1\x85\x9f",     /* U+115F Hangul choseong filler */
	"\xe1\x85\xa0",     /* U+1160 Hangul jungseong filler */
	"\xe3\x85\xa4",     /* U+3164 Hangul filler */
	"\xef\xbe\xa0",     /* U+FFA0 halfwidth Hangul filler */
};

/* their neighbours outside the ranges are kept */
static const char *const kept_neighbours[] = {
	"\xef\xb8\x90",     /* U+FE10 (NFKC: ',') */
	"\xef\xb7\xbf",     /* U+FDFF */
	"\xf3\xa0\x83\xbf", /* U+E00FF */
	"\xf3\xa0\x87\xb0", /* U+E01F0 */
	"\xcd\x8e",         /* U+034E */
	"\xe1\xa0\x8a",     /* U+180A */
	"\xe1\xa0\x90",     /* U+1810 */
	"\xe1\x9e\xb3",     /* U+17B3 */
	"\xe1\x9e\xb6",     /* U+17B6 */
	"\xe1\x85\x9e",     /* U+115E */
	"\xe1\x85\xa1",     /* U+1161 */
	"\xe3\x85\xa5",     /* U+3165 */
	"\xef\xbe\xa1",     /* U+FFA1 */
};

static gboolean oracle_ignorable(gunichar c)
{
	return g_unichar_type(c) == G_UNICODE_FORMAT || (c >= 0xfe00 && c <= 0xfe0f) ||
	       (c >= 0xe0100 && c <= 0xe01ef) || (c >= 0x180b && c <= 0x180d) || c == 0x180f ||
	       c == 0x17b4 || c == 0x17b5 || c == 0x34f || c == 0x115f || c == 0x1160 || c == 0x3164 || c == 0xffa0;
}

static void test_casefold_ignorables(void)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(ignorables); i++) {
		char *with = g_strconcat("#a", ignorables[i], "b", NULL);
		char *only = g_strconcat("#", ignorables[i], NULL);

		SAME("#ab", with, E2E_CASEMAP_UNICODE);
		SAME("#", only, E2E_CASEMAP_UNICODE);
		DIFF("#ab", with, E2E_CASEMAP_ASCII);
		DIFF("#ab", with, E2E_CASEMAP_RFC1459);
		DIFF("#ab", with, E2E_CASEMAP_STRICT_RFC1459);
		g_free(with);
		g_free(only);
	}
	for (i = 0; i < G_N_ELEMENTS(kept_neighbours); i++) {
		char *with = g_strconcat("#a", kept_neighbours[i], "b", NULL);

		DIFF("#ab", with, E2E_CASEMAP_UNICODE);
		g_free(with);
	}
	SAME("#a\xe1\xa0\x8b" "b", "#ab", E2E_CASEMAP_UNICODE); /* U+180B */
	DIFF("#a\xe1\xa0\x8b" "b", "#ab", E2E_CASEMAP_RFC1459);
	/* a joiner between marks blocks reordering until it is dropped */
	SAME("#a\xcc\x81\xcd\x8f\xcc\x80", "#a\xcc\x81\xcc\x80", E2E_CASEMAP_UNICODE);
	SAME("#\xc3\x81\xcd\x8f", "#a\xcc\x81", E2E_CASEMAP_UNICODE);
}

static void test_casefold_ex(void)
{
	gboolean exact;
	char *s, *t;

	exact = FALSE;
	s = e2e_casefold_ex("#\xc5\xbb" "aba", E2E_CASEMAP_UNICODE, &exact);
	g_assert_true(exact);
	t = e2e_casefold("#\xc5\xbb" "aba", E2E_CASEMAP_UNICODE);
	g_assert_cmpstr(s, ==, t);
	g_assert_cmpstr(s, ==, "#\xc5\xbc" "aba");
	g_free(s);
	g_free(t);

	/* the byte-wise fallback says so, with the same result */
	exact = TRUE;
	s = e2e_casefold_ex("#A\xff[", E2E_CASEMAP_UNICODE, &exact);
	g_assert_false(exact);
	g_assert_cmpstr(s, ==, "#a\xff{");
	t = e2e_casefold("#A\xff[", E2E_CASEMAP_UNICODE);
	g_assert_cmpstr(s, ==, t);
	g_free(s);
	g_free(t);

	/* the exact maps are exact whatever the bytes */
	exact = FALSE;
	s = e2e_casefold_ex("#A\xff[", E2E_CASEMAP_RFC1459, &exact);
	g_assert_true(exact);
	g_assert_cmpstr(s, ==, "#a\xff{");
	g_free(s);
	exact = FALSE;
	s = e2e_casefold_ex("#A", E2E_CASEMAP_ASCII, &exact);
	g_assert_true(exact);
	g_free(s);
	exact = FALSE;
	s = e2e_casefold_ex("#A~", E2E_CASEMAP_STRICT_RFC1459, &exact);
	g_assert_true(exact);
	g_assert_cmpstr(s, ==, "#a~");
	g_free(s);

	/* any byte from 0x80 up is not ASCII: no fast path for it */
	exact = TRUE;
	s = e2e_casefold_ex("#A\x80", E2E_CASEMAP_UNICODE, &exact);
	g_assert_false(exact);
	g_assert_cmpstr(s, ==, "#a\x80");
	g_free(s);

	/* a NULL name decides nothing */
	exact = TRUE;
	g_assert_null(e2e_casefold_ex(NULL, E2E_CASEMAP_UNICODE, &exact));
	g_assert_false(exact);
	exact = TRUE;
	g_assert_null(e2e_casefold_ex(NULL, E2E_CASEMAP_ASCII, &exact));
	g_assert_false(exact);

	/* exact may be NULL */
	s = e2e_casefold_ex("#\xff", E2E_CASEMAP_UNICODE, NULL);
	g_assert_cmpstr(s, ==, "#\xff");
	g_free(s);
}

#define RANDOM_SEED       0x52504532u
#define RANDOM_ITERATIONS 100000

/* a code point from pools that stress normalisation and case folding */
static gunichar random_code_point(GRand *rand)
{
	static const char ascii[] = "AZaz09[]\\~{}|^#-";
	static const gunichar special[] = {
		0x130, 0x131, 0x1f0, 0xdf, 0x1e9e, 0x3a3, 0x3c2, 0x345, 0x390, 0x1fb3,
		0x13a0, 0x13f5, 0x13f8, 0xab70, 0xabbf, 0x1c4, 0x1c5, 0x1c6, 0xfb00, 0xfb03,
		0x212a, 0x212b, 0x2126, 0x1e9b, 0x149, 0x17f, 0xb5, 0x3d0, 0x10400, 0x10428,
	};
	static const struct { gunichar lo, hi; } ranges[] = {
		{ 0xc0, 0x24f },       /* Latin letters */
		{ 0x300, 0x36f },      /* combining marks */
		{ 0x327, 0x338 },      /* cedilla .. long solidus overlay */
		{ 0x370, 0x3ff },      /* Greek */
		{ 0x400, 0x4ff },      /* Cyrillic */
		{ 0x1100, 0x11ff },    /* Hangul jamo */
		{ 0xac00, 0xd7a3 },    /* Hangul syllables */
		{ 0x1e00, 0x1fff },    /* Latin extended additional, Greek extended */
		{ 0xff01, 0xff5e },    /* fullwidth */
		{ 0x2160, 0x217f },    /* Roman numerals */
		{ 0x24b6, 0x24e9 },    /* circled letters */
		{ 0x1d400, 0x1d7ff },  /* mathematical letters */
		{ 0x10a0, 0x10ff },    /* Georgian */
	};
	gunichar c;

	switch (g_rand_int_range(rand, 0, 6)) {
	case 0:
		return ascii[g_rand_int_range(rand, 0, sizeof(ascii) - 1)];
	case 1:
		return special[g_rand_int_range(rand, 0, G_N_ELEMENTS(special))];
	case 2: {
		const char *s = ignorables[g_rand_int_range(rand, 0, G_N_ELEMENTS(ignorables))];

		return g_utf8_get_char(s);
	}
	case 3:
	case 4: {
		int r = g_rand_int_range(rand, 0, G_N_ELEMENTS(ranges));

		return g_rand_int_range(rand, ranges[r].lo, ranges[r].hi + 1);
	}
	default:
		do
			c = g_rand_int_range(rand, 0x20, 0x110000);
		while (!g_unichar_validate(c));
		return c;
	}
}

/* the same name another way: other case per letter, rfc1459 specials,
   another normalisation form, invisible characters added */
static char *variant_of(GRand *rand, const char *s)
{
	static const GNormalizeMode modes[] = {
		G_NORMALIZE_NFD, G_NORMALIZE_NFC, G_NORMALIZE_NFKD, G_NORMALIZE_NFKC,
	};
	GString *out = g_string_new(NULL);
	const char *p;
	char *normal;
	int mode, extra;

	for (p = s; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p), v = c;
		const char *swap;

		if (g_rand_boolean(rand)) {
			if (c < 0x80 && (swap = strchr("[]\\~{}|^", c)) != NULL)
				v = "{}|^[]\\~"[swap - "[]\\~{}|^"];
			else if (c == 0x130 || c == 0x131)
				v = c;
			else if (g_rand_boolean(rand))
				v = g_unichar_toupper(c);
			else if (g_rand_boolean(rand))
				v = g_unichar_tolower(c);
			else
				v = g_unichar_totitle(c);
		}
		g_string_append_unichar(out, v);
	}
	mode = g_rand_int_range(rand, 0, G_N_ELEMENTS(modes) + 1);
	if (mode < (int) G_N_ELEMENTS(modes)) {
		normal = g_utf8_normalize(out->str, -1, modes[mode]);
		g_string_assign(out, normal);
		g_free(normal);
	}
	for (extra = g_rand_int_range(rand, 0, 3); extra > 0; extra--) {
		glong at = g_rand_int_range(rand, 1, g_utf8_strlen(out->str, -1) + 1);
		const char *ign = ignorables[g_rand_int_range(rand, 0, G_N_ELEMENTS(ignorables))];

		g_string_insert(out, g_utf8_offset_to_pointer(out->str, at) - out->str, ign);
	}
	return g_string_free(out, FALSE);
}

/* the code points of s, for messages */
static char *code_points(const char *s)
{
	GString *out = g_string_new(NULL);
	const char *p;

	for (p = s; *p != '\0'; p = g_utf8_next_char(p))
		g_string_append_printf(out, "%sU+%04X", out->len > 0 ? " " : "", g_utf8_get_char(p));
	return g_string_free(out, FALSE);
}

#define SERVER_MODELS 6

/* what a server may take as the same name as s: rfc8265 / PRECIS order,
   tests/e2e/e2eircd.py's utf8 model, the other order, whole-string GLib
   case mappings */
static char *server_image(int model, const char *s)
{
	char *a, *b;

	switch (model) {
	case 0:
		a = g_utf8_casefold(s, -1);
		b = g_utf8_normalize(a, -1, G_NORMALIZE_NFC);
		break;
	case 1:
		a = g_utf8_normalize(s, -1, G_NORMALIZE_NFKC);
		b = g_utf8_casefold(a, -1);
		break;
	case 2:
		a = g_utf8_casefold(s, -1);
		b = g_utf8_normalize(a, -1, G_NORMALIZE_NFKC);
		break;
	case 3:
		return g_utf8_strup(s, -1);
	case 4:
		return g_utf8_strdown(s, -1);
	default:
		return g_utf8_casefold(s, -1);
	}
	g_free(a);
	return b;
}

/* seeded random names of several code points: the superset fold is
   idempotent, drops every invisible character, folds every variant of a
   name (rfc1459-equal ones among them) to the same string, and every name
   a server model takes as the same */
static void test_casefold_random(void)
{
	GRand *rand = g_rand_new_with_seed(RANDOM_SEED);
	GString *name = g_string_new(NULL);
	int i, n, model;

	for (i = 0; i < RANDOM_ITERATIONS; i++) {
		char *once, *twice, *variant, *vfold;
		const char *p;

		g_string_assign(name, "#");
		for (n = g_rand_int_range(rand, 1, 7); n > 0; n--)
			g_string_append_unichar(name, random_code_point(rand));

		once = e2e_casefold(name->str, E2E_CASEMAP_UNICODE);
		g_assert_true(g_utf8_validate(once, -1, NULL));
		for (p = once; *p != '\0'; p = g_utf8_next_char(p)) {
			if (oracle_ignorable(g_utf8_get_char(p)))
				g_error("iteration %d: U+%04X left in the fold", i,
				        g_utf8_get_char(p));
		}
		twice = e2e_casefold(once, E2E_CASEMAP_UNICODE);
		if (strcmp(once, twice) != 0)
			g_error("iteration %d: fold of \"%s\" is not idempotent", i, name->str);

		variant = variant_of(rand, name->str);
		vfold = e2e_casefold(variant, E2E_CASEMAP_UNICODE);
		if (strcmp(once, vfold) != 0)
			g_error("iteration %d: %s and %s fold apart", i, code_points(name->str),
			        code_points(variant));

		for (model = 0; model < SERVER_MODELS; model++) {
			char *image, *ifold;

			/* the simple upper case of U+0131 is plain I, full case
			   folding keeps U+0131 (see the code point sweep) */
			if (model == 3 && strstr(name->str, "\xc4\xb1") != NULL)
				continue;
			image = server_image(model, name->str);
			ifold = e2e_casefold(image, E2E_CASEMAP_UNICODE);

			if (strcmp(once, ifold) != 0)
				g_error("iteration %d: %s and its server model %d image %s fold apart",
				        i, code_points(name->str), model, code_points(image));
			g_free(image);
			g_free(ifold);
		}

		g_free(once);
		g_free(twice);
		g_free(variant);
		g_free(vfold);
	}
	g_string_free(name, TRUE);
	g_rand_free(rand);
}

/* random ASCII names: the UNICODE fold of a pure ASCII name equals the
   full Unicode path (forced by an invisible U+200B, which it drops) and
   the byte-wise rfc1459 fold */
static void test_casefold_ascii(void)
{
	GRand *rand = g_rand_new_with_seed(RANDOM_SEED);
	GString *name = g_string_new(NULL);
	int i, n;

	for (i = 0; i < 20000; i++) {
		char *fast, *slow, *bytes, *forced;
		gboolean exact = FALSE;

		g_string_truncate(name, 0);
		for (n = g_rand_int_range(rand, 0, 12); n > 0; n--)
			g_string_append_c(name, (char) g_rand_int_range(rand, 1, 0x80));
		forced = g_strconcat(name->str, "\xe2\x80\x8b", NULL);

		fast = e2e_casefold_ex(name->str, E2E_CASEMAP_UNICODE, &exact);
		g_assert_true(exact);
		slow = e2e_casefold(forced, E2E_CASEMAP_UNICODE);
		bytes = e2e_casefold(name->str, E2E_CASEMAP_RFC1459);
		g_assert_cmpstr(fast, ==, slow);
		g_assert_cmpstr(fast, ==, bytes);
		g_free(fast);
		g_free(slow);
		g_free(bytes);
		g_free(forced);
	}
	g_string_free(name, TRUE);
	g_rand_free(rand);
}

/* every code point: the superset fold is idempotent and equates a letter
   with its upper and lower case - except U+0130 and U+0131, where the
   simple case mapping (to and from plain i / I) and full case folding
   (CaseFolding.txt, as rfc8265 uses it) disagree; the fold follows full
   case folding there, U+0130 -> i U+0307 */
static void test_casefold_all_code_points(void)
{
	gunichar u, variants[2];
	char buf[8], vbuf[8];
	const char *p;
	int len, k;

	for (u = 0x80; u <= 0x10ffff; u++) {
		char *once, *twice;

		if (!g_unichar_validate(u))
			continue;
		len = g_unichar_to_utf8(u, buf);
		buf[len] = '\0';
		once = e2e_casefold(buf, E2E_CASEMAP_UNICODE);
		g_assert_true(g_utf8_validate(once, -1, NULL));
		if (oracle_ignorable(u) ? *once != '\0' : *once == '\0')
			g_error("U+%04X: dropped wrongly or kept wrongly", u);
		for (p = once; *p != '\0'; p = g_utf8_next_char(p)) {
			if (oracle_ignorable(g_utf8_get_char(p)))
				g_error("U+%04X folds to an invisible U+%04X", u, g_utf8_get_char(p));
		}
		twice = e2e_casefold(once, E2E_CASEMAP_UNICODE);
		if (strcmp(once, twice) != 0)
			g_error("fold of U+%04X is not idempotent", u);
		g_free(twice);

		if (u == 0x130 || u == 0x131) {
			g_free(once);
			continue;
		}
		variants[0] = g_unichar_toupper(u);
		variants[1] = g_unichar_tolower(u);
		for (k = 0; k < 2; k++) {
			char *other;

			if (variants[k] == u)
				continue;
			len = g_unichar_to_utf8(variants[k], vbuf);
			vbuf[len] = '\0';
			other = e2e_casefold(vbuf, E2E_CASEMAP_UNICODE);
			if (strcmp(once, other) != 0)
				g_error("U+%04X and U+%04X fold apart", u, variants[k]);
			g_free(other);
		}
		g_free(once);
	}
}

static void test_contexts(void)
{
	E2E_NET net, odd, bare, none, ctl;
	char *s, *t;

	e2e_net_init(&net, "IRCnet", "IRCnet2", NULL, FALSE);
	g_assert_cmpstr(net.label, ==, "IRCnet");
	g_assert_cmpint(net.casemap, ==, E2E_CASEMAP_RFC1459);
	g_assert_false(net.legacy_ok);

	s = e2e_ctx_scoped(&net, "#test");
	g_assert_cmpstr(s, ==, "IRCnet\x1f#test");
	g_assert_cmpstr(e2e_ctx_wire(s), ==, "#test");
	g_assert_cmpstr(e2e_ctx_wire("#test"), ==, "#test");
	t = e2e_ctx_network(s);
	g_assert_cmpstr(t, ==, "IRCnet");
	g_free(t);
	g_assert_null(e2e_ctx_network("#test"));
	t = e2e_ctx_display(s);
	g_assert_cmpstr(t, ==, "IRCnet:#test");
	g_free(t);
	t = e2e_ctx_display("@a@b");
	g_assert_cmpstr(t, ==, "@a@b");
	g_free(t);
	t = e2e_ctx_rescope(s, "@me@host");
	g_assert_cmpstr(t, ==, "IRCnet\x1f@me@host");
	g_free(t);
	t = e2e_ctx_rescope("#test", "@me@host");
	g_assert_cmpstr(t, ==, "@me@host");
	g_free(t);
	g_free(s);

	s = e2e_ctx_scoped(NULL, "#test");
	g_assert_cmpstr(s, ==, "#test");
	g_free(s);
	g_assert_null(e2e_ctx_scoped(&net, NULL));
	g_assert_cmpstr(e2e_ctx_wire(NULL), ==, NULL);
	g_assert_null(e2e_ctx_network(NULL));
	g_assert_null(e2e_ctx_display(NULL));

	g_assert_true(e2e_ctx_on_net("IRCnet\x1f#a", &net));
	g_assert_false(e2e_ctx_on_net("Libera\x1f#a", &net));
	g_assert_false(e2e_ctx_on_net("IRCnet2\x1f#a", &net));
	g_assert_false(e2e_ctx_on_net("IRCne\x1f#a", &net));
	g_assert_false(e2e_ctx_on_net("\x1f#a", &net));
	g_assert_true(e2e_ctx_on_net("#a", &net)); /* legacy row: any network */
	g_assert_false(e2e_ctx_on_net(NULL, &net));
	/* a scoped row is on no network that has no label */
	g_assert_false(e2e_ctx_on_net("IRCnet\x1f#a", NULL));
	g_assert_true(e2e_ctx_on_net("#a", NULL));

	/* only the first separator splits: the label never has one */
	g_assert_cmpstr(e2e_ctx_wire("IRCnet\x1f#a\x1f" "b"), ==, "#a\x1f" "b");
	t = e2e_ctx_network("IRCnet\x1f#a\x1f" "b");
	g_assert_cmpstr(t, ==, "IRCnet");
	g_free(t);
	t = e2e_ctx_rescope("IRCnet\x1f#a\x1f" "b", "#c");
	g_assert_cmpstr(t, ==, "IRCnet\x1f#c");
	g_free(t);
	t = e2e_ctx_display("IRCnet\x1f#a\x1f" "b");
	g_assert_cmpstr(t, ==, "IRCnet:#a\x1f" "b");
	g_free(t);

	/* a wire context holding the separator cannot be scoped */
	g_assert_null(e2e_ctx_scoped(&net, "#a\x1f" "b"));
	g_assert_null(e2e_ctx_scoped(&net, "\x1f"));
	g_assert_null(e2e_ctx_scoped(NULL, "#a\x1f" "b"));
	g_assert_null(e2e_ctx_rescope("IRCnet\x1f#a", "#a\x1f" "b"));
	g_assert_null(e2e_ctx_rescope("#a", "#a\x1f" "b"));
	g_assert_null(e2e_ctx_rescope(NULL, "\x1f"));
	g_assert_null(e2e_ctx_rescope("IRCnet\x1f#a", NULL));
	t = e2e_ctx_rescope(NULL, "#b");
	g_assert_cmpstr(t, ==, "#b");
	g_free(t);

	e2e_net_init(&odd, "IR|C\x1fnet", "t", "ascii", TRUE);
	g_assert_cmpstr(odd.label, ==, "IR_C_net");
	g_assert_cmpint(odd.casemap, ==, E2E_CASEMAP_ASCII);
	g_assert_true(odd.legacy_ok);
	s = e2e_ctx_scoped(&odd, "#x");
	g_assert_cmpstr(s, ==, "IR_C_net\x1f#x");
	g_assert_true(e2e_ctx_on_net(s, &odd));
	g_free(s);

	e2e_net_init(&bare, NULL, "libera", NULL, TRUE);
	g_assert_cmpstr(bare.label, ==, "libera");
	e2e_net_clear(&bare);
	e2e_net_init(&bare, "", "libera", NULL, TRUE);
	g_assert_cmpstr(bare.label, ==, "libera");

	e2e_net_init(&ctl, NULL, "\x01" "a\x1f" "b|c\x7f", NULL, FALSE);
	g_assert_cmpstr(ctl.label, ==, "_a_b_c_");
	e2e_net_clear(&ctl);
	e2e_net_init(&ctl, "My Net", "t", NULL, FALSE);
	g_assert_cmpstr(ctl.label, ==, "My Net"); /* space and printable ASCII kept */
	e2e_net_clear(&ctl);
	e2e_net_init(&ctl, " !~}", "t", NULL, FALSE);
	g_assert_cmpstr(ctl.label, ==, " !~}");
	e2e_net_clear(&ctl);
	/* C1 controls are one '_' each; U+00A0 and other letters are kept */
	e2e_net_init(&ctl, "a\xc2\x80" "b\xc2\x85" "c\xc2\x9f" "d\xc2\xa0" "e\xc5\xbb", "t", NULL,
	             FALSE);
	g_assert_cmpstr(ctl.label, ==, "a_b_c_d\xc2\xa0" "e\xc5\xbb");
	e2e_net_clear(&ctl);
	/* every byte of invalid UTF-8 is a '_' */
	e2e_net_init(&ctl, "a\xff" "b\xc5", "t", NULL, FALSE);
	g_assert_cmpstr(ctl.label, ==, "a_b_");
	e2e_net_clear(&ctl);
	e2e_net_init(&ctl, "\xc5" "a\xc0\x80" "b\xed\xa0\x80" "c\xf4\x90\x80\x80", "t", NULL, FALSE);
	g_assert_cmpstr(ctl.label, ==, "_a__b___c____");
	e2e_net_clear(&ctl);
	e2e_net_init(&ctl, "\xe2\x82", "t", NULL, FALSE); /* a truncated sequence */
	g_assert_cmpstr(ctl.label, ==, "__");
	g_assert_true(g_utf8_validate(ctl.label, -1, NULL));

	e2e_net_init(&none, NULL, NULL, "utf8", FALSE);
	g_assert_null(none.label);
	g_assert_cmpint(none.casemap, ==, E2E_CASEMAP_UNICODE);
	/* no label, no scoped key: never a bare one in its place */
	g_assert_null(e2e_ctx_scoped(&none, "#test"));
	g_assert_false(e2e_ctx_on_net("IRCnet\x1f#a", &none));
	e2e_net_clear(&none);
	e2e_net_init(&none, "", "", NULL, FALSE);
	g_assert_null(none.label);

	e2e_net_clear(&net);
	g_assert_null(net.label);
	e2e_net_clear(&net); /* twice is fine */
	e2e_net_clear(&odd);
	e2e_net_clear(&bare);
	e2e_net_clear(&ctl);
	e2e_net_clear(&none);
}

static void test_json_roundtrip(void)
{
	E2E_JSON *root, *channels, *back;
	GPtrArray *keys;
	const char *error = NULL;
	char *text;

	root = e2e_json_new(E2E_JSON_OBJECT);
	channels = e2e_json_object_member(root, "channels");
	e2e_json_set(channels, "IRCnet\x1f#t", e2e_json_new(E2E_JSON_OBJECT));
	e2e_json_set_int(e2e_json_get(channels, "IRCnet\x1f#t"), "enabled", 1);

	text = e2e_json_encode(root, FALSE);
	g_assert_nonnull(strstr(text, "IRCnet\\u001f#t"));
	g_assert_null(strchr(text, '\x1f'));

	back = e2e_json_parse(text, strlen(text), &error);
	g_assert_nonnull(back);
	keys = e2e_json_keys(e2e_json_get(back, "channels"));
	g_assert_cmpuint(keys->len, ==, 1);
	g_assert_cmpmem(g_ptr_array_index(keys, 0), strlen(g_ptr_array_index(keys, 0)),
	                "IRCnet\x1f#t", 9);
	g_assert_cmpint(e2e_json_get_int(e2e_json_get(e2e_json_get(back, "channels"),
	                                              "IRCnet\x1f#t"),
	                                 "enabled", 0),
	                ==, 1);
	g_ptr_array_free(keys, TRUE);

	e2e_json_free(back);
	e2e_json_free(root);
	g_free(text);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/e2e/scope/casemap-parse", test_casemap_parse);
	g_test_add_func("/e2e/scope/casefold", test_casefold);
	g_test_add_func("/e2e/scope/casefold-ignorables", test_casefold_ignorables);
	g_test_add_func("/e2e/scope/casefold-ex", test_casefold_ex);
	g_test_add_func("/e2e/scope/casefold-random", test_casefold_random);
	g_test_add_func("/e2e/scope/casefold-ascii", test_casefold_ascii);
	g_test_add_func("/e2e/scope/casefold-all-code-points", test_casefold_all_code_points);
	g_test_add_func("/e2e/scope/contexts", test_contexts);
	g_test_add_func("/e2e/scope/json-roundtrip", test_json_roundtrip);

	return g_test_run();
}
