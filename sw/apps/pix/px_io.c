/*
 * pix -- the file formats. See px_io.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "px_io.h"

/* -- little helpers -- */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static void fail(char *err, int errlen, const char *msg) {
	if (err && errlen > 0) {
		strncpy(err, msg, (size_t)errlen - 1);
		err[errlen - 1] = 0;
	}
}

/* A growing output buffer. */
typedef struct {
	uint8_t *p;
	uint32_t len, cap;
	bool bad;
} buf_t;

static void put(buf_t *b, const void *src, uint32_t n) {
	if (b->bad) return;
	if (b->len + n > b->cap) {
		uint32_t cap = b->cap ? b->cap : 4096;
		uint8_t *q;
		while (cap < b->len + n) cap *= 2;
		q = realloc(b->p, cap);
		if (!q) { b->bad = true; return; }
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->len, src, n);
	b->len += n;
}

static void put8(buf_t *b, unsigned v) { uint8_t c = (uint8_t)v; put(b, &c, 1); }
static void put16(buf_t *b, uint32_t v) { put8(b, v & 0xff); put8(b, (v >> 8) & 0xff); }
static void put32(buf_t *b, uint32_t v) { put16(b, v & 0xffff); put16(b, v >> 16); }

static uint8_t *done(buf_t *b, uint32_t *len) {
	if (b->bad) { free(b->p); return NULL; }
	if (!b->p) b->p = malloc(1);
	*len = b->len;
	return b->p;
}

/* -- detecting -- */

static bool ends_with(const char *s, const char *tail) {
	size_t a, b;
	if (!s) return false;
	a = strlen(s); b = strlen(tail);
	if (a < b) return false;
	for (; b; a--, b--)
		if (tolower((unsigned char)s[a - 1]) != tail[b - 1]) return false;
	return true;
}

int px_detect(const uint8_t *data, uint32_t len, const char *path) {
	if (len >= 4 && !memcmp(data, "ZIC2", 4)) return PX_ZIC;
	if (len >= 4 && !memcmp(data, "ZFN1", 4)) return PX_ZFN;
	if (len >= 4 && !memcmp(data, "ZBM1", 4)) return PX_ZBM;
	if (len >= 5 && !memcmp(data, "zsp 1", 5)) return PX_ZSP;
	if (ends_with(path, ".zic")) return PX_ZIC;
	if (ends_with(path, ".zfn")) return PX_ZFN;
	if (ends_with(path, ".zbm")) return PX_ZBM;
	if (ends_with(path, ".zsp")) return PX_ZSP;
	return 0;
}

/* ===================================================================
 * .zic -- ZIC2
 * =================================================================== */

#define ZIC_ENTRY 32
#define ZIC_NAME  16
#define ZIC_ICON  32
#define ZIC_BYTES (ZIC_ICON * ZIC_ICON / 8)

/* XOR with the row above, then run-length: c < 0x80 is c+1 literal
 * bytes, c >= 0x80 (c & 0x7f)+1 zero bytes. The same coding as
 * sw/data/icons/gen_dock_icon_data.py, byte for byte. */
static void zic_encode(buf_t *b, const uint8_t *raw) {
	uint8_t x[ZIC_BYTES];
	int i, j;
	for (i = 0; i < ZIC_BYTES; i++)
		x[i] = (uint8_t)(raw[i] ^ (i >= 4 ? raw[i - 4] : 0));
	i = 0;
	while (i < ZIC_BYTES) {
		j = i;
		if (x[i] == 0) {
			while (j < ZIC_BYTES && x[j] == 0 && j - i < 128) j++;
			put8(b, 0x80u | (unsigned)(j - i - 1));
		} else {
			while (j < ZIC_BYTES && x[j] != 0 && j - i < 128) j++;
			put8(b, (unsigned)(j - i - 1));
			put(b, x + i, (uint32_t)(j - i));
		}
		i = j;
	}
}

static bool zic_decode(const uint8_t *blob, uint32_t len, uint8_t *out) {
	uint32_t i = 0;
	int k = 0;
	while (i < len) {
		uint8_t c = blob[i++];
		int n = (c & 0x7f) + 1;
		if (k + n > ZIC_BYTES) return false;
		if (c & 0x80) {
			memset(out + k, 0, (size_t)n);
		} else {
			if (i + (uint32_t)n > len) return false;
			memcpy(out + k, blob + i, (size_t)n);
			i += (uint32_t)n;
		}
		k += n;
	}
	if (k != ZIC_BYTES) return false;
	for (k = 4; k < ZIC_BYTES; k++) out[k] ^= out[k - 4];
	return true;
}

/* Walk a ZIC2 file, calling `each` per entry with its decoded icon. */
typedef bool (*zic_each_fn)(px_doc_t *d, const char *name, const char *spoken,
	uint32_t f1, uint32_t f2, uint16_t flags, const uint8_t *icon);

static bool zic_walk(px_doc_t *d, const uint8_t *data, uint32_t len,
	zic_each_fn each, char *err, int errlen) {

	uint32_t count, esize, spoken_len, icons_len, ents, sp0, ic0, i;

	if (len < 16 || memcmp(data, "ZIC2", 4)) {
		fail(err, errlen, "not a ZIC2 icon file");
		return false;
	}
	count = rd16(data + 4);
	esize = rd16(data + 6);
	spoken_len = rd32(data + 8);
	icons_len = rd32(data + 12);
	if (esize < ZIC_ENTRY) { fail(err, errlen, "bad entry size"); return false; }
	ents = 16;
	sp0 = ents + count * esize;
	ic0 = sp0 + spoken_len;
	if ((uint64_t)ic0 + icons_len > len) { fail(err, errlen, "cut short"); return false; }

	for (i = 0; i < count; i++) {
		const uint8_t *e = data + ents + i * esize;
		char name[ZIC_NAME + 1], spoken[PX_NAME_MAX];
		uint32_t soff = rd16(e + 26), ioff = rd16(e + 28), ilen = rd16(e + 30);
		uint8_t icon[ZIC_BYTES];

		memcpy(name, e, ZIC_NAME);
		name[ZIC_NAME] = 0;
		spoken[0] = 0;
		if (soff < spoken_len) {
			uint32_t k = 0;
			while (soff + k < spoken_len && data[sp0 + soff + k] && k < PX_NAME_MAX - 1) {
				spoken[k] = (char)data[sp0 + soff + k];
				k++;
			}
			spoken[k] = 0;
		}
		if ((uint64_t)ioff + ilen > icons_len || !zic_decode(data + ic0 + ioff, ilen, icon)) {
			fail(err, errlen, "an icon does not decode");
			return false;
		}
		if (!each(d, name, spoken, rd32(e + 16), rd32(e + 20), (uint16_t)rd16(e + 24), icon)) {
			fail(err, errlen, "out of memory");
			return false;
		}
	}
	return true;
}

static void cell_set_meta(px_cell_t *c, const char *spoken, uint32_t f1,
	uint32_t f2, uint16_t flags) {
	strncpy(c->spoken, spoken, PX_NAME_MAX - 1);
	c->spoken[PX_NAME_MAX - 1] = 0;
	c->feat1 = f1;
	c->feat2 = f2;
	c->zic_flags = flags;
	if (flags & 1) c->flags |= PX_CELL_HIDDEN;
}

static bool zic_add(px_doc_t *d, const char *name, const char *spoken,
	uint32_t f1, uint32_t f2, uint16_t flags, const uint8_t *icon) {
	int i = px_cell_insert(d, d->ncells, ZIC_ICON, ZIC_ICON, name);
	if (i < 0) return false;
	memcpy(px_cell_rows(d, i), icon, ZIC_BYTES);
	cell_set_meta(&d->cell[i], spoken, f1, f2, flags);
	return true;
}

static bool zic_over(px_doc_t *d, const char *name, const char *spoken,
	uint32_t f1, uint32_t f2, uint16_t flags, const uint8_t *icon) {
	int i = px_find(d, name);
	if (i < 0) {
		/* an icon for an app the release has none for: insert in name
		 * order, as the generator sorts */
		int at = 0;
		while (at < d->ncells && strcmp(d->cell[at].name, name) < 0) at++;
		i = px_cell_insert(d, at, ZIC_ICON, ZIC_ICON, name);
		if (i < 0) return false;
		cell_set_meta(&d->cell[i], spoken, f1, f2, flags);
	} else if (spoken[0]) {
		/* the overrides file's spoken name replaces the release's; its
		 * hardware needs do not (px_io.h) */
		strncpy(d->cell[i].spoken, spoken, PX_NAME_MAX - 1);
	}
	memcpy(px_cell_rows(d, i), icon, ZIC_BYTES);
	d->cell[i].flags |= PX_CELL_CHANGED;
	return true;
}

static int name_cmp(const void *a, const void *b) {
	return strcmp((*(const px_cell_t *const *)a)->name, (*(const px_cell_t *const *)b)->name);
}

static uint8_t *zic_write(const px_doc_t *d, bool changed_only, uint32_t *len) {
	buf_t hdr = { 0 }, ent = { 0 }, sp = { 0 }, ic = { 0 }, out = { 0 };
	const px_cell_t **order;
	int i, n = 0;
	uint8_t *res;

	order = malloc(sizeof(*order) * (size_t)(d->ncells ? d->ncells : 1));
	if (!order) return NULL;
	for (i = 0; i < d->ncells; i++) {
		const px_cell_t *c = &d->cell[i];
		if (c->w != ZIC_ICON || c->h != ZIC_ICON) continue;
		if (changed_only && !(c->flags & PX_CELL_CHANGED)) continue;
		order[n++] = c;
	}
	qsort(order, (size_t)n, sizeof(*order), name_cmp);

	for (i = 0; i < n; i++) {
		const px_cell_t *c = order[i];
		char name[ZIC_NAME];
		uint32_t soff = sp.len, ioff = ic.len;
		memset(name, 0, sizeof(name));
		strncpy(name, c->name, ZIC_NAME);
		put(&sp, c->spoken, (uint32_t)strlen(c->spoken) + 1);
		zic_encode(&ic, d->bits + c->off);
		put(&ent, name, ZIC_NAME);
		put32(&ent, c->feat1);
		put32(&ent, c->feat2);
		put16(&ent, c->zic_flags);
		put16(&ent, soff);
		put16(&ent, ioff);
		put16(&ent, ic.len - ioff);
	}
	free(order);

	put(&hdr, "ZIC2", 4);
	put16(&hdr, (uint32_t)n);
	put16(&hdr, ZIC_ENTRY);
	put32(&hdr, sp.len);
	put32(&hdr, ic.len);

	put(&out, hdr.p, hdr.len);
	if (ent.len) put(&out, ent.p, ent.len);
	if (sp.len) put(&out, sp.p, sp.len);
	if (ic.len) put(&out, ic.p, ic.len);
	out.bad |= hdr.bad || ent.bad || sp.bad || ic.bad || sp.len > 0xffff || ic.len > 0xffff;
	free(hdr.p); free(ent.p); free(sp.p); free(ic.p);
	res = done(&out, len);
	return res;
}

bool px_zic_overlay(px_doc_t *d, const uint8_t *data, uint32_t len,
	char *err, int errlen) {
	return zic_walk(d, data, len, zic_over, err, errlen);
}

/* ===================================================================
 * .zfn -- ZFN1
 *
 *   0 "ZFN1"; 4 w; 5 h; 6 bytes per row; 7 ascent; 8 u32 count;
 *   12 u16 codepoints ascending, padded to 4; then count*h*stride rows,
 *   MSB-first.
 * =================================================================== */

static bool zfn_read(px_doc_t *d, const uint8_t *data, uint32_t len,
	char *err, int errlen) {

	uint32_t w, h, stride, n, base, i, y, rb;

	if (len < 12) { fail(err, errlen, "too short for a font"); return false; }
	w = data[4]; h = data[5]; stride = data[6]; n = rd32(data + 8);
	d->ascent = data[7];
	if (!w || !h || !stride || stride * 8 < w || !n || n > PX_MAX_CELLS) {
		fail(err, errlen, "not a font this can read");
		return false;
	}
	base = (12 + n * 2 + 3) & ~3u;
	if ((uint64_t)base + (uint64_t)n * h * stride > len) {
		fail(err, errlen, "font cut short");
		return false;
	}
	rb = (w + 7) / 8;
	for (i = 0; i < n; i++) {
		char name[PX_NAME_MAX];
		uint32_t cp = rd16(data + 12 + i * 2);
		int c;
		px_glyph_name(name, cp);
		c = px_cell_insert(d, d->ncells, (int)w, (int)h, name);
		if (c < 0) { fail(err, errlen, "out of memory"); return false; }
		d->cell[c].code = cp;
		for (y = 0; y < h; y++)
			memcpy(px_cell_rows(d, c) + y * rb, data + base + (i * h + y) * stride, rb);
	}
	return true;
}

static uint8_t *zfn_write(const px_doc_t *d, uint32_t *len) {
	buf_t b = { 0 };
	int i, w = 0, h = 0;
	uint32_t rb;

	if (d->ncells) { w = d->cell[0].w; h = d->cell[0].h; }
	rb = (uint32_t)(w + 7) / 8;
	put(&b, "ZFN1", 4);
	put8(&b, (unsigned)w);
	put8(&b, (unsigned)h);
	put8(&b, rb);
	put8(&b, (unsigned)d->ascent);
	put32(&b, (uint32_t)d->ncells);
	for (i = 0; i < d->ncells; i++) put16(&b, d->cell[i].code & 0xffff);
	while (b.len & 3) put8(&b, 0);
	for (i = 0; i < d->ncells; i++)
		put(&b, d->bits + d->cell[i].off, px_cell_bytes(d, w, h));
	return done(&b, len);
}

/* ===================================================================
 * .zbm -- ZBM1: pixel x at bit (x & 31) of little-endian word x >> 5,
 * least significant bit leftmost (zbm.h). In bytes: byte x >> 3 of
 * the row, bit x & 7, LSB first -- the mirror of a cell's MSB-first.
 * =================================================================== */

static uint8_t rev8(uint8_t v) {
	v = (uint8_t)((v >> 4) | (v << 4));
	v = (uint8_t)(((v & 0xcc) >> 2) | ((v & 0x33) << 2));
	return (uint8_t)(((v & 0xaa) >> 1) | ((v & 0x55) << 1));
}

/* A colour ZBM (zbm.h, "colour pictures"): the palette block, then
 * each row's planes in turn. */
static bool zbm_colour_read(px_doc_t *d, const uint8_t *data, uint32_t len, int planes,
	char *err, int errlen) {
	uint32_t w = rd32(data + 4), h = rd32(data + 8), stride, used, y, x;
	const uint8_t *px;
	int c, k;
	if (planes < 2 || planes > 8 || (rd32(data + 12) & ~0x0fu)) {
		fail(err, errlen, "a ZBM kind this cannot read");
		return false;
	}
	if (!zsp_palette_from_bin(&d->pal, data + 16, len - 16, &used) || d->pal.colours > (1 << planes)) {
		fail(err, errlen, "a colour picture with a bad palette");
		return false;
	}
	if (!w || !h || w > PX_MAX_DIM || h > PX_MAX_DIM || w * h > PX_MAX_COLOUR_PX) {
		fail(err, errlen, "picture too big (512K pixels at most in colour)");
		return false;
	}
	stride = (w + 31) / 32 * 4;
	if ((uint64_t)16 + used + (uint64_t)stride * h * (uint32_t)planes > len) {
		fail(err, errlen, "picture cut short");
		return false;
	}
	d->depth = 8;
	c = px_cell_insert(d, 0, (int)w, (int)h, "picture");
	if (c < 0) { fail(err, errlen, "out of memory"); return false; }
	px = data + 16 + used;
	for (y = 0; y < h; y++) {
		uint8_t *row = px_cell_rows(d, c) + y * w;
		for (x = 0; x < w; x++) {
			int v = 0;
			for (k = 0; k < planes; k++)
				v |= ((px[(y * (uint32_t)planes + (uint32_t)k) * stride + (x >> 3)] >> (x & 7)) & 1) << k;
			row[x] = (uint8_t)(v < d->pal.colours ? v : 0);
		}
	}
	return true;
}

static bool zbm_read(px_doc_t *d, const uint8_t *data, uint32_t len,
	char *err, int errlen) {
	uint32_t w, h, stride, y, x, rb;
	int c;
	if (len < 16) { fail(err, errlen, "too short for a ZBM"); return false; }
	w = rd32(data + 4); h = rd32(data + 8);
	if (rd32(data + 12) != 0) return zbm_colour_read(d, data, len, (int)(rd32(data + 12) & 0x0fu), err, errlen);
	if (!w || !h || w > PX_MAX_DIM || h > PX_MAX_DIM) {
		fail(err, errlen, "picture too big (1024 x 1024 at most)");
		return false;
	}
	stride = (w + 31) / 32 * 4;
	if ((uint64_t)16 + (uint64_t)stride * h > len) { fail(err, errlen, "picture cut short"); return false; }
	c = px_cell_insert(d, 0, (int)w, (int)h, "picture");
	if (c < 0) { fail(err, errlen, "out of memory"); return false; }
	rb = (w + 7) / 8;
	for (y = 0; y < h; y++)
		for (x = 0; x < rb; x++)
			px_cell_rows(d, c)[y * rb + x] = rev8(data[16 + y * stride + x]);
	/* the last byte's bits past the width are not pixels */
	if (w & 7)
		for (y = 0; y < h; y++)
			px_cell_rows(d, c)[y * rb + rb - 1] &= (uint8_t)(0xff00u >> (w & 7));
	return true;
}

static uint8_t *zbm_colour_write(const px_doc_t *d, uint32_t *len) {
	buf_t b = { 0 };
	uint32_t w = d->cell[0].w, h = d->cell[0].h, stride = (w + 31) / 32 * 4, y, x, pn;
	int planes = 1, k;
	uint8_t *pal, *row;
	while ((1 << planes) < d->pal.colours) planes++;
	if (planes < 2) planes = 2;
	pn = zsp_palette_bin_size(&d->pal);
	pal = malloc(pn);
	row = malloc(stride);
	if (!pal || !row) { free(pal); free(row); return NULL; }
	zsp_palette_to_bin(&d->pal, pal);
	put(&b, "ZBM1", 4);
	put32(&b, w); put32(&b, h); put32(&b, (uint32_t)planes);
	put(&b, pal, pn);
	for (y = 0; y < h; y++) {
		const uint8_t *src = d->bits + d->cell[0].off + y * w;
		for (k = 0; k < planes; k++) {
			memset(row, 0, stride);
			for (x = 0; x < w; x++)
				if ((src[x] >> k) & 1) row[x >> 3] |= (uint8_t)(1u << (x & 7));
			put(&b, row, stride);
		}
	}
	free(pal);
	free(row);
	return done(&b, len);
}

static uint8_t *zbm_write(const px_doc_t *d, uint32_t *len) {
	buf_t b = { 0 };
	uint32_t w, h, stride, rb, y, x;
	if (!d->ncells) return NULL;
	if (d->depth == 8) return zbm_colour_write(d, len);
	w = d->cell[0].w; h = d->cell[0].h;
	stride = (w + 31) / 32 * 4;
	rb = (w + 7) / 8;
	put(&b, "ZBM1", 4);
	put32(&b, w); put32(&b, h); put32(&b, 0);
	for (y = 0; y < h; y++) {
		const uint8_t *row = d->bits + d->cell[0].off + y * rb;
		for (x = 0; x < stride; x++) put8(&b, x < rb ? rev8(row[x]) : 0);
	}
	return done(&b, len);
}

/* ===================================================================
 * .zsp -- text, through zsprite.c: the sheet's palette is the
 * document's, its sprites the cells (depth 16)
 * =================================================================== */

static bool zsp_read(px_doc_t *d, const uint8_t *data, uint32_t len,
	char *err, int errlen) {

	zsp_sheet_t s;
	int i;

	if (!zsp_parse(&s, (const char *)data, len, err, errlen)) {
		zsp_free(&s);
		return false;
	}
	d->pal = s.p;
	for (i = 0; i < s.n; i++) {
		const zsp_sprite_t *e = &s.spr[i];
		int c = px_cell_insert(d, d->ncells, e->w, e->h, e->name);
		if (c < 0) { zsp_free(&s); fail(err, errlen, "out of memory"); return false; }
		memcpy(px_cell_rows(d, c), e->px, px_cell_bytes(d, e->w, e->h));
	}
	d->extra = s.extra;
	d->extra_len = s.extra_len;
	s.extra = NULL;
	zsp_free(&s);
	d->cursor = d->ncells == 2 && d->pal.colours == 2 &&
		px_find(d, "normal") >= 0 && px_find(d, "busy") >= 0 &&
		d->cell[0].w == 16 && d->cell[0].h == 16 && d->cell[1].w == 16 && d->cell[1].h == 16;
	return true;
}

static uint8_t *zsp_write_doc(const px_doc_t *d, uint32_t *len) {
	zsp_sheet_t s;
	uint8_t *out;
	int i;

	/* the cells lent to the sheet as they are: nothing is copied */
	memset(&s, 0, sizeof(s));
	s.p = d->pal;
	s.n = s.cap = d->ncells;
	s.spr = calloc((size_t)(d->ncells ? d->ncells : 1), sizeof(zsp_sprite_t));
	if (!s.spr) return NULL;
	for (i = 0; i < d->ncells; i++) {
		const px_cell_t *c = &d->cell[i];
		strncpy(s.spr[i].name, c->name, ZSP_NAME - 1);
		s.spr[i].w = c->w;
		s.spr[i].h = c->h;
		s.spr[i].px = (uint16_t *)(void *)(d->bits + c->off);
	}
	s.extra = d->extra;
	s.extra_len = d->extra_len;
	out = (uint8_t *)zsp_write(&s, len);
	free(s.spr);
	return out;
}

/* ===================================================================
 * the front door
 * =================================================================== */

bool px_read(px_doc_t *d, int kind, const uint8_t *data, uint32_t len,
	char *err, int errlen) {
	bool ok;
	px_doc_free(d);
	px_doc_init(d, kind);
	switch (kind) {
	case PX_ZIC: ok = zic_walk(d, data, len, zic_add, err, errlen); break;
	case PX_ZFN: ok = zfn_read(d, data, len, err, errlen); break;
	case PX_ZBM: ok = zbm_read(d, data, len, err, errlen); break;
	case PX_ZSP: ok = zsp_read(d, data, len, err, errlen); break;
	default: fail(err, errlen, "not a kind of file pix reads"); ok = false; break;
	}
	if (!ok) {
		px_doc_free(d);
		px_doc_init(d, kind);
	}
	d->dirty = false;
	return ok;
}

uint8_t *px_write(const px_doc_t *d, bool changed_only, uint32_t *len) {
	switch (d->kind) {
	case PX_ZIC: return zic_write(d, changed_only, len);
	case PX_ZFN: return zfn_write(d, len);
	case PX_ZBM: return zbm_write(d, len);
	case PX_ZSP: return zsp_write_doc(d, len);
	default: return NULL;
	}
}
