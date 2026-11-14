/*
 * zsys's T2, the triangle engine (rtl/gpu/gpu_t2.v, docs/zgl_t2.md):
 * its registers at 0x7C00_0000, and a frame's stream carried out at END
 * by T2's own model -- the geometry unit's program (sw/common/zgl/
 * t2fw.c, what zeitlos32g runs) and the raster's model
 * (sw/common/zgl/tests/t2_raster.c), the two the RTL bench is held to,
 * word for word -- into chunky VRAM. As sim/gtri.c does for sim/'s
 * single-app emulator.
 *
 * Like the blitter, it does a frame's work between two instructions:
 * never busy, its FIFO never full, and what the CPU spends is only
 * sending the stream. On the board the engine is a frame's work behind
 * the CPU; here it is no time at all, so a 3D game is paced by the CPU
 * alone.
 *
 * T2 is in sergei_ml2's build (with -DGPU_T2) and the console profiles;
 * zsys has it with its 45F video unless --no-t2.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zsys.h"
#include "../../sw/common/zgl/zgl_t2.h"

// sw/common/zgl/tests/t2_model.c
#include "../../sw/common/zgl/tests/t2_raster.h"
int t2_model_run(const uint32_t *w, uint32_t n, uint8_t pic[240][320], t2_stats_t *st);
void t2_model_tex(uint32_t addr, uint32_t four_texels);

static uint8_t pic[240][320];

#define T2_MAX (1u << 20)
static uint32_t *t2buf;
static uint32_t t2n, t2left, t2op, t2pos, t2nv, t2texaddr, t2strips, t2vbase = 0x20000000u;
static int t2bad;

// pixel (x, y) of chunky VRAM: four 80-pixel segments a line
static void put_px(zsys_t *s, int x, int y, uint8_t v) {
	uint32_t seg = (uint32_t)x / 80, r = (uint32_t)y + ((seg & 1) ? 240u : 0u) + ((seg & 2) ? 480u : 0u);
	uint32_t b = (r * ZSYS_STRIDE_640 + (uint32_t)x % 80) & (s->vram_size - 1);
	s->vram[b] = v;
}

static void t2_frame(zsys_t *s) {
	t2_stats_t st;
	if (t2_model_run(t2buf, t2n, pic, &st) < 0) {
		t2bad = 1;
		fprintf(stderr, "zsys: T2: a stream not understood (%u words)\n", t2n);
		return;
	}
	for (int y = st.ry0; y <= st.ry1 && y < 240; y++)
		for (int x = st.rx0; x <= st.rx1 && x < 320; x++)
			put_px(s, x, y, pic[y][x]);
	if (st.ry1 >= st.ry0) t2strips += (uint32_t)(st.ry1 - st.ry0) / 12u + 1u;
	s->t2_frames++;
}

// A command word: kept until the frame's END, the command lengths
// followed so that a word that happens to look like END is not one.
static void t2_word(zsys_t *s, uint32_t val) {
	if (!t2buf && !(t2buf = malloc(T2_MAX * 4))) { t2bad = 1; return; }
	if (t2n >= T2_MAX) { t2bad = 1; return; }
	t2buf[t2n++] = val;
	if (t2left == 0) {
		t2op = T2_OP(val);
		if (t2op == T2_FRAME && t2n > 1) { t2buf[0] = val; t2n = 1; }
		switch (t2op) {
		case T2_FRAME: case T2_VIEW: t2left = 3; break;
		case T2_XFORM: t2left = 19; break;
		case T2_LIGHT: t2left = 11; break;
		case T2_STATE: case T2_END: t2left = 1; break;
		case T2_DRAW: t2left = 9; t2pos = 0; break;
		case T2_TRI: t2left = 13; break;
		case T2_LINE: t2left = 9; break;
		case T2_PLIGHT: t2left = 8; break;
		default: t2bad = 1; t2left = 1; break;
		}
	} else if (t2op == T2_DRAW) {
		t2pos++;
		if (t2pos == 5) { t2nv = val & 0xffff; t2left += t2nv * 4 + (val >> 16) * 2; }
		if (t2pos == 6) {
			t2left += val >> 16;
			if ((val >> 8) & T2_C_TEX) t2left += 1 + t2nv;
		}
	}
	if (--t2left == 0 && t2op == T2_END) { t2_frame(s); t2n = 0; }
}

uint32_t zt2_read(zsys_t *s, uint32_t w) {
	(void)s;
	switch (w) {
	case 1: return (uint32_t)t2bad << 1 | 0x3ffu << 16;	// STATUS: never busy, the FIFO free
	case 3: return t2vbase;
	case 4: return t2strips;				// STRIPS
	case 7: return T2_ID;					// "ZTR2"
	case 11: return 3u | (uint32_t)T2_TEX_LOG2 << 8;	// CAPS: textures, VBASE in order
	}
	return 0;
}

void zt2_write(zsys_t *s, uint32_t w, uint32_t val) {
	switch (w) {
	case 2: t2_word(s, val); return;			// CMD
	case 3: t2vbase = val; return;				// VBASE (chunky VRAM: where it always is)
	case 5: t2n = t2left = 0; t2bad = 0; t2strips = 0; return;	// RESET
	case 12: t2texaddr = val; return;			// TEXADDR
	case 13: t2_model_tex(t2texaddr++, val); return;	// TEXDATA
	}
}

void zt2_reset(void) {
	t2n = t2left = 0;
	t2bad = 0;
	t2strips = 0;
	t2vbase = 0x20000000u;
}
