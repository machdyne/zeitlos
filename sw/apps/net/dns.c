/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DNS (RFC 1035) client. See dns.h for the public API; this file is
 * the query construction, response parsing, and retry/timeout state
 * machine behind it.
 *
 * -- Scope: A records only, no caching, no CNAME following --
 *
 * Only asks for, and only looks at, type-A (IPv4 address) answer
 * records. A server that replies with a CNAME chain instead of (or
 * before) an A record -- common for e.g. a hostname that's actually
 * an alias to another name -- is treated as "no A record in
 * response", a resolve failure, even though a real resolver would
 * follow the CNAME and try again. No result caching either: every
 * z_dns_resolve() call (sw/common/zdns.h) is a fresh query on the
 * wire, even for the same hostname resolved a moment ago. Both are
 * real, deliberate scope cuts for a first version -- same spirit as
 * dhcp.c's own "no lease renewal" cut, and TFTP's one-transfer-at-a-
 * time -- not oversights. Worth revisiting if a real use ever needs
 * to resolve a CNAME-fronted hostname, or resolves the same name
 * often enough that the extra round trips start to matter.
 *
 * -- One resolution in flight at a time --
 *
 * Same simplification as TFTP's one transfer/TCP's one connection
 * elsewhere in this app: a single static instance of the pending-
 * query state below, not a table. A Z_NET_DNS_RESOLVE that arrives
 * while one is already in progress gets an immediate "busy" reply
 * (see dns_resolve_start()), not queued.
 *
 * -- Name compression on the reply, but not on our own query --
 *
 * Our own outgoing query always writes the question name out in
 * full (build_query() below) -- there's nothing to compress against
 * yet, being the first and only name in the packet. A server's
 * REPLY very commonly points its answer record's NAME field back at
 * the question via a compression pointer (RFC 1035 4.1.4) instead of
 * repeating it, though -- skip_name() below handles that (and plain
 * uncompressed labels) for exactly as much as this file needs: moving
 * an offset past a NAME field it doesn't otherwise care about the
 * actual bytes of. It does NOT reconstruct the name a pointer refers
 * to (nothing here needs to -- see "Scope" above, we only ever look
 * at the answer's TYPE/CLASS/RDATA, never its NAME), and does NOT
 * follow chained/nested pointers beyond the one hop RFC 1035 itself
 * describes (a pointer's target is defined to not itself be another
 * pointer in a well-formed message, so one hop is always correct for
 * a compliant server; a hostile or badly broken one could in
 * principle violate that, but this is a dev-tool DNS client on a
 * trusted local network, not a hardened parser for adversarial
 * input).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "dns.h"
#include "udp.h"
#include "../../common/zeitlos.h"
#include "../../common/znet.h"

#define DNS_SERVER_PORT      53
#define DNS_MAX_HOSTNAME_LEN 64

// enough for the fixed 12-byte header, a DNS_MAX_HOSTNAME_LEN-byte
// hostname encoded as length-prefixed labels (adds at most one extra
// length byte per label plus the final zero terminator -- comfortably
// under hostname_len + 8 even for a maximally dotted name), and the
// 4-byte QTYPE/QCLASS trailer.
#define DNS_PKT_MAX  (12 + DNS_MAX_HOSTNAME_LEN + 8 + 4)

// retry/timeout tuning -- same ~732Hz tick rate documented throughout
// this app (tcp.c's TCP_RTO_TICKS_BASE comment, docs/networking.md's
// "z_uptime_ticks()"). Shorter than TCP's own backoff, similar
// reasoning to dhcp.c's own DHCP_TIMEOUT_TICKS: a real nameserver on
// a local network answers in single-digit milliseconds, so there's no
// reason to wait long per attempt -- and sw/common/zdns.h's own outer
// ZDNS_TIMEOUT_TICKS budget needs this file's total retry budget to
// fit comfortably inside it.
#define DNS_TIMEOUT_TICKS   366   // ~0.5s
#define DNS_MAX_RETRIES     3

typedef enum { DNS_IDLE, DNS_QUERYING } dns_state_t;

static dns_state_t state = DNS_IDLE;
static uint32_t nameserver_ip;

static uint16_t our_qid;
static uint16_t local_port;
static char pending_hostname[DNS_MAX_HOSTNAME_LEN + 1];
static uint32_t requester_pid;
static uint32_t requester_tag;
static uint32_t last_tx_ticks;
static uint8_t retries;
static uint8_t last_pkt[DNS_PKT_MAX];
static uint16_t last_pkt_len;

static uint16_t next_local_port(void) {
	static uint16_t p = 49151;
	p++;
	if (p < 49152) p = 49152;	// stay in the ephemeral range
	return p;
}

static uint16_t next_qid(void) {
	// varies per query, not cryptographically random -- same
	// z_uptime_ticks()-seeded approach dhcp.c's own xid and tcp.c's
	// ISN/next_local_port use, for the same reason: just needs to not
	// collide with a slow-to-arrive stray reply from an earlier
	// query, not to resist a hostile guesser on this trusted local
	// dev network.
	return (uint16_t)(z_uptime_ticks() * 2654435761u);
}

static void print_ip(uint32_t ip) {
	printf("%ld.%ld.%ld.%ld",
		(long)((ip >> 24) & 0xFF), (long)((ip >> 16) & 0xFF),
		(long)((ip >> 8) & 0xFF), (long)(ip & 0xFF));
}

void dns_set_nameserver(uint32_t ip) {
	nameserver_ip = ip;
	if (ip) {
		printf("net: dns: nameserver ");
		print_ip(ip);
		printf("\n");
	} else {
		printf("net: dns: no nameserver configured -- hostname lookups "
			"will fail until one is (see docs/networking.md)\n");
	}
}

// encodes hostname as DNS length-prefixed labels into pkt+offset,
// followed by QTYPE=A/QCLASS=IN. Returns the new offset, or 0 on a
// malformed hostname (empty label, e.g. leading/trailing/doubled
// dot, or a label over the RFC 1035 63-byte limit) -- callers must
// treat a 0 return as "don't send this".
static uint16_t encode_question(uint8_t *pkt, uint16_t offset, const char *hostname) {

	const char *label_start = hostname;

	while (1) {

		const char *p = label_start;
		while (*p && *p != '.') p++;

		uint32_t label_len = (uint32_t)(p - label_start);
		if (label_len == 0 || label_len > 63) return 0;

		pkt[offset++] = (uint8_t)label_len;
		for (uint32_t j = 0; j < label_len; j++)
			pkt[offset++] = (uint8_t)label_start[j];

		if (*p == 0) break;
		label_start = p + 1;

	}

	pkt[offset++] = 0x00;	// root label -- terminates QNAME

	pkt[offset++] = 0x00; pkt[offset++] = 0x01;	// QTYPE  = A
	pkt[offset++] = 0x00; pkt[offset++] = 0x01;	// QCLASS = IN

	return offset;

}

static uint16_t build_query(uint16_t qid, const char *hostname, uint8_t *pkt) {

	pkt[0] = (qid >> 8) & 0xFF; pkt[1] = qid & 0xFF;
	pkt[2] = 0x01; pkt[3] = 0x00;	// flags: RD (recursion desired) = 1, standard query
	pkt[4] = 0x00; pkt[5] = 0x01;	// QDCOUNT = 1
	pkt[6] = 0x00; pkt[7] = 0x00;	// ANCOUNT = 0
	pkt[8] = 0x00; pkt[9] = 0x00;	// NSCOUNT = 0
	pkt[10] = 0x00; pkt[11] = 0x00;	// ARCOUNT = 0

	return encode_question(pkt, 12, hostname);

}

// moves past a NAME field (a question's QNAME, or an answer record's
// own NAME) without needing to reconstruct it -- see this file's own
// header comment ("Name compression on the reply") for why that's
// all this needs to do. Returns len (clamped) on anything that runs
// off the end of the buffer, which every caller below already treats
// as "stop parsing, this record/packet is malformed" via its own
// subsequent bounds checks.
static uint16_t skip_name(const uint8_t *p, uint16_t offset, uint16_t len) {

	while (offset < len) {

		uint8_t b = p[offset];

		if ((b & 0xC0) == 0xC0) return offset + 2;	// compression pointer -- always exactly 2 bytes here
		if (b == 0) return offset + 1;				// root label -- name ends here

		uint32_t next = (uint32_t)offset + 1 + b;
		if (next > len) return len;
		offset = (uint16_t)next;

	}

	return offset;

}

static void finish_query(bool ok, uint32_t ip, const char *err) {

	if (ok) {
		printf("net: dns: %s -> ", pending_hostname);
		print_ip(ip);
		printf("\n");
	} else {
		printf("net: dns: %s -> failed: %s\n", pending_hostname, err);
	}

	z_obj_t reply = z_obj_map(2);
	z_map_set(&reply, "ok", z_obj_uint32(ok ? 1 : 0));
	if (ok)
		z_map_set(&reply, "ip", z_obj_uint32(ip));
	else
		z_map_set(&reply, "error", z_obj_str(err));
	// `reply` intentionally never freed -- same one-shot, bounded-cost
	// borrowed-reply tradeoff net.c's own reply_error() documents for
	// itself (docs/messaging.md).
	z_msg_new_send(requester_pid, Z_NET_DNS_RESOLVE_REPLY, requester_tag, reply);

	udp_close(local_port);
	state = DNS_IDLE;

}

// dispatched from udp.c for whatever local_port this query is using.
static void handle_dns_reply(uint32_t src_ip, uint16_t src_port,
	const uint8_t *p, uint16_t len) {

	if (state != DNS_QUERYING) return;

	// only trust the server we actually asked, on the port DNS
	// servers actually answer from -- guards against a stray/forged
	// UDP packet elsewhere on the network landing on this same
	// ephemeral local port being mistaken for our reply.
	if (src_ip != nameserver_ip || src_port != DNS_SERVER_PORT) return;

	if (len < 12) return;	// shorter than a DNS header -- not a real reply

	uint16_t qid = ((uint16_t)p[0] << 8) | p[1];
	if (qid != our_qid) return;	// not a reply to THIS transaction

	uint16_t flags = ((uint16_t)p[2] << 8) | p[3];
	if (!(flags & 0x8000)) return;	// QR bit unset -- this is a query, not a response (shouldn't happen on this port, but don't trust it)

	uint16_t qdcount = ((uint16_t)p[4] << 8) | p[5];
	uint16_t ancount = ((uint16_t)p[6] << 8) | p[7];
	uint8_t rcode = flags & 0x0F;

	uint16_t offset = 12;

	for (uint16_t q = 0; q < qdcount && offset < len; q++) {
		offset = skip_name(p, offset, len);
		if ((uint32_t)offset + 4 > len) { offset = len; break; }
		offset += 4;	// QTYPE + QCLASS
	}

	if (rcode != 0) {
		// RFC 1035 4.1.1 -- 3 is the one worth naming specifically
		// (NXDOMAIN, "this name genuinely doesn't exist" -- the
		// common, expected case of a typo'd or made-up hostname);
		// everything else just gets a generic message, since this
		// client has no specific handling for e.g. SERVFAIL (2) or
		// REFUSED (5) beyond reporting that something went wrong
		// server-side.
		finish_query(false, 0, rcode == 3 ?
			"dns: host not found (nxdomain)" : "dns: server returned an error");
		return;
	}

	uint32_t found_ip = 0;
	bool found = false;

	for (uint16_t a = 0; a < ancount && offset < len; a++) {

		offset = skip_name(p, offset, len);
		if ((uint32_t)offset + 10 > len) break;	// TYPE(2)+CLASS(2)+TTL(4)+RDLENGTH(2)

		uint16_t rtype = ((uint16_t)p[offset] << 8) | p[offset + 1];
		uint16_t rclass = ((uint16_t)p[offset + 2] << 8) | p[offset + 3];
		uint16_t rdlen = ((uint16_t)p[offset + 8] << 8) | p[offset + 9];
		offset += 10;

		if ((uint32_t)offset + rdlen > len) break;

		if (!found && rtype == 1 && rclass == 1 && rdlen == 4) {
			found_ip = ((uint32_t)p[offset] << 24) | ((uint32_t)p[offset + 1] << 16) |
				((uint32_t)p[offset + 2] << 8) | p[offset + 3];
			found = true;
			// don't break -- keep the loop bounds-consistent by still
			// advancing offset past this record below; there's
			// nothing left worth extracting from a second A record
			// even if one follows (first one wins), but stopping
			// early here would leave `offset` wherever this record's
			// RDATA started, which is harmless since nothing reads it
			// again, but "always finish advancing offset the same way"
			// is simpler to reason about than a special-cased early
			// exit here.
		}

		offset += rdlen;

	}

	if (found) finish_query(true, found_ip, NULL);
	else finish_query(false, 0, "dns: no A record in response "
		"(CNAME-only answers aren't followed -- see dns.c)");

}

void dns_resolve_start(const char *hostname, uint32_t req_pid, uint32_t tag) {

	if (state != DNS_IDLE) {
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str("dns: busy with another resolution"));
		z_msg_new_send(req_pid, Z_NET_DNS_RESOLVE_REPLY, tag, reply);
		return;
	}

	if (!nameserver_ip) {
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str("dns: no nameserver configured"));
		z_msg_new_send(req_pid, Z_NET_DNS_RESOLVE_REPLY, tag, reply);
		return;
	}

	uint32_t hlen = hostname ? strlen(hostname) : 0;
	if (hlen == 0 || hlen > DNS_MAX_HOSTNAME_LEN) {
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str(hlen == 0 ?
			"dns: empty hostname" : "dns: hostname too long"));
		z_msg_new_send(req_pid, Z_NET_DNS_RESOLVE_REPLY, tag, reply);
		return;
	}

	our_qid = next_qid();
	local_port = next_local_port();

	uint16_t plen = build_query(our_qid, hostname, last_pkt);
	if (plen == 0) {
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str("dns: malformed hostname"));
		z_msg_new_send(req_pid, Z_NET_DNS_RESOLVE_REPLY, tag, reply);
		return;
	}

	strncpy(pending_hostname, hostname, sizeof(pending_hostname) - 1);
	pending_hostname[sizeof(pending_hostname) - 1] = 0;
	requester_pid = req_pid;
	requester_tag = tag;
	last_pkt_len = plen;
	retries = 0;
	state = DNS_QUERYING;

	printf("net: dns: resolving '%s' via ", pending_hostname);
	print_ip(nameserver_ip);
	printf(" (qid=%04x)\n", our_qid);

	udp_listen(local_port, handle_dns_reply);
	udp_send(nameserver_ip, DNS_SERVER_PORT, local_port, last_pkt, last_pkt_len);
	last_tx_ticks = z_uptime_ticks();

}

void dns_poll(void) {

	if (state != DNS_QUERYING) return;

	if (z_uptime_ticks() - last_tx_ticks < DNS_TIMEOUT_TICKS) return;

	retries++;

	if (retries > DNS_MAX_RETRIES) {
		finish_query(false, 0, "dns: no response from nameserver");
		return;
	}

	udp_send(nameserver_ip, DNS_SERVER_PORT, local_port, last_pkt, last_pkt_len);
	last_tx_ticks = z_uptime_ticks();

}
