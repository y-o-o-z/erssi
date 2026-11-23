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

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include <irssi/src/otr/otr-reassembly.h>

void otr_fragment_queue_clear(OTR_FRAGMENT_QUEUE *queue)
{
	if (queue == NULL)
		return;

	free(queue->full_msg);
	queue->full_msg = NULL;
	queue->msg_size = 0;
	queue->msg_len = 0;
}

/*
 * For the given message we received through irssi, check if we need to queue
 * it for the case where that message is part of a bigger OTR full message.
 * This can happen with bitlbee for instance where OTR messages are split in
 * different PRIVMSG.
 *
 * This uses a "queue" in the peer context so it is very important to have the
 * peer context associated with the message (nickname + irssi object).
 *
 * Return an otr_msg_status code indicating the caller what to do with the msg.
 * OTR_MSG_ERROR indicates an error probably memory related. OTR_MSG_WAIT_MORE
 * tells the caller to NOT send out the message since we are waiting for more
 * to complete the OTR original message. OTR_MSG_ORIGINAL tell the caller to
 * simply use the original message. OTR_MSG_USE_QUEUE indicates that full_msg
 * can be used containing the reconstructed message. The caller SHOULD free(3)
 * this pointer after use.
 */
enum otr_msg_status otr_fragment_enqueue(const char *msg,
		OTR_FRAGMENT_QUEUE *queue, char **full_msg)
{
	enum otr_msg_status ret;
	size_t msg_len;

	g_return_val_if_fail(msg != NULL, OTR_MSG_ERROR);
	g_return_val_if_fail(queue != NULL, OTR_MSG_ERROR);

	/* We are going to use it quite a bit so ease our life a bit. */
	msg_len = strlen(msg);

	if (queue->full_msg) {
		/* Bound the reassembly buffer: a peer must not grow the queue
		 * without ever ending it with OTR_MSG_END_TAG. On overflow drop
		 * everything collected so far and report an error. */
		if (msg_len + 1 > OTR_MSG_MAX_SIZE - queue->msg_len) {
			otr_fragment_queue_clear(queue);
			return OTR_MSG_ERROR;
		}

		if (msg_len + 1 > (queue->msg_size - queue->msg_len)) {
			char *tmp_ptr;

			/* Realloc memory if there is not enough space. The `+ 1`
			 * reserves room for the trailing nul written below, so a
			 * fragment whose length exactly fills the remaining data
			 * capacity still triggers a realloc instead of writing the
			 * nul one byte past the buffer. */
			tmp_ptr = realloc(queue->full_msg, queue->msg_size + msg_len + 1);
			if (tmp_ptr == NULL) {
				/* Drop the whole queue, not just the buffer: leaving
				 * msg_len/msg_size set with full_msg == NULL would make
				 * the next begin fragment index past a fresh buffer. */
				otr_fragment_queue_clear(queue);
				return OTR_MSG_ERROR;
			}
			queue->full_msg = tmp_ptr;
			queue->msg_size += msg_len + 1;
		}

		/* Copy msg to full message since we already have a part pending. Note
		 * that we do not copy `msg`'s trailing nul byte because we explicit
		 * set queue->full_msg[queue->msg_len] to nul afterwards. */
		memcpy(queue->full_msg + queue->msg_len, msg, msg_len);
		queue->msg_len += msg_len;
		queue->full_msg[queue->msg_len] = '\0';

		/*
		 * Are we waiting for more? If the message ends with a ".", the
		 * transmission has ended else we have to wait for more. An empty
		 * fragment (msg_len == 0) has no last byte to look at and never
		 * ends the message, so it is handled before indexing msg: reading
		 * msg[(size_t) -1] would be a heap buffer over-read.
		 */
		if (msg_len == 0 || msg[msg_len - 1] != OTR_MSG_END_TAG) {
			ret = OTR_MSG_WAIT_MORE;
			return ret;
		}

		/*
		 * Dup the string with enough space for the NULL byte since we are
		 * about to free it before passing it to the caller.
		 */
		*full_msg = g_strndup(queue->full_msg, queue->msg_len + 1);
		/* Reset everything. */
		otr_fragment_queue_clear(queue);
		ret = OTR_MSG_USE_QUEUE;
		return ret;
	} else {
		char *pos;

		/*
		 * Try to find the OTR message tag at the _beginning_of the packet and
		 * check if this packet is not the end with the end tag of OTR "."
		 */
		pos = strstr(msg, OTR_MSG_BEGIN_TAG);
		if (pos && (pos == msg) && msg[msg_len - 1] != OTR_MSG_END_TAG) {
			/* Bound the first fragment as the continuation path bounds
			 * the rest: without this an oversized opening fragment sets
			 * msg_len > OTR_MSG_MAX_SIZE, and the next fragment's
			 * `OTR_MSG_MAX_SIZE - msg_len` would wrap, disabling the
			 * ceiling; (msg_len * 2) + 1 could also overflow. */
			if (msg_len + 1 > OTR_MSG_MAX_SIZE) {
				return OTR_MSG_ERROR;
			}
			/* Allocate full message buffer with an extra for NULL byte. */
			queue->full_msg = g_new0(char, (msg_len * 2) + 1);
			if (!queue->full_msg) {
				ret = OTR_MSG_ERROR;
				return ret;
			}
			/* Copy full message with NULL terminated byte. */
			memcpy(queue->full_msg, msg, msg_len);
			queue->msg_len += msg_len;
			queue->msg_size += ((msg_len * 2) + 1);
			queue->full_msg[queue->msg_len] = '\0';
			ret = OTR_MSG_WAIT_MORE;
			return ret;
		}

		/* Use original message. */
		ret = OTR_MSG_ORIGINAL;
	}

	return ret;
}
