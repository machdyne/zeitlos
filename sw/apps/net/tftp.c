/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * TFTP client. See tftp.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tftp.h"
#include "udp.h"
#include "enc28j60.h"
#include "../../common/zeitlos.h"
#include "../../common/zstream.h"

#define TFTP_OP_RRQ    1
#define TFTP_OP_WRQ    2
#define TFTP_OP_DATA   3
#define TFTP_OP_ACK    4
#define TFTP_OP_ERROR  5

#define TFTP_SERVER_PORT   69
#define TFTP_BLOCK_SIZE    512
#define TFTP_TIMEOUT_TICKS 366	// ~0.5s at the ~732Hz tick rate (see z_uptime_ticks())
// GET, before the first DATA block: the server may have to produce the
// file first (the ESP32 gateway fetches http(s):// names on demand --
// a TLS handshake plus a redirect is ~6 s), so the RRQ is retried
// every ~4 s instead, same retry count: ~20 s of patience in total.
#define TFTP_FIRST_TIMEOUT_TICKS (4 * 732)
#define TFTP_MAX_RETRIES   5

typedef enum { T_IDLE, T_GET, T_PUT, T_DONE_OK, T_DONE_ERR } tstate_t;

static tstate_t state = T_IDLE;
static bool is_get;
static uint32_t srv_ip;
static uint16_t srv_port;	// 0 until learned from the server's first reply
static uint16_t local_port;
static uint16_t block;
static uint32_t buf_len;	// total bytes transferred so far (GET: received, PUT: sent) -- final byte count reported via tftp_poll()
static uint16_t last_chunk_len;	// PUT only: length of the most recently SENT block
static uint32_t last_tx_ticks;
static uint8_t retries;
static bool got_first;	// GET: first DATA block seen (see TFTP_FIRST_TIMEOUT_TICKS)
static uint8_t last_pkt[4 + TFTP_BLOCK_SIZE];
static uint16_t last_pkt_len;
static char err_msg[64];

// GET: this process is the zstream producer -- each block received
// from the server is held (see held_* below) until the receiver
// pulls it, then delivered and (only then) ACKed to the server. This
// keeps buffering bounded to exactly one block regardless of how the
// receiver's own pace compares to the network's -- a receiver slower
// than the network just means the server's own retransmits of the
// still-unacked block get ignored (see handle_packet()) until we
// catch up, rather than us needing to buffer ahead of what's been
// delivered.
static zstream_producer_t out_stream;
static bool pull_pending;	// receiver has an outstanding pull we haven't satisfied yet

// PUT: this process is the zstream consumer -- pulls chunks from the
// sender and forwards each to the server as the previous block gets
// ACKed. wrq_sent stays false until the first chunk actually arrives
// (there's nothing to send as block 1 before then, so no point
// starting the exchange with the server yet); server_ack_pending
// tracks the reverse case, where the server has ACKed but we don't
// have the next chunk in hand yet.
static zstream_consumer_t in_stream;
static char put_filename[64];
static bool wrq_sent;
static bool server_ack_pending;

// shared one-block holding area. GET: a block received from the
// server, not yet delivered to (pulled by) the receiver. PUT: a
// chunk received from the sender, not yet sent to the server. GET
// and PUT are never active at the same time (one transfer at a time,
// see state above), so sharing this is safe.
static uint8_t held_data[TFTP_BLOCK_SIZE];
static uint16_t held_len;
static bool held_valid;

static void set_err(const char *msg) {
	int i = 0;
	for (; msg[i] && i < 63; i++) err_msg[i] = msg[i];
	err_msg[i] = 0;
}

static uint16_t next_local_port(void) {
	static uint16_t p = 49151;
	p++;
	if (p < 49152) p = 49152;	// stay in the ephemeral range
	return p;
}

static void send_last_packet(void) {

	bool ok = udp_send(srv_ip, srv_port ? srv_port : TFTP_SERVER_PORT, local_port,
		last_pkt, last_pkt_len);

	uint8_t op = last_pkt[1];
	const char *result = ok ? "sent" : "FAILED (arp not resolved yet?)";
	uint16_t port = srv_port ? srv_port : TFTP_SERVER_PORT;

	if (op == TFTP_OP_DATA || op == TFTP_OP_ACK) {
		printf("net: tftp send op=%d block=%d len=%d to port %d: %s\n",
			op, (last_pkt[2] << 8) | last_pkt[3], last_pkt_len, port, result);
	} else {
		printf("net: tftp send op=%d len=%d to port %d: %s\n",
			op, last_pkt_len, port, result);
	}

	last_tx_ticks = z_uptime_ticks();

}

static void send_rrq_wrq(uint8_t opcode, const char *filename) {

	uint8_t *p = last_pkt;
	p[0] = 0; p[1] = opcode;

	int i = 2;
	for (; filename[i - 2] && i < (int)sizeof(last_pkt) - 8; i++)
		p[i] = filename[i - 2];
	p[i++] = 0;

	const char *mode = "octet";
	for (int j = 0; mode[j]; j++) p[i++] = mode[j];
	p[i++] = 0;

	last_pkt_len = i;
	srv_port = 0;	// not yet learned -- send_last_packet() falls back to TFTP_SERVER_PORT
	send_last_packet();

}

static void send_ack(uint16_t blk) {
	last_pkt[0] = 0; last_pkt[1] = TFTP_OP_ACK;
	last_pkt[2] = (blk >> 8) & 0xFF; last_pkt[3] = blk & 0xFF;
	last_pkt_len = 4;
	send_last_packet();
}

static void send_data(uint16_t blk, const uint8_t *data, uint16_t len) {
	last_pkt[0] = 0; last_pkt[1] = TFTP_OP_DATA;
	last_pkt[2] = (blk >> 8) & 0xFF; last_pkt[3] = blk & 0xFF;
	for (uint16_t i = 0; i < len; i++) last_pkt[4 + i] = data[i];
	last_pkt_len = 4 + len;
	send_last_packet();
}

static void fail(const char *msg) {
	set_err(msg);
	state = T_DONE_ERR;
	udp_close(local_port);
	if (is_get) {
		if (out_stream.active) zstream_send_error(&out_stream, msg);
	} else {
		if (in_stream.active) zstream_abort(&in_stream);
	}
}

// GET: called whenever there's both a block held (received from the
// server) and a pull outstanding (requested by the receiver) --
// delivers it and ACKs the server, only now that delivery is
// confirmed successful.
static void try_deliver_get(void) {

	if (!held_valid || !pull_pending) return;

	zstream_send_chunk(&out_stream, held_data, held_len);
	block = block + 1;	// held_data was always block+1 by construction (see handle_packet())
	buf_len += held_len;
	pull_pending = false;

	uint16_t delivered_len = held_len;
	held_valid = false;

	send_ack(block);

	if (delivered_len < TFTP_BLOCK_SIZE) {
		// short (or empty) block signals end of file, per RFC 1350
		state = T_DONE_OK;
		udp_close(local_port);
		zstream_send_eof(&out_stream);
	}

}

// PUT: sends the currently-held chunk as the next block, if there is
// one; otherwise remembers that the server's waiting so
// put_deliver_chunk()/put_deliver_eof() can send as soon as one
// arrives.
static void try_send_put_block(void) {

	if (!held_valid) {
		server_ack_pending = true;
		return;
	}

	block++;
	send_data(block, held_data, held_len);
	buf_len += held_len;
	last_chunk_len = held_len;
	held_valid = false;

	if (last_chunk_len == TFTP_BLOCK_SIZE) {
		// might not be the last block -- ask the sender for more.
		// (if it WAS the last block, RFC 1350 still needs a final
		// empty block -- put_deliver_eof() below handles that once
		// the sender confirms there's nothing more.)
		zstream_pull_async(&in_stream);
	}

}

static void handle_packet(uint32_t src_ip, uint16_t src_port, const uint8_t *p, uint16_t len) {

	if (state != T_GET && state != T_PUT) return;
	if (src_ip != srv_ip) return;	// not our server

	if (len < 4) return;
	uint16_t opcode = (p[0] << 8) | p[1];

	// lock onto the server's ephemeral reply port from its first
	// response (standard TFTP behavior -- the initial request goes to
	// port 69, but the server replies from, and expects further
	// traffic on, a new per-transfer port)
	if (srv_port == 0) {
		srv_port = src_port;
		printf("net: tftp server replied from port %d, locking on\n", src_port);
	} else if (src_port != srv_port) return;	// packet from an unrelated port, ignore

	if (opcode == TFTP_OP_ERROR) {
		char msg[64];
		int i = 0;
		for (; i < (int)len - 4 && i < 63; i++) msg[i] = p[4 + i];
		msg[i] = 0;
		fail(msg[0] ? msg : "server error");
		return;
	}

	if (is_get && opcode == TFTP_OP_DATA) {

		uint16_t blk = (p[2] << 8) | p[3];
		uint16_t dlen = len - 4;

		if (held_valid) {
			// already holding a block, waiting for our own receiver
			// to pull it before we ack -- a retransmit of that same
			// block (the server hasn't seen our ack yet) needs no
			// action; nothing else to do here until the pull arrives
			return;
		}

		if (blk == block + 1) {

			if (dlen > TFTP_BLOCK_SIZE) { fail("oversized block from server"); return; }

			for (uint16_t i = 0; i < dlen; i++) held_data[i] = p[4 + i];
			held_len = dlen;
			held_valid = true;
			retries = 0;
			got_first = true;

			try_deliver_get();	// may satisfy immediately if a pull is already pending

		} else if (blk == block) {
			// already-delivered-and-acked block, ordinary duplicate
			send_ack(block);
		}
		// else: unexpected block number -- ignore, let the server's
		// own timeout drive any retry

	} else if (!is_get && opcode == TFTP_OP_ACK) {

		uint16_t blk = (p[2] << 8) | p[3];

		if (blk != block) return;	// not the ACK we're waiting for

		retries = 0;

		if (block > 0 && last_chunk_len < TFTP_BLOCK_SIZE) {
			// that ACK confirmed our final (short, possibly empty)
			// block -- done
			state = T_DONE_OK;
			udp_close(local_port);
			return;
		}

		try_send_put_block();

	}

}

bool tftp_get_start(uint32_t server_ip, const char *filename,
	uint32_t receiver_pid, uint32_t open_tag) {

	if (state == T_GET || state == T_PUT) return false;

	srv_ip = server_ip;
	srv_port = 0;
	local_port = next_local_port();
	is_get = true;
	buf_len = 0;
	block = 0;
	retries = 0;
	got_first = false;
	pull_pending = false;
	held_valid = false;

	zstream_accept(&out_stream, receiver_pid, open_tag);

	udp_listen(local_port, handle_packet);
	send_rrq_wrq(TFTP_OP_RRQ, filename);
	state = T_GET;

	return true;

}

bool tftp_put_start(uint32_t server_ip, const char *filename, uint32_t sender_pid) {

	if (state == T_GET || state == T_PUT) return false;

	srv_ip = server_ip;
	srv_port = 0;
	local_port = next_local_port();
	is_get = false;
	buf_len = 0;
	block = 0;
	last_chunk_len = 0;
	retries = 0;
	wrq_sent = false;
	server_ack_pending = false;
	held_valid = false;

	strncpy(put_filename, filename, sizeof(put_filename) - 1);
	put_filename[sizeof(put_filename) - 1] = 0;

	udp_listen(local_port, handle_packet);

	// waiting on our own stream to open + deliver the first chunk --
	// nothing sent to the server yet (there's nothing to send as
	// block 1 until then). last_tx_ticks starts the "how long have we
	// been waiting" clock tftp_poll() checks below.
	last_tx_ticks = z_uptime_ticks();

	zstream_open_async(&in_stream, sender_pid, z_obj_none());

	state = T_PUT;

	return true;

}

static void put_deliver_chunk(const uint8_t *data, uint32_t len) {

	if (len > TFTP_BLOCK_SIZE) {
		fail("chunk larger than a TFTP block");
		return;
	}

	held_len = (uint16_t)len;
	memcpy(held_data, data, len);
	held_valid = true;

	if (!wrq_sent) {
		// first chunk -- now we have something to send as block 1,
		// so start the actual exchange with the server. it'll go out
		// once the server ACKs the WRQ (block 0), same as the ACK
		// branch above handles every subsequent block.
		wrq_sent = true;
		send_rrq_wrq(TFTP_OP_WRQ, put_filename);
		return;
	}

	if (server_ack_pending) {
		server_ack_pending = false;
		try_send_put_block();
	}

}

static void put_deliver_eof(void) {

	if (!wrq_sent) {
		// the file was empty from the very start -- still need to
		// run the WRQ/empty-block exchange so the server gets a
		// valid (empty) file rather than nothing at all
		wrq_sent = true;
		held_len = 0;
		held_valid = true;
		send_rrq_wrq(TFTP_OP_WRQ, put_filename);
		return;
	}

	if (last_chunk_len < TFTP_BLOCK_SIZE) {
		// last block we sent was already short -- RFC 1350 completion
		// is already satisfied; the ACK branch handles it once the
		// server ACKs that block
		return;
	}

	// last block was exactly a full one -- still need one more,
	// empty block to signal true EOF
	held_len = 0;
	held_valid = true;
	if (server_ack_pending) {
		server_ack_pending = false;
		try_send_put_block();
	}
	// else: server hasn't ACKed the previous block yet --
	// try_send_put_block() runs from the ACK branch once it does,
	// and will find this held, empty block waiting

}

bool tftp_handle_stream_msg(z_msg_t *msg) {

	switch (msg->subject) {

		case Z_STREAM_PULL:
		case Z_STREAM_ABORT: {
			if (state != T_GET) return false;
			zstream_event_t ev = zstream_producer_handle(&out_stream, msg);
			if (ev == ZSTREAM_EVENT_PULL) {
				pull_pending = true;
				try_deliver_get();
			} else if (ev == ZSTREAM_EVENT_ABORT) {
				fail("receiver aborted");
			}
			return true;
		}

		case Z_STREAM_OPEN_REPLY:
		case Z_STREAM_CHUNK:
		case Z_STREAM_EOF:
		case Z_STREAM_ERROR: {
			if (state != T_PUT) return false;
			const uint8_t *data = NULL;
			uint32_t dlen = 0;
			char err[64];
			zstream_consumer_event_t ev =
				zstream_consumer_handle(&in_stream, msg, &data, &dlen, err, sizeof(err));
			switch (ev) {
				case ZSTREAM_CEVENT_OPENED:
					zstream_pull_async(&in_stream);
					break;
				case ZSTREAM_CEVENT_OPEN_FAILED:
					fail(err);
					break;
				case ZSTREAM_CEVENT_CHUNK:
					put_deliver_chunk(data, dlen);
					break;
				case ZSTREAM_CEVENT_EOF:
					put_deliver_eof();
					break;
				case ZSTREAM_CEVENT_ERROR:
					fail(err);
					break;
				default:
					break;
			}
			return true;
		}

		default:
			return false;

	}

}

tftp_result_t tftp_poll(uint32_t *out_len, char *err_out, uint32_t err_out_len) {

	if (state == T_PUT && !wrq_sent) {
		// waiting on our own stream to open + deliver the first
		// chunk -- local, in-memory messaging isn't lossy the way
		// UDP is, so there's nothing to usefully retransmit; just
		// time out if it's taking too long (a stuck sender, not a
		// lost packet). same total window as the network-facing
		// retry timeout below, for consistency.
		if (z_uptime_ticks() - last_tx_ticks >= (uint32_t)TFTP_TIMEOUT_TICKS * TFTP_MAX_RETRIES) {
			fail("timed out waiting for data to send");
		}
	} else if (state == T_GET || state == T_PUT) {
		uint32_t tmo = (state == T_GET && !got_first)
			? TFTP_FIRST_TIMEOUT_TICKS : TFTP_TIMEOUT_TICKS;
		if (z_uptime_ticks() - last_tx_ticks >= tmo) {
			retries++;
			if (retries > TFTP_MAX_RETRIES) {
				printf("net: tftp giving up after %d retries\n", TFTP_MAX_RETRIES);
				fail("timed out");
			} else {
				printf("net: tftp retry %d/%d\n", retries, TFTP_MAX_RETRIES);
				//enc28j60_debug_dump();
				send_last_packet();	// retransmit
			}
		}
	}

	if (state == T_DONE_OK) {
		if (out_len) *out_len = buf_len;
		state = T_IDLE;
		return TFTP_RESULT_OK;
	}

	if (state == T_DONE_ERR) {
		if (err_out && err_out_len) {
			uint32_t i = 0;
			for (; err_msg[i] && i < err_out_len - 1; i++) err_out[i] = err_msg[i];
			err_out[i] = 0;
		}
		state = T_IDLE;
		return TFTP_RESULT_ERROR;
	}

	return TFTP_RESULT_PENDING;

}
