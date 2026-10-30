/*
 * draw's stamps (drawstamp.c) and undo (drawundo.c), on the build
 * machine.  make test
 *
 *   - the word-at-a-time brush, eraser, fill and outline leave exactly
 *     the pixels the per-pixel ones they replaced did (kept below, as
 *     they were in draw.c), on random strokes all over and off the page
 *   - undo and redo put the page back byte for byte, across many
 *     strokes and shapes, in both forms (a ring in memory; one level in
 *     the spare page), however full the ring is
 *   - the limits: DRAW_UNDO_LEVELS, a ring too small for an edit, a
 *     new edit dropping redo, a load into the spare page, "unmodified"
 *     again after undoing back to the save
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../drawstamp.h"
#include "../drawundo.h"
#include "../drawfill.h"

#define W	DRAW_PAGE_W
#define H	DRAW_PAGE_H
#define WPL	DRAW_PAGE_WPL
#define WORDS	DRAW_PAGE_WORDS

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static const uint8_t pats[4][8] = {
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 },
	{ 0x80, 0x10, 0x02, 0x40, 0x08, 0x01, 0x20, 0x04 },
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

// -- the per-pixel stamps, exactly as draw.c had them --

static uint32_t *ref;

static void ref_pset(int x, int y, int color) {
	if ((unsigned)x >= W || (unsigned)y >= H) return;
	uint32_t m = 1u << (x & 31);
	if (color) ref[y * WPL + (x >> 5)] |= m;
	else ref[y * WPL + (x >> 5)] &= ~m;
}

static int pat_bit(const uint8_t *pat, int x, int y) {
	return (pat[y & 7] >> (7 - (x & 7))) & 1;
}

static const uint8_t *ref_pat;

static void ref_brush(int x, int y) {
	for (int j = 0; j < DRAW_BRUSH_SIZE; j++)
		for (int i = 0; i < DRAW_BRUSH_SIZE; i++)
			if (draw_brush_mask[j] & (0x80u >> i)) {
				int px = x - DRAW_BRUSH_SIZE / 2 + i, py = y - DRAW_BRUSH_SIZE / 2 + j;
				ref_pset(px, py, pat_bit(ref_pat, px, py));
			}
}

static void ref_eraser(int x, int y) {
	for (int j = 0; j < DRAW_ERASER_SIZE; j++)
		for (int i = 0; i < DRAW_ERASER_SIZE; i++)
			ref_pset(x - DRAW_ERASER_SIZE / 2 + i, y - DRAW_ERASER_SIZE / 2 + j, 0);
}

static void ref_rect(int x0, int y0, int x1, int y1, int color) {
	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
	for (int x = x0; x <= x1; x++) { ref_pset(x, y0, color); ref_pset(x, y1, color); }
	for (int y = y0; y <= y1; y++) { ref_pset(x0, y, color); ref_pset(x1, y, color); }
}

static void ref_fill(int x, int y, int w, int h, const uint8_t *pat) {
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			ref_pset(i, j, pat_bit(pat, i, j));
}

// draw.c's Bresenham walk
static void walk(int x0, int y0, int x1, int y1, void (*stamp)(int, int)) {
	int dx = x1 - x0, dy = y1 - y0;
	int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
	if (dx < 0) dx = -dx;
	if (dy < 0) dy = -dy;
	int err = dx - dy;
	for (;;) {
		stamp(x0, y0);
		if (x0 == x1 && y0 == y1) break;
		int e2 = err * 2;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

// -- the new ones, on `page` --

static uint32_t *page;
static const uint8_t *cur_pat;
static void new_brush(int x, int y) { draw_stamp_brush(page, x, y, cur_pat); }
static void new_eraser(int x, int y) { draw_stamp_eraser(page, x, y); }

static void random_page(uint32_t *p) {
	for (int i = 0; i < WORDS; i++) p[i] = rnd() ^ (rnd() << 16);
}

// A point on the page or up to 20 px off any edge.
static int rx(void) { return rr(-20, W + 19); }
static int ry(void) { return rr(-20, H + 19); }

static void test_stamps(void) {

	static uint32_t a[WORDS], b[WORDS];
	ref = a; page = b;

	for (int round = 0; round < 300; round++) {

		random_page(a);
		memcpy(b, a, sizeof(a));

		for (int k = 0; k < 6; k++) {
			int kind = rr(0, 3);
			int x0 = rx(), y0 = ry(), x1 = rx(), y1 = ry();
			// mostly short strokes, as the mouse makes them
			if (rr(0, 3)) { x1 = x0 + rr(-40, 40); y1 = y0 + rr(-40, 40); }
			ref_pat = cur_pat = pats[rr(0, 3)];
			switch (kind) {
				case 0:
					walk(x0, y0, x1, y1, ref_brush);
					walk(x0, y0, x1, y1, new_brush);
					break;
				case 1:
					walk(x0, y0, x1, y1, ref_eraser);
					walk(x0, y0, x1, y1, new_eraser);
					break;
				case 2: {
					int c = rr(0, 1);
					ref_rect(x0, y0, x1, y1, c);
					draw_rect(b, x0, y0, x1, y1, c);
					break;
				}
				case 3: {
					int fw = rr(-2, 200), fh = rr(-2, 200);
					ref_fill(x0, y0, fw, fh, cur_pat);
					draw_fill_rect(b, x0, y0, fw, fh, cur_pat);
					break;
				}
			}
		}

		CHECK(!memcmp(a, b, sizeof(a)));

	}

	// every alignment of a single stamp, around both edges
	for (int y = -8; y < 12; y++)
		for (int x = -8; x < W + 8; x++) {
			if (x > 40 && x < W - 40) continue;
			memset(a, 0x5A, sizeof(a));
			memset(b, 0x5A, sizeof(b));
			ref_pat = cur_pat = pats[1];
			ref_brush(x, y); new_brush(x, y);
			ref_eraser(x, H - 1 - y); new_eraser(x, H - 1 - y);
			if (memcmp(a, b, sizeof(a))) { CHECK(!"stamp at an edge"); return; }
		}

}

// -- undo --

static uint32_t pg_a[WORDS], pg_b[WORDS];

// One random edit as draw.c makes one: begin, change, end with a box
// holding everything it may have touched.
static void edit(draw_undo_t *u, int big) {

	draw_undo_begin(u);
	page = u->page;
	cur_pat = pats[rr(0, 3)];

	int x0 = rr(0, W - 1), y0 = rr(0, H - 1), x1, y1;
	if (big) { x1 = rr(0, W - 1); y1 = rr(0, H - 1); }
	else { x1 = x0 + rr(-60, 60); y1 = y0 + rr(-60, 60); }

	int pad = DRAW_ERASER_SIZE;
	switch (rr(0, 4)) {
		case 0: walk(x0, y0, x1, y1, new_brush); break;
		case 1: walk(x0, y0, x1, y1, new_eraser); break;
		case 2: draw_rect(page, x0, y0, x1, y1, 1); pad = 0; break;
		case 3: {
			int ax = x0 < x1 ? x0 : x1, ay = y0 < y1 ? y0 : y1;
			draw_fill_rect(page, ax, ay, abs(x1 - x0) + 1, abs(y1 - y0) + 1, cur_pat);
			pad = 0;
			break;
		}
		case 4: pad = 0; break;	// a click that changes nothing
	}

	int bx0 = (x0 < x1 ? x0 : x1) - pad, bx1 = (x0 < x1 ? x1 : x0) + pad;
	int by0 = (y0 < y1 ? y0 : y1) - pad, by1 = (y0 < y1 ? y1 : y0) + pad;
	draw_undo_end(u, bx0, by0, bx1, by1);

}

#define STATES 80
static uint32_t states[STATES + 1][WORDS];

static void check_box(const uint32_t *before, const uint32_t *after, const int box[4]) {
	// every word that changed lies in the box undo reported
	for (int y = 0; y < H; y++)
		for (int w = 0; w < WPL; w++)
			if (before[y * WPL + w] != after[y * WPL + w])
				if (y < box[1] || y > box[3] || w * 32 < box[0] || w * 32 + 31 > box[2]) {
					CHECK(!"a change outside the box");
					return;
				}
}

// Many edits, then undo all the way and redo all the way, comparing
// with the page as it was at each step.
static void undo_round(void *mem, uint32_t bytes, int nedits, int big) {

	static uint32_t prev[WORDS];
	draw_undo_t u;
	random_page(pg_a);
	memset(pg_b, 0xEE, sizeof(pg_b));	// spare starts as anything
	draw_undo_init(&u, pg_a, pg_b, mem, bytes);

	memcpy(states[0], pg_a, sizeof(pg_a));
	int n = 0;
	for (int i = 0; i < nedits && n < STATES; i++) {
		int before = draw_undo_levels(&u);
		edit(&u, big);
		if (draw_undo_levels(&u) != before || memcmp(states[n], pg_a, sizeof(pg_a)))
			memcpy(states[++n], pg_a, sizeof(pg_a));
	}

	int levels = draw_undo_levels(&u);
	CHECK(levels <= n && levels <= DRAW_UNDO_LEVELS);
	if (!mem) CHECK(levels <= 1);
	CHECK(draw_undo_redo_levels(&u) == 0);

	int box[4];
	for (int k = 0; k < levels; k++) {
		memcpy(prev, pg_a, sizeof(pg_a));
		CHECK(draw_undo_undo(&u, box));
		CHECK(!memcmp(pg_a, states[n - 1 - k], sizeof(pg_a)));
		check_box(prev, pg_a, box);
	}
	CHECK(!draw_undo_undo(&u, box));
	CHECK(draw_undo_redo_levels(&u) == levels);

	for (int k = levels - 1; k >= 0; k--) {
		memcpy(prev, pg_a, sizeof(pg_a));
		CHECK(draw_undo_redo(&u, box));
		CHECK(!memcmp(pg_a, states[n - k], sizeof(pg_a)));
		check_box(prev, pg_a, box);
	}
	CHECK(!draw_undo_redo(&u, box));

	// half way back, then a new edit: redo is gone, undo still works
	for (int k = 0; k < levels / 2; k++) draw_undo_undo(&u, box);
	int left = draw_undo_levels(&u);
	memcpy(prev, pg_a, sizeof(pg_a));
	draw_undo_begin(&u);
	draw_fill_rect(pg_a, 100, 100, 50, 50, pats[1]);
	draw_fill_rect(pg_a, 101, 101, 48, 48, pats[2]);
	CHECK(draw_undo_end(&u, 100, 100, 149, 149));
	CHECK(draw_undo_redo_levels(&u) == 0);
	CHECK(draw_undo_levels(&u) == (mem ? left + 1 : 1));
	CHECK(draw_undo_undo(&u, box));
	CHECK(!memcmp(pg_a, prev, sizeof(pg_a)));

}

static void test_undo(void) {

	static uint32_t big[(4u << 20) / 4];

	// a roomy ring: DRAW_UNDO_LEVELS is the limit
	for (int r = 0; r < 4; r++) undo_round(big, sizeof(big), 100, 0);
	// page-sized edits: the ring is the limit, edits drop the oldest
	for (int r = 0; r < 4; r++) undo_round(big, 160u * 1024u, 60, 1);
	for (int r = 0; r < 4; r++) undo_round(big, 40u * 1024u, 30, 1);
	// tiny ring: mostly too large to keep
	for (int r = 0; r < 4; r++) undo_round(big, 2048, 30, 0);
	// no ring: one level, in spare
	for (int r = 0; r < 8; r++) undo_round(NULL, 0, 30, r & 1);

	// -- DRAW_UNDO_LEVELS, exactly --
	draw_undo_t u;
	int box[4];
	memset(pg_a, 0, sizeof(pg_a));
	draw_undo_init(&u, pg_a, pg_b, big, sizeof(big));
	for (int i = 0; i < DRAW_UNDO_LEVELS + 10; i++) {
		draw_undo_begin(&u);
		pg_a[i] = 1;
		CHECK(draw_undo_end(&u, 0, 0, W - 1, H - 1));
		CHECK(draw_undo_levels(&u) == (i < DRAW_UNDO_LEVELS ? i + 1 : DRAW_UNDO_LEVELS));
	}
	while (draw_undo_undo(&u, box));
	for (int i = 0; i < DRAW_UNDO_LEVELS + 10; i++) CHECK(pg_a[i] == (i < 10 ? 1u : 0u));

	// -- an edit larger than the ring forgets everything before it --
	draw_undo_init(&u, pg_a, pg_b, big, 8192);
	memset(pg_a, 0, sizeof(pg_a));
	draw_undo_clear(&u, pg_a, pg_b);
	draw_undo_begin(&u); pg_a[0] = 7; CHECK(draw_undo_end(&u, 0, 0, 0, 0));
	draw_undo_begin(&u); memset(pg_a, 0xFF, sizeof(pg_a));
	CHECK(!draw_undo_end(&u, 0, 0, W - 1, H - 1));
	CHECK(draw_undo_levels(&u) == 0);
	CHECK(!draw_undo_undo(&u, box));
	// and the next small one is undoable again, back to the big one
	draw_undo_begin(&u); pg_a[5] = 0; CHECK(draw_undo_end(&u, 160, 0, 191, 0));
	CHECK(draw_undo_undo(&u, box) && pg_a[5] == 0xFFFFFFFFu);
	CHECK(box[0] == 160 && box[2] == 191 && box[1] == 0 && box[3] == 0);

	// -- a box that misses the change keeps nothing; one that changed nothing too --
	draw_undo_begin(&u);
	CHECK(!draw_undo_end(&u, 0, 0, W - 1, H - 1));

	// -- saved state --
	memset(pg_a, 0, sizeof(pg_a));
	draw_undo_init(&u, pg_a, pg_b, big, sizeof(big));
	CHECK(draw_undo_at_saved(&u));
	draw_undo_begin(&u); pg_a[0] = 1; draw_undo_end(&u, 0, 0, 31, 0);
	CHECK(!draw_undo_at_saved(&u));
	draw_undo_mark_saved(&u);
	CHECK(draw_undo_at_saved(&u));
	draw_undo_begin(&u); pg_a[1] = 1; draw_undo_end(&u, 32, 0, 63, 0);
	CHECK(!draw_undo_at_saved(&u));
	draw_undo_undo(&u, box);
	CHECK(draw_undo_at_saved(&u));
	draw_undo_undo(&u, box);
	CHECK(!draw_undo_at_saved(&u));
	draw_undo_redo(&u, box);
	CHECK(draw_undo_at_saved(&u));
	// one level: dropped levels keep the count right
	memset(pg_a, 0, sizeof(pg_a));
	draw_undo_init(&u, pg_a, pg_b, NULL, 0);
	draw_undo_begin(&u); pg_a[0] = 1; draw_undo_end(&u, 0, 0, 31, 0);
	draw_undo_mark_saved(&u);
	draw_undo_begin(&u); pg_a[1] = 1; draw_undo_end(&u, 32, 0, 63, 0);
	CHECK(!draw_undo_at_saved(&u));
	CHECK(draw_undo_undo(&u, box) && draw_undo_at_saved(&u));
	CHECK(!draw_undo_undo(&u, box));
	CHECK(draw_undo_redo(&u, box) && !draw_undo_at_saved(&u));

	// -- a load into spare: the one-level form loses its level --
	memset(pg_a, 0, sizeof(pg_a));
	draw_undo_init(&u, pg_a, pg_b, NULL, 0);
	draw_undo_begin(&u); pg_a[3] = 9; draw_undo_end(&u, 96, 0, 127, 0);
	draw_undo_spare_lost(&u);
	memset(pg_b, 0x33, sizeof(pg_b));		// a stopped load's leftovers
	CHECK(!draw_undo_undo(&u, box) && pg_a[3] == 9);
	draw_undo_begin(&u); pg_a[4] = 9; draw_undo_end(&u, 128, 0, 159, 0);
	CHECK(draw_undo_undo(&u, box) && pg_a[4] == 0 && pg_a[3] == 9);
	for (int i = 0; i < WORDS; i++) if (pg_a[i] != (i == 3 ? 9u : 0u)) { CHECK(!"page"); break; }

	// ... the ring keeps all of it
	memset(pg_a, 0, sizeof(pg_a));
	draw_undo_init(&u, pg_a, pg_b, big, sizeof(big));
	draw_undo_begin(&u); pg_a[3] = 9; draw_undo_end(&u, 96, 0, 127, 0);
	draw_undo_begin(&u); pg_a[4] = 9; draw_undo_end(&u, 128, 0, 159, 0);
	draw_undo_spare_lost(&u);
	memset(pg_b, 0x33, sizeof(pg_b));
	CHECK(draw_undo_undo(&u, box) && pg_a[4] == 0);
	draw_undo_begin(&u); pg_a[7] = 1; draw_undo_end(&u, 224, 0, 255, 0);
	CHECK(draw_undo_levels(&u) == 2);
	CHECK(draw_undo_undo(&u, box) && draw_undo_undo(&u, box));
	for (int i = 0; i < WORDS; i++) if (pg_a[i]) { CHECK(!"page"); break; }

	// ... and a load swapped in is a new page: nothing to undo
	draw_undo_clear(&u, pg_b, pg_a);
	CHECK(draw_undo_levels(&u) == 0 && draw_undo_redo_levels(&u) == 0);
	CHECK(draw_undo_at_saved(&u));

}

// -- the paint bucket (drawfill.c) --
//
// Against a plain flood fill with a visited array and a queue as long as
// the page: the region, and the pattern in it.

static uint8_t seen[H][W];
static int16_t qx[W * H], qy[W * H];

static int px(const uint32_t *p, int x, int y) { return (int)((p[y * WPL + (x >> 5)] >> (x & 31)) & 1u); }

static void ref_flood(uint32_t *out, const uint32_t *in, int x, int y, const uint8_t *pat) {
	int t = px(in, x, y), head = 0, tail = 0;
	memcpy(out, in, WORDS * 4);
	memset(seen, 0, sizeof(seen));
	seen[y][x] = 1; qx[tail] = (int16_t)x; qy[tail++] = (int16_t)y;
	while (head < tail) {
		int cx = qx[head], cy = qy[head++];
		uint32_t m = 1u << (cx & 31);
		if (pat_bit(pat, cx, cy)) out[cy * WPL + (cx >> 5)] |= m;
		else out[cy * WPL + (cx >> 5)] &= ~m;
		static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
		for (int k = 0; k < 4; k++) {
			int nx = cx + dx[k], ny = cy + dy[k];
			if ((unsigned)nx >= W || (unsigned)ny >= H || seen[ny][nx]) continue;
			if (px(in, nx, ny) != t) continue;
			seen[ny][nx] = 1; qx[tail] = (int16_t)nx; qy[tail++] = (int16_t)ny;
		}
	}
}

static uint32_t f_in[WORDS], f_page[WORDS], f_want[WORDS];

// Fills (x, y) in steps of `work`, as draw does between messages.
static draw_fill_t fill;
static int fill_run(int x, int y, const uint8_t *pat, int work) {
	memcpy(f_page, f_in, WORDS * 4);
	draw_fill_start(&fill, f_page, f_in, x, y, pat);
	int steps = 0;
	while (draw_fill_step(&fill, work) == DRAW_FILL_MORE) steps++;
	return steps;
}

static void scene(int kind) {
	memset(f_in, 0, sizeof(f_in));
	ref = f_in;
	if (kind == 0) {
		// shapes: outlines, filled boxes, lines
		for (int i = 0; i < 40; i++) {
			int x0 = rr(0, W - 1), y0 = rr(0, H - 1), x1 = rr(0, W - 1), y1 = rr(0, H - 1);
			switch (rr(0, 2)) {
			case 0: ref_rect(x0, y0, x1, y1, 1); break;
			case 1: ref_fill(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, abs(x1 - x0) / 4 + 1,
				abs(y1 - y0) / 4 + 1, pats[rr(0, 2)]); break;
			default: ref_pat = pats[0]; walk(x0, y0, x1, y1, ref_brush); break;
			}
		}
	} else if (kind == 1) {
		// noise: many tiny regions, and some long thin ones
		for (int i = 0; i < WORDS; i++) f_in[i] = rnd() & rnd() & rnd();
	} else {
		// a comb: hundreds of slots, each a separate run to come back to,
		// which a stack of DRAW_FILL_STACK cannot hold at once
		for (int x = 0; x < W; x += 2)
			for (int y = (x % 4 ? 0 : 1); y < H - 1; y++) ref_pset(x, y + (x % 4 ? 1 : 0), 1);
		for (int y = 3; y < H; y += 6)
			for (int x = 1; x < W; x += 2) if ((x / 2 + y) % 5 == 0) ref_pset(x, y, 1);
	}
}

static void test_fill(void) {

	int sweeps = 0, most_steps = 0;

	for (int kind = 0; kind < 3; kind++) {
		for (int trial = 0; trial < 12; trial++) {
			scene(kind);
			int x = rr(0, W - 1), y = rr(0, H - 1);
			const uint8_t *pat = pats[rr(0, 3)];
			ref_flood(f_want, f_in, x, y, pat);
			int work = trial % 3 == 0 ? 1 : trial % 3 == 1 ? 16 : 1000;
			int steps = fill_run(x, y, pat, work);
			if (steps > most_steps) most_steps = steps;
			sweeps += (int)fill.sweeps;
			CHECK(!memcmp(f_page, f_want, sizeof(f_page)));
			// everything it changed is inside its box
			for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++)
				if (px(f_page, xx, yy) != px(f_in, xx, yy))
					CHECK(xx >= fill.x0 && xx <= fill.x1 && yy >= fill.y0 && yy <= fill.y1);
		}
	}
	CHECK(sweeps > 0);		// the comb ran the stack out, and the sweep finished it

	// the whole empty page, one step at a time: each step is short
	memset(f_in, 0, sizeof(f_in));
	ref_flood(f_want, f_in, 5, 5, pats[1]);
	fill_run(5, 5, pats[1], 1);
	CHECK(!memcmp(f_page, f_want, sizeof(f_page)));

	// stopped part way: the page as it was
	scene(2);
	memcpy(f_page, f_in, WORDS * 4);
	draw_fill_start(&fill, f_page, f_in, 3, 3, pats[2]);
	for (int i = 0; i < 50; i++) draw_fill_step(&fill, 4);
	draw_fill_stop(&fill);
	CHECK(!memcmp(f_page, f_in, sizeof(f_page)));

	// off the page: nothing
	CHECK(!draw_fill_start(&fill, f_page, f_in, -1, 3, pats[0]));
	CHECK(draw_fill_step(&fill, 10) == DRAW_FILL_DONE);

	printf("fill: ok (%d sweeps after the stack ran out; at most %d steps)\n", sweeps, most_steps);

}

int main(void) {
	test_stamps();
	test_undo();
	test_fill();
	if (fails) { printf("draw stamps/undo: %d FAILED\n", fails); return 1; }
	printf("draw stamps/undo: ok\n");
	return 0;
}
