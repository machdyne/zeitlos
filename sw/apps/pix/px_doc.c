/*
 * pix -- the document. See px_doc.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Cells and pixels are kept apart: the cell table is in display order
 * (and, for a font, codepoint order), the pixels in one pool in the
 * order they were made. Inserting a cell moves table entries, never
 * pixels; deleting one closes its gap in the pool and moves the
 * offsets after it down, so the pool never fragments.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "px_doc.h"

void px_doc_init(px_doc_t *d, int kind) {
	memset(d, 0, sizeof(*d));
	d->kind = kind;
	d->depth = kind == PX_ZSP ? 16 : 1;
	zsp_palette_init(&d->pal, 2);
}

/* A cell's pixels all cleared: transparent, or 0. */
static void clear_px(px_doc_t *d, uint32_t off, uint32_t n) {
	if (d->depth == 16) {
		uint16_t *p = (uint16_t *)(void *)(d->bits + off);
		uint32_t k;
		for (k = 0; k < n / 2; k++) p[k] = ZSP_T;
	} else {
		memset(d->bits + off, 0, n);
	}
}

void px_doc_free(px_doc_t *d) {
	free(d->cell);
	free(d->bits);
	free(d->extra);
	px_doc_init(d, 0);
}

static bool grow_cells(px_doc_t *d, int need) {
	px_cell_t *n;
	int cap;
	if (need <= d->cap) return true;
	if (need > PX_MAX_CELLS) return false;
	cap = d->cap ? d->cap * 2 : 32;
	while (cap < need) cap *= 2;
	if (cap > PX_MAX_CELLS) cap = PX_MAX_CELLS;
	n = realloc(d->cell, (size_t)cap * sizeof(*n));
	if (!n) return false;
	d->cell = n;
	d->cap = cap;
	return true;
}

static bool grow_bits(px_doc_t *d, uint32_t need) {
	uint8_t *n;
	uint32_t cap;
	if (need <= d->bits_cap) return true;
	cap = d->bits_cap ? d->bits_cap : 1024;
	while (cap < need) cap *= 2;
	n = realloc(d->bits, cap);
	if (!n) return false;
	d->bits = n;
	d->bits_cap = cap;
	return true;
}

int px_cell_insert(px_doc_t *d, int at, int w, int h, const char *name) {
	uint32_t n = px_cell_bytes(d, w, h);
	px_cell_t *c;

	if (w < 1 || h < 1 || w > PX_MAX_DIM || h > PX_MAX_DIM) return -1;
	if (at < 0 || at > d->ncells) at = d->ncells;
	if (!grow_cells(d, d->ncells + 1)) return -1;
	if (!grow_bits(d, d->bits_len + n)) return -1;

	memmove(&d->cell[at + 1], &d->cell[at],
		(size_t)(d->ncells - at) * sizeof(px_cell_t));
	c = &d->cell[at];
	memset(c, 0, sizeof(*c));
	if (name) {
		strncpy(c->name, name, PX_NAME_MAX - 1);
		c->name[PX_NAME_MAX - 1] = 0;
	}
	c->w = (uint16_t)w;
	c->h = (uint16_t)h;
	c->off = d->bits_len;
	clear_px(d, d->bits_len, n);
	d->bits_len += n;
	d->ncells++;
	return at;
}

int px_cell_dup(px_doc_t *d, int i, const char *name) {
	int j;
	px_cell_t keep;
	if (i < 0 || i >= d->ncells) return -1;
	keep = d->cell[i];
	j = px_cell_insert(d, i + 1, keep.w, keep.h, name);
	if (j < 0) return -1;
	/* the pool may have moved: copy through offsets, not pointers */
	memcpy(d->bits + d->cell[j].off, d->bits + d->cell[i].off,
		px_cell_bytes(d, keep.w, keep.h));
	strcpy(d->cell[j].spoken, keep.spoken);
	d->cell[j].code = keep.code;
	d->cell[j].flags = PX_CELL_CHANGED;
	d->dirty = true;
	return j;
}

void px_cell_delete(px_doc_t *d, int i) {
	uint32_t off, n;
	int k;
	if (i < 0 || i >= d->ncells) return;
	off = d->cell[i].off;
	n = px_cell_bytes(d, d->cell[i].w, d->cell[i].h);
	memmove(d->bits + off, d->bits + off + n, d->bits_len - off - n);
	d->bits_len -= n;
	memmove(&d->cell[i], &d->cell[i + 1],
		(size_t)(d->ncells - i - 1) * sizeof(px_cell_t));
	d->ncells--;
	for (k = 0; k < d->ncells; k++)
		if (d->cell[k].off > off) d->cell[k].off -= n;
	d->dirty = true;
}

bool px_cell_resize(px_doc_t *d, int i, int w, int h) {
	px_cell_t *c;
	uint32_t n, old_off, old_n, rb_old, rb_new;
	int y, k, ow, oh;
	if (i < 0 || i >= d->ncells) return false;
	if (w < 1 || h < 1 || w > PX_MAX_DIM || h > PX_MAX_DIM) return false;
	n = px_cell_bytes(d, w, h);
	if (!grow_bits(d, d->bits_len + n)) return false;
	c = &d->cell[i];
	ow = c->w; oh = c->h;
	old_off = c->off;
	old_n = px_cell_bytes(d, ow, oh);
	if (d->depth == 16) { rb_old = (uint32_t)ow * 2; rb_new = (uint32_t)w * 2; }
	else if (d->depth == 8) { rb_old = (uint32_t)ow; rb_new = (uint32_t)w; }
	else { rb_old = (uint32_t)(ow + 7) / 8; rb_new = (uint32_t)(w + 7) / 8; }
	/* the new pixels at the end of the pool, then the old gap closed */
	clear_px(d, d->bits_len, n);
	for (y = 0; y < h && y < oh; y++) {
		uint32_t m = rb_new < rb_old ? rb_new : rb_old;
		memcpy(d->bits + d->bits_len + (uint32_t)y * rb_new, d->bits + old_off + (uint32_t)y * rb_old, m);
		if (d->depth == 1 && w < ow && (w & 7))
			d->bits[d->bits_len + (uint32_t)y * rb_new + rb_new - 1] &= (uint8_t)(0xff00u >> (w & 7));
	}
	c->off = d->bits_len;
	c->w = (uint16_t)w;
	c->h = (uint16_t)h;
	d->bits_len += n;
	memmove(d->bits + old_off, d->bits + old_off + old_n, d->bits_len - old_off - old_n);
	d->bits_len -= old_n;
	for (k = 0; k < d->ncells; k++)
		if (d->cell[k].off > old_off) d->cell[k].off -= old_n;
	return true;
}

int px_glyph_slot(const px_doc_t *d, uint32_t code, bool *exists) {
	int lo = 0, hi = d->ncells;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (d->cell[mid].code < code) lo = mid + 1;
		else hi = mid;
	}
	if (exists) *exists = lo < d->ncells && d->cell[lo].code == code;
	return lo;
}

void px_glyph_name(char *out, uint32_t code) {
	if (code > 0x20 && code < 0x7f && code != '\'')
		snprintf(out, PX_NAME_MAX, "'%c' U+%04X", (int)code, (unsigned)code);
	else
		snprintf(out, PX_NAME_MAX, "U+%04X", (unsigned)code);
}

int px_find(const px_doc_t *d, const char *name) {
	int i;
	for (i = 0; i < d->ncells; i++)
		if (!strcmp(d->cell[i].name, name)) return i;
	return -1;
}
