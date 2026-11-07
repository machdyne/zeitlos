/*
 * Zeitlos simulator: a rough cycle model (ZSIM_COST), for tuning code
 * whose cost is in memory, not in instructions. sim/README.md,
 * "Cycle model".
 *
 * What it models, as rtl/cache_split.v and a DDR3 or SDRAM board have
 * it (numbers from Doom's -membench on sergei_ml2, docs/doom_app.md):
 *
 *   - an instruction: 1 cycle, plus 2 for a taken branch or jump
 *     (zeitlos32p's BTB catches most loops; this is the average);
 *   - the instruction cache: 8 KB, direct mapped, 16-byte lines; a miss
 *     waits for posted stores, then the fill;
 *   - the data cache: 4 KB, direct mapped, 16-byte lines, loads only
 *     (write-through, no write-allocate); a miss as above;
 *   - stores: posted into a buffer of two, each taking the memory for
 *     its time; a store into a full buffer waits;
 *   - VRAM and registers: uncached, after the buffer drains.
 *
 * ZSIM_COST=ddr3 (or 1): fill 40, store 20, uncached 10.
 * ZSIM_COST=sdram: fill 20, store 6, uncached 8.
 * ZSIM_COST=lakritz: lakritz_console -- picorv32 (4 cycles an
 *   instruction, 1 more for a taken branch), a 4 KB instruction cache,
 *   NO data cache: every load waits 10 for SDRAM, every store 8 (not
 *   posted); fill 20.
 * ZSIM_COST=lakritz_dc: the same with a 4 KB data cache (as the model's,
 *   write-through, posted stores of 6).
 * rdcycle then reads the model's cycles, so a program's own timing
 * (gldemo's breakdown, zcycles.h) reads them too.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cost.h"

int cost_on = -1;
static uint64_t t;                     /* cycles */
static uint64_t wb[2];                 /* when each store buffer slot frees */
static uint64_t wb_last;               /* when the last posted store completes */
static uint32_t fill = 40, st = 20, unc = 10;
static uint32_t insn = 1, taken_x = 2, imask = 511;
static int dc_on = 1, st_wait = 0;          /* stores: posted (0) or waited on */
static uint32_t itag[512], dtag[256];  /* 8 KB and 4 KB of 16-byte lines; tag+1, 0 empty */
static uint64_t n_imiss, n_dmiss, n_store, n_stall, n_unc;

static void cost_init(void) {
	const char *e = getenv("ZSIM_COST");
	cost_on = e != NULL && *e;
	if (!cost_on) return;
	if (!strcmp(e, "sdram")) { fill = 20; st = 6; unc = 8; }
	if (!strncmp(e, "lakritz", 7)) {
		insn = 4; taken_x = 1; imask = 255; fill = 20; unc = 10;
		if (!strcmp(e, "lakritz_dc")) st = 6;
		else { dc_on = 0; st = 8; st_wait = 1; }
	}
	memset(itag, 0, sizeof(itag));
	memset(dtag, 0, sizeof(dtag));
	atexit(cost_report);
}

int cost_enabled(void) {
	if (cost_on < 0) cost_init();
	return cost_on;
}

static int cached(uint32_t a) { return a >= 0x80000000u; }   /* the app's RAM */

static void drain(void) { if (t < wb_last) t = wb_last; }

void cost_insn(int taken) { t += insn + (taken ? taken_x : 0); }

void cost_fetch(uint32_t pc) {
	uint32_t line = pc >> 4, i = line & imask;
	if (!cached(pc)) { drain(); t += unc; return; }
	if (itag[i] != line + 1) { itag[i] = line + 1; drain(); t += fill; n_imiss++; }
}

void cost_load(uint32_t a) {
	uint32_t line = a >> 4, i = line & 255;
	if (!cached(a) || !dc_on) { drain(); t += unc; n_unc++; return; }
	if (dtag[i] != line + 1) { dtag[i] = line + 1; drain(); t += fill; n_dmiss++; }
}

void cost_store(uint32_t a) {
	uint64_t start;
	int k;
	if (!cached(a)) { drain(); t += unc; n_unc++; return; }
	n_store++;
	if (st_wait) { t += st; return; }          /* not posted: picorv32 waits for the ack */
	k = wb[0] <= wb[1] ? 0 : 1;            /* the slot that frees first */
	if (wb[k] > t) { n_stall += wb[k] - t; t = wb[k]; }
	start = t > wb_last ? t : wb_last;
	wb[k] = wb_last = start + st;
}

uint64_t cost_cycles(void) { return t; }

void cost_report(void) {
	fprintf(stderr, "zeitlos-sim: cycle model: %llu cycles; %llu I misses, %llu D misses, "
		"%llu stores (%llu cycles waiting for the buffer), %llu uncached\n",
		(unsigned long long)t, (unsigned long long)n_imiss, (unsigned long long)n_dmiss,
		(unsigned long long)n_store, (unsigned long long)n_stall, (unsigned long long)n_unc);
}
