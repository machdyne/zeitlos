/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpsvg -- see zpsvg.h.
 */

#include "zpsvg.h"
#include "zpaper.h"

#include <string.h>

// -- writing numbers and text, without printf ------------------------------------

typedef struct {
	zp_svg_t	*s;
	bool		ok;
	// patterns made so far: their ids are their place here
	struct { uint8_t pat[8], tone, opaque; } pats[24];
	int			npat;
	int			nclip;
	bool		in_clip;
} wr_t;

static void put(wr_t *w, const char *t) {
	if (w->ok && !w->s->put(w->s->ctx, t, (uint32_t)strlen(t))) w->ok = false;
}

static void putn(wr_t *w, const char *t, uint32_t n) {
	if (w->ok && n && !w->s->put(w->s->ctx, t, n)) w->ok = false;
}

static void put_int(wr_t *w, long v) {
	char b[16];
	int k = 0;
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
	char t[16];
	int n = 0;
	do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
	if (v < 0) b[k++] = '-';
	while (n) b[k++] = t[--n];
	b[k] = 0;
	put(w, b);
}

// A value in 1/scale as a decimal with up to 3 places: device units of
// 24.8 over ZP_SUB are page units.
static void put_dec(wr_t *w, int64_t v, int64_t scale) {
	int64_t whole, frac;
	char b[8];
	int k;
	if (v < 0) { put(w, "-"); v = -v; }
	whole = v / scale;
	frac = ((v % scale) * 1000 + scale / 2) / scale;
	if (frac >= 1000) { whole++; frac -= 1000; }
	put_int(w, (long)whole);
	if (!frac) return;
	b[0] = '.';
	b[1] = (char)('0' + frac / 100);
	b[2] = (char)('0' + frac / 10 % 10);
	b[3] = (char)('0' + frac % 10);
	k = 4;
	while (k > 1 && b[k - 1] == '0') k--;
	b[k] = 0;
	put(w, b);
}

// Device units (24.8, ZP_SUB a page unit) as page units.
static void put_u(wr_t *w, zc_fix v) { put_dec(w, v, (int64_t)ZC_ONE * ZP_SUB); }

static void put_text(wr_t *w, const char *s, uint32_t n) {
	uint32_t i, from = 0;
	for (i = 0; i < n; i++) {
		const char *esc = s[i] == '<' ? "&lt;" : s[i] == '>' ? "&gt;" : s[i] == '&' ? "&amp;" :
			s[i] == '"' ? "&quot;" : NULL;
		if (!esc) continue;
		putn(w, s + from, i - from);
		put(w, esc);
		from = i + 1;
	}
	putn(w, s + from, n - from);
}

// -- paint -------------------------------------------------------------------------

static void put_grey(wr_t *w, int tone) {
	// tone 0 white .. 16 black
	int v = 255 - (tone * 255 + 8) / 16;
	static const char hx[] = "0123456789abcdef";
	char b[8];
	if (v < 0) v = 0;
	b[0] = '#';
	b[1] = hx[v >> 4]; b[2] = hx[v & 15];
	b[3] = b[1]; b[4] = b[2];
	b[5] = b[1]; b[6] = b[2];
	b[7] = 0;
	put(w, b);
}

// A pattern's id, defining it first if it is new.
static int pattern_id(wr_t *w, const zc_paint_t *p) {
	int i, x, y;
	for (i = 0; i < w->npat; i++)
		if (!memcmp(w->pats[i].pat, p->pat, 8) && w->pats[i].tone == p->tone && w->pats[i].opaque == p->opaque)
			return i;
	if (w->npat == (int)(sizeof(w->pats) / sizeof(w->pats[0]))) return -1;
	i = w->npat++;
	memcpy(w->pats[i].pat, p->pat, 8);
	w->pats[i].tone = p->tone;
	w->pats[i].opaque = p->opaque;
	// one pattern bit a page unit, anchored at the page's origin
	put(w, "<defs><pattern id=\"p"); put_int(w, i);
	put(w, "\" patternUnits=\"userSpaceOnUse\" width=\"8\" height=\"8\">");
	if (p->opaque) put(w, "<rect width=\"8\" height=\"8\" fill=\"#ffffff\"/>");
	put(w, "<path fill=\"");
	put_grey(w, p->tone);
	put(w, "\" d=\"");
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++)
			if (p->pat[y] & (0x80 >> x)) {
				put(w, "M"); put_int(w, x); put(w, " "); put_int(w, y); put(w, "h1v1h-1z");
			}
	put(w, "\"/></pattern></defs>\n");
	return i;
}

// fill="..." or stroke="..." for a paint
static void put_paint(wr_t *w, const char *attr, const zc_paint_t *p, int pid) {
	put(w, " "); put(w, attr); put(w, "=\"");
	if (p->kind == ZC_P_PATTERN && pid >= 0) { put(w, "url(#p"); put_int(w, pid); put(w, ")"); }
	else if (p->kind == ZC_P_XOR) put(w, "#808080");
	else put_grey(w, p->tone);
	put(w, "\"");
}

// -- the path of a fill or a stroke --------------------------------------------------

static void put_path_d(wr_t *w, const zp_op_t *op, bool close) {
	int s, i;
	put(w, " d=\"");
	for (s = 0; s < op->nsub; s++) {
		int a = zp_sub_at(op, s), b = s + 1 < op->nsub ? zp_sub_at(op, s + 1) : op->npts;
		for (i = a; i < b; i++) {
			zc_fix x, y;
			zp_point(op, i, &x, &y);
			put(w, i == a ? "M" : "L");
			put_u(w, x); put(w, " "); put_u(w, y);
		}
		if (close) put(w, "Z");
	}
	put(w, "\"");
}

// -- pictures -------------------------------------------------------------------------

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void put_b64(wr_t *w, const uint8_t *d, uint32_t n) {
	char b[64 + 4];
	int k = 0;
	uint32_t i;
	for (i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
		b[k++] = b64[v >> 18 & 63];
		b[k++] = b64[v >> 12 & 63];
		b[k++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
		b[k++] = i + 2 < n ? b64[v & 63] : '=';
		if (k >= 64) { putn(w, b, (uint32_t)k); k = 0; }
	}
	putn(w, b, (uint32_t)k);
}

// base64 of a stream made a few bytes at a time: the BMP below
typedef struct { wr_t *w; uint8_t q[3]; int nq; } b64s_t;
static void b64s_byte(b64s_t *b, uint8_t c) {
	b->q[b->nq++] = c;
	if (b->nq == 3) { put_b64(b->w, b->q, 3); b->nq = 0; }
}
static void b64s_end(b64s_t *b) { if (b->nq) put_b64(b->w, b->q, (uint32_t)b->nq); b->nq = 0; }
static void b64s_le(b64s_t *b, uint32_t v, int n) { int i; for (i = 0; i < n; i++) b64s_byte(b, (uint8_t)(v >> (8 * i))); }

static void put_xywh(wr_t *w, zc_fix x, zc_fix y, zc_fix ww, zc_fix hh) {
	put(w, " x=\""); put_u(w, x);
	put(w, "\" y=\""); put_u(w, y);
	put(w, "\" width=\""); put_u(w, ww);
	put(w, "\" height=\""); put_u(w, hh);
	put(w, "\"");
}

// Pixels in the job, as an 8-bit grey BMP (bottom row first, rows of
// 4 bytes).
static void image_pixels(wr_t *w, const zp_op_t *op) {
	b64s_t b = { w, { 0 }, 0 };
	int x, y, row = (op->sw + 3) & ~3;
	uint32_t data = (uint32_t)row * (uint32_t)op->sh, off = 14 + 40 + 1024;
	if (op->kind == ZC_IMG_MASK) { w->s->skipped++; return; }
	put(w, "<image preserveAspectRatio=\"none\"");
	put_xywh(w, op->dx, op->dy, op->dw, op->dh);
	put(w, " href=\"data:image/bmp;base64,");
	b64s_byte(&b, 'B'); b64s_byte(&b, 'M');
	b64s_le(&b, off + data, 4); b64s_le(&b, 0, 4); b64s_le(&b, off, 4);
	b64s_le(&b, 40, 4); b64s_le(&b, (uint32_t)op->sw, 4); b64s_le(&b, (uint32_t)op->sh, 4);
	b64s_le(&b, 1, 2); b64s_le(&b, 8, 2); b64s_le(&b, 0, 4); b64s_le(&b, data, 4);
	b64s_le(&b, 2835, 4); b64s_le(&b, 2835, 4); b64s_le(&b, 256, 4); b64s_le(&b, 0, 4);
	for (x = 0; x < 256; x++) { b64s_byte(&b, (uint8_t)x); b64s_byte(&b, (uint8_t)x); b64s_byte(&b, (uint8_t)x); b64s_byte(&b, 0); }
	for (y = op->sh - 1; y >= 0; y--) {
		const uint8_t *r = op->pix + (size_t)y * (size_t)op->rowbytes;
		for (x = 0; x < row; x++) {
			uint8_t v = 255;
			if (x < op->sw) {
				if (op->kind == ZC_IMG_GRAY) v = r[x];
				else {
					uint32_t word = (uint32_t)r[(x >> 5) * 4] | (uint32_t)r[(x >> 5) * 4 + 1] << 8 |
						(uint32_t)r[(x >> 5) * 4 + 2] << 16 | (uint32_t)r[(x >> 5) * 4 + 3] << 24;
					v = (word >> (x & 31)) & 1 ? 255 : 0;
				}
			}
			b64s_byte(&b, v);
		}
	}
	b64s_end(&b);
	put(w, "\"/>\n");
}

static const char *mime_of(const char *path) {
	const char *e = strrchr(path, '.');
	char x[6];
	int i;
	if (!e) return NULL;
	for (i = 0; i < 5 && e[i + 1]; i++) x[i] = (char)(e[i + 1] | 32);
	x[i] = 0;
	if (!strcmp(x, "jpg") || !strcmp(x, "jpeg")) return "image/jpeg";
	if (!strcmp(x, "png")) return "image/png";
	if (!strcmp(x, "gif")) return "image/gif";
	if (!strcmp(x, "bmp")) return "image/bmp";
	return NULL;
}

static void image_file(wr_t *w, const zp_op_t *op) {
	const char *mime = mime_of(op->path);
	uint8_t *d = NULL;
	uint32_t n = 0;
	put(w, "<image preserveAspectRatio=\"none\"");
	put_xywh(w, op->dx, op->dy, op->dw, op->dh);
	// grey, as Zeitlos shows and prints it
	put(w, " style=\"filter:grayscale(1)\"");
	if (mime && w->s->load) d = w->s->load(w->s->ctx, op->path, &n);
	if (d) {
		put(w, " href=\"data:"); put(w, mime); put(w, ";base64,");
		put_b64(w, d, n);
		if (w->s->release) w->s->release(w->s->ctx, d);
	} else {
		// linked, as it is on the card
		put(w, " href=\"");
		put_text(w, op->path, (uint32_t)strlen(op->path));
	}
	put(w, "\"/>\n");
}

// -- text ----------------------------------------------------------------------------------

static const char *family_of(const char *font) {
	if (!strncmp(font, "serif", 5)) return "serif";
	if (!strncmp(font, "script", 6)) return "cursive";
	if (!strncmp(font, "gothic", 6)) return "fantasy";
	if (font[0] == 'm' && ((font[1] >= '0' && font[1] <= '9') || !strncmp(font, "mono", 4))) return "monospace";
	return "sans-serif";
}

static void put_m16(wr_t *w, int32_t v) { put_dec(w, v, 65536); }

static void text(wr_t *w, const zp_op_t *op) {
	uint32_t n = op->tlen;
	if (!n) return;
	while (n && (op->text[n - 1] == '\n' || op->text[n - 1] == '\r')) n--;
	put(w, "<text");
	// the run's transform, as the printer has it: device units out
	put(w, " transform=\"matrix(");
	put_m16(w, op->xf.a); put(w, " "); put_m16(w, op->xf.b); put(w, " ");
	put_m16(w, op->xf.c); put(w, " "); put_m16(w, op->xf.d); put(w, " ");
	put_dec(w, op->xf.e, (int64_t)ZC_ONE); put(w, " "); put_dec(w, op->xf.f, (int64_t)ZC_ONE);
	// ... and the whole page scaled from device units to page units
	put(w, ")\" x=\""); put_dec(w, op->x, ZC_ONE);
	put(w, "\" y=\""); put_dec(w, op->y, ZC_ONE);
	put(w, "\" font-family=\""); put(w, family_of(op->font));
	put(w, "\" font-size=\""); put_dec(w, op->size, ZC_ONE);
	put(w, "\"");
	if (w->s->font) {
		const zc_font_t *f = NULL;
		if (w->s->font(w->s->ctx, op->font, &f) && f) {
			zc_style_t st;
			zc_fix wd;
			memset(&st, 0, sizeof(st));
			st.font = f;
			st.size = op->size;
			st.weight = op->weight;
			st.track = op->track;
			st.bold = op->bold;
			st.italic = op->italic;
			wd = zc_text_width(&st, op->text, n);
			if (wd > 0) {
				put(w, " textLength=\""); put_dec(w, wd, ZC_ONE);
				put(w, "\" lengthAdjust=\"spacingAndGlyphs\"");
			}
		}
	}
	if (op->bold || strchr(op->font, 'b')) put(w, " font-weight=\"bold\"");
	if (op->italic || (strlen(op->font) > 1 && op->font[strlen(op->font) - 1] == 'i')) put(w, " font-style=\"italic\"");
	put_paint(w, "fill", &op->paint, -1);
	put(w, " xml:space=\"preserve\">");
	put_text(w, op->text, n);
	put(w, "</text>\n");
}

// -- the page -------------------------------------------------------------------------------

bool zp_svg_page(zp_svg_t *s, const void *job, uint32_t len, int index) {
	static uint32_t start[256], bytes[256];
	zp_reader_t r;
	zp_rec_t rec;
	zp_page_t pi;
	wr_t w;
	int npages = zp_pages(job, len, start, bytes, 256);
	if (index < 0 || index >= npages || index >= 256) return false;
	memset(&w, 0, sizeof(w));
	w.s = s;
	w.ok = true;
	if (!zp_open(&r, (const uint8_t *)job + start[index], bytes[index])) {
		// the page's bytes alone have no header: read it in place
		r.data = (const uint8_t *)job + start[index];
		r.len = bytes[index];
		r.pos = 0;
		r.bad = false;
	}
	if (!zp_next(&r, &rec) || rec.op != ZP_PAGE || !zp_page_info(&rec, &pi)) return false;

	put(&w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"");
	if (pi.kind == ZPAGE_PAPER && pi.w_mm10) {
		put_dec(&w, pi.w_mm10, 10); put(&w, "mm\" height=\""); put_dec(&w, pi.h_mm10, 10); put(&w, "mm\"");
	} else {
		put_int(&w, pi.gw); put(&w, "\" height=\""); put_int(&w, pi.gh); put(&w, "\"");
	}
	put(&w, " viewBox=\"0 0 "); put_int(&w, pi.gw); put(&w, " "); put_int(&w, pi.gh); put(&w, "\">\n");
	// text runs are in device units (ZP_SUB a page unit): one group
	// scales them, the rest is written in page units already
	put(&w, "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n");

	while (w.ok && zp_next(&r, &rec)) {
		zp_op_t op;
		if (rec.op == ZP_END) break;
		if (!zp_decode(&rec, &op)) continue;
		switch (rec.op) {
		case ZP_CLIP:
			if (w.in_clip) put(&w, "</g>\n");
			put(&w, "<clipPath id=\"c"); put_int(&w, w.nclip); put(&w, "\"><rect");
			put_xywh(&w, ZC_I(op.clip[0]), ZC_I(op.clip[1]), ZC_I(op.clip[2] - op.clip[0]), ZC_I(op.clip[3] - op.clip[1]));
			put(&w, "/></clipPath><g clip-path=\"url(#c"); put_int(&w, w.nclip); put(&w, ")\">\n");
			w.nclip++;
			w.in_clip = true;
			break;
		case ZP_RECT: {
			int pid = op.paint.kind == ZC_P_PATTERN ? pattern_id(&w, &op.paint) : -1;
			put(&w, "<rect");
			put_xywh(&w, op.box[0], op.box[1], op.box[2] - op.box[0], op.box[3] - op.box[1]);
			put_paint(&w, "fill", &op.paint, pid);
			put(&w, "/>\n");
			break;
		}
		case ZP_FILL: {
			int pid = op.paint.kind == ZC_P_PATTERN ? pattern_id(&w, &op.paint) : -1;
			put(&w, "<path");
			put_path_d(&w, &op, true);
			put_paint(&w, "fill", &op.paint, pid);
			if (op.rule == ZC_EVENODD) put(&w, " fill-rule=\"evenodd\"");
			put(&w, "/>\n");
			break;
		}
		case ZP_STROKE: {
			int pid = op.paint.kind == ZC_P_PATTERN ? pattern_id(&w, &op.paint) : -1;
			put(&w, "<path fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\"");
			put_path_d(&w, &op, op.closed != 0);
			put_paint(&w, "stroke", &op.paint, pid);
			put(&w, " stroke-width=\"");
			if (op.width) put_u(&w, op.width); else put(&w, "0.1");
			put(&w, "\"/>\n");
			break;
		}
		case ZP_IMAGE:
			image_pixels(&w, &op);
			break;
		case ZP_IMAGE_FILE:
			image_file(&w, &op);
			break;
		case ZP_TEXT:
			{
				// 1 / ZP_SUB, to six places (0.0625)
				char b[12];
				int k = 0, i;
				long rem = 1;
				b[k++] = '0'; b[k++] = '.';
				for (i = 0; i < 6 && rem; i++) { rem *= 10; b[k++] = (char)('0' + rem / ZP_SUB); rem %= ZP_SUB; }
				b[k] = 0;
				put(&w, "<g transform=\"scale(");
				put(&w, b);
				put(&w, ")\">");
			}
			text(&w, &op);
			put(&w, "</g>");
			break;
		default:
			break;
		}
	}
	if (w.in_clip) put(&w, "</g>\n");
	put(&w, "</svg>\n");
	return w.ok;
}
