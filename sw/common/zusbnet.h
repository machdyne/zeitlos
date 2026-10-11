#ifndef ZUSBNET_H
#define ZUSBNET_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A USB ethernet adapter (CDC-ECM), for `net`. The driver is in the
 * `usb` process (sw/apps/usb/usbh_ecm.c; until Phase 3 it was the
 * kernel's, behind Z_SYS_USBNET). See docs/usb_ethernet.md and
 * docs/usb_driver.md.
 *
 * -- One owner --
 *
 * sw/apps/net is the only client (its usb_ecm.c backend): whoever
 * opened the adapter last has it.
 *
 * -- Frames move through two rings, not messages --
 *
 * A message per frame would be a round trip per frame, about a
 * millisecond each. Instead NET_OPEN (zusbproto.h) sets up two
 * single-producer, single-consumer rings, and each side writes only
 * its own memory:
 *
 *   z_usbnet_srv_t, in `usb`:  frames received, and how far `usb` has
 *                              got with the frames `net` gave it
 *   z_usbnet_cli_t, in `net`:  frames to send, and how far `net` has
 *                              got with the frames it was given
 *
 * Each reads the other's directly: any process may load from anywhere
 * (docs/mpu.md), and a blob in a message arrives as a physical address
 * (docs/messaging.md). NET_OPEN carries net's ring as its data blob,
 * and `usb` keeps that address; its reply IS `usb`'s ring, whose
 * address net keeps. That is the one reply that is not borrowed: it
 * stays valid while `usb` runs. Indices only grow; a slot is index %
 * slots. Single core, and the data cache is the CPU's own, so an index
 * written after its slot is never seen before it.
 *
 * Wakes, only when a ring goes from empty to not:
 *
 *   `usb` received a frame and net's rx_tail had caught up with rx_head
 *       -> Z_USB_EVENT (Z_USBEV_NET) to net
 *   net queued a frame and `usb`'s tx_tail had caught up with tx_head
 *       -> Z_USB_KICK to `usb`
 *
 * A side that finds its ring empty after draining it may sleep: a
 * frame added after that look sends the wake, and a message waits in
 * the mailbox until read.
 *
 * -- Frames, not packets --
 *
 * One whole Ethernet frame per slot (no FCS), at most Z_USBNET_FRAME
 * bytes. A full receive ring loses nothing: `usb` stops asking the
 * adapter, which holds the frame (NAKs) until there is room.
 */

#include <stdint.h>
#include <stdbool.h>

#define Z_USBNET_FRAME		1536
// A received frame starts this far into its slot (rx[i] + 2): then the
// IP header behind the 14-byte Ethernet header is word-aligned, and so
// is a TFTP block in it, and net's copies run a word at a time instead
// of a byte (newlib's memcpy only does words between buffers aligned
// alike). docs/net_throughput.md, round 3.
#define Z_USBNET_RX_OFF		2
#define Z_USBNET_RX_SLOTS	2
#define Z_USBNET_TX_SLOTS	2

// link: what the adapter's notification endpoint last said.
#define Z_USBNET_LINK_UNKNOWN   0
#define Z_USBNET_LINK_UP        1
#define Z_USBNET_LINK_DOWN      2

typedef struct {
	uint8_t present;        // an adapter is bound
	uint8_t mac_ok;         // mac[] was read from the adapter
	uint8_t link;           // Z_USBNET_LINK_*
	uint8_t promisc;        // the adapter was put in promiscuous mode
	uint8_t mac[6];         // the adapter's own address (iMACAddress)
	uint16_t maxseg;        // wMaxSegmentSize, 0 if not reported
	uint32_t gen;           // bumped by every bind and unbind
	uint32_t rx, tx;        // frames moved since `usb` started
	uint32_t rx_err, tx_err;
	uint32_t rx_drop;       // frames longer than a slot
} z_usbnet_info_t;

// In `usb`, written by `usb` only. Starts with a z_usbrep_t's two
// words (zusbproto.h), because it is NET_OPEN's reply.
typedef struct {
	int32_t result;
	uint32_t len;
	volatile uint32_t rx_head;		// frames put in rx[]
	volatile uint32_t tx_tail;		// frames taken from net's tx[]
	z_usbnet_info_t info;			// kept current by `usb`
	uint16_t rx_len[Z_USBNET_RX_SLOTS];
	uint32_t rx_at[Z_USBNET_RX_SLOTS];	// cycle counter when put: net's wait, measured
	uint8_t rx[Z_USBNET_RX_SLOTS][Z_USBNET_FRAME];
} z_usbnet_srv_t;

// In `net`, written by `net` only.
typedef struct {
	volatile uint32_t rx_tail;		// frames taken from rx[]
	volatile uint32_t tx_head;		// frames put in tx[]
	uint16_t tx_len[Z_USBNET_TX_SLOTS];
	uint8_t tx[Z_USBNET_TX_SLOTS][Z_USBNET_FRAME];
} z_usbnet_cli_t;

#ifndef ZUSBNET_STRUCTS_ONLY

// sw/common/zusbc.c.

// Fills *info. False with no `usb` (no controller, or not started).
bool z_usbnet_info(z_usbnet_info_t *info);

// Takes the adapter, with the address net sends from (an adapter
// plugged in later whose own address differs is made promiscuous, so
// it still receives frames for this one) and net's ring, which must
// stay where it is. Returns `usb`'s ring, or NULL.
const volatile z_usbnet_srv_t *z_usbnet_open(const uint8_t mac[6],
	z_usbnet_cli_t *cli);

// Tell `usb` a frame waits in a send ring it had emptied.
void z_usbnet_kick(void);

// Is the `usb` the last open was made with still running? False after
// it exits or restarts: its ring is gone, and the adapter must be
// opened again.
bool z_usbnet_alive(void);

#endif

#endif
