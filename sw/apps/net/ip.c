/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * IPv4 + ICMP echo. See ip.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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

// -- one-slot pending-packet queue, for when ip_send() (below) can't
// reach the next hop's MAC yet --
//
// ip_send()'s own header comment (ip.h) documents the intended
// contract: a false return "kicks off resolution... the caller should
// just retry". Nothing actually implemented that retry -- tcp.c's
// send_tracked()/tcp_poll() never check tcp_send_segment()'s return
// value at all, so a fresh outbound TCP connection's very first SYN
// (needing ARP resolution almost always, since a brand-new peer is
// essentially never already cached) silently vanished, with only
// tcp.c's own multi-second RTO backoff as a chance to try again --
// and even that could keep losing the race on a busy network, since
// arp.c's own cache eviction policy ("evict slot 0", not LRU) could
// kick a resolved entry back out before the next retry needed it
// again. Confirmed as the root cause of a real "TCP handshake never
// happens, even against a server confirmed reachable from another
// host" symptom on real hardware.
//
// This queues the fully-built packet (header, payload, checksum
// already computed) here instead of just dropping it, and ip_poll()
// (called every iteration of net's own main loop, same as eth_poll()/
// tcp_poll()/etc.) actually sends it the moment the ARP cache shows
// the next hop resolved -- typically within a millisecond or two on a
// local network, not tcp.c's own RTO backoff (which grows to several
// seconds by its own later retries).
//
// Single slot: a second ip_send() while one is already pending
// overwrites it, silently reproducing this same bug for whichever one
// gets overwritten. Fine for how this is actually used today -- every
// current caller (tcp.c, udp.c, this file's own ICMP reply) has at
// most one outstanding send that could need this at a time -- worth
// revisiting with a real per-destination queue if a caller ever needs
// more than one concurrently pending target.
static bool pending_valid;
static uint32_t pending_next_hop;
static uint8_t pending_pkt[IP_HDR_LEN + IP_MAX_PAYLOAD];
static uint16_t pending_len;

// call every main-loop iteration (net.c) -- flushes the pending
// packet above the moment its next hop's MAC resolves. A no-op
// (single `if`, cheap) on every call where nothing's pending, which
// is the overwhelming majority of calls in practice.
void ip_poll(void) {

	if (!pending_valid) return;

	uint8_t dst_mac[6];
	if (!arp_lookup(pending_next_hop, dst_mac)) return;	// still waiting

	pending_valid = false;
	eth_send(dst_mac, ETHERTYPE_IPV4, pending_pkt, pending_len);

}


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

	// limited broadcast (255.255.255.255) never goes through ARP --
	// it's not a real host to resolve, and there may not even BE an
	// ARP-resolvable next hop yet (this is exactly the situation
	// dhcp.c's DISCOVER/REQUEST are sent in: our_ip is still 0.0.0.0,
	// there's no gateway to speak of, and the whole point of the
	// packet is reaching a server we can't already address any other
	// way). Goes straight to the link-layer broadcast address instead.
	if (dst_ip == 0xFFFFFFFFu) {
		static const uint8_t bcast_mac[6] =
			{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
		return eth_send(bcast_mac, ETHERTYPE_IPV4, pkt, total_len);
	}

	uint32_t next_hop = ip_is_local(dst_ip) ? dst_ip : our_gateway;

	uint8_t dst_mac[6];
	if (!arp_lookup(next_hop, dst_mac)) {
		arp_request(next_hop);
		// queue this fully-built packet (header/payload/checksum
		// already done above) so ip_poll() can actually send it once
		// ARP resolves, instead of just dropping it here -- see that
		// function's own comment, and the pending-packet fields just
		// above it, for the full story.
		pending_next_hop = next_hop;
		memcpy(pending_pkt, pkt, total_len);
		pending_len = total_len;
		pending_valid = true;
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
	if (total_len < ihl) return;	// declared total length shorter than
					// the header alone -- malformed
					// (possible with IP options present,
					// since ihl can exceed the 20-byte
					// minimum but total_len is otherwise
					// unvalidated against it). Without this,
					// `total_len - ihl` below underflows as
					// unsigned arithmetic (e.g. 20-24 wraps
					// to 65532), handing icmp_handle()/
					// tcp_handle()/udp_handle() a huge, bogus
					// payload length far beyond the actual
					// received frame -- a real, confirmed gap,
					// found by inspection during a real-
					// hardware crash investigation, though
					// never confirmed as that crash's actual
					// cause.

	uint8_t protocol = p[9];
	uint32_t src_ip = ((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16) |
		((uint32_t)p[14] << 8) | p[15];
	uint32_t dst_ip = ((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16) |
		((uint32_t)p[18] << 8) | p[19];

	// accept unicast addressed to us, or full broadcast -- not
	// attempting subnet-broadcast or multicast handling yet.
	//
	// Exception: while we have no address yet (our_ip == 0.0.0.0,
	// true only during dhcp.c's own DISCOVER/REQUEST exchange at
	// startup, before ip_init() is called again with a real address
	// -- see net.c's main()), accept anything. This is what makes a
	// DHCPOFFER/DHCPACK actually reachable at all: some servers honor
	// the client's broadcast flag (dhcp.c always sets it) and reply
	// to 255.255.255.255, which the check above already lets through
	// regardless -- but others unicast straight to the still-being-
	// offered address instead (valid per RFC 2131, and common in
	// practice: the server already knows our MAC from the request's
	// chaddr field, so it doesn't need ARP either), which is a dst_ip
	// we have no way to pre-validate against anything meaningful yet.
	// Harmless to relax here specifically: this only widens what
	// reaches udp_handle()'s dst-port dispatch during the brief
	// bootstrap window, and dhcp.c's own xid check (dhcp.c) rejects
	// anything that isn't actually a reply to our own request.
	if (dst_ip != our_ip && dst_ip != 0xFFFFFFFFu && our_ip != 0) return;

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
