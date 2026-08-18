/*
 * net -- ARP + ICMP echo (ping) + TFTP client
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
 * IP config is static (see docs/networking.md for why). The netmask
 * and gateway below are ASSUMED (a typical home-router /24 with
 * gateway at .1) -- only the IP address (192.168.178.230) was
 * actually specified. Correct if wrong; for same-subnet traffic it
 * won't matter, since the gateway is only consulted for destinations
 * outside the local subnet.
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
#include "net_phy.h"
#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "tftp.h"

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

	printf("net: ip 192.168.178.230/24, listening (arp + icmp echo + tftp)\n");

	while (1) {

		eth_poll();

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_STREAM_OPEN) handle_stream_open(&msg);
			else if (msg.subject == Z_NET_TFTP_PUT) handle_tftp_put_request(&msg);
			else tftp_handle_stream_msg(&msg);
		}

		check_tftp_progress();

		for (volatile int i = 0; i < 500; i++) ; // light throttle

	}

}
