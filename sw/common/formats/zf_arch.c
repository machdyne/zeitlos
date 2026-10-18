/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Extracting archives: zip, tar, gzip and tar.gz.
 *
 * Every name in an archive is a path chosen by whoever made it, so
 * each is checked before anything is created: an absolute path, a
 * drive letter, or a ".." that climbs above the target is refused
 * (ZF_E_PATH) -- not skipped, since a file that tries is not one to
 * half-extract. Backslashes (zips made on Windows) are separators.
 * Links, devices and fifos in a tar are skipped. Sizes are capped by
 * ZF_ENTRY_MAX per file and ZF_OUT_MAX in all.
 */

#include <string.h>

#include "zf.h"
#include "zf_zip.h"

// `name` as a safe relative path in `out`, or false.
static bool safe_path(const char *name, char *out, int cap) {
	int o = 0, depth = 0;
	const char *p = name;
	if (*p == '/' || *p == '\\') return false;
	if (((*p | 32) >= 'a' && (*p | 32) <= 'z') && p[1] == ':') return false;
	while (*p) {
		const char *s = p;
		while (*s && *s != '/' && *s != '\\') s++;
		int len = (int)(s - p);
		if (len == 0 || (len == 1 && p[0] == '.')) { /* skip */ }
		else if (len == 2 && p[0] == '.' && p[1] == '.') {
			if (!depth) return false;
			depth--;
			if (o) o--;
			while (o > 0 && out[o - 1] != '/') o--;
		} else {
			for (int i = 0; i < len; i++) if ((unsigned char)p[i] < 0x20) return false;
			if (o + len + 2 > cap) return false;
			if (o) out[o++] = '/';
			memcpy(out + o, p, (size_t)len);
			o += len;
			depth++;
		}
		p = *s ? s + 1 : s;
	}
	out[o] = 0;
	return true;
}

// Makes each directory on the way to `path` (the last part is a file
// unless `is_dir`).
static bool make_dirs(zf_dir_t *d, const char *path, bool is_dir) {
	char tmp[ZF_PATH_MAX];
	int n = (int)strlen(path);
	if (n >= ZF_PATH_MAX) return false;
	for (int i = 0; i <= n; i++) {
		if (path[i] == '/' || (i == n && is_dir)) {
			memcpy(tmp, path, (size_t)i);
			tmp[i] = 0;
			if (i && !d->mkdir(d, tmp)) return false;
		}
	}
	return true;
}

static bool cancelled(void) {
	return zf_cancel_poll && zf_cancel_poll();
}

// Copies a source into a new file at `rel`.
static int copy_out(zf_dir_t *d, const char *rel, zf_src_t *src, uint32_t limit, uint32_t *total) {
	static uint8_t buf[4096];
	zf_io_t out;
	memset(&out, 0, sizeof out);
	if (!make_dirs(d, rel, false)) return ZF_E_IO;
	if (!d->open(d, rel, &out)) return ZF_E_IO;
	int r = ZF_OK;
	uint32_t n = 0;
	for (;;) {
		if (cancelled()) { r = ZF_E_CANCEL; break; }
		uint32_t want = sizeof buf;
		if (limit != 0xFFFFFFFFu && limit - n < want) want = limit - n;
		if (!want) break;
		int32_t k = src->read(src, buf, want);
		if (k < 0) { r = k; break; }
		if (!k) break;
		n += (uint32_t)k;
		*total += (uint32_t)k;
		if (n > ZF_ENTRY_MAX || *total > ZF_OUT_MAX) { r = ZF_E_LIMIT; break; }
		if (zf_write(&out, buf, (uint32_t)k) < 0) { r = ZF_E_IO; break; }
	}
	d->close(d, &out);
	if (r < 0 && d->remove) d->remove(d, rel);
	return r;
}

// -- zip --

int zf_extract_zip(zf_io_t *in, zf_dir_t *d) {
	static zz_t z;
	static zz_entry_t e;
	static zz_rd_t rd;
	static zf_src_t src;
	static char rel[ZF_PATH_MAX];
	int r = zz_open(&z, in);
	if (r < 0) return r;
	uint32_t total = 0, i = 0;
	// every path checked before anything is written
	while ((r = zz_next(&z, &e)) == 1)
		if (!safe_path(e.name, rel, sizeof rel)) return ZF_E_PATH;
	if (r < 0) return r;
	zz_rewind(&z);
	while ((r = zz_next(&z, &e)) == 1) {
		safe_path(e.name, rel, sizeof rel);
		int nl = (int)strlen(e.name);
		if (!rel[0]) continue;
		if (nl && (e.name[nl - 1] == '/' || e.name[nl - 1] == '\\')) {
			if (!make_dirs(d, rel, true)) return ZF_E_IO;
			continue;
		}
		if ((r = zz_entry_src(&z, &e, &rd, &src)) < 0) return r;
		r = copy_out(d, rel, &src, 0xFFFFFFFFu, &total);
		zz_entry_close(&rd);
		if (r < 0) return r;
		i++;
		if (zf_progress && z.count) zf_progress(i * 1000 / z.count);
	}
	return r < 0 ? r : ZF_OK;
}

// -- tar --

static uint32_t octal(const uint8_t *p, int n, bool *ok) {
	uint32_t v = 0;
	int i = 0;
	while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
	if (i < n && (p[i] & 0x80)) { *ok = false; return 0; }		// base-256: > 8GB
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
		if (v > 0x1FFFFFFF) { *ok = false; return 0; }
		v = v * 8 + (uint32_t)(p[i] - '0');
	}
	return v;
}

// Reads exactly n bytes, or fails.
static int read_full(zf_src_t *s, uint8_t *buf, uint32_t n) {
	uint32_t got = 0;
	while (got < n) {
		int32_t k = s->read(s, buf + got, n - got);
		if (k < 0) return k;
		if (!k) return ZF_E_CORRUPT;
		got += (uint32_t)k;
	}
	return ZF_OK;
}

static int skip_bytes(zf_src_t *s, uint32_t n) {
	static uint8_t junk[512];
	while (n) {
		if (cancelled()) return ZF_E_CANCEL;
		uint32_t k = n < sizeof junk ? n : (uint32_t)sizeof junk;
		int r = read_full(s, junk, k);
		if (r < 0) return r;
		n -= k;
	}
	return ZF_OK;
}

// A source that stops after `left` bytes of another.
typedef struct { zf_src_t *in; uint32_t left; } lim_t;
static int32_t lim_read(zf_src_t *s, uint8_t *buf, uint32_t n) {
	lim_t *l = (lim_t *)s->ctx;
	if (!l->left) return 0;
	if (n > l->left) n = l->left;
	int32_t k = l->in->read(l->in, buf, n);
	if (k == 0) return ZF_E_CORRUPT;		// the archive ended inside a file
	if (k > 0) l->left -= (uint32_t)k;
	return k;
}

int zf_extract_tar(zf_src_t *in, zf_dir_t *d) {
	static uint8_t h[512];
	static char name[ZF_PATH_MAX], longname[ZF_PATH_MAX], rel[ZF_PATH_MAX];
	uint32_t total = 0;
	int zeros = 0;
	bool have_long = false;
	for (;;) {
		if (cancelled()) return ZF_E_CANCEL;
		int32_t k = 0;
		uint32_t got = 0;
		while (got < 512 && (k = in->read(in, h + got, 512 - got)) > 0) got += (uint32_t)k;
		if (k < 0) return k;
		if (got == 0) break;					// no end marker: accepted
		if (got < 512) return ZF_E_CORRUPT;
		bool zero = true;
		for (int i = 0; i < 512 && zero; i++) if (h[i]) zero = false;
		if (zero) { if (++zeros == 2) break; continue; }
		zeros = 0;
		// checksum: the header with its own field as spaces
		bool ok = true;
		uint32_t want = octal(h + 148, 8, &ok), sum = 0;
		for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
		if (!ok || sum != want) return ZF_E_CORRUPT;
		uint32_t size = octal(h + 124, 12, &ok);
		if (!ok) return ZF_E_UNSUPP;
		uint32_t pad = (512 - (size & 511)) & 511;
		char type = (char)h[156];

		if (type == 'L' || type == 'x') {
			// GNU long name, or a pax header that may hold path=
			int r;
			if (size >= ZF_PATH_MAX * 4) { if ((r = skip_bytes(in, size + pad)) < 0) return r; continue; }
			static uint8_t data[ZF_PATH_MAX * 4];
			if ((r = read_full(in, data, size)) < 0) return r;
			if ((r = skip_bytes(in, pad)) < 0) return r;
			data[size < sizeof data ? size : sizeof data - 1] = 0;
			if (type == 'L') { memcpy(longname, data, ZF_PATH_MAX - 1); longname[ZF_PATH_MAX - 1] = 0; have_long = true; }
			else {
				// records "len key=value\n"
				char *p = (char *)data, *end = (char *)data + size;
				while (p < end) {
					char *sp = memchr(p, ' ', (size_t)(end - p));
					if (!sp) break;
					uint32_t len = 0;
					for (char *q = p; q < sp; q++) len = len * 10 + (uint32_t)(*q - '0');
					if (!len || p + len > end) break;
					if (!strncmp(sp + 1, "path=", 5)) {
						uint32_t vl = (uint32_t)(p + len - 1 - (sp + 6));
						if (vl < ZF_PATH_MAX) { memcpy(longname, sp + 6, vl); longname[vl] = 0; have_long = true; }
					}
					p += len;
				}
			}
			continue;
		}
		if (type == 'g') { int r = skip_bytes(in, size + pad); if (r < 0) return r; continue; }

		if (have_long) { memcpy(name, longname, sizeof name); have_long = false; }
		else {
			int o = 0;
			if (!memcmp(h + 257, "ustar", 5) && h[345]) {
				for (int i = 0; i < 155 && h[345 + i]; i++) name[o++] = (char)h[345 + i];
				name[o++] = '/';
			}
			for (int i = 0; i < 100 && h[i]; i++) name[o++] = (char)h[i];
			name[o] = 0;
		}
		if (!safe_path(name, rel, sizeof rel)) return ZF_E_PATH;

		int r = ZF_OK;
		if (type == '5') {
			if (rel[0] && !make_dirs(d, rel, true)) return ZF_E_IO;
			r = skip_bytes(in, size + pad);
		} else if ((type == '0' || type == 0 || type == '7') && rel[0]) {
			static lim_t lim;
			static zf_src_t ls;
			lim.in = in;
			lim.left = size;
			ls.ctx = &lim;
			ls.read = lim_read;
			r = copy_out(d, rel, &ls, size, &total);
			if (r >= 0 && lim.left) r = ZF_E_CORRUPT;
			if (r >= 0) r = skip_bytes(in, pad);
		} else {
			r = skip_bytes(in, size + pad);		// links, devices: not made
		}
		if (r < 0) return r;
	}
	return ZF_OK;
}

// -- gzip --

typedef struct {
	zf_io_t		*in;
	z_inflate_t	inf;
	uint8_t		ibuf[2048];
	uint32_t	ipos, ilen;
	bool		in_eof, done;
	uint32_t	crc, size;
	int			err;
} gz_t;

static gz_t G;

static int32_t gz_read(zf_src_t *s, uint8_t *buf, uint32_t n) {
	gz_t *g = (gz_t *)s->ctx;
	if (g->err) return g->err;
	if (g->done) return 0;
	uint32_t got = 0;
	while (got < n && !g->done) {
		if (g->ipos == g->ilen && !g->in_eof) {
			int32_t k = g->in->read(g->in, g->ibuf, sizeof g->ibuf);
			if (k < 0) return g->err = ZF_E_IO;
			if (!k) g->in_eof = true;
			g->ilen = (uint32_t)k;
			g->ipos = 0;
			if (zf_progress && g->in->size) zf_progress((uint32_t)((uint64_t)g->in->pos * 1000u / g->in->size));
		}
		uint32_t il = g->ilen - g->ipos, ol = n - got;
		int rv = z_inflate(&g->inf, g->ibuf + g->ipos, &il, buf + got, &ol);
		g->ipos += il;
		g->crc = zf_crc32(g->crc, buf + got, ol);
		g->size += ol;
		got += ol;
		if (rv == Z_INFLATE_DONE) {
			const uint8_t *t = g->inf.hdr;
			uint32_t crc = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
			uint32_t sz = (uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
			if (crc != g->crc || sz != g->size) return g->err = ZF_E_CORRUPT;
			g->done = true;
			break;
		}
		if (rv == Z_INFLATE_E_LIMIT) return g->err = ZF_E_LIMIT;
		if (rv < 0) return g->err = ZF_E_CORRUPT;
		if (!il && !ol && (g->ipos < g->ilen || g->in_eof)) return g->err = ZF_E_CORRUPT;
	}
	return (int32_t)got;
}

static int gz_open(zf_io_t *in, zf_src_t *src) {
	memset(&G, 0, sizeof G);
	uint8_t *w = zz_window_take();
	if (!w) return ZF_E_NOMEM;
	G.in = in;
	z_inflate_init(&G.inf, w, Z_INFLATE_GZIP, ZF_OUT_MAX);
	G.inf.gzip_fields = true;
	src->ctx = &G;
	src->read = gz_read;
	return ZF_OK;
}

int zf_extract_gz(zf_io_t *in, zf_io_t *out) {
	static zf_src_t src;
	static uint8_t buf[4096];
	int r = gz_open(in, &src);
	if (r < 0) return r;
	for (;;) {
		int32_t k = src.read(&src, buf, sizeof buf);
		if (k < 0) { r = k; break; }
		if (!k) break;
		if (zf_write(out, buf, (uint32_t)k) < 0) { r = out->written + (uint32_t)k > ZF_OUT_MAX ? ZF_E_LIMIT : ZF_E_IO; break; }
		if (cancelled()) { r = ZF_E_CANCEL; break; }
	}
	zz_window_give();
	return r < 0 ? r : ZF_OK;
}

int zf_extract_tgz(zf_io_t *in, zf_dir_t *d) {
	static zf_src_t src;
	int r = gz_open(in, &src);
	if (r < 0) return r;
	r = zf_extract_tar(&src, d);
	zz_window_give();
	return r;
}
