/*
 * print -- PDF and PostScript. See vec.h.
 */

#include "vec.h"

#include <string.h>

#include "../../common/zutf8.h"

// -- small writers -------------------------------------------------------

static void s_(vec_t *v, const char *s) { out_str(v->o, s); }
static void i_(vec_t *v, int32_t n) { out_int(v->o, n); out_put(v->o, " ", 1); }
static void x_(vec_t *v, int32_t f) { out_fix(v->o, f); out_put(v->o, " ", 1); }	// 24.8
static void m_(vec_t *v, int32_t f) { out_f16(v->o, f); out_put(v->o, " ", 1); }	// 16.16

// a 64-bit 24.8 value (products of transforms)
static void x64_(vec_t *v, int64_t f) {
	char b[32];
	out_put(v->o, b, (uint32_t)fmt_dec(b, f, 8, 2));
	out_put(v->o, " ", 1);
}

// grey of a tone, as zcanvas makes it: 0..1
static void grey_(vec_t *v, int tone) {
	char b[16];
	int g = 255 - (tone * 255 + 8) / 16;
	out_put(v->o, b, (uint32_t)fmt_dec(b, ((int64_t)g << 16) / 255, 16, 3));
	out_put(v->o, " ", 1);
}

static const char hexd[] = "0123456789ABCDEF";

static void hex_(vec_t *v, const uint8_t *d, uint32_t n) {
	char b[2];
	uint32_t i;
	for (i = 0; i < n; i++) {
		b[0] = hexd[d[i] >> 4];
		b[1] = hexd[d[i] & 15];
		out_put(v->o, b, 2);
		if ((i & 31) == 31) out_put(v->o, "\n", 1);
	}
}

static void lit_(vec_t *v, const char *s) {
	// a PDF / PostScript (string), escaped
	out_put(v->o, "(", 1);
	for (; *s; s++) {
		if (*s == '(' || *s == ')' || *s == '\\') out_put(v->o, "\\", 1);
		if ((uint8_t)*s >= 32 && (uint8_t)*s < 127) out_put(v->o, s, 1);
		else out_put(v->o, "?", 1);
	}
	out_put(v->o, ")", 1);
}

// -- run-length coding (PDF /RunLengthDecode, PostScript RunLengthDecode) --

typedef struct {
	vec_t		*v;
	bool		a85;		// PostScript: ASCII85 on top
	uint8_t		lit[128];
	int			nlit;
	int			last, reps;	// a pending repeat
	// ASCII85 state
	uint8_t		q[4];
	int			nq, col;
} rl_t;

static void a85_flush4(rl_t *r, bool final) {
	uint32_t w = 0;
	int i, n = r->nq;
	char c[5];
	if (!n) return;
	for (i = 0; i < 4; i++) w = (w << 8) | (i < n ? r->q[i] : 0);
	if (!final || n == 4) {
		if (w == 0 && n == 4) {
			out_put(r->v->o, "z", 1);
			r->col++;
			r->nq = 0;
			if (r->col >= 72) { out_put(r->v->o, "\n", 1); r->col = 0; }
			return;
		}
	}
	for (i = 4; i >= 0; i--) { c[i] = (char)('!' + w % 85); w /= 85; }
	out_put(r->v->o, c, (uint32_t)(n + 1));
	r->col += n + 1;
	r->nq = 0;
	if (r->col >= 72) { out_put(r->v->o, "\n", 1); r->col = 0; }
}

static void raw_(rl_t *r, const uint8_t *d, int n) {
	int i;
	if (!r->a85) { out_put(r->v->o, d, (uint32_t)n); return; }
	for (i = 0; i < n; i++) {
		r->q[r->nq++] = d[i];
		if (r->nq == 4) a85_flush4(r, false);
	}
}

static void rl_lits(rl_t *r) {
	uint8_t n;
	if (!r->nlit) return;
	n = (uint8_t)(r->nlit - 1);
	raw_(r, &n, 1);
	raw_(r, r->lit, r->nlit);
	r->nlit = 0;
}

static void rl_rep(rl_t *r) {
	uint8_t b[2];
	if (!r->reps) return;
	if (r->reps == 1) {
		r->lit[r->nlit++] = (uint8_t)r->last;
		if (r->nlit == 128) rl_lits(r);
	} else {
		rl_lits(r);
		b[0] = (uint8_t)(257 - r->reps);
		b[1] = (uint8_t)r->last;
		raw_(r, b, 2);
	}
	r->reps = 0;
}

static void rl_byte(rl_t *r, uint8_t c) {
	if (r->reps && c == r->last && r->reps < 128) { r->reps++; return; }
	rl_rep(r);
	r->last = c;
	r->reps = 1;
}

static void rl_bytes(rl_t *r, const uint8_t *d, uint32_t n) {
	uint32_t i;
	for (i = 0; i < n; i++) rl_byte(r, d[i]);
}

static void rl_begin(rl_t *r, vec_t *v, bool a85) {
	memset(r, 0, sizeof(*r));
	r->v = v;
	r->a85 = a85;
}

static void rl_end(rl_t *r) {
	uint8_t eod = 128;
	rl_rep(r);
	rl_lits(r);
	raw_(r, &eod, 1);
	if (r->a85) {
		a85_flush4(r, true);
		out_put(r->v->o, "~>", 2);
	}
}

// -- PDF objects ----------------------------------------------------------------

static int new_obj(vec_t *v) {
	if (v->nobj + 1 >= v->xref_cap) { v->skipped++; return v->xref_cap - 1; }
	return ++v->nobj;
}

static void obj_at(vec_t *v, int n) {
	// offsets from the file's first byte, not the stream's: over IPP the
	// request's attributes come first
	if (n > 0 && n < v->xref_cap) v->xref[n] = v->o->total - v->base;
	i_(v, n);
	s_(v, "0 obj\n");
}

static void end_obj(vec_t *v) { s_(v, "\nendobj\n"); }

// -- fonts -------------------------------------------------------------------------

static vfont_t *font_for(vec_t *v, const zp_op_t *op, const zc_font_t *f, int from) {
	zc_style_t st;
	uint8_t it, bd;
	int32_t pen = 0;
	int i;
	memset(&st, 0, sizeof(st));
	st.font = f;
	st.size = op->size;
	st.weight = op->weight;
	st.bold = op->bold;
	st.italic = op->italic;
	if (f->kind == ZC_FONT_STROKE) {
		it = op->italic && !(f->flags & ZC_FONT_ITALIC);
		bd = op->bold && !(f->flags & ZC_FONT_BOLD);
		if (op->size > 0)
			pen = (int32_t)((((int64_t)zc_text_pen(&st) * f->upm) << 8) / op->size);
		pen = (pen + 8) & ~15;		// a sixteenth of a unit: one font per weight
	} else {
		it = op->italic ? 1 : 0;
		bd = op->bold ? 1 : 0;
	}
	for (i = from; i < v->nfonts; i++) {
		vfont_t *vf = &v->fonts[i];
		if (vf->font == f && vf->italic == it && vf->bold == bd && vf->pen == pen && vf->n < 256)
			return vf;
	}
	if (v->nfonts >= VEC_FONTS) return NULL;
	{
		vfont_t *vf = &v->fonts[v->nfonts];
		memset(vf, 0, sizeof(*vf));
		memcpy(vf->name, f->name, sizeof(vf->name) < sizeof(f->name) ? sizeof(vf->name) : sizeof(f->name));
		vf->name[sizeof(vf->name) - 1] = 0;
		vf->font = f;
		vf->italic = it;
		vf->bold = bd;
		vf->pen = pen;
		vf->id = v->nfonts + 1;
		vf->obj = v->fmt == VEC_PDF ? new_obj(v) : 0;
		v->nfonts++;
		return vf;
	}
}

static int code_of(vfont_t *vf, uint32_t c) {
	int i;
	for (i = 0; i < vf->n; i++) if (vf->cp[i] == c) return i;
	if (vf->n >= 256) return -1;
	vf->cp[vf->n] = (uint16_t)c;
	return vf->n++;
}

static const zc_font_t *load_font(vec_t *v, const char *name) {
	const zc_font_t *f = NULL;
	if (!v->font || !v->font(v->ctx, name, &f) || !f) f = v->fallback;
	return f;
}

// The glyph's advance and its drawing, in font units.
static int32_t glyph_adv(const vfont_t *vf, uint32_t c) {
	const zc_font_t *f = vf->font;
	if (f->kind == ZC_FONT_STROKE) {
		int adv = (f->upm * 5) / 8;
		if (c) zc_font_strokes(f, c, NULL, &adv, NULL);
		return adv + (vf->bold ? 1 : 0);
	}
	return f->w + (vf->bold ? 1 : 0);
}

static void glyph_proc(vec_t *v, const vfont_t *vf, uint32_t c) {
	const zc_font_t *f = vf->font;
	int adv = glyph_adv(vf, c);
	bool ps = v->fmt == VEC_PS;
	i_(v, adv);
	s_(v, ps ? "0 setcharwidth\n" : "0 d0\n");
	if (f->kind == ZC_FONT_STROKE) {
		int n = 0, i;
		const int8_t *p = c ? zc_font_strokes(f, c, &n, NULL, NULL) : NULL;
		x_(v, vf->pen);
		s_(v, "w 1 J 1 j\n");
		if (!p) {
			// the box drawn for a missing character
			int a = (f->upm * 5) / 8, bh = f->cap;
			i_(v, a / 8); i_(v, -bh); i_(v, (a * 3) / 4); i_(v, bh);
			s_(v, "re S\n");
			return;
		}
		{
			bool up = true;
			int lx = 0, ly = 0, run = 0;
			for (i = 0; i < n; i++) {
				int gx = p[2 * i], gy = p[2 * i + 1];
				if (gx == -128) {
					if (run == 1) { i_(v, lx); i_(v, ly); s_(v, "l\n"); }	// a dot
					up = true;
					run = 0;
					continue;
				}
				i_(v, gx); i_(v, gy);
				s_(v, up ? "m " : "l ");
				lx = gx; ly = gy;
				up = false;
				run++;
			}
			if (run == 1) { i_(v, lx); i_(v, ly); s_(v, "l "); }
			s_(v, "S\n");
		}
		return;
	}
	{
		// a bitmap glyph: its rows as an image mask, bold and italic baked in
		const uint8_t *rows = c ? zc_font_rows(f, c) : NULL;
		int w = f->w, h = f->h, slant = vf->italic ? (h - 1) / 4 : 0;
		int mw = w + (vf->bold ? 1 : 0) + slant, bpr = (mw + 7) / 8, y, k;
		uint8_t line[16];
		if (!rows || h > 64 || bpr > 16) {
			i_(v, w / 8); i_(v, -(int)f->asc); i_(v, (w * 3) / 4); i_(v, f->asc);
			s_(v, ps ? "re 1 setlinewidth S\n" : "re 1 w S\n");
			return;
		}
		if (ps) {
			s_(v, "0 "); i_(v, -(int)f->asc); s_(v, "translate ");
			i_(v, mw); i_(v, h); s_(v, "true [1 0 0 1 0 0] <");
		} else {
			s_(v, "q "); i_(v, mw); s_(v, "0 0 "); i_(v, -h); s_(v, "0 "); i_(v, h - f->asc);
			s_(v, "cm BI /IM true /W "); i_(v, mw); s_(v, "/H "); i_(v, h);
			s_(v, "/BPC 1 /D [1 0] /F /AHx ID\n");
		}
		for (y = 0; y < h; y++) {
			uint64_t ink = 0;
			for (k = 0; k < f->stride && k < 8; k++)
				ink = (ink << 8) | rows[y * f->stride + k];
			// MSB-first: pixel 0 is bit (8 * stride - 1)
			ink >>= (8 * f->stride - w);		// right-align the w pixels
			memset(line, 0, sizeof(line));
			for (k = 0; k < w; k++) {
				if ((ink >> (w - 1 - k)) & 1) {
					int x = k + (slant ? (h - 1 - y) / 4 : 0);
					line[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
					if (vf->bold) line[(x + 1) >> 3] |= (uint8_t)(0x80 >> ((x + 1) & 7));
				}
			}
			hex_(v, line, (uint32_t)bpr);
		}
		s_(v, ps ? "> imagemask\n" : ">\nEI Q\n");
	}
}

// -- PostScript prolog -----------------------------------------------------------

static const char ps_prolog[] =
	"/ZL 64 dict def ZL begin\n"
	"/q {gsave} bind def /Q {grestore} bind def\n"
	"/cm {6 array astore concat} bind def\n"
	"/m {moveto} bind def /l {lineto} bind def /h {closepath} bind def\n"
	"/re {4 2 roll moveto 1 index 0 rlineto 0 exch rlineto neg 0 rlineto closepath} bind def\n"
	"/f {fill} bind def /f* {eofill} bind def /S {stroke} bind def /n {newpath} bind def\n"
	"/W {clip} bind def /W* {eoclip} bind def\n"
	"/g {setgray} bind def /G {setgray} bind def\n"
	"/w {setlinewidth} bind def /J {setlinecap} bind def /j {setlinejoin} bind def\n"
	"/PG {[/Pattern /DeviceGray] setcolorspace setcolor} bind def\n"
	"/BT {} def /ET {} def /TC 0 def\n"
	"/Tc {/TC exch def} bind def\n"
	"/Tf {exch findfont exch scalefont setfont} bind def\n"
	"/Tm {6 array astore /TM exch def} bind def\n"
	"/Tj {gsave TM concat 0 0 moveto TC 0 3 -1 roll ashow grestore} bind def\n"
	"/T3 {12 dict begin /Procs exch def /FontMatrix exch def /FontBBox [-64 -192 256 128] def\n"
	"  /FontType 3 def /Encoding StandardEncoding def\n"
	"  /BuildChar {exch /Procs get exch get exec} bind def\n"
	"  currentdict end definefont pop} bind def\n"
	"end\n";

static void ps_fonts(vec_t *v) {
	int i, k;
	for (i = 0; i < v->nfonts; i++) {
		const vfont_t *vf = &v->fonts[i];
		const zc_font_t *f = vf->font;
		int upm = f->kind == ZC_FONT_STROKE ? f->upm : f->h;
		out_str(v->o, "/F");
		out_int(v->o, vf->id);
		s_(v, " [");
		// 1/upm, with a fifth of a slant for synthetic italic strokes
		m_(v, (65536 + upm / 2) / upm); s_(v, "0 ");
		if (vf->italic && f->kind == ZC_FONT_STROKE) m_(v, -(65536 / 5 + upm / 2) / upm); else s_(v, "0 ");
		m_(v, (65536 + upm / 2) / upm); s_(v, "0 0]\n[\n");
		for (k = 0; k < vf->n; k++) {
			s_(v, "{");
			glyph_proc(v, vf, vf->cp[k]);
			s_(v, "}\n");
		}
		s_(v, "] T3\n");
	}
}

// -- the content of a sheet ------------------------------------------------------

// Which pattern object (PDF) or name (PostScript) for a paint in a cell.
static int pattern_id(vec_t *v, const uint8_t pat[8], int cell, bool make_pdf,
	const zc_xf_t *cxf, int sheet_h) {
	int i;
	for (i = 0; i < v->npat; i++)
		if (v->pat[i].cell == cell && !memcmp(v->pat[i].pat, pat, 8)) return i;
	if (v->npat >= VEC_PATTERNS || (v->fmt == VEC_PDF && !make_pdf)) return -1;
	i = v->npat++;
	memcpy(v->pat[i].pat, pat, 8);
	v->pat[i].cell = cell;
	v->pat[i].id = ++v->pat_ids;
	v->pat[i].obj = 0;
	if (v->fmt == VEC_PDF) {
		// pattern space: page units of this cell, on the PDF page
		// (y up, points): flip . 0.1 . cell
		int obj = new_obj(v), len_obj = new_obj(v);
		uint32_t at;
		v->pat[i].obj = obj;
		if (v->pat[i].id < VEC_PAT_ALL) v->pat_all[v->pat[i].id] = obj;
		obj_at(v, obj);
		s_(v, "<< /PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 8 8] /XStep 8 /YStep 8\n");
		s_(v, "/Resources << >> /Matrix [");
		m_(v, cxf->a / 10); m_(v, -cxf->b / 10); m_(v, cxf->c / 10); m_(v, -cxf->d / 10);
		x_(v, cxf->e / 10); x_(v, ZC_I(sheet_h) / 10 - cxf->f / 10);
		s_(v, "] /Length "); i_(v, len_obj); s_(v, "0 R >>\nstream\n");
		at = v->o->total;
		s_(v, "q 8 0 0 -8 0 8 cm BI /IM true /W 8 /H 8 /BPC 1 /D [1 0] /F /AHx ID\n");
		hex_(v, pat, 8);
		s_(v, ">\nEI Q\n");
		at = v->o->total - at;
		s_(v, "endstream");
		end_obj(v);
		obj_at(v, len_obj);
		out_int(v->o, (int32_t)at);
		end_obj(v);
	}
	return i;
}

static void paint_(vec_t *v, const zc_paint_t *p, int cell, bool stroke) {
	if (p->kind == ZC_P_PATTERN && !stroke) {
		int i = pattern_id(v, p->pat, cell, false, NULL, 0);
		if (i >= 0) {
			if (v->fmt == VEC_PDF) {
				s_(v, "/CsP cs ");
				grey_(v, p->tone);
				s_(v, "/P"); out_int(v->o, v->pat[i].id); s_(v, " scn\n");
			} else {
				grey_(v, p->tone);
				s_(v, "P"); out_int(v->o, v->pat[i].id); s_(v, " PG\n");
			}
			return;
		}
	}
	grey_(v, p->kind == ZC_P_PATTERN ? p->tone / 2 : p->tone);
	s_(v, stroke ? "G\n" : "g\n");
}

static void path_(vec_t *v, const zp_op_t *op, bool close) {
	int k, i;
	for (k = 0; k < op->nsub; k++) {
		int s0 = zp_sub_at(op, k), s1 = k + 1 < op->nsub ? zp_sub_at(op, k + 1) : op->npts;
		for (i = s0; i < s1; i++) {
			zc_fix x, y;
			zp_point(op, i, &x, &y);
			x_(v, x);
			x_(v, y);
			s_(v, i == s0 ? "m " : "l ");
			if ((i - s0) % 6 == 5) s_(v, "\n");
		}
		if (s1 - s0 == 1) {
			// one point: a dot, as zcanvas strokes it
			zc_fix x, y;
			zp_point(op, s0, &x, &y);
			x_(v, x); x_(v, y); s_(v, "l ");
		}
		if (close) s_(v, "h ");
	}
}

static void image_inline(vec_t *v, const zp_op_t *op) {
	rl_t r;
	int y;
	bool ps = v->fmt == VEC_PS;
	if (op->kind == ZC_IMG_MASK) paint_(v, &op->paint, -1, false);
	s_(v, "q ");
	if (ps) {
		x_(v, op->dx); x_(v, op->dy); s_(v, "translate "); x_(v, op->dw); x_(v, op->dh); s_(v, "scale ");
		i_(v, op->sw); i_(v, op->sh);
		s_(v, op->kind == ZC_IMG_GRAY ? "8 " : op->kind == ZC_IMG_BITS ? "1 " : "false ");
		s_(v, "["); i_(v, op->sw); s_(v, "0 0 "); i_(v, op->sh); s_(v, "0 0] currentfile /ASCII85Decode filter /RunLengthDecode filter ");
		s_(v, op->kind == ZC_IMG_MASK ? "imagemask\n" : "image\n");
	} else {
		x_(v, op->dw); s_(v, "0 0 "); x_(v, -op->dh); x_(v, op->dx); x_(v, op->dy + op->dh);
		s_(v, "cm BI /W "); i_(v, op->sw); s_(v, "/H "); i_(v, op->sh);
		if (op->kind == ZC_IMG_GRAY) s_(v, "/BPC 8 /CS /G");
		else if (op->kind == ZC_IMG_BITS) s_(v, "/BPC 1 /CS /G");
		else s_(v, "/IM true");
		s_(v, " /F /RL ID\n");
	}
	rl_begin(&r, v, ps);
	for (y = 0; y < op->sh; y++) {
		const uint8_t *row = op->pix + (size_t)y * op->rowbytes;
		if (op->kind == ZC_IMG_GRAY) {
			rl_bytes(&r, row, (uint32_t)op->sw);
		} else {
			// framebuffer packing (bit 0 leftmost) to MSB-first bytes
			int x;
			for (x = 0; x < op->sw; x += 8) {
				uint8_t b = 0;
				int k;
				for (k = 0; k < 8; k++) {
					int px = x + k;
					int bit = px < op->sw ? (row[px >> 3] >> (px & 7)) & 1 : 1;
					b = (uint8_t)(b | (bit << (7 - k)));
				}
				rl_byte(&r, b);
			}
		}
	}
	rl_end(&r);
	s_(v, ps ? "\nQ\n" : "\nEI Q\n");
}

static bool image_file_pdf(vec_t *v, const zp_op_t *op, int *id) {
	int i;
	for (i = 0; i < v->nimg; i++) {
		if (!strcmp(v->img[i].path, op->path) && v->img[i].contrast == op->contrast &&
			v->img[i].bright == op->bright) { *id = v->img[i].id; return true; }
	}
	{
		const uint8_t *pix;
		int w, h, y, obj, len_obj;
		uint32_t at;
		rl_t r;
		if (v->nimg >= VEC_IMAGES || !v->image ||
			!v->image(v->ctx, op->path, op->contrast, op->bright, 4096, 4096, &pix, &w, &h))
			return false;
		obj = new_obj(v);
		len_obj = new_obj(v);
		i = v->nimg++;
		memcpy(v->img[i].path, op->path, sizeof(v->img[i].path));
		v->img[i].contrast = (int16_t)op->contrast;
		v->img[i].bright = (int16_t)op->bright;
		v->img[i].obj = obj;
		v->img[i].id = i + 1;
		obj_at(v, obj);
		s_(v, "<< /Type /XObject /Subtype /Image /Width "); i_(v, w); s_(v, "/Height "); i_(v, h);
		s_(v, "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /RunLengthDecode /Length ");
		i_(v, len_obj); s_(v, "0 R >>\nstream\n");
		at = v->o->total;
		rl_begin(&r, v, false);
		for (y = 0; y < h; y++) rl_bytes(&r, pix + (size_t)y * w, (uint32_t)w);
		rl_end(&r);
		at = v->o->total - at;
		s_(v, "\nendstream");
		end_obj(v);
		obj_at(v, len_obj);
		out_int(v->o, (int32_t)at);
		end_obj(v);
		*id = v->img[i].id;
		return true;
	}
}

static void image_file_ps(vec_t *v, const zp_op_t *op) {
	const uint8_t *pix;
	int w, h, y;
	rl_t r;
	if (!v->image || !v->image(v->ctx, op->path, op->contrast, op->bright, 4096, 4096, &pix, &w, &h)) {
		v->skipped++;
		return;
	}
	s_(v, "q "); x_(v, op->dx); x_(v, op->dy); s_(v, "translate "); x_(v, op->dw); x_(v, op->dh);
	s_(v, "scale "); i_(v, w); i_(v, h); s_(v, "8 ["); i_(v, w); s_(v, "0 0 "); i_(v, h);
	s_(v, "0 0] currentfile /ASCII85Decode filter /RunLengthDecode filter image\n");
	rl_begin(&r, v, true);
	for (y = 0; y < h; y++) rl_bytes(&r, pix + (size_t)y * w, (uint32_t)w);
	rl_end(&r);
	s_(v, "\nQ\n");
}

static void text_(vec_t *v, const zp_op_t *op) {
	const zc_font_t *f = load_font(v, op->font);
	const char *s = op->text, *e = op->text + op->tlen;
	vfont_t *vf;
	int from = 0;
	int64_t ex, ey;
	if (!f) { v->skipped++; return; }
	vf = font_for(v, op, f, 0);
	if (!vf) { v->skipped++; return; }
	// text space: user units at (x, y), through the run's transform
	ex = ((int64_t)op->xf.a * op->x + (int64_t)op->xf.c * op->y) / 65536 + op->xf.e;
	ey = ((int64_t)op->xf.b * op->x + (int64_t)op->xf.d * op->y) / 65536 + op->xf.f;
	paint_(v, &op->paint, -1, false);
	paint_(v, &op->paint, -1, true);
	s_(v, "BT /F"); out_int(v->o, vf->id); s_(v, " "); x_(v, op->size); s_(v, "Tf ");
	x_(v, op->track); s_(v, "Tc ");
	m_(v, op->xf.a); m_(v, op->xf.b); m_(v, op->xf.c); m_(v, op->xf.d);
	x64_(v, ex); x64_(v, ey); s_(v, "Tm <");
	while (s < e) {
		uint32_t cp = z_utf8_next(&s, e);
		uint32_t c;
		int code;
		if (cp == '\n' || cp == '\r') continue;
		c = zc_font_resolve(f, cp);
		code = code_of(vf, c);
		if (code < 0) {
			// 256 glyphs in this font: go on in a second one
			vfont_t *nf;
			s_(v, "> Tj ET\n");
			from = (int)(vf - v->fonts) + 1;
			nf = font_for(v, op, f, from);
			if (!nf) { v->skipped++; return; }
			vf = nf;
			code = code_of(vf, c);
			// the pen moved: continue from where the run had got to
			{
				zc_style_t st;
				const char *done = op->text;
				memset(&st, 0, sizeof(st));
				st.font = f; st.size = op->size; st.weight = op->weight;
				st.track = op->track; st.bold = op->bold; st.italic = op->italic;
				{
					const char *prev = s;
					// back up over this character to measure what came before
					const char *q = op->text;
					const char *last = op->text;
					while (q < prev) { last = q; z_utf8_next(&q, e); }
					done = last;
				}
				{
					zc_fix adv = zc_text_width(&st, op->text, (uint32_t)(done - op->text));
					zc_fix nx = op->x + adv;
					ex = ((int64_t)op->xf.a * nx + (int64_t)op->xf.c * op->y) / 65536 + op->xf.e;
					ey = ((int64_t)op->xf.b * nx + (int64_t)op->xf.d * op->y) / 65536 + op->xf.f;
				}
			}
			s_(v, "BT /F"); out_int(v->o, vf->id); s_(v, " "); x_(v, op->size); s_(v, "Tf ");
			x_(v, op->track); s_(v, "Tc ");
			m_(v, op->xf.a); m_(v, op->xf.b); m_(v, op->xf.c); m_(v, op->xf.d);
			x64_(v, ex); x64_(v, ey); s_(v, "Tm <");
		}
		{
			char b[2] = { hexd[(code >> 4) & 15], hexd[code & 15] };
			out_put(v->o, b, 2);
		}
	}
	s_(v, "> Tj ET\n");
}

// Pass 1 (PDF): the objects a sheet's content refers to -- pictures and
// patterns -- written before it, since a content stream cannot hold them.
static void sheet_objects(vec_t *v, const int *page, const zpage_cell_t *cell, int n,
	int sheet_h) {
	int i;
	for (i = 0; i < n; i++) {
		zp_reader_t r;
		zp_rec_t rec;
		zp_op_t op;
		r.data = v->job->data + v->job->start[page[i]];
		r.len = v->job->bytes[page[i]];
		r.pos = 0;
		r.bad = false;
		while (zp_next(&r, &rec)) {
			if (rec.op == ZP_END) break;
			if (!zp_decode(&rec, &op)) continue;
			if (op.op == ZP_IMAGE_FILE) {
				int id;
				if (!image_file_pdf(v, &op, &id)) v->skipped++;
			} else if ((op.op == ZP_FILL || op.op == ZP_RECT) && op.paint.kind == ZC_P_PATTERN) {
				pattern_id(v, op.paint.pat, i, true, &cell[i].xf, sheet_h);
			}
		}
	}
}

static void page_content(vec_t *v, int pi, int ci, const zpage_cell_t *c) {
	zp_reader_t r;
	zp_rec_t rec;
	zp_op_t op;
	bool clipped = false;
	zc_xf_t sub = { 65536 / ZP_SUB, 0, 0, 65536 / ZP_SUB, 0, 0 };
	zc_xf_t xd = zc_xf_mul(&c->xf, &sub);

	s_(v, "q "); i_(v, c->x); i_(v, c->y); i_(v, c->w); i_(v, c->h); s_(v, "re W n\n");
	m_(v, xd.a); m_(v, xd.b); m_(v, xd.c); m_(v, xd.d); x_(v, xd.e); x_(v, xd.f); s_(v, "cm\n");
	if (v->fmt == VEC_PS) {
		// patterns for this cell, made here so they take its transform:
		// pattern space is page units (16 of the units here)
		int i;
		for (i = 0; i < v->npat; i++) {
			if (v->pat[i].cell != ci) continue;
			s_(v, "/P"); out_int(v->o, v->pat[i].id);
			s_(v, " << /PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 8 8] /XStep 8 /YStep 8\n");
			s_(v, "/PaintProc {pop 8 8 true [1 0 0 1 0 0] <");
			hex_(v, v->pat[i].pat, 8);
			s_(v, "> imagemask} >> [16 0 0 16 0 0] makepattern def\n");
		}
	}
	s_(v, "1 J 1 j\n");

	r.data = v->job->data + v->job->start[pi];
	r.len = v->job->bytes[pi];
	r.pos = 0;
	r.bad = false;
	while (zp_next(&r, &rec)) {
		if (rec.op == ZP_END) break;
		if (!zp_decode(&rec, &op)) continue;
		if (op.paint.kind == ZC_P_XOR && op.op != ZP_CLIP && op.op != ZP_IMAGE_FILE) continue;
		switch (op.op) {
		case ZP_CLIP:
			if (clipped) s_(v, "Q ");
			s_(v, "q ");
			i_(v, op.clip[0]); i_(v, op.clip[1]); i_(v, op.clip[2] - op.clip[0]); i_(v, op.clip[3] - op.clip[1]);
			s_(v, "re W n\n");
			clipped = true;
			break;
		case ZP_RECT:
			if (op.paint.kind == ZC_P_PATTERN && op.paint.opaque) {
				s_(v, "1 g "); x_(v, op.box[0]); x_(v, op.box[1]); x_(v, op.box[2] - op.box[0]);
				x_(v, op.box[3] - op.box[1]); s_(v, "re f\n");
			}
			paint_(v, &op.paint, ci, false);
			x_(v, op.box[0]); x_(v, op.box[1]); x_(v, op.box[2] - op.box[0]); x_(v, op.box[3] - op.box[1]);
			s_(v, "re f\n");
			break;
		case ZP_FILL:
			if (op.paint.kind == ZC_P_PATTERN && op.paint.opaque) {
				s_(v, "1 g ");
				path_(v, &op, false);
				s_(v, op.rule == ZC_EVENODD ? "f*\n" : "f\n");
			}
			paint_(v, &op.paint, ci, false);
			path_(v, &op, false);
			s_(v, op.rule == ZC_EVENODD ? "f*\n" : "f\n");
			break;
		case ZP_STROKE:
			paint_(v, &op.paint, ci, true);
			x_(v, op.width);
			s_(v, "w ");
			path_(v, &op, op.closed != 0);
			s_(v, "S\n");
			break;
		case ZP_IMAGE:
			image_inline(v, &op);
			break;
		case ZP_IMAGE_FILE:
			if (v->fmt == VEC_PDF) {
				int id;
				if (!image_file_pdf(v, &op, &id)) break;
				s_(v, "q "); x_(v, op.dw); s_(v, "0 0 "); x_(v, -op.dh); x_(v, op.dx); x_(v, op.dy + op.dh);
				s_(v, "cm /Im"); out_int(v->o, id); s_(v, " Do Q\n");
			} else {
				image_file_ps(v, &op);
			}
			break;
		case ZP_TEXT:
			text_(v, &op);
			break;
		default:
			break;
		}
	}
	if (clipped) s_(v, "Q ");
	s_(v, "Q\n");
}

static void sheet_marks(vec_t *v, const zpage_sheet_t *sh, const zpage_cell_t *cell, int n) {
	static int marks[JOB_MAX_NUP * 8][4];
	int nm, i;
	if (v->job->frame) {
		s_(v, "0.375 G 2.5 w\n");
		for (i = 0; i < n; i++) {
			i_(v, cell[i].x); i_(v, cell[i].y); i_(v, cell[i].w); i_(v, cell[i].h); s_(v, "re S\n");
		}
	}
	if (!v->job->cut || n <= 0) return;
	nm = zpage_cut_marks(sh, cell, n, zpage_mm10_to_px(50, sh->dpi), marks, JOB_MAX_NUP * 8);
	s_(v, "0 G 2.5 w 0 J\n");
	for (i = 0; i < nm; i++) {
		i_(v, marks[i][0]); i_(v, marks[i][1]); s_(v, "m ");
		i_(v, marks[i][2]); i_(v, marks[i][3]); s_(v, "l S\n");
	}
}

// -- the container ------------------------------------------------------------------

bool vec_begin(vec_t *v) {
	v->base = v->o->total;
	v->nobj = 0;
	v->nkids = 0;
	v->nfonts = 0;
	v->nimg = 0;
	v->npat = 0;
	v->pat_ids = 0;
	memset(v->pat_all, 0, sizeof(v->pat_all));
	v->sheet_no = 0;
	v->skipped = 0;
	if (v->fmt == VEC_PDF) {
		// 1 catalog, 2 pages, 3 resources, 4 info: written at the end but
		// numbered now
		v->nobj = 4;
		s_(v, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
		return !v->o->failed;
	}
	{
		// PostScript: fonts go in the prolog, so find every glyph first
		int first, count, p;
		const zpage_size_t *paper;
		zpage_sheet_t sh;
		int wmm, hmm;
		job_range(v->job, &first, &count);
		for (p = first; p < first + count; p++) {
			zp_reader_t r;
			zp_rec_t rec;
			zp_op_t op;
			r.data = v->job->data + v->job->start[p];
			r.len = v->job->bytes[p];
			r.pos = 0;
			r.bad = false;
			while (zp_next(&r, &rec)) {
				const zc_font_t *f;
				vfont_t *vf;
				const char *s, *e;
				if (rec.op == ZP_END) break;
				if (rec.op != ZP_TEXT || !zp_decode(&rec, &op)) continue;
				f = load_font(v, op.font);
				if (!f) continue;
				vf = font_for(v, &op, f, 0);
				s = op.text;
				e = op.text + op.tlen;
				while (vf && s < e) {
					uint32_t cp = z_utf8_next(&s, e);
					if (cp == '\n' || cp == '\r') continue;
					if (code_of(vf, zc_font_resolve(f, cp)) < 0) {
						vf = font_for(v, &op, f, (int)(vf - v->fonts) + 1);
						if (vf) code_of(vf, zc_font_resolve(f, cp));
					}
				}
			}
		}
		job_sheet_at(v->job, 0, VEC_DPI, NULL, NULL, &sh, &paper, &wmm, &hmm);
		s_(v, "%!PS-Adobe-3.0\n%%Creator: Zeitlos print\n%%Title: ");
		s_(v, v->job->title[0] ? v->job->title : "Zeitlos");
		s_(v, "\n%%Pages: "); i_(v, job_sheets(v->job));
		s_(v, "\n%%LanguageLevel: 2\n%%EndComments\n%%BeginProlog\n");
		s_(v, ps_prolog);
		s_(v, "%%EndProlog\n%%BeginSetup\nZL begin\n");
		ps_fonts(v);
		s_(v, "<< /PageSize ["); x_(v, ZC_I(sh.w) / 10); x_(v, ZC_I(sh.h) / 10);
		s_(v, "] /NumCopies "); i_(v, v->job->copies); s_(v, ">> setpagedevice\n");
		s_(v, "%%EndSetup\n");
	}
	return !v->o->failed;
}

bool vec_sheet(vec_t *v, int s) {
	int page[JOB_MAX_NUP];
	zpage_cell_t cell[JOB_MAX_NUP];
	zpage_sheet_t sh;
	const zpage_size_t *paper;
	int wmm, hmm, n, i;
	job_sheet_at(v->job, s, VEC_DPI, NULL, NULL, &sh, &paper, &wmm, &hmm);
	n = job_sheet(v->job, s, &sh, page, cell);
	v->sheet_no++;
	v->npat = 0;

	if (v->fmt == VEC_PDF) {
		int content, len_obj, pg;
		uint32_t at;
		sheet_objects(v, page, cell, n, sh.h);
		content = new_obj(v);
		len_obj = new_obj(v);
		pg = new_obj(v);
		obj_at(v, content);
		s_(v, "<< /Length "); i_(v, len_obj); s_(v, "0 R >>\nstream\n");
		at = v->o->total;
		// y down, a tenth of a point
		s_(v, "1 0 0 -1 0 "); x_(v, ZC_I(sh.h) / 10); s_(v, "cm 0.1 0 0 0.1 0 0 cm\n");
		for (i = 0; i < n; i++) page_content(v, page[i], i, &cell[i]);
		sheet_marks(v, &sh, cell, n);
		at = v->o->total - at;
		s_(v, "endstream");
		end_obj(v);
		obj_at(v, len_obj);
		out_int(v->o, (int32_t)at);
		end_obj(v);
		obj_at(v, pg);
		s_(v, "<< /Type /Page /Parent 2 0 R /Resources 3 0 R /MediaBox [0 0 ");
		x_(v, ZC_I(sh.w) / 10); x_(v, ZC_I(sh.h) / 10);
		s_(v, "] /Contents "); i_(v, content); s_(v, "0 R >>");
		end_obj(v);
		v->kids[v->nkids++] = pg;
	} else {
		s_(v, "%%Page: "); i_(v, v->sheet_no); i_(v, v->sheet_no);
		s_(v, "\n");
		if (!paper) {
			s_(v, "<< /PageSize ["); x_(v, ZC_I(sh.w) / 10); x_(v, ZC_I(sh.h) / 10); s_(v, "] >> setpagedevice\n");
		}
		s_(v, "save ZL begin\n");
		s_(v, "0 "); x_(v, ZC_I(sh.h) / 10); s_(v, "translate 1 -1 scale 0.1 0.1 scale\n");
		// patterns: collect this sheet's, made inside each cell
		for (i = 0; i < n; i++) {
			zp_reader_t r;
			zp_rec_t rec;
			zp_op_t op;
			r.data = v->job->data + v->job->start[page[i]];
			r.len = v->job->bytes[page[i]];
			r.pos = 0;
			r.bad = false;
			while (zp_next(&r, &rec)) {
				if (rec.op == ZP_END) break;
				if ((rec.op == ZP_FILL || rec.op == ZP_RECT) && zp_decode(&rec, &op) &&
					op.paint.kind == ZC_P_PATTERN)
					pattern_id(v, op.paint.pat, i, false, NULL, 0);
			}
		}
		for (i = 0; i < n; i++) page_content(v, page[i], i, &cell[i]);
		sheet_marks(v, &sh, cell, n);
		s_(v, "end restore showpage\n");
	}
	return !v->o->failed;
}

bool vec_end(vec_t *v) {
	int i, k;
	if (v->fmt == VEC_PS) {
		s_(v, "%%Trailer\nend\n%%EOF\n");
		return !v->o->failed;
	}
	// fonts
	for (i = 0; i < v->nfonts; i++) {
		vfont_t *vf = &v->fonts[i];
		const zc_font_t *f = vf->font;
		int upm = f->kind == ZC_FONT_STROKE ? f->upm : f->h;
		int first_proc = v->nobj + 1;
		for (k = 0; k < vf->n; k++) {
			int obj = new_obj(v), len_obj = new_obj(v);
			uint32_t at;
			obj_at(v, obj);
			s_(v, "<< /Length "); i_(v, len_obj); s_(v, "0 R >>\nstream\n");
			at = v->o->total;
			glyph_proc(v, vf, vf->cp[k]);
			at = v->o->total - at;
			s_(v, "endstream");
			end_obj(v);
			obj_at(v, len_obj);
			out_int(v->o, (int32_t)at);
			end_obj(v);
		}
		obj_at(v, vf->obj);
		s_(v, "<< /Type /Font /Subtype /Type3 /FontBBox [-64 -192 256 128] /FontMatrix [");
		m_(v, (65536 + upm / 2) / upm); s_(v, "0 ");
		if (vf->italic && f->kind == ZC_FONT_STROKE) m_(v, -(65536 / 5 + upm / 2) / upm); else s_(v, "0 ");
		m_(v, (65536 + upm / 2) / upm); s_(v, "0 0]\n/CharProcs <<");
		for (k = 0; k < vf->n; k++) {
			s_(v, " /g"); out_int(v->o, k); s_(v, " "); i_(v, first_proc + 2 * k); s_(v, "0 R");
			if ((k & 7) == 7) s_(v, "\n");
		}
		s_(v, " >>\n/Encoding << /Type /Encoding /Differences [0");
		for (k = 0; k < vf->n; k++) { s_(v, " /g"); out_int(v->o, k); if ((k & 15) == 15) s_(v, "\n"); }
		s_(v, "] >>\n/FirstChar 0 /LastChar "); i_(v, vf->n ? vf->n - 1 : 0);
		s_(v, "/Widths [");
		for (k = 0; k < vf->n; k++) { i_(v, glyph_adv(vf, vf->cp[k])); if ((k & 15) == 15) s_(v, "\n"); }
		s_(v, "] /Resources << >> >>");
		end_obj(v);
	}

	// 3: resources, shared by every page
	obj_at(v, 3);
	s_(v, "<< /ProcSet [/PDF /ImageB /Text]\n/ColorSpace << /CsP [/Pattern /DeviceGray] >>\n/Font <<");
	for (i = 0; i < v->nfonts; i++) {
		s_(v, " /F"); out_int(v->o, v->fonts[i].id); s_(v, " "); i_(v, v->fonts[i].obj); s_(v, "0 R");
	}
	s_(v, " >>\n/XObject <<");
	for (i = 0; i < v->nimg; i++) {
		s_(v, " /Im"); out_int(v->o, v->img[i].id); s_(v, " "); i_(v, v->img[i].obj); s_(v, "0 R");
	}
	s_(v, " >>\n/Pattern <<");
	// every pattern made, on any sheet: numbered across the whole file
	for (i = 1; i <= v->pat_ids && i < VEC_PAT_ALL; i++) {
		if (!v->pat_all[i]) continue;
		s_(v, " /P"); out_int(v->o, i); s_(v, " "); i_(v, v->pat_all[i]); s_(v, "0 R");
		if ((i & 7) == 7) s_(v, "\n");
	}
	s_(v, " >> >>");
	end_obj(v);

	// 2: pages
	obj_at(v, 2);
	s_(v, "<< /Type /Pages /Count "); i_(v, v->nkids); s_(v, "/Kids [");
	for (i = 0; i < v->nkids; i++) { i_(v, v->kids[i]); s_(v, "0 R "); }
	s_(v, "] >>");
	end_obj(v);
	// 1: catalog, 4: info
	obj_at(v, 1);
	s_(v, "<< /Type /Catalog /Pages 2 0 R >>");
	end_obj(v);
	obj_at(v, 4);
	s_(v, "<< /Producer (Zeitlos print) /Title ");
	lit_(v, v->job->title[0] ? v->job->title : "Zeitlos");
	s_(v, " >>");
	end_obj(v);

	// cross-reference
	{
		uint32_t xref_at = v->o->total - v->base;
		char b[24];
		s_(v, "xref\n0 "); out_int(v->o, v->nobj + 1); s_(v, "\n0000000000 65535 f \n");
		for (i = 1; i <= v->nobj; i++) {
			uint32_t off = i < v->xref_cap ? v->xref[i] : 0;
			int d;
			for (d = 9; d >= 0; d--) { b[d] = (char)('0' + off % 10); off /= 10; }
			memcpy(b + 10, " 00000 n \n", 10);
			out_put(v->o, b, 20);
		}
		s_(v, "trailer\n<< /Size "); i_(v, v->nobj + 1); s_(v, "/Root 1 0 R /Info 4 0 R >>\nstartxref\n");
		out_int(v->o, (int32_t)xref_at);
		s_(v, "\n%%EOF\n");
	}
	return !v->o->failed;
}
