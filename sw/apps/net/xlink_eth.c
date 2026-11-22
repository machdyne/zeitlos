/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's Ethernet over the XA-XD link to the carrier's RP2040 (Sergei
 * and Mozart; `XLINK, rtl/xlink.v; docs/xlink.md). Built with -DNETX
 * only.
 *
 * The RP2040 (machboot-sv) is the PC's USB network adapter, CDC-ECM, and
 * the link's master: it asks, and Zeitlos answers each request before
 * the next. So frames go like this:
 *
 *   PC -> Zeitlos   NET_PUT: the request carries the frame; the reply
 *                   says it was taken (or BUSY: dropped, as a full NIC
 *                   would).
 *   Zeitlos -> PC   the frame waits here (up to XLINK_TXQ) and FLAGS bit 1
 *                   says so; the RP2040, polling STATUS, asks NET_GET and
 *                   the reply carries the oldest.
 *
 * HELLO's reply tells the RP2040 it is Zeitlos it talks to (machboot's
 * says 0 there), and that it can do the network. FLAGS stay 0 until a
 * HELLO has been answered: the RP2040 reads them as machboot's until it
 * knows (bit 1 would be machboot's "starting"). Every request's pc byte
 * says whether the PC has its interface up, and the RP2040 sends HELLO
 * again whenever that changes; frames for a PC that does not have it up
 * are dropped here rather than queued for nobody.
 *
 * recv() is where the link is served: eth_poll() calls it whenever net
 * wakes, which a message's arrival does (its edge is ORed into
 * Z_IRQ_ETH).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "xlink_eth.h"

#ifdef XLINK_ETH_HOST
/* a host test's model of rtl/xlink.v (machboot rp2040/tests/xlink_model.h) */
uint32_t xl_rd(uint32_t off);
void xl_wr(uint32_t off, uint32_t v);
#define xlink_present() true
#else
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#define XL_BASE 0x64000000u
static inline uint32_t xl_rd(uint32_t off) { return *(volatile uint32_t *)(uintptr_t)(XL_BASE + off); }
static inline void xl_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(uintptr_t)(XL_BASE + off) = v; }
#define xlink_present() z_xlink()
#endif

#define R_MAGIC	0x0000
#define R_RX	0x0004
#define R_TX	0x0008
#define R_FLAGS	0x000C
#define R_FRAMES 0x0010
#define B_RX	0x1000
#define B_TX	0x1800
#define MAGIC	0x4B4E4C58u		/* "XLNK" */

static struct { uint16_t len; uint8_t f[1536]; } txq[XLINK_TXQ];
static int txq_head, txq_n;
static uint8_t pc_state;
static uint8_t flags;
static bool hello_done;		/* FLAGS stay 0 until then: to machboot's eyes they mean other things */
static bool paused;		/* share0 has the link (xlink_eth_pause) */
#ifndef XLINK_ETH_HOST
static uint32_t paused_check;
#endif
static uint32_t n_put, n_get, n_busy, n_dropped;

/* FLAGS' bits 7:4 are the card's generation (share0 counts it, as
   machboot does: the RP2040 tells the PC the medium changed): kept */
static void set_flags(void)
{
	flags = (uint8_t)((xl_rd(R_FLAGS) & 0xF0) |
		(hello_done ? XL_F_UP | (txq_n ? XL_F_FRAME : 0) : 0));
	xl_wr(R_FLAGS, flags);
}

void xlink_eth_pause(bool on)
{
	paused = on;
	if (on) {
		txq_head = txq_n = 0;	/* frames for the PC: gone, as on a pulled cable */
		return;
	}
	set_flags();
}

bool xlink_eth_paused(void)
{
	return paused;
}

/* bytes from the receive buffer, at off */
static void rx_copy(uint8_t *dst, uint32_t off, uint32_t n)
{
	uint32_t i, w = 0;
	for (i = 0; i < n; i++) {
		uint32_t a = off + i;
		if (i == 0 || (a & 3) == 0)
			w = xl_rd(B_RX + (a & ~3u));
		dst[i] = (uint8_t)(w >> (8 * (a & 3)));
	}
}

/* the reply: its header, then n bytes of data; armed */
static void reply(uint8_t op, uint8_t tag, uint8_t status, uint8_t nb,
		const uint8_t *data, uint32_t n)
{
	uint32_t i, w;
	xl_wr(B_TX, (uint32_t)(op | 0x80) | (uint32_t)tag << 8 | (uint32_t)status << 16 |
		(uint32_t)nb << 24);
	for (i = 0; i < n; i += 4) {
		w = 0;
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)data[i + k] << (8 * k);
		xl_wr(B_TX + 4 + i, w);
	}
	xl_wr(R_TX, 0x80000000u | ((4 + n) & 0x7FF));
}

bool xlink_eth_supported(void)
{
	return xlink_present() && xl_rd(R_MAGIC) == MAGIC;
}

bool xlink_eth_init(const uint8_t mac[6])
{
	(void)mac;			/* the PC's side is the RP2040's; this MAC is netx's */
	txq_head = txq_n = 0;
	hello_done = false;
	set_flags();
	return true;
}

uint16_t xlink_eth_recv(uint8_t *buf, uint16_t maxlen)
{
	if (paused) {
#ifndef XLINK_ETH_HOST
		/* share0 gone without a word (it crashed): the link is ours again */
		uint32_t pid, now = z_uptime_ticks();
		if (now - paused_check >= Z_TICK_HZ) {
			paused_check = now;
			if (!z_pid_lookup("share0", &pid))
				xlink_eth_pause(false);
		}
#endif
		if (paused)
			return 0;
	}
	for (;;) {
		uint32_t rx = xl_rd(R_RX), len;
		uint8_t h[4];
		if (!(rx & 1))
			return 0;
		len = (rx >> 16) & 0x7FF;
		if (len == 0)
			len = 2048;
		rx_copy(h, 0, 4);
		pc_state = h[2];
		if (len < 4) {
			xl_wr(R_RX, 1);
			continue;
		}
		switch (h[0]) {
		case XL_HELLO: {
			static const uint8_t hello[8] = { 0, XL_SYSTEM_ZEITLOS, XL_CAP_NET, 0, 0, 0, 0, 0 };
			reply(XL_HELLO, h[1], XL_ST_OK, 0, hello, sizeof(hello));
			xl_wr(R_RX, 1);
			hello_done = true;
			set_flags();
			continue;
		}
		case XL_NET_PUT: {
			uint32_t n = len - 4;
			if (n > maxlen || n < 14) {
				reply(XL_NET_PUT, h[1], XL_ST_BAD, 0, 0, 0);
				xl_wr(R_RX, 1);
				continue;
			}
			rx_copy(buf, 4, n);
			reply(XL_NET_PUT, h[1], XL_ST_OK, 0, 0, 0);
			xl_wr(R_RX, 1);
			n_put++;
			return (uint16_t)n;
		}
		case XL_NET_GET:
			if (txq_n) {
				int i = txq_head;
				reply(XL_NET_GET, h[1], XL_ST_OK, 0, txq[i].f, txq[i].len);
				txq_head = (txq_head + 1) % XLINK_TXQ;
				txq_n--;
				n_get++;
			} else {
				reply(XL_NET_GET, h[1], XL_ST_NONE, 0, 0, 0);
			}
			set_flags();
			xl_wr(R_RX, 1);
			continue;
		default:
			reply(h[0], h[1], XL_ST_BAD, 0, 0, 0);
			xl_wr(R_RX, 1);
			continue;
		}
	}
}

bool xlink_eth_send(const uint8_t *buf, uint16_t len)
{
	int i;
	if (paused || !(pc_state & XL_PC_NET) || len > sizeof(txq[0].f)) {
		n_dropped++;
		return true;		/* nobody to take it: gone, as on a wire with nobody on it */
	}
	if (txq_n == XLINK_TXQ) {
		n_busy++;
		return false;
	}
	i = (txq_head + txq_n) % XLINK_TXQ;
	memcpy(txq[i].f, buf, len);
	txq[i].len = len;
	txq_n++;
	set_flags();
	return true;
}

int xlink_eth_tx_room(void)
{
	return XLINK_TXQ - txq_n;
}

void xlink_eth_debug_dump(void)
{
	printf("xlink: frames %lu, pc %s%s, to Zeitlos %lu, to the PC %lu, full %lu, no PC %lu, waiting %d\n",
		(unsigned long)xl_rd(R_FRAMES), pc_state & XL_PC_USB ? "usb" : "-",
		pc_state & XL_PC_NET ? "+net" : "", (unsigned long)n_put, (unsigned long)n_get,
		(unsigned long)n_busy, (unsigned long)n_dropped, txq_n);
}
