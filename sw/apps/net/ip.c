/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * IPv4 + ICMP echo. See ip.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "ip.h"
#include "eth.h"
#include "arp.h"
#include "udp.h"
#include "tcp.h"
#include "../../common/zeitlos.h"

#define IP_HDR_LEN     20
#define IP_MAX_PAYLOAD 1480	// leaves room for the 20-byte header within ETH_MTU

static uint32_t our_ip;
static uint32_t our_netmask;
static uint32_t our_gateway;
static uint16_t next_ip_id = 1;

void ip_init(uint32_t ip, uint32_t netmask, uint32_t gateway_ip) {
	our_ip = ip;
	our_netmask = netmask;
	our_gateway = gateway_ip;
}

uint32_t ip_our_addr(void) {
	return our_ip;
}

static bool ip_is_local(uint32_t ip) {
	return (ip & our_netmask) == (our_ip & our_netmask);
}

// standard Internet checksum (RFC 1071): 16-bit one's complement sum
static uint16_t ip_checksum(const uint8_t *data, uint16_t len) {

	uint32_t sum = 0;

	for (uint16_t i = 0; i + 1 < len; i += 2)
		sum += ((uint32_t)data[i] << 8) | data[i + 1];

	if (len & 1)
		sum += (uint32_t)data[len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	return (uint16_t)(~sum);

}

bool ip_send(uint32_t dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t len) {

	static uint8_t pkt[IP_HDR_LEN + IP_MAX_PAYLOAD];
	if (len > IP_MAX_PAYLOAD) return false;

	uint16_t total_len = IP_HDR_LEN + len;

	pkt[0] = 0x45;	// version 4, IHL 5 (20 bytes, no options)
	pkt[1] = 0x00;	// DSCP/ECN
	pkt[2] = (total_len >> 8) & 0xFF;
	pkt[3] = total_len & 0xFF;
	pkt[4] = (next_ip_id >> 8) & 0xFF;
	pkt[5] = next_ip_id & 0xFF;
	next_ip_id++;
	pkt[6] = 0x40; pkt[7] = 0x00;	// flags: DF (don't fragment) set, no offset -- we don't reassemble fragments
	pkt[8] = 64;					// TTL
	pkt[9] = protocol;
	pkt[10] = 0; pkt[11] = 0;		// checksum, filled in below
	pkt[12] = (our_ip >> 24) & 0xFF;
	pkt[13] = (our_ip >> 16) & 0xFF;
	pkt[14] = (our_ip >> 8) & 0xFF;
	pkt[15] = our_ip & 0xFF;
	pkt[16] = (dst_ip >> 24) & 0xFF;
	pkt[17] = (dst_ip >> 16) & 0xFF;
	pkt[18] = (dst_ip >> 8) & 0xFF;
	pkt[19] = dst_ip & 0xFF;

	uint16_t csum = ip_checksum(pkt, IP_HDR_LEN);
	pkt[10] = (csum >> 8) & 0xFF;
	pkt[11] = csum & 0xFF;

	for (uint16_t i = 0; i < len; i++) pkt[IP_HDR_LEN + i] = payload[i];

	uint32_t next_hop = ip_is_local(dst_ip) ? dst_ip : our_gateway;

	uint8_t dst_mac[6];
	if (!arp_lookup(next_hop, dst_mac)) {
		arp_request(next_hop);
		return false;
	}

	return eth_send(dst_mac, ETHERTYPE_IPV4, pkt, total_len);

}

static void icmp_handle(uint32_t src_ip, const uint8_t *p, uint16_t len) {

	if (len < 8) return;

	uint8_t type = p[0];
	if (type != 8) return;	// only handle echo request (type 8) for now

	if (len > IP_MAX_PAYLOAD) return;	// implausibly large ping, ignore

	printf("net: icmp echo request from %ld.%ld.%ld.%ld, replying\n",
		(long)(src_ip >> 24 & 0xFF), (long)(src_ip >> 16 & 0xFF),
		(long)(src_ip >> 8 & 0xFF), (long)(src_ip & 0xFF));

	// echo reply: identical payload (identifier/sequence/data),
	// type changed to 0 (echo reply), checksum recomputed
	static uint8_t reply[IP_MAX_PAYLOAD];
	for (uint16_t i = 0; i < len; i++) reply[i] = p[i];
	reply[0] = 0;	// type: echo reply
	reply[1] = 0;	// code
	reply[2] = 0; reply[3] = 0;	// checksum, filled in below

	uint16_t csum = ip_checksum(reply, len);
	reply[2] = (csum >> 8) & 0xFF;
	reply[3] = csum & 0xFF;

	ip_send(src_ip, 1, reply, len);	// protocol 1 = ICMP

}

void ip_handle(const uint8_t src_mac[6], const uint8_t *p, uint16_t len) {

	(void)src_mac;

	if (len < IP_HDR_LEN) return;

	uint8_t ihl = (p[0] & 0x0F) * 4;
	if (ihl < IP_HDR_LEN || len < ihl) return;

	uint16_t total_len = (p[2] << 8) | p[3];
	if (total_len > len) return;	// truncated frame

	uint8_t protocol = p[9];
	uint32_t src_ip = ((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16) |
		((uint32_t)p[14] << 8) | p[15];
	uint32_t dst_ip = ((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16) |
		((uint32_t)p[18] << 8) | p[19];

	// accept unicast addressed to us, or full broadcast -- not
	// attempting subnet-broadcast or multicast handling yet
	if (dst_ip != our_ip && dst_ip != 0xFFFFFFFFu) return;

	const uint8_t *payload = p + ihl;
	uint16_t paylen = total_len - ihl;

	switch (protocol) {
		case 1:	// ICMP
			icmp_handle(src_ip, payload, paylen);
			break;
		case 6:		// TCP
			tcp_handle(src_ip, payload, paylen);
			break;
		case 17:	// UDP
			udp_handle(src_ip, payload, paylen);
			break;
		default:
			break;
	}

}
