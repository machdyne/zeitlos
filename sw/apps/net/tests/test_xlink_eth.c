/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * xlink_eth.c (netx's Ethernet over the XA-XD link) against a model of
 * rtl/xlink.v's registers, with the RP2040's requests written straight
 * into its receive buffer.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../xlink_eth.h"

static uint8_t rxbuf[2048], txbuf[2048];
static int rx_full, rx_len, rx_seq, tx_armed, tx_len, flags;

uint32_t xl_rd(uint32_t off)
{
	if (off >= 0x1000 && off < 0x1800) {
		uint32_t o = off - 0x1000;
		return rxbuf[o] | rxbuf[o + 1] << 8 | rxbuf[o + 2] << 16 | (uint32_t)rxbuf[o + 3] << 24;
	}
	switch (off) {
	case 0x0: return 0x4B4E4C58u;
	case 0x4: return (uint32_t)(rx_len & 0x7ff) << 16 | (uint32_t)(rx_seq & 0xff) << 8 | (uint32_t)rx_full;
	case 0x8: return (uint32_t)tx_armed;
	case 0xC: return (uint32_t)flags;
	case 0x10: return 7;
	}
	return 0;
}

void xl_wr(uint32_t off, uint32_t v)
{
	if (off >= 0x1800 && off < 0x2000) {
		uint32_t o = off - 0x1800;
		for (int k = 0; k < 4; k++) txbuf[o + k] = (uint8_t)(v >> (8 * k));
		return;
	}
	if (off == 0x4 && (v & 1)) rx_full = 0;
	if (off == 0x8) { tx_armed = (v >> 31) & 1; tx_len = v & 0x7ff; }
	if (off == 0xC) flags = v & 0xff;
}

static int fails, checks;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

/* the RP2040 sends a request (the link would refuse one while rx_full) */
static int rp_send(uint8_t op, uint8_t tag, uint8_t pc, const uint8_t *d, int n)
{
	if (rx_full || tx_armed)
		return -1;
	rxbuf[0] = op; rxbuf[1] = tag; rxbuf[2] = pc; rxbuf[3] = 0;
	if (n) memcpy(rxbuf + 4, d, (size_t)n);
	rx_len = 4 + n;
	rx_seq++;
	rx_full = 1;
	return 0;
}

/* ... and takes the reply */
static int rp_recv(uint8_t *rep)
{
	if (!tx_armed)
		return -1;
	memcpy(rep, txbuf, (size_t)tx_len);
	tx_armed = 0;
	return tx_len;
}

int main(void)
{
	uint8_t mac[6] = { 2, 0, 0, 0, 0, 1 }, buf[1600], rep[2048], frame[1514];
	int n, i;

	CHECK(xlink_eth_supported(), "the magic is seen");
	CHECK(xlink_eth_init(mac), "init");
	CHECK(flags == 0, "FLAGS: 0 until a HELLO is answered");

	/* HELLO */
	rp_send(XL_HELLO, 1, XL_PC_USB, 0, 0);
	CHECK(xlink_eth_recv(buf, sizeof(buf)) == 0, "HELLO gives netx no frame");
	CHECK(!rx_full, "the request is taken");
	n = rp_recv(rep);
	CHECK(n == 12, "HELLO's reply: header + 8");
	CHECK(rep[0] == 0x81 && rep[1] == 1 && rep[2] == XL_ST_OK, "HELLO's header");
	CHECK(rep[5] == XL_SYSTEM_ZEITLOS && (rep[6] & XL_CAP_NET), "Zeitlos, with the network");
	CHECK(flags == XL_F_UP, "FLAGS: up, nothing waiting, once HELLO is answered");

	/* the PC's interface is not up: frames from netx go nowhere */
	memset(frame, 0xAB, sizeof(frame));
	CHECK(xlink_eth_send(frame, 60), "a frame with no PC is gone, not refused");
	CHECK(!(flags & XL_F_FRAME), "and does not wait");

	/* NET_PUT from the PC, which now has its interface up */
	for (i = 0; i < 60; i++) frame[i] = (uint8_t)i;
	rp_send(XL_NET_PUT, 2, XL_PC_USB | XL_PC_NET, frame, 60);
	n = xlink_eth_recv(buf, sizeof(buf));
	CHECK(n == 60 && !memcmp(buf, frame, 60), "NET_PUT: netx gets the frame");
	n = rp_recv(rep);
	CHECK(n == 4 && rep[0] == (XL_NET_PUT | 0x80) && rep[1] == 2 && rep[2] == XL_ST_OK, "NET_PUT's reply");

	/* a full-size frame, unaligned bytes on both sides */
	for (i = 0; i < 1514; i++) frame[i] = (uint8_t)(i * 7 + 3);
	rp_send(XL_NET_PUT, 3, XL_PC_USB | XL_PC_NET, frame, 1514);
	n = xlink_eth_recv(buf, sizeof(buf));
	CHECK(n == 1514 && !memcmp(buf, frame, 1514), "a 1514-byte frame");
	rp_recv(rep);

	/* too short, too long */
	rp_send(XL_NET_PUT, 4, XL_PC_USB | XL_PC_NET, frame, 10);
	CHECK(xlink_eth_recv(buf, sizeof(buf)) == 0, "a runt is not a frame");
	rp_recv(rep);
	CHECK(rep[2] == XL_ST_BAD, "a runt's reply: BAD");
	rp_send(XL_NET_PUT, 5, XL_PC_USB | XL_PC_NET, frame, 200);
	CHECK(xlink_eth_recv(buf, 100) == 0, "a frame bigger than netx's buffer is refused");
	rp_recv(rep);
	CHECK(rep[2] == XL_ST_BAD, "... BAD");

	/* NET_GET with nothing waiting */
	rp_send(XL_NET_GET, 6, XL_PC_USB | XL_PC_NET, 0, 0);
	xlink_eth_recv(buf, sizeof(buf));
	n = rp_recv(rep);
	CHECK(n == 4 && rep[2] == XL_ST_NONE, "NET_GET, nothing: NONE");

	/* netx sends: they wait, FLAGS says so, NET_GET takes them in order */
	CHECK(xlink_eth_tx_room() == XLINK_TXQ, "room for all");
	for (i = 0; i < XLINK_TXQ; i++) {
		memset(frame, 0x10 + i, 64 + i);
		CHECK(xlink_eth_send(frame, (uint16_t)(64 + i)), "queued");
	}
	CHECK(flags & XL_F_FRAME, "FLAGS: a frame waits");
	CHECK(xlink_eth_tx_room() == 0, "no room");
	CHECK(!xlink_eth_send(frame, 64), "a fifth: refused (netx keeps it)");
	for (i = 0; i < XLINK_TXQ; i++) {
		rp_send(XL_NET_GET, (uint8_t)(10 + i), XL_PC_USB | XL_PC_NET, 0, 0);
		xlink_eth_recv(buf, sizeof(buf));
		n = rp_recv(rep);
		CHECK(n == 4 + 64 + i && rep[2] == XL_ST_OK && rep[4] == 0x10 + i && rep[4 + 63 + i] == 0x10 + i,
			"NET_GET: the oldest, whole");
	}
	CHECK(!(flags & XL_F_FRAME), "FLAGS: none waiting");
	CHECK(flags & XL_F_UP, "FLAGS: still up");

	/* the queue wraps */
	for (i = 0; i < 10; i++) {
		memset(frame, i, 100);
		xlink_eth_send(frame, 100);
		rp_send(XL_NET_GET, (uint8_t)(20 + i), XL_PC_USB | XL_PC_NET, 0, 0);
		xlink_eth_recv(buf, sizeof(buf));
		n = rp_recv(rep);
		CHECK(n == 104 && rep[4] == i && rep[103] == i, "wrapped");
	}

	/* a request between frames is served and the frame still comes */
	rp_send(XL_HELLO, 40, XL_PC_USB | XL_PC_NET, 0, 0);
	CHECK(xlink_eth_recv(buf, sizeof(buf)) == 0, "HELLO again");
	rp_recv(rep);
	rp_send(XL_NET_PUT, 41, XL_PC_USB | XL_PC_NET, frame, 60);
	CHECK(xlink_eth_recv(buf, sizeof(buf)) == 60, "then a frame");
	rp_recv(rep);

	/* an op it does not know */
	rp_send(99, 50, 0, 0, 0);
	xlink_eth_recv(buf, sizeof(buf));
	n = rp_recv(rep);
	CHECK(n == 4 && rep[0] == (99 | 0x80) && rep[2] == XL_ST_BAD, "unknown op: BAD");

	/* the PC's interface goes down: frames are dropped, not queued */
	rp_send(XL_HELLO, 51, XL_PC_USB, 0, 0);
	xlink_eth_recv(buf, sizeof(buf));
	rp_recv(rep);
	xlink_eth_send(frame, 60);
	CHECK(!(flags & XL_F_FRAME), "PC down: nothing waits");

	/* a 2048-byte message (length 0 on the wire) is not a frame netx takes */
	rp_send(XL_NET_PUT, 52, XL_PC_USB | XL_PC_NET, frame, 0);
	rx_len = 0;
	CHECK(xlink_eth_recv(buf, sizeof(buf)) == 0, "2048 bytes: refused");
	rp_recv(rep);
	CHECK(rep[2] == XL_ST_BAD, "... BAD");

	printf("test_xlink_eth: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
