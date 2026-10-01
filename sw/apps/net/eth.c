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

static uint8_t txbuf[ETH_MTU];
static uint8_t rxbuf[ETH_MTU];

void eth_init(const uint8_t mac[6]) {
	for (int i = 0; i < 6; i++) eth_our_mac[i] = mac[i];
}

bool eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
	const uint8_t *payload, uint16_t len) {

	if ((uint32_t)ETH_HDR_LEN + len > sizeof(txbuf)) return false;

	for (int i = 0; i < 6; i++) txbuf[i] = dst_mac[i];
	for (int i = 0; i < 6; i++) txbuf[6 + i] = eth_our_mac[i];
	txbuf[12] = (ethertype >> 8) & 0xFF;
	txbuf[13] = ethertype & 0xFF;

	memcpy(txbuf + ETH_HDR_LEN, payload, len);

	uint16_t framelen = ETH_HDR_LEN + len;

	if (framelen < 60) {
		// zero the padding explicitly -- txbuf is reused across
		// calls, so without this, leftover bytes from a previous,
		// longer send could leak into this frame's padding
		for (uint16_t i = framelen; i < 60; i++) txbuf[i] = 0;
		framelen = 60;
	}

	return phy_send(txbuf, framelen);

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
