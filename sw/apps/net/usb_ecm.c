/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net's USB ethernet (CDC-ECM) backend. See usb_ecm.h.
 *
 * Since Phase 3 the adapter is the `usb` driver's, and frames move
 * through two rings shared with it (sw/common/zusbnet.h): `usb` asks the
 * adapter for frames and leaves them in its ring, and sends net a
 * Z_USB_EVENT when its ring stops being empty; net leaves frames to
 * send in its own ring and kicks `usb` when that one stops being
 * empty. So net no longer polls the adapter -- it sleeps until a frame
 * is there -- and `usb` keeps the "busy" and "idle" pace net used to.
 */

#include <stdio.h>
#include <string.h>

#include "usb_ecm.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zusbc.h"
#include "../../common/zusbnet.h"

static z_usbnet_cli_t cli;			// net's ring; `usb` reads it
static const volatile z_usbnet_srv_t *srv;	// `usb`'s ring; net reads it

static uint8_t our_mac[6];
static bool have_mac;
static uint32_t gen;            // the adapter generation net is using
static bool was_present;
static uint32_t rx_frames, tx_frames, tx_full;
static uint32_t last_check;     // ticks: the last look for a restarted `usb`

bool usb_ecm_supported(void)
{
	// Only the hardware: `usb` may not have registered yet when net
	// starts, and usb_ecm_init() waits for it.
	return z_soc_has_feature2(Z_FEATURE2_USB_HOST);
}

// An adapter is attached now (and `usb` is running): net's choice
// between a silent ESP32 link and a USB adapter (net.c).
bool usb_ecm_present(void)
{
	z_usbnet_info_t in;
	return z_usbnet_info(&in) && in.present;
}

// Take the adapter: give `usb` our ring and our address, and keep its
// ring. NULL if `usb` is not there.
static bool open_rings(void)
{
	srv = z_usbnet_open(our_mac, &cli);
	if (!srv) return false;
	gen = srv->info.gen;
	was_present = srv->info.present;
	return true;
}

bool usb_ecm_init(const uint8_t mac[6])
{
	z_usbnet_info_t in;
	int waited = 0;

	// Wait, rather than fail -- for `usb` to start, and then for an
	// adapter. On a board whose only NIC is a USB adapter, net starting
	// before the adapter is plugged in is the normal case, not an error,
	// and there is nothing else to restart net when one appears.
	while (!z_usbnet_info(&in) || !in.present) {
		if (!waited)
			printf("net: waiting for a USB ethernet adapter "
				"(CDC-ECM) -- plug one in\n");
		waited++;
		z_proc_wait(Z_TICK_HZ / 2);
	}

	if (in.mac_ok) {
		memcpy(our_mac, in.mac, 6);
	} else {
		// No iMACAddress string, so `usb` has already put it in
		// promiscuous mode and net's own address is the one on the
		// wire.
		memcpy(our_mac, mac, 6);
		printf("net: adapter reports no MAC address of its own\n");
	}
	have_mac = true;

	// Our address goes with the open, so that an adapter swapped later
	// is made promiscuous rather than filtering our traffic out.
	if (!open_rings()) {
		printf("net: the usb driver did not open the adapter\n");
		return false;
	}
	last_check = z_uptime_ticks();

	printf("net: usb ethernet adapter ready%s%s\n",
		in.promisc ? ", promiscuous" : "",
		in.maxseg && in.maxseg < 1514 ? ", SHORT max segment" : "");
	return true;
}

bool usb_ecm_mac(uint8_t mac[6])
{
	if (!have_mac) return false;
	memcpy(mac, our_mac, 6);
	return true;
}

// Notices an adapter arriving or leaving, for the log, from `usb`'s
// ring -- memory, not a request. About once a second it also checks
// that `usb` is the one the rings were opened with: a restarted `usb`
// has a new ring, and the old one is no longer its memory.
static bool ref_out;		// a frame lent by recv_ref() and not yet done

static void check_gen(void)
{
	uint32_t now = z_uptime_ticks();

	// Not while net is reading a frame in place in the ring: reopening
	// would leave it reading the old ring, and recv_done() would step
	// the new ring's tail past a frame. It is done within this pass.
	if (ref_out) return;

	if (now - last_check >= Z_TICK_HZ) {
		last_check = now;
		// srv->len is set by NET_OPEN only: a new `usb` that got the
		// old one's pid and address has it 0.
		if (!srv || !srv->len || !z_usbnet_alive()) {
			srv = NULL;
			if (open_rings())
				printf("net: usb driver restarted; adapter reopened\n");
			return;
		}
	}
	if (!srv) return;
	if (srv->info.present == was_present && srv->info.gen == gen) return;

	if (!srv->info.present)
		printf("net: usb ethernet adapter unplugged\n");
	else
		printf("net: usb ethernet adapter back%s\n",
			srv->info.promisc ? " (promiscuous: a different one, so "
			"this keeps our address)" : "");
	was_present = srv->info.present;
	gen = srv->info.gen;
}

// When the frame net last took was put in usb's ring, and when net
// took it (cycle counter): the exact wait, for the TFTP timing line
// (tftp.c, through usb_ecm_frame_times()).
static uint32_t last_put, last_take;

void usb_ecm_frame_times(uint32_t *put, uint32_t *take)
{
	*put = last_put;
	*take = last_take;
}

// The frame in place: net reads it in usb's ring (any process may
// load from anywhere), and its slot is given back by recv_done(), so
// usb cannot reuse it meanwhile. One 1.5KB copy fewer per frame.
uint16_t usb_ecm_recv_ref(const uint8_t **frame)
{
	while (srv && srv->rx_head != cli.rx_tail) {
		uint32_t i = cli.rx_tail % Z_USBNET_RX_SLOTS;
		uint16_t n = srv->rx_len[i];
		if (!n || n > Z_USBNET_FRAME - Z_USBNET_RX_OFF) {	// cannot happen
			cli.rx_tail++;
			continue;
		}
		uint32_t c;
		__asm__ volatile ("rdcycle %0" : "=r"(c));
		last_put = srv->rx_at[i];
		last_take = c;
		*frame = (const uint8_t *)srv->rx[i] + Z_USBNET_RX_OFF;
		ref_out = true;
		return n;
	}
	check_gen();
	return 0;
}

void usb_ecm_recv_done(void)
{
	if (!ref_out) return;
	ref_out = false;
	cli.rx_tail++;		// after net is done with it: usb may reuse it
	rx_frames++;
}

uint16_t usb_ecm_recv(uint8_t *buf, uint16_t maxlen)
{
	uint32_t tail = cli.rx_tail;

	if (srv && srv->rx_head != tail) {
		uint32_t i = tail % Z_USBNET_RX_SLOTS;
		uint16_t n = srv->rx_len[i];
		if (n > maxlen) n = 0;		// cannot happen: both are a frame
		if (n) {
			uint32_t c;
			__asm__ volatile ("rdcycle %0" : "=r"(c));
			last_put = srv->rx_at[i];
			last_take = c;
			memcpy(buf, (const uint8_t *)srv->rx[i] + Z_USBNET_RX_OFF, n);
		}
		cli.rx_tail = tail + 1;		// after the copy: `usb` may reuse it
		if (n) rx_frames++;
		return n;
	}
	// No frame: the moment to notice a plug event.
	check_gen();
	return 0;
}

// Free slots in the transmit ring (net_phy.h tx_room).
int usb_ecm_tx_room(void)
{
	if (!srv || !srv->info.present) return 0;
	return (int)Z_USBNET_TX_SLOTS - (int)(cli.tx_head - srv->tx_tail);
}

bool usb_ecm_send(const uint8_t *buf, uint16_t len)
{
	uint32_t head = cli.tx_head;

	if (!srv || !srv->info.present || len > Z_USBNET_FRAME) {
		check_gen();
		return false;
	}
	// Full: `usb` has not taken the last ones yet. Dropped, as a MAC
	// with a full transmit buffer would; TCP sends it again.
	if (head - srv->tx_tail >= Z_USBNET_TX_SLOTS) {
		tx_full++;
		return false;
	}
	memcpy(cli.tx[head % Z_USBNET_TX_SLOTS], buf, len);
	cli.tx_len[head % Z_USBNET_TX_SLOTS] = len;
	cli.tx_head = head + 1;			// after the frame: `usb` may take it
	if (srv->tx_tail == head) z_usbnet_kick();	// it had emptied the ring
	tx_frames++;
	return true;
}

// For a TFTP retry (tftp.c): is a frame waiting in usb's ring that net
// has not taken? If so the block arrived and net was not woken for it.
void usb_ecm_note(void)
{
	if (!srv) { printf("net:   usb ring: not open\n"); return; }
	printf("net:   usb ring: %lu put, %lu taken (%s), usb rx %lu tx %lu, net rx %lu tx %lu\n",
		(unsigned long)srv->rx_head, (unsigned long)cli.rx_tail,
		srv->rx_head != cli.rx_tail ? "A FRAME WAITS" : "empty",
		(unsigned long)srv->info.rx, (unsigned long)srv->info.tx,
		(unsigned long)rx_frames, (unsigned long)tx_frames);
}

bool usb_ecm_rx_pending(void)
{
	return srv && srv->rx_head != cli.rx_tail;
}

// How long net may sleep for this backend's sake. `usb` wakes net when
// a frame arrives (Z_USB_EVENT), so only the once-a-second check above
// needs a timer.
uint32_t usb_ecm_idle_ticks(void)
{
	return Z_TICK_HZ;
}

void usb_ecm_debug_dump(void)
{
	z_usbnet_info_t in;

	if (!z_usbnet_info(&in)) {
		printf("net: usb ethernet: the usb driver is not running\n");
		return;
	}
	printf("net: usb ethernet: %s, link %s%s\n",
		in.present ? "adapter present" : "NO ADAPTER",
		in.link == Z_USBNET_LINK_UP ? "up" :
		in.link == Z_USBNET_LINK_DOWN ? "DOWN" : "unreported",
		in.promisc ? ", promiscuous" : "");
	printf("net: usb ethernet: mac %02x:%02x:%02x:%02x:%02x:%02x%s, "
		"max segment %d\n", our_mac[0], our_mac[1], our_mac[2],
		our_mac[3], our_mac[4], our_mac[5],
		in.mac_ok ? "" : " (ours, not the adapter's)", in.maxseg);
	// The driver's counters and net's own. They should agree; if they
	// do not, frames are being lost between the two, which is the one
	// thing this dump exists to show.
	printf("net: usb ethernet: usb rx %lu tx %lu, errors rx %lu tx "
		"%lu, dropped %lu\n", (unsigned long)in.rx, (unsigned long)in.tx,
		(unsigned long)in.rx_err, (unsigned long)in.tx_err,
		(unsigned long)in.rx_drop);
	printf("net: usb ethernet: net rx %lu tx %lu, send ring full %lu\n",
		(unsigned long)rx_frames, (unsigned long)tx_frames,
		(unsigned long)tx_full);
}
