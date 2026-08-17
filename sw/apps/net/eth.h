#ifndef ETH_H
#define ETH_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Ethernet framing: builds/parses the 14-byte Ethernet header and
 * dispatches received frames to arp.c/ip.c by ethertype. Everything
 * below this (enc28j60.c) doesn't know about frame contents at all;
 * everything above it (arp.c, ip.c) never touches the chip directly.
 */

#include <stdint.h>
#include <stdbool.h>

#define ETH_ADDR_LEN  6
#define ETH_HDR_LEN   14
#define ETH_MTU       1518	// max standard Ethernet frame

#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806

// this interface's own MAC, set by eth_init()
extern uint8_t eth_our_mac[6];

void eth_init(const uint8_t mac[6]);

// builds and sends one frame: dst_mac + our own MAC + ethertype +
// payload, padded to the minimum frame size if needed.
bool eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
	const uint8_t *payload, uint16_t len);

// drains every currently-pending received frame, dispatching each to
// arp_handle()/ip_handle() by ethertype. call this every iteration of
// your own main loop.
void eth_poll(void);

#endif
