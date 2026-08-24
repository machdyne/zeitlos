/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Telnet client. See telnet.h for the option-negotiation policy
 * (accept ECHO/SUPPRESS-GO-AHEAD, refuse everything else, never reply
 * to an unsolicited DONT/WONT) and why it's loop-safe.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "telnet.h"
#include "tcp.h"

#define TELNET_PORT 23

// telnet command bytes (RFC 854)
#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define SE    240

// options we actually accept when the peer offers them (see telnet.h)
#define OPT_ECHO 1
#define OPT_SGA  3

typedef enum {
	TN_DATA,	// ordinary data byte
	TN_IAC,		// just saw IAC, waiting for the command byte
	TN_CMD,		// just saw IAC DO/DONT/WILL/WONT, waiting for the option byte
	TN_SB,		// inside a subnegotiation (IAC SB ... IAC SE) -- we don't
				// implement any subnegotiation, just skip to the end
	TN_SB_IAC,	// inside a subnegotiation, just saw an IAC -- next byte
				// SE ends it, anything else means "still inside" (a
				// literal 0xFF the peer escaped as part of the
				// subnegotiation payload)
} telnet_parse_state_t;

static telnet_parse_state_t pstate;
static uint8_t pending_cmd;	// DO/DONT/WILL/WONT waiting for its option byte (TN_CMD)

static telnet_established_handler_t app_on_established;
static telnet_data_handler_t app_on_data;
static telnet_closed_handler_t app_on_closed;

// -- outbound queue -- decouples telnet_send()/negotiation replies
// from tcp_send()'s single-outstanding-segment limit (tcp.h). Sized
// well beyond one TCP_MAX_PAYLOAD segment so a burst of typed
// keystrokes plus a handful of negotiation replies don't immediately
// collide with a slow/not-yet-acked in-flight segment.
#define TELNET_TX_QUEUE_LEN 512
static uint8_t tx_queue[TELNET_TX_QUEUE_LEN];
static uint16_t tx_queue_len;

static bool queue_bytes(const uint8_t *data, uint16_t len) {
	if ((uint32_t)tx_queue_len + len > TELNET_TX_QUEUE_LEN) return false;
	memcpy(tx_queue + tx_queue_len, data, len);
	tx_queue_len += len;
	return true;
}

// answers exactly one DO or WILL from the peer -- see telnet.h for
// why DONT/WONT never reach here (on_tcp_event()/the TN_CMD case
// below only calls this for DO/WILL).
static void answer_negotiation(uint8_t cmd, uint8_t opt) {

	uint8_t reply;

	if (cmd == DO) {
		// the peer is asking us to enable `opt` -- we implement
		// nothing a server would ask FOR, so always refuse.
		reply = WONT;
	} else {
		// cmd == WILL: the peer is offering to provide `opt` itself.
		// accept exactly the two options that make an interactive
		// session actually usable (see telnet.h); refuse everything
		// else.
		reply = (opt == OPT_ECHO || opt == OPT_SGA) ? DO : DONT;
	}

	uint8_t r[3] = { IAC, reply, opt };
	// best-effort -- if the queue is genuinely full, dropping a
	// negotiation reply is a rare, harmless-in-practice edge case
	// (worst case the peer just re-sends its offer, or the session
	// proceeds in whatever its own default is); not worth failing
	// the whole connection over.
	queue_bytes(r, 3);

}

// runs the telnet byte-stream parser over one chunk of received TCP
// data, stripping negotiation sequences and answering DO/WILL as it
// goes (answer_negotiation() above), and delivers whatever ordinary
// data bytes remain to app_on_data() in one call. State (pstate/
// pending_cmd) persists across calls, since a IAC sequence can
// legitimately straddle two separate TCP segments.
static void telnet_feed(const uint8_t *data, uint16_t len) {

	// sized to TCP_MAX_RX_PAYLOAD (tcp.h), NOT TCP_MAX_PAYLOAD -- see
	// that header's own comment for why these are two different
	// numbers (536 vs 1460): TCP_MAX_PAYLOAD is our own SEND-side MSS
	// convention, which says nothing about how large a segment a real
	// server can actually send US. This buffer used to be sized off
	// the wrong one of the two -- confirmed reachable on real
	// hardware once tcp_handle() (tcp.c) started properly clamping
	// what it delivers to TCP_MAX_RX_PAYLOAD too, closing this from
	// both ends: the buffer is now big enough for anything that could
	// arrive, AND the sender-side clamp means it never has to be.
	static uint8_t clean[TCP_MAX_RX_PAYLOAD];	// output can't exceed
												// input length (IAC
												// removal only shrinks
												// it), so this is always
												// big enough
	uint16_t clean_len = 0;

	for (uint16_t i = 0; i < len; i++) {

		uint8_t b = data[i];

		switch (pstate) {

		case TN_DATA:
			if (b == IAC) pstate = TN_IAC;
			else clean[clean_len++] = b;
			break;

		case TN_IAC:
			if (b == IAC) {
				clean[clean_len++] = b;	// escaped literal 0xFF
				pstate = TN_DATA;
			} else if (b == DO || b == DONT || b == WILL || b == WONT) {
				pending_cmd = b;
				pstate = TN_CMD;
			} else if (b == SB) {
				pstate = TN_SB;
			} else {
				// any other 2-byte command (NOP, DM, BRK, IP, AO,
				// AYT, EC, EL, GA, ...) -- consumed, no reply needed,
				// nothing we act on
				pstate = TN_DATA;
			}
			break;

		case TN_CMD:
			if (pending_cmd == DO || pending_cmd == WILL)
				answer_negotiation(pending_cmd, b);
			// DONT/WONT: no reply -- see telnet.h
			pstate = TN_DATA;
			break;

		case TN_SB:
			if (b == IAC) pstate = TN_SB_IAC;
			break;

		case TN_SB_IAC:
			pstate = (b == SE) ? TN_DATA : TN_SB;
			break;

		}

	}

	if (clean_len && app_on_data) app_on_data(clean, clean_len);

}

static void on_tcp_event(tcp_event_t ev, const uint8_t *data, uint16_t len) {

	switch (ev) {

	case TCP_EVENT_ESTABLISHED:
		printf("telnet: TCP_EVENT_ESTABLISHED, app_on_established=%s\n",
			app_on_established ? "set" : "NULL");
		if (app_on_established) app_on_established();
		break;

	case TCP_EVENT_DATA:
		telnet_feed(data, len);
		break;

	case TCP_EVENT_CLOSED:
		// diagnostic: this is the exact dispatch point between tcp.c's
		// notify() and net.c's telnet_on_closed() -- a real bug once
		// meant notify() itself never called this function at all
		// (tcp.c's reset_to_closed()/notify() ordering, now fixed).
		// Printing here regardless of whether app_on_closed is set
		// confirms the event actually made it this far, which is the
		// one thing the old silent-drop bug made impossible to tell
		// from net.c's own logs alone.
		printf("telnet: TCP_EVENT_CLOSED, app_on_closed=%s\n",
			app_on_closed ? "set" : "NULL");
		if (app_on_closed) app_on_closed();
		break;

	}

}

bool telnet_connect(uint32_t ip, telnet_established_handler_t on_established,
	telnet_data_handler_t on_data, telnet_closed_handler_t on_closed) {

	pstate = TN_DATA;
	tx_queue_len = 0;

	app_on_established = on_established;
	app_on_data = on_data;
	app_on_closed = on_closed;

	return tcp_connect(ip, TELNET_PORT, on_tcp_event);

}

bool telnet_send(const uint8_t *data, uint16_t len) {

	if (!tcp_is_connected()) return false;

	// escape any literal 0xFF (IAC) byte -- see telnet.h. computed up
	// front so this either queues the whole thing or none of it, no
	// partial-write edge case to reason about on a full queue.
	uint16_t escaped_len = len;
	for (uint16_t i = 0; i < len; i++)
		if (data[i] == IAC) escaped_len++;

	if ((uint32_t)tx_queue_len + escaped_len > TELNET_TX_QUEUE_LEN) return false;

	for (uint16_t i = 0; i < len; i++) {
		tx_queue[tx_queue_len++] = data[i];
		if (data[i] == IAC) tx_queue[tx_queue_len++] = IAC;
	}

	return true;

}

void telnet_close(void) {
	tcp_close();
}

void telnet_abort(void) {
	tx_queue_len = 0;
	pstate = TN_DATA;
	tcp_abort();
}

void telnet_poll(void) {

	if (!tcp_is_connected()) return;
	if (tx_queue_len == 0) return;

	uint16_t n = tx_queue_len > TCP_MAX_PAYLOAD ? TCP_MAX_PAYLOAD : tx_queue_len;

	if (tcp_send(tx_queue, n)) {
		memmove(tx_queue, tx_queue + n, tx_queue_len - n);
		tx_queue_len -= n;
	}

}
