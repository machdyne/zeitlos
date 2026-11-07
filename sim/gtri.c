/*
 * Zeitlos simulator: T2, the triangle engine (rtl/gpu/gpu_t2.v,
 * docs/zgl_t2.md), with ZSIM_GTRI set in the environment. A frame's
 * stream is carried out by T2's C model (sw/common/zgl/tests/t2_model.c,
 * which the RTL bench is held to) at END, into chunky VRAM (or, with
 * ZSIM_CONSOLE, main memory at VBASE). It is never busy and its FIFO
 * never fills, so a program's cycles are the CPU's own: what the CPU
 * spends sending the frame (with ZSIM_COST, each a store to an
 * uncached register).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "gtri.h"
#include "../sw/common/zgl/zgl_t2.h"

static int on = -1;
static uint8_t pic[240][320];

/* ZSIM_CONSOLE: VBASE, and the RAM a LINEAR picture is in */
static uint32_t vbase = 0x20000000u;
static uint8_t *lin_ram;
static uint32_t lin_base, lin_size;

void gtri_set_linear(uint8_t *ram, uint32_t ram_base, uint32_t size) {
	lin_ram = ram; lin_base = ram_base; lin_size = size;
}

/* pixel (x, y) of the picture: chunky VRAM's segments, or LINEAR */
static void put_px(uint32_t *vram, int x, int y, uint8_t v) {
	if (lin_ram) {
		uint32_t a = vbase + (uint32_t)y * 320u + (uint32_t)x;
		if (a >= lin_base && a - lin_base < lin_size) lin_ram[a - lin_base] = v;
		return;
	}
	{
		uint32_t seg = (uint32_t)x / 80, r = (uint32_t)y + ((seg & 1) ? 240u : 0u) + ((seg & 2) ? 480u : 0u);
		uint32_t b = r * 80 + (uint32_t)x % 80;
		uint32_t *wd = &vram[b / 4];
		*wd = (*wd & ~(0xffu << (b % 4 * 8))) | (uint32_t)v << (b % 4 * 8);
	}
}

/* ZSIM_GTRI set (and not 0): T2 is here. (It was 2; 1 was the first
 * engine, gpu_tri, retired: both mean T2 now.) */
int gtri_enabled(void) {
	if (on < 0) {
		const char *e = getenv("ZSIM_GTRI");
		on = e != NULL && *e && *e != '0';
	}
	return on;
}

/* T2: the frame's words, carried out by its model at END */
#define T2_MAX (1u << 20)
static uint32_t *t2buf;
static uint32_t t2n, t2left, t2op, t2pos, t2nv;
static uint32_t t2texaddr;
static int t2bad;
static uint32_t t2strips;              /* STRIPS: written out since RESET (12-row strips) */

static void t2_frame(uint32_t *vram) {
	t2_stats_t st;
	int y, x;
	if (t2_model_run(t2buf, t2n, pic, &st) < 0) { t2bad = 1; fprintf(stderr, "gtri: T2 stream not understood (%u words)\n", t2n); return; }
	if (getenv("ZSIM_GTRI_STATS"))
		fprintf(stderr, "gtri: T2 frame: %u words, %u draws, %u vertices, %u triangles (%u from the CPU), %u lines, "
			"%u culled, %u clipped, %u drawn, %u pixels; engine cycles: geometry %u, raster %u, out %u\n",
			t2n, st.draws, st.verts, st.tris, st.cpu_tris, st.lines, st.culled, st.clipped, st.drawn, st.pixels,
			st.cyc_geom, st.cyc_raster, st.cyc_out);
	for (y = st.ry0; y <= st.ry1 && y < 240; y++)
		for (x = st.rx0; x <= st.rx1 && x < 320; x++)
			put_px(vram, x, y, pic[y][x]);
	if (st.ry1 >= st.ry0) t2strips += (uint32_t)(st.ry1 - st.ry0) / 12u + 1u;
}

static void t2_word(uint32_t *vram, uint32_t val) {
	if (!t2buf) t2buf = malloc(T2_MAX * 4);
	if (t2n >= T2_MAX) { t2bad = 1; return; }
	t2buf[t2n++] = val;
	if (t2left == 0) {                      /* a command's first word */
		t2op = T2_OP(val);
		if (t2op == T2_FRAME && t2n > 1) { t2buf[0] = val; t2n = 1; }
		switch (t2op) {
		case T2_FRAME: case T2_VIEW: t2left = 3; break;
		case T2_XFORM: t2left = 19; break;
		case T2_LIGHT: t2left = 11; break;
		case T2_STATE: case T2_END: t2left = 1; break;
		case T2_DRAW: t2left = 9; t2pos = 0; break;   /* to the chunk's header; then its body */
		case T2_TRI: t2left = 13; break;
		case T2_LINE: t2left = 9; break;
		case T2_PLIGHT: t2left = 8; break;
		default: t2bad = 1; t2left = 1; break;
		}
	} else if (t2op == T2_DRAW) {
		/* the chunk's header: nv | nt << 16, then nl in word 1's top */
		t2pos++;
		if (t2pos == 5) { t2nv = val & 0xffff; t2left += t2nv * 4 + (val >> 16) * 2; }
		if (t2pos == 6) {
			t2left += val >> 16;
			/* textured: the texture word, and s | t a vertex */
			if ((val >> 8) & T2_C_TEX) t2left += 1 + t2nv;
		}
	}
	if (--t2left == 0 && t2op == T2_END) { t2_frame(vram); t2n = 0; }
}

uint32_t gtri_read(uint32_t w) {
	switch (w) {
	case 1: return (uint32_t)t2bad << 1 | 0x3ffu << 16;   /* STATUS: never busy */
	case 3: return vbase;
	case 4: return t2strips;                             /* STRIPS */
	case 7: return T2_ID;
	case 11:                                             /* CAPS: textures (ZSIM_GTRI_NOTEX: none, as lakritz_console); VBASE taken in order (never busy) */
		return getenv("ZSIM_GTRI_NOTEX") ? 2u : 3u | (uint32_t)T2_TEX_LOG2 << 8;
	default: return 0;
	}
}

void gtri_write(uint32_t *vram, uint32_t w, uint32_t val) {
	switch (w) {
	case 3: vbase = val; break;                          /* VBASE */
	case 12: t2texaddr = val; break;                     /* TEXADDR */
	case 13: t2_model_tex(t2texaddr++, val); break;      /* TEXDATA */
	case 2: t2_word(vram, val); break;                   /* CMD */
	case 5: t2n = 0; t2left = 0; t2bad = 0; t2strips = 0; break;   /* RESET */
	default: break;
	}
}
