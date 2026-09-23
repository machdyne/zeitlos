/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net's USB ethernet (CDC-ECM) backend. See usb_ecm.h.
 */

#include <stdio.h>
#include <string.h>

#include "usb_ecm.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zusbnet.h"

// How long the link may be quiet before this asks less often, and the
// gap it settles at. A poll costs one USB transaction that the adapter
// NAKs, so the cost of asking is small; the cost of WAKING is not --
// net is a process, and a process that wakes 732 times a second takes a
// full scheduler share from whatever is in the foreground
// (docs/networking.md). 16 ticks is about 22 ms, so an idle link
// answers a ping in at most that.
#define ECM_BUSY_HOLD   (Z_TICK_HZ / 2)     // "recently active"
#define ECM_GAP_IDLE    16

// What the adapter can hold while we are not asking. A USB device does
// not drop a frame when the host is slow: it NAKs, and the frame waits
// in its own buffer. The number that matters is therefore the
// ADAPTER's, which nothing reports -- the RTL8152's is tens of KB, a
// small gadget's may be one frame. Four segments is deliberately
// conservative, the same order as the ENC28J60's ring, and it has room
// to grow once this is measured on hardware (docs/usb_ethernet.md,
// "Throughput").
#define ECM_RX_CAPACITY (4 * 536)

static uint8_t our_mac[6];
static bool have_mac;
static uint32_t gen;            // the adapter generation net is using
static uint32_t last_active;
static uint32_t rx_frames, tx_frames, tx_fail;
static bool was_present;

bool usb_ecm_supported(void)
{
	z_usbnet_info_t in;
	// Both questions matter: a bitstream without the controller, and a
	// kernel too old to have this syscall at all (it leaves n at -1).
	if (!z_soc_has_feature2(Z_FEATURE2_USB_HOST)) return false;
	return z_usbnet_info(&in);
}

bool usb_ecm_init(const uint8_t mac[6])
{
	z_usbnet_info_t in;
	int waited = 0;

	if (!z_usbnet_info(&in)) {
		printf("net: this kernel has no USB ethernet support\n");
		return false;
	}

	// Wait, rather than fail. On a board whose only NIC is a USB
	// adapter, net starting before the adapter is plugged in is the
	// normal case, not an error -- and there is nothing else to
	// restart net when one appears.
	while (!in.present) {
		if (!waited)
			printf("net: waiting for a USB ethernet adapter "
				"(CDC-ECM) -- plug one in\n");
		waited++;
		z_proc_wait(Z_TICK_HZ / 2);
		z_usbnet_info(&in);
	}

	gen = in.gen;
	was_present = true;
	last_active = z_uptime_ticks();

	if (in.mac_ok) {
		memcpy(our_mac, in.mac, 6);
	} else {
		// No iMACAddress string, so the kernel has already put it in
		// promiscuous mode and net's own address is the one on the
		// wire.
		memcpy(our_mac, mac, 6);
		printf("net: adapter reports no MAC address of its own\n");
	}
	have_mac = true;

	// Tell the kernel which address net sends from, so that an adapter
	// swapped later is made promiscuous rather than filtering our
	// traffic out.
	z_usbnet_open(our_mac);

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

// Notices an adapter arriving or leaving, for the log. Cheap: one
// syscall with no bus traffic, and only from the paths below that
// already found nothing to do.
static void check_gen(void)
{
	z_usbnet_info_t in;

	if (!z_usbnet_info(&in)) return;
	if (in.present == was_present && in.gen == gen) return;

	if (!in.present) {
		printf("net: usb ethernet adapter unplugged\n");
	} else {
		printf("net: usb ethernet adapter back%s\n",
			in.promisc ? " (promiscuous: a different one, so this "
			"keeps our address)" : "");
		// A fresh bind may need to be told our address again -- the
		// kernel keeps it across binds, but not across a kernel that
		// was restarted under us.
		z_usbnet_open(our_mac);
	}
	was_present = in.present;
	gen = in.gen;
}

uint16_t usb_ecm_recv(uint8_t *buf, uint16_t maxlen)
{
	int32_t n = z_usbnet_recv(buf, maxlen);

	if (n > 0) {
		rx_frames++;
		last_active = z_uptime_ticks();
		return (uint16_t)n;
	}
	// 0 is an idle adapter, -1 is no adapter; either way there is no
	// frame, and this is the moment to notice a plug event.
	check_gen();
	return 0;
}

bool usb_ecm_send(const uint8_t *buf, uint16_t len)
{
	if (z_usbnet_send(buf, len) != 0) {
		tx_fail++;
		check_gen();
		return false;
	}
	tx_frames++;
	last_active = z_uptime_ticks();
	return true;
}

uint32_t usb_ecm_idle_ticks(void)
{
	if (!was_present) return ECM_GAP_IDLE;
	if ((z_uptime_ticks() - last_active) < ECM_BUSY_HOLD) return 1;
	return ECM_GAP_IDLE;
}

void usb_ecm_debug_dump(void)
{
	z_usbnet_info_t in;

	if (!z_usbnet_info(&in)) {
		printf("net: usb ethernet: no kernel support\n");
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
	// The kernel's counters and net's own. They should agree; if they
	// do not, frames are being lost between the two, which is the one
	// thing this dump exists to show.
	printf("net: usb ethernet: kernel rx %lu tx %lu, errors rx %lu tx "
		"%lu, dropped %lu\n", (unsigned long)in.rx, (unsigned long)in.tx,
		(unsigned long)in.rx_err, (unsigned long)in.tx_err,
		(unsigned long)in.rx_drop);
	printf("net: usb ethernet: net rx %lu tx %lu, failed sends %lu, "
		"polling every %lu tick(s)\n", (unsigned long)rx_frames,
		(unsigned long)tx_frames, (unsigned long)tx_fail,
		(unsigned long)usb_ecm_idle_ticks());
}
