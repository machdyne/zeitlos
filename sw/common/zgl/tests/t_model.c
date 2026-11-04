/*
 * A model of the triangle engine (rtl/gpu/gpu_tri.v) in C: the
 * commands zglf_t_frame() makes, carried out as the engine carries them
 * out, into a 320x240 picture of indices. The tests hold it to the
 * software strips (test_frames) and the RTL to it (tb_gpu_tri.v, by
 * way of t_vectors). docs/gpu_tri.md.
 */
#include <stdint.h>
#include <string.h>
#include "t_model.h"

static const uint8_t bayer4[4][4] = {
	{   8, 136,  40, 168 },
	{ 200,  72, 232, 104 },
	{  56, 184,  24, 152 },
	{ 248, 120, 216,  88 }
};

typedef struct { int32_t q, r, qs, rs, D; } edge_t;

static void edge_get(const uint32_t *w, edge_t *e) {
	e->q = (int16_t)(w[0] & 0xffff);
	e->qs = (int16_t)(w[0] >> 16);
	e->r = (int32_t)(w[1] & 0xffff);
	e->rs = (int32_t)(w[1] >> 16);
	e->D = (int32_t)(w[2] & 0xffff);
}

static void edge_step(edge_t *e) {
	e->q += e->qs;
	e->r -= e->rs;
	if (e->r < 0) { e->r += e->D; e->q++; }
}

int t_model_run(const uint32_t *cmd, int n, uint8_t pic[240][320]) {
	static uint8_t col[16 * 320];
	static uint16_t zb[16 * 320];
	int32_t y0 = 0, rows = 16, cx0 = 0, cx1 = 319;
	int i = 0, k, strips = 0;
	uint8_t clear = 0;
	while (i < n) {
		uint32_t h = cmd[i];
		uint32_t op = h >> 28, depth = (h >> 27) & 1;
		if (op == 3) {                                   /* BEGIN */
			y0 = (int32_t)((h >> 12) & 0xff);
			rows = (int32_t)(h & 15) + 1;
			clear = (uint8_t)((h >> 4) & 0xff);
			memset(col, clear, sizeof(col));
			memset(zb, 0xff, sizeof(zb));
			cx0 = (int32_t)(cmd[i + 1] & 0xffff);
			cx1 = (int32_t)(cmd[i + 1] >> 16);
			i += 2;
		} else if (op == 4) {                            /* END */
			for (k = 0; k < rows; k++)
				if (y0 + k < 240) memcpy(pic[y0 + k], &col[k * 320], 320);
			strips++;
			i += 1;
		} else if (op == 1) {                            /* TRAP */
			edge_t L, R;
			int32_t ya = (int32_t)((h >> 4) & 15), yb = (int32_t)(h & 15), y, x;
			uint32_t zrow = cmd[i + 7], dzdx = cmd[i + 8], dzdy = cmd[i + 9];
			uint32_t crow = cmd[i + 10], dcdx = cmd[i + 11], dcdy = cmd[i + 12];
			edge_get(&cmd[i + 1], &L);
			edge_get(&cmd[i + 4], &R);
			for (y = ya; y <= yb; y++) {
				int32_t x0 = L.q, x1 = R.q - 1;
				edge_step(&L);
				edge_step(&R);
				if (x0 < cx0) x0 = cx0;
				if (x1 > cx1) x1 = cx1;
				if (x0 <= x1) {
					uint32_t z = zrow + dzdx * (uint32_t)x0, c = crow + dcdx * (uint32_t)x0;
					for (x = x0; x <= x1; x++) {
						int o = y * 320 + x;
						uint8_t ci = (uint8_t)((c + bayer4[(y0 + y) & 3][x & 3]) >> 8);
						if (depth) {
							uint32_t zv = (int32_t)z < 0 ? 0 : z >> 15;
							if (zv < zb[o]) { zb[o] = (uint16_t)zv; col[o] = ci; }
						} else col[o] = ci;
						z += dzdx;
						c += dcdx;
					}
				}
				zrow += dzdy;
				crow += dcdy;
			}
			i += 13;
		} else if (op == 2) {                            /* LINE */
			uint32_t steps = (h & 0xffff) + 1, s;
			uint32_t x = cmd[i + 1], y = cmd[i + 2], sx = cmd[i + 3], sy = cmd[i + 4];
			uint32_t z = cmd[i + 5], sz = cmd[i + 6], c = cmd[i + 7], sc = cmd[i + 8];
			for (s = 0; s < steps; s++) {
				int32_t px = (int32_t)x >> 20, py = ((int32_t)y >> 20) - y0;
				if (py >= 0 && py < rows && px >= cx0 && px <= cx1) {
					int o = py * 320 + px;
					uint32_t zv = (int32_t)z < 0 ? 0 : z >> 15;
					if (!depth || zv <= zb[o]) {
						if (depth) zb[o] = (uint16_t)zv;
						col[o] = (uint8_t)((c + 128) >> 8);
					}
				}
				x += sx; y += sy; z += sz; c += sc;
			}
			i += 9;
		} else return -1;                                /* not a command */
	}
	return strips;
}
