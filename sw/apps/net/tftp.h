#ifndef TFTP_H
#define TFTP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * TFTP client (RFC 1350). Non-blocking, poll-driven, one transfer at
 * a time: call tftp_get_start()/tftp_put_start(), then call
 * tftp_poll() repeatedly (e.g. from the same loop as eth_poll()) --
 * the actual protocol progress happens via a UDP listener callback
 * (driven by eth_poll()/udp_handle()); tftp_poll() itself only
 * handles retransmit timeouts and reports the final result.
 *
 * Both directions stream through sw/common/zstream.h -- no
 * whole-file-in-memory buffer, no size limit beyond what the other
 * end of the stream imposes on itself. See docs/networking.md for
 * the design (and the objcopy/stack-corruption bug that motivated
 * moving off a fixed-size buffer in the first place).
 *
 * GET: this process is the zstream producer (it has the data, as it
 * arrives from the server); tftp_get_start() calls zstream_accept()
 * internally against receiver_pid/open_tag (the caller has already
 * received the Z_STREAM_OPEN that triggered this).
 *
 * PUT: this process is the zstream consumer (it needs to pull the
 * data from whoever's sending it, to forward to the server);
 * tftp_put_start() calls zstream_open_async() internally against
 * sender_pid. Since this process can't block waiting on either the
 * network or the stream, tftp_handle_stream_msg() is how the two
 * halves make progress -- feed it every message your own loop reads.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../../common/zmsg.h"

typedef enum {
	TFTP_RESULT_PENDING,	// still in progress, call tftp_poll() again
	TFTP_RESULT_OK,
	TFTP_RESULT_ERROR
} tftp_result_t;

// starts a GET (RRQ). streams received data to receiver_pid via
// zstream.h as it arrives -- see the file header comment above.
// open_tag should be the tag from the Z_STREAM_OPEN message that
// triggered this (zstream_accept() replies using it). returns false
// if a transfer is already in progress.
bool tftp_get_start(uint32_t server_ip, const char *filename,
	uint32_t receiver_pid, uint32_t open_tag);

// starts a PUT (WRQ). pulls the data to send from sender_pid via
// zstream.h, forwarding each chunk to the server as it arrives.
// returns false if a transfer is already in progress.
bool tftp_put_start(uint32_t server_ip, const char *filename,
	uint32_t sender_pid);

// feed every message your own loop reads to this. handles whichever
// stream-protocol messages belong to the currently active transfer
// (GET: PULL/ABORT from the receiver; PUT: OPEN_REPLY/CHUNK/EOF/ERROR
// from the sender) and returns true if the message was consumed.
// returns false, leaving the message untouched, for anything else --
// including if there's no active transfer.
bool tftp_handle_stream_msg(z_msg_t *msg);

// call repeatedly while a transfer is in progress. on
// TFTP_RESULT_OK, *out_len is set to the total bytes transferred
// (GET: received: PUT: sent). on TFTP_RESULT_ERROR, err_out (if
// non-NULL) is filled with a NUL-terminated message, up to
// err_out_len bytes.
tftp_result_t tftp_poll(uint32_t *out_len, char *err_out, uint32_t err_out_len);

#endif
