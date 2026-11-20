// zv.c -- ZV decoder: the base layer into every view (zv.h, docs/zv.md).
//
// Plain C: no allocation, no I/O, no floating point, no libm, so it
// builds unchanged for the board, the host encoder and the Linux
// player. The 64-bit arithmetic is at set-up only.
//
// How it is fast enough on the board: a codebook entry is drawn for the
// view once, when it arrives (r1/r4), already dithered for each place a
// block can sit in the 8x8 mask. Drawing a block is then copying bytes:
// a V1 block of the full mono view is eight byte stores.

#include "zv.h"
#include "zv_masks.h"

#include <string.h>

// ---- little-endian access ------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ---- header ---------------------------------------------------------

int zv_parse_header(const uint8_t *b, size_t n, zv_header_t *h) {
	if (n < ZV_HDR_SIZE) return -1;
	if (b[0] != ZV_MAGIC0 || b[1] != ZV_MAGIC1 || b[2] != ZV_MAGIC2 || b[3] != ZV_MAGIC3) return -2;
	if (rd16(b + 4) < ZV_HDR_SIZE) return -3;
	memset(h, 0, sizeof(*h));
	h->flags = rd16(b + 6);
	h->w = rd16(b + 8);
	h->h = rd16(b + 10);
	h->fps_num = rd32(b + 12);
	h->fps_den = rd32(b + 16);
	h->frames = rd32(b + 20);
	h->arate = rd32(b + 24);
	h->achan = b[28];
	h->afmt = b[29];
	h->index_off = rd32(b + 32);
	h->index_n = rd32(b + 36);
	h->colour_off = rd32(b + 40);
	h->max_rec = rd32(b + 44);
	h->video_bytes = rd32(b + 48);
	h->audio_bytes = rd32(b + 52);
	h->refresh = rd16(b + 56);
	if (h->w == 0 || h->h == 0 || (h->w & 7) || (h->h & 7)) return -4;
	if (h->w > ZV_MAX_W || h->h > ZV_MAX_H) return -5;
	if (h->fps_num == 0 || h->fps_den == 0) return -6;
	if (h->flags & ZV_F_AUDIO) {
		if (h->arate == 0 || (h->achan != 1 && h->achan != 2) || h->afmt != ZV_A_PCM8) return -7;
	}
	return 0;
}

void zv_write_header(uint8_t *b, const zv_header_t *h) {
	memset(b, 0, ZV_HDR_SIZE);
	b[0] = ZV_MAGIC0; b[1] = ZV_MAGIC1; b[2] = ZV_MAGIC2; b[3] = ZV_MAGIC3;
	wr16(b + 4, ZV_HDR_SIZE);
	wr16(b + 6, h->flags);
	wr16(b + 8, h->w);
	wr16(b + 10, h->h);
	wr32(b + 12, h->fps_num);
	wr32(b + 16, h->fps_den);
	wr32(b + 20, h->frames);
	wr32(b + 24, h->arate);
	b[28] = h->achan;
	b[29] = h->afmt;
	wr32(b + 32, h->index_off);
	wr32(b + 36, h->index_n);
	wr32(b + 40, h->colour_off);
	wr32(b + 44, h->max_rec);
	wr32(b + 48, h->video_bytes);
	wr32(b + 52, h->audio_bytes);
	wr16(b + 56, h->refresh);
}

uint32_t zv_start_for(const zv_header_t *h, uint32_t n) {
	return n > h->refresh ? n - h->refresh : 0;
}

uint32_t zv_audio_before(const zv_header_t *h, uint32_t n) {
	return (uint32_t)(((uint64_t)n * h->arate * h->fps_den) / h->fps_num);
}

uint32_t zv_frame_vsync(const zv_header_t *h, uint32_t n, uint32_t hz) {
	uint64_t a = (uint64_t)n * hz * h->fps_den * 2 + h->fps_num;
	return (uint32_t)(a / ((uint64_t)h->fps_num * 2));
}

int zv_split_record(const zv_header_t *h, const uint8_t *r, size_t n, zv_rec_t *o) {
	uint32_t size, ab;
	size_t vo;
	if (n < ZV_REC_HDR) return -1;
	size = rd32(r);
	if (size < ZV_REC_HDR || size > n) return -2;
	o->kind = r[4];
	o->flags = r[5];
	o->n_audio = rd16(r + 6);
	ab = o->n_audio * (h->achan ? h->achan : 1);
	if (!(h->flags & ZV_F_AUDIO)) ab = 0;
	vo = (ZV_REC_HDR + ab + 3) & ~(size_t)3;
	if (vo > size) return -3;
	o->audio = r + ZV_REC_HDR;
	o->video = r + vo;
	o->video_len = size - vo;
	return 0;
}

// ---- fixed-point powers (set-up only) --------------------------------

// log2(x) in Q16, x > 0
static int32_t log2_q16(uint32_t x) {
	int n = 31;
	uint64_t z;
	int32_t y;
	int i;
	while (!(x & 0x80000000u)) { x <<= 1; n--; }
	y = n << 16;
	z = (uint64_t)x >> 1;                   // Q30 in [1, 2)
	for (i = 15; i >= 0; i--) {
		z = (z * z) >> 30;
		if (z >= ((uint64_t)2 << 30)) { z >>= 1; y += 1 << i; }
	}
	return y;
}

// 2^(y/65536) for y <= 0, in Q16 (0..65536)
static uint32_t exp2_q16(int32_t y) {
	// 2^(-2^-k) in Q30, k = 1..16
	static const uint32_t c[16] = {
		759250125u, 902905651u, 984625594u, 1028218693u, 1050733751u,
		1062175491u, 1067942999u, 1070838486u, 1072289173u, 1073015252u,
		1073378477u, 1073560135u, 1073650976u, 1073696399u, 1073719111u,
		1073730468u,
	};
	uint32_t ip, fp;
	uint64_t r = (uint64_t)1 << 30;
	int k;
	if (y >= 0) return 65536;
	y = -y;
	ip = (uint32_t)y >> 16;
	fp = (uint32_t)y & 0xffff;
	if (ip >= 31) return 0;
	for (k = 0; k < 16; k++)
		if (fp & (0x8000u >> k)) r = (r * c[k]) >> 30;
	return (uint32_t)((r >> ip) >> 14);
}

// 4096 * (num/den)^(g100/100), num <= den
static uint32_t pow_frac(uint32_t num, uint32_t den, int g100) {
	int32_t l;
	if (num == 0) return 0;
	if (num >= den) return 4096;
	l = log2_q16(num) - log2_q16(den);     // < 0
	l = (int32_t)(((int64_t)l * g100) / 100);
	return (exp2_q16(l) + 8) >> 4;
}

uint16_t zv_lin12(uint16_t c, int g100) {
	uint32_t r = pow_frac((c >> 8) & 15, 15, g100);
	uint32_t g = pow_frac((c >> 4) & 15, 15, g100);
	uint32_t b = pow_frac(c & 15, 15, g100);
	return (uint16_t)((2126 * r + 7152 * g + 722 * b + 5000) / 10000);
}

int zv_grey_palette(uint16_t *pal, int max, int kind) {
	// candidates: channels at most one step apart (true greys only for
	// ZV_GREYS_TRUE), by brightness
	static uint16_t cand[16 * 16 * 16];
	static uint16_t cl[16 * 16 * 16];
	int nc = 0, i, j, k, n = 0;
	if (max < 2) max = 2;
	if (max > ZV_PAL_MAX) max = ZV_PAL_MAX;
	if (kind == ZV_GREYS_TRUE) {
		for (i = 0; i < 16 && i < max; i++) pal[i] = (uint16_t)(i * 0x111);
		return i;
	}
	for (i = 0; i < 4096; i++) {
		int r = (i >> 8) & 15, g = (i >> 4) & 15, b = i & 15;
		int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
		int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
		uint16_t l;
		if (hi - lo > 1) continue;
		l = zv_lin12((uint16_t)i, 220);
		// insertion by brightness; on a tie keep the more neutral
		for (j = nc; j > 0 && cl[j - 1] > l; j--) { cand[j] = cand[j - 1]; cl[j] = cl[j - 1]; }
		cand[j] = (uint16_t)i; cl[j] = l;
		nc++;
	}
	// pick `max`, evenly spaced in gamma-encoded brightness (what the eye
	// sees as even steps); black and white are the ends
	for (k = 0; k < max; k++) {
		// target in encoded space 0..4096, back to linear
		uint32_t t_enc = (uint32_t)(4096 * k / (max - 1));
		uint32_t t = pow_frac(t_enc, 4096, 220);
		int best = -1;
		uint32_t bd = 0xffffffffu, last = n ? zv_lin12(pal[n - 1], 220) : 0;
		for (j = 0; j < nc; j++) {
			uint32_t d = cl[j] > t ? cl[j] - t : t - cl[j];
			if (n && cl[j] <= last) continue;
			if (d < bd) { bd = d; best = j; }
		}
		if (best < 0) break;
		pal[n++] = cand[best];
	}
	return n;
}

// ---- views -----------------------------------------------------------

static const uint8_t *mask_of(int m) {
	return m == ZV_MASK_VAC8 ? zv_mask_vac8 : zv_mask_bayer8;
}

// white when the light is above the rank's threshold
#define THR(rank) ((uint32_t)(2 * (rank) + 1) * 32)

static uint8_t pal_px(const zv_dec_t *d, uint8_t g, uint8_t rank) {
	return (uint8_t)(d->pal_lo[g] + (d->pal_frac[g] > rank));
}

// white at mask place (x, y) for grey g
#define PAT(d, g, x, y) (((d)->pat[g][y] >> (x)) & 1)
// the same for the four planes at once, plane k in byte k (the plane
// views: the mono views' arithmetic, a byte a plane)
#define PATW(d, g, x, y) (((d)->pat4[g][y] >> (x)) & 0x01010101u)
#define B4(m) ((uint32_t)(m) * 0x01010101u)

static uint8_t avg4(const uint8_t *e) { return (uint8_t)((e[0] + e[1] + e[2] + e[3] + 2) >> 2); }

// the 2x2 average of a V4 entry, as the half views see it
static void half2(const uint8_t *e, uint8_t a[4]) {
	int xx, yy;
	for (yy = 0; yy < 2; yy++)
		for (xx = 0; xx < 2; xx++) {
			const uint8_t *p = e + yy * 8 + xx * 2;
			a[yy * 2 + xx] = (uint8_t)((p[0] + p[1] + p[4] + p[5] + 2) >> 2);
		}
}

// -- V1: a 4x4 entry. Full mono: doubled to 8x8, one variant, 8 row
// bytes. Half views: 4x4 as it is, at one of 4 places (ph = py:px) in
// the mask: mono 4 row nibbles at r[ph*4], palette 16 bytes at r[ph*16].
static void draw_v1(zv_dec_t *d, int i, int ph) {
	const uint8_t *e = d->cb1[i];
	uint8_t *r = (uint8_t *)d->r1[i];
	int x, y, px = (ph & 1) * 4, py = (ph >> 1) * 4;
	switch (d->view) {
	case ZV_VIEW_MONO_FULL:
		for (y = 0; y < 8; y++) {
			const uint8_t *c = e + (y >> 1) * 4;
			r[y] = (uint8_t)((d->pat[c[0]][y] & 0x03) | (d->pat[c[1]][y] & 0x0c) |
			                 (d->pat[c[2]][y] & 0x30) | (d->pat[c[3]][y] & 0xc0));
		}
		break;
	case ZV_VIEW_MONO_HALF:
		for (y = 0; y < 4; y++) {
			const uint8_t *c = e + y * 4;
			int row = py + y;
			r[ph * 4 + y] = (uint8_t)(PAT(d, c[0], px, row) | (PAT(d, c[1], px + 1, row) << 1) |
			                          (PAT(d, c[2], px + 2, row) << 2) | (PAT(d, c[3], px + 3, row) << 3));
		}
		break;
	case ZV_VIEW_PL_FULL:
		for (y = 0; y < 8; y++) {
			const uint8_t *c = e + (y >> 1) * 4;
			d->r1[i][y] = (d->pat4[c[0]][y] & B4(0x03)) | (d->pat4[c[1]][y] & B4(0x0c)) |
			              (d->pat4[c[2]][y] & B4(0x30)) | (d->pat4[c[3]][y] & B4(0xc0));
		}
		break;
	case ZV_VIEW_PL_HALF:
		for (y = 0; y < 4; y++) {
			const uint8_t *c = e + y * 4;
			int row = py + y;
			d->r1[i][ph * 4 + y] = PATW(d, c[0], px, row) | (PATW(d, c[1], px + 1, row) << 1) |
			                       (PATW(d, c[2], px + 2, row) << 2) | (PATW(d, c[3], px + 3, row) << 3);
		}
		break;
	case ZV_VIEW_PAL_HALF: {
		const uint8_t *m = mask_of(d->mask);
		for (y = 0; y < 4; y++)
			for (x = 0; x < 4; x++)
				r[ph * 16 + y * 4 + x] = pal_px(d, e[y * 4 + x], m[(py + y) * 8 + px + x]);
		break;
	}
	case ZV_VIEW_PAL_FULL: {
		// doubled to 8x8: one variant, 64 bytes
		const uint8_t *m = mask_of(d->mask);
		for (y = 0; y < 8; y++)
			for (x = 0; x < 8; x++)
				r[y * 8 + x] = pal_px(d, e[(y >> 1) * 4 + (x >> 1)], m[y * 8 + x]);
		break;
	}
	default:
		break;
	}
}

// -- V4: a 4x4 entry at full size. Full mono: one of 4 quadrants q of the
// 8x8 mask, 4 row nibbles at r[q*4]. Half views: its 2x2 average at one
// of 16 places v = py:px:qy:qx (the block's place, then the quadrant's):
// mono 2 row bit-pairs at r[v*2], palette 4 bytes at r[v*4].
static void draw_v4(zv_dec_t *d, int i, int v) {
	const uint8_t *e = d->cb4[i];
	uint8_t *r = (uint8_t *)d->r4[i];
	int x, y;
	switch (d->view) {
	case ZV_VIEW_MONO_FULL: {
		int qx = (v & 1) * 4, qy = (v >> 1) * 4;
		for (y = 0; y < 4; y++) {
			const uint8_t *c = e + y * 4;
			int row = qy + y;
			r[v * 4 + y] = (uint8_t)(PAT(d, c[0], qx, row) | (PAT(d, c[1], qx + 1, row) << 1) |
			                         (PAT(d, c[2], qx + 2, row) << 2) | (PAT(d, c[3], qx + 3, row) << 3));
		}
		break;
	}
	case ZV_VIEW_PL_FULL: {
		int qx = (v & 1) * 4, qy = (v >> 1) * 4;
		for (y = 0; y < 4; y++) {
			const uint8_t *c = e + y * 4;
			int row = qy + y;
			d->r4[i][v * 4 + y] = PATW(d, c[0], qx, row) | (PATW(d, c[1], qx + 1, row) << 1) |
			                      (PATW(d, c[2], qx + 2, row) << 2) | (PATW(d, c[3], qx + 3, row) << 3);
		}
		break;
	}
	case ZV_VIEW_PL_HALF: {
		uint8_t a[4];
		int col = ((v >> 2) & 1) * 4 + (v & 1) * 2;
		int row = ((v >> 3) & 1) * 4 + ((v >> 1) & 1) * 2;
		half2(e, a);
		for (y = 0; y < 2; y++)
			d->r4[i][v * 2 + y] = PATW(d, a[y * 2], col, row + y) | (PATW(d, a[y * 2 + 1], col + 1, row + y) << 1);
		break;
	}
	case ZV_VIEW_PAL_FULL: {
		const uint8_t *m = mask_of(d->mask);
		int qx = (v & 1) * 4, qy = (v >> 1) * 4;
		for (y = 0; y < 4; y++)
			for (x = 0; x < 4; x++)
				r[v * 16 + y * 4 + x] = pal_px(d, e[y * 4 + x], m[(qy + y) * 8 + qx + x]);
		break;
	}
	case ZV_VIEW_MONO_HALF:
	case ZV_VIEW_PAL_HALF: {
		uint8_t a[4];
		const uint8_t *m = mask_of(d->mask);
		int col = ((v >> 2) & 1) * 4 + (v & 1) * 2;
		int row = ((v >> 3) & 1) * 4 + ((v >> 1) & 1) * 2;
		half2(e, a);
		for (y = 0; y < 2; y++) {
			if (d->view == ZV_VIEW_MONO_HALF)
				r[v * 2 + y] = (uint8_t)(PAT(d, a[y * 2], col, row + y) | (PAT(d, a[y * 2 + 1], col + 1, row + y) << 1));
			else
				for (x = 0; x < 2; x++)
					r[v * 4 + y * 2 + x] = pal_px(d, a[y * 2 + x], m[(row + y) * 8 + col + x]);
		}
		break;
	}
	default:
		break;
	}
}

// -- V16: a 2x2 entry. Full mono: one of the 16 places ph = sy:sx of a
// 2x2 cell in the 8x8 mask, one byte (bits: top-left, top-right,
// bottom-left, bottom-right). Half views: the cell is one pixel; all 64
// places at once: mono the grey's own pattern (8 bytes), palette 64 bytes.
static void draw_v16(zv_dec_t *d, int i, int ph) {
	const uint8_t *e = d->cb16[i];
	uint8_t *r = (uint8_t *)d->r16[i];
	int k;
	switch (d->view) {
	case ZV_VIEW_MONO_FULL:
		(void)ph;
		for (k = 0; k < 16; k++) {
			int sx = (k & 3) * 2, sy = (k >> 2) * 2;
			r[k] = (uint8_t)(PAT(d, e[0], sx, sy) | (PAT(d, e[1], sx + 1, sy) << 1) |
			                 (PAT(d, e[2], sx, sy + 1) << 2) | (PAT(d, e[3], sx + 1, sy + 1) << 3));
		}
		break;
	case ZV_VIEW_PAL_FULL: {
		const uint8_t *m = mask_of(d->mask);
		for (k = 0; k < 16; k++) {
			int sx = (k & 3) * 2, sy = (k >> 2) * 2;
			r[k * 4 + 0] = pal_px(d, e[0], m[sy * 8 + sx]);
			r[k * 4 + 1] = pal_px(d, e[1], m[sy * 8 + sx + 1]);
			r[k * 4 + 2] = pal_px(d, e[2], m[(sy + 1) * 8 + sx]);
			r[k * 4 + 3] = pal_px(d, e[3], m[(sy + 1) * 8 + sx + 1]);
		}
		break;
	}
	case ZV_VIEW_MONO_HALF:
		memcpy(r, d->pat[avg4(e)], 8);
		break;
	case ZV_VIEW_PL_FULL:
		for (k = 0; k < 16; k++) {
			int sx = (k & 3) * 2, sy = (k >> 2) * 2;
			d->r16[i][k] = PATW(d, e[0], sx, sy) | (PATW(d, e[1], sx + 1, sy) << 1) |
			               (PATW(d, e[2], sx, sy + 1) << 2) | (PATW(d, e[3], sx + 1, sy + 1) << 3);
		}
		break;
	case ZV_VIEW_PL_HALF:
		memcpy(r, d->pat4[avg4(e)], 32);
		break;
	case ZV_VIEW_PAL_HALF: {
		const uint8_t *m = mask_of(d->mask);
		uint8_t a = avg4(e);
		for (k = 0; k < 64; k++) r[k] = pal_px(d, a, m[k]);
		break;
	}
	default:
		break;
	}
}

// the entry drawn at this place, drawing it first if it is not
// an entry drawn for the view costs about this many blocks drawn from
// drawn entries: counted against step_blocks, so a call just after a
// seek point (every entry new) takes no longer than any other
#define ZV_DRAW_COST 6
#define ZV_STEP_V16 16      // V16 entries drawn a call, in steps
static inline const uint8_t *use_v1(zv_dec_t *d, int i, int ph) {
	if (!(d->ok1[i] & (1u << ph))) {
		d->draws++;
		draw_v1(d, i, ph);
		d->ok1[i] |= (uint16_t)(d->view == ZV_VIEW_MONO_FULL || d->view == ZV_VIEW_PAL_FULL || d->view == ZV_VIEW_PL_FULL ? 0xffff : 1u << ph);
	}
	return (const uint8_t *)d->r1[i];
}
static inline const uint8_t *use_v4(zv_dec_t *d, int i, int v) {
	if (!(d->ok4[i] & (1u << v))) { draw_v4(d, i, v); d->ok4[i] |= (uint16_t)(1u << v); d->draws++; }
	return (const uint8_t *)d->r4[i];
}
static inline const uint8_t *use_v16(zv_dec_t *d, int i, int ph) {
	if (!(d->ok16[i] & (1u << ph))) { draw_v16(d, i, ph); d->ok16[i] = 0xffff; }
	return (const uint8_t *)d->r16[i];
}

static void render_all(zv_dec_t *d) {
	const uint8_t *m = mask_of(d->mask);
	int g, k;
	for (g = 0; g < 256; g++) {
		memset(d->pat[g], 0, 8);
		for (k = 0; k < 64; k++) if (d->lin[g] > THR(m[k])) d->pat[g][k >> 3] |= (uint8_t)(1 << (k & 7));
	}
	if (d->view == ZV_VIEW_PL_FULL || d->view == ZV_VIEW_PL_HALF)
		for (g = 0; g < 256; g++)
			for (k = 0; k < 8; k++) {
				uint32_t w = 0;
				int x;
				for (x = 0; x < 8; x++) {
					uint32_t ix = pal_px(d, (uint8_t)g, m[k * 8 + x]) & 15u;
					w |= ((ix & 1) | ((ix & 2) << 7) | ((ix & 4) << 14) | ((ix & 8) << 21)) << x;
				}
				d->pat4[g][k] = w;
			}
	memset(d->ok1, 0, sizeof(d->ok1));
	memset(d->ok4, 0, sizeof(d->ok4));
	// V16 entries are drawn when they arrive (and here, all of them, for
	// the view): blocks use them without asking
	for (g = 0; g < ZV_CB_N; g++) draw_v16(d, g, 0);
	memset(d->ok16, 0xff, sizeof(d->ok16));
	d->drawn = 1;
}

// The palette view mixes real light from the palette, so it always uses
// the display's own gamma (2.2), whatever the mono views are set to.
static void setup_palette(zv_dec_t *d) {
	uint16_t pal[ZV_PAL_MAX];
	uint16_t pl[ZV_PAL_MAX];
	int n = zv_grey_palette(pal, ZV_PAL_MAX, d->greys), i, k = 0;
	for (i = 0; i < n; i++) pl[i] = zv_lin12(pal[i], 220);
	d->pal_n = n;
	for (i = 0; i < 256; i++) {
		uint32_t l = pow_frac((uint32_t)i, 255, 220);
		while (k + 1 < n && pl[k + 1] <= l) k++;
		while (k > 0 && pl[k] > l) k--;
		d->pal_lo[i] = (uint8_t)k;
		if (k + 1 >= n || l <= pl[k]) d->pal_frac[i] = 0;
		else d->pal_frac[i] = (uint8_t)(((l - pl[k]) * 64) / (uint32_t)(pl[k + 1] - pl[k]));
	}
}

void zv_dec_init(zv_dec_t *d, const zv_header_t *h, int view, int mask, int g100) {
	int i;
	memset(d, 0, sizeof(*d));
	d->h = *h;
	d->bw = h->w / ZV_BLK;
	d->bh = h->h / ZV_BLK;
	if (g100 <= 0) g100 = ZV_GAMMA_DEFAULT;
	for (i = 0; i < 256; i++) d->lin[i] = (uint16_t)pow_frac((uint32_t)i, 255, g100);
	d->greys = ZV_GREYS_TRUE;   // neutral: the near-greys' tints show in flat areas
	setup_palette(d);
	d->view = view;
	d->mask = mask;
	render_all(d);
}

void zv_dec_set_view(zv_dec_t *d, int view, int mask) {
	d->rs_on = 0;               // (a frame part drawn is dropped: seek, or decode on)
	d->view = view;
	d->mask = mask;
	render_all(d);
}

void zv_mono_to_planes(const uint32_t *mono, int mono_st, uint32_t *pl, int pl_st, int plane_off, int words, int h) {
	int y, x, k;
	for (y = 0; y < h; y++) {
		const uint32_t *m = (const uint32_t *)(const void *)((const uint8_t *)mono + (size_t)y * mono_st);
		for (k = 0; k < 4; k++) {
			uint32_t *p = (uint32_t *)(void *)((uint8_t *)pl + (size_t)k * plane_off + (size_t)y * pl_st);
			for (x = 0; x < words; x++) p[x] = m[x];
		}
	}
}

// A pixel is white where its grey is above the mask's threshold there
// (0..14, so 15 is always white and 0 never): the four planes' words
// against the threshold's four, 32 pixels a time.
void zv_planes_to_mono(const uint32_t *pl, int pl_st, int plane_off, uint32_t *mono, int mono_st, int words, int h, int mask) {
	const uint8_t *mk = mask_of(mask);
	uint32_t t[8][4];
	int y, x, k;
	for (y = 0; y < 8; y++)
		for (k = 0; k < 4; k++) {
			uint32_t b = 0;
			for (x = 0; x < 8; x++) b |= (uint32_t)(((mk[y * 8 + x] * 15u / 64u) >> k) & 1u) << x;
			t[y][k] = b * 0x01010101u;
		}
	for (y = 0; y < h; y++) {
		const uint32_t *p = (const uint32_t *)(const void *)((const uint8_t *)pl + (size_t)y * pl_st);
		uint32_t *m = (uint32_t *)(void *)((uint8_t *)mono + (size_t)y * mono_st);
		uint32_t t0 = t[y & 7][0], t1 = t[y & 7][1], t2 = t[y & 7][2], t3 = t[y & 7][3];
		int po = plane_off / 4;
		for (x = 0; x < words; x++) {
			uint32_t p0 = p[x], p1 = p[x + po], p2 = p[x + 2 * po], p3 = p[x + 3 * po];
			uint32_t gt = p0 & ~t0;
			gt = (p1 & ~t1) | (~(p1 ^ t1) & gt);
			gt = (p2 & ~t2) | (~(p2 ^ t2) & gt);
			gt = (p3 & ~t3) | (~(p3 ^ t3) & gt);
			m[x] = gt;
		}
	}
}

void zv_dec_set_greys(zv_dec_t *d, int kind) {
	d->greys = kind;
	setup_palette(d);
	render_all(d);
}

void zv_dec_seek(zv_dec_t *d, uint32_t frame) {
	d->frame = frame;
	d->have_key = 0;
	d->rs_on = 0;
}

void zv_view_size(const zv_dec_t *d, int *w, int *h, int *ms) {
	int fw = d->h.w, fh = d->h.h;
	switch (d->view) {
	case ZV_VIEW_GREY_FULL: *w = fw; *h = fh; *ms = fw; break;
	case ZV_VIEW_MONO_FULL: case ZV_VIEW_PL_FULL: *w = fw; *h = fh; *ms = fw / 8; break;
	case ZV_VIEW_MONO_HALF: case ZV_VIEW_PL_HALF: *w = fw / 2; *h = fh / 2; *ms = (fw / 2 + 7) / 8; break;
	case ZV_VIEW_PAL_FULL: *w = fw; *h = fh; *ms = fw; break;
	default: *w = fw / 2; *h = fh / 2; *ms = fw / 2; break;
	}
}

// ---- drawing blocks -----------------------------------------------------

static void put_v1(zv_dec_t *d, int bx, int by, int i, uint8_t *out, int st) {
	const uint8_t *e = d->cb1[i];
	int x, y;
	switch (d->view) {
	case ZV_VIEW_GREY_FULL:
		for (y = 0; y < 8; y++) {
			uint8_t *o = out + (by * 8 + y) * st + bx * 8;
			const uint8_t *s = e + (y >> 1) * 4;
			for (x = 0; x < 8; x++) o[x] = s[x >> 1];
		}
		break;
	case ZV_VIEW_GREY_HALF:
		for (y = 0; y < 4; y++) memcpy(out + (by * 4 + y) * st + bx * 4, e + y * 4, 4);
		break;
	case ZV_VIEW_MONO_FULL: {
		const uint8_t *r = use_v1(d, i, 0);
		uint8_t *o = out + by * 8 * st + bx;
		for (y = 0; y < 8; y++, o += st) *o = r[y];
		break;
	}
	case ZV_VIEW_MONO_HALF: {
		int ph = ((by & 1) << 1) | (bx & 1), sh = (bx & 1) * 4;
		const uint8_t *r = use_v1(d, i, ph) + ph * 4;
		uint8_t keep = (uint8_t)~(0x0f << sh), *o = out + by * 4 * st + (bx >> 1);
		for (y = 0; y < 4; y++, o += st) *o = (uint8_t)((*o & keep) | (r[y] << sh));
		break;
	}
	case ZV_VIEW_PAL_HALF: {
		int ph = ((by & 1) << 1) | (bx & 1);
		const uint32_t *r = (const uint32_t *)(use_v1(d, i, ph) + ph * 16);
		uint8_t *o = out + by * 4 * st + bx * 4;
		for (y = 0; y < 4; y++, o += st) *(uint32_t *)o = r[y];
		break;
	}
	case ZV_VIEW_PAL_FULL: {
		const uint32_t *r = (const uint32_t *)(const void *)use_v1(d, i, 0);
		uint8_t *o = out + by * 8 * st + bx * 8;
		for (y = 0; y < 8; y++, o += st) { ((uint32_t *)o)[0] = r[y * 2]; ((uint32_t *)o)[1] = r[y * 2 + 1]; }
		break;
	}
	}
}

static void put_v4(zv_dec_t *d, int bx, int by, const uint8_t *ix, uint8_t *out, int st) {
	int x, y, q;
	switch (d->view) {
	case ZV_VIEW_GREY_FULL:
		for (q = 0; q < 4; q++) {
			const uint8_t *e = d->cb4[ix[q]];
			for (y = 0; y < 4; y++)
				memcpy(out + (by * 8 + (q >> 1) * 4 + y) * st + bx * 8 + (q & 1) * 4, e + y * 4, 4);
		}
		break;
	case ZV_VIEW_GREY_HALF:
		for (q = 0; q < 4; q++) {
			uint8_t a[4];
			half2(d->cb4[ix[q]], a);
			for (y = 0; y < 2; y++)
				for (x = 0; x < 2; x++)
					out[(by * 4 + (q >> 1) * 2 + y) * st + bx * 4 + (q & 1) * 2 + x] = a[y * 2 + x];
		}
		break;
	case ZV_VIEW_MONO_FULL: {
		uint8_t *o = out + by * 8 * st + bx;
		for (q = 0; q < 4; q += 2) {
			const uint8_t *l = use_v4(d, ix[q], q) + q * 4;
			const uint8_t *rr = use_v4(d, ix[q + 1], q + 1) + (q + 1) * 4;
			for (y = 0; y < 4; y++, o += st) *o = (uint8_t)(l[y] | (rr[y] << 4));
		}
		break;
	}
	case ZV_VIEW_MONO_HALF: {
		int base = ((by & 1) << 3) | ((bx & 1) << 2), sh = (bx & 1) * 4;
		uint8_t keep = (uint8_t)~(0x0f << sh), *o = out + by * 4 * st + (bx >> 1);
		for (q = 0; q < 4; q += 2) {
			int vl = base | q, vr = base | q | 1;
			const uint8_t *l = use_v4(d, ix[q], vl) + vl * 2;
			const uint8_t *rr = use_v4(d, ix[q + 1], vr) + vr * 2;
			for (y = 0; y < 2; y++, o += st) *o = (uint8_t)((*o & keep) | ((l[y] | (rr[y] << 2)) << sh));
		}
		break;
	}
	case ZV_VIEW_PAL_HALF: {
		int base = ((by & 1) << 3) | ((bx & 1) << 2);
		for (q = 0; q < 4; q++) {
			int v = base | q;
			const uint8_t *r = use_v4(d, ix[q], v) + v * 4;
			uint8_t *o = out + (by * 4 + (q >> 1) * 2) * st + bx * 4 + (q & 1) * 2;
			o[0] = r[0]; o[1] = r[1];
			o[st] = r[2]; o[st + 1] = r[3];
		}
		break;
	}
	case ZV_VIEW_PAL_FULL:
		for (q = 0; q < 4; q++) {
			const uint32_t *r = (const uint32_t *)(const void *)(use_v4(d, ix[q], q) + q * 16);
			uint8_t *o = out + (by * 8 + (q >> 1) * 4) * st + bx * 8 + (q & 1) * 4;
			for (y = 0; y < 4; y++, o += st) *(uint32_t *)o = r[y];
		}
		break;
	}
}

static void put_v16(zv_dec_t *d, int bx, int by, const uint8_t *ix, uint8_t *out, int st) {
	int sx, sy;
	switch (d->view) {
	case ZV_VIEW_GREY_FULL:
		for (sy = 0; sy < 4; sy++)
			for (sx = 0; sx < 4; sx++) {
				const uint8_t *e = d->cb16[ix[sy * 4 + sx]];
				uint8_t *o = out + (by * 8 + sy * 2) * st + bx * 8 + sx * 2;
				o[0] = e[0]; o[1] = e[1]; o[st] = e[2]; o[st + 1] = e[3];
			}
		break;
	case ZV_VIEW_GREY_HALF:
		for (sy = 0; sy < 4; sy++)
			for (sx = 0; sx < 4; sx++)
				out[(by * 4 + sy) * st + bx * 4 + sx] = avg4(d->cb16[ix[sy * 4 + sx]]);
		break;
	case ZV_VIEW_MONO_FULL: {
		uint8_t *o = out + by * 8 * st + bx;
		for (sy = 0; sy < 4; sy++, o += 2 * st) {
			uint8_t r0 = 0, r1 = 0;
			for (sx = 0; sx < 4; sx++) {
				int ph = sy * 4 + sx;
				uint8_t v = use_v16(d, ix[ph], ph)[ph];
				r0 |= (uint8_t)((v & 3) << (sx * 2));
				r1 |= (uint8_t)(((v >> 2) & 3) << (sx * 2));
			}
			o[0] = r0;
			o[st] = r1;
		}
		break;
	}
	case ZV_VIEW_MONO_HALF: {
		int px = (bx & 1) * 4, py = (by & 1) * 4, sh = (bx & 1) * 4;
		uint8_t keep = (uint8_t)~(0x0f << sh), *o = out + by * 4 * st + (bx >> 1);
		for (sy = 0; sy < 4; sy++, o += st) {
			uint8_t nb = 0;
			for (sx = 0; sx < 4; sx++)
				nb |= (uint8_t)(((use_v16(d, ix[sy * 4 + sx], 0)[py + sy] >> (px + sx)) & 1) << sx);
			*o = (uint8_t)((*o & keep) | (nb << sh));
		}
		break;
	}
	case ZV_VIEW_PAL_HALF: {
		int px = (bx & 1) * 4, py = (by & 1) * 4;
		uint8_t *o = out + by * 4 * st + bx * 4;
		for (sy = 0; sy < 4; sy++, o += st)
			for (sx = 0; sx < 4; sx++)
				o[sx] = use_v16(d, ix[sy * 4 + sx], 0)[(py + sy) * 8 + px + sx];
		break;
	}
	case ZV_VIEW_PAL_FULL:
		for (sy = 0; sy < 4; sy++)
			for (sx = 0; sx < 4; sx++) {
				int ph = sy * 4 + sx;
				const uint8_t *r = use_v16(d, ix[ph], 0) + ph * 4;
				uint8_t *o = out + (by * 8 + sy * 2) * st + bx * 8 + sx * 2;
				o[0] = r[0]; o[1] = r[1]; o[st] = r[2]; o[st + 1] = r[3];
			}
		break;
	}
}

// 16 bytes from an unaligned source into an aligned entry, by words;
// true if that changed it (a seek point sends every entry again, mostly
// as it was: those keep what was drawn of them)
static int copy16(uint8_t *dst, const uint8_t *s) {
	uint32_t *w = (uint32_t *)(void *)dst, diff = 0;
	int k;
	for (k = 0; k < 4; k++, s += 4) {
		uint32_t v = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
		diff |= w[k] ^ v;
		w[k] = v;
	}
	return diff != 0;
}

// ---- the board's views: one loop each ---------------------------------------
//
// The same blocks as put_v1/v4/v16 above, with the output pointer kept
// as the loop goes rather than worked out for every block. These are
// what a Zeitlos runs; the tests hold them to the generic code.

#define R16(d, i) ((const uint8_t *)(d)->r16[i])
// No colour: the words ORed in are these.
static const uint32_t col_zero[4];
#define COLW(d, bi) ((d)->colw ? (d)->colw + (bi) * 4 : col_zero)

// the generic path's: the classes into a block already drawn
static void col_fix(const zv_dec_t *d, int bx, int by, uint8_t *out, int st) {
	const uint32_t *q = d->colw + (by * d->bw + bx) * 4;
	int bs = d->view == ZV_VIEW_PAL_FULL ? 8 : 4, x, y;
	uint8_t *o = out + (size_t)by * bs * st + bx * bs;
	for (y = 0; y < bs; y++)
		for (x = 0; x < bs; x++)
			o[y * st + x] = (uint8_t)((o[y * st + x] & 15) | (q[(y >= bs / 2) * 2 + (x >= bs / 2)] & 0xf0));
}

#define NEXT_BLOCK(adv, rowstep) do { if (dmap) dmap[bi] = 1; bi++; o += (adv); if (++bx == bw) { bx = 0; by++; row += (rowstep); o = row; } } while (0)

static int fast_mono_full(zv_dec_t *d, const uint8_t *p, const uint8_t *end, uint8_t *out, int st) {
	uint8_t *dmap = d->dirty;
	int bw = d->bw, nb = bw * d->bh, b = 0, bi = 0, bx = 0, by = 0, rs = 8 * st;
	uint8_t *row = out, *o = out;
	int left = d->step_blocks > 0 ? d->step_blocks : 0x7fffffff;
	d->draws = 0;
	b = d->rs_b; bx = d->rs_bx; by = d->rs_by;        /* (0 at a frame's start) */
	row = out + by * rs; o = row + (bx);
	while (b < nb) {
		if (left <= d->draws * ZV_DRAW_COST) { d->rs_p = p; d->rs_b = b; d->rs_bx = bx; d->rs_by = by; return 2; }
		int c, run, k, y;
		if (p >= end) return -4;
		c = *p++;
		run = (c & 0xc0) == 0xc0 ? ZV_CMD_RUN32(c) : ZV_CMD_RUN(c);
		if (b + run > nb) return -5;
		b += run;
		bi = b - run;
		if ((c & 0xc0) != ZV_CMD_SKIP) left -= run;
		switch (c & 0xc0) {
		case ZV_CMD_SKIP:
			bx += run;
			while (bx >= bw) { bx -= bw; by++; row += rs; }
			o = row + bx;
			break;
		case ZV_CMD_V1:
			if (end - p < run) return -6;
			for (k = 0; k < run; k++) {
				const uint32_t *r = (const uint32_t *)(const void *)use_v1(d, *p++, 0);
				uint32_t a = r[0], z = r[1];
				o[0] = (uint8_t)a; o[st] = (uint8_t)(a >> 8); o[2 * st] = (uint8_t)(a >> 16); o[3 * st] = (uint8_t)(a >> 24);
				o[4 * st] = (uint8_t)z; o[5 * st] = (uint8_t)(z >> 8); o[6 * st] = (uint8_t)(z >> 16); o[7 * st] = (uint8_t)(z >> 24);
				NEXT_BLOCK(1, rs);
			}
			break;
		case ZV_CMD_V4:
			if (end - p < run * 4) return -6;
			for (k = 0; k < run; k++, p += 4) {
				const uint8_t *a = use_v4(d, p[0], 0), *bb = use_v4(d, p[1], 1) + 4;
				const uint8_t *cc = use_v4(d, p[2], 2) + 8, *dd = use_v4(d, p[3], 3) + 12;
				uint8_t *q = o;
				for (y = 0; y < 4; y++, q += st) *q = (uint8_t)(a[y] | (bb[y] << 4));
				for (y = 0; y < 4; y++, q += st) *q = (uint8_t)(cc[y] | (dd[y] << 4));
				NEXT_BLOCK(1, rs);
			}
			break;
		default:
			if ((c & 0xe0) == ZV_CMD_V16) {
				if (end - p < run * 16) return -6;
				for (k = 0; k < run; k++, p += 16) {
					uint8_t *q = o;
					for (y = 0; y < 4; y++, q += 2 * st) {
						uint8_t v0 = R16(d, p[y * 4])[y * 4], v1 = R16(d, p[y * 4 + 1])[y * 4 + 1];
						uint8_t v2 = R16(d, p[y * 4 + 2])[y * 4 + 2], v3 = R16(d, p[y * 4 + 3])[y * 4 + 3];
						q[0] = (uint8_t)((v0 & 3) | ((v1 & 3) << 2) | ((v2 & 3) << 4) | ((v3 & 3) << 6));
						q[st] = (uint8_t)((v0 >> 2) | ((v1 >> 2) << 2) | ((v2 >> 2) << 4) | ((v3 >> 2) << 6));
					}
					NEXT_BLOCK(1, rs);
				}
			} else {
				const uint32_t *r;
				uint32_t a, z;
				if (p >= end) return -6;
				r = (const uint32_t *)(const void *)use_v1(d, *p++, 0);
				a = r[0]; z = r[1];
				for (k = 0; k < run; k++) {
					o[0] = (uint8_t)a; o[st] = (uint8_t)(a >> 8); o[2 * st] = (uint8_t)(a >> 16); o[3 * st] = (uint8_t)(a >> 24);
					o[4 * st] = (uint8_t)z; o[5 * st] = (uint8_t)(z >> 8); o[6 * st] = (uint8_t)(z >> 16); o[7 * st] = (uint8_t)(z >> 24);
					NEXT_BLOCK(1, rs);
				}
			}
			break;
		}
	}
	return 0;
}

// half mono: a block is a nibble of 4 rows; two blocks share a byte
static int fast_mono_half(zv_dec_t *d, const uint8_t *p, const uint8_t *end, uint8_t *out, int st) {
	uint8_t *dmap = d->dirty;
	int bw = d->bw, nb = bw * d->bh, b = 0, bi = 0, bx = 0, by = 0, rs = 4 * st;
	uint8_t *row = out, *o = out;
	int left = d->step_blocks > 0 ? d->step_blocks : 0x7fffffff;
	d->draws = 0;
	b = d->rs_b; bx = d->rs_bx; by = d->rs_by;        /* (0 at a frame's start) */
	row = out + by * rs; o = row + ((bx >> 1));
	while (b < nb) {
		if (left <= d->draws * ZV_DRAW_COST) { d->rs_p = p; d->rs_b = b; d->rs_bx = bx; d->rs_by = by; return 2; }
		int c, run, k, y;
		if (p >= end) return -4;
		c = *p++;
		run = (c & 0xc0) == 0xc0 ? ZV_CMD_RUN32(c) : ZV_CMD_RUN(c);
		if (b + run > nb) return -5;
		b += run;
		bi = b - run;
		if ((c & 0xc0) != ZV_CMD_SKIP) left -= run;
		if ((c & 0xc0) == ZV_CMD_SKIP) {
			bx += run;
			while (bx >= bw) { bx -= bw; by++; row += rs; }
			o = row + (bx >> 1);
			continue;
		}
		if ((c & 0xc0) == ZV_CMD_V1 && end - p < run) return -6;
		if ((c & 0xc0) == ZV_CMD_V4 && end - p < run * 4) return -6;
		if ((c & 0xe0) == ZV_CMD_V16 && end - p < run * 16) return -6;
		if ((c & 0xe0) == ZV_CMD_REP && p >= end) return -6;
		{
			int rep = (c & 0xe0) == ZV_CMD_REP, ri = rep ? *p++ : 0;
			for (k = 0; k < run; k++) {
				int sh = (bx & 1) * 4, ph = ((by & 1) << 1) | (bx & 1);
				uint8_t keep = (uint8_t)~(0x0f << sh), n4[4];
				if (rep || (c & 0xc0) == ZV_CMD_V1) {
					const uint8_t *r = use_v1(d, rep ? ri : *p++, ph) + ph * 4;
					n4[0] = r[0]; n4[1] = r[1]; n4[2] = r[2]; n4[3] = r[3];
				} else if ((c & 0xc0) == ZV_CMD_V4) {
					int base = ((by & 1) << 3) | ((bx & 1) << 2);
					const uint8_t *a = use_v4(d, p[0], base) + base * 2;
					const uint8_t *bb = use_v4(d, p[1], base | 1) + (base | 1) * 2;
					const uint8_t *cc = use_v4(d, p[2], base | 2) + (base | 2) * 2;
					const uint8_t *dd = use_v4(d, p[3], base | 3) + (base | 3) * 2;
					n4[0] = (uint8_t)(a[0] | (bb[0] << 2)); n4[1] = (uint8_t)(a[1] | (bb[1] << 2));
					n4[2] = (uint8_t)(cc[0] | (dd[0] << 2)); n4[3] = (uint8_t)(cc[1] | (dd[1] << 2));
					p += 4;
				} else {
					int px = (bx & 1) * 4, py = (by & 1) * 4, sx;
					for (y = 0; y < 4; y++) {
						uint8_t v = 0;
						for (sx = 0; sx < 4; sx++)
							v |= (uint8_t)(((R16(d, p[y * 4 + sx])[py + y] >> (px + sx)) & 1) << sx);
						n4[y] = v;
					}
					p += 16;
				}
				o[0] = (uint8_t)((o[0] & keep) | (n4[0] << sh));
				o[st] = (uint8_t)((o[st] & keep) | (n4[1] << sh));
				o[2 * st] = (uint8_t)((o[2 * st] & keep) | (n4[2] << sh));
				o[3 * st] = (uint8_t)((o[3 * st] & keep) | (n4[3] << sh));
				NEXT_BLOCK(bx & 1, rs);
			}
		}
	}
	return 0;
}

// ---- the plane views: the mono loops, four planes a time ----------------
//
// A word holds a row's bits for the four planes, a byte each, so the
// mono loops' arithmetic does all four at once; a row is then stored a
// byte a plane.

#define PUT4(q, w) do { uint32_t w_ = (w); (q)[0] = (uint8_t)w_; (q)[pl] = (uint8_t)(w_ >> 8); \
	(q)[2 * pl] = (uint8_t)(w_ >> 16); (q)[3 * pl] = (uint8_t)(w_ >> 24); } while (0)

static int fast_pl_full(zv_dec_t *d, const uint8_t *p, const uint8_t *end, uint8_t *out, int st) {
	uint8_t *dmap = d->dirty;
	int bw = d->bw, nb = bw * d->bh, b = 0, bi = 0, bx = 0, by = 0, rs = 8 * st, pl = d->plane_off;
	uint8_t *row = out, *o = out;
	int left = d->step_blocks > 0 ? d->step_blocks : 0x7fffffff;
	d->draws = 0;
	b = d->rs_b; bx = d->rs_bx; by = d->rs_by;        /* (0 at a frame's start) */
	row = out + by * rs; o = row + (bx);
	while (b < nb) {
		if (left <= d->draws * ZV_DRAW_COST) { d->rs_p = p; d->rs_b = b; d->rs_bx = bx; d->rs_by = by; return 2; }
		int c, run, k, y;
		if (p >= end) return -4;
		c = *p++;
		run = (c & 0xc0) == 0xc0 ? ZV_CMD_RUN32(c) : ZV_CMD_RUN(c);
		if (b + run > nb) return -5;
		b += run;
		bi = b - run;
		if ((c & 0xc0) != ZV_CMD_SKIP) left -= run;
		switch (c & 0xc0) {
		case ZV_CMD_SKIP:
			bx += run;
			while (bx >= bw) { bx -= bw; by++; row += rs; }
			o = row + bx;
			break;
		case ZV_CMD_V1:
			if (end - p < run) return -6;
			for (k = 0; k < run; k++) {
				const uint32_t *r = (const uint32_t *)(const void *)use_v1(d, *p++, 0);
				uint8_t *q = o;
				for (y = 0; y < 8; y++, q += st) PUT4(q, r[y]);
				NEXT_BLOCK(1, rs);
			}
			break;
		case ZV_CMD_V4:
			if (end - p < run * 4) return -6;
			for (k = 0; k < run; k++, p += 4) {
				const uint32_t *a = (const uint32_t *)(const void *)use_v4(d, p[0], 0);
				const uint32_t *bb = (const uint32_t *)(const void *)use_v4(d, p[1], 1) + 4;
				const uint32_t *cc = (const uint32_t *)(const void *)use_v4(d, p[2], 2) + 8;
				const uint32_t *dd = (const uint32_t *)(const void *)use_v4(d, p[3], 3) + 12;
				uint8_t *q = o;
				for (y = 0; y < 4; y++, q += st) PUT4(q, a[y] | (bb[y] << 4));
				for (y = 0; y < 4; y++, q += st) PUT4(q, cc[y] | (dd[y] << 4));
				NEXT_BLOCK(1, rs);
			}
			break;
		default:
			if ((c & 0xe0) == ZV_CMD_V16) {
				if (end - p < run * 16) return -6;
				for (k = 0; k < run; k++, p += 16) {
					uint8_t *q = o;
					for (y = 0; y < 4; y++, q += 2 * st) {
						uint32_t v0 = d->r16[p[y * 4]][y * 4], v1 = d->r16[p[y * 4 + 1]][y * 4 + 1];
						uint32_t v2 = d->r16[p[y * 4 + 2]][y * 4 + 2], v3 = d->r16[p[y * 4 + 3]][y * 4 + 3];
						uint32_t m3 = B4(3);
						PUT4(q, (v0 & m3) | ((v1 & m3) << 2) | ((v2 & m3) << 4) | ((v3 & m3) << 6));
						PUT4(q + st, ((v0 >> 2) & m3) | (((v1 >> 2) & m3) << 2) | (((v2 >> 2) & m3) << 4) | (((v3 >> 2) & m3) << 6));
					}
					NEXT_BLOCK(1, rs);
				}
			} else {
				const uint32_t *r;
				if (p >= end) return -6;
				r = (const uint32_t *)(const void *)use_v1(d, *p++, 0);
				for (k = 0; k < run; k++) {
					uint8_t *q = o;
					for (y = 0; y < 8; y++, q += st) PUT4(q, r[y]);
					NEXT_BLOCK(1, rs);
				}
			}
			break;
		}
	}
	return 0;
}

static int fast_pl_half(zv_dec_t *d, const uint8_t *p, const uint8_t *end, uint8_t *out, int st) {
	uint8_t *dmap = d->dirty;
	int bw = d->bw, nb = bw * d->bh, b = 0, bi = 0, bx = 0, by = 0, rs = 4 * st, pl = d->plane_off;
	uint8_t *row = out, *o = out;
	int left = d->step_blocks > 0 ? d->step_blocks : 0x7fffffff;
	d->draws = 0;
	b = d->rs_b; bx = d->rs_bx; by = d->rs_by;        /* (0 at a frame's start) */
	row = out + by * rs; o = row + ((bx >> 1));
	while (b < nb) {
		if (left <= d->draws * ZV_DRAW_COST) { d->rs_p = p; d->rs_b = b; d->rs_bx = bx; d->rs_by = by; return 2; }
		int c, run, k, y, j;
		if (p >= end) return -4;
		c = *p++;
		run = (c & 0xc0) == 0xc0 ? ZV_CMD_RUN32(c) : ZV_CMD_RUN(c);
		if (b + run > nb) return -5;
		b += run;
		bi = b - run;
		if ((c & 0xc0) != ZV_CMD_SKIP) left -= run;
		if ((c & 0xc0) == ZV_CMD_SKIP) {
			bx += run;
			while (bx >= bw) { bx -= bw; by++; row += rs; }
			o = row + (bx >> 1);
			continue;
		}
		if ((c & 0xc0) == ZV_CMD_V1 && end - p < run) return -6;
		if ((c & 0xc0) == ZV_CMD_V4 && end - p < run * 4) return -6;
		if ((c & 0xe0) == ZV_CMD_V16 && end - p < run * 16) return -6;
		if ((c & 0xe0) == ZV_CMD_REP && p >= end) return -6;
		{
			int rep = (c & 0xe0) == ZV_CMD_REP, ri = rep ? *p++ : 0;
			for (k = 0; k < run; k++) {
				int sh = (bx & 1) * 4, ph = ((by & 1) << 1) | (bx & 1);
				uint8_t keep = (uint8_t)~(0x0f << sh);
				uint32_t n4[4];
				if (rep || (c & 0xc0) == ZV_CMD_V1) {
					const uint32_t *r = (const uint32_t *)(const void *)use_v1(d, rep ? ri : *p++, ph) + ph * 4;
					n4[0] = r[0]; n4[1] = r[1]; n4[2] = r[2]; n4[3] = r[3];
				} else if ((c & 0xc0) == ZV_CMD_V4) {
					int base = ((by & 1) << 3) | ((bx & 1) << 2);
					const uint32_t *a = (const uint32_t *)(const void *)use_v4(d, p[0], base) + base * 2;
					const uint32_t *bb = (const uint32_t *)(const void *)use_v4(d, p[1], base | 1) + (base | 1) * 2;
					const uint32_t *cc = (const uint32_t *)(const void *)use_v4(d, p[2], base | 2) + (base | 2) * 2;
					const uint32_t *dd = (const uint32_t *)(const void *)use_v4(d, p[3], base | 3) + (base | 3) * 2;
					n4[0] = a[0] | (bb[0] << 2); n4[1] = a[1] | (bb[1] << 2);
					n4[2] = cc[0] | (dd[0] << 2); n4[3] = cc[1] | (dd[1] << 2);
					p += 4;
				} else {
					int px = (bx & 1) * 4, py = (by & 1) * 4, sx;
					for (y = 0; y < 4; y++) {
						uint32_t v = 0;
						for (sx = 0; sx < 4; sx++)
							v |= ((d->r16[p[y * 4 + sx]][py + y] >> (px + sx)) & B4(1)) << sx;
						n4[y] = v;
					}
					p += 16;
				}
				for (y = 0; y < 4; y++) {
					uint8_t *q = o + y * st;
					uint32_t w = n4[y] << sh;
					for (j = 0; j < 4; j++, q += pl, w >>= 8) *q = (uint8_t)((*q & keep) | (w & 0xff));
				}
				NEXT_BLOCK(bx & 1, rs);
			}
		}
	}
	return 0;
}

// a half view's row of a block, as a word: two pixels of each quarter
// (the quarters' words have the class in every byte)
#define HALF_W(l, r) (((l) & 0x0000ffffu) | ((r) & 0xffff0000u))
// a full view's V1 block with its classes: rows 0-3 the top quarters
#define FULL_V1_COL(w, r, q) do { \
	uint32_t c0 = (q)[0], c1 = (q)[1], c2 = (q)[2], c3 = (q)[3]; int y_; \
	for (y_ = 0; y_ < 4; y_++, (w) += sw) { (w)[0] = (r)[y_ * 2] | c0; (w)[1] = (r)[y_ * 2 + 1] | c1; } \
	for (; y_ < 8; y_++, (w) += sw) { (w)[0] = (r)[y_ * 2] | c2; (w)[1] = (r)[y_ * 2 + 1] | c3; } \
} while (0)

// the palette view: 4x4 bytes a block, word stores for V1
static int fast_pal_half(zv_dec_t *d, const uint8_t *p, const uint8_t *end, uint8_t *out, int st) {
	uint8_t *dmap = d->dirty;
	int bw = d->bw, nb = bw * d->bh, b = 0, bi = 0, bx = 0, by = 0, rs = 4 * st;
	uint8_t *row = out, *o = out;
	int left = d->step_blocks > 0 ? d->step_blocks : 0x7fffffff;
	d->draws = 0;
	b = d->rs_b; bx = d->rs_bx; by = d->rs_by;        /* (0 at a frame's start) */
	row = out + by * rs; o = row + (bx * 4);
	while (b < nb) {
		if (left <= d->draws * ZV_DRAW_COST) { d->rs_p = p; d->rs_b = b; d->rs_bx = bx; d->rs_by = by; return 2; }
		int c, run, k, y, q;
		if (p >= end) return -4;
		c = *p++;
		run = (c & 0xc0) == 0xc0 ? ZV_CMD_RUN32(c) : ZV_CMD_RUN(c);
		if (b + run > nb) return -5;
		b += run;
		bi = b - run;
		if ((c & 0xc0) != ZV_CMD_SKIP) left -= run;
		switch (c & 0xc0) {
		case ZV_CMD_SKIP:
			bx += run;
			while (bx >= bw) { bx -= bw; by++; row += rs; }
			o = row + bx * 4;
			break;
		case ZV_CMD_V1:
			if (end - p < run) return -6;
			for (k = 0; k < run; k++) {
				int ph = ((by & 1) << 1) | (bx & 1);
				const uint32_t *r = (const uint32_t *)(use_v1(d, *p++, ph) + ph * 16);
				uint32_t ht = 0, hl = 0;
				if (d->colw) { const uint32_t *q = d->colw + bi * 4; ht = HALF_W(q[0], q[1]); hl = HALF_W(q[2], q[3]); }
				*(uint32_t *)o = r[0] | ht; *(uint32_t *)(o + st) = r[1] | ht;
				*(uint32_t *)(o + 2 * st) = r[2] | hl; *(uint32_t *)(o + 3 * st) = r[3] | hl;
				NEXT_BLOCK(4, rs);
			}
			break;
		case ZV_CMD_V4:
			if (end - p < run * 4) return -6;
			for (k = 0; k < run; k++, p += 4) {
				int base = ((by & 1) << 3) | ((bx & 1) << 2);
				const uint32_t *cq = COLW(d, bi);
				for (q = 0; q < 4; q++) {
					const uint8_t *r = use_v4(d, p[q], base | q) + (base | q) * 4;
					uint8_t *t = o + (q >> 1) * 2 * st + (q & 1) * 2, c = (uint8_t)cq[q];
					t[0] = r[0] | c; t[1] = r[1] | c; t[st] = r[2] | c; t[st + 1] = r[3] | c;
				}
				NEXT_BLOCK(4, rs);
			}
			break;
		default:
			if ((c & 0xe0) == ZV_CMD_V16) {
				if (end - p < run * 16) return -6;
				for (k = 0; k < run; k++, p += 16) {
					int px = (bx & 1) * 4, py = (by & 1) * 4, sx;
					const uint32_t *cq = COLW(d, bi);
					for (y = 0; y < 4; y++)
						for (sx = 0; sx < 4; sx++)
							o[y * st + sx] = R16(d, p[y * 4 + sx])[(py + y) * 8 + px + sx] | (uint8_t)cq[(y >> 1) * 2 + (sx >> 1)];
					NEXT_BLOCK(4, rs);
				}
			} else {
				int ix;
				if (p >= end) return -6;
				ix = *p++;
				for (k = 0; k < run; k++) {
					int ph = ((by & 1) << 1) | (bx & 1);
					const uint32_t *r = (const uint32_t *)(use_v1(d, ix, ph) + ph * 16);
					uint32_t ht = 0, hl = 0;
					if (d->colw) { const uint32_t *q = d->colw + bi * 4; ht = HALF_W(q[0], q[1]); hl = HALF_W(q[2], q[3]); }
					*(uint32_t *)o = r[0] | ht; *(uint32_t *)(o + st) = r[1] | ht;
					*(uint32_t *)(o + 2 * st) = r[2] | hl; *(uint32_t *)(o + 3 * st) = r[3] | hl;
					NEXT_BLOCK(4, rs);
				}
			}
			break;
		}
	}
	return 0;
}

// the palette view at full size: 8x8 bytes a block, word stores
static int fast_pal_full(zv_dec_t *d, const uint8_t *p, const uint8_t *end, uint8_t *out, int st) {
	uint8_t *dmap = d->dirty;
	int bw = d->bw, nb = bw * d->bh, b = 0, bi = 0, bx = 0, by = 0, rs = 8 * st, sw = st / 4;
	uint8_t *row = out, *o = out;
	int left = d->step_blocks > 0 ? d->step_blocks : 0x7fffffff;
	d->draws = 0;
	b = d->rs_b; bx = d->rs_bx; by = d->rs_by;        /* (0 at a frame's start) */
	row = out + by * rs; o = row + (bx * 8);
	while (b < nb) {
		if (left <= d->draws * ZV_DRAW_COST) { d->rs_p = p; d->rs_b = b; d->rs_bx = bx; d->rs_by = by; return 2; }
		int c, run, k, y, q;
		if (p >= end) return -4;
		c = *p++;
		run = (c & 0xc0) == 0xc0 ? ZV_CMD_RUN32(c) : ZV_CMD_RUN(c);
		if (b + run > nb) return -5;
		b += run;
		bi = b - run;
		if ((c & 0xc0) != ZV_CMD_SKIP) left -= run;
		switch (c & 0xc0) {
		case ZV_CMD_SKIP:
			bx += run;
			while (bx >= bw) { bx -= bw; by++; row += rs; }
			o = row + bx * 8;
			break;
		case ZV_CMD_V1:
			if (end - p < run) return -6;
			for (k = 0; k < run; k++) {
				const uint32_t *r = (const uint32_t *)(const void *)use_v1(d, *p++, 0);
				uint32_t *w = (uint32_t *)(void *)o;
				if (!d->colw) for (y = 0; y < 8; y++, w += sw) { w[0] = r[y * 2]; w[1] = r[y * 2 + 1]; }
				else FULL_V1_COL(w, r, d->colw + bi * 4);
				NEXT_BLOCK(8, rs);
			}
			break;
		case ZV_CMD_V4:
			if (end - p < run * 4) return -6;
			for (k = 0; k < run; k++, p += 4) {
				const uint32_t *cq = COLW(d, bi);
				for (q = 0; q < 4; q++) {
					const uint32_t *r = (const uint32_t *)(const void *)(use_v4(d, p[q], q) + q * 16);
					uint32_t *w = (uint32_t *)(void *)(o + (q >> 1) * 4 * st + (q & 1) * 4), c = cq[q];
					w[0] = r[0] | c; w[sw] = r[1] | c; w[2 * sw] = r[2] | c; w[3 * sw] = r[3] | c;
				}
				NEXT_BLOCK(8, rs);
			}
			break;
		default:
			if ((c & 0xe0) == ZV_CMD_V16) {
				if (end - p < run * 16) return -6;
				for (k = 0; k < run; k++, p += 16) {
					uint32_t *w = (uint32_t *)(void *)o, cl, cr;
					const uint32_t *cq = COLW(d, bi);
					for (y = 0; y < 4; y++, w += 2 * sw) {
						const uint8_t *a = R16(d, p[y * 4]) + (y * 4) * 4, *bb = R16(d, p[y * 4 + 1]) + (y * 4 + 1) * 4;
						const uint8_t *cc = R16(d, p[y * 4 + 2]) + (y * 4 + 2) * 4, *ee = R16(d, p[y * 4 + 3]) + (y * 4 + 3) * 4;
						w[0] = (uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)bb[0] << 16) | ((uint32_t)bb[1] << 24);
						w[1] = (uint32_t)cc[0] | ((uint32_t)cc[1] << 8) | ((uint32_t)ee[0] << 16) | ((uint32_t)ee[1] << 24);
						w[sw] = (uint32_t)a[2] | ((uint32_t)a[3] << 8) | ((uint32_t)bb[2] << 16) | ((uint32_t)bb[3] << 24);
						w[sw + 1] = (uint32_t)cc[2] | ((uint32_t)cc[3] << 8) | ((uint32_t)ee[2] << 16) | ((uint32_t)ee[3] << 24);
						cl = cq[(y >> 1) * 2]; cr = cq[(y >> 1) * 2 + 1];
						w[0] |= cl; w[1] |= cr; w[sw] |= cl; w[sw + 1] |= cr;
					}
					NEXT_BLOCK(8, rs);
				}
			} else {
				const uint32_t *r;
				if (p >= end) return -6;
				r = (const uint32_t *)(const void *)use_v1(d, *p++, 0);
				for (k = 0; k < run; k++) {
					uint32_t *w = (uint32_t *)(void *)o;
					if (!d->colw) for (y = 0; y < 8; y++, w += sw) { w[0] = r[y * 2]; w[1] = r[y * 2 + 1]; }
					else FULL_V1_COL(w, r, d->colw + bi * 4);
					NEXT_BLOCK(8, rs);
				}
			}
			break;
		}
	}
	return 0;
}

int zv_dec_video(zv_dec_t *d, const uint8_t *p, size_t n, int key, uint8_t *out, int st) {
	const uint8_t *end = p + n;
	uint32_t n1, n4, n16, i;
	int b = 0, nb = d->bw * d->bh;
	int bx = 0, by = 0, dn = 0;
	if (d->rs_on) {
		// a frame begun and not finished (step_blocks): on with it
		p = d->rs_p;
		end = d->rs_end;
		if (d->rs_on == 2) { n16 = d->rs_n16; i = d->rs_i; goto books16; }
		goto blocks;
	}
	d->rs_b = d->rs_bx = d->rs_by = 0;
	if (n < 6) return -1;
	n1 = rd16(p);
	n4 = rd16(p + 2);
	n16 = rd16(p + 4);
	p += 6;
	if (n1 > ZV_CB_N || n4 > ZV_CB_N || n16 > ZV_CB_N) return -2;
	if ((size_t)(end - p) < (n1 + n4) * (1 + ZV_VEC) + n16 * (1 + ZV_VEC16)) return -3;
	if (!key && !d->have_key) { d->frame++; return 1; }   // waiting for a seek point
	if (key) d->have_key = 1;
	for (i = 0; i < n1; i++, p += 1 + ZV_VEC)
		if (copy16(d->cb1[p[0]], p + 1) || !d->drawn) d->ok1[p[0]] = 0;
	for (i = 0; i < n4; i++, p += 1 + ZV_VEC)
		if (copy16(d->cb4[p[0]], p + 1) || !d->drawn) d->ok4[p[0]] = 0;
	i = 0;
books16:
	for (; i < n16; i++, p += 1 + ZV_VEC16) {
		uint8_t *e = d->cb16[p[0]];
		if (e[0] == p[1] && e[1] == p[2] && e[2] == p[3] && e[3] == p[4] && d->drawn) continue;
		e[0] = p[1]; e[1] = p[2]; e[2] = p[3]; e[3] = p[4];
		// drawn now: V16 entries are used 16 to a block
		draw_v16(d, p[0], 0);
		d->ok16[p[0]] = 0xffff;
		// (in steps: a seek point's 256 of them are a frame's work
		// in the plane views, so a few dozen a call)
		if (d->step_blocks > 0 && !d->generic && ++dn >= ZV_STEP_V16 && i + 1 < n16) {
			d->rs_i = i + 1; d->rs_n16 = n16;
			d->rs_p = p + 1 + ZV_VEC16; d->rs_end = end;
			d->rs_on = 2;
			return 2;
		}
	}
	d->drawn = 1;
	d->rs_end = end;
	if (d->step_blocks > 0 && !d->generic && (dn > 0 || d->rs_on == 2)) {
		// the books were a call's work themselves
		d->rs_p = p;
		d->rs_on = 1;
		return 2;
	}
blocks:
	if (!d->generic || d->view == ZV_VIEW_PL_FULL || d->view == ZV_VIEW_PL_HALF) {
		int r = 1;
		switch (d->view) {
		case ZV_VIEW_MONO_FULL: r = fast_mono_full(d, p, end, out, st); break;
		case ZV_VIEW_MONO_HALF: r = fast_mono_half(d, p, end, out, st); break;
		case ZV_VIEW_PAL_HALF: r = fast_pal_half(d, p, end, out, st); break;
		case ZV_VIEW_PL_FULL: r = fast_pl_full(d, p, end, out, st); break;
		case ZV_VIEW_PL_HALF: r = fast_pl_half(d, p, end, out, st); break;
		case ZV_VIEW_PAL_FULL:
			if (!(st & 3) && !((uintptr_t)out & 3)) r = fast_pal_full(d, p, end, out, st);
			break;
		}
		if (r == 2) { d->rs_on = 1; return 2; }        // more to do: call again
		d->rs_on = 0;
		if (r <= 0) { d->frame++; return r; }
	}
	while (b < nb) {
		int c, run, k;
		if (p >= end) return -4;
		c = *p++;
		run = (c & 0xc0) == 0xc0 ? ZV_CMD_RUN32(c) : ZV_CMD_RUN(c);
		if (b + run > nb) return -5;
		switch (c & 0xc0) {
		case ZV_CMD_SKIP:
			b += run;
			bx += run;
			while (bx >= d->bw) { bx -= d->bw; by++; }
			continue;
		case ZV_CMD_V1:
			if (end - p < run) return -6;
			for (k = 0; k < run; k++) {
				put_v1(d, bx, by, *p++, out, st);
				if (d->colw && (d->view == ZV_VIEW_PAL_HALF || d->view == ZV_VIEW_PAL_FULL)) col_fix(d, bx, by, out, st);
				if (d->dirty) d->dirty[by * d->bw + bx] = 1;
				if (++bx == d->bw) { bx = 0; by++; }
			}
			break;
		case ZV_CMD_V4:
			if (end - p < run * 4) return -6;
			for (k = 0; k < run; k++) {
				put_v4(d, bx, by, p, out, st);
				if (d->colw && (d->view == ZV_VIEW_PAL_HALF || d->view == ZV_VIEW_PAL_FULL)) col_fix(d, bx, by, out, st);
				p += 4;
				if (d->dirty) d->dirty[by * d->bw + bx] = 1;
				if (++bx == d->bw) { bx = 0; by++; }
			}
			break;
		default: {               // ZV_CMD_REP, ZV_CMD_V16
			int ix;
			if ((c & 0xe0) == ZV_CMD_V16) {
				if (end - p < run * 16) return -6;
				for (k = 0; k < run; k++) {
					put_v16(d, bx, by, p, out, st);
				if (d->colw && (d->view == ZV_VIEW_PAL_HALF || d->view == ZV_VIEW_PAL_FULL)) col_fix(d, bx, by, out, st);
					p += 16;
					if (d->dirty) d->dirty[by * d->bw + bx] = 1;
					if (++bx == d->bw) { bx = 0; by++; }
				}
				break;
			}
			if (p >= end) return -6;
			ix = *p++;
			for (k = 0; k < run; k++) {
				put_v1(d, bx, by, ix, out, st);
				if (d->colw && (d->view == ZV_VIEW_PAL_HALF || d->view == ZV_VIEW_PAL_FULL)) col_fix(d, bx, by, out, st);
				if (d->dirty) d->dirty[by * d->bw + bx] = 1;
				if (++bx == d->bw) { bx = 0; by++; }
			}
			break;
		}
		}
		b += run;
	}
	d->frame++;
	return 0;
}

// ---- the colour layer -------------------------------------------------------

int zv_col_parse_header(const uint8_t *p, size_t n, zv_col_header_t *c) {
	if (n < ZV_COL_HDR || rd32(p) != ZV_COL_MAGIC) return -1;
	c->cw = rd16(p + 4);
	c->ch = rd16(p + 6);
	c->groups = rd32(p + 8);
	c->cell = p[12] ? p[12] : 8;
	if (c->cell != 8 && c->cell != 4) return -3;
	if (!c->cw || !c->ch || c->cw > ZV_MAX_W / c->cell || c->ch > ZV_MAX_H / c->cell) return -2;
	return 0;
}

int zv_col_frame(const uint8_t *p, size_t n, uint8_t *cls, int cells, uint8_t *mark) {
	uint32_t len, i = 2;
	int at = 0;
	if (n < 2) return -1;
	len = rd16(p);
	if (len + 2 > n) return -1;
	while (i < len + 2) {
		uint8_t c = p[i++];
		int k = (c & 127) + 1, j;
		if (at + k > cells) return -2;
		if (!(c & 0x80)) { at += k; continue; }
		if (i + (uint32_t)((k + 1) >> 1) > len + 2) return -3;
		for (j = 0; j < k; j++, at++) {
			uint8_t c = (uint8_t)((j & 1) ? p[i + (j >> 1)] >> 4 : p[i + (j >> 1)] & 15);
			if (mark && cls[at] != c) mark[at] = 1;
			cls[at] = c;
		}
		i += (uint32_t)((k + 1) >> 1);
	}
	return (int)(len + 2);
}

// 0..15 from 0..255*16 (fixed point), rounded and clipped
static int ch4(int32_t v) {
	v = (v + 8 * 17) / (16 * 17);
	return v < 0 ? 0 : v > 15 ? 15 : v;
}

// the most of a chroma step `d` (at full strength, s = 16) that keeps
// y + d * s / 16 inside lo..hi
static int fit16(int32_t y, int32_t d, int32_t lo, int32_t hi) {
	int32_t m;
	if (d > 0) m = (hi - y) * 16 / d;
	else if (d < 0) m = (y - lo) * 16 / -d;
	else return 16;
	return m < 0 ? 0 : m > 16 ? 16 : (int)m;
}

void zv_col_palette(const int8_t *uv, uint16_t *pal) {
	int c, l;
	for (c = 0; c < ZV_COL_CLASSES; c++)
		for (l = 0; l < 16; l++) {
			// in 1/16 steps of 0..255: Y' of the level, then the chroma,
			// scaled down until the colour fits (keeps its brightness) --
			// worked out, not searched: 3 divisions an entry, so a group's
			// 256 colours are a few tens of thousands of cycles
			int32_t y = l * 17 * 16, u = c ? uv[c * 2] * 16 : 0, v = c ? uv[c * 2 + 1] * 16 : 0;
			int32_t dr = (1402 * v) / 1000, dg = -(344 * u + 714 * v) / 1000, db = (1772 * u) / 1000;
			int32_t lo = -8 * 16, hi = 263 * 16, uu, vv;
			int s = fit16(y, dr, lo, hi), t, r, g, b;
			t = fit16(y, dg, lo, hi); if (t < s) s = t;
			t = fit16(y, db, lo, hi); if (t < s) s = t;
			uu = u * s / 16; vv = v * s / 16;
			r = (int)(y + (1402 * vv) / 1000);
			g = (int)(y - (344 * uu + 714 * vv) / 1000);
			b = (int)(y + (1772 * uu) / 1000);
			pal[c * 16 + l] = (uint16_t)(ch4(r) << 8 | ch4(g) << 4 | ch4(b));
		}
}

void zv_col_apply(const uint8_t *cls, int cw, int ch, int cell, uint8_t *out, int stride) {
	int cx, cy, x, y;
	for (cy = 0; cy < ch; cy++)
		for (y = 0; y < cell; y++) {
			uint8_t *o = out + (size_t)(cy * cell + y) * stride;
			for (cx = 0; cx < cw; cx++) {
				uint8_t hi = (uint8_t)(cls[cy * cw + cx] << 4);
				for (x = 0; x < cell; x++, o++) *o = (uint8_t)((*o & 15) | hi);
			}
		}
}

void zv_col_apply_map(const uint8_t *cls, const uint8_t *map, int bw, int bh, int bs, int cell,
                      uint8_t *out, int stride) {
	int bx, by, y, w, wpb = bs >> 2, ws = stride >> 2;
	int cw = cell == 4 ? bw * 2 : bw;
	for (by = 0; by < bh; by++)
		for (bx = 0; bx < bw; bx++) {
			uint32_t *o;
			if (!map[by * bw + bx]) continue;
			o = (uint32_t *)(void *)(out + (size_t)by * bs * stride) + bx * wpb;
			if (cell != 4) {
				uint32_t hi = (uint32_t)cls[by * bw + bx] * 0x10101010u;
				for (y = 0; y < bs; y++, o += ws)
					for (w = 0; w < wpb; w++) o[w] = (o[w] & 0x0f0f0f0fu) | hi;
			} else {
				// four cells: the top two for the block's top half
				const uint8_t *c0 = cls + (size_t)(by * 2) * cw + bx * 2;
				for (y = 0; y < bs; y++, o += ws) {
					const uint8_t *c = c0 + (y >= bs / 2 ? cw : 0);
					if (wpb == 2) {         // 8 wide: a word a cell
						o[0] = (o[0] & 0x0f0f0f0fu) | (uint32_t)c[0] * 0x10101010u;
						o[1] = (o[1] & 0x0f0f0f0fu) | (uint32_t)c[1] * 0x10101010u;
					} else {                // 4 wide: half a word each (little-endian)
						o[0] = (o[0] & 0x0f0f0f0fu) | (uint32_t)c[0] * 0x00001010u | (uint32_t)c[1] * 0x10100000u;
					}
				}
			}
		}
}

int zv_col_frame_blocks(const uint8_t *p, size_t n, uint8_t *cls, int cw, int ch, int cell,
                        uint8_t *bmark, uint16_t *list, int *nlist) {
	uint32_t len, i = 2;
	int at = 0, cx = 0, cy = 0, cells = cw * ch, sh = cell == 4 ? 1 : 0, bw = cw >> sh, nl = *nlist;
	if (n < 2) return -1;
	len = rd16(p);
	if (len + 2 > n) return -1;
	while (i < len + 2) {
		uint8_t c = p[i++];
		int k = (c & 127) + 1, j;
		if (at + k > cells) return -2;
		if (!(c & 0x80)) {
			at += k;
			cx += k;
			while (cx >= cw) { cx -= cw; cy++; }
			continue;
		}
		if (i + (uint32_t)((k + 1) >> 1) > len + 2) return -3;
		for (j = 0; j < k; j++, at++) {
			uint8_t v = (uint8_t)((j & 1) ? p[i + (j >> 1)] >> 4 : p[i + (j >> 1)] & 15);
			if (cls[at] != v) {
				int bx = cx >> sh, by = cy >> sh, b = by * bw + bx;
				cls[at] = v;
				if (!bmark[b]) { bmark[b] = 1; list[nl++] = (uint16_t)(by << 8 | bx); }
			}
			if (++cx == cw) { cx = 0; cy++; }
		}
		i += (uint32_t)((k + 1) >> 1);
	}
	*nlist = nl;
	return (int)(len + 2);
}

static uint32_t rep4(uint32_t v) { v |= v << 8; return v | (v << 16); }

void zv_col_words(const uint8_t *cls, int cw, int ch, int cell, const uint8_t *bmark, uint32_t *colw) {
	int bx, by, bw = cell == 4 ? cw / 2 : cw, bh = cell == 4 ? ch / 2 : ch, b = 0;
	const uint8_t *row = cls;
	for (by = 0; by < bh; by++, row += cell == 4 ? 2 * cw : cw)
		for (bx = 0; bx < bw; bx++, b++) {
			uint32_t *w;
			if (bmark && !bmark[b]) continue;
			w = colw + b * 4;
			if (cell == 4) {
				const uint8_t *c = row + bx * 2;
				w[0] = rep4((uint32_t)c[0] << 4); w[1] = rep4((uint32_t)c[1] << 4);
				w[2] = rep4((uint32_t)c[cw] << 4); w[3] = rep4((uint32_t)c[cw + 1] << 4);
			} else w[0] = w[1] = w[2] = w[3] = rep4((uint32_t)row[bx] << 4);
		}
}

void zv_col_words_list(const uint8_t *cls, int cw, int cell, const uint16_t *list, int n, uint32_t *colw) {
	int i, bw = cell == 4 ? cw / 2 : cw;
	for (i = 0; i < n; i++) {
		int bx = list[i] & 0xff, by = list[i] >> 8;
		uint32_t *w = colw + (by * bw + bx) * 4;
		if (cell == 4) {
			const uint8_t *c = cls + (by * 2) * cw + bx * 2;
			w[0] = rep4((uint32_t)c[0] << 4); w[1] = rep4((uint32_t)c[1] << 4);
			w[2] = rep4((uint32_t)c[cw] << 4); w[3] = rep4((uint32_t)c[cw + 1] << 4);
		} else w[0] = w[1] = w[2] = w[3] = rep4((uint32_t)cls[by * cw + bx] << 4);
	}
}

void zv_col_apply_list(const uint32_t *colw, int bw, const uint16_t *list, int n, int bs,
                       uint8_t *out, int stride) {
	int i, y, ws = stride >> 2;
	for (i = 0; i < n; i++) {
		int bx = list[i] & 0xff, by = list[i] >> 8;
		const uint32_t *q = colw + (by * bw + bx) * 4;
		uint32_t *o = (uint32_t *)(void *)(out + (size_t)by * bs * stride) + bx * (bs >> 2);
		if (bs == 8)
			for (y = 0; y < 8; y++, o += ws) {
				int h = y < 4 ? 0 : 2;
				o[0] = (o[0] & 0x0f0f0f0fu) | q[h];
				o[1] = (o[1] & 0x0f0f0f0fu) | q[h + 1];
			}
		else
			for (y = 0; y < 4; y++, o += ws) {
				int h = y < 2 ? 0 : 2;
				o[0] = (o[0] & 0x0f0f0f0fu) | HALF_W(q[h], q[h + 1]);
			}
	}
}
