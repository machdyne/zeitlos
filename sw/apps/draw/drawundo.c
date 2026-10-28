/*
 * draw -- undo and redo. See drawundo.h.
 */

#include "drawundo.h"

#include <string.h>

#define WPL	DRAW_PAGE_WPL

static draw_undo_rec_t *at(draw_undo_t *u, int i) {
	return &u->rec[(u->first + i) % DRAW_UNDO_LEVELS];
}

static uint32_t rec_words(const draw_undo_rec_t *r) {
	return (uint32_t)(r->wb - r->wa + 1) * (uint32_t)(r->y1 - r->y0 + 1);
}

void draw_undo_init(draw_undo_t *u, uint32_t *page, uint32_t *spare,
	void *mem, uint32_t bytes) {

	memset(u, 0, sizeof(*u));
	u->mem = mem;
	u->cap = mem ? bytes / 4 : 0;
	draw_undo_clear(u, page, spare);

}

void draw_undo_clear(draw_undo_t *u, uint32_t *page, uint32_t *spare) {

	u->page = page;
	u->spare = spare;
	u->first = u->count = u->cursor = 0;
	u->synced = false;
	u->serial = u->base = u->saved = ++u->next;

}

void draw_undo_spare_lost(draw_undo_t *u) {

	// The one-level form's level IS spare. The ring keeps its levels;
	// it only has to copy the page across again at the next press.
	if (!u->mem) u->first = u->count = u->cursor = 0;
	u->synced = false;

}

// Copies the box from one page to the other.
static void copy_box(uint32_t *dst, const uint32_t *src,
	const draw_undo_rec_t *r) {

	int n = (r->wb - r->wa + 1) * 4;
	for (int y = r->y0; y <= r->y1; y++)
		memcpy(dst + y * WPL + r->wa, src + y * WPL + r->wa, (size_t)n);

}

void draw_undo_begin(draw_undo_t *u) {

	if (!u->synced) {
		// after a load, or the first press: the whole page, once
		memcpy(u->spare, u->page, DRAW_PAGE_WORDS * 4);
		u->synced = true;
		if (!u->mem) u->first = u->count = u->cursor = 0;
		return;
	}

	if (u->mem) return;	// spare is the page already

	// One level: spare differs from the page only inside it. Copying
	// that across makes them equal, and gives the level up.
	if (u->count) {
		copy_box(u->spare, u->page, at(u, 0));
		u->count = u->cursor = 0;
		// what can be undone back to is now just this
		u->base = u->serial;
	}

}

// Drops the oldest level. Its state after is now as far back as undo
// goes.
static void drop_oldest(draw_undo_t *u) {

	u->base = at(u, 0)->serial;
	u->first = (u->first + 1) % DRAW_UNDO_LEVELS;
	u->count--;
	u->cursor--;

}

static bool overlaps(const draw_undo_rec_t *r, uint32_t off, uint32_t n) {
	return r->off < off + n && off < r->off + rec_words(r);
}

bool draw_undo_end(draw_undo_t *u, int x0, int y0, int x1, int y1) {

	if (!u->synced) return false;	// no begin(): nothing to compare with

	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= DRAW_PAGE_W) x1 = DRAW_PAGE_W - 1;
	if (y1 >= DRAW_PAGE_H) y1 = DRAW_PAGE_H - 1;
	if (x1 < x0 || y1 < y0) return false;

	// What actually changed, in words: the rectangle the caller gives
	// is padded for the brush, and is the whole drag for a shape.
	int wa = x0 >> 5, wb = x1 >> 5;
	int ca = WPL, cb = -1, ra = DRAW_PAGE_H, rb = -1;

	for (int y = y0; y <= y1; y++) {
		const uint32_t *p = u->page + y * WPL, *s = u->spare + y * WPL;
		for (int w = wa; w <= wb; w++) {
			if (p[w] == s[w]) continue;
			if (w < ca) ca = w;
			if (w > cb) cb = w;
			if (y < ra) ra = y;
			rb = y;
		}
	}

	if (cb < 0) return false;

	draw_undo_rec_t nr;
	nr.wa = (uint16_t)ca; nr.wb = (uint16_t)cb;
	nr.y0 = (uint16_t)ra; nr.y1 = (uint16_t)rb;
	nr.off = 0;
	nr.serial = ++u->next;

	// a new edit: what could have been redone is gone
	u->count = u->cursor;

	if (!u->mem) {
		// spare already holds the page from before: that is the level
		u->first = 0;
		u->rec[0] = nr;
		u->count = u->cursor = 1;
		u->serial = nr.serial;
		return true;
	}

	uint32_t n = rec_words(&nr);

	if (n > u->cap) {
		// Too large to keep. Nothing older can be reached past it.
		copy_box(u->spare, u->page, &nr);
		u->first = u->count = u->cursor = 0;
		u->serial = u->base = nr.serial;
		return false;
	}

	if (u->count == DRAW_UNDO_LEVELS) drop_oldest(u);

	// Where it goes: after the newest level, or back at the start of
	// mem when there is no room above it. Whatever old levels are in
	// the way go -- oldest first, which in a ring is the order they
	// are met in.
	uint32_t off = 0;
	if (u->count) {
		const draw_undo_rec_t *last = at(u, u->count - 1);
		off = last->off + rec_words(last);
		if (off + n > u->cap) off = 0;
	}

	for (;;) {
		bool hit = false;
		for (int i = 0; i < u->count && !hit; i++)
			hit = overlaps(at(u, i), off, n);
		if (!hit) break;
		drop_oldest(u);
		if (!u->count) off = 0;
	}

	nr.off = off;

	// the old words, out of spare; then spare takes the new ones
	int ww = cb - ca + 1;
	uint32_t *d = u->mem + off;
	for (int y = ra; y <= rb; y++, d += ww)
		memcpy(d, u->spare + y * WPL + ca, (size_t)ww * 4);
	copy_box(u->spare, u->page, &nr);

	*at(u, u->count) = nr;
	u->count++;
	u->cursor = u->count;
	u->serial = nr.serial;
	return true;

}

// Swaps the level's words with the page's.
static void swap_level(draw_undo_t *u, const draw_undo_rec_t *r) {

	int ww = r->wb - r->wa + 1;

	// one level: the other copy is spare itself, at the same place
	uint32_t *d = u->mem ? u->mem + r->off : NULL;

	for (int y = r->y0; y <= r->y1; y++) {
		uint32_t *p = u->page + y * WPL + r->wa;
		uint32_t *o = d ? d : u->spare + y * WPL + r->wa;
		for (int i = 0; i < ww; i++) {
			uint32_t t = p[i];
			p[i] = o[i];
			o[i] = t;
		}
		if (d) d += ww;
	}

	// The ring keeps spare equal to the page -- unless a load has had
	// it since, and the next press copies the whole page across anyway.
	if (u->mem && u->synced) copy_box(u->spare, u->page, r);

}

static void box_of(const draw_undo_rec_t *r, int box[4]) {
	box[0] = r->wa * 32;
	box[1] = r->y0;
	box[2] = r->wb * 32 + 31;
	box[3] = r->y1;
}

bool draw_undo_undo(draw_undo_t *u, int box[4]) {

	// (the one-level form's level is in spare: gone if a load had it)
	if (u->cursor <= 0 || (!u->mem && !u->synced)) return false;

	const draw_undo_rec_t *r = at(u, u->cursor - 1);
	swap_level(u, r);
	box_of(r, box);

	u->cursor--;
	u->serial = u->cursor ? at(u, u->cursor - 1)->serial : u->base;
	return true;

}

bool draw_undo_redo(draw_undo_t *u, int box[4]) {

	if (u->cursor >= u->count || (!u->mem && !u->synced)) return false;

	const draw_undo_rec_t *r = at(u, u->cursor);
	swap_level(u, r);
	box_of(r, box);

	u->cursor++;
	u->serial = r->serial;
	return true;

}

int draw_undo_levels(const draw_undo_t *u) {
	return u->cursor;
}

int draw_undo_redo_levels(const draw_undo_t *u) {
	return u->count - u->cursor;
}

void draw_undo_mark_saved(draw_undo_t *u) {
	u->saved = u->serial;
}

bool draw_undo_at_saved(const draw_undo_t *u) {
	return u->serial == u->saved;
}
