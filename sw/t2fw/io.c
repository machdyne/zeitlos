/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * T2's geometry unit on zeitlos32g: its way in (the stream FIFO) and
 * out (the record list and the raster), as registers
 * (rtl/gpu/gpu_t2.v, docs/zgl_t2.md).
 */
#include <stdint.h>
#include "zgl_t2.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define T2G_IN      REG(0x20000u)   /* r: the next stream word (waits for one) */
#define T2G_STATUS  REG(0x20004u)   /* r: bit 0 the raster is busy */
#define T2G_RX      REG(0x20008u)   /* w: the frame's columns x0 | x1 << 16 */
#define T2G_RY      REG(0x2000cu)   /* w: its rows y0 | y1 << 16 */
#define T2G_CLEAR   REG(0x20010u)   /* w: its background index */
#define T2G_GO      REG(0x20014u)   /* w: raster the list's first n words */
#define T2G_LIST    ((volatile uint32_t *)0x30000u)   /* w: the record list */
#define LIST_WORDS  (T2_RECS * T2_REC_WORDS)


static uint32_t wp, nrec;

volatile uint32_t *t2io_rec(int n) {
	volatile uint32_t *p;
	if (nrec >= T2_RECS || wp + (uint32_t)n > LIST_WORDS) return 0;
	p = &T2G_LIST[wp];
	wp += (uint32_t)n;
	nrec++;
	return p;
}

void t2io_frame(int32_t x0, int32_t x1, int32_t y0, int32_t y1, uint8_t clear) {
	while (T2G_STATUS & 1) ;          /* the list is the raster's until it is done */
	T2G_RX = (uint32_t)x0 | (uint32_t)x1 << 16;
	T2G_RY = (uint32_t)y0 | (uint32_t)y1 << 16;
	T2G_CLEAR = clear;
	wp = 0;
	nrec = 0;
}

void t2io_end(void) {
	T2G_GO = wp;
}

int main(void) {
	for (;;) t2fw_step();
}

/* what gcc may call for structure copies */
void *memcpy(void *d, const void *s, unsigned n) {
	uint8_t *dp = d; const uint8_t *sp = s;
	while (n--) *dp++ = *sp++;
	return d;
}
void *memset(void *d, int c, unsigned n) {
	uint8_t *dp = d;
	while (n--) *dp++ = (uint8_t)c;
	return d;
}
