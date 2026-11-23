/*
 reassembly.c : fuzz the OTR fragment reassembly

    Copyright (C) 2026 the Shellter team

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 A peer can split a long OTR message over several PRIVMSGs (bitlbee does);
 otr_fragment_enqueue() joins them back. The input is split on '\n' into
 fragments and fed one by one to one queue, as otr_receive() would. Besides
 not crashing and not growing the queue past OTR_MSG_MAX_SIZE, the code must
 agree with a model kept here: the return code for every fragment, and, when
 a message completes (OTR_MSG_USE_QUEUE), the joined string byte for byte.
 The queue is cleared at the end, so AddressSanitizer/LeakSanitizer flags
 only a buffer the code itself lost, never the fragments left pending.
*/

#include <irssi/src/fe-fuzz/fuzz-target.h>

#include <irssi/src/otr/otr-reassembly.h>

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Mirror of what otr_fragment_enqueue() does, kept independently so the two
 * can be compared. `pending` is the string collected so far (NULL when the
 * queue is empty), `plen` its length. Returns the expected status and, on
 * OTR_MSG_USE_QUEUE, the expected joined message in *expect (caller frees).
 */
static enum otr_msg_status model_step(const char *frag, GString *pending,
		gboolean *have_pending, char **expect)
{
	size_t flen = strlen(frag);

	if (*have_pending) {
		/* overflow: msg_len + 1 > OTR_MSG_MAX_SIZE - pending->len */
		if (flen + 1 > OTR_MSG_MAX_SIZE - pending->len) {
			g_string_truncate(pending, 0);
			*have_pending = FALSE;
			return OTR_MSG_ERROR;
		}
		g_string_append_len(pending, frag, flen);
		if (flen == 0 || frag[flen - 1] != OTR_MSG_END_TAG)
			return OTR_MSG_WAIT_MORE;
		*expect = g_strdup(pending->str);
		g_string_truncate(pending, 0);
		*have_pending = FALSE;
		return OTR_MSG_USE_QUEUE;
	}

	/* queue empty: a begin tag at the very start that does not also end
	 * the message starts a reassembly, otherwise the message is used as is */
	if (flen >= strlen(OTR_MSG_BEGIN_TAG) &&
	    strncmp(frag, OTR_MSG_BEGIN_TAG, strlen(OTR_MSG_BEGIN_TAG)) == 0 &&
	    frag[flen - 1] != OTR_MSG_END_TAG) {
		/* the opening fragment is bounded like the rest */
		if (flen + 1 > OTR_MSG_MAX_SIZE)
			return OTR_MSG_ERROR;
		g_string_append_len(pending, frag, flen);
		*have_pending = TRUE;
		return OTR_MSG_WAIT_MORE;
	}
	return OTR_MSG_ORIGINAL;
}

static void run_fragments(char **frags)
{
	OTR_FRAGMENT_QUEUE queue = { 0 };
	GString *pending = g_string_new(NULL);
	gboolean have_pending = FALSE;
	int i;

	for (i = 0; frags[i] != NULL; i++) {
		char *expect = NULL, *got = NULL;
		enum otr_msg_status want, have;

		want = model_step(frags[i], pending, &have_pending, &expect);
		/* got is written only on OTR_MSG_USE_QUEUE; a sentinel catches
		 * any path that touches it when it must not */
		got = (char *) -1;
		have = otr_fragment_enqueue(frags[i], &queue, &got);

		if (have != want) {
			fprintf(stderr, "status %d, expected %d, on \"%s\"\n",
			        (int) have, (int) want, frags[i]);
			abort();
		}
		if (have == OTR_MSG_USE_QUEUE) {
			if (got == NULL || got == (char *) -1 || strcmp(got, expect) != 0) {
				fprintf(stderr, "joined message differs\n");
				abort();
			}
			free(got);
		} else if (got != (char *) -1) {
			/* only USE_QUEUE may write through the out pointer */
			abort();
		}
		g_free(expect);

		/* the queue tracks the model exactly and never passes the
		 * advertised ceiling */
		if (queue.msg_len != pending->len || queue.msg_len > OTR_MSG_MAX_SIZE)
			abort();
	}

	/* nothing collected must outlive the queue */
	otr_fragment_queue_clear(&queue);
	g_string_free(pending, TRUE);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	/* the target fuzzes the real code: check once that it is */
	OTR_FRAGMENT_QUEUE q = { 0 };
	char *out = NULL;

	(void) argc;
	(void) argv;
	if (otr_fragment_enqueue("?OTR:abc", &q, &out) != OTR_MSG_WAIT_MORE)
		abort();
	if (otr_fragment_enqueue("def.", &q, &out) != OTR_MSG_USE_QUEUE ||
	    out == NULL || strcmp(out, "?OTR:abcdef.") != 0) {
		fprintf(stderr, "the known fragments do not reassemble\n");
		abort();
	}
	free(out);
	otr_fragment_queue_clear(&q);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *text = g_strndup((const char *) data, size);
	char **frags = g_strsplit(text, "\n", -1);

	run_fragments(frags);

	g_strfreev(frags);
	g_free(text);
	return 0;
}
