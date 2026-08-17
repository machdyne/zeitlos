#ifndef ENC28J60_H
#define ENC28J60_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Driver for the Microchip ENC28J60 SPI Ethernet controller (PMOD).
 *
 * IMPORTANT: this driver's register bank/address assignments and init
 * sequence are transcribed from training knowledge of the ENC28J60
 * datasheet (Microchip DS39662) and widely-used reference drivers,
 * not verified against a live datasheet lookup, and completely
 * untested on real hardware. See enc28j60.c's header comment for the
 * specific things most likely to be wrong if something doesn't work.
 * Start by checking whether enc28j60_init() returns true and
 * enc28j60_revision() comes back as a small nonzero value (typically
 * 1-6) -- that's the cheapest possible test of whether SPI
 * communication works at all, before trusting anything else.
 */

#include <stdint.h>
#include <stdbool.h>

// call once at startup. mac[6] is this interface's MAC address (the
// chip has no factory-assigned one). returns true if the chip
// responded with a plausible revision id.
bool enc28j60_init(const uint8_t mac[6]);

// last-read chip revision id (valid after enc28j60_init())
uint8_t enc28j60_revision(void);

// prints the chip's own ESTAT/EIR/EPKTCNT registers directly, plus
// our own tracked next_packet_ptr -- for checking whether the
// hardware itself thinks something is wrong (a receive error flag,
// an unexpected status) versus a purely software-side issue.
void enc28j60_debug_dump(void);

// non-blocking: returns the number of bytes received (0 if no packet
// is waiting). writes into buf, up to maxlen bytes -- frames longer
// than maxlen are truncated (the rest is still consumed/discarded
// from the chip's buffer, just not copied out).
uint16_t enc28j60_recv(uint8_t *buf, uint16_t maxlen);

// blocks until the frame is transmitted or a retry/timeout limit is
// hit (see enc28j60.c, ETH_TX_TIMEOUT -- a documented ENC28J60 errata
// workaround, not just a sanity bound). returns true on success.
bool enc28j60_send(const uint8_t *buf, uint16_t len);

#endif
