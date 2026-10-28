/*
 * draw -- the tools' stamps, a word at a time. See drawstamp.h.
 */

#include "drawstamp.h"

const uint8_t draw_brush_mask[DRAW_BRUSH_SIZE] = {
	0x3C, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C
};

// Pattern row `r` (MSB leftmost) as a page word (LSB leftmost),
// repeated four times across it. A word starts on a multiple of 32,
// so of 8 too, and the pattern stays anchored to the page's grid.
static uint32_t pat_word(uint8_t r) {
	uint32_t rev = 0;
	for (int i = 0; i < 8; i++)
		if ((r >> (7 - i)) & 1) rev |= 1u << i;
	return rev * 0x01010101u;
}

// Pixels x0 + i of row y, for each bit i of `bits` (at most 32 wide),
// take their value from `pw`. Two words at most; anything off the page
// is dropped.
//
// The page is a whole number of words wide, so dropping a word that is
// off the page drops exactly the pixels that are: no pixel-level
// clipping is needed. x0 may be negative -- `>> 5` floors on this
// compiler and `& 31` is then the non-negative remainder, the same
// reliance canvas_fetch32() in draw.c states.
static void put_bits(uint32_t *page, int y, int x0, uint32_t bits,
	uint32_t pw) {

	if ((unsigned)y >= DRAW_PAGE_H || !bits) return;

	int wi = x0 >> 5, s = x0 & 31;
	uint32_t *row = page + y * DRAW_PAGE_WPL;

	// what falls in word wi, and what spills into wi + 1. Shifting by
	// 32 is undefined, so s == 0 (nothing spills) is its own case.
	uint32_t lo = bits << s;
	uint32_t hi = s ? bits >> (32 - s) : 0;

	if (lo && wi >= 0 && wi < DRAW_PAGE_WPL)
		row[wi] = (row[wi] & ~lo) | (pw & lo);
	if (hi && wi + 1 >= 0 && wi + 1 < DRAW_PAGE_WPL)
		row[wi + 1] = (row[wi + 1] & ~hi) | (pw & hi);

}

void draw_stamp_brush(uint32_t *page, int x, int y, const uint8_t *pat) {

	int x0 = x - DRAW_BRUSH_SIZE / 2, y0 = y - DRAW_BRUSH_SIZE / 2;

	for (int j = 0; j < DRAW_BRUSH_SIZE; j++) {

		int row = y0 + j;
		if ((unsigned)row >= DRAW_PAGE_H) continue;

		// the mask row is MSB-leftmost too: reverse it the same way
		uint32_t bits = 0;
		for (int i = 0; i < DRAW_BRUSH_SIZE; i++)
			if (draw_brush_mask[j] & (0x80u >> i)) bits |= 1u << i;

		put_bits(page, row, x0, bits, pat_word(pat[row & 7]));

	}

}

void draw_stamp_eraser(uint32_t *page, int x, int y) {

	int x0 = x - DRAW_ERASER_SIZE / 2, y0 = y - DRAW_ERASER_SIZE / 2;
	uint32_t bits = (1u << DRAW_ERASER_SIZE) - 1;

	for (int j = 0; j < DRAW_ERASER_SIZE; j++)
		put_bits(page, y0 + j, x0, bits, 0);

}

// A page-wide pixel-by-pixel fill took a few hundred ms -- longer than
// the main loop may stop (docs/responsive.md). Same pixels as setting
// each one to its pattern bit, clipped to the page.
void draw_fill_rect(uint32_t *page, int x, int y, int w, int h,
	const uint8_t *pat) {

	int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= DRAW_PAGE_W) x1 = DRAW_PAGE_W - 1;
	if (y1 >= DRAW_PAGE_H) y1 = DRAW_PAGE_H - 1;
	if (x1 < x0 || y1 < y0) return;

	int wa = x0 >> 5, wb = x1 >> 5;
	uint32_t ma = 0xFFFFFFFFu << (x0 & 31);
	// shifting by 32 is undefined, hence the special case
	uint32_t mb = (x1 & 31) == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu << ((x1 & 31) + 1));

	for (int j = y0; j <= y1; j++) {

		uint32_t pw = pat_word(pat[j & 7]);
		uint32_t *row = page + j * DRAW_PAGE_WPL;

		for (int wi = wa; wi <= wb; wi++) {
			uint32_t m = 0xFFFFFFFFu;
			if (wi == wa) m &= ma;
			if (wi == wb) m &= mb;
			row[wi] = (row[wi] & ~m) | (pw & m);
		}

	}

}

static void pset(uint32_t *page, int x, int y, int color) {
	if ((unsigned)x >= DRAW_PAGE_W || (unsigned)y >= DRAW_PAGE_H) return;
	uint32_t m = 1u << (x & 31);
	if (color) page[y * DRAW_PAGE_WPL + (x >> 5)] |= m;
	else page[y * DRAW_PAGE_WPL + (x >> 5)] &= ~m;
}

// The top and bottom edges as spans, the sides a pixel a row -- the
// sides are only ever two pixels wide.
void draw_rect(uint32_t *page, int x0, int y0, int x1, int y1, int color) {

	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

	static const uint8_t solid[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	static const uint8_t blank[8] = { 0 };
	const uint8_t *p = color ? solid : blank;

	draw_fill_rect(page, x0, y0, x1 - x0 + 1, 1, p);
	draw_fill_rect(page, x0, y1, x1 - x0 + 1, 1, p);

	for (int y = y0; y <= y1; y++) {
		pset(page, x0, y, color);
		pset(page, x1, y, color);
	}

}
