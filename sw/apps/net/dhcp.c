/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DHCP (RFC 2131/2132) client. See dhcp.h for the public API and the
 * "blocking-with-timeout, one-shot, no renewal" scope this was
 * deliberately built to. This file is the DISCOVER/OFFER/REQUEST/ACK
 * state machine and packet construct/parse; net.c's main() is the
 * only caller, right after eth_init() and before arp_init()/ip_init()/
 * tcp_init() are called with a real address (see dhcp.h's own
 * comment on dhcp_acquire() for exactly why the ordering matters).
 *
 * -- Why one-shot, no lease renewal --
 *
 * A real DHCP client re-requests the lease at T1 (typically 50% of
 * the lease time, unicast to the same server) and, if that fails,
 * again at T2 (~87.5%, broadcast to any server) before finally
 * dropping the address at expiry. None of that is implemented here:
 * dhcp_acquire() runs once at startup and whatever it returns is used
 * for the rest of the process's life, same as the static config it
 * replaces. This is a real functional gap on a network whose DHCP
 * server hands out short leases -- Zeitlos would keep using an
 * address the server may have long since reassigned to someone else.
 * Accepted deliberately for a first version, for the same reason
 * docs/networking.md gives for other simplifications throughout this
 * stack (TFTP's one-transfer-at-a-time, TCP's single TCB, etc.): this
 * is a dev-loop tool restarted often (every `run net` is a fresh
 * process, hence a fresh lease), on networks that typically hand out
 * long leases to begin with. Worth revisiting -- via a T1/T2-driven
 * unicast/broadcast re-REQUEST folded into net.c's normal
 * non-blocking main-loop poll, NOT this file's blocking acquire path
 * -- if a real deployment ever needs a `net` process to stay up
 * longer than its lease.
 *
 * -- Why blocking, unlike every other layer in this stack --
 *
 * arp.c/tftp.c/tcp.c are all non-blocking-poll by design (see their
 * own header comments) because net.c's main loop has other things to
 * do concurrently -- service messages from other processes, poll an
 * in-progress TFTP transfer, etc. -- while any one of those is
 * pending. Nothing else meaningful CAN happen before net has an
 * address: there's no other process to serve yet (net hasn't even
 * registered its pid -- see net.c's z_pid_register() call, which
 * happens after this), and ip_send() has nowhere useful to route
 * anything non-broadcast without our_ip/our_gateway actually set.
 * So dhcp_acquire() just runs its own tight eth_poll()-driven loop
 * and returns once it's done, exactly like z_port_connect_arg_timeout()
 * (sw/common/zport.c) or zstream.c's open/pull timeouts already do
 * for their own single-purpose blocking exchanges.
 *
 * -- Why every packet is broadcast, never unicast --
 *
 * RFC 2131 allows (and real servers sometimes use) a unicast
 * DHCPOFFER/DHCPACK straight to the offered address once the server
 * already knows the client's MAC (from the request's own chaddr
 * field) -- no ARP needed on the server's end, since it's addressing
 * a specific known MAC directly. This client always sets the
 * broadcast flag (RFC 2131 section 4.1, the top bit of the flags
 * field) in every DISCOVER/REQUEST it sends, asking any compliant
 * server to reply via 255.255.255.255 instead -- and ip.c's own
 * ip_handle() also relaxes its normal dst-IP filter while our_ip is
 * still 0.0.0.0 (see that file's comment) specifically so a server
 * that ignores the flag and unicasts anyway still gets through. The
 * REQUEST that follows a SELECTING-state OFFER is *always* broadcast
 * too, per RFC 2131 (ciaddr is still 0.0.0.0 at that point, same as
 * DISCOVER) -- this isn't a simplification, it's what the RFC itself
 * requires here. Combined, this means dhcp.c never needs ARP at all:
 * every send goes out ip_send()'s limited-broadcast fast path (see
 * ip.c), which goes straight to the Ethernet broadcast address.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "dhcp.h"
#include "udp.h"
#include "ip.h"
#include "eth.h"
#include "../../common/zeitlos.h"

#define DHCP_CLIENT_PORT  68
#define DHCP_SERVER_PORT  67

#define DHCP_MSG_DISCOVER  1
#define DHCP_MSG_OFFER     2
#define DHCP_MSG_REQUEST   3
#define DHCP_MSG_ACK       5
#define DHCP_MSG_NAK       6

#define DHCP_MAGIC_COOKIE  0x63825363u

// fixed BOOTP header: op(1) htype(1) hlen(1) hops(1) xid(4) secs(2)
// flags(2) ciaddr(4) yiaddr(4) siaddr(4) giaddr(4) chaddr(16)
// sname(64) file(128) = 236 bytes, then the 4-byte magic cookie
// before options start.
#define DHCP_BOOTP_LEN     236
#define DHCP_OPTIONS_OFF   (DHCP_BOOTP_LEN + 4)

// generous -- fixed header + magic cookie + our own outgoing options
// (message type, requested-IP, server-id, parameter list, client-id,
// end) comfortably fits well under 300 bytes; an incoming OFFER/ACK
// with a handful of vendor-specific options in the 300-548 byte range
// some servers add is still within UDP_MAX_PAYLOAD (udp.h), just
// truncated here at the options we actually look for -- see
// parse_reply()'s bounds check on every option it walks.
#define DHCP_BUF_LEN       320

// retry/timeout tuning -- same ~732Hz tick rate tcp.c's own
// TCP_RTO_TICKS_BASE comment (tcp.c) and z_uptime_ticks() (docs/
// networking.md) document elsewhere in this stack. Kept shorter than
// TCP's own backoff ceiling: a missing DHCP server should fail back
// to the static config (net.c) in well under the time a person
// watching boot output would call "hung", not TCP's ~30s worst case.
#define DHCP_TIMEOUT_TICKS   732        // ~1s per attempt
#define DHCP_MAX_ATTEMPTS    4          // per phase (DISCOVER, then REQUEST)

// -- reply state, filled in by the udp_listen(68, ...) callback below
// while dhcp_acquire()'s own wait loop spins on eth_poll(). Single
// static instance, same "only one thing can be in flight at a time"
// reasoning tftp.c's held_data/held_len/held_valid documents for
// itself -- there is only ever one DHCP transaction, ever, for the
// life of this process (see this file's header comment on why no
// renewal). --

static bool reply_pending;
static uint8_t reply_msg_type;
static uint32_t reply_yiaddr;
static uint32_t reply_server_id;   // option 54 -- 0 if absent (shouldn't be, but don't trust it)
static uint32_t reply_netmask;     // option 1 -- 0 if absent, caller defaults it
static uint32_t reply_gateway;     // option 3 -- 0 if absent (no gateway)
static uint32_t reply_dns;         // option 6 -- 0 if absent; only the FIRST address is kept, see dhcp.h
static uint32_t reply_lease;       // option 51, seconds -- unused beyond this file (no renewal), kept for a future one

static uint32_t our_xid;
static uint8_t our_mac[6];

static void reset_reply(void) {
	reply_pending = false;
	reply_msg_type = 0;
	reply_yiaddr = 0;
	reply_server_id = 0;
	reply_netmask = 0;
	reply_gateway = 0;
	reply_dns = 0;
	reply_lease = 0;
}

// parses a received DHCPOFFER/DHCPACK/DHCPNAK into the reply_*
// statics above. Returns false (leaving reply_pending false) for
// anything that isn't actually a well-formed reply to OUR request --
// wrong xid, wrong magic cookie, too short, missing message-type
// option -- so a stray broadcast from some other client's DHCP
// transaction on the same network never gets mistaken for ours.
static void handle_dhcp_reply(uint32_t src_ip, uint16_t src_port,
	const uint8_t *p, uint16_t len) {

	(void)src_ip;
	if (src_port != DHCP_SERVER_PORT) return;
	if (len < DHCP_OPTIONS_OFF) return;

	if (p[0] != 2) return;            // op: must be BOOTREPLY
	if (p[1] != 1 || p[2] != 6) return;   // htype=Ethernet, hlen=6

	uint32_t xid = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
		((uint32_t)p[6] << 8) | p[7];
	if (xid != our_xid) return;       // not a reply to our own transaction

	uint32_t cookie = ((uint32_t)p[DHCP_BOOTP_LEN] << 24) |
		((uint32_t)p[DHCP_BOOTP_LEN + 1] << 16) |
		((uint32_t)p[DHCP_BOOTP_LEN + 2] << 8) |
		p[DHCP_BOOTP_LEN + 3];
	if (cookie != DHCP_MAGIC_COOKIE) return;

	uint32_t yiaddr = ((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16) |
		((uint32_t)p[18] << 8) | p[19];

	uint8_t msg_type = 0;
	uint32_t server_id = 0, netmask = 0, gateway = 0, dns = 0, lease = 0;

	uint16_t i = DHCP_OPTIONS_OFF;
	while (i < len) {

		uint8_t code = p[i++];
		if (code == 0xFF) break;        // End
		if (code == 0x00) continue;     // Pad

		if (i >= len) break;            // truncated -- no room for a length byte
		uint8_t olen = p[i++];
		if ((uint32_t)i + olen > len) break;   // truncated option data

		switch (code) {
			case 53: if (olen >= 1) msg_type = p[i]; break;
			case 54: if (olen >= 4) server_id = ((uint32_t)p[i] << 24) |
				((uint32_t)p[i+1] << 16) | ((uint32_t)p[i+2] << 8) | p[i+3]; break;
			case 1:  if (olen >= 4) netmask = ((uint32_t)p[i] << 24) |
				((uint32_t)p[i+1] << 16) | ((uint32_t)p[i+2] << 8) | p[i+3]; break;
			case 3:  if (olen >= 4) gateway = ((uint32_t)p[i] << 24) |
				((uint32_t)p[i+1] << 16) | ((uint32_t)p[i+2] << 8) | p[i+3]; break;
			case 6:  if (olen >= 4) dns = ((uint32_t)p[i] << 24) |
				((uint32_t)p[i+1] << 16) | ((uint32_t)p[i+2] << 8) | p[i+3]; break;
				// option 6 (domain name server) may list several
				// addresses back to back (olen can be a multiple of
				// 4) -- only the first is kept, see dhcp.h's own
				// comment on out_dns for why
			case 51: if (olen >= 4) lease = ((uint32_t)p[i] << 24) |
				((uint32_t)p[i+1] << 16) | ((uint32_t)p[i+2] << 8) | p[i+3]; break;
			default: break;   // ignored -- e.g. domain name (15), T1/T2 (58/59, unused, no renewal)
		}

		i += olen;

	}

	if (msg_type != DHCP_MSG_OFFER && msg_type != DHCP_MSG_ACK &&
		msg_type != DHCP_MSG_NAK) return;   // not a message type we act on

	reply_pending = true;
	reply_msg_type = msg_type;
	reply_yiaddr = yiaddr;
	reply_server_id = server_id;
	reply_netmask = netmask;
	reply_gateway = gateway;
	reply_dns = dns;
	reply_lease = lease;

}

// builds and broadcasts one DHCP message. requested_ip/server_id are
// only meaningful (and only included, as options 50/54) for a
// DHCPREQUEST following an OFFER -- pass 0 for DHCPDISCOVER.
static void send_dhcp(uint8_t msg_type, uint32_t requested_ip, uint32_t server_id) {

	uint8_t pkt[DHCP_BUF_LEN];
	memset(pkt, 0, sizeof(pkt));

	pkt[0] = 1;      // op: BOOTREQUEST
	pkt[1] = 1;      // htype: Ethernet
	pkt[2] = 6;      // hlen
	pkt[3] = 0;      // hops

	pkt[4] = (our_xid >> 24) & 0xFF;
	pkt[5] = (our_xid >> 16) & 0xFF;
	pkt[6] = (our_xid >> 8) & 0xFF;
	pkt[7] = our_xid & 0xFF;

	// secs/ciaddr/yiaddr/siaddr/giaddr (8-27) all left 0 -- we have
	// no elapsed-time tracking worth reporting and, per this file's
	// header comment, never have a ciaddr to offer (every request,
	// including the REQUEST that follows SELECTING, is sent from
	// 0.0.0.0)

	pkt[10] = 0x80;  // flags: broadcast bit set (RFC 2131 4.1) -- see
	                 // this file's header comment on why every send
	                 // asks for a broadcast reply
	pkt[11] = 0x00;

	for (int j = 0; j < 6; j++) pkt[28 + j] = our_mac[j];   // chaddr

	pkt[DHCP_BOOTP_LEN + 0] = (DHCP_MAGIC_COOKIE >> 24) & 0xFF;
	pkt[DHCP_BOOTP_LEN + 1] = (DHCP_MAGIC_COOKIE >> 16) & 0xFF;
	pkt[DHCP_BOOTP_LEN + 2] = (DHCP_MAGIC_COOKIE >> 8) & 0xFF;
	pkt[DHCP_BOOTP_LEN + 3] = DHCP_MAGIC_COOKIE & 0xFF;

	uint16_t i = DHCP_OPTIONS_OFF;

	pkt[i++] = 53; pkt[i++] = 1; pkt[i++] = msg_type;   // DHCP message type

	if (msg_type == DHCP_MSG_REQUEST) {
		pkt[i++] = 50; pkt[i++] = 4;                     // requested IP
		pkt[i++] = (requested_ip >> 24) & 0xFF;
		pkt[i++] = (requested_ip >> 16) & 0xFF;
		pkt[i++] = (requested_ip >> 8) & 0xFF;
		pkt[i++] = requested_ip & 0xFF;
		pkt[i++] = 54; pkt[i++] = 4;                     // server identifier
		pkt[i++] = (server_id >> 24) & 0xFF;
		pkt[i++] = (server_id >> 16) & 0xFF;
		pkt[i++] = (server_id >> 8) & 0xFF;
		pkt[i++] = server_id & 0xFF;
	}

	pkt[i++] = 61; pkt[i++] = 7; pkt[i++] = 1;          // client identifier: htype=1 + our MAC
	for (int j = 0; j < 6; j++) pkt[i++] = our_mac[j];

	pkt[i++] = 55; pkt[i++] = 5;                        // parameter request list
	pkt[i++] = 1;    // subnet mask
	pkt[i++] = 3;    // router
	pkt[i++] = 6;    // domain name server
	pkt[i++] = 51;   // lease time
	pkt[i++] = 54;   // server identifier

	pkt[i++] = 0xFF;                                    // End

	// pad to at least 300 bytes total (BOOTP's own historical
	// minimum, still commonly expected by older/embedded DHCP
	// server implementations even though nothing in RFC 2131 itself
	// requires it) -- memset() above already zeroed the buffer, so
	// this is just extending the length, not writing new zero bytes
	if (i < 300) i = 300;

	udp_send(0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, pkt, i);

}

bool dhcp_acquire(const uint8_t mac[6],
	uint32_t *out_ip, uint32_t *out_netmask, uint32_t *out_gateway,
	uint32_t *out_dns) {

	for (int j = 0; j < 6; j++) our_mac[j] = mac[j];

	// varies per boot, not cryptographically random -- same
	// z_uptime_ticks()-seeded approach tcp.c's own next_local_port/
	// ISN use, and for the identical reason (see tcp.c's tcp_init()
	// comment): just needs to not collide with whatever a recent
	// prior boot's transaction used, so a slow-to-arrive stray reply
	// from an earlier attempt can't be mistaken for this one's.
	our_xid = z_uptime_ticks() * 2654435761u;   // Knuth multiplicative hash

	reset_reply();
	udp_listen(DHCP_CLIENT_PORT, handle_dhcp_reply);

	printf("net: dhcp: sending discover (xid=%08lx)\n", (unsigned long)our_xid);

	uint32_t offered_ip = 0, offered_server = 0;
	bool have_offer = false;

	// -- phase 1: DISCOVER -> OFFER --
	for (int attempt = 0; !have_offer && attempt < DHCP_MAX_ATTEMPTS; attempt++) {

		send_dhcp(DHCP_MSG_DISCOVER, 0, 0);

		uint32_t start = z_uptime_ticks();
		while ((z_uptime_ticks() - start) < DHCP_TIMEOUT_TICKS) {

			eth_poll();
			ip_poll();

			if (reply_pending) {
				if (reply_msg_type == DHCP_MSG_OFFER) {
					offered_ip = reply_yiaddr;
					offered_server = reply_server_id;
					have_offer = true;
					printf("net: dhcp: offer %ld.%ld.%ld.%ld from %ld.%ld.%ld.%ld\n",
						(long)(offered_ip >> 24 & 0xFF), (long)(offered_ip >> 16 & 0xFF),
						(long)(offered_ip >> 8 & 0xFF), (long)(offered_ip & 0xFF),
						(long)(offered_server >> 24 & 0xFF), (long)(offered_server >> 16 & 0xFF),
						(long)(offered_server >> 8 & 0xFF), (long)(offered_server & 0xFF));
				}
				reset_reply();
				if (have_offer) break;
				// some other message type (or a stray NAK with
				// nothing to NAK yet) -- keep waiting out this
				// attempt's timeout for a real OFFER
			}

		}

	}

	if (!have_offer) {
		printf("net: dhcp: no offer after %d attempt(s), giving up\n", DHCP_MAX_ATTEMPTS);
		udp_close(DHCP_CLIENT_PORT);
		return false;
	}

	// -- phase 2: REQUEST -> ACK/NAK --
	for (int attempt = 0; attempt < DHCP_MAX_ATTEMPTS; attempt++) {

		send_dhcp(DHCP_MSG_REQUEST, offered_ip, offered_server);

		uint32_t start = z_uptime_ticks();
		while ((z_uptime_ticks() - start) < DHCP_TIMEOUT_TICKS) {

			eth_poll();
			ip_poll();

			if (reply_pending) {

				uint8_t mt = reply_msg_type;
				uint32_t ip = reply_yiaddr;
				uint32_t netmask = reply_netmask;
				uint32_t gateway = reply_gateway;
				uint32_t dns = reply_dns;
				reset_reply();

				if (mt == DHCP_MSG_NAK) {
					printf("net: dhcp: request denied (nak), giving up\n");
					udp_close(DHCP_CLIENT_PORT);
					return false;
				}

				if (mt == DHCP_MSG_ACK) {

					// netmask defaults to a /24 if the server's ack
					// didn't include option 1 -- same ASSUMED-not-
					// confirmed fallback net.c's own static config
					// used before this file existed (see
					// docs/networking.md's old "Config" section);
					// still a reasonable default for a typical
					// home/office network, just no longer the ONLY
					// option now that a server can tell us for real.
					if (netmask == 0) netmask = 0xFFFFFF00u;

					printf("net: dhcp: ack -- %ld.%ld.%ld.%ld/",
						(long)(ip >> 24 & 0xFF), (long)(ip >> 16 & 0xFF),
						(long)(ip >> 8 & 0xFF), (long)(ip & 0xFF));
					printf("%ld.%ld.%ld.%ld gw ",
						(long)(netmask >> 24 & 0xFF), (long)(netmask >> 16 & 0xFF),
						(long)(netmask >> 8 & 0xFF), (long)(netmask & 0xFF));
					if (gateway)
						printf("%ld.%ld.%ld.%ld dns ",
							(long)(gateway >> 24 & 0xFF), (long)(gateway >> 16 & 0xFF),
							(long)(gateway >> 8 & 0xFF), (long)(gateway & 0xFF));
					else
						printf("(none) dns ");
					if (dns)
						printf("%ld.%ld.%ld.%ld\n",
							(long)(dns >> 24 & 0xFF), (long)(dns >> 16 & 0xFF),
							(long)(dns >> 8 & 0xFF), (long)(dns & 0xFF));
					else
						printf("(none)\n");

					*out_ip = ip;
					*out_netmask = netmask;
					*out_gateway = gateway;
					*out_dns = dns;

					udp_close(DHCP_CLIENT_PORT);
					return true;

				}

				// any other message type -- keep waiting out this
				// attempt's timeout for a real ACK/NAK

			}

		}

	}

	printf("net: dhcp: no ack after %d attempt(s), giving up\n", DHCP_MAX_ATTEMPTS);
	udp_close(DHCP_CLIENT_PORT);
	return false;

}
