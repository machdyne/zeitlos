/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Driver for rtl/ethmac_rmii.v. See rmii_eth.h.
 *
 * There's no chip on the other end of a register read to fail to
 * respond (unlike enc28j60_init(), which can tell a real SPI
 * communication failure from a working chip via enc28j60_revision())
 * -- if rtl/ethmac_rmii.v wasn't actually built into this bitstream,
 * reads from 0x6000_0000 land on whatever that address happens to
 * decode to (nothing, on a board without ETH_RMII -- see sysctl.v),
 * not a detectably-absent chip. rmii_eth_init() can't do better than
 * print what it sees and trust the build matched the board -- see
 * this driver only getting linked in at all via NET_PHY=RMII in
 * sw/apps/net/Makefile, which is the actual point where that gets
 * decided.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "rmii_eth.h"

// loop iterations, not calibrated to real time -- mirrors
// enc28j60.c's ETH_TX_TIMEOUT. Sized to comfortably cover one real
// transmission (preamble+max frame+FCS+IFG is a few hundred
// eth_refclk cycles, i.e. a few microseconds) plus a generous margin,
// not just a register poll.
#define RMII_TX_TIMEOUT 2000000

bool rmii_eth_init(const uint8_t mac[6]) {

	(void)mac;  // unused -- see rmii_eth.h's header comment

	uint32_t status = reg_ethmac_status;
	printf("net: rmii eth: status=0x%03lx crs_dv=%d (a single read here can't confirm "
		"ETH_REFCLK is toggling -- its heartbeat period is ~168ms, see STATUS bit1 in "
		"rtl/ethmac_rmii.v if you need to check that separately)\n",
		(unsigned long)(status & 0xfff),
		(status & REG_ETHMAC_CRS_DV) ? 1 : 0);

	return true;
}

void rmii_eth_debug_dump(void) {

	uint32_t status = reg_ethmac_status;

	printf("net: rmii eth: status=0x%08lx crs_dv=%d refclk_hb=%d rx_ready=%d "
		"tx_busy=%d rx_drop=%lu rx_err=%lu\n",
		(unsigned long)status,
		(status & REG_ETHMAC_CRS_DV) ? 1 : 0,
		(status & REG_ETHMAC_REFCLK_HB) ? 1 : 0,
		(status & REG_ETHMAC_RX_READY) ? 1 : 0,
		(status & REG_ETHMAC_TX_BUSY) ? 1 : 0,
		(unsigned long)((status >> REG_ETHMAC_RX_DROP_SHIFT) & REG_ETHMAC_RX_DROP_MASK),
		(unsigned long)((status >> REG_ETHMAC_RX_ERR_SHIFT) & REG_ETHMAC_RX_ERR_MASK));
}

uint16_t rmii_eth_recv(uint8_t *buf, uint16_t maxlen) {

	if (!(reg_ethmac_status & REG_ETHMAC_RX_READY))
		return 0;

	uint16_t len = (uint16_t)reg_ethmac_rxlen;
	uint16_t to_read = (len < maxlen) ? len : maxlen;

	// words, not bytes -- rounds up so a partial last word is still
	// read (and its in-range bytes copied out below), same as
	// enc28j60_recv() truncating rather than dropping a frame whose
	// length isn't a multiple of anything in particular.
	uint16_t words = (to_read + 3) / 4;

	for (uint16_t i = 0; i < words; i++) {
		uint32_t w = reg_ethmac_rxbuf[i];
		uint16_t base = i * 4;
		if (base + 0 < to_read) buf[base + 0] = (uint8_t)(w & 0xff);
		if (base + 1 < to_read) buf[base + 1] = (uint8_t)((w >> 8) & 0xff);
		if (base + 2 < to_read) buf[base + 2] = (uint8_t)((w >> 16) & 0xff);
		if (base + 3 < to_read) buf[base + 3] = (uint8_t)((w >> 24) & 0xff);
	}

	// release the buffer regardless of truncation -- same contract
	// as enc28j60_recv(): the rest of an over-length frame is
	// consumed either way, just not copied out.
	reg_ethmac_rxctrl = 1;

	return to_read;
}

bool rmii_eth_send(const uint8_t *buf, uint16_t len) {

	if (len > (REG_ETHMAC_TXBUF_WORDS * 4))
		return false;

	// must not still be busy from a previous send -- see
	// rmii_eth.h's write-ordering contract. bounded wait rather than
	// an assert, in case a caller doesn't obey it.
	uint32_t timeout;
	for (timeout = 0; timeout < RMII_TX_TIMEOUT; timeout++) {
		if (!(reg_ethmac_status & REG_ETHMAC_TX_BUSY)) break;
	}
	if (reg_ethmac_status & REG_ETHMAC_TX_BUSY)
		return false;

	uint16_t words = (len + 3) / 4;
	for (uint16_t i = 0; i < words; i++) {
		uint16_t base = i * 4;
		uint32_t w = 0;
		if (base + 0 < len) w |= (uint32_t)buf[base + 0];
		if (base + 1 < len) w |= (uint32_t)buf[base + 1] << 8;
		if (base + 2 < len) w |= (uint32_t)buf[base + 2] << 16;
		if (base + 3 < len) w |= (uint32_t)buf[base + 3] << 24;
		reg_ethmac_txbuf[i] = w;
	}

	reg_ethmac_txlen = len;
	reg_ethmac_txctrl = 1;  // start -- value doesn't matter, see rtl/ethmac_rmii.v

	for (timeout = 0; timeout < RMII_TX_TIMEOUT; timeout++) {
		if (!(reg_ethmac_status & REG_ETHMAC_TX_BUSY)) break;
	}

	return !(reg_ethmac_status & REG_ETHMAC_TX_BUSY);
}
