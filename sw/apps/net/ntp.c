/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SNTP (RFC 4330) client. See ntp.h for the public API and the design
 * notes; this file is the packet format, the validation, and the
 * state machine behind them.
 *
 * -- the packet --
 *
 * An SNTP request is 48 bytes of which exactly one matters going out:
 * the first, holding leap indicator, version and mode. Everything else
 * a client sends is ignored by the server, with one useful exception
 * -- the transmit timestamp at offset 40, which the server copies back
 * verbatim into the reply's ORIGINATE field at offset 24. That copy is
 * the only thing tying a reply to a request, so this fills it with a
 * value that changes every time and checks it on the way back. Without
 * that check a stale reply from a request three retries ago would be
 * accepted as an answer to the current one, which on a slow link is
 * not hypothetical.
 *
 * The reply's own transmit timestamp (offset 40) is the answer: the
 * server's idea of the time at the moment it sent. That is what gets
 * written to the RTC, plus half the measured round trip -- see
 * apply_time() below.
 *
 * -- the epoch, and the 2036 rollover --
 *
 * NTP counts seconds from 1 January 1900; Unix counts from 1970. The
 * difference is 2208988800 seconds, which is also very nearly the
 * point at which NTP's unsigned 32-bit second count wraps -- era 0
 * ends on 7 February 2036.
 *
 * So the subtraction has two cases, and the second one is not
 * hypothetical the way most overflow handling is: it has a date. An
 * NTP second count at or above the offset is era 0 and converts by
 * subtracting; one below it is era 1 (post-2036) and converts by
 * ADDING the complement instead. Getting this wrong produces a clock
 * that works perfectly until a specific Thursday, which is the worst
 * possible failure schedule, so it is handled now rather than noted as
 * a TODO. Note the RTC's own uint32 seconds then runs out in 2106.
 *
 * -- what a bad answer looks like --
 *
 * Public servers are mostly well behaved, but "mostly" is doing real
 * work in a client with no authentication. Rejected here: a reply that
 * is not mode 4, one whose originate timestamp does not match what was
 * sent, stratum 0 (a Kiss-o'-Death packet, which is a server asking to
 * be left alone and specifically NOT a time source), stratum above 15,
 * a leap indicator of 3 (the server itself says it is not
 * synchronised), and a zero transmit timestamp. Each of those is a
 * case where the packet still parses fine and the number in it is
 * simply not the time.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "ntp.h"
#include "udp.h"
#include "dns.h"
#include "../../common/zeitlos.h"
#include "../../common/znet.h"
#include "../../common/zrtc.h"
#include "../../common/zsoc.h"

#define NTP_SERVER_PORT 123
#define NTP_PKT_LEN     48

// Defaults matching sw/apps/net/Makefile's own -D flags. As with
// net.c's NET_STATIC_* fallbacks, these exist so this file still
// compiles standalone; a real build always defines them.
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef NTP_ENABLE
#define NTP_ENABLE 1
#endif

// -- timing --
//
// Ticks throughout, from z_uptime_ticks() -- the ~732Hz KTIMER rate
// documented across this app (tcp.c's TCP_RTO_TICKS_BASE, dns.c's
// DNS_TIMEOUT_TICKS). Not wall-clock seconds, for the obvious reason
// that the wall clock is the thing being set.

// How long after ntp_init() the first attempt goes out. Long enough
// for net's own startup printing to finish and for an ARP to the
// gateway to resolve, short enough that a machine that just booted
// knows the time before anyone has finished logging in.
#define NTP_FIRST_DELAY_TICKS   (3u * Z_TICK_HZ)

// Between successful syncs. Hourly rather than daily: the RTC is a
// counter on the same crystal everything else here runs from, so its
// drift is whatever that part's tolerance is -- tens of ppm, call it a
// couple of seconds a day. An hour keeps the error under a tenth of a
// second, costs one 90-byte exchange, and means a clock left running
// overnight is still right in the morning. Daily would also be
// defensible; hourly is cheap enough that there is no reason to
// choose it.
#define NTP_SYNC_INTERVAL_TICKS (3600u * Z_TICK_HZ)

// After a failed attempt. Deliberately much shorter than the success
// interval: the overwhelmingly likely reason for a failure at boot is
// that something else is not ready yet (no DHCP lease, no nameserver,
// the link still negotiating), and those resolve in seconds.
#define NTP_RETRY_TICKS         (60u * Z_TICK_HZ)

// Per-request timeout and how many requests one sync attempt makes
// before giving up and falling back to NTP_RETRY_TICKS. A server on
// the far side of the internet is a lot further away than dns.c's
// nameserver, hence the longer wait.
#define NTP_TIMEOUT_TICKS       (2u * Z_TICK_HZ)
#define NTP_MAX_RETRIES         3

// Seconds between 1900-01-01 and 1970-01-01 -- see this file's header
// on why this appears twice below rather than once.
#define NTP_UNIX_OFFSET 2208988800u

typedef enum {
	NTP_DISABLED,	// built with NTP_ENABLE=0, or no RTC to set
	NTP_IDLE,		// waiting for next_attempt_ticks
	NTP_RESOLVING,	// dns lookup in flight
	NTP_QUERYING	// request sent, waiting for a reply
} ntp_state_t;

static ntp_state_t state = NTP_DISABLED;

// Pinned by NET_STATIC_NTP, or filled in by a DNS resolution. Kept
// after a successful lookup so subsequent hourly syncs skip DNS --
// there is no caching in dns.c (see its header) and a name that
// resolved an hour ago has almost certainly not moved.
static uint32_t server_ip;
static bool server_ip_pinned;	// from the build, never re-resolved

static uint16_t local_port;
static uint32_t next_attempt_ticks;
static uint32_t last_tx_ticks;
static uint8_t retries;

static bool synced_ever;
static uint32_t last_sync_ticks;

// The transmit timestamp sent in the current request, echoed back by
// the server in the reply's originate field. Two words, compared as a
// pair -- see this file's header comment.
static uint32_t sent_xmt_hi, sent_xmt_lo;

// Tag on our own DNS request, so ntp_handle_dns_reply() can tell a
// reply meant for this client from one meant for some other process
// that happened to arrive while net was routing messages. Arbitrary,
// just needs to be recognisable.
#define NTP_DNS_TAG 0x4E545001u

static void print_ip(uint32_t ip) {
	printf("%ld.%ld.%ld.%ld",
		(long)((ip >> 24) & 0xFF), (long)((ip >> 16) & 0xFF),
		(long)((ip >> 8) & 0xFF), (long)(ip & 0xFF));
}

static uint16_t next_local_port(void) {
	// Same z_uptime_ticks()-varied, not-random approach dns.c and
	// tcp.c use, and for the same reason: it only needs to avoid
	// colliding with a late reply to an earlier request of our own,
	// not to resist anybody guessing it.
	static uint16_t p = 0;
	if (p < 49152) p = (uint16_t)(49152 + (z_uptime_ticks() & 0x3FF));
	else p++;
	if (p < 49152) p = 49152;
	return p;
}

// Schedules the next attempt and returns to idle. `ticks` is measured
// from now.
static void schedule(uint32_t ticks) {
	next_attempt_ticks = z_uptime_ticks() + ticks;
	state = NTP_IDLE;
}

static void give_up(const char *why) {
	printf("net: ntp: %s -- retrying in %ld s\n", why,
		(long)(NTP_RETRY_TICKS / Z_TICK_HZ));
	udp_close(local_port);
	schedule(NTP_RETRY_TICKS);
}

// Builds and sends one request to server_ip. Assumes local_port is
// already listening.
static void send_request(void) {

	uint8_t pkt[NTP_PKT_LEN];
	memset(pkt, 0, sizeof(pkt));

	// LI = 0 (no warning), VN = 4, Mode = 3 (client).
	pkt[0] = 0x23;

	// Transmit timestamp. Not a real NTP timestamp -- we do not have
	// one to give, which is the entire point of asking -- just a value
	// that differs between requests, so the echo in the reply's
	// originate field identifies which request it answers. Uptime
	// ticks in the low word, and a marker in the high word so a
	// packet capture is readable.
	sent_xmt_hi = 0x5A454954u;		// "ZEIT"
	sent_xmt_lo = z_uptime_ticks();

	pkt[40] = (uint8_t)(sent_xmt_hi >> 24);
	pkt[41] = (uint8_t)(sent_xmt_hi >> 16);
	pkt[42] = (uint8_t)(sent_xmt_hi >> 8);
	pkt[43] = (uint8_t)(sent_xmt_hi);
	pkt[44] = (uint8_t)(sent_xmt_lo >> 24);
	pkt[45] = (uint8_t)(sent_xmt_lo >> 16);
	pkt[46] = (uint8_t)(sent_xmt_lo >> 8);
	pkt[47] = (uint8_t)(sent_xmt_lo);

	udp_send(server_ip, NTP_SERVER_PORT, local_port, pkt, NTP_PKT_LEN);
	last_tx_ticks = z_uptime_ticks();

}

static uint32_t rd32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Converts the server's timestamp, corrects for half the round trip,
// and writes the RTC.
static void apply_time(uint32_t ntp_sec, uint32_t ntp_frac, uint32_t rtt_ticks) {

	// -- epoch. See this file's header on the 2036 case.
	uint32_t unix_sec;
	if (ntp_sec >= NTP_UNIX_OFFSET) {
		unix_sec = ntp_sec - NTP_UNIX_OFFSET;
	} else {
		// era 1: the count has wrapped past 2036. Adding the
		// complement of the offset is the same subtraction done
		// modulo 2^32, written the way that makes it obvious it is
		// deliberate rather than an accident of unsigned arithmetic.
		unix_sec = ntp_sec + (uint32_t)(0x100000000ull - NTP_UNIX_OFFSET);
	}

	// -- fraction. NTP's is a binary fraction of a second in the top
	// 32 bits, and the RTC's is in units of 1/1024s, so this is a
	// shift rather than a divide: frac / 2^32 * 1024 == frac >> 22.
	// That equivalence is exactly why rtc.v's sub-second rate is a
	// power of two (see its header).
	uint32_t rate = z_rtc_rate();
	uint32_t sub;
	if (rate == 1024u) {
		sub = ntp_frac >> 22;
	} else {
		// A differently-built RTC. Divide instead, keeping the
		// operands inside 32 bits by shifting the fraction down
		// first -- (frac >> 16) * rate can still overflow for a large
		// rate, so this trades precision for safety, which at
		// sub-second resolution costs nothing that matters.
		sub = ((ntp_frac >> 16) * rate) >> 16;
		if (sub >= rate) sub = rate - 1;
	}

	// -- half the round trip. The server's timestamp describes the
	// moment it sent, so by the time it is being written here the
	// reply's own travel time has already elapsed. Assuming the two
	// legs are symmetric -- which is what SNTP assumes and is close
	// enough on any normal path -- that is half the measured round
	// trip.
	//
	// rtt_ticks is small (a few hundred at most for a two-second
	// timeout), so this multiplication cannot overflow.
	uint32_t half_sub = (rtt_ticks * rate) / (2u * Z_TICK_HZ);
	sub += half_sub;
	while (sub >= rate) {
		sub -= rate;
		unix_sec++;
	}

	z_rtc_set(unix_sec, sub);

	synced_ever = true;
	last_sync_ticks = z_uptime_ticks();

	// Print the result as a date rather than as a number of seconds.
	// The number is what got written, but the date is what tells you
	// at a glance that it is right -- an off-by-an-era or
	// byte-swapped timestamp is a perfectly plausible-looking uint32
	// and an obviously absurd year.
	z_tm_t tm;
	z_time_to_tm(unix_sec, &tm);
	printf("net: ntp: %s %04ld-%02d-%02d %02d:%02d:%02d UTC "
		"(rtt %ld ms, %ld s since epoch)\n",
		z_wday_name(tm.wday), (long)tm.year, tm.month, tm.day,
		tm.hour, tm.min, tm.sec,
		(long)((rtt_ticks * 1000u) / Z_TICK_HZ), (long)unix_sec);

}

// Dispatched from udp.c for local_port.
static void handle_ntp_reply(uint32_t src_ip, uint16_t src_port,
	const uint8_t *p, uint16_t len) {

	if (state != NTP_QUERYING) return;

	// Only the server we asked, on the port NTP servers answer from.
	// Same guard, same reasoning, as dns.c's own.
	if (src_ip != server_ip || src_port != NTP_SERVER_PORT) return;

	if (len < NTP_PKT_LEN) return;

	uint8_t li = (p[0] >> 6) & 0x3;
	uint8_t mode = p[0] & 0x7;
	uint8_t stratum = p[1];

	if (mode != 4) return;			// not a server reply -- ignore quietly

	// Does this answer the request we actually sent? The server echoes
	// our transmit timestamp into the originate field at offset 24.
	if (rd32(p + 24) != sent_xmt_hi || rd32(p + 28) != sent_xmt_lo) {
		// Almost certainly a late reply to an earlier retry. Not an
		// error and not worth a message -- the request it belongs to
		// has already been superseded.
		return;
	}

	// From here on the packet is definitely ours, so a rejection is
	// worth reporting: it means this server will not do, which is
	// something the user can act on.

	if (stratum == 0) {
		// Kiss-o'-Death. The four bytes at offset 12 are an ASCII
		// code (RATE, DENY, RSTR) explaining why, printed as-is
		// rather than interpreted -- there are only a handful and the
		// code itself is the searchable thing.
		char kod[5];
		kod[0] = (char)p[12]; kod[1] = (char)p[13];
		kod[2] = (char)p[14]; kod[3] = (char)p[15];
		kod[4] = 0;
		for (int i = 0; i < 4; i++)
			if (kod[i] < 32 || kod[i] > 126) kod[i] = '?';
		udp_close(local_port);
		printf("net: ntp: server sent kiss-o'-death '%s' -- backing off\n", kod);
		// A KoD is a request to stop asking, so this backs off by the
		// full success interval rather than the short retry one.
		schedule(NTP_SYNC_INTERVAL_TICKS);
		return;
	}

	if (stratum > 15) {
		udp_close(local_port);
		give_up("server reported an unusable stratum");
		return;
	}

	if (li == 3) {
		// The server is telling us its own clock is not synchronised.
		udp_close(local_port);
		give_up("server is not synchronised");
		return;
	}

	uint32_t xmt_sec = rd32(p + 40);
	uint32_t xmt_frac = rd32(p + 44);

	if (xmt_sec == 0) {
		udp_close(local_port);
		give_up("server sent a zero timestamp");
		return;
	}

	uint32_t rtt = z_uptime_ticks() - last_tx_ticks;

	udp_close(local_port);
	apply_time(xmt_sec, xmt_frac, rtt);
	schedule(NTP_SYNC_INTERVAL_TICKS);

}

// Opens the socket and fires the first request of an attempt.
static void begin_query(void) {

	local_port = next_local_port();
	retries = 0;
	state = NTP_QUERYING;

	udp_listen(local_port, handle_ntp_reply);
	send_request();

	printf("net: ntp: querying ");
	print_ip(server_ip);
	printf("\n");

}

// Starts an attempt: straight to the query if the address is already
// known, via DNS if it is not.
static void begin_attempt(void) {

	if (server_ip) {
		begin_query();
		return;
	}

	// dns_resolve_start() handles every outcome itself, including the
	// immediate failures (no nameserver, busy) -- it always sends
	// exactly one reply to the pid/tag given, so there is nothing to
	// check here and no path where this silently goes nowhere. See
	// dns.h.
	printf("net: ntp: resolving %s\n", NTP_SERVER);
	state = NTP_RESOLVING;
	dns_resolve_start(NTP_SERVER, z_getpid(), NTP_DNS_TAG);

}

bool ntp_init(uint32_t ip) {

#if !NTP_ENABLE
	printf("net: ntp: disabled at build time (NTP_ENABLE=0)\n");
	state = NTP_DISABLED;
	return;
#else

	// No point running at all if there is nothing to set. `RTC is
	// optional in rtl/boards.vh (on by default), and older bitstreams
	// predate it entirely; either way this is the whole extent of the
	// consequence -- net works exactly as it did before, minus the
	// clock. Note z_rtc_available() and not z_rtc_present(): the
	// latter would HANG on a pre-RTC ICACHE bitstream rather than
	// returning false. See zrtc.h.
	if (!z_rtc_available()) {
		printf("net: ntp: this bitstream has no RTC -- nothing to set, "
			"ntp disabled. Check `RTC in rtl/boards.vh, then "
			"`make flash` (not `make dev-flash` -- it's gateware).\n");
		state = NTP_DISABLED;
		return false;
	}

	server_ip = ip;
	server_ip_pinned = (ip != 0);

	if (server_ip_pinned) {
		printf("net: ntp: server pinned to ");
		print_ip(server_ip);
		printf(" (NET_STATIC_NTP)\n");
	} else {
		printf("net: ntp: server %s, syncing every %ld min\n",
			NTP_SERVER, (long)(NTP_SYNC_INTERVAL_TICKS / Z_TICK_HZ / 60));
	}

	schedule(NTP_FIRST_DELAY_TICKS);

	return true;

#endif

}

void ntp_sync_now(void) {

	if (state != NTP_IDLE) return;	// disabled, or already working

	// Not "attempt immediately" but "attempt on the next poll", so
	// this is safe to call from a message handler -- the work happens
	// in the main loop where the rest of it does.
	next_attempt_ticks = z_uptime_ticks();

}

bool ntp_handle_dns_reply(const z_msg_t *msg) {

	if (msg->tag != NTP_DNS_TAG) return false;
	if (state != NTP_RESOLVING) return true;	// ours, but stale -- consume it

	z_obj_t *ok_obj = z_map_find((z_obj_t *)&msg->obj, "ok");
	z_obj_t *ip_obj = z_map_find((z_obj_t *)&msg->obj, "ip");

	if (!ok_obj || ok_obj->type != Z_UINT32 || !ok_obj->val.uint32 ||
		!ip_obj || ip_obj->type != Z_UINT32) {

		z_obj_t *err = z_map_find((z_obj_t *)&msg->obj, "error");
		printf("net: ntp: could not resolve %s: %s\n", NTP_SERVER,
			(err && err->type == Z_STR && err->val.str) ?
				err->val.str : "(no reason given)");
		give_up("name resolution failed");
		return true;

	}

	server_ip = ip_obj->val.uint32;
	// Deliberately kept for next time -- see server_ip's declaration.
	begin_query();

	return true;

}

void ntp_poll(void) {

	switch (state) {

		case NTP_DISABLED:
			return;

		case NTP_IDLE:
			// Unsigned difference, so this is correct across the
			// ~68-day wrap of z_uptime_ticks() as long as the
			// interval itself is well short of that -- an hour is.
			if ((int32_t)(z_uptime_ticks() - next_attempt_ticks) < 0) return;
			begin_attempt();
			return;

		case NTP_RESOLVING:
			// dns.c owns the timeout here and always replies, so there
			// is nothing to time out on this side. Left as an explicit
			// case rather than folded into the default so that stays
			// on the record.
			return;

		case NTP_QUERYING:

			if (z_uptime_ticks() - last_tx_ticks < NTP_TIMEOUT_TICKS) return;

			retries++;

			if (retries > NTP_MAX_RETRIES) {
				udp_close(local_port);
				// A pinned address is never re-resolved; a resolved
				// one is dropped so the next attempt asks DNS again.
				// Worth doing: an unreachable pool member is exactly
				// the case where a fresh lookup gets a different and
				// possibly working address.
				if (!server_ip_pinned) server_ip = 0;
				give_up("no reply from server");
				return;
			}

			send_request();
			return;

	}

}

bool ntp_ever_synced(void) {
	return synced_ever;
}

uint32_t ntp_last_sync_ticks(void) {
	return last_sync_ticks;
}
