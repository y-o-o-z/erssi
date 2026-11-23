/*
 image-preview-parse.c : extract the og:image URL from a fetched page

    Copyright (C) 2024 erssi team
                  2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A clicked page URL is downloaded and searched for its og:image meta tag,
 whose value is then decoded and resolved to an http(s) URL. The HTML comes
 from whoever posted the link, so this parses untrusted input. It is a file
 of its own, with no fetch state and no libcurl transfer - only glib and the
 libcurl URL parser - so the fuzz target (src/fe-fuzz/image-preview/parse.c)
 runs it exactly as the preview builds it.
*/

#include <irssi/src/image-preview/image-preview-parse.h>

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

/* Extract the raw og:image attribute value from HTML content */
static char *extract_og_image(const char *html)
{
	GRegex *regex;
	GMatchInfo *match_info;
	char *og_image = NULL;
	GError *error = NULL;

	if (html == NULL || *html == '\0')
		return NULL;

	/* Pattern matches both attribute orders:
	 * <meta property="og:image" content="URL">
	 * <meta content="URL" property="og:image"> */
	regex = g_regex_new(
		"<meta[^>]+property=[\"']og:image[\"'][^>]+content=[\"']([^\"']+)[\"']"
		"|<meta[^>]+content=[\"']([^\"']+)[\"'][^>]+property=[\"']og:image[\"']",
		G_REGEX_CASELESS | G_REGEX_DOTALL, 0, &error);

	if (error != NULL) {
		g_error_free(error);
		return NULL;
	}

	if (g_regex_match(regex, html, 0, &match_info)) {
		/* Try first capture group */
		og_image = g_match_info_fetch(match_info, 1);
		if (og_image == NULL || *og_image == '\0') {
			g_free(og_image);
			/* Try second capture group */
			og_image = g_match_info_fetch(match_info, 2);
		}
	}

	g_match_info_free(match_info);
	g_regex_unref(regex);

	if (og_image == NULL || *og_image == '\0') {
		g_free(og_image);
		og_image = NULL;
	}

	return og_image;
}

/* The og:image value is HTML attribute text: "&amp;" in it is "&". Only
 * the entities a URL needs are decoded (the five basic ones and numeric
 * references to printable ASCII), anything else is kept as it is. */
char *image_preview_html_attr_decode(const char *value)
{
	static const struct {
		const char *name;
		char chr;
	} entities[] = {
		{ "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
	};
	GString *out;
	const char *p;

	out = g_string_sized_new(strlen(value));
	for (p = value; *p != '\0'; p++) {
		gboolean done = FALSE;
		guint i;

		if (*p != '&') {
			g_string_append_c(out, *p);
			continue;
		}
		for (i = 0; i < G_N_ELEMENTS(entities) && !done; i++) {
			size_t len = strlen(entities[i].name);

			if (strncmp(p + 1, entities[i].name, len) == 0) {
				g_string_append_c(out, entities[i].chr);
				p += len;
				done = TRUE;
			}
		}
		if (!done && p[1] == '#') {
			const char *num = p + 2;
			char *end;
			gulong chr;

			if (*num == 'x' || *num == 'X')
				chr = strtoul(num + 1, &end, 16);
			else
				chr = strtoul(num, &end, 10);
			if (end != num && end != num + 1 && *end == ';' && chr >= 0x20 && chr < 0x7f) {
				g_string_append_c(out, (char) chr);
				p = end;
				done = TRUE;
			}
		}
		if (!done)
			g_string_append_c(out, '&');
	}
	return g_string_free(out, FALSE);
}

/* The URL of the picture: og:image decoded and, when it is relative
 * ("/img.png", "//cdn/x.png"), resolved against the page it was found on.
 * NULL unless the result is an http or https URL - the download is limited
 * to those anyway, and to public addresses (open_public_socket()). */
static char *og_image_resolve(const char *page_url, const char *value)
{
	char *decoded, *result = NULL;

	decoded = image_preview_html_attr_decode(value);
	g_strstrip(decoded);
	if (*decoded == '\0') {
		g_free(decoded);
		return NULL;
	}
#if LIBCURL_VERSION_NUM >= 0x073e00
	{
		CURLU *url = curl_url();
		char *scheme = NULL, *full = NULL;

		if (url != NULL && page_url != NULL &&
		    curl_url_set(url, CURLUPART_URL, page_url, 0) == CURLUE_OK &&
		    curl_url_set(url, CURLUPART_URL, decoded, 0) == CURLUE_OK &&
		    curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
		    (g_ascii_strcasecmp(scheme, "http") == 0 ||
		     g_ascii_strcasecmp(scheme, "https") == 0) &&
		    curl_url_get(url, CURLUPART_URL, &full, 0) == CURLUE_OK)
			result = g_strdup(full);
		curl_free(scheme);
		curl_free(full);
		curl_url_cleanup(url);
	}
#else
	/* no URL parser in this libcurl: absolute URLs only */
	if (g_ascii_strncasecmp(decoded, "http://", 7) == 0 ||
	    g_ascii_strncasecmp(decoded, "https://", 8) == 0)
		result = g_strdup(decoded);
#endif
	g_free(decoded);
	return result;
}

/* The picture of a downloaded page, NULL when it names none */
char *image_preview_og_image_url(const char *html, const char *page_url)
{
	char *value, *url;

	value = extract_og_image(html);
	if (value == NULL)
		return NULL;
	url = og_image_resolve(page_url, value);
	g_free(value);
	return url;
}
