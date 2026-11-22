/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Runtime NIC driver selection. See net_phy.h for why this is a
 * runtime choice rather than a build-time one.
 *
 * The two drivers already had identical signatures before this
 * existed, which is what makes the table below mechanical: there are
 * no shims and no adapters, only structs of function pointers.
 */

#include <stdbool.h>
#include <stdint.h>

#include "net_phy.h"
#include "../../common/zsoc.h"

static const net_phy_t phy_enc28j60 = {
	"enc28j60",
	enc28j60_init,
	enc28j60_recv,
	enc28j60_send,
	enc28j60_debug_dump,
	0,
	// 6656-byte ring (RXSTART_INIT..RXSTOP_INIT in enc28j60.c). Each
	// 536-byte segment occupies about 600 bytes of it once the
	// Ethernet header, the CRC and the chip's own 6-byte status
	// vector are counted, so eleven fit: 11 * 536 = 5896. Rounded
	// down to a whole number of segments with one spare.
	10 * 536,
	0,
	0,
	enc28j60_rx_rearm,
};

static const net_phy_t phy_esp32link = {
	"esp32link",
	esp32link_init,
	esp32link_recv,
	esp32link_send,
	esp32link_debug_dump,
	esp32link_poll_wifi,
	// 2048-byte FIFO (rtl/esp32_rxfifo.v). Three segments fit with
	// their framing; two is the safe advertisement.
	2 * 536,
	esp32link_idle_ticks,
	0,
	0,
	0,
	0,
	0,		// rx_mss: 536 -- the FIFO holds one full frame
	esp32link_tx_room,
};

// Not const: rx_capacity is filled in by net_phy_select() from the
// MAC itself. See below.
static net_phy_t phy_rmii = {
	"rmii",
	rmii_eth_init,
	rmii_eth_recv,
	rmii_eth_send,
	rmii_eth_debug_dump,
	0,
	// Placeholder: the ML1 value (four slots). Replaced at select
	// time with what the MAC reports.
	3 * 536,
	0,
};

// One frame per slot whatever its size (rtl/ethmac_rmii.v RX_SLOTS),
// and one slot held back for the gap between a frame landing and net
// being scheduled -- so four slots advertise three segments, as they
// always did, and two advertise one.
//
// Read from the hardware rather than assumed, because `ETH_RX_SLOTS is
// a per-build choice (rtl/boards.vh): every board builds four today,
// but a build with two that still advertised three segments would hit
// exactly the failure this field exists to prevent -- see rx_capacity
// in net_phy.h.
static uint16_t rmii_rx_capacity(void)
{
	unsigned slots = rmii_eth_rx_slots();
	unsigned segs = (slots > 1) ? slots - 1 : 1;
	return (uint16_t)(segs * 536);
}

// A USB adapter. No MAC of its own in the bitstream: the hardware is
// whatever is plugged into the USB host controller, so this backend
// can be chosen on any board that has one. See usb_ecm.h.
static const net_phy_t phy_usb_ecm = {
	"usb-ecm",
	usb_ecm_init,
	usb_ecm_recv,
	usb_ecm_send,
	usb_ecm_debug_dump,
	0,
	// The adapter's own buffer, which nothing reports; see usb_ecm.c.
	// Four full segments: adapters buffer 16KB and more.
	4 * 1460,
	usb_ecm_idle_ticks,
	usb_ecm_mac,
	0,
	usb_ecm_recv_ref,
	usb_ecm_recv_done,
	1460,
	usb_ecm_tx_room,
};

// The USB-C socket's CDC-ECM function (usbd_eth.c). Frames wait in
// the PC while both receive slots are full, so the window is the PC's
// buffering rather than ours: full-size segments, as for the adapter.
static const net_phy_t phy_usbd = {
	"usbd",
	usbd_eth_init,
	usbd_eth_recv,
	usbd_eth_send,
	usbd_eth_debug_dump,
	0,
	4 * 1460,
	0,
	0,
	0,
	0,
	0,
	1460,
	usbd_eth_tx_room,
};

const net_phy_t *net_phy = 0;

const net_phy_t *net_phy_select(int mode)
{
	// net.cfg asked for USB explicitly: a board with a MAC it should
	// ignore, or one whose MAC is unusable.
	if (mode == NET_PHY_USB) {
		net_phy = usb_ecm_supported() ? &phy_usb_ecm : 0;
		return net_phy;
	}
	// ... or the USB-C socket as a USB ethernet device, on a board
	// with a MAC it should not use.
	if (mode == NET_PHY_USBDEV) {
		net_phy = usbd_eth_supported() ? &phy_usbd : 0;
		return net_phy;
	}

	// Positive detection first. rtl/csrs.vh mirrors rtl/boards.vh's
	// own `ifdefs bit for bit, so these two bits are exactly "was
	// this SOC built with that MAC".
	if (z_soc_has_feature(Z_FEATURE_ESP32_LINK))
		net_phy = &phy_esp32link;
	else if (z_soc_has_feature(Z_FEATURE_ETH_RMII)) {
		phy_rmii.rx_capacity = rmii_rx_capacity();
		net_phy = &phy_rmii;
	}
	else if (z_soc_has_feature(Z_FEATURE_SPI_ETH))
		net_phy = &phy_enc28j60;
	else if (!z_soc_csrs_present())
		// Cannot ask -- this bitstream predates rtl/csrs.v. "Unknown"
		// is not "absent", so proceed with the driver NET_PHY used to
		// default to, and let phy_init() report the truth. An old
		// bitstream therefore behaves exactly as it did before this
		// file existed.
		net_phy = &phy_enc28j60;
	else
		// CSRs present and both bits clear: real, positive evidence
		// there is no MAC here. Which is not the same as no NIC any
		// more -- a USB ethernet adapter is one, and on a board with a
		// USB host controller it is the only one there was ever going
		// to be. phy=builtin in net.cfg keeps the old behaviour of
		// exiting instead.
		//
		// With the USB-C socket a USB device (`USB_DEV), that comes
		// first: the PC at the other end of the cable that powers the
		// board -- unless an adapter is plugged in already, which
		// somebody meant. phy=usb or phy=usbdev in net.cfg says which
		// when both are there.
		net_phy = (mode != NET_PHY_BUILTIN && usbd_eth_supported() &&
				!(usb_ecm_supported() && usb_ecm_present())) ? &phy_usbd :
			(mode != NET_PHY_BUILTIN && usb_ecm_supported()) ?
			&phy_usb_ecm : 0;

	return net_phy;
}

// The USB adapter instead of the chosen MAC: net.c, when the ESP32
// link chosen by detection never answers and an adapter is attached.
#ifdef NETX
static net_phy_t phy_rmii_b = {
	"rmii2",
	rmii_ethb_init,
	rmii_ethb_recv,
	rmii_ethb_send,
	rmii_ethb_debug_dump,
	0,
	3 * 536,
	0,
};

static const net_phy_t phy_xlink = {
	"xlink",
	xlink_eth_init,
	xlink_eth_recv,
	xlink_eth_send,
	xlink_eth_debug_dump,
	0,
	4 * 1460,
	0,
	0,
	0,
	0,
	0,
	1460,
	xlink_eth_tx_room,
};

// netx: the PHY of a kind (NET_PHY_BUILTIN, _USB, _USBDEV), or 0 when
// the board has none. A USB adapter counts when the host controller is
// there, plugged in or not; netx.c asks usb_ecm_present() itself.
const net_phy_t *net_phy_of(int kind)
{
	if (kind == NET_PHY_USB)
		return usb_ecm_supported() ? &phy_usb_ecm : 0;
	if (kind == NET_PHY_USBDEV)
		return usbd_eth_supported() ? &phy_usbd : 0;
	if (kind == NET_PHY_XLINK)
		return xlink_eth_supported() ? &phy_xlink : 0;
	if (kind == NET_PHY_BUILTIN2) {
		unsigned slots;
		if (!z_eth_rmii_b())
			return 0;	/* and nothing at 0x6300_0000 to read */
		slots = rmii_ethb_rx_slots();
		phy_rmii_b.rx_capacity = (uint16_t)((slots > 1 ? slots - 1 : 1) * 536);
		return &phy_rmii_b;
	}
	if (z_soc_has_feature(Z_FEATURE_ESP32_LINK))
		return &phy_esp32link;
	if (z_soc_has_feature(Z_FEATURE_ETH_RMII)) {
		phy_rmii.rx_capacity = rmii_rx_capacity();
		return &phy_rmii;
	}
	if (z_soc_has_feature(Z_FEATURE_SPI_ETH) || !z_soc_csrs_present())
		return &phy_enc28j60;
	return 0;
}
#endif

const net_phy_t *net_phy_use_usb(void)
{
	net_phy = &phy_usb_ecm;
	return net_phy;
}

// tcp.h: frames the NIC can take now.
int tcp_link_room(void)
{
	return (net_phy && net_phy->tx_room) ? net_phy->tx_room() : 255;
}
