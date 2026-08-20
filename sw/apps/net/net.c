/*
 * net -- ARP + ICMP echo (ping) + TFTP client + TCP/telnet client
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
 * `telnet <ip>` command via `term`'s Z_TERM_SET_PORT (sw/common/
 * zterm.h). See this file's own "telnet:" section below for the
 * message flow, and tcp.h/telnet.h for what's simplified relative to
 * a general-purpose TCP/telnet implementation (one connection at a
 * time, stop-and-wait sending, no out-of-order reassembly).
 *
 * IP config is static (see docs/networking.md for why). The netmask
 * and gateway below are ASSUMED (a typical home-router /24 with
 * gateway at .1) -- only the IP address (192.168.178.230) was
 * actually specified. Correct if wrong; for same-subnet traffic it
 * won't matter, since the gateway is only consulted for destinations
 * outside the local subnet.
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

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/znet.h"
#include "../../common/zstream.h"
#include "../../common/zport.h"
#include "../../common/zsoc.h"
#include "net_phy.h"
#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "tftp.h"
#include "tcp.h"
#include "telnet.h"

// no factory MAC on this chip -- locally-administered address (the
// 0x02 first-octet bit pattern marks it as such, avoiding any clash
// with real vendor-assigned addresses)
static const uint8_t our_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

#define OUR_IP       0xC0A8B2E6u	// 192.168.178.230
#define OUR_NETMASK  0xFFFFFF00u	// /24 -- ASSUMED, see file header comment
#define OUR_GATEWAY  0xC0A8B201u	// 192.168.178.1 -- ASSUMED, see file header comment

static bool transfer_active = false;

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
		// term's own z_port_connect_arg() (zport.c) is waiting up to
		// ~2 seconds for CONNECTED or REFUSED -- if this arrives
		// after that timeout already fired, it's harmlessly ignored
		// on term's end (same accepted limitation z_port_connect()
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

// -- telnet: zport provider side (Z_PORT_CONNECT/DATA/CLOSE from a
// `term` instance) --

// a Z_PORT_CONNECT here always means "start a telnet session" --
// there's currently only one thing a CONNECT to net can mean, same
// reasoning Z_STREAM_OPEN's own comment gives for TFTP GET. Requires
// obj=Z_UINT32 (the target IP) -- see zport.h's
// z_port_connect_arg()/zterm.h's Z_TERM_SET_PORT Z_MAP form for how a
// `term` ends up sending that instead of the default Z_NONE.
static void handle_telnet_port_connect(const z_msg_t *msg) {

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
	// deliberately no CONNECTED/REFUSED sent yet -- see
	// telnet_on_established()/telnet_on_closed() above, and this
	// file's own header comment on telnet_state.

}

static void handle_telnet_port_data(const z_msg_t *msg) {

	if (telnet_state != TN_ACTIVE || !telnet_port.connected ||
		msg->tag != telnet_port.conn_id) return;

	uint32_t len = z_blob_len(&msg->obj);
	void *data = z_blob_data(&msg->obj);
	if (data && len) telnet_send((const uint8_t *)data, (uint16_t)len);
	// telnet_send() returning false here (its own outbound queue is
	// full) just drops these bytes -- the same accepted
	// fire-and-forget flow-control gap docs/ports.md already
	// documents for the port protocol itself, not worth building a
	// second layer of backpressure over.

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
	arp_init(OUR_IP);
	ip_init(OUR_IP, OUR_NETMASK, OUR_GATEWAY);
	tcp_init(OUR_IP);

	// registers as "net0" (see sw/os/pidreg.h) -- callers can now
	// reach net by name instead of only the fixed Z_PID_NET constant
	// (znet.h); sh.c's tftp calls fall back to Z_PID_NET if this ever
	// fails or hasn't happened yet. Deliberately not fatal if
	// registration fails -- net is still fully usable via the fixed
	// pid, same as it always has been, just not independently
	// discoverable by name in that case.
	char net_name[24];
	if (z_pid_register("net", net_name, sizeof(net_name)))
		printf("net: registered as '%s'\n", net_name);
	else
		printf("net: name registration failed (still usable via fixed pid)\n");

	printf("net: ip 192.168.178.230/24, listening (arp + icmp echo + tftp + telnet)\n");

	while (1) {

		eth_poll();

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_STREAM_OPEN) handle_stream_open(&msg);
			else if (msg.subject == Z_NET_TFTP_PUT) handle_tftp_put_request(&msg);
			else if (msg.subject == Z_PORT_CONNECT) handle_telnet_port_connect(&msg);
			else if (msg.subject == Z_PORT_DATA) handle_telnet_port_data(&msg);
			else if (msg.subject == Z_PORT_CLOSE) handle_telnet_port_close(&msg);
			else tftp_handle_stream_msg(&msg);
		}

		check_tftp_progress();
		tcp_poll();
		telnet_poll();

		for (volatile int i = 0; i < 500; i++) ; // light throttle

	}

}
