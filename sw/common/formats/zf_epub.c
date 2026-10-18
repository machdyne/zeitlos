/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading EPUB (2 and 3) as one document.
 *
 * META-INF/container.xml names the package document; its manifest
 * maps ids to files and its spine gives the reading order. Each
 * chapter in the spine that is (X)HTML is read by zf_html_src() into
 * the same document, in order; the title is the package's dc:title.
 * DRM-protected books (META-INF/encryption.xml naming content) are
 * refused: what would come out is noise.
 *
 * The manifest and spine are kept in zf_heap: a book can list a few
 * thousand items.
 */

#include <string.h>

#include "zf.h"
#include "zf_zip.h"
#include "zf_xml.h"

static void copy(char *d, const char *s, int cap) {
	int i = 0;
	if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

// "a/b/../c%20d.xhtml#x" relative to `dir` -> "a/c d.xhtml"; false if it
// climbs out of the archive or does not fit.
static bool resolve(const char *dir, const char *href, char *out, int cap) {
	char tmp[ZF_PATH_MAX * 2];
	int n = 0;
	if (href[0] != '/') {
		for (int i = 0; dir[i] && n < (int)sizeof tmp - 1; i++) tmp[n++] = dir[i];
	} else href++;
	for (int i = 0; href[i] && href[i] != '#' && href[i] != '?' && n < (int)sizeof tmp - 1; i++) {
		char c = href[i];
		if (c == '%' && href[i + 1] && href[i + 2]) {
			int v = 0;
			for (int k = 1; k <= 2; k++) {
				char h = href[i + k];
				v = v * 16 + ((h >= '0' && h <= '9') ? h - '0' : (h | 32) - 'a' + 10);
			}
			c = (char)v;
			i += 2;
		}
		tmp[n++] = c;
	}
	tmp[n] = 0;
	// normalise . and ..
	int o = 0;
	const char *p = tmp;
	while (*p) {
		const char *s = strchr(p, '/');
		int len = s ? (int)(s - p) : (int)strlen(p);
		if (len == 0 || (len == 1 && p[0] == '.')) { /* nothing */ }
		else if (len == 2 && p[0] == '.' && p[1] == '.') {
			if (o == 0) return false;
			o--;
			while (o > 0 && out[o - 1] != '/') o--;
		} else {
			if (o + len + 2 > cap) return false;
			memcpy(out + o, p, (size_t)len);
			o += len;
			if (s) out[o++] = '/';
		}
		if (!s) break;
		p = s + 1;
	}
	out[o] = 0;
	return o > 0;
}

int zf_read_epub(zf_io_t *in, zf_doc_t *d) {
	static zz_t z;
	static char opf[ZF_PATH_MAX], dir[ZF_PATH_MAX], path[ZF_PATH_MAX], title[128];
	int r = zz_open(&z, in);
	if (r < 0) return r;
	if (!zf_heap || zf_heap_size < 4096) return ZF_E_NOMEM;

	// encrypted content?
	{
		zz_entry_t e;
		if (zz_find(&z, "META-INF/encryption.xml", &e) == 1) {
			if ((r = zz_ent_open(&z, "META-INF/encryption.xml")) < 0) return r;
			bool enc = false;
			for (;;) {
				zx_tok_t t = zx_next(&zz_ent.x);
				if (t == ZX_EOF || t == ZX_ERR) break;
				if (t == ZX_START && !strcmp(zx_local(zz_ent.x.name), "EncryptedData")) { enc = true; break; }
			}
			zz_ent_close();
			if (enc) return ZF_E_UNSUPP;
		}
	}

	// the package document
	opf[0] = 0;
	if ((r = zz_ent_open(&z, "META-INF/container.xml")) < 0) { zz_ent_close(); return r == ZF_E_FORMAT ? ZF_E_FORMAT : r; }
	for (;;) {
		zx_tok_t t = zx_next(&zz_ent.x);
		if (t == ZX_EOF || t == ZX_ERR) break;
		if (t == ZX_START && !strcmp(zx_local(zz_ent.x.name), "rootfile")) {
			copy(opf, zx_attr(&zz_ent.x, "full-path"), sizeof opf);
			break;
		}
	}
	zz_ent_close();
	if (!opf[0]) return ZF_E_FORMAT;
	copy(dir, opf, sizeof dir);
	{ char *s = strrchr(dir, '/'); if (s) s[1] = 0; else dir[0] = 0; }

	// manifest (id\0href\0, html only) from the bottom of the heap,
	// spine (idref\0) from the top
	uint8_t *heap = zf_heap;
	uint32_t mtop = 0, sbot = zf_heap_size;
	uint32_t nspine = 0;
	title[0] = 0;
	if ((r = zz_ent_open(&z, opf)) < 0) { zz_ent_close(); return r; }
	{
		zx_t *x = &zz_ent.x;
		bool in_title = false;
		int tl = 0;
		for (;;) {
			zx_tok_t t = zx_next(x);
			if (t == ZX_EOF) break;
			if (t == ZX_ERR) { r = zz_ent.rd.err ? zz_ent.rd.err : ZF_E_CORRUPT; break; }
			const char *n = zx_local(x->name);
			if (t == ZX_START && !strcmp(n, "title") && !title[0]) in_title = true;
			else if (t == ZX_END && !strcmp(n, "title")) in_title = false;
			else if (t == ZX_TEXT && in_title) {
				for (uint32_t i = 0; i < x->tlen && tl < (int)sizeof title - 1; i++) title[tl++] = x->text[i];
				title[tl] = 0;
			} else if (t == ZX_START && !strcmp(n, "item")) {
				const char *id = zx_attr(x, "id"), *href = zx_attr(x, "href"), *mt = zx_attr(x, "media-type");
				if (!id || !href || !mt) continue;
				if (strcmp(mt, "application/xhtml+xml") && strcmp(mt, "text/html")) continue;
				uint32_t need = (uint32_t)(strlen(id) + strlen(href) + 2);
				if (mtop + need > sbot) { r = ZF_E_NOMEM; break; }
				memcpy(heap + mtop, id, strlen(id) + 1);
				mtop += (uint32_t)strlen(id) + 1;
				memcpy(heap + mtop, href, strlen(href) + 1);
				mtop += (uint32_t)strlen(href) + 1;
			} else if (t == ZX_START && !strcmp(n, "itemref")) {
				const char *id = zx_attr(x, "idref"), *lin = zx_attr(x, "linear");
				if (!id) continue;
				(void)lin;		// non-linear items (notes, covers) are kept: they are text
				uint32_t need = (uint32_t)strlen(id) + 1;
				if (sbot - need < mtop) { r = ZF_E_NOMEM; break; }
				sbot -= need;
				memcpy(heap + sbot, id, need);
				nspine++;
			}
		}
	}
	{ int e2 = zz_ent_close(); if (r >= 0 && e2 < 0) r = e2; }
	if (r < 0) return r;
	if (!nspine) return ZF_E_FORMAT;

	zf_html_reset(d);
	if (title[0]) zf_html_title(title);

	// the spine was stored top down: walk it from the top end backwards
	uint32_t at = zf_heap_size;
	for (uint32_t k = 0; k < nspine; k++) {
		// the k-th itemref ends at `at`; find its start
		uint32_t end = at - 1;		// its NUL
		uint32_t start = end;
		while (start > sbot && heap[start - 1]) start--;
		at = start;
		const char *idref = (const char *)heap + start;
		// look it up
		const char *href = NULL;
		for (uint32_t m = 0; m < mtop; ) {
			const char *id = (const char *)heap + m;
			m += (uint32_t)strlen(id) + 1;
			const char *h = (const char *)heap + m;
			m += (uint32_t)strlen(h) + 1;
			if (!strcmp(id, idref)) { href = h; break; }
		}
		if (!href) continue;
		if (!resolve(dir, href, path, sizeof path)) continue;
		if ((r = zz_ent_open(&z, path)) < 0) { zz_ent_close(); if (r == ZF_E_FORMAT) continue; return r; }
		r = zf_html_src(&zz_ent.src, d, true);
		int e2 = zz_ent_close();
		if (r < 0) return r;
		if (e2 < 0) return e2;
		if (zf_progress) zf_progress((k + 1) * 1000 / nspine);
	}
	return zf_html_finish(d);
}
