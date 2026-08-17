/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * ARP. See arp.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "arp.h"
#include "eth.h"

#define ARP_CACHE_SIZE  8

typedef struct {
	bool used;
	uint32_t ip;
	uint8_t mac[6];
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint32_t our_ip;

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void arp_init(uint32_t ip) {
	our_ip = ip;
	for (int i = 0; i < ARP_CACHE_SIZE; i++) arp_cache[i].used = false;
}

static void arp_cache_insert(uint32_t ip, const uint8_t mac[6]) {

	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (arp_cache[i].used && arp_cache[i].ip == ip) {
			for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
			return;
		}
	}

	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!arp_cache[i].used) {
			arp_cache[i].used = true;
			arp_cache[i].ip = ip;
			for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
			printf("net: arp learned %ld.%ld.%ld.%ld = %02x:%02x:%02x:%02x:%02x:%02x\n",
				(long)(ip >> 24 & 0xFF), (long)(ip >> 16 & 0xFF),
				(long)(ip >> 8 & 0xFF), (long)(ip & 0xFF),
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
			return;
		}
	}

	// cache full -- evict slot 0. simple, not LRU; fine for a
	// handful of hosts on a dev network.
	arp_cache[0].used = true;
	arp_cache[0].ip = ip;
	for (int j = 0; j < 6; j++) arp_cache[0].mac[j] = mac[j];

}

bool arp_lookup(uint32_t ip, uint8_t mac_out[6]) {

	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (arp_cache[i].used && arp_cache[i].ip == ip) {
			for (int j = 0; j < 6; j++) mac_out[j] = arp_cache[i].mac[j];
			return true;
		}
	}

	return false;

}

static void arp_send(uint16_t oper, const uint8_t target_mac[6], uint32_t target_ip) {

	uint8_t p[28];

	p[0] = 0x00; p[1] = 0x01;	// hardware type: Ethernet
	p[2] = 0x08; p[3] = 0x00;	// protocol type: IPv4
	p[4] = 6;					// hardware address length
	p[5] = 4;					// protocol address length
	p[6] = (oper >> 8) & 0xFF; p[7] = oper & 0xFF;

	for (int i = 0; i < 6; i++) p[8 + i] = eth_our_mac[i];	// sender hw addr
	p[14] = (our_ip >> 24) & 0xFF;
	p[15] = (our_ip >> 16) & 0xFF;
	p[16] = (our_ip >> 8) & 0xFF;
	p[17] = our_ip & 0xFF;								// sender proto addr

	for (int i = 0; i < 6; i++)						// target hw addr
		p[18 + i] = target_mac ? target_mac[i] : 0x00;	// (0 for requests -- unknown)
	p[24] = (target_ip >> 24) & 0xFF;
	p[25] = (target_ip >> 16) & 0xFF;
	p[26] = (target_ip >> 8) & 0xFF;
	p[27] = target_ip & 0xFF;							// target proto addr

	eth_send(target_mac ? target_mac : bcast_mac, ETHERTYPE_ARP, p, sizeof(p));

}

void arp_request(uint32_t ip) {
	arp_send(1, NULL, ip);	// operation 1 = request
}

void arp_handle(const uint8_t src_mac[6], const uint8_t *p, uint16_t len) {

	(void)src_mac;

	if (len < 28) return;	// too short to be a valid Ethernet/IPv4 ARP packet

	uint16_t htype = (p[0] << 8) | p[1];
	uint16_t ptype = (p[2] << 8) | p[3];
	uint8_t hlen = p[4];
	uint8_t plen = p[5];
	uint16_t oper = (p[6] << 8) | p[7];

	if (htype != 1 || ptype != 0x0800 || hlen != 6 || plen != 4)
		return;	// not an Ethernet/IPv4 ARP packet, ignore

	const uint8_t *sha = &p[8];
	uint32_t spa = ((uint32_t)p[14] << 24) | ((uint32_t)p[15] << 16) |
		((uint32_t)p[16] << 8) | p[17];
	uint32_t tpa = ((uint32_t)p[24] << 24) | ((uint32_t)p[25] << 16) |
		((uint32_t)p[26] << 8) | p[27];

	// learn the sender's mapping from any ARP traffic we see, not
	// just replies to our own requests
	arp_cache_insert(spa, sha);

	if (oper == 1 && tpa == our_ip) {
		printf("net: arp who-has us from %ld.%ld.%ld.%ld, replying\n",
			(long)(spa >> 24 & 0xFF), (long)(spa >> 16 & 0xFF),
			(long)(spa >> 8 & 0xFF), (long)(spa & 0xFF));
		arp_send(2, sha, spa);	// operation 2 = reply
	}

}
