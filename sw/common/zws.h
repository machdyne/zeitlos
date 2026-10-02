#ifndef ZWS_H
#define ZWS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The server side of WebSocket (RFC 6455), as pieces: the opening
 * handshake, the header of a frame going out, and an incremental
 * parser for the frames coming in. Nothing else -- no extensions, no
 * text validation, no sockets, no malloc. The caller owns every
 * buffer and does the I/O, so this works over zport, over a host
 * socket in a test, or over anything that moves bytes.
 *
 * -- the server side only --
 *
 * A server sends frames unmasked and receives them masked; a client
 * is the other way round. Only the server's half exists: outgoing
 * frames are never masked, and an incoming frame that is not masked
 * is a protocol error (RFC 6455 section 5.1).
 *
 * -- handshake --
 *
 * The caller parses the HTTP request and hands over the value of
 * Sec-WebSocket-Key; z_ws_response() writes the whole `101` reply.
 * Subprotocols and extensions are not offered, so a client that asks
 * for permessage-deflate simply does not get it (the reply does not
 * mention it, which is how a server declines).
 *
 * -- sending --
 *
 * z_ws_header() writes the 2, 4 or 10 header bytes for a payload of a
 * given length; the caller appends the payload itself, so a big
 * payload is never copied here. z_ws_control() builds a small control
 * frame (close, ping, pong) whole.
 *
 * -- receiving --
 *
 * z_ws_feed() takes whatever bytes have arrived, in any chunking down
 * to one at a time, and returns at most one event per call. The
 * caller loops until the input is used up:
 *
 *	while (n) {
 *		z_ws_event ev;
 *		size_t used = z_ws_feed(&p, buf, n, &ev);
 *		buf += used; n -= used;
 *		switch (ev.type) { ... }
 *	}
 *
 * The input is modified in place -- payload is unmasked where it lies
 * -- and an event's `data` points into it, or, for a control frame,
 * into the parser, and is valid until the next z_ws_feed(). A data
 * frame is delivered as it arrives, in as many events as the input
 * took to arrive, so the largest frame is not a buffer size; a
 * control frame is held until whole (at most 125 bytes, so it fits
 * the parser) and delivered once. A message in fragments is delivered
 * as the fragments' data events in order; `fin` marks the end of the
 * message, and `opcode` stays that of the message (text or binary),
 * not CONTINUATION.
 *
 * What is checked, and answered with an ERROR event whose `code` is
 * the status to close with: 1002 for an unmasked frame, a reserved
 * bit, an unknown opcode, a fragmented or oversize control frame, a
 * continuation with nothing to continue, a new message in the middle
 * of one, a malformed close; 1009 for a data frame longer than
 * `max_payload`. After an ERROR the parser discards all input; the
 * caller sends the close (z_ws_control(.., Z_WS_CLOSE, ..)) and
 * drops the connection. Not checked: that text is UTF-8, that a
 * close code is a legal one, that a CLOSE is answered -- the caller
 * decides, and decides the same way for all three.
 */

// -- handshake --

#define Z_WS_KEY_LEN		24	// a client key: base64 of 16 bytes
#define Z_WS_ACCEPT_LEN		28	// the Sec-WebSocket-Accept value, without the NUL
#define Z_WS_RESPONSE_MAX	160	// the whole 101 reply, NUL included

// Writes `HTTP/1.1 101 ...` up to and including the blank line, and
// a NUL, into `out`. `key` is the value of Sec-WebSocket-Key without
// surrounding whitespace. Returns the length of the reply, or 0 if
// the key is not Z_WS_KEY_LEN characters or `cap` is under
// Z_WS_RESPONSE_MAX.
size_t z_ws_response(char *out, size_t cap, const char *key, size_t keylen);

// Just the accept value, for a caller that builds its own reply.
// `out` takes Z_WS_ACCEPT_LEN + 1 bytes. Returns 0, or -1 for a bad key.
int z_ws_accept(char out[Z_WS_ACCEPT_LEN + 1], const char *key, size_t keylen);

// -- opcodes --

#define Z_WS_CONT	0x0
#define Z_WS_TEXT	0x1
#define Z_WS_BINARY	0x2
#define Z_WS_CLOSE	0x8
#define Z_WS_PING	0x9
#define Z_WS_PONG	0xA

// Close status codes this file produces (RFC 6455 section 7.4.1).
#define Z_WS_CLOSE_NORMAL	1000
#define Z_WS_CLOSE_PROTOCOL	1002
#define Z_WS_CLOSE_TOO_BIG	1009

// -- sending --

#define Z_WS_HEADER_MAX		10	// 2 + 8 bytes of length
#define Z_WS_CONTROL_MAX	127	// header + 125 bytes of payload

// Writes the header of one unmasked, FIN-set frame of `len` payload
// bytes with `opcode`, and returns its size (2, 4 or 10). Use the
// shortest length encoding, as the RFC requires.
size_t z_ws_header(uint8_t out[Z_WS_HEADER_MAX], uint8_t opcode, uint64_t len);

// A whole control frame: header then `len` payload bytes (at most
// 125; more is cut to 125). `out` takes Z_WS_CONTROL_MAX bytes.
// Returns the frame's size.
size_t z_ws_control(uint8_t out[Z_WS_CONTROL_MAX], uint8_t opcode,
	const void *payload, size_t len);

// A close frame carrying only a status code (a Z_WS_CLOSE_*).
size_t z_ws_close(uint8_t out[Z_WS_CONTROL_MAX], uint16_t code);

// -- receiving --

typedef enum {
	Z_WS_EV_NONE,		// more input needed
	Z_WS_EV_DATA,		// part of a text or binary message
	Z_WS_EV_PING,		// answer with the same payload as a PONG
	Z_WS_EV_PONG,
	Z_WS_EV_CLOSE,		// `code`, then the reason text in data/len
	Z_WS_EV_ERROR		// close with `code` and drop the connection
} z_ws_ev_type;

typedef struct {
	z_ws_ev_type type;
	uint8_t opcode;		// DATA: Z_WS_TEXT or Z_WS_BINARY, of the whole message
	uint8_t fin;		// DATA: this chunk ends the message
	uint16_t code;		// CLOSE: status sent (1005 if none); ERROR: status to send
	const uint8_t *data;
	uint32_t len;
} z_ws_event;

typedef struct {
	uint8_t st;
	uint8_t op;		// opcode of the frame being read
	uint8_t fin;
	uint8_t msg_op;		// opcode of the message in progress, 0 if none
	uint8_t ext;		// extended length bytes still to come
	uint8_t mask[4];
	uint8_t mask_i;		// next byte's place in the mask
	uint8_t ctl_len;
	uint32_t remain;	// payload bytes of this frame still to come
	uint32_t max_payload;
	uint8_t ctl[125];
} z_ws_parser;

// `max_payload` is the longest data frame accepted, in payload bytes.
// Control frames are bound by the RFC to 125 whatever this says.
void z_ws_parser_init(z_ws_parser *p, uint32_t max_payload);

// Consumes input from `in` (up to `n` bytes) until one event is
// ready, and returns how many bytes it took. With Z_WS_EV_NONE it has
// taken all of them. Unmasks in place; see the notes above.
size_t z_ws_feed(z_ws_parser *p, uint8_t *in, size_t n, z_ws_event *ev);

#endif
