/*
 parse.c : fuzz the og:image extraction of the image preview

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A clicked page URL is downloaded and searched for its og:image meta tag
 (image-preview-parse.c). The HTML is whoever posted the link's, so it is
 untrusted. The input is fed as that HTML, with its first line taken as the
 page URL it was served from (both attacker-reachable). Besides not
 crashing, two things must hold: a resolved picture URL is only ever http or
 https (never javascript:, data:, file: ...), and the attribute decoder
 agrees with an independent model here and never makes its output longer
 than its input.
*/

#include <irssi/src/fe-fuzz/fuzz-target.h>

#include <irssi/src/image-preview/image-preview-parse.h>

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolver's contract: a picture URL it returns is http(s) or nothing. */
static void check_scheme(char *url)
{
	if (url == NULL)
		return;
	if (g_ascii_strncasecmp(url, "http://", 7) != 0 &&
	    g_ascii_strncasecmp(url, "https://", 8) != 0) {
		fprintf(stderr, "resolved a non-http(s) URL: %s\n", url);
		abort();
	}
}

/*
 * Independent mirror of image_preview_html_attr_decode(), written over an
 * index rather than a moving pointer so a divergence points at one of them.
 * The five basic entities and numeric references to printable ASCII decode,
 * anything else is copied verbatim; every step consumes at least as many
 * input bytes as it writes, so the result is never longer than the input.
 */
static char *model_attr_decode(const char *value)
{
	static const struct {
		const char *name;
		char chr;
	} ent[] = {
		{ "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
	};
	size_t n = strlen(value), i = 0, o = 0;
	char *out = g_malloc(n + 1);

	while (i < n) {
		gboolean done = FALSE;
		guint e;

		if (value[i] != '&') {
			out[o++] = value[i++];
			continue;
		}
		for (e = 0; e < G_N_ELEMENTS(ent) && !done; e++) {
			size_t l = strlen(ent[e].name);

			if (strncmp(value + i + 1, ent[e].name, l) == 0) {
				out[o++] = ent[e].chr;
				i += 1 + l;
				done = TRUE;
			}
		}
		if (!done && value[i + 1] == '#') {
			const char *num = value + i + 2;
			char *end;
			gulong chr;

			if (*num == 'x' || *num == 'X')
				chr = strtoul(num + 1, &end, 16);
			else
				chr = strtoul(num, &end, 10);
			if (end != num && end != num + 1 && *end == ';' && chr >= 0x20 && chr < 0x7f) {
				out[o++] = (char) chr;
				i = (size_t) (end - value) + 1;
				done = TRUE;
			}
		}
		if (!done)
			out[o++] = value[i++];
	}
	out[o] = '\0';
	return out;
}

static void check_decode(const char *value)
{
	char *got = image_preview_html_attr_decode(value);
	char *exp = model_attr_decode(value);

	if (strcmp(got, exp) != 0) {
		fprintf(stderr, "attr decode differs: got \"%s\", model \"%s\"\n", got, exp);
		abort();
	}
	if (strlen(got) > strlen(value))
		abort();
	g_free(got);
	g_free(exp);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	/* the target fuzzes the real code: check once that it is */
	char *url;

	(void) argc;
	(void) argv;

	url = image_preview_og_image_url(
		"<meta property=\"og:image\" content=\"http://example.com/a.png\">",
		"http://example.com/");
	if (url == NULL || strstr(url, "example.com/a.png") == NULL) {
		fprintf(stderr, "the known og:image tag did not resolve\n");
		abort();
	}
	check_scheme(url);
	g_free(url);

	/* a non-http(s) og:image is refused, not handed back */
	url = image_preview_og_image_url(
		"<meta property='og:image' content='javascript:alert(1)'>",
		"http://example.com/");
	if (url != NULL) {
		fprintf(stderr, "a javascript: og:image was not refused: %s\n", url);
		abort();
	}

	url = image_preview_html_attr_decode("a&amp;b&#38;c&#x2f;d");
	if (strcmp(url, "a&b&c/d") != 0) {
		fprintf(stderr, "known entities did not decode: %s\n", url);
		abort();
	}
	g_free(url);
	check_decode("a&amp;b&#38;c&#x2f;d");

	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *full = g_strndup((const char *) data, size);
	char *split = g_strndup((const char *) data, size);
	char *nl, *url;

	/* attacker HTML resolved against a fixed, valid page URL: exercises
	 * relative og:image resolution on the whole input */
	url = image_preview_og_image_url(full, "http://host.example/dir/page.html");
	check_scheme(url);
	g_free(url);

	/* the page URL is attacker-reachable too (the effective URL after
	 * redirects): first line is the page URL, the rest is the HTML */
	nl = strchr(split, '\n');
	if (nl != NULL) {
		*nl = '\0';
		url = image_preview_og_image_url(nl + 1, split);
	} else {
		url = image_preview_og_image_url(split, split);
	}
	check_scheme(url);
	g_free(url);

	/* the entity decoder on the raw bytes, against the model */
	check_decode(full);

	g_free(split);
	g_free(full);
	return 0;
}
