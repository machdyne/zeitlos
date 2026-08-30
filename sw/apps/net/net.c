/*
 * net -- ARP + ICMP echo (ping) + TFTP client + TCP/telnet client + DNS + NTP
 *
 * Phases 1-4 of the staged plan in docs/networking.md: SPI/chip
 * bring-up, ARP, IP + ICMP echo reply, UDP + TFTP -- all confirmed
 * working on real hardware. TFTP streams both directions through
 * sw/common/zstream.h -- see tftp.h/docs/networking.md for the design
 * and the bug (an objcopy build truncation corrupting this process's
 * own stack) that motivated moving off a fixed-size whole-file buffer.
 *
 * TFTP is exposed to other processes (including the shell's tget/tput
 * commands) via messaging -- see sw/common/znet.h for the protocol.
 *
 * TCP (tcp.c/h) + a minimal telnet client (telnet.c/h) sit alongside
 * TFTP -- net acts as a zport provider (sw/common/zport.h,
 * docs/ports.md) for a single telnet session, reached by `repl`'s
 * `telnet <ip-or-hostname>` command via `term`'s Z_TERM_SET_PORT
 * (sw/common/zterm.h). See this file's own "telnet:" section below
 * for the message flow, and tcp.h/telnet.h for what's simplified
 * relative to a general-purpose TCP/telnet implementation (one
 * connection at a time, stop-and-wait sending, no out-of-order
 * reassembly).
 *
 * DNS (dns.c/h) resolves hostnames to the IPs everything else in this
 * file actually needs -- A records only, one resolution at a time,
 * exposed to other processes via Z_NET_DNS_RESOLVE/_REPLY (znet.h).
 * Most callers don't send those directly: sw/common/zdns.h's
 * z_resolve_host() (used by repl's `telnet` command, the primary
 * caller this was built for) wraps parsing a plain dotted-quad IP
 * and falling back to an actual DNS query into one call. See dns.c's
 * own header comment for scope cuts (no CNAME following, no caching)
 * and docs/networking.md's "DNS client" section for where the
 * nameserver itself comes from (DHCP by default, or the
 * NET_STATIC_DNS build-time override).
 *
 * NTP (ntp.c/h) sets the hardware RTC (rtl/rtc.v, sw/common/zrtc.h)
 * from a public time server -- once a few seconds after the network is
 * up, then hourly. Nothing waits for it and nothing depends on it: a
 * machine that never manages a sync is one that doesn't know the date,
 * not a broken one. It resolves its server's hostname by calling
 * dns.c directly with net's OWN pid as the requester, rather than
 * through zdns.h's blocking wrapper, which would deadlock here -- see
 * ntp.c's header comment for why, and for what an SNTP client
 * deliberately leaves out. Other processes can prod it with
 * Z_NET_NTP_SYNC (sw/common/zntp.h); nothing needs it to READ the
 * time, which is a plain load from a memory-mapped register.
 *
 * IP config: DHCP by default (sw/apps/net/dhcp.c -- DISCOVER/OFFER/
 * REQUEST/ACK, run once at startup, no renewal -- see that file's own
 * header comment for the full design and why it's scoped that way),
 * falling back to the static config below (OUR_IP/OUR_NETMASK/
 * OUR_GATEWAY, themselves just this file's names for the Makefile's
 * NET_STATIC_IP/NETMASK/GATEWAY) if no DHCP server answers within
 * dhcp_acquire()'s retry budget. Build with `make NET_DHCP=0` to skip
 * DHCP entirely and always use the static config instead -- e.g. for
 * a network with no DHCP server, or any setup that wants a fixed
 * address regardless of what a server might offer. Either way, set
 * NET_STATIC_IP/NET_STATIC_NETMASK/NET_STATIC_GATEWAY (dotted-quad or
 * hex, see the Makefile) to override the static values themselves --
 * they default to the ASSUMED-typical config this file always used
 * before DHCP existed (a home-router /24 with gateway at .1; only the
 * IP address, 192.168.178.230, was ever actually specified for it --
 * see docs/networking.md's "Config" section). For same-subnet traffic
 * the gateway's exact value won't matter either way, since it's only
 * consulted for destinations outside the local subnet.
 *
 * On startup, checks rtl/csrs.v's capability CSR (sw/common/zsoc.h,
 * docs/csrs.md) to confirm THIS board's build actually has the
 * ethernet backend (SPI_ETH or ETH_RMII) this binary was compiled
 * for, exiting cleanly (not hanging) if it's confirmed absent -- e.g.
 * Lakritz, which has neither. This is what lets sw/os/sh.c's `init`
 * always attempt starting net now, on every board, instead of the
 * old pid-reservation-only workaround -- see net_phy.h's own header
 * comment for the full story.
 *
 * Run with:
 *   > run net
 *
 * Then, from another machine on the same network:
 *   $ ping 192.168.178.230
 *
 * And from the Zeitlos shell, once a TFTP server is running on some
 * host on the network:
 *   > tget <server-ip> <remote-file> [local-file]
 *   > tput <server-ip> <local-file> [remote-file]
 */

// Set by sw/apps/net/Makefile (NTP_ENABLE ?= 1). Defined here first
// because the #include below is itself guarded on it -- a fallback
// further down, alongside the NET_STATIC_* ones, would be too late and
// would silently build without the header on a standalone compile.
#ifndef NTP_ENABLE
#define NTP_ENABLE 1
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/znet.h"
#include "../../common/zstream.h"
#include "../../common/zport.h"
#include "../../common/zsoc.h"
#include "../../common/zntp.h"
#include "net_phy.h"
#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "dhcp.h"
#include "dns.h"
#if NTP_ENABLE
#include "ntp.h"
#endif
#include "tftp.h"
#include "tcp.h"
#include "../../common/zrng.h"
#include "telnet.h"
#if SSH_ENABLE
#include "ssh/ssh.h"
#endif

// no factory MAC on this chip -- locally-administered address (the
// 0x02 first-octet bit pattern marks it as such, avoiding any clash
// with real vendor-assigned addresses)
static const uint8_t our_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

// NET_DHCP and NET_STATIC_IP/NETMASK/GATEWAY/DNS are set by
// sw/apps/net/Makefile (NET_DHCP ?= 1, NET_STATIC_* default to
// 192.168.178.230/24 gw .1 dns none) -- see that Makefile's own
// comment. #ifndef fallbacks here are just so this file still
// compiles standalone (e.g. a syntax-check invocation of gcc without
// the Makefile's -D flags); the Makefile always defines all five for
// a real build, so these defaults normally never matter in practice.
#ifndef NET_DHCP
#define NET_DHCP 1
#endif
#ifndef NET_STATIC_IP
#define NET_STATIC_IP       0xC0A8B2E6u	// 192.168.178.230
#endif
#ifndef NET_STATIC_NETMASK
#define NET_STATIC_NETMASK  0xFFFFFF00u	// 255.255.255.0
#endif
#ifndef NET_STATIC_GATEWAY
#define NET_STATIC_GATEWAY  0xC0A8B201u	// 192.168.178.1
#endif
#ifndef NET_STATIC_DNS
#define NET_STATIC_DNS      0x00000000u	// none -- see "Config" below
#endif
#ifndef NET_STATIC_NTP
#define NET_STATIC_NTP      0x00000000u	// none -- resolve NTP_SERVER by name
#endif

// kept as the OUR_* names below since every existing comment/message
// in this file already refers to them that way -- NET_STATIC_* is
// just the Makefile-facing spelling of the exact same three values.
// With NET_DHCP=1 (the default) these are only the FALLBACK, used if
// no DHCP server answers dhcp_acquire()'s retries -- ASSUMED-typical
// values (a home-router /24, gateway at .1), not necessarily right
// for your network; see docs/networking.md's "Config" section. With
// NET_DHCP=0 these are used unconditionally instead -- set them via
// `make NET_STATIC_IP=... NET_STATIC_NETMASK=... NET_STATIC_GATEWAY=...`
// (dotted-quad or hex, either works -- see the Makefile).
#define OUR_IP       NET_STATIC_IP
#define OUR_NETMASK  NET_STATIC_NETMASK
#define OUR_GATEWAY  NET_STATIC_GATEWAY

// unlike OUR_IP/NETMASK/GATEWAY above, NET_STATIC_DNS is NOT a
// DHCP-failure fallback -- it's a standing manual override, checked
// AFTER dhcp_acquire() regardless of whether DHCP itself succeeded or
// even ran at all (NET_DHCP=0). Left as 0 (the default), whatever
// nameserver DHCP handed back (option 6) is used as-is, including
// "none" if DHCP didn't offer one or wasn't run. Set explicitly, it
// always wins over whatever DHCP said -- see this file's main() and
// docs/networking.md's "Config"/"DNS client" sections for the full
// reasoning on why DNS gets this different-from-IP/gateway treatment.
#define OUR_DNS      NET_STATIC_DNS

// Same standing-override shape as OUR_DNS above, one step further: set
// it and ntp.c talks to exactly that address and never asks DNS at
// all; leave it 0 (the default) and ntp.c resolves the Makefile's
// NTP_SERVER hostname instead. There is no DHCP option involved either
// way -- option 42 exists, but no home router this has been tried
// against offers it, and a public pool is a better default than
// whatever a consumer router thinks a time server is.
#define OUR_NTP      NET_STATIC_NTP

static bool transfer_active = false;

// Whether ntp_init() found something to do -- false on a build with
// NTP_ENABLE=0 or a bitstream without rtl/rtc.v. Kept here rather than
// asked of ntp.c because it is answered once, at startup, and only
// Z_NET_NTP_STATUS ever wants it.
static bool ntp_enabled = false;

// only PUT needs a final reply sent from here -- for GET, the stream
// itself (opened directly by the receiver, see handle_stream_open())
// already carries completion/failure via Z_STREAM_EOF/Z_STREAM_ERROR,
// so there's nothing left for net.c itself to notify once tftp_poll()
// reports done.
static bool pending_is_put = false;
static uint32_t pending_to = 0;
static uint32_t pending_tag = 0;

// -- telnet: net acts as a zport provider (sw/common/zport.h,
// docs/ports.md) for a single telnet session at a time, matching
// tcp.c's own single-TCB constraint (there can only ever be one TCP
// connection open, so there can only ever be one telnet session
// either). See docs/networking.md's "TCP + telnet" notes and
// sw/common/zterm.h's Z_TERM_SET_PORT for how a `term` instance ends
// up connecting here in the first place (repl's `telnet <ip>`
// command).
//
// TN_IDLE: no session. TN_CONNECTING: a Z_PORT_CONNECT was accepted
// enough to start the TCP handshake, but CONNECTED/REFUSED hasn't
// been sent back to the waiting client yet -- see
// handle_telnet_port_connect() for why that has to be deferred.
// TN_ACTIVE: telnet_port is a real, accepted zport connection,
// relaying both directions.
typedef enum { TN_IDLE, TN_CONNECTING, TN_ACTIVE } telnet_session_state_t;

static telnet_session_state_t telnet_state = TN_IDLE;
static z_port_t telnet_port;
static uint32_t telnet_client_pid;	// valid once state != TN_IDLE
#define TELNET_CONN_ID 1			// only one connection ever -- no
									// need to hand out distinct ids

#if SSH_ENABLE

// -- ssh: a second zport session type through the same single TCB --
//
// tcp.c has one connection, so SSH and telnet are mutually exclusive
// and both states are checked before either starts. That is not a
// limitation worth engineering around here: one `term` window is
// having one remote session either way.
//
// The pending-token record is the other half of the Z_NET_SSH_PREPARE
// handshake described in sw/common/znet.h -- `repl` registers the
// username here in a single hop (where its strings resolve correctly)
// and gets back a token that `term` can carry as a plain scalar.
typedef enum { SSH_IDLE, SSH_ACTIVE } ssh_session_state_t;

static ssh_session_state_t ssh_state = SSH_IDLE;
static z_port_t ssh_port;
static uint32_t ssh_client_pid;
#define SSH_CONN_ID 2				// distinct from TELNET_CONN_ID so a
									// stale DATA from a previous telnet
									// session cannot be mistaken for ours

static struct {
	bool valid;
	uint32_t token;
	uint32_t ip;
	uint16_t port;
	char user[64];
	uint32_t expires;			// z_uptime_ticks() deadline
} ssh_pending;

// ~30 seconds at the kernel tick rate. Long enough for a user to
// finish whatever `repl` was doing between PREPARE and the actual
// connect, short enough that an abandoned token does not sit in memory
// holding a username for the rest of the boot.
#define SSH_TOKEN_TTL_TICKS (732 * 30)

#endif

// tftp_handle_stream_msg() returns void, so there is no way to ask it
// whether it recognised a message. This wrapper answers that question
// for the dispatch chain's catch-all without changing tftp.h's own
// contract: the stream subjects are exactly the ones it handles.
static bool tftp_handle_stream_msg_checked(const z_msg_t *msg) {
	if (msg->subject == Z_STREAM_OPEN_REPLY ||
		msg->subject == Z_STREAM_PULL || msg->subject == Z_STREAM_CHUNK ||
		msg->subject == Z_STREAM_EOF || msg->subject == Z_STREAM_ERROR ||
		msg->subject == Z_STREAM_ABORT) {
		tftp_handle_stream_msg((z_msg_t *)msg);
		return true;
	}
	return false;
}

// -- SUBJECT COLLISION CHECK --
//
// net's subject numbers come from three headers (znet.h, zntp.h,
// zstream.h) that continue one shared sequence, and nothing but
// convention keeps them apart. Reusing a number does not fail to
// compile and does not look wrong at runtime: the dispatch chain below
// is a series of `else if`, so the FIRST matching branch wins and the
// message is handled -- by the wrong handler, silently.
//
// That is not hypothetical. Z_NET_SSH_PREPARE was added at 306, which
// zntp.h already used for Z_NET_NTP_SYNC, and every `ssh` command was
// quietly delivered to the NTP sync handler.
//
// So every subject this file dispatches is listed here and checked
// against every other. A negative array size rather than
// _Static_assert(): this tree builds --std=gnu99.
#define Z_SUBJ_DISTINCT2(a, b) ((a) != (b))
typedef char net_subject_numbers_are_distinct[
	(Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE, Z_NET_NTP_SYNC) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE, Z_NET_NTP_STATUS) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE, Z_NET_DNS_RESOLVE) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE, Z_NET_DNS_RESOLVE_REPLY) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE, Z_NET_TFTP_PUT) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE, Z_STREAM_OPEN) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE_REPLY, Z_NET_NTP_SYNC) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE_REPLY, Z_NET_NTP_STATUS) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE_REPLY, Z_NET_DNS_RESOLVE_REPLY) &&
	 Z_SUBJ_DISTINCT2(Z_NET_SSH_PREPARE_REPLY, Z_NET_TFTP_PUT_REPLY) &&
	 Z_SUBJ_DISTINCT2(Z_NET_NTP_SYNC, Z_NET_DNS_RESOLVE) &&
	 Z_SUBJ_DISTINCT2(Z_NET_NTP_STATUS, Z_NET_DNS_RESOLVE_REPLY) &&
	 Z_SUBJ_DISTINCT2(Z_NET_TFTP_PUT, Z_NET_DNS_RESOLVE)) ? 1 : -1];

static void print_ip(uint32_t ip) {
	printf("%ld.%ld.%ld.%ld",
		(long)((ip >> 24) & 0xFF), (long)((ip >> 16) & 0xFF),
		(long)((ip >> 8) & 0xFF), (long)(ip & 0xFF));
}

static void reply_error(uint32_t to, uint32_t subject, uint32_t tag, const char *msg) {
	z_obj_t reply = z_obj_map(2);
	z_map_set(&reply, "ok", z_obj_uint32(0));
	z_map_set(&reply, "error", z_obj_str(msg));
	z_msg_new_send(to, subject, tag, reply);
	// note: `reply` intentionally never freed -- same borrowed-reply
	// tradeoff documented throughout (see docs/messaging.md); a
	// one-shot message, not a per-chunk one, so the cost is bounded
}

// a Z_STREAM_OPEN arriving at net always means "start a TFTP GET" --
// there's currently only one thing an open can mean here (see
// znet.h's comment). payload: Z_MAP{"ip":Z_UINT32, "filename":Z_STR}.
static void handle_stream_open(z_msg_t *msg) {

	if (transfer_active) {
		zstream_reject(msg->from, msg->tag, "net is busy with another transfer");
		return;
	}

	z_obj_t *ip_obj = z_map_find(&msg->obj, "ip");
	z_obj_t *fn_obj = z_map_find(&msg->obj, "filename");

	if (!ip_obj || ip_obj->type != Z_UINT32 || !fn_obj || fn_obj->type != Z_STR) {
		zstream_reject(msg->from, msg->tag, "bad request");
		return;
	}

	char filename[64];
	strncpy(filename, fn_obj->val.str, sizeof(filename) - 1);
	filename[sizeof(filename) - 1] = 0;

	printf("net: tftp get '%s' from ", filename);
	print_ip(ip_obj->val.uint32);
	printf(" for pid %ld\n", (long)msg->from);

	tftp_get_start(ip_obj->val.uint32, filename, msg->from, msg->tag);

	pending_is_put = false;
	transfer_active = true;

}

static void handle_tftp_put_request(z_msg_t *msg) {

	if (transfer_active) {
		reply_error(msg->from, Z_NET_TFTP_PUT_REPLY, msg->tag, "net is busy with another transfer");
		return;
	}

	z_obj_t *ip_obj = z_map_find(&msg->obj, "ip");
	z_obj_t *fn_obj = z_map_find(&msg->obj, "filename");

	if (!ip_obj || ip_obj->type != Z_UINT32 || !fn_obj || fn_obj->type != Z_STR) {
		reply_error(msg->from, Z_NET_TFTP_PUT_REPLY, msg->tag, "bad request");
		return;
	}

	char filename[64];
	strncpy(filename, fn_obj->val.str, sizeof(filename) - 1);
	filename[sizeof(filename) - 1] = 0;

	printf("net: tftp put '%s' to ", filename);
	print_ip(ip_obj->val.uint32);
	printf(" for pid %ld\n", (long)msg->from);

	// net becomes the zstream *consumer* here -- tftp_put_start()
	// opens a stream back to msg->from to pull the file's bytes (see
	// tftp.h). the requester needs to be ready to act as a producer
	// (respond to our Z_STREAM_OPEN) essentially immediately after
	// sending this.
	tftp_put_start(ip_obj->val.uint32, filename, msg->from);

	pending_to = msg->from;
	pending_tag = msg->tag;
	pending_is_put = true;
	transfer_active = true;

}

// -- telnet: tcp.c/telnet.c event callbacks --

// the TCP handshake (started from handle_telnet_port_connect() below)
// completed -- only now do we tell the waiting `term` it's connected,
// mirroring what z_port_accept() does internally (zport.c). Can't
// call z_port_accept() itself here since it needs the original
// z_msg_t, which is long gone by the time an async TCP handshake
// resolves -- telnet_client_pid (captured at CONNECT time) is all it
// would have given us anyway.
static void telnet_on_established(void) {
	telnet_port.peer_pid = telnet_client_pid;
	telnet_port.conn_id = TELNET_CONN_ID;
	telnet_port.connected = true;
	telnet_state = TN_ACTIVE;
	z_msg_new_send(telnet_client_pid, Z_PORT_CONNECTED, 0, z_obj_uint32(TELNET_CONN_ID));
	printf("net: telnet connected, relaying to pid %ld\n", (long)telnet_client_pid);
}

static void telnet_on_data(const uint8_t *data, uint16_t len) {
	if (telnet_state != TN_ACTIVE) return;
	z_port_send(&telnet_port, data, len);
}

// covers both "the handshake itself never completed" (state was
// still TN_CONNECTING) and "an established session ended" (state was
// TN_ACTIVE) -- tcp.h's TCP_EVENT_CLOSED documents the same two-in-one
// shape, telnet.c just passes it straight through.
static void telnet_on_closed(void) {

	if (telnet_state == TN_CONNECTING) {
		// term's own connect_port() (term.c) uses
		// TERM_TELNET_CONNECT_TIMEOUT_TICKS for this specific
		// connection (~45s -- long enough to cover this file's own
		// tcp.c worst-case retry budget, ~31.5s, with margin), not
		// zport.h's ~2s default -- found via a real bug where the 2s
		// default was shorter than tcp.c's own retry budget, so term
		// always gave up locally before this could ever fire. If this
		// message still arrives after term's (now much longer)
		// timeout has already fired, it's harmlessly ignored on
		// term's end (same accepted limitation z_port_connect()
		// already documents).
		z_msg_new_send(telnet_client_pid, Z_PORT_REFUSED, 0,
			z_obj_str("net: telnet connection failed"));
		printf("net: telnet connect to pid %ld failed\n", (long)telnet_client_pid);
	} else if (telnet_state == TN_ACTIVE) {
		z_port_close(&telnet_port);	// notifies the peer -- term
										// falls back to local echo on
										// receiving this (term.c)
		printf("net: telnet session ended\n");
	}

	telnet_state = TN_IDLE;

}

#if SSH_ENABLE

// -- ssh: ssh.c callbacks --

// Everything the SSH layer wants the user to see: progress lines,
// the host key fingerprint, prompts, and channel data once the shell
// is up. All of it is just bytes to the port, which is the whole
// reason prompting in band works (see ssh.h).
static void ssh_on_out(const uint8_t *data, uint16_t len) {
	if (ssh_state != SSH_ACTIVE) return;
	z_port_send(&ssh_port, data, len);
}

static void ssh_on_ready(void) {
	printf("net: ssh shell open, relaying to pid %ld\n", (long)ssh_client_pid);
}

static void ssh_on_closed(const char *reason) {
	if (ssh_state == SSH_ACTIVE) {
		// The reason text has already gone to the port as part of the
		// session output (ssh.c does that), so this only has to tear
		// the port down -- term falls back to local echo on receiving
		// it, same as telnet.
		z_port_close(&ssh_port);
		printf("net: ssh session ended: %s\n", reason ? reason : "?");
	}
	ssh_state = SSH_IDLE;
}

// -- ssh: Z_NET_SSH_PREPARE --

static void handle_ssh_prepare(const z_msg_t *msg) {

	z_obj_t *user_obj, *ip_obj, *port_obj;

	// Unconditional, before any validation: this one line distinguishes
	// "net never got the message" from "net got it and refused", which
	// from repl's end look identical.
	printf("net: Z_NET_SSH_PREPARE from pid %ld\n", (long)msg->from);
	z_obj_t reply;
	uint32_t token;

	if (msg->obj.type != Z_MAP) {
		reply_error(msg->from, Z_NET_SSH_PREPARE_REPLY, msg->tag,
			"ssh: bad prepare request");
		return;
	}

	// The cast drops const because z_map_find() takes a non-const map
	// (zobj.h) despite only reading it. Same thing handle_dns_resolve()
	// and the rest of this file already do with msg->obj; changing the
	// signature is the right fix and a wider change than this belongs to.
	user_obj = z_map_find((z_obj_t *)&msg->obj, "user");
	ip_obj   = z_map_find((z_obj_t *)&msg->obj, "ip");
	port_obj = z_map_find((z_obj_t *)&msg->obj, "port");

	if (!ip_obj || ip_obj->type != Z_UINT32) {
		reply_error(msg->from, Z_NET_SSH_PREPARE_REPLY, msg->tag,
			"ssh: prepare requires a target IP");
		return;
	}

	if (telnet_state != TN_IDLE || ssh_state != SSH_IDLE) {
		reply_error(msg->from, Z_NET_SSH_PREPARE_REPLY, msg->tag,
			"net: already busy with another session");
		return;
	}

	// Refuse here, at the earliest point, rather than at connect time.
	// ssh_connect() checks this too and would refuse anyway, but by
	// then `repl` has already told the user it is connecting and `term`
	// has left its repl connection -- so the failure would arrive in a
	// window that had just been disconnected from everything.
	if (!z_rng_secure()) {
		reply_error(msg->from, Z_NET_SSH_PREPARE_REPLY, msg->tag,
			"ssh: no trustworthy entropy source -- refusing (see docs/trng.md)");
		return;
	}

	// A random token, not a counter. It is the only thing standing
	// between a CONNECT and the credentials it picks up, and a
	// predictable one would let any process on the system claim a
	// pending session by guessing.
	z_rng_bytes(&token, sizeof(token));
	if (!token) token = 1;		// 0 is the "no pending token" sentinel

	ssh_pending.valid = true;
	ssh_pending.token = token;
	ssh_pending.ip = ip_obj->val.uint32;
	ssh_pending.port = (port_obj && port_obj->type == Z_UINT32 &&
		port_obj->val.uint32) ? (uint16_t)port_obj->val.uint32 : 22;
	ssh_pending.expires = z_uptime_ticks() + SSH_TOKEN_TTL_TICKS;

	ssh_pending.user[0] = 0;
	if (user_obj && user_obj->type == Z_STR && user_obj->val.str) {
		strncpy(ssh_pending.user, user_obj->val.str,
			sizeof(ssh_pending.user) - 1);
		ssh_pending.user[sizeof(ssh_pending.user) - 1] = 0;
	}

	reply = z_obj_map(2);
	z_map_set(&reply, "ok", z_obj_uint32(1));
	z_map_set(&reply, "token", z_obj_uint32(token));
	z_msg_new_send(msg->from, Z_NET_SSH_PREPARE_REPLY, msg->tag, reply);

	printf("net: ssh prepared for ");
	print_ip(ssh_pending.ip);
	printf(":%d user '%s'\n", (int)ssh_pending.port, ssh_pending.user);

}

// Returns true if this CONNECT is redeeming a valid SSH token. The
// token is consumed either way -- a single use is the point of it.
static bool ssh_claim_token(uint32_t token) {

	if (!ssh_pending.valid || !token || token != ssh_pending.token)
		return false;

	ssh_pending.valid = false;

	if ((int32_t)(z_uptime_ticks() - ssh_pending.expires) > 0) {
		printf("net: ssh token expired\n");
		return false;
	}

	return true;

}

static bool handle_ssh_port_connect(const z_msg_t *msg) {

	if (msg->obj.type != Z_UINT32) {
		printf("net: PORT_CONNECT with no scalar arg -- not ssh\n");
		return false;
	}
	if (!ssh_claim_token(msg->obj.val.uint32)) {
		// Not an error by itself: a telnet connect carries a target IP
		// here and is supposed to fall through. Logged because if an
		// SSH connect ends up in telnet's hands, THIS is the line that
		// says so.
		printf("net: PORT_CONNECT arg %08lx did not match an ssh token "
			"(pending=%d) -- treating as telnet\n",
			(unsigned long)msg->obj.val.uint32, (int)ssh_pending.valid);
		return false;
	}

	if (telnet_state != TN_IDLE || ssh_state != SSH_IDLE) {
		z_port_refuse(msg, "net: already busy with another session");
		return true;
	}

	ssh_client_pid = msg->from;

	// Accepted IMMEDIATELY, unlike telnet, which defers until its TCP
	// handshake resolves. SSH needs the port open first: the whole
	// handshake is several seconds of work with a host key confirmation
	// and a password prompt in the middle, and all of that is rendered
	// through this port. Deferring would mean a frozen window followed
	// by a prompt nobody could answer.
	ssh_port.peer_pid = ssh_client_pid;
	ssh_port.conn_id = SSH_CONN_ID;
	ssh_port.connected = true;
	ssh_state = SSH_ACTIVE;
	z_msg_new_send(ssh_client_pid, Z_PORT_CONNECTED, 0,
		z_obj_uint32(SSH_CONN_ID));

	if (!ssh_connect(ssh_pending.ip, ssh_pending.port,
			ssh_pending.user[0] ? ssh_pending.user : NULL,
			ssh_on_out, ssh_on_ready, ssh_on_closed)) {
		z_port_send(&ssh_port,
			(const uint8_t *)"ssh: could not start session\r\n", 30);
		z_port_close(&ssh_port);
		ssh_state = SSH_IDLE;
		return true;
	}

	// Clear the username out of the pending record now it has been
	// handed over -- it is copied inside ssh.c and there is no reason
	// for a second copy to sit here until the next connect overwrites
	// it.
	memset(ssh_pending.user, 0, sizeof(ssh_pending.user));

	printf("net: ssh connecting for pid %ld\n", (long)ssh_client_pid);
	return true;

}

static bool handle_ssh_port_data(const z_msg_t *msg) {

	uint32_t len;
	void *data;

	if (ssh_state != SSH_ACTIVE || !ssh_port.connected ||
		msg->tag != ssh_port.conn_id) return false;

	len = z_blob_len(&msg->obj);
	data = z_blob_data(&msg->obj);
	if (data && len) ssh_input((const uint8_t *)data, (uint16_t)len);

	z_port_send_ack(msg);
	return true;

}

static bool handle_ssh_port_close(const z_msg_t *msg) {

	if (ssh_state != SSH_ACTIVE || !ssh_port.connected ||
		msg->tag != ssh_port.conn_id) return false;

	ssh_port.connected = false;
	ssh_abort();
	ssh_state = SSH_IDLE;
	printf("net: ssh port closed by peer\n");
	return true;

}

#endif // SSH_ENABLE

// -- telnet: zport provider side (Z_PORT_CONNECT/DATA/CLOSE from a
// `term` instance) --

// a Z_PORT_CONNECT here always means "start a telnet session" --
// there's currently only one thing a CONNECT to net can mean, same
// reasoning Z_STREAM_OPEN's own comment gives for TFTP GET. Requires
// obj=Z_UINT32 (the target IP) -- see zport.h's
// z_port_connect_arg()/zterm.h's Z_TERM_SET_PORT Z_MAP form for how a
// `term` ends up sending that instead of the default Z_NONE.
static void handle_telnet_port_connect(const z_msg_t *msg) {

#if SSH_ENABLE
	// An SSH token looks exactly like a telnet target IP on the wire --
	// both are a bare Z_UINT32 -- so SSH gets first refusal. It only
	// claims the message if the value matches a live token it issued,
	// so a real IP falls straight through to telnet below.
	if (handle_ssh_port_connect(msg)) return;

	if (ssh_state != SSH_IDLE) {
		z_port_refuse(msg, "net: already busy with an ssh session");
		return;
	}
#endif

	if (telnet_state != TN_IDLE) {
		z_port_refuse(msg, "net: already busy with another telnet session");
		return;
	}

	if (msg->obj.type != Z_UINT32) {
		z_port_refuse(msg, "net: telnet requires a target IP");
		return;
	}

	uint32_t ip = msg->obj.val.uint32;
	telnet_client_pid = msg->from;

	printf("net: telnet connecting to ");
	print_ip(ip);
	printf(" for pid %ld\n", (long)telnet_client_pid);

	if (!telnet_connect(ip, telnet_on_established, telnet_on_data, telnet_on_closed)) {
		z_port_refuse(msg, "net: tcp busy with another connection");
		return;
	}

	telnet_state = TN_CONNECTING;
	// diagnostic: confirms the TCP layer actually accepted the
	// connection attempt and telnet_state is now TN_CONNECTING --
	// distinguishes "SYN sent, now waiting on the network" (this
	// line) from a silent failure somewhere in tcp.c/telnet.c between
	// here and either telnet_on_established() or telnet_on_closed()
	// eventually firing (see tcp.c's notify() and telnet.c's
	// on_tcp_event(), both of which now log unconditionally for the
	// same reason).
	printf("net: telnet_connect() accepted, state=TN_CONNECTING, "
		"waiting on TCP handshake\n");
	// deliberately no CONNECTED/REFUSED sent yet -- see
	// telnet_on_established()/telnet_on_closed() above, and this
	// file's own header comment on telnet_state.

}

static void handle_telnet_port_data(const z_msg_t *msg) {

	if (telnet_state == TN_ACTIVE && telnet_port.connected &&
		msg->tag == telnet_port.conn_id) {

		uint32_t len = z_blob_len(&msg->obj);
		void *data = z_blob_data(&msg->obj);
		if (data && len) telnet_send((const uint8_t *)data, (uint16_t)len);
		// telnet_send() returning false here (its own outbound queue
		// is full) just drops these bytes -- the same accepted
		// fire-and-forget flow-control gap docs/ports.md already
		// documents for the port protocol itself, not worth building
		// a second layer of backpressure over.

	}

	// tells `term` it's now safe to free its own z_obj_blob()
	// allocation for this message -- see z_port_send_ack()'s own
	// comment (zport.h) for why this has to come after the branch
	// above has genuinely finished reading `data` (telnet_send()
	// already copied whatever it needed via queue_bytes()'s own
	// memcpy(), telnet.c, before returning). Sent unconditionally,
	// even when the guard above didn't match and `data` was never
	// touched at all -- that's still a message `net` will never look
	// at again, and `term`'s own pending-sends slot for it
	// (Z_PORT_MAX_PENDING_SENDS, zport.h) needs an ack to ever be
	// freed regardless of whether `net` did anything useful with it.
	z_port_send_ack(msg);

}

static void handle_telnet_port_close(const z_msg_t *msg) {

	if (telnet_state != TN_ACTIVE || !telnet_port.connected ||
		msg->tag != telnet_port.conn_id) return;

	telnet_port.connected = false;
	telnet_abort();	// term already left -- no reason to wait out a
						// graceful FIN exchange with the remote server
	telnet_state = TN_IDLE;
	printf("net: telnet port closed by peer\n");

}

// a Z_NET_DNS_RESOLVE always carries a Z_STR (the hostname) --
// dns_resolve_start() (dns.c) handles every outcome itself, including
// replying directly to msg->from/msg->tag on failure, so there's
// nothing left for net.c to do here beyond basic payload validation.
static void handle_dns_resolve(const z_msg_t *msg) {

	if (msg->obj.type != Z_STR || !msg->obj.val.str) {
		z_obj_t reply = z_obj_map(2);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str("dns: bad request (expected a hostname string)"));
		z_msg_new_send(msg->from, Z_NET_DNS_RESOLVE_REPLY, msg->tag, reply);
		return;
	}

	dns_resolve_start(msg->obj.val.str, msg->from, msg->tag);

}

#if NTP_ENABLE

// -- time sync (sw/common/zntp.h, sw/apps/net/ntp.c) --

// A Z_NET_NTP_SYNC carries no payload -- there is exactly one thing it
// can mean and no parameters to it. Fire-and-forget on the sender's
// side, so nothing is sent back: see zntp.h on why a UI must not block
// on a public server's response time.
static void handle_ntp_sync(const z_msg_t *msg) {
	printf("net: ntp: sync requested by pid %ld\n", (long)msg->from);
	ntp_sync_now();
}

static void handle_ntp_status(const z_msg_t *msg) {

	uint32_t last = ntp_last_sync_ticks();

	z_obj_t reply = z_obj_map(3);
	z_map_set(&reply, "enabled", z_obj_uint32(ntp_enabled ? 1 : 0));
	z_map_set(&reply, "synced", z_obj_uint32(ntp_ever_synced() ? 1 : 0));
	z_map_set(&reply, "age", z_obj_uint32(last ? (z_uptime_ticks() - last) : 0));
	// `reply` intentionally never freed -- same one-shot borrowed-reply
	// tradeoff reply_error() above documents (docs/messaging.md).
	z_msg_new_send(msg->from, Z_NET_NTP_STATUS, msg->tag, reply);

}

#else

// NTP compiled out entirely (`make NTP_ENABLE=0`) -- ntp.o isn't even
// linked (see the Makefile's NTP_OBJ), so there is no ntp_sync_now()
// to call. Z_NET_NTP_SYNC is simply ignored in that build, and a
// status request still gets a well-formed reply saying so, rather than
// no reply at all: a caller waiting on one should learn that the
// answer is "not built in", not time out wondering.
static void handle_ntp_status(const z_msg_t *msg) {
	z_obj_t reply = z_obj_map(3);
	z_map_set(&reply, "enabled", z_obj_uint32(0));
	z_map_set(&reply, "synced", z_obj_uint32(0));
	z_map_set(&reply, "age", z_obj_uint32(0));
	z_msg_new_send(msg->from, Z_NET_NTP_STATUS, msg->tag, reply);
}

#endif

static void check_tftp_progress(void) {

	if (!transfer_active) return;

	uint32_t len;
	char err[64];
	tftp_result_t r = tftp_poll(&len, err, sizeof(err));

	if (r == TFTP_RESULT_PENDING) return;

	transfer_active = false;

	if (!pending_is_put) {
		if (r == TFTP_RESULT_OK)
			printf("net: tftp get complete, %ld bytes\n", (long)len);
		else
			printf("net: tftp get failed: %s\n", err);
		return;
	}

	z_obj_t reply = z_obj_map(2);

	if (r == TFTP_RESULT_OK) {
		printf("net: tftp put complete, %ld bytes\n", (long)len);
		z_map_set(&reply, "ok", z_obj_uint32(1));
	} else {
		printf("net: tftp put failed: %s\n", err);
		z_map_set(&reply, "ok", z_obj_uint32(0));
		z_map_set(&reply, "error", z_obj_str(err));
	}

	z_msg_new_send(pending_to, Z_NET_TFTP_PUT_REPLY, pending_tag, reply);
	printf("net: tftp put reply sent to pid %ld\n", (long)pending_to);

}

int main(void) {

	// Register the name FIRST, before touching hardware.
	//
	// This used to happen ~116 lines further down, after the ENC28J60
	// bring-up and a full DHCP acquisition -- seconds during which
	// z_pid_lookup("net0") missed and every caller silently fell
	// through to the fixed Z_PID_NET constant. On a boot where net
	// does not land on the pid that constant names (start it without
	// wm, say) those callers talk to whatever process IS at that pid.
	//
	// Nothing about being on the network is a precondition for having
	// a name: this process exists and can receive messages from the
	// moment it starts, which is exactly when it should be findable.
	// Registering here means a lookup either succeeds or the process
	// genuinely is not running.
	char net_name[24];
	if (z_pid_register("net", net_name, sizeof(net_name)))
		printf("net: registered as '%s'\n", net_name);
	else
		printf("net: name registration FAILED -- callers will not find "
			"this process\n");

	// diagnostic: distinguishes "this process's own execution jumped
	// back to main()" (canary stays MAGIC -- .bss was never re-zeroed,
	// since that only happens when fs_load() rewrites this process's
	// entire memory region from the on-disk binary, i.e. a genuine
	// fresh k_proc_create()+fs_load()+k_proc_start()) from "something
	// actually re-created and reloaded this process, just without
	// printing any of the usual `run`/`creating process`/`dock:
	// launching` messages every existing caller of k_proc_create()
	// already has" (canary reads 0 -- fresh .bss). A static local,
	// not a stack variable, specifically so it survives whichever of
	// those two things actually happened to reach this exact line
	// again. Added while investigating a real-hardware symptom: net
	// reappearing under a new pidreg name ("net1") with no visible
	// process-creation message anywhere in the log.
	static uint32_t entry_canary;
	printf("net: main() entered, entry_canary=0x%08lx (expect 0 on a "
		"genuine fresh load, nonzero if this is the same process's "
		"own execution jumping back here)\n", (unsigned long)entry_canary);
	entry_canary = 0xDEADBEEF;

	printf("net: initializing %s...\n", NET_PHY_NAME);

	// check the SOC actually has the ethernet backend this binary was
	// built for BEFORE touching any of its registers -- see
	// net_phy.h's own header comment and docs/csrs.md for the full
	// story. z_soc_feature_confirmed_absent() (not a plain negated
	// z_soc_has_feature()) is deliberate: an older bitstream that
	// predates rtl/csrs.v entirely can't answer this at all, and the
	// safe, backward-compatible behavior there is "proceed as before"
	// (this board might genuinely have the hardware, we just can't
	// confirm it), not "refuse". Only a POSITIVE, confirmed "this
	// board's build has neither SPI_ETH nor ETH_RMII" makes net exit
	// here -- which is exactly what makes it safe for sw/os/sh.c's
	// `init` to always attempt starting net now, on every board,
	// instead of the old pid-reservation-only workaround.
#ifdef NET_PHY_RMII
	if (z_soc_feature_confirmed_absent(Z_FEATURE_ETH_RMII)) {
		printf("net: this SOC build has no RMII ethernet (rtl/boards.vh's "
			"ETH_RMII) -- nothing to do here, exiting cleanly.\n");
		return 1;
	}
#else
	if (z_soc_feature_confirmed_absent(Z_FEATURE_SPI_ETH)) {
		printf("net: this SOC build has no SPI ethernet (rtl/boards.vh's "
			"SPI_ETH) -- nothing to do here, exiting cleanly.\n");
		return 1;
	}
#endif

	if (!phy_init(our_mac)) {
		printf("net: phy_init (%s) failed -- see that driver's header comment "
			"for what to check first.\n", NET_PHY_NAME);
		return 1;
	}

	phy_debug_dump();
	printf("net: mac %02x:%02x:%02x:%02x:%02x:%02x\n",
		our_mac[0], our_mac[1], our_mac[2], our_mac[3], our_mac[4], our_mac[5]);

	eth_init(our_mac);

	// arp/ip/tcp all need to start out at "no address yet" (0) for
	// dhcp_acquire() to run at all -- see ip_handle()'s own comment
	// in ip.c and dhcp.c's header comment for why. Re-initialized
	// just below with whichever address actually wins (DHCP's lease,
	// or the static fallback), before anything past this point could
	// possibly care what our_ip is.
	arp_init(0);
	ip_init(0, 0, 0);
	tcp_init(0);

	uint32_t use_ip, use_netmask, use_gateway, use_dns = 0;

#if NET_DHCP
	if (dhcp_acquire(our_mac, &use_ip, &use_netmask, &use_gateway, &use_dns)) {
		printf("net: using dhcp-assigned address\n");
	} else {
		printf("net: dhcp unavailable, falling back to static "
			"config (see this file's header comment)\n");
		use_ip = OUR_IP;
		use_netmask = OUR_NETMASK;
		use_gateway = OUR_GATEWAY;
	}
#else
	// DHCP compiled out entirely (`make NET_DHCP=0`) -- not even
	// attempted, straight to the static config. dhcp.o itself isn't
	// even linked in this case (see the Makefile's DHCP_OBJ), so
	// there's no dhcp_acquire() to call here at all.
	printf("net: dhcp disabled at build time (NET_DHCP=0), using "
		"static config\n");
	use_ip = OUR_IP;
	use_netmask = OUR_NETMASK;
	use_gateway = OUR_GATEWAY;
#endif

	// NET_STATIC_DNS is a standing override, not a DHCP-failure
	// fallback -- see this file's own comment on OUR_DNS above. If
	// it's unset (0), whatever DHCP provided (possibly also 0, i.e.
	// none) is used as-is.
	if (OUR_DNS) use_dns = OUR_DNS;
	dns_set_nameserver(use_dns);

	arp_init(use_ip);
	ip_init(use_ip, use_netmask, use_gateway);
	tcp_init(use_ip);

	// Arms the SNTP client (sw/apps/net/ntp.c). Deliberately AFTER
	// dns_set_nameserver() and the arp/ip/tcp re-init above: with the
	// default (hostname) server it needs a working nameserver, and
	// with any server it needs an address and a route. Nothing waits
	// on it -- the first request goes out a few seconds from now, off
	// ntp_poll(), and a machine that never manages a sync is a machine
	// that just doesn't know the time, not a broken one.
#if NTP_ENABLE
	ntp_enabled = ntp_init(OUR_NTP);
#else
	printf("net: ntp disabled at build time (NTP_ENABLE=0)\n");
#endif

	// registers as "net0" (see sw/os/pidreg.h) -- callers can now

	printf("net: ip ");
	print_ip(use_ip);
	printf("/");
	print_ip(use_netmask);
	printf(", listening (arp + icmp echo + tftp + telnet%s + dns%s)\n",
#if SSH_ENABLE
		" + ssh",
#else
		"",
#endif
		ntp_enabled ? " + ntp" : "");

#if SSH_ENABLE
	// Seeded HERE, at boot, not lazily on the first request. The first
	// z_rng_secure() call runs a full reseed, and doing that inside
	// handle_ssh_prepare() would put it on repl's RPC deadline -- a
	// slow seed would then be indistinguishable from net never having
	// received the message at all.
	//
	// The line it prints is also the single most useful thing on this
	// console when `ssh` misbehaves: it says both that this net
	// UNDERSTANDS ssh and whether it has entropy to run it.
	printf("net: ssh support built in, entropy %s\n",
		z_rng_secure() ? "ok" : "UNAVAILABLE (ssh will refuse)");
#else
	printf("net: built without ssh support (SSH_ENABLE=0)\n");
#endif

	while (1) {

		eth_poll();

		// flushes ip_send()'s pending packet (see ip.c's own comment)
		// the moment eth_poll() just above has processed an ARP reply
		// resolving whatever it was waiting on -- placed right after
		// eth_poll() specifically to minimize that latency, rather
		// than waiting for some later point in this same loop.
		ip_poll();

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_STREAM_OPEN) handle_stream_open(&msg);
			else if (msg.subject == Z_NET_TFTP_PUT) handle_tftp_put_request(&msg);
			else if (msg.subject == Z_NET_DNS_RESOLVE) handle_dns_resolve(&msg);
			// A DNS reply arriving HERE, at net, is net's own DNS
			// request coming back to itself -- ntp.c asks dns.c
			// directly and names this process as the requester, since
			// the usual blocking wrapper (zdns.h's z_dns_resolve())
			// would deadlock when the caller is the process that has
			// to answer. See ntp.c's header comment. If the tag says
			// it isn't ntp's, fall through: something else in a later
			// version may be doing the same trick.
#if NTP_ENABLE
			else if (msg.subject == Z_NET_DNS_RESOLVE_REPLY &&
				ntp_handle_dns_reply(&msg)) { /* consumed */ }
			else if (msg.subject == Z_NET_NTP_SYNC) handle_ntp_sync(&msg);
#endif
			else if (msg.subject == Z_NET_NTP_STATUS) handle_ntp_status(&msg);
#if SSH_ENABLE
			else if (msg.subject == Z_NET_SSH_PREPARE) handle_ssh_prepare(&msg);
#endif
			else if (msg.subject == Z_PORT_CONNECT) handle_telnet_port_connect(&msg);
			else if (msg.subject == Z_PORT_DATA) {
#if SSH_ENABLE
				if (!handle_ssh_port_data(&msg))
#endif
				handle_telnet_port_data(&msg);
			}
			else if (msg.subject == Z_PORT_DATA_ACK) {
				z_port_handle_ack(&telnet_port, &msg);
#if SSH_ENABLE
				z_port_handle_ack(&ssh_port, &msg);
#endif
			}
			else if (msg.subject == Z_PORT_CLOSE) {
#if SSH_ENABLE
				if (!handle_ssh_port_close(&msg))
#endif
				handle_telnet_port_close(&msg);
			}
			else if (!tftp_handle_stream_msg_checked(&msg)) {
				// Everything above is an explicit subject and tftp
				// claims the stream ones; anything left is a message
				// this build does not understand. Silently dropping it
				// is what made a stale net indistinguishable from a
				// broken one -- the sender just waits forever.
				printf("net: unhandled subject %ld from pid %ld\n",
					(long)msg.subject, (long)msg.from);
			}
		}

		check_tftp_progress();
		tcp_poll();
		telnet_poll();
#if SSH_ENABLE
		ssh_poll();
#endif
		dns_poll();
#if NTP_ENABLE
		// Cheap: one comparison in the state this is in almost all of
		// the time. See ntp.c's own ntp_poll().
		ntp_poll();
#endif

		// Yield the rest of the timeslice.
		//
		// MEASURED, not assumed: removing this made tftp 7% SLOWER
		// (2.693s -> 2.896s for the same 88872-byte transfer with the
		// same processes running). The reasoning that said it should
		// help was backwards.
		//
		// TFTP is stop-and-wait, so once a block has been handed to the
		// shell this process has nothing to do until the shell replies.
		// Spinning here does not find the reply any sooner -- the reply
		// cannot exist until the shell RUNS -- it just makes the shell
		// wait out this process's full 1.37ms timeslice first. Yielding
		// gets the shell scheduled sooner and shortens the round trip.
		//
		// Note this is the opposite of what the CPU-bound `bench`
		// numbers suggested about blocking, and both are correct:
		// yielding costs throughput and buys latency. This loop is
		// bound by latency.
		//
		// Blocks until a frame arrives or the timeout expires.
		//
		// This used to be a bare 1-tick wait because incoming packets
		// could only be FOUND BY POLLING -- neither MAC had an
		// interrupt wired to anything that could wake a blocked
		// process, so the timeout was the only thing that got this
		// process running again. That meant ~732 wakes a second to
		// discover, almost every time, that nothing had happened.
		//
		// It cost more than it looks. The scheduler shares the CPU
		// between RUNNABLE processes, so a process that wakes
		// constantly takes a full share out of whatever is in the
		// foreground rather than out of idle time. A full-screen app
		// measured a quarter of the machine with three such wakers
		// alongside it.
		//
		// Both MACs now raise Z_IRQ_ETH when a frame is waiting
		// (rtl/ethmac_rmii.v's eth_int_o, and the ENC28J60's INT pin
		// via rtl/sysctl.v), and the kernel turns that into a message
		// -- so this wakes when a packet actually arrives.
		//
		// The timeout stays, and is now long rather than 1 tick. It
		// is no longer how packets are noticed; it is a backstop for
		// the periodic work this loop still does (ARP ageing, socket
		// timers, TX retries), and insurance against a MAC that
		// somehow leaves its RX buffer occupied without the interrupt
		// following. Ten wakes a second for housekeeping instead of
		// 732 for nothing.
		z_proc_wait(Z_TICK_HZ / 10);

	}

}
