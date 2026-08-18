#ifndef RMII_ETH_H
#define RMII_ETH_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Driver for rtl/ethmac_rmii.v, the in-fabric RMII Ethernet MAC
 * (LAN8720A PHY). mozart_ml1 only. Alternative to enc28j60.c/h for
 * boards with an RMII PHY instead of the SPI ENC28J60 PMOD -- see
 * eth.c for how the two are selected (a build-time choice, NET_PHY in
 * sw/apps/net/Makefile, not a runtime probe -- see that Makefile's
 * comment for why).
 *
 * Deliberately the same function shapes as enc28j60.h so eth.c's
 * calls into this file are a near-identical swap. Two real
 * differences worth knowing, though:
 *
 * 1. No destination-MAC filtering. The ENC28J60 filters incoming
 *    frames in hardware (ERXFCON, not audited as part of this
 *    driver); rtl/ethmac_rmii.v has no such filter -- no MDIO to
 *    configure one, and it wasn't worth building into fabric logic
 *    for a first version. This MAC receives EVERY frame that
 *    reaches the wire (broadcast, multicast, unicast to other
 *    hosts, all of it), not just ones addressed to `mac`. Harmless
 *    functionally -- arp.c/ip.c only act on frames whose contents
 *    they recognize -- but on a busy LAN it means the single RX
 *    buffer slot (see rtl/ethmac_rmii.v's header comment) fills with
 *    irrelevant traffic more often, which shows up as a higher
 *    rx_drop_count than the ENC28J60 backend would see under the
 *    same conditions.
 *
 * 2. `mac` is accepted for API-shape symmetry with enc28j60_init()
 *    but otherwise unused here -- there's no hardware register to
 *    program it into (no address filter to program it, no factory
 *    MAC to compare against). eth.c already keeps its own copy
 *    (eth_our_mac) for building outgoing frames' source address;
 *    that's the only place it actually matters.
 */

#include <stdint.h>
#include <stdbool.h>

// call once at startup. always returns true (there's no chip to
// fail to respond -- if rtl/ethmac_rmii.v wasn't built into this
// bitstream, reads from its address range are simply meaningless,
// not detectably absent -- see rmii_eth.c's comment on this).
// prints a courtesy diagnostic (ETH_REFCLK heartbeat, CRS_DV) but
// doesn't gate on it.
bool rmii_eth_init(const uint8_t mac[6]);

// prints reg_ethmac_status decoded, plus the drop/error counters --
// for checking whether the hardware itself thinks something is
// wrong before assuming a software bug.
void rmii_eth_debug_dump(void);

// non-blocking: returns the number of bytes received (0 if no frame
// is waiting). writes into buf, up to maxlen bytes -- frames longer
// than maxlen are truncated (same contract as enc28j60_recv(): the
// rest is still consumed/released from the hardware buffer, just not
// copied out, and the truncated length is what's returned).
uint16_t rmii_eth_recv(uint8_t *buf, uint16_t maxlen);

// blocks until the frame is transmitted or a timeout is hit. returns
// true on success. len must not exceed the hardware TX buffer size
// (REG_ETHMAC_TXBUF_WORDS*4 bytes, see zeitlos.h) -- eth_send()'s own
// ETH_MTU-sized txbuf is already well under that, so this should
// never actually trigger in normal use.
bool rmii_eth_send(const uint8_t *buf, uint16_t len);

#endif
