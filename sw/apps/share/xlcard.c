/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * share's card over the XA-XD link (xlcard.h). The RP2040 is already
 * the PC's card reader for machboot (machboot's rp2040/src/msc.c); here
 * Zeitlos answers its requests as machboot's fw/xlink.c does, with the
 * card share has taken from fs:
 *
 *   HELLO  reply [shared][0x5A][caps][0][sectors 4]: 0x5A keeps the
 *          RP2040 in Zeitlos mode, caps bit 0 says netx is there
 *   READ   [lba 4][count][0][0][0], count 1-3: reply n = count, the
 *          sectors after the header
 *   WRITE  the same, then count x 512 bytes: reply n = sectors written
 *   EJECT  the PC ejected the drive: share gives the card back
 *   NET_PUT / NET_GET (netx's): BUSY / NONE while the card is shared
 *
 * Status 0 ok, 1 not shared, 2 I/O error, 3 bad request. FLAGS bit 0
 * says shared and bits 7:4 count each change, so the RP2040 tells the
 * PC the medium changed (its msc_ready()).
 */

#include <stdint.h>
#include <string.h>
#include "xlcard.h"

#ifdef XLCARD_HOST
uint32_t xl_rd(uint32_t off);
void xl_wr(uint32_t off, uint32_t v);
#else
#define XL_BASE 0x64000000u
static inline uint32_t xl_rd(uint32_t off) { return *(volatile uint32_t *)(uintptr_t)(XL_BASE + off); }
static inline void xl_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(uintptr_t)(XL_BASE + off) = v; }
#endif

#define R_MAGIC	0x0000
#define R_RX	0x0004
#define R_TX	0x0008
#define R_FLAGS	0x000C
#define R_FRAMES 0x0010
#define B_RX	0x1000
#define B_TX	0x1800
#define MAGIC	0x4B4E4C58u

#define OP_HELLO	1
#define OP_READ		2
#define OP_WRITE	3
#define OP_EJECT	4
#define OP_NET_PUT	12
#define OP_NET_GET	13

static uint32_t sectw[XLC_MAX_SECT * 128];
static uint32_t n_sectors, n_rd, n_wr;
static int shared, net, pc;

static void flags_gen(void)
{
	uint32_t g = ((xl_rd(R_FLAGS) >> 4) + 1) & 0x0F;
	xl_wr(R_FLAGS, g << 4 | (shared ? 0x01u : 0u) | 0x04u);
}

int xlc_present(void)
{
	return xl_rd(R_MAGIC) == MAGIC;
}

uint32_t xlc_frames(void)
{
	return xl_rd(R_FRAMES) & 0xFFFF;
}

void xlc_begin(uint32_t sectors, int has_net)
{
	n_sectors = sectors;
	net = has_net;
	n_rd = n_wr = 0;
	shared = 1;
	flags_gen();
}

void xlc_end(void)
{
	shared = 0;
	flags_gen();
}

int xlc_pc(void)
{
	return pc;
}

void xlc_counts(uint32_t *rd, uint32_t *wr)
{
	*rd = n_rd;
	*wr = n_wr;
}

static void reply(uint32_t op, uint32_t tag, uint32_t st, uint32_t n, uint32_t len)
{
	xl_wr(B_TX, (op | 0x80u) | tag << 8 | st << 16 | n << 24);
	xl_wr(R_TX, 0x80000000u | (len & 0x7FF));
}

int xlc_serve(void)
{
	int r = 0;
	for (;;) {
		uint32_t rx = xl_rd(R_RX), len, h, op, tag, lba = 0, count = 0, i, done = 0;
		if (!(rx & 1))
			return r;
		len = (rx >> 16) & 0x7FF;
		if (len == 0)
			len = 2048;
		h = xl_rd(B_RX);
		op = h & 0xFF;
		tag = (h >> 8) & 0xFF;
		pc = (int)((h >> 16) & 1);
		if (len >= 12) {
			lba = xl_rd(B_RX + 4);
			count = xl_rd(B_RX + 8) & 0xFF;
		}
		r |= 1;
		switch (op) {
		case OP_HELLO:
			xl_wr(B_TX + 4, (shared ? 1u : 0u) | 0x5Au << 8 | (net ? 1u : 0u) << 16);
			xl_wr(B_TX + 8, shared ? n_sectors : 0);
			reply(op, tag, 0, 0, 12);
			break;
		case OP_READ:
		case OP_WRITE:
			if (len < 12 || count < 1 || count > XLC_MAX_SECT ||
					(op == OP_WRITE && len < 12 + count * 512)) {
				reply(op, tag, 3, 0, 4);
				break;
			}
			if (!shared || lba >= n_sectors || count > n_sectors - lba) {
				reply(op, tag, shared ? 2 : 1, 0, 4);
				break;
			}
			if (op == OP_READ) {
				if (usbd_card_read(lba, (uint8_t *)sectw, count) == 0) {
					done = count;
					for (i = 0; i < count * 128; i++)
						xl_wr(B_TX + 4 + i * 4, sectw[i]);
					n_rd += count;
				}
				reply(op, tag, done == count ? 0 : 2, done, 4 + done * 512);
			} else {
				for (i = 0; i < count * 128; i++)
					sectw[i] = xl_rd(B_RX + 12 + i * 4);
				if (usbd_card_write(lba, (const uint8_t *)sectw, count) == 0) {
					done = count;
					n_wr += count;
				}
				reply(op, tag, done == count ? 0 : 2, done, 4);
			}
			break;
		case OP_EJECT:
			reply(op, tag, 0, 0, 4);
			r |= 2;
			break;
		case OP_NET_PUT:
			reply(op, tag, 5, 0, 4);	/* BUSY: the frame is dropped */
			break;
		case OP_NET_GET:
			reply(op, tag, 6, 0, 4);	/* NONE */
			break;
		default:
			reply(op, tag, 3, 0, 4);
			break;
		}
		xl_wr(R_RX, 1);
	}
}
