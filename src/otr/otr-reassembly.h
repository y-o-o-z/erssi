/*
 * OTR fragment reassembly - the pure string handling that joins the
 * PRIVMSGs a peer splits a long OTR message into.
 *
 * Copyright (C) 2008 - Uli Meis <a.sporto+bee@gmail.com>
 *               2012 - David Goulet <dgoulet@ev0ke.net>
 *               2026 - the Shellter team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301,USA
 */

#ifndef IRSSI_OTR_OTR_REASSEMBLY_H
#define IRSSI_OTR_OTR_REASSEMBLY_H

#include <stddef.h>

/*
 * The reassembly code is deliberately free of any libotr type so that it
 * can be built and fuzzed on its own (src/fe-fuzz/otr/). otr.h includes
 * this header and embeds OTR_FRAGMENT_QUEUE in its peer context.
 */

/*
 * Specified in OTR protocol version 3. See:
 * https://otr.cypherpunks.ca/Protocol-v3-4.1.1.html
 */
#define OTR_MSG_BEGIN_TAG             "?OTR:"
#define OTR_MSG_END_TAG               '.'

/* Upper bound on a message reassembled from OTR fragments. A peer sends
 * fragments until one ends with OTR_MSG_END_TAG; without a ceiling it
 * could grow the per-peer queue without end. 512 KiB is far above any
 * real OTR message (base64 of an AKE or data message) yet bounds the
 * memory a single peer can make us hold. */
#define OTR_MSG_MAX_SIZE              (512 * 1024)

/* What otr_fragment_enqueue() tells the caller to do with the message. */
enum otr_msg_status {
	OTR_MSG_ORIGINAL		= 1,
	OTR_MSG_WAIT_MORE		= 2,
	OTR_MSG_USE_QUEUE		= 3,
	OTR_MSG_ERROR			= 4,
};

/*
 * One peer's pending reassembly. All three fields are zero when nothing is
 * pending; otr_fragment_queue_clear() returns it to that state.
 */
typedef struct otr_fragment_queue {
	/* The partial message collected so far, nul-terminated, or NULL. */
	char *full_msg;
	/* Allocated size of full_msg. */
	size_t msg_size;
	/* Length of the string in full_msg, NOT counting the nul byte. */
	size_t msg_len;
} OTR_FRAGMENT_QUEUE;

/*
 * Add one received fragment to the queue.
 *
 * Returns an otr_msg_status code telling the caller what to do:
 *   OTR_MSG_ORIGINAL  - not a fragment; use the message as given.
 *   OTR_MSG_WAIT_MORE - a fragment was queued; send nothing, wait for more.
 *   OTR_MSG_USE_QUEUE - the message is complete; *full_msg is the joined
 *                       string, which the caller must free() after use.
 *   OTR_MSG_ERROR     - a bad argument or an overflow/allocation failure;
 *                       the queue is left empty.
 *
 * *full_msg is written only on OTR_MSG_USE_QUEUE.
 */
enum otr_msg_status otr_fragment_enqueue(const char *msg,
		OTR_FRAGMENT_QUEUE *queue, char **full_msg);

/* Free any pending data and return the queue to the empty state. Safe on an
 * already-empty queue. */
void otr_fragment_queue_clear(OTR_FRAGMENT_QUEUE *queue);

#endif /* IRSSI_OTR_OTR_REASSEMBLY_H */
