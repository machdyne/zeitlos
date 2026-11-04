/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zeitlos32g, the geometry unit's core, as an instruction-set simulator:
 * RV32IM, its code and data memories and its registers (sw/t2fw/io.c,
 * rtl/gpu/zeitlos32g.v). It runs the real firmware (sw/t2fw/t2fw.bin)
 * on a frame's stream, collects the records it writes, and counts its
 * instructions and cycles (the RTL's: see t2g_cycles()).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t2g_iss.h"

#define IMEM_BYTES (12 * 1024)
#define DMEM_BASE  0x10000u
#define DMEM_BYTES (4 * 1024)
#define REG_BASE   0x20000u
#define LIST_BASE  0x30000u
#define LIST_WORDS (T2_RECS * T2_REC_WORDS)

static uint8_t imem[IMEM_BYTES], dmem[DMEM_BYTES];
static uint32_t x[32], pc;
static uint32_t list[LIST_WORDS];
uint64_t t2g_prof[IMEM_BYTES / 4];
FILE *t2g_trace;

int t2g_load(const char *path) {
	FILE *f = fopen(path, "rb");
	size_t n;
	if (!f) return -1;
	memset(imem, 0, sizeof(imem));
	n = fread(imem, 1, sizeof(imem), f);
	fclose(f);
	return n > 0 ? 0 : -1;
}

/* the cycles an instruction takes on zeitlos32g (rtl/gpu/zeitlos32g.v):
 * two stages, fetch and execute; a taken branch or a jump refetches
 * (a bubble), a load waits a cycle for the data memory, a multiply for
 * the multiplier's registers, a divide 33 for the divider (a bit a
 * clock) */
static uint32_t t2g_cycles(uint32_t insn, int taken) {
	uint32_t op = insn & 0x7f, f3 = (insn >> 12) & 7, f7 = insn >> 25;
	if (op == 0x03) return 3;                        /* load */
	if (op == 0x23) return 1;                        /* store (an I/O one: 2, and waits) */
	if (op == 0x33 && f7 == 1) return f3 < 4 ? 3 : 19;   /* mul, div */
	if (op == 0x63 || op == 0x6f || op == 0x67) return taken ? 2 : 1;
	return 1;
}

static uint32_t rd32(uint32_t a, t2g_run_t *r, int *stall) {
	*stall = 0;
	if (a + 4 <= IMEM_BYTES) { uint32_t v; memcpy(&v, &imem[a], 4); return v; }
	if (a >= DMEM_BASE && a + 4 <= DMEM_BASE + DMEM_BYTES) { uint32_t v; memcpy(&v, &dmem[a - DMEM_BASE], 4); return v; }
	if (a == REG_BASE) {                             /* IN */
		if (r->in_i >= r->in_n) { *stall = 1; return 0; }
		return r->in[r->in_i++];
	}
	if (a == REG_BASE + 4) return 0;                 /* STATUS: the raster is never busy here */
	fprintf(stderr, "t2g: read of %08x at pc %08x\n", a, pc);
	r->bad = 1;
	return 0;
}

static void wr(uint32_t a, uint32_t v, int bytes, t2g_run_t *r) {
	if (a >= DMEM_BASE && a + (uint32_t)bytes <= DMEM_BASE + DMEM_BYTES) { memcpy(&dmem[a - DMEM_BASE], &v, (size_t)bytes); return; }
	if (bytes == 4 && a >= LIST_BASE && a < LIST_BASE + LIST_WORDS * 4) { list[(a - LIST_BASE) / 4] = v; return; }
	if (bytes == 4 && a >= REG_BASE && a < REG_BASE + 0x20) {
		switch (a - REG_BASE) {
		case 0x08: r->rx = v; return;
		case 0x0c: r->ry = v; return;
		case 0x10: r->clear = v; return;
		case 0x14:                                   /* GO: the frame's records */
			r->nlist = v < LIST_WORDS ? v : LIST_WORDS;
			memcpy(r->list, list, r->nlist * 4);
			r->frames++;
			return;
		}
	}
	fprintf(stderr, "t2g: write of %08x to %08x at pc %08x\n", v, a, pc);
	r->bad = 1;
}

static int32_t sx(uint32_t v, int b) { uint32_t m = 1u << (b - 1); return (int32_t)((v ^ m) - m); }

void t2g_reset(void) {
	memset(x, 0, sizeof(x));
	memset(dmem, 0xa5, sizeof(dmem));
	pc = 0;
}

/* runs until the stream is used up and the core waits for more */
void t2g_run(t2g_run_t *r) {
	while (!r->bad) {
		uint32_t insn, op, rd, rs1, rs2, f3, f7, a, b, v = 0, npc;
		int wb = 1, taken = 0, stall;
		if (pc + 4 > IMEM_BYTES) { fprintf(stderr, "t2g: pc %08x\n", pc); r->bad = 1; return; }
		memcpy(&insn, &imem[pc], 4);
		op = insn & 0x7f; rd = (insn >> 7) & 31; f3 = (insn >> 12) & 7;
		rs1 = (insn >> 15) & 31; rs2 = (insn >> 20) & 31; f7 = insn >> 25;
		a = x[rs1]; b = x[rs2];
		npc = pc + 4;
		switch (op) {
		case 0x37: v = insn & 0xfffff000u; break;                       /* lui */
		case 0x17: v = pc + (insn & 0xfffff000u); break;                /* auipc */
		case 0x6f: v = npc; npc = pc + (uint32_t)sx(((insn >> 31) << 20) | (((insn >> 12) & 0xff) << 12) |
		           (((insn >> 20) & 1) << 11) | (((insn >> 21) & 0x3ff) << 1), 21); taken = 1; break;
		case 0x67: v = npc; npc = (a + (uint32_t)sx(insn >> 20, 12)) & ~1u; taken = 1; break;
		case 0x63: {
			int32_t off = sx(((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) | (((insn >> 25) & 0x3f) << 5) |
			                 (((insn >> 8) & 0xf) << 1), 13);
			switch (f3) {
			case 0: taken = a == b; break;
			case 1: taken = a != b; break;
			case 4: taken = (int32_t)a < (int32_t)b; break;
			case 5: taken = (int32_t)a >= (int32_t)b; break;
			case 6: taken = a < b; break;
			case 7: taken = a >= b; break;
			default: r->bad = 1; return;
			}
			if (taken) npc = pc + (uint32_t)off;
			wb = 0;
			break;
		}
		case 0x03: {
			uint32_t ad = a + (uint32_t)sx(insn >> 20, 12), w = rd32(ad & ~3u, r, &stall);
			if (stall) return;                                         /* waiting for the stream */
			w >>= (ad & 3) * 8;
			switch (f3) {
			case 0: v = (uint32_t)sx(w & 0xff, 8); break;
			case 1: v = (uint32_t)sx(w & 0xffff, 16); break;
			case 2: v = w; break;
			case 4: v = w & 0xff; break;
			case 5: v = w & 0xffff; break;
			default: r->bad = 1; return;
			}
			break;
		}
		case 0x23: {
			uint32_t ad = a + (uint32_t)sx(((insn >> 25) << 5) | ((insn >> 7) & 31), 12);
			if (f3 == 2) wr(ad, b, 4, r);
			else if (f3 == 1) wr(ad, b & 0xffff, 2, r);
			else wr(ad, b & 0xff, 1, r);
			wb = 0;
			break;
		}
		case 0x13: {
			uint32_t im = (uint32_t)sx(insn >> 20, 12), sh = rs2;
			switch (f3) {
			case 0: v = a + im; break;
			case 2: v = (int32_t)a < (int32_t)im; break;
			case 3: v = a < im; break;
			case 4: v = a ^ im; break;
			case 6: v = a | im; break;
			case 7: v = a & im; break;
			case 1: v = a << sh; break;
			case 5: v = (f7 & 0x20) ? (uint32_t)((int32_t)a >> sh) : a >> sh; break;
			}
			break;
		}
		case 0x33:
			if (f7 == 1) {
				int64_t sa = (int32_t)a, sb = (int32_t)b;
				switch (f3) {
				case 0: v = a * b; break;
				case 1: v = (uint32_t)((sa * sb) >> 32); break;
				case 2: v = (uint32_t)((sa * (int64_t)(uint64_t)b) >> 32); break;
				case 3: v = (uint32_t)(((uint64_t)a * b) >> 32); break;
				case 4: v = b == 0 ? 0xffffffffu : ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
				case 5: v = b == 0 ? 0xffffffffu : a / b; break;
				case 6: v = b == 0 ? a : ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
				case 7: v = b == 0 ? a : a % b; break;
				}
			} else switch (f3) {
				case 0: v = (f7 & 0x20) ? a - b : a + b; break;
				case 1: v = a << (b & 31); break;
				case 2: v = (int32_t)a < (int32_t)b; break;
				case 3: v = a < b; break;
				case 4: v = a ^ b; break;
				case 5: v = (f7 & 0x20) ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
				case 6: v = a | b; break;
				case 7: v = a & b; break;
			}
			break;
		default:
			fprintf(stderr, "t2g: instruction %08x at %08x\n", insn, pc);
			r->bad = 1;
			return;
		}
		if (wb && rd) {
			x[rd] = v;
			if (t2g_trace) fprintf(t2g_trace, "%08x %2u %08x\n", pc, rd, v);
		}
		r->insns++;
		r->cycles += t2g_cycles(insn, taken);
		t2g_prof[pc / 4] += t2g_cycles(insn, taken);
		pc = npc;
	}
}
