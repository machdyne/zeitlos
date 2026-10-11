#ifndef ZLINK_FAST_H
#define ZLINK_FAST_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink's fast transport: the DS port (zfast.h, rtl/zfast.v),
 * docs/zlink.md ("Fast"). EXPERIMENTAL.
 *
 * -- Who transmits on which pair --
 *
 * Mozart's DS port is wired like a SATA device, so on a straight cable
 * two Mozarts both have DS1 on the same wires: one of them must transmit
 * on DS2. Nothing is fixed in advance; the two ends find out, the way
 * soft zlink finds out which way round its cable is:
 *
 *   listen on DS1   the other end transmits there -> transmit on DS2
 *                   (an ML2 cannot: it waits for DS1 to go quiet)
 *   listen on DS2   the other end transmits there -> transmit on DS1
 *   neither         after a random wait, transmit on DS1 and listen on
 *                   DS2; nothing within 80 ms -> stop and start again
 *
 * Two ends that try DS1 at the same moment on a straight cable both
 * drive one pair for up to 80 ms -- LVDS drivers are current sources and
 * take that -- then back off for different random times. Two ML2s on a
 * straight cable can never link (both can only transmit on DS1): after
 * a few rounds zl_fast_hint() says a crossover cable is needed.
 *
 * A pair wired P to N arrives inverted: commas still align, but every
 * data symbol is an error. A receiver that is aligned and sees mostly
 * errors flips its RXINV.
 *
 * -- Frames --
 *
 *   K27.7, length (2 bytes, little-endian), the frame, K29.7
 *
 * The length lets the receiver take the frame four bytes a bus access
 * (z_zf_read_words()), which is what keeps up with 100 Mbit/s; a
 * control symbol inside the frame -- damage -- shows as KWORD and the
 * frame is dropped. The link layer's CRC catches the rest.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zlink.h"

typedef struct {
	zl_transport_t t;
	int rate;				// Z_ZF_100M ...
	bool tx_ds2;			// this board can transmit on DS2

	// negotiation
	uint8_t ns;
	uint32_t now, until, quiet_since, last_aligned;
	int tx, rx;				// the pairs, once linked
	bool inv;				// RXINV
	uint32_t rng, rounds;
	uint32_t err_last, err_at, frames_at;

	// receiving
	uint8_t rs;
	uint16_t want, got;
	uint8_t rx_buf[ZL_FRAME_MAX];
	uint32_t frames_rx, dropped;
} zl_fast_t;

// The block must be claimed (z_zf_claim()). Starts listening.
void zl_fast_init(zl_fast_t *f, int rate, uint32_t seed);

// Turn the block off (it stays claimed).
void zl_fast_stop(zl_fast_t *f);

// Pairs agreed and the far end heard: frames can move.
bool zl_fast_linked(const zl_fast_t *f);

// "transmits on DS2, receives on DS1", or what it is doing meanwhile.
const char *zl_fast_role(const zl_fast_t *f);

// Advice when it will not link: NULL, or why (a crossover cable).
const char *zl_fast_hint(const zl_fast_t *f);

#endif
