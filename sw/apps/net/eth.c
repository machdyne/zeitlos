/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Ethernet framing. See eth.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "eth.h"
#include "net_phy.h"
#include "arp.h"
#include "ip.h"
#include "../../common/zeitlos.h"
#include <string.h>

uint8_t eth_our_mac[6];

// The transmitted buffer starts 2 mod 4, so buf+14 (the payload,
// after the header written a byte at a time below) is 4-aligned.
// libz memcpy copies a word at a time only when both ends are, and
// the TCP segment it copies from already is. A struct of plain
// uint8_t is aligned to 1 unless asked: without the attribute the
// word path would never run. rxbuf stays where it is; the copy this
// is about is the one in eth_send().
static struct {
	uint8_t pad[2];
	uint8_t buf[ETH_MTU];
} tx __attribute__((aligned(4)));
_Static_assert(__alignof__(tx) >= 4 &&
	__builtin_offsetof(__typeof__(tx), buf) == 2,
	"payload at tx.buf+14 must be word-aligned");
static uint8_t rxbuf[ETH_MTU];

void eth_init(const uint8_t mac[6]) {
	for (int i = 0; i < 6; i++) eth_our_mac[i] = mac[i];
}

bool eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
	const uint8_t *payload, uint16_t len) {

	if ((uint32_t)ETH_HDR_LEN + len > sizeof(tx.buf)) return false;

	for (int i = 0; i < 6; i++) tx.buf[i] = dst_mac[i];
	for (int i = 0; i < 6; i++) tx.buf[6 + i] = eth_our_mac[i];
	tx.buf[12] = (ethertype >> 8) & 0xFF;
	tx.buf[13] = ethertype & 0xFF;

	memcpy(tx.buf + ETH_HDR_LEN, payload, len);

	uint16_t framelen = ETH_HDR_LEN + len;

	if (framelen < 60) {
		// zero the padding explicitly -- tx.buf is reused across
		// calls, so without this, leftover bytes from a previous,
		// longer send could leak into this frame's padding
		for (uint16_t i = framelen; i < 60; i++) tx.buf[i] = 0;
		framelen = 60;
	}

	return phy_send(tx.buf, framelen);

}

void eth_poll(void) {

	uint16_t len;
	int drained = 0;

	// Bounded so a driver that never returns 0 cannot spin here
	// forever. The test has to come BEFORE phy_recv(): the old one
	// sat after it, so the 65th frame was already taken off the NIC
	// (and, on the ENC28J60, PKTDEC'd) and then thrown away unprocessed.
	// Stopping at 64 leaves that frame queued. A driver whose pin
	// stays asserted until the queue is empty -- the ENC28J60 -- is
	// told to make another edge, because a level is not a second IRQ.
	while (drained < 64) {

		len = phy_recv(rxbuf, sizeof(rxbuf));
		if (len == 0) break;
		drained++;

		if (len < ETH_HDR_LEN) continue;

		uint16_t ethertype = (rxbuf[12] << 8) | rxbuf[13];
		const uint8_t *src_mac = &rxbuf[6];
		const uint8_t *payload = &rxbuf[ETH_HDR_LEN];
		uint16_t paylen = len - ETH_HDR_LEN;

		// printf("net: rx frame ethertype=0x%04x len=%d\n", ethertype, len);

		switch (ethertype) {
			case ETHERTYPE_ARP:
				arp_handle(src_mac, payload, paylen);
				break;
			case ETHERTYPE_IPV4:
				ip_handle(src_mac, payload, paylen);
				break;
			default:
				break;
		}

	}

	if (drained == 64) phy_rx_rearm();

}
