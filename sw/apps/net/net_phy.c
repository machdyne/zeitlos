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

const net_phy_t *net_phy = 0;

const net_phy_t *net_phy_select(void)
{
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
		// there is no NIC here.
		net_phy = 0;

	return net_phy;
}
