/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Driver for rtl/usbd_ep.v's frame endpoint. See usbd_eth.h.
 *
 * The gateware gathers each frame the PC sends into one of two 2 KB
 * receive slots and raises Z_IRQ_ETH when one is ready, so this reads
 * a whole frame from a window, as rmii_eth.c does; and sends one by
 * writing it to the transmit slot with its length. Over USB nothing is
 * lost while a slot is full: the endpoint NAKs, and the PC keeps the
 * frame until there is room.
 *
 * Sending differs from a MAC in one way: the frame leaves only when the
 * PC asks for it. A PC that is not listening -- not plugged in, the
 * interface down -- leaves it there. So send() waits only a few
 * milliseconds for the slot, tx_room() tells TCP whether there is one,
 * and with the endpoint not enabled at all (no PC has selected the
 * data interface) a frame goes nowhere, as into an unplugged cable.
 *
 * NCTL bit 0 is this side's link: set by init(), it is what the PC is
 * told (usbd sends NETWORK_CONNECTION).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zusbd.h"
#include "usbd_eth.h"

#define USBD_TX_WAIT_TICKS	4		/* ~5 ms */

bool usbd_eth_supported(void) {
	return z_usb_dev() && zu_rd(ZU_MAGIC) == ZU_MAGIC_VAL;
}

bool usbd_eth_init(const uint8_t mac[6]) {
	(void)mac;		// no filter here: the PC sends us what is ours
	zu_wr(ZU_NTXCTL, ZU_NTX_ABANDON);
	while (zu_rd(ZU_NSTAT) & ZU_N_RX_READY)	// what a previous run left
		zu_wr(ZU_NRXCTL, 1);
	zu_wr(ZU_NCTL, 1);			// up: the PC is told
	printf("net: usb device ethernet: the PC's side is 02:5a:4c:00:00:01%s\n",
		(zu_rd(ZU_NSTAT) & ZU_N_ENABLED) ? "" : " (no PC yet)");
	return true;
}

void usbd_eth_debug_dump(void) {
	uint32_t s = zu_rd(ZU_NSTAT);
	printf("net: usbd eth: nstat=0x%08lx rx_ready=%d tx_busy=%d enabled=%d link=%d "
		"dropped=%lu\n", (unsigned long)s,
		(s & ZU_N_RX_READY) ? 1 : 0, (s & ZU_N_TX_BUSY) ? 1 : 0,
		(s & ZU_N_ENABLED) ? 1 : 0, (s & ZU_N_LINK) ? 1 : 0,
		(unsigned long)ZU_N_DROPS(s));
}

uint16_t usbd_eth_recv(uint8_t *buf, uint16_t maxlen) {
	uint32_t s = zu_rd(ZU_NSTAT);
	if (!(s & ZU_N_RX_READY))
		return 0;
	uint16_t len = (uint16_t)ZU_N_RXLEN(s);
	uint16_t n = (len < maxlen) ? len : maxlen;
	uint16_t words = (uint16_t)((n + 3) / 4);
	for (uint16_t i = 0; i < words; i++) {
		uint32_t w = zu_rd(ZU_NRXBUF + 4u * i);
		uint16_t b = (uint16_t)(i * 4);
		if (b + 0 < n) buf[b + 0] = (uint8_t)w;
		if (b + 1 < n) buf[b + 1] = (uint8_t)(w >> 8);
		if (b + 2 < n) buf[b + 2] = (uint8_t)(w >> 16);
		if (b + 3 < n) buf[b + 3] = (uint8_t)(w >> 24);
	}
	zu_wr(ZU_NRXCTL, 1);		// the slot is free; a longer frame's rest goes with it
	return n;
}

int usbd_eth_tx_room(void) {
	uint32_t s = zu_rd(ZU_NSTAT);
	if (!(s & ZU_N_ENABLED))
		return 1;		// nowhere to go: no reason to hold anything back
	return (s & ZU_N_TX_BUSY) ? 0 : 1;
}

bool usbd_eth_send(const uint8_t *buf, uint16_t len) {
	if (len == 0 || len > ZU_FRAME_MAX)
		return false;
	if (!(zu_rd(ZU_NSTAT) & ZU_N_ENABLED))
		return true;		// no PC: gone, as into an unplugged cable
	uint32_t t0 = z_uptime_ticks();
	while (zu_rd(ZU_NSTAT) & ZU_N_TX_BUSY) {
		if (z_uptime_ticks() - t0 > USBD_TX_WAIT_TICKS)
			return false;	// the PC is not taking frames
	}
	uint16_t words = (uint16_t)((len + 3) / 4);
	for (uint16_t i = 0; i < words; i++) {
		uint16_t b = (uint16_t)(i * 4);
		uint32_t w = 0;
		if (b + 0 < len) w |= (uint32_t)buf[b + 0];
		if (b + 1 < len) w |= (uint32_t)buf[b + 1] << 8;
		if (b + 2 < len) w |= (uint32_t)buf[b + 2] << 16;
		if (b + 3 < len) w |= (uint32_t)buf[b + 3] << 24;
		zu_wr(ZU_NTXBUF + 4u * i, w);
	}
	zu_wr(ZU_NTXCTL, len | ZU_NTX_SEND);
	return true;
}
