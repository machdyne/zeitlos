/*
 * Zeitlos simulator: the triangle engine (rtl/gpu/gpu_tri.v,
 * docs/gpu_tri.md), with ZSIM_GTRI set in the environment. Its
 * commands are carried out by the engine's C model
 * (sw/common/zgl/tests/t_model.c, which the RTL bench is held to), a
 * strip at a time at END, into chunky VRAM. It is never busy and its
 * FIFO never fills, so a program's cycles are the CPU's own: what the
 * CPU spends making commands (with ZSIM_COST, each a store to an
 * uncached register). MONO (the black-and-white desktop) is not
 * modelled: such frames are not drawn.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "gtri.h"
#include "../sw/common/zgl/tests/t_model.h"
#include "../sw/common/zgl/zgl_t2.h"

#define GT_ID      0x5A545249u              /* "ZTRI" */
#define GT_MAX     65536

static int on = -1;
static uint32_t buf[GT_MAX];
static int n, bad, left;
static uint32_t op;                 /* left: words still to come of a command */
static uint32_t ctrl;
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

/* ZSIM_GTRI=1: today's engine; 2: T2, the engine that runs the frame */
int gtri_enabled(void) {
	if (on < 0) {
		const char *e = getenv("ZSIM_GTRI");
		on = e != NULL && *e && *e != '0';
		if (on && *e == '2') on = 2;
	}
	return on;
}

/* T2: the frame's words, carried out by its model at END */
#define T2_MAX (1u << 20)
static uint32_t *t2buf;
static uint32_t t2n, t2left, t2op, t2pos;
static int t2bad;

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
		default: t2bad = 1; t2left = 1; break;
		}
	} else if (t2op == T2_DRAW) {
		/* the chunk's header: nv | nt << 16, then nl in word 1's top */
		t2pos++;
		if (t2pos == 5) t2left += (val & 0xffff) * 4 + (val >> 16) * 2;
		if (t2pos == 6) t2left += val >> 16;
	}
	if (--t2left == 0 && t2op == T2_END) { t2_frame(vram); t2n = 0; }
}

uint32_t gtri_read(uint32_t w) {
	switch (w) {
	case 1: return (uint32_t)(bad | t2bad) << 1 | 0x3ffu << 16;    /* STATUS: never busy */
	case 3: return vbase;
	case 7: return on == 2 ? T2_ID : GT_ID;
	default: return 0;
	}
}

/* a strip (BEGIN .. END in buf) to chunky VRAM: rows of 4 segments of
 * 80 pixels, z_chunky_seg()'s layout (sw/common/zsoc.h) */
static void strip(uint32_t *vram) {
	uint32_t h = buf[0];
	int y0 = (int)((h >> 12) & 0xff), rows = (int)(h & 15) + 1;
	int cx0 = (int)(buf[1] & 0xffff), cx1 = (int)(buf[1] >> 16), y, x;
	if (t_model_run(buf, n, pic) != 1) { bad = 1; fprintf(stderr, "gtri: bad strip %08x n %d\n", buf[0], n); return; }
	if (ctrl & 4) return;                                /* MONO: not modelled */
	for (y = y0; y < y0 + rows && y < 240; y++)
		for (x = cx0; x <= cx1 && x < 320; x++)
			put_px(vram, x, y, pic[y][x]);
}

void gtri_write(uint32_t *vram, uint32_t w, uint32_t val) {
	switch (w) {
	case 0: ctrl = val; break;
	case 3: vbase = val; break;                          /* VBASE */
	case 2:                                              /* CMD */
		if (on == 2) { t2_word(vram, val); break; }
		if (!left) {                                     /* a command's first word */
			static const int len[8] = { 0, 13, 9, 2, 1, 0, 0, 0 };
			op = val >> 28;
			if (op == 3) n = 0;                          /* BEGIN */
			left = op < 8 ? len[op] : 0;
			if (!left) { bad = 1; break; }
		}
		if (n < GT_MAX) buf[n++] = val; else bad = 1;
		if (--left == 0 && op == 4) { strip(vram); n = 0; }   /* END */
		break;
	case 5: n = 0; bad = 0; ctrl = 0; left = 0; t2n = 0; t2left = 0; t2bad = 0; break;             /* RESET */
	default: break;
	}
}
