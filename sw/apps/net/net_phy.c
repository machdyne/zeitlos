/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Runtime NIC driver selection. See net_phy.h for why this is a
 * runtime choice rather than a build-time one.
 *
 * The two drivers already had identical signatures before this
 * existed, which is what makes the table below mechanical: there are
 * no shims and no adapters, only two structs of five pointers.
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
};

static const net_phy_t phy_esp32link = {
	"esp32link",
	esp32link_init,
	esp32link_recv,
	esp32link_send,
	esp32link_debug_dump,
	esp32link_poll_wifi,
};

static const net_phy_t phy_rmii = {
	"rmii",
	rmii_eth_init,
	rmii_eth_recv,
	rmii_eth_send,
	rmii_eth_debug_dump,
	0,
};

const net_phy_t *net_phy = 0;

const net_phy_t *net_phy_select(void)
{
	// Positive detection first. rtl/csrs.vh mirrors rtl/boards.vh's
	// own `ifdefs bit for bit, so these two bits are exactly "was
	// this SOC built with that MAC".
	if (z_soc_has_feature(Z_FEATURE_ESP32_LINK))
		net_phy = &phy_esp32link;
	else if (z_soc_has_feature(Z_FEATURE_ETH_RMII))
		net_phy = &phy_rmii;
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
