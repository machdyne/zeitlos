/*
 * draw -- the paint bucket, a little at a time. See drawfill.h.
 */

#include "drawfill.h"

#include <string.h>

enum { P_SPANS, P_SWEEP, P_PATTERN, P_DONE };

#define WPL		DRAW_PAGE_WPL
#define W		DRAW_PAGE_W
#define H		DRAW_PAGE_H

static uint32_t *row_of(const draw_fill_t *f, int y) { return f->page + y * WPL; }

// The pixels of the region's colour, as set bits.
static uint32_t tw(const draw_fill_t *f, uint32_t w) { return f->target ? w : ~w; }

// Pattern row (MSB leftmost) as a page word (LSB leftmost), as
// drawstamp.c's pat_word().
static uint32_t pat_word(uint8_t r) {
	uint32_t rev = 0;
	for (int i = 0; i < 8; i++)
		if ((r >> (7 - i)) & 1) rev |= 1u << i;
	return rev * 0x01010101u;
}

// Bits a..b (0..31, a <= b) of a word.
static uint32_t bits(int a, int b) {
	uint32_t hi = b == 31 ? 0xFFFFFFFFu : ((1u << (b + 1)) - 1u);
	return hi & ~((1u << a) - 1u);
}

static void touch(draw_fill_t *f, int x0, int x1, int y) {
	if (x0 < f->x0) f->x0 = x0;
	if (x1 > f->x1) f->x1 = x1;
	if (y < f->y0) f->y0 = y;
	if (y > f->y1) f->y1 = y;
}

static void push(draw_fill_t *f, int x0, int x1, int y) {
	if (f->sp >= DRAW_FILL_STACK) { f->overflow = true; return; }
	f->st[f->sp].x0 = (int16_t)x0;
	f->st[f->sp].x1 = (int16_t)x1;
	f->st[f->sp].y = (int16_t)y;
	f->sp++;
}

// The run of the region's colour through (x, y), which must be one:
// found whole words at a time, set to the other colour, and pushed.
// Returns its right end.
static int run(draw_fill_t *f, int x, int y) {

	uint32_t *r = row_of(f, y);
	int a = x, b = x;

	// right: the first pixel past x not of the colour
	for (int wi = x >> 5; wi < WPL; wi++) {
		uint32_t not_t = ~tw(f, r[wi]);
		if (wi == (x >> 5)) not_t &= ~bits(0, x & 31);
		if (not_t) { b = wi * 32 + __builtin_ctz(not_t) - 1; break; }
		b = wi * 32 + 31;
	}

	// left: the last pixel before x not of the colour
	for (int wi = x >> 5; wi >= 0; wi--) {
		uint32_t not_t = ~tw(f, r[wi]);
		if (wi == (x >> 5)) not_t &= bits(0, x & 31);
		if (not_t) { a = wi * 32 + 31 - __builtin_clz(not_t) + 1; break; }
		a = wi * 32;
	}

	// to the other colour: visited
	for (int wi = a >> 5; wi <= (b >> 5); wi++) {
		int lo = wi == (a >> 5) ? (a & 31) : 0;
		int hi = wi == (b >> 5) ? (b & 31) : 31;
		uint32_t m = bits(lo, hi);
		if (f->target) r[wi] &= ~m;
		else r[wi] |= m;
	}

	touch(f, a, b, y);
	push(f, a, b, y);
	return b;

}

// The row beside a span: every run of the region's colour that touches
// x0..x1 on row y.
static void beside(draw_fill_t *f, int x0, int x1, int y) {

	if ((unsigned)y >= H) return;
	const uint32_t *r = row_of(f, y);
	int x = x0;

	while (x <= x1) {
		int wi = x >> 5;
		uint32_t t = tw(f, r[wi]) & ~((1u << (x & 31)) - 1u);	// from x on
		if (!t) { x = (wi + 1) * 32; continue; }
		int at = wi * 32 + __builtin_ctz(t);
		if (at > x1) break;
		x = run(f, at, y) + 1;
	}

}

bool draw_fill_start(draw_fill_t *f, uint32_t *page, const uint32_t *orig,
	int x, int y, const uint8_t *pat) {

	memset(f, 0, sizeof(*f));
	if ((unsigned)x >= W || (unsigned)y >= H) { f->phase = P_DONE; return false; }

	f->page = page;
	f->orig = orig;
	f->pat = pat;
	f->target = (int)((page[y * WPL + (x >> 5)] >> (x & 31)) & 1u);
	f->x0 = f->x1 = x;
	f->y0 = f->y1 = y;
	f->phase = P_SPANS;
	run(f, x, y);
	return true;

}

// One row of a sweep: a pixel of the region's colour beside one the
// fill has changed is a run it missed. True if it found one.
static bool sweep_row(draw_fill_t *f, int y) {

	const uint32_t *r = row_of(f, y);
	const uint32_t *o = f->orig + y * WPL;
	const uint32_t *ru = y > 0 ? r - WPL : NULL, *ou = y > 0 ? o - WPL : NULL;
	const uint32_t *rd = y < H - 1 ? r + WPL : NULL, *od = y < H - 1 ? o + WPL : NULL;
	int wa = (f->x0 > 0 ? f->x0 - 1 : 0) >> 5, wb = (f->x1 < W - 1 ? f->x1 + 1 : W - 1) >> 5;

	for (int wi = wa; wi <= wb; wi++) {
		uint32_t t = tw(f, r[wi]);
		if (!t) continue;
		uint32_t c = r[wi] ^ o[wi];
		uint32_t l = wi > 0 ? (r[wi - 1] ^ o[wi - 1]) >> 31 : 0;
		uint32_t rr = wi < WPL - 1 ? (r[wi + 1] ^ o[wi + 1]) << 31 : 0;
		uint32_t n = (c << 1) | l | (c >> 1) | rr;
		if (ru) n |= ru[wi] ^ ou[wi];
		if (rd) n |= rd[wi] ^ od[wi];
		t &= n;
		if (t) {
			run(f, wi * 32 + __builtin_ctz(t), y);
			return true;
		}
	}
	return false;

}

int draw_fill_step(draw_fill_t *f, int work) {

	while (work-- > 0) {

		switch (f->phase) {

		case P_SPANS:
			if (f->sp) {
				draw_span_t s = f->st[--f->sp];
				beside(f, s.x0, s.x1, s.y - 1);
				beside(f, s.x0, s.x1, s.y + 1);
				break;
			}
			if (f->row) { f->phase = P_SWEEP; break; }		// back to a sweep
			if (f->overflow) {
				f->phase = P_SWEEP;
				f->row = (f->y0 > 0 ? f->y0 - 1 : 0) + 1;	// +1: 0 means none
				f->found = false;
				f->overflow = false;
				f->sweeps++;
				break;
			}
			f->phase = P_PATTERN;
			f->row = f->y0;
			break;

		case P_SWEEP: {
			int y = f->row - 1;
			int last = f->y1 < H - 1 ? f->y1 + 1 : H - 1;
			if (y > last) {
				if (f->found || f->overflow) {
					f->row = (f->y0 > 0 ? f->y0 - 1 : 0) + 1;
					f->found = false;
					f->overflow = false;
					f->sweeps++;
				} else {
					f->row = f->y0;
					f->phase = P_PATTERN;
				}
				break;
			}
			if (sweep_row(f, y)) {
				f->found = true;
				f->phase = P_SPANS;		// and back here, the same row
			} else f->row++;
			break;
		}

		case P_PATTERN: {
			if (f->row > f->y1) { f->phase = P_DONE; return DRAW_FILL_DONE; }
			int y = f->row++;
			uint32_t *r = row_of(f, y);
			const uint32_t *o = f->orig + y * WPL;
			uint32_t pw = pat_word(f->pat[y & 7]);
			for (int wi = f->x0 >> 5; wi <= (f->x1 >> 5); wi++) {
				uint32_t m = r[wi] ^ o[wi];
				r[wi] = (o[wi] & ~m) | (pw & m);
			}
			break;
		}

		default:
			return DRAW_FILL_DONE;

		}

	}

	return f->phase == P_DONE ? DRAW_FILL_DONE : DRAW_FILL_MORE;

}

void draw_fill_stop(draw_fill_t *f) {
	if (f->page && f->phase != P_DONE)
		for (int y = f->y0; y <= f->y1; y++)
			memcpy(row_of(f, y), f->orig + y * WPL, WPL * 4);
	f->phase = P_DONE;
}
