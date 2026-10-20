/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpaper -- see zpaper.h and docs/zpaper.md.
 */

#include "zpaper.h"

#include <string.h>

// -- bytes ---------------------------------------------------------------

static inline uint16_t rd16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
		((uint32_t)p[3] << 24);
}

static inline int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }

typedef struct {
	uint8_t		b[128];
	uint32_t	n;
} buf_t;

static void b8(buf_t *b, uint32_t v) { b->b[b->n++] = (uint8_t)v; }
static void b16(buf_t *b, uint32_t v) { b8(b, v); b8(b, v >> 8); }
static void b32(buf_t *b, uint32_t v) { b16(b, v); b16(b, v >> 16); }
static void bbytes(buf_t *b, const void *d, uint32_t n) {
	memcpy(b->b + b->n, d, n);
	b->n += n;
}
static void bpaint(buf_t *b, const zc_paint_t *p) {
	zc_paint_t black = zc_black();
	if (!p) p = &black;
	b8(b, p->kind);
	b8(b, p->tone);
	b8(b, p->opaque);
	b8(b, 0);
	bbytes(b, p->pat, 8);
}

// -- writing ---------------------------------------------------------------

static void put(zp_writer_t *w, const void *d, uint32_t n) {
	if (w->failed || !n) return;
	if (!w->write(w->ctx, d, n)) w->failed = true;
}

static void pad(zp_writer_t *w, uint32_t len) {
	static const uint8_t z[4] = { 0 };
	put(w, z, (4 - (len & 3)) & 3);
}

static void head(zp_writer_t *w, int op, uint32_t len) {
	buf_t b;
	b.n = 0;
	b8(&b, (uint32_t)op);
	b8(&b, 0);
	b16(&b, 0);
	b32(&b, len);
	put(w, b.b, b.n);
}

void zp_writer_init(zp_writer_t *w, zp_write_fn write, void *ctx) {
	buf_t b;
	memset(w, 0, sizeof(*w));
	w->write = write;
	w->ctx = ctx;
	b.n = 0;
	bbytes(&b, ZP_MAGIC, 4);
	b32(&b, ZP_VERSION);
	put(w, b.b, b.n);
}

bool zp_job(zp_writer_t *w, const char *key, const char *value) {
	uint32_t kl = (uint32_t)strlen(key), vl = (uint32_t)strlen(value);
	head(w, ZP_JOB, kl + 1 + vl);
	put(w, key, kl);
	put(w, "=", 1);
	put(w, value, vl);
	pad(w, kl + 1 + vl);
	return !w->failed;
}

// The clip as it stands, written when it changed.
static void sync_clip(zp_writer_t *w, const zc_target_t *t) {
	buf_t b;
	if (w->clip_sent && w->clip[0] == t->cx0 && w->clip[1] == t->cy0 &&
		w->clip[2] == t->cx1 && w->clip[3] == t->cy1) return;
	w->clip[0] = t->cx0;
	w->clip[1] = t->cy0;
	w->clip[2] = t->cx1;
	w->clip[3] = t->cy1;
	w->clip_sent = true;
	b.n = 0;
	b32(&b, (uint32_t)t->cx0);
	b32(&b, (uint32_t)t->cy0);
	b32(&b, (uint32_t)t->cx1);
	b32(&b, (uint32_t)t->cy1);
	head(w, ZP_CLIP, b.n);
	put(w, b.b, b.n);
}

static void rec_rect(zc_target_t *t, int x0, int y0, int x1, int y1,
	const zc_paint_t *p) {
	zp_writer_t *w = (zp_writer_t *)t->rec_ctx;
	buf_t b;
	sync_clip(w, t);
	b.n = 0;
	bpaint(&b, p);
	b32(&b, (uint32_t)x0);
	b32(&b, (uint32_t)y0);
	b32(&b, (uint32_t)x1);
	b32(&b, (uint32_t)y1);
	head(w, ZP_RECT, b.n);
	put(w, b.b, b.n);
}

static void rec_path(zc_target_t *t, int op, const zc_path_t *path, int rule,
	zc_fix width, bool closed, const zc_paint_t *p) {
	zp_writer_t *w = (zp_writer_t *)t->rec_ctx;
	buf_t b;
	uint32_t subs = (uint32_t)path->nsub * 2, len;
	zc_fix grow = width / 2 + ZC_ONE;
	int i;
	sync_clip(w, t);
	if (op == ZP_FILL) grow = ZC_ONE;
	b.n = 0;
	bpaint(&b, p);
	b8(&b, op == ZP_FILL ? (uint32_t)rule : (closed ? 1u : 0u));
	b8(&b, 0);
	b16(&b, (uint32_t)path->nsub);
	b32(&b, (uint32_t)path->n);
	b32(&b, (uint32_t)width);
	b32(&b, (uint32_t)(path->bx0 - grow));
	b32(&b, (uint32_t)(path->by0 - grow));
	b32(&b, (uint32_t)(path->bx1 + grow));
	b32(&b, (uint32_t)(path->by1 + grow));
	len = b.n + subs + ((4 - (subs & 3)) & 3) + (uint32_t)path->n * 8;
	head(w, op, len);
	put(w, b.b, b.n);
	for (i = 0; i < path->nsub; i++) {
		uint8_t s2[2] = { (uint8_t)path->sub[i], (uint8_t)(path->sub[i] >> 8) };
		put(w, s2, 2);
	}
	pad(w, subs);
	for (i = 0; i < path->n; i++) {
		buf_t q;
		q.n = 0;
		b32(&q, (uint32_t)path->pt[i].x);
		b32(&q, (uint32_t)path->pt[i].y);
		put(w, q.b, q.n);
	}
}

static void rec_fill(zc_target_t *t, const zc_path_t *path, int rule,
	const zc_paint_t *p) {
	rec_path(t, ZP_FILL, path, rule, 0, false, p);
}

static void rec_stroke(zc_target_t *t, const zc_path_t *path, zc_fix width,
	bool closed, const zc_paint_t *p) {
	rec_path(t, ZP_STROKE, path, 0, width, closed, p);
}

static void rec_image(zc_target_t *t, int kind, const void *src, int stride,
	int sw, int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh,
	const zc_paint_t *p) {
	zp_writer_t *w = (zp_writer_t *)t->rec_ctx;
	buf_t b;
	uint32_t rowbytes, y;
	if (sw <= 0 || sh <= 0 || sw > 65535 || sh > 65535) return;
	rowbytes = kind == ZC_IMG_GRAY ? (uint32_t)sw : (uint32_t)((sw + 31) / 32) * 4;
	sync_clip(w, t);
	b.n = 0;
	bpaint(&b, p);
	b8(&b, (uint32_t)kind);
	b8(&b, 0);
	b16(&b, 0);
	b16(&b, (uint32_t)sw);
	b16(&b, (uint32_t)sh);
	b32(&b, rowbytes);
	b32(&b, (uint32_t)dx);
	b32(&b, (uint32_t)dy);
	b32(&b, (uint32_t)dw);
	b32(&b, (uint32_t)dh);
	head(w, ZP_IMAGE, b.n + rowbytes * (uint32_t)sh);
	put(w, b.b, b.n);
	for (y = 0; y < (uint32_t)sh; y++) {
		const uint8_t *row = kind == ZC_IMG_GRAY
			? (const uint8_t *)src + (size_t)y * (size_t)stride
			: (const uint8_t *)((const uint32_t *)src + (size_t)y * (size_t)stride);
		put(w, row, rowbytes);
	}
	pad(w, rowbytes * (uint32_t)sh);
}

static void rec_text(zc_target_t *t, const zc_xf_t *xf, const void *style,
	zc_fix x, zc_fix y, const char *s, uint32_t len, const zc_paint_t *p) {
	zp_writer_t *w = (zp_writer_t *)t->rec_ctx;
	const zc_style_t *st = (const zc_style_t *)style;
	buf_t b;
	char name[ZP_FONT_MAX];
	zc_fix asc, desc, wd, pen, lx, ly, hx, hy;
	zc_fix cx[4], cy[4];
	int i;
	if (!len) return;
	zc_text_metrics(st, &asc, &desc, NULL);
	wd = zc_text_width(st, s, len);
	pen = zc_text_pen(st);
	// the run's box, turned with it: italic leans past the advance by a
	// fifth of the ascent, the pen spreads every edge
	zc_xf_apply(xf, x - pen - asc / 5, y - asc - pen, &cx[0], &cy[0]);
	zc_xf_apply(xf, x + wd + pen + asc / 5, y - asc - pen, &cx[1], &cy[1]);
	zc_xf_apply(xf, x - pen - asc / 5, y + desc + pen, &cx[2], &cy[2]);
	zc_xf_apply(xf, x + wd + pen + asc / 5, y + desc + pen, &cx[3], &cy[3]);
	lx = hx = cx[0];
	ly = hy = cy[0];
	for (i = 1; i < 4; i++) {
		if (cx[i] < lx) lx = cx[i];
		if (cx[i] > hx) hx = cx[i];
		if (cy[i] < ly) ly = cy[i];
		if (cy[i] > hy) hy = cy[i];
	}
	memset(name, 0, sizeof(name));
	for (i = 0; i < ZP_FONT_MAX - 1 && st->font->name[i]; i++) name[i] = st->font->name[i];

	sync_clip(w, t);
	b.n = 0;
	bpaint(&b, p);
	b32(&b, (uint32_t)xf->a);
	b32(&b, (uint32_t)xf->b);
	b32(&b, (uint32_t)xf->c);
	b32(&b, (uint32_t)xf->d);
	b32(&b, (uint32_t)xf->e);
	b32(&b, (uint32_t)xf->f);
	b32(&b, (uint32_t)x);
	b32(&b, (uint32_t)y);
	b32(&b, (uint32_t)st->size);
	b32(&b, (uint32_t)st->weight);
	b32(&b, (uint32_t)st->track);
	b8(&b, st->bold);
	b8(&b, st->italic);
	b16(&b, 0);
	bbytes(&b, name, ZP_FONT_MAX);
	b32(&b, (uint32_t)(lx - ZC_ONE));
	b32(&b, (uint32_t)(ly - ZC_ONE));
	b32(&b, (uint32_t)(hx + ZC_ONE));
	b32(&b, (uint32_t)(hy + ZC_ONE));
	b32(&b, len);
	head(w, ZP_TEXT, b.n + len);
	put(w, b.b, b.n);
	put(w, s, len);
	pad(w, len);
}

static const zc_rec_ops_t rec_ops = {
	rec_rect, rec_fill, rec_stroke, rec_image, rec_text,
};

zc_target_t *zp_page_begin(zp_writer_t *w, const zpage_t *pg, zc_xf_t *xf) {
	buf_t b;
	int mw = 0, mh = 0;
	char name[ZP_NAME_MAX];
	int i;
	if (w->in_page) zp_page_end(w);
	memset(name, 0, sizeof(name));
	for (i = 0; i < ZP_NAME_MAX - 1 && pg->size.name && pg->size.name[i]; i++)
		name[i] = pg->size.name[i];
	if (!zpage_physical(pg, &mw, &mh)) { mw = 0; mh = 0; }
	b.n = 0;
	b16(&b, pg->gw);
	b16(&b, pg->gh);
	b8(&b, pg->size.kind);
	b8(&b, pg->landscape ? 1 : 0);
	b16(&b, (uint32_t)mw);
	b16(&b, (uint32_t)mh);
	b16(&b, 0);
	bbytes(&b, name, ZP_NAME_MAX);
	head(w, ZP_PAGE, b.n);
	put(w, b.b, b.n);

	zc_target_rec(&w->t, &rec_ops, w, pg->gw * ZP_SUB, pg->gh * ZP_SUB);
	w->t.pat_scale = ZP_SUB;
	w->clip_sent = false;
	w->in_page = true;
	*xf = zc_xf_scale(ZP_SUB << 16, 0, 0);
	return &w->t;
}

bool zp_page_end(zp_writer_t *w) {
	if (!w->in_page) return !w->failed;
	head(w, ZP_END, 0);
	w->in_page = false;
	w->pages++;
	return !w->failed;
}

void zp_image_file(zc_target_t *t, const char *path, int contrast, int bright,
	zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh) {
	zp_writer_t *w;
	buf_t b;
	uint32_t pl;
	if (t->fmt != ZC_REC || t->rec != &rec_ops || zc_clip_empty(t)) return;
	w = (zp_writer_t *)t->rec_ctx;
	pl = (uint32_t)strlen(path);
	if (pl >= ZP_PATH_MAX) return;
	sync_clip(w, t);
	b.n = 0;
	b32(&b, (uint32_t)dx);
	b32(&b, (uint32_t)dy);
	b32(&b, (uint32_t)dw);
	b32(&b, (uint32_t)dh);
	b16(&b, (uint32_t)(int16_t)contrast);
	b16(&b, (uint32_t)(int16_t)bright);
	b16(&b, pl);
	b16(&b, 0);
	head(w, ZP_IMAGE_FILE, b.n + pl);
	put(w, b.b, b.n);
	put(w, path, pl);
	pad(w, pl);
}

// -- reading -------------------------------------------------------------------

bool zp_open(zp_reader_t *r, const void *data, uint32_t len) {
	const uint8_t *d = (const uint8_t *)data;
	memset(r, 0, sizeof(*r));
	if (!d || len < 8 || memcmp(d, ZP_MAGIC, 4) || rd32(d + 4) != ZP_VERSION) return false;
	r->data = d;
	r->len = len;
	r->pos = 8;
	return true;
}

bool zp_next(zp_reader_t *r, zp_rec_t *rec) {
	uint32_t len, total;
	if (r->bad || r->pos + 8 > r->len) {
		if (r->pos != r->len) r->bad = true;
		return false;
	}
	len = rd32(r->data + r->pos + 4);
	total = 8 + ((len + 3) & ~3u);
	if (len > r->len || total > r->len - r->pos) { r->bad = true; return false; }
	rec->op = r->data[r->pos];
	rec->len = len;
	rec->p = r->data + r->pos + 8;
	r->pos += total;
	return true;
}

bool zp_job_get(const void *data, uint32_t len, const char *key, char *out, int outlen) {
	zp_reader_t r;
	zp_rec_t rec;
	uint32_t kl = (uint32_t)strlen(key);
	if (outlen > 0) out[0] = 0;
	if (!zp_open(&r, data, len)) return false;
	while (zp_next(&r, &rec)) {
		if (rec.op == ZP_PAGE) break;
		if (rec.op != ZP_JOB || rec.len <= kl || memcmp(rec.p, key, kl) || rec.p[kl] != '=')
			continue;
		{
			uint32_t vl = rec.len - kl - 1, i;
			if (outlen <= 0) return true;
			if (vl > (uint32_t)outlen - 1) vl = (uint32_t)outlen - 1;
			for (i = 0; i < vl; i++) out[i] = (char)rec.p[kl + 1 + i];
			out[vl] = 0;
			return true;
		}
	}
	return false;
}

int zp_pages(const void *data, uint32_t len, uint32_t *start, uint32_t *bytes, int max) {
	zp_reader_t r;
	zp_rec_t rec;
	int n = 0;
	uint32_t at = 0;
	bool open = false;
	if (!zp_open(&r, data, len)) return 0;
	for (;;) {
		uint32_t here = r.pos;
		if (!zp_next(&r, &rec)) break;
		if (rec.op == ZP_PAGE) {
			at = here;
			open = true;
		} else if (rec.op == ZP_END && open) {
			if (n < max) {
				start[n] = at;
				bytes[n] = r.pos - at;
			}
			n++;
			open = false;
		}
	}
	return n;
}

bool zp_page_info(const zp_rec_t *rec, zp_page_t *out) {
	if (rec->op != ZP_PAGE || rec->len < 12 + ZP_NAME_MAX) return false;
	memset(out, 0, sizeof(*out));
	out->gw = rd16(rec->p);
	out->gh = rd16(rec->p + 2);
	out->kind = rec->p[4];
	out->landscape = rec->p[5];
	out->w_mm10 = rd16(rec->p + 6);
	out->h_mm10 = rd16(rec->p + 8);
	memcpy(out->name, rec->p + 12, ZP_NAME_MAX);
	out->name[ZP_NAME_MAX - 1] = 0;
	return out->gw > 0 && out->gh > 0;
}

void zp_page_zpage(const zp_page_t *pi, zpage_t *pg) {
	memset(pg, 0, sizeof(*pg));
	if (!zpage_set(pg, pi->name, pi->landscape != 0)) {
		if (pi->kind == ZPAGE_PAPER && pi->w_mm10 && pi->h_mm10)
			zpage_custom(pg, ZPAGE_PAPER, pi->w_mm10, pi->h_mm10, pi->landscape != 0);
		else
			zpage_custom(pg, ZPAGE_RATIO, pi->gw, pi->gh, pi->gw >= pi->gh);
	}
	// the recorded grid is what the drawing used
	pg->gw = pi->gw;
	pg->gh = pi->gh;
}

static void rd_paint(const uint8_t *p, zc_paint_t *out) {
	out->kind = p[0];
	out->tone = p[1];
	out->opaque = p[2];
	memcpy(out->pat, p + 4, 8);
}

bool zp_decode(const zp_rec_t *rec, zp_op_t *op) {
	const uint8_t *p = rec->p;
	uint32_t n = rec->len;
	memset(op, 0, sizeof(*op));
	op->op = rec->op;
	switch (rec->op) {
	case ZP_CLIP:
		if (n < 16) return false;
		op->clip[0] = rds32(p);
		op->clip[1] = rds32(p + 4);
		op->clip[2] = rds32(p + 8);
		op->clip[3] = rds32(p + 12);
		return true;
	case ZP_RECT:
		if (n < 28) return false;
		rd_paint(p, &op->paint);
		op->box[0] = ZC_I(rds32(p + 12));
		op->box[1] = ZC_I(rds32(p + 16));
		op->box[2] = ZC_I(rds32(p + 20));
		op->box[3] = ZC_I(rds32(p + 24));
		return true;
	case ZP_FILL:
	case ZP_STROKE: {
		uint32_t subs, need;
		if (n < 40) return false;
		rd_paint(p, &op->paint);
		if (rec->op == ZP_FILL) op->rule = p[12]; else op->closed = p[12];
		op->nsub = rd16(p + 14);
		op->npts = (int)rd32(p + 16);
		op->width = rds32(p + 20);
		op->box[0] = rds32(p + 24);
		op->box[1] = rds32(p + 28);
		op->box[2] = rds32(p + 32);
		op->box[3] = rds32(p + 36);
		subs = (uint32_t)op->nsub * 2;
		subs += (4 - (subs & 3)) & 3;
		if (op->npts < 0 || (uint32_t)op->npts > (n - 40) / 8) return false;
		need = 40 + subs + (uint32_t)op->npts * 8;
		if (need > n) return false;
		op->sub = p + 40;
		op->pts = p + 40 + subs;
		{
			int i;
			for (i = 0; i < op->nsub; i++)
				if (rd16(op->sub + i * 2) >= (uint32_t)op->npts) return false;
		}
		return true;
	}
	case ZP_IMAGE:
		if (n < 40) return false;
		rd_paint(p, &op->paint);
		op->kind = p[12];
		op->sw = rd16(p + 16);
		op->sh = rd16(p + 18);
		op->rowbytes = (int)rd32(p + 20);
		op->dx = rds32(p + 24);
		op->dy = rds32(p + 28);
		op->dw = rds32(p + 32);
		op->dh = rds32(p + 36);
		if (!op->sw || !op->sh || op->rowbytes <= 0) return false;
		if (op->kind > ZC_IMG_GRAY) return false;
		if (op->rowbytes < (op->kind == ZC_IMG_GRAY ? op->sw : (op->sw + 31) / 32 * 4)) return false;
		if ((uint64_t)op->rowbytes * (uint64_t)op->sh > n - 40) return false;
		op->pix = p + 40;
		op->box[0] = op->dx;
		op->box[1] = op->dy;
		op->box[2] = op->dx + op->dw;
		op->box[3] = op->dy + op->dh;
		return true;
	case ZP_IMAGE_FILE: {
		uint32_t pl;
		if (n < 24) return false;
		op->dx = rds32(p);
		op->dy = rds32(p + 4);
		op->dw = rds32(p + 8);
		op->dh = rds32(p + 12);
		op->contrast = (int16_t)rd16(p + 16);
		op->bright = (int16_t)rd16(p + 18);
		pl = rd16(p + 20);
		if (pl >= ZP_PATH_MAX || 24 + pl > n) return false;
		memcpy(op->path, p + 24, pl);
		op->path[pl] = 0;
		op->box[0] = op->dx;
		op->box[1] = op->dy;
		op->box[2] = op->dx + op->dw;
		op->box[3] = op->dy + op->dh;
		return true;
	}
	case ZP_TEXT:
		if (n < 100) return false;
		rd_paint(p, &op->paint);
		op->xf.a = rds32(p + 12);
		op->xf.b = rds32(p + 16);
		op->xf.c = rds32(p + 20);
		op->xf.d = rds32(p + 24);
		op->xf.e = rds32(p + 28);
		op->xf.f = rds32(p + 32);
		op->x = rds32(p + 36);
		op->y = rds32(p + 40);
		op->size = rds32(p + 44);
		op->weight = rds32(p + 48);
		op->track = rds32(p + 52);
		op->bold = p[56];
		op->italic = p[57];
		memcpy(op->font, p + 60, ZP_FONT_MAX);
		op->font[ZP_FONT_MAX - 1] = 0;
		op->box[0] = rds32(p + 80);
		op->box[1] = rds32(p + 84);
		op->box[2] = rds32(p + 88);
		op->box[3] = rds32(p + 92);
		op->tlen = rd32(p + 96);
		if (op->tlen > n - 100) return false;
		op->text = (const char *)(p + 100);
		return true;
	default:
		return false;
	}
}

void zp_point(const zp_op_t *op, int i, zc_fix *x, zc_fix *y) {
	*x = rds32(op->pts + (size_t)i * 8);
	*y = rds32(op->pts + (size_t)i * 8 + 4);
}

int zp_sub_at(const zp_op_t *op, int i) {
	return rd16(op->sub + (size_t)i * 2);
}

// -- playing ------------------------------------------------------------------

int zp_max_points(const void *data, uint32_t len) {
	zp_reader_t r;
	zp_rec_t rec;
	int m = 0;
	if (!zp_open(&r, data, len)) return 0;
	while (zp_next(&r, &rec)) {
		if ((rec.op == ZP_FILL || rec.op == ZP_STROKE) && rec.len >= 40) {
			int n = (int)rd32(rec.p + 16);
			if (n > m) m = n;
		}
	}
	return m;
}

// The device rectangle a box of recorded units covers through xd.
static void dev_box(const zc_xf_t *xd, const zc_fix *box, zc_fix *lx, zc_fix *ly,
	zc_fix *hx, zc_fix *hy) {
	zc_fix x0, y0, x1, y1;
	zc_xf_apply(xd, box[0], box[1], &x0, &y0);
	zc_xf_apply(xd, box[2], box[3], &x1, &y1);
	*lx = x0 < x1 ? x0 : x1;
	*hx = x0 < x1 ? x1 : x0;
	*ly = y0 < y1 ? y0 : y1;
	*hy = y0 < y1 ? y1 : y0;
}

static bool outside(const zc_target_t *t, zc_fix lx, zc_fix ly, zc_fix hx, zc_fix hy) {
	return hx < ZC_I(t->cx0) || lx > ZC_I(t->cx1) || hy < ZC_I(t->cy0) || ly > ZC_I(t->cy1);
}

// A pattern for a page laid a quarter (or half) turn: patterns are
// drawn in device cells, so the 8x8 itself is turned to stay with the
// page. Each device cell's centre is taken back into page cells.
static void turn_pattern(const zc_xf_t *xf, zc_paint_t *p) {
	int sa = (xf->a > 0) - (xf->a < 0), sb = (xf->b > 0) - (xf->b < 0);
	int sc = (xf->c > 0) - (xf->c < 0), sd = (xf->d > 0) - (xf->d < 0);
	int det = sa * sd - sb * sc;
	uint8_t out[8];
	int i, j;
	if (p->kind != ZC_P_PATTERN || (sb == 0 && sc == 0 && sa > 0 && sd > 0) || !det) return;
	for (i = 0; i < 8; i++) {
		out[i] = 0;
		for (j = 0; j < 8; j++) {
			// the inverse of [sa sc; sb sd] on the cell's centre, doubled
			int cx = 2 * j + 1, cy = 2 * i + 1;
			int x2 = (sd * cx - sc * cy) * det, y2 = (-sb * cx + sa * cy) * det;
			int px = (x2 >= 0 ? x2 / 2 : (x2 - 1) / 2) & 7;
			int py = (y2 >= 0 ? y2 / 2 : (y2 - 1) / 2) & 7;
			if ((p->pat[py] >> (7 - px)) & 1) out[i] |= (uint8_t)(0x80 >> j);
		}
	}
	memcpy(p->pat, out, 8);
}

static bool build_path(zp_play_t *pl, const zp_op_t *op, const zc_xf_t *xd) {
	zc_path_t *pa = pl->path;
	int k, i;
	if (op->npts > pa->cap || op->nsub > pa->subcap) return false;
	zc_path_set_xf(pa, xd);
	zc_path_reset(pa);
	for (k = 0; k < op->nsub; k++) {
		int s0 = zp_sub_at(op, k);
		int s1 = k + 1 < op->nsub ? zp_sub_at(op, k + 1) : op->npts;
		for (i = s0; i < s1; i++) {
			zc_fix x, y;
			zp_point(op, i, &x, &y);
			if (i == s0) zc_path_move(pa, x, y);
			else zc_path_line(pa, x, y);
		}
	}
	return !pa->overflow;
}

void zp_play_page(zp_play_t *pl, const uint8_t *page, uint32_t len,
	zc_target_t *t, const zc_xf_t *xf) {
	zp_reader_t r;
	zp_rec_t rec;
	zp_op_t op;
	zc_xf_t sub = { 65536 / ZP_SUB, 0, 0, 65536 / ZP_SUB, 0, 0 };
	zc_xf_t xd = zc_xf_mul(xf, &sub);
	int bx0 = t->cx0, by0 = t->cy0, bx1 = t->cx1, by1 = t->cy1;
	uint8_t ps0 = t->pat_scale;
	int px0 = t->px, py0 = t->py;
	int32_t s = zc_xf_scale_of(xf);
	zc_fix ox, oy;

	// one pattern bit a page unit, anchored at the page's corner
	t->pat_scale = (uint8_t)(s < 98304 ? 1 : s >= (255 << 16) ? 255 : (s + 32768) >> 16);
	zc_xf_apply(xf, 0, 0, &ox, &oy);
	t->px = (ox + ZC_HALF) >> ZC_FRAC;
	t->py = (oy + ZC_HALF) >> ZC_FRAC;

	r.data = page;
	r.len = len;
	r.pos = 0;
	r.bad = false;
	while (zp_next(&r, &rec)) {
		zc_fix lx, ly, hx, hy;
		if (rec.op == ZP_END) break;
		if (rec.op == ZP_PAGE || rec.op == ZP_JOB) continue;
		if (!zp_decode(&rec, &op)) { pl->skipped++; continue; }

		if (op.op == ZP_CLIP) {
			zc_fix c[4];
			c[0] = ZC_I(op.clip[0]);
			c[1] = ZC_I(op.clip[1]);
			c[2] = ZC_I(op.clip[2]);
			c[3] = ZC_I(op.clip[3]);
			dev_box(&xd, c, &lx, &ly, &hx, &hy);
			t->cx0 = bx0; t->cy0 = by0; t->cx1 = bx1; t->cy1 = by1;
			zc_clip(t, (lx + ZC_HALF) >> ZC_FRAC, (ly + ZC_HALF) >> ZC_FRAC,
				(hx + ZC_HALF) >> ZC_FRAC, (hy + ZC_HALF) >> ZC_FRAC);
			continue;
		}

		dev_box(&xd, op.box, &lx, &ly, &hx, &hy);
		if (zc_clip_empty(t) || outside(t, lx, ly, hx, hy)) continue;
		turn_pattern(xf, &op.paint);

		switch (op.op) {
		case ZP_RECT: {
			zc_path_t *pa = pl->path;
			zc_path_set_xf(pa, &xd);
			zc_path_reset(pa);
			zc_path_rect(pa, op.box[0], op.box[1], op.box[2] - op.box[0], op.box[3] - op.box[1]);
			zc_fill(t, pa, ZC_NONZERO, &op.paint);
			break;
		}
		case ZP_FILL:
			if (!build_path(pl, &op, &xd) || !zc_fill(t, pl->path, op.rule, &op.paint))
				pl->skipped++;
			break;
		case ZP_STROKE:
			if (!build_path(pl, &op, &xd) ||
				!zc_stroke(t, pl->path, op.width, op.closed != 0, pl->work, &op.paint))
				pl->skipped++;
			break;
		case ZP_IMAGE: {
			// source pixels -> recorded units -> device
			zc_xf_t im = { (int32_t)(((int64_t)op.dw << 8) / op.sw), 0, 0,
				(int32_t)(((int64_t)op.dh << 8) / op.sh), op.dx, op.dy };
			zc_xf_t m = zc_xf_mul(&xd, &im);
			int stride = op.kind == ZC_IMG_GRAY ? op.rowbytes : op.rowbytes / 4;
			// words of a bits source must be aligned; the record is
			if (op.kind != ZC_IMG_GRAY && ((uintptr_t)op.pix & 3)) { pl->skipped++; break; }
			zc_blit_xf(t, op.kind, op.pix, stride, op.sw, op.sh, &m, &op.paint);
			break;
		}
		case ZP_IMAGE_FILE: {
			const uint8_t *pix;
			int w, h, mw = (int)((hx - lx) >> ZC_FRAC) + 1, mh = (int)((hy - ly) >> ZC_FRAC) + 1;
			bool turned = xd.b != 0 || xd.c != 0;
			if (turned) { int k = mw; mw = mh; mh = k; }
			if (!pl->image || !pl->image(pl->ctx, op.path, op.contrast, op.bright,
				mw, mh, &pix, &w, &h) || w <= 0 || h <= 0) {
				pl->skipped++;
				break;
			}
			{
				zc_xf_t im = { (int32_t)(((int64_t)op.dw << 8) / w), 0, 0,
					(int32_t)(((int64_t)op.dh << 8) / h), op.dx, op.dy };
				zc_xf_t m = zc_xf_mul(&xd, &im);
				zc_blit_xf(t, ZC_IMG_GRAY, pix, w, w, h, &m, NULL);
			}
			break;
		}
		case ZP_TEXT: {
			const zc_font_t *f = NULL;
			zc_style_t st;
			zc_xf_t tx;
			if (!pl->font || !pl->font(pl->ctx, op.font, &f) || !f) f = pl->fallback;
			if (!f) { pl->skipped++; break; }
			memset(&st, 0, sizeof(st));
			st.font = f;
			st.size = op.size;
			st.weight = op.weight;
			st.track = op.track;
			st.bold = op.bold;
			st.italic = op.italic;
			tx = zc_xf_mul(&xd, &op.xf);
			zc_text_draw(t, &tx, &st, op.x, op.y, op.text, op.tlen, &op.paint);
			break;
		}
		default:
			break;
		}
	}
	if (r.bad) pl->skipped++;

	t->cx0 = bx0; t->cy0 = by0; t->cx1 = bx1; t->cy1 = by1;
	t->pat_scale = ps0;
	t->px = px0;
	t->py = py0;
}
