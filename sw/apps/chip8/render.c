/*
 * chip8 -- guest display to a 1bpp bitmap. See render.h.
 */

#include <string.h>

#include "render.h"

/* Ordered dither, 4x4. Values 0..15; a pixel is lit where its colour's
 * level exceeds the threshold at its position.
 *
 * The four colour levels are 0, 5, 11 and 16 sixteenths. Evenly spaced
 * would be 0, 5.33, 10.67 and 16, so this is within a third of a
 * subpixel of even -- as close as 17 available levels get. The
 * endpoints matter more than the spacing anyway: level 0 must be
 * entirely off and level 16 entirely on, or backgrounds get speckled
 * and solid foreground gets holes. 0 > t is never true and 16 > t is
 * always true, so both are exact rather than nearly exact. */
static const uint8_t bayer[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 }
};

/* Indexed by c8_palette_t, then by colour index. See render.h for why
 * C8_PAL_FILL rather than the index order is the default. */
static const uint8_t level_of[C8_PAL_COUNT][4] = {
	{ 0, 16, 11,  5 },      /* C8_PAL_FILL  */
	{ 0,  5, 11, 16 },      /* C8_PAL_INDEX */
	{ 0, 16, 16, 16 }       /* C8_PAL_SOLID */
};

static const char *const palette_names[C8_PAL_COUNT] = {
	"fill", "index", "solid"
};

const char *c8_palette_name(int pal) {
	if (pal < 0 || pal >= C8_PAL_COUNT) return "?";
	return palette_names[pal];
}

static int clamp_scale(int s) {
	if (s >= 4) return 4;
	if (s >= 2) return 2;
	return 1;
}

static void build_tables(c8_render_t *r) {

	int n, p, k, d, idx;
	int w = r->pxw;

	if (!r->grey) {

		/* One entry per nibble of guest pixels. Bit 3 of the nibble
		 * is the leftmost pixel (the core stores rows MSB-first);
		 * the leftmost output pixel is the LOWEST bit (the
		 * framebuffer is LSB-leftmost). The reversal lives here and
		 * nowhere else.
		 *
		 * No dither and no level table on this path: a one-plane
		 * program's colour 1 is FOREGROUND, so the expansion is
		 * simply "set, at pxw resolution". Running it through the
		 * grey path with a two-entry palette would give the same
		 * pixels and cost a 4KB table and four times the work. */
		for (n = 0; n < 16; n++) {
			uint32_t v = 0;
			for (p = 0; p < 4; p++) {
				if (!((n >> (3 - p)) & 1)) continue;
				for (k = 0; k < w; k++)
					v |= (uint32_t)1 << (p * w + k);
			}
			r->exp[n] = v;
		}

		r->tables_valid = true;
		return;

	}

	for (d = 0; d < 4; d++) {
		for (idx = 0; idx < 256; idx++) {

			uint32_t v = 0;
			int n0 = (idx >> 4) & 0xF;
			int n1 = idx & 0xF;

			for (p = 0; p < 4; p++) {

				int c = ((n0 >> (3 - p)) & 1) | (((n1 >> (3 - p)) & 1) << 1);
				int lvl = level_of[r->pal][c];

				for (k = 0; k < w; k++) {
					int x = p * w + k;
					/* x is content-relative, and every entry starts
					 * at an output x that is a multiple of 4*w --
					 * itself a multiple of 4 -- so the dither column
					 * within an entry is always (p*w + k) & 3 with no
					 * per-entry phase to carry. */
					if (lvl > bayer[d][x & 3])
						v |= (uint32_t)1 << x;
				}

			}

			r->gexp[d][idx] = v;

		}
	}

	r->tables_valid = true;

}

void c8_render_init(c8_render_t *r, int scale) {

	memset(r, 0, sizeof(*r));

	r->scale = clamp_scale(scale);
	r->w = C8_OUT_W(r->scale);
	r->h = C8_OUT_H(r->scale);
	r->stride = C8_STRIDE(r->scale);

	r->pal = C8_PAL_FILL;
	r->pxw = 0;             /* forces a table build on first render */
	r->tables_valid = false;

}

bool c8_render_set_scale(c8_render_t *r, int scale) {

	int s = clamp_scale(scale);

	if (s == r->scale) return false;

	r->scale = s;
	r->w = C8_OUT_W(s);
	r->h = C8_OUT_H(s);
	r->stride = C8_STRIDE(s);
	r->tables_valid = false;

	return true;

}

bool c8_render_set_palette(c8_render_t *r, int pal) {

	if (pal < 0 || pal >= C8_PAL_COUNT) return false;
	if (pal == r->pal) return false;

	r->pal = pal;
	r->tables_valid = false;

	return true;

}

/* Expand one guest row into one output row.
 *
 * 4*pxw is 4, 8, 16 or 32, so it always divides 32 exactly: a fixed
 * number of table entries pack into each output word with no entry
 * ever straddling a word boundary. That is why this is a nested pair
 * of counted loops rather than a bit cursor with carry handling.
 */
static void expand_row(const c8_render_t *r, const uint32_t *p0,
	const uint32_t *p1, int nibs, int dither_row, uint32_t *dst) {

	int bits = 4 * r->pxw;
	int per  = 32 / bits;
	int words = (nibs * bits) / 32;
	int wi, j;

	if (!r->grey) {
		for (wi = 0; wi < words; wi++) {
			uint32_t v = 0;
			for (j = 0; j < per; j++) {
				int nib = wi * per + j;
				int n = (int)((p0[nib >> 3] >> (28 - 4 * (nib & 7))) & 0xF);
				v |= r->exp[n] << (j * bits);
			}
			dst[wi] = v;
		}
		return;
	}

	for (wi = 0; wi < words; wi++) {
		uint32_t v = 0;
		for (j = 0; j < per; j++) {
			int nib = wi * per + j;
			int sh = 28 - 4 * (nib & 7);
			int n0 = (int)((p0[nib >> 3] >> sh) & 0xF);
			int n1 = (int)((p1[nib >> 3] >> sh) & 0xF);
			v |= r->gexp[dither_row][(n0 << 4) | n1] << (j * bits);
		}
		dst[wi] = v;
	}

}

bool c8_render(c8_render_t *r, c8_t *c, bool force) {

	int pxw = c->hires ? r->scale : r->scale * 2;
	bool grey = c->two_plane;
	int nibs = c->w / 4;
	int gy, y0 = 64, y1 = -1;

	if (pxw != r->pxw || grey != r->grey || !r->tables_valid) {
		r->pxw = pxw;
		r->grey = grey;
		build_tables(r);
		force = true;
	}

	if (force) {
		y0 = 0;
		y1 = c->h;
	} else {
		if (c->dirty == 0) {
			r->dirty_y0 = r->dirty_y1 = 0;
			return false;
		}
		/* Bounding band rather than per-row blits. One blit of a few
		 * extra rows costs far less than several blits of exactly the
		 * right ones -- the blitter has no queue, so each operation
		 * pays a full setup and a wait for idle. */
		for (gy = 0; gy < c->h; gy++) {
			if (!(c->dirty & ((uint64_t)1 << gy))) continue;
			if (gy < y0) y0 = gy;
			y1 = gy + 1;
		}
		if (y1 <= y0) {
			c8_clear_dirty(c);
			r->dirty_y0 = r->dirty_y1 = 0;
			return false;
		}
	}

	for (gy = y0; gy < y1; gy++) {

		int base = gy * pxw;
		int distinct = r->grey ? (pxw < 4 ? pxw : 4) : 1;
		int k;

		/* A guest row becomes pxw identical output rows in mono. In
		 * grey it becomes pxw rows that differ only by dither row, and
		 * pxw is 1, 2, 4 or 8 -- so for pxw >= 4 the guest row starts
		 * on dither row 0 and the pattern repeats every four. Either
		 * way only `distinct` rows are computed and the rest are
		 * copies. */
		for (k = 0; k < distinct; k++)
			expand_row(r, c->px[0][gy], c->px[1][gy], nibs, (base + k) & 3,
				(uint32_t *)(r->buf + (base + k) * r->stride));

		for (k = distinct; k < pxw; k++)
			memcpy(r->buf + (base + k) * r->stride,
			       r->buf + (base + k - distinct) * r->stride,
			       (size_t)r->stride);

	}

	c8_clear_dirty(c);

	r->dirty_y0 = y0 * pxw;
	r->dirty_y1 = y1 * pxw;

	return true;

}
