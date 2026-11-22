/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The second RMII MAC (`ETH_RMII_B: Klinge's Ethernet B, at
 * 0x6300_0000; docs/netx.md), for netx: rmii_eth.c again, with its
 * registers, its DMA addresses and its functions renamed, so that each
 * MAC has a driver of its own (its own state too: every static in
 * rmii_eth.c is this file's). Built with -DNETX only.
 */

#include "../../common/zeitlos.h"

#undef reg_ethmac_status
#undef reg_ethmac_rxlen
#undef reg_ethmac_rxctrl
#undef reg_ethmac_txlen
#undef reg_ethmac_txctrl
#undef reg_ethmac_rxbuf
#undef reg_ethmac_txbuf
#undef Z_DMA_DEV_RMII_RX
#undef Z_DMA_DEV_RMII_TX
#define reg_ethmac_status (*(volatile uint32_t*)0x63000000)
#define reg_ethmac_rxlen  (*(volatile uint32_t*)0x63000004)
#define reg_ethmac_rxctrl (*(volatile uint32_t*)0x63000008)
#define reg_ethmac_txlen  (*(volatile uint32_t*)0x6300000c)
#define reg_ethmac_txctrl (*(volatile uint32_t*)0x63000010)
#define reg_ethmac_rxbuf  ((volatile uint32_t*)0x63000100)
#define reg_ethmac_txbuf  ((volatile uint32_t*)0x63000a00)
#define Z_DMA_DEV_RMII_RX 0x63000100u
#define Z_DMA_DEV_RMII_TX 0x63000a00u

#define rmii_eth_init       rmii_ethb_init
#define rmii_eth_debug_dump rmii_ethb_debug_dump
#define rmii_eth_rx_slots   rmii_ethb_rx_slots
#define rmii_eth_recv       rmii_ethb_recv
#define rmii_eth_send       rmii_ethb_send

#include "rmii_eth.c"
