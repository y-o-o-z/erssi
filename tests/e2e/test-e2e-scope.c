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
	"#Stra\xc3\x9f" "e", "#STRASSE", "#\xc4\xb0", "#i\xcc\x87", "",
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

/* every code point: the superset fold is idempotent and equates a letter
   with its upper and lower case - except U+0130 and U+0131, where the
   simple case mapping (to and from plain i / I) and full case folding
   (CaseFolding.txt, as rfc8265 uses it) disagree; the fold follows full
   case folding there, U+0130 -> i U+0307 */
static void test_casefold_all_code_points(void)
{
	gunichar u, variants[2];
	char buf[8], vbuf[8];
	int len, k;

	for (u = 0x80; u <= 0x10ffff; u++) {
		char *once, *twice;

		if (!g_unichar_validate(u))
			continue;
		len = g_unichar_to_utf8(u, buf);
		buf[len] = '\0';
		once = e2e_casefold(buf, E2E_CASEMAP_UNICODE);
		g_assert_true(g_utf8_validate(once, -1, NULL));
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
	g_assert_cmpstr(ctl.label, ==, "_a_b_c\x7f");
	e2e_net_clear(&ctl);
	e2e_net_init(&ctl, "My Net", "t", NULL, FALSE);
	g_assert_cmpstr(ctl.label, ==, "My Net"); /* only bytes below 0x20 and | */

	e2e_net_init(&none, NULL, NULL, "utf8", FALSE);
	g_assert_null(none.label);
	g_assert_cmpint(none.casemap, ==, E2E_CASEMAP_UNICODE);
	s = e2e_ctx_scoped(&none, "#test");
	g_assert_cmpstr(s, ==, "#test");
	g_free(s);
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
	g_test_add_func("/e2e/scope/casefold-all-code-points", test_casefold_all_code_points);
	g_test_add_func("/e2e/scope/contexts", test_contexts);
	g_test_add_func("/e2e/scope/json-roundtrip", test_json_roundtrip);

	return g_test_run();
}
