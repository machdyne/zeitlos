#ifndef ZLINK_SOFT_H
#define ZLINK_SOFT_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink's soft transport: two open-drain wires and nothing else.
 * docs/zlink.md, "Soft zlink".
 *
 * -- One bit --
 *
 * Both wires idle high (pull-ups). To send a 0 the sender pulls X low,
 * to send a 1 it pulls Y low. The receiver, when it notices, records
 * the bit and pulls the OTHER wire low. The sender sees both low,
 * releases its wire; the receiver sees the sender's wire rise and
 * releases its own. Both high again: the next bit. Every step waits for
 * the other side, so an end that is preempted for a while only pauses
 * the link. Nothing is timed except the timeouts that recover from a
 * collision or a peer that went away.
 *
 * -- Frames --
 *
 * LSB first, delimited the way SLIP does it: END END, the frame with
 * END, ESC and 0x3F escaped as ESC (b ^ 0x20), then END. END is 0xC0.
 * 0x3F is escaped as well because it is END with every bit inverted:
 * on a CROSSED cable -- this end's X is the peer's Y -- every bit
 * arrives inverted, the receiver finds 0x3F 0x3F where it hunts for
 * END END, and from then on decodes the frame inverted. So either
 * cable works, and the receiver says which it saw (zl_soft_crossed()).
 *
 * -- Half duplex --
 *
 * One frame on the wires at a time. zlink.c's polling discipline keeps
 * the two ends from talking at once; when they do anyway (while the
 * link comes up), the frame is garbled or both ends wait for an ack
 * that never comes, a timeout aborts it, and the link layer resends.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zlink.h"

#define ZL_SOFT_END  0xC0
#define ZL_SOFT_ESC  0xDB
#define ZL_SOFT_INV  0x3F		// END inverted
#define ZL_SOFT_BUF  (2 * ZL_FRAME_MAX + 4)

// The two wires. pull(w, true) drives wire w low; pull(w, false)
// releases it to the pull-up. level(w) is the wire as it is now.
typedef struct {
	void *ctx;
	void (*pull)(void *ctx, int wire, bool low);
	bool (*level)(void *ctx, int wire);
} zl_pins_t;

typedef struct {
	zl_pins_t pins;
	zl_transport_t t;

	// bit engine
	uint8_t state;
	uint32_t since;			// ms, entry to the current waiting state
	int8_t mine;			// the wire we hold low, or -1
	uint32_t now;

	// sending
	uint8_t tx[ZL_SOFT_BUF];
	uint16_t tx_len, tx_pos;	// bytes
	uint8_t tx_bit;
	bool tx_active;

	// receiving
	uint16_t hunt;			// the last 16 bits, while hunting
	bool aligned, inverted, esc;
	uint8_t rx_byte, rx_bits;
	uint8_t rx[ZL_FRAME_MAX];
	uint16_t rx_len;
	uint8_t done[ZL_FRAME_MAX];	// one finished frame for recv()
	uint32_t last_rx_bit;	// ms: when a bit last arrived
	uint16_t done_len;

	// counters
	uint32_t bits_tx, bits_rx, aborts, frames_rx, overflows;
	bool crossed;			// the last frame came in inverted
} zl_soft_t;

// Set up a soft transport on two wires; the zl_transport_t to hand to
// zl_init() is &s->t. rto_ms suits a link of a few hundred kbit/s with
// both ends polling.
void zl_soft_init(zl_soft_t *s, const zl_pins_t *pins);

// Release both wires and forget anything in progress.
void zl_soft_stop(zl_soft_t *s);

// Is a frame being sent or received right now? An app polls in a
// tight loop while this is true (every bit waits for both ends).
bool zl_soft_busy(const zl_soft_t *s);

// The last frame received arrived inverted: the cable is crossed.
bool zl_soft_crossed(const zl_soft_t *s);

#endif
