/*
 image-preview-parse.h : extracting the og:image URL from a fetched page
 */

#ifndef IRSSI_IMAGE_PREVIEW_PARSE_H
#define IRSSI_IMAGE_PREVIEW_PARSE_H

#include <glib.h>

/*
 * The og:image picture named by a downloaded page, or NULL when it names
 * none. `html` is the page body as received (attacker-controlled: the page
 * belongs to whoever posted the link), `page_url` the address it was served
 * from, used to resolve a relative og:image. The result is a freshly
 * allocated http(s) URL the caller frees, never another scheme.
 */
char *image_preview_og_image_url(const char *html, const char *page_url);

/*
 * Decode the HTML-attribute text of an og:image value: the five basic
 * entities (&amp; &lt; &gt; &quot; &apos;) and numeric references to
 * printable ASCII, everything else kept verbatim. Freshly allocated, caller
 * frees. Exposed for the fuzz target; the resolver uses it internally.
 */
char *image_preview_html_attr_decode(const char *value);

#endif /* IRSSI_IMAGE_PREVIEW_PARSE_H */
