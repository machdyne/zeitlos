/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ZIP reading and (stored) writing. See zf_zip.h.
 */

#include <string.h>

#include "zf_zip.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool read_at(zf_io_t *io, uint32_t pos, void *buf, uint32_t n) {
	if (!io->seek(io, pos)) return false;
	uint8_t *b = (uint8_t *)buf;
	while (n) {
		int32_t r = io->read(io, b, n);
		if (r <= 0) return false;
		b += r;
		n -= (uint32_t)r;
	}
	return true;
}

// -- reading --

int zz_open(zz_t *z, zf_io_t *io) {
	memset(z, 0, sizeof(*z));
	z->io = io;
	if (io->size < 22) return ZF_E_FORMAT;

	// The end-of-central-directory record is the last 22 bytes, unless
	// the archive has a comment (up to 64KB) after it: search back.
	uint8_t buf[1024];
	uint32_t lim = io->size > 65557 ? io->size - 65557 : 0;
	uint32_t end = io->size;
	while (end > lim) {
		uint32_t start = end > sizeof(buf) ? end - sizeof(buf) : 0;
		if (start < lim) start = lim;
		uint32_t n = end - start;
		if (!read_at(io, start, buf, n)) return ZF_E_IO;
		for (int i = (int)n - 22; i >= 0; i--) {
			if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6) {
				uint8_t *e = &buf[i];
				if (rd16(e + 4) || rd16(e + 6)) return ZF_E_UNSUPP;	// split archives
				z->count = rd16(e + 10);
				z->cd_size = rd32(e + 12);
				z->cd_off = rd32(e + 16);
				if (z->cd_off == 0xFFFFFFFFu || z->count == 0xFFFF) return ZF_E_UNSUPP;	// ZIP64
				if ((uint64_t)z->cd_off + z->cd_size > start + (uint32_t)i) return ZF_E_CORRUPT;
				z->at = z->cd_off;
				return ZF_OK;
			}
		}
		if (start == lim) break;
		end = start + 21;		// overlap so a record across the boundary is seen
	}
	return ZF_E_FORMAT;
}

void zz_rewind(zz_t *z) { z->at = z->cd_off; z->i = 0; }

int zz_next(zz_t *z, zz_entry_t *e) {
	if (z->i >= z->count) return 0;
	uint8_t h[46];
	if (z->at + 46 > z->cd_off + z->cd_size) return ZF_E_CORRUPT;
	if (!read_at(z->io, z->at, h, 46)) return ZF_E_IO;
	if (rd32(h) != 0x02014b50) return ZF_E_CORRUPT;
	uint16_t nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
	memset(e, 0, sizeof(*e));
	e->flags = rd16(h + 8);
	e->method = rd16(h + 10);
	e->crc = rd32(h + 16);
	e->csize = rd32(h + 20);
	e->usize = rd32(h + 24);
	e->lho = rd32(h + 42);
	uint16_t take = nl < ZF_PATH_MAX - 1 ? nl : ZF_PATH_MAX - 1;
	if (!read_at(z->io, z->at + 46, e->name, take)) return ZF_E_IO;
	e->name[take] = 0;
	z->at += 46u + nl + xl + cl;
	z->i++;
	return 1;
}

int zz_find(zz_t *z, const char *name, zz_entry_t *e) {
	zz_rewind(z);
	int r;
	while ((r = zz_next(z, e)) == 1)
		if (!strcmp(e->name, name)) return 1;
	return r;
}

// One window, for whichever entry is being inflated.
static uint8_t zz_window[Z_INFLATE_WINDOW];
static bool zz_window_busy;

static int32_t entry_read(zf_src_t *s, uint8_t *buf, uint32_t n) {
	zz_rd_t *r = (zz_rd_t *)s->ctx;
	if (r->err) return r->err;
	if (r->done) return 0;
	zf_io_t *io = r->z->io;
	uint32_t got = 0;

	while (got < n) {
		// more compressed input
		if (r->ipos >= r->ilen && r->in_left) {
			uint32_t k = r->in_left < sizeof(r->inbuf) ? r->in_left : (uint32_t)sizeof(r->inbuf);
			if (!read_at(io, r->foff, r->inbuf, k)) { r->err = ZF_E_IO; return r->err; }
			r->foff += k;
			r->in_left -= k;
			r->ipos = 0;
			r->ilen = k;
		}
		if (r->e.method == 0) {
			uint32_t k = r->ilen - r->ipos;
			if (!k) break;
			if (k > n - got) k = n - got;
			memcpy(buf + got, r->inbuf + r->ipos, k);
			r->ipos += k;
			got += k;
		} else {
			uint32_t il = r->ilen - r->ipos, ol = n - got;
			int rv = z_inflate(&r->inf, r->inbuf + r->ipos, &il, buf + got, &ol);
			r->ipos += il;
			got += ol;
			if (rv == Z_INFLATE_DONE) { r->done = true; break; }
			if (rv == Z_INFLATE_E_LIMIT) { r->err = ZF_E_LIMIT; return r->err; }
			if (rv < 0) { r->err = ZF_E_CORRUPT; return r->err; }
			// No progress with input in hand, or none left to give: a
			// stream that ended early, or one that would loop forever.
			if (!il && !ol && (r->ipos < r->ilen || !r->in_left)) {
				r->err = ZF_E_CORRUPT;
				return r->err;
			}
		}
		if (r->e.method == 0 && r->ipos >= r->ilen && !r->in_left) { r->done = true; break; }
	}

	r->crc = zf_crc32(r->crc, buf, got);
	r->out += got;
	if (r->out > ZF_ENTRY_MAX) { r->err = ZF_E_LIMIT; return r->err; }
	if (r->done) {
		if (r->out != r->e.usize || r->crc != r->e.crc) { r->err = ZF_E_CORRUPT; return r->err; }
	}
	if (zf_progress && r->e.usize)
		zf_progress((uint32_t)((uint64_t)r->out * 1000u / r->e.usize));
	return (int32_t)got;
}

int zz_entry_src(zz_t *z, const zz_entry_t *e, zz_rd_t *r, zf_src_t *src) {
	memset(r, 0, sizeof(*r));
	r->z = z;
	r->e = *e;
	if (e->flags & 1) return ZF_E_UNSUPP;				// encrypted
	if (e->method != 0 && e->method != 8) return ZF_E_UNSUPP;
	if (e->usize > ZF_ENTRY_MAX) return ZF_E_LIMIT;
	uint8_t h[30];
	if (!read_at(z->io, e->lho, h, 30)) return ZF_E_IO;
	if (rd32(h) != 0x04034b50) return ZF_E_CORRUPT;
	r->foff = e->lho + 30u + rd16(h + 26) + rd16(h + 28);
	if ((uint64_t)r->foff + e->csize > z->io->size) return ZF_E_CORRUPT;
	r->in_left = e->csize;
	if (e->method == 8) {
		if (zz_window_busy) { r->e.method = 0; return ZF_E_NOMEM; }
		zz_window_busy = true;
		z_inflate_init(&r->inf, zz_window, Z_INFLATE_RAW, ZF_ENTRY_MAX);
	} else if (e->csize != e->usize) return ZF_E_CORRUPT;
	if (e->usize == 0 && e->method == 0) r->done = true;
	src->ctx = r;
	src->read = entry_read;
	return ZF_OK;
}

void zz_entry_close(zz_rd_t *r) {
	if (r->e.method == 8) zz_window_busy = false;
	r->e.method = 0;
}

uint8_t *zz_window_take(void) {
	if (zz_window_busy) return NULL;
	zz_window_busy = true;
	return zz_window;
}

void zz_window_give(void) { zz_window_busy = false; }

zz_ent_t zz_ent;

int zz_ent_open(zz_t *z, const char *name) {
	zz_entry_t e;
	if (zz_ent.open) zz_ent_close();
	int r = zz_find(z, name, &e);
	if (r < 0) return r;
	if (!r) return ZF_E_FORMAT;
	r = zz_entry_src(z, &e, &zz_ent.zr, &zz_ent.src);
	if (r < 0) return r;
	zf_rd_init(&zz_ent.rd, &zz_ent.src);
	zx_init(&zz_ent.x, &zz_ent.rd);
	zz_ent.open = true;
	return ZF_OK;
}

int zz_ent_close(void) {
	int err = zz_ent.rd.err ? zz_ent.rd.err : zz_ent.zr.err;
	if (zz_ent.open) zz_entry_close(&zz_ent.zr);
	zz_ent.open = false;
	return err;
}

// -- writing --

static const uint16_t zw_date = ((2026 - 1980) << 9) | (1 << 5) | 1;

void zw_init(zw_t *w, zf_io_t *io) {
	memset(w, 0, sizeof(*w));
	w->io = io;
}

int zw_begin(zw_t *w, const char *name) {
	if (w->n >= ZW_MAX_ENTRIES || w->open) return ZF_E_LIMIT;
	size_t nl = strlen(name);
	if (nl >= sizeof(w->ent[0].name)) return ZF_E_LIMIT;
	memcpy(w->ent[w->n].name, name, nl + 1);
	w->ent[w->n].off = w->io->written;
	w->ent[w->n].crc = 0;
	w->ent[w->n].size = 0;
	uint8_t h[30];
	memset(h, 0, sizeof h);
	wr32(h, 0x04034b50);
	wr16(h + 4, 10);			// version needed: 1.0, stored
	wr16(h + 6, 0x0800);		// names are UTF-8
	wr16(h + 12, zw_date);
	wr16(h + 26, (uint16_t)nl);
	int32_t r = zf_write(w->io, h, 30);
	if (r < 0) return r;
	r = zf_write(w->io, name, (uint32_t)nl);
	if (r < 0) return r;
	w->open = true;
	return ZF_OK;
}

int zw_write(zw_t *w, const void *buf, uint32_t n) {
	if (!w->open) return ZF_E_IO;
	int32_t r = zf_write(w->io, buf, n);
	if (r < 0) return r;
	w->ent[w->n].crc = zf_crc32(w->ent[w->n].crc, buf, n);
	w->ent[w->n].size += n;
	return ZF_OK;
}

int zw_puts(zw_t *w, const char *s) { return zw_write(w, s, (uint32_t)strlen(s)); }

int zw_end(zw_t *w) {
	if (!w->open) return ZF_E_IO;
	uint8_t p[12];
	wr32(p, w->ent[w->n].crc);
	wr32(p + 4, w->ent[w->n].size);
	wr32(p + 8, w->ent[w->n].size);
	uint32_t here = w->io->written;
	// patch the local header's CRC and sizes, then carry on at the end
	if (!w->io->seek(w->io, w->ent[w->n].off + 14)) return ZF_E_IO;
	if (w->io->write(w->io, p, 12) != 12) return ZF_E_IO;
	if (!w->io->seek(w->io, here)) return ZF_E_IO;
	w->open = false;
	w->n++;
	return ZF_OK;
}

int zw_file(zw_t *w, const char *name, const char *content) {
	int r = zw_begin(w, name);
	if (r) return r;
	r = zw_puts(w, content);
	if (r) return r;
	return zw_end(w);
}

int zw_finish(zw_t *w) {
	uint32_t cd = w->io->written;
	for (int i = 0; i < w->n; i++) {
		uint8_t h[46];
		size_t nl = strlen(w->ent[i].name);
		memset(h, 0, sizeof h);
		wr32(h, 0x02014b50);
		wr16(h + 4, 0x031E);		// made by: Unix, 3.0
		wr16(h + 6, 10);
		wr16(h + 8, 0x0800);
		wr16(h + 14, zw_date);
		wr32(h + 16, w->ent[i].crc);
		wr32(h + 20, w->ent[i].size);
		wr32(h + 24, w->ent[i].size);
		wr16(h + 28, (uint16_t)nl);
		wr32(h + 38, 0644u << 16);	// external attributes: rw-r--r--
		wr32(h + 42, w->ent[i].off);
		int32_t r = zf_write(w->io, h, 46);
		if (r < 0) return r;
		r = zf_write(w->io, w->ent[i].name, (uint32_t)nl);
		if (r < 0) return r;
	}
	uint8_t e[22];
	memset(e, 0, sizeof e);
	wr32(e, 0x06054b50);
	wr16(e + 8, (uint16_t)w->n);
	wr16(e + 10, (uint16_t)w->n);
	wr32(e + 12, w->io->written - cd);
	wr32(e + 16, cd);
	int32_t r = zf_write(w->io, e, 22);
	return r < 0 ? r : ZF_OK;
}
