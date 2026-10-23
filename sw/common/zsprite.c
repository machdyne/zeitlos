/*
 * Zeitlos -- sprite sheets (.zsp). See zsprite.h and docs/zsp.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

#include "zsprite.h"

/* ===================================================================
 * the palette
 * =================================================================== */

static const uint16_t c64[16] = {
	0x000, 0xfff, 0x633, 0x7aa, 0x748, 0x584, 0x327, 0xbc7,
	0x752, 0x430, 0x965, 0x444, 0x666, 0x9c8, 0x66b, 0x999
};

int zsp_lum(uint16_t rgb) {
	int r = (rgb >> 8) & 15, g = (rgb >> 4) & 15, b = rgb & 15;
	return (r * 2 + g * 5 + b + 4) / 8;
}

static int dist(uint16_t a, uint16_t b) {
	int dr = ((a >> 8) & 15) - ((b >> 8) & 15);
	int dg = ((a >> 4) & 15) - ((b >> 4) & 15);
	int db = (a & 15) - (b & 15);
	return dr * dr * 2 + dg * dg * 4 + db * db;
}

/* The entry 0..n-1 nearest to rgb; the lowest on a tie. */
static int nearest(const zsp_palette_t *p, uint16_t rgb, int n) {
	int i, best = 0, bd = 1 << 30;
	for (i = 0; i < n; i++) {
		int d = dist(p->pal[i].rgb, rgb);
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

void zsp_defaults(zsp_palette_t *p, int i) {
	zsp_colour_t *c = &p->pal[i & 255];
	int l = zsp_lum(c->rgb);
	i &= 255;
	if (!(c->given & ZSP_GIVEN_TO64)) c->to64 = (uint8_t)(i < 64 ? i : nearest(p, c->rgb, 64));
	if (!(c->given & ZSP_GIVEN_TO16)) c->to16 = (uint8_t)(i < 16 ? i : nearest(p, c->rgb, 16));
	if (!(c->given & ZSP_GIVEN_SLOT)) c->slot = (uint8_t)(l * 4 / 16);
	if (!(c->given & ZSP_GIVEN_INK))  c->ink  = (uint8_t)((l * 16 + 7) / 15);
}

void zsp_defaults_all(zsp_palette_t *p) {
	int i;
	for (i = 0; i < 256; i++) zsp_defaults(p, i);
}

/* A shade of an RGB444 colour: toward black (t < 0) or white (t > 0),
 * |t| of 16 of the way. */
static uint16_t shade(uint16_t rgb, int t) {
	int k, out = 0;
	for (k = 0; k < 12; k += 4) {
		int v = (rgb >> k) & 15;
		v = t < 0 ? v + (v * t) / 16 : v + ((15 - v) * t) / 16;
		out |= v << k;
	}
	return (uint16_t)out;
}

void zsp_palette_init(zsp_palette_t *p, int colours) {
	int i;
	memset(p, 0, sizeof(*p));
	p->colours = colours;
	p->slot_rgb[0] = 0x000; p->slot_rgb[1] = 0x555;
	p->slot_rgb[2] = 0xaaa; p->slot_rgb[3] = 0xfff;
	for (i = 0; i < 16; i++) p->pal[i].rgb = c64[i];
	/* 16..63: three shades of each, darker, lighter, darkest;
	 * 64..255: twelve more, from black to white */
	for (i = 16; i < 64; i++) {
		static const int t[3] = { -8, 8, -12 };
		p->pal[i].rgb = shade(c64[i & 15], t[(i - 16) / 16]);
	}
	for (i = 64; i < 256; i++) {
		int k = (i - 64) / 16;                 /* 0..11 */
		int t = k < 6 ? -2 - k * 2 : (k - 5) * 2;
		p->pal[i].rgb = shade(c64[i & 15], t);
	}
	zsp_defaults_all(p);
}

int zsp_reduce(const zsp_palette_t *p, int idx, int target) {
	const zsp_colour_t *c;
	if (idx == ZSP_T || idx < 0) return ZSP_T;
	idx &= 255;
	if (target >= p->colours) return idx;
	c = &p->pal[idx];
	if (target >= 64) return c->to64;
	if (target >= 16) return p->colours > 16 ? c->to16 : idx;
	if (target >= 4) return c->slot & 3;
	return c->ink >= 8;
}

int zsp_ink(const zsp_palette_t *p, int idx) {
	if (idx == ZSP_T || idx < 0) return 0;
	return p->pal[idx & 255].ink;
}

uint16_t zsp_shown_rgb(const zsp_palette_t *p, int idx, int target) {
	int r = zsp_reduce(p, idx, target);
	if (r == ZSP_T) return 0;
	if (target < p->colours && target <= 2) {
		int g = (p->pal[idx & 255].ink * 15 + 8) / 16;
		return (uint16_t)(g << 8 | g << 4 | g);
	}
	if (target < p->colours && target <= 4) return p->slot_rgb[r & 3];
	return p->pal[r & 255].rgb;
}

bool zsp_collision(const zsp_palette_t *p, int *a, int *b, int *tier) {
	static const int tiers[4] = { 64, 16, 4, 2 };
	int i, t;
	for (t = 0; t < 4; t++) {
		if (tiers[t] >= p->colours) continue;
		for (i = 0; i < p->ndiffer; i++) {
			int x = p->differ[i][0], y = p->differ[i][1];
			if (zsp_reduce(p, x, tiers[t]) == zsp_reduce(p, y, tiers[t])) {
				if (a) *a = x;
				if (b) *b = y;
				if (tier) *tier = tiers[t];
				return true;
			}
		}
	}
	return false;
}

/* ===================================================================
 * the sheet
 * =================================================================== */

void zsp_init(zsp_sheet_t *s, int colours) {
	memset(s, 0, sizeof(*s));
	zsp_palette_init(&s->p, colours);
}

void zsp_free(zsp_sheet_t *s) {
	int i;
	for (i = 0; i < s->n; i++) free(s->spr[i].px);
	free(s->spr);
	free(s->extra);
	s->spr = NULL;
	s->extra = NULL;
	s->n = s->cap = 0;
	s->extra_len = 0;
}

int zsp_find(const zsp_sheet_t *s, const char *name) {
	int i;
	for (i = 0; i < s->n; i++) if (!strcmp(s->spr[i].name, name)) return i;
	return -1;
}

zsp_sprite_t *zsp_add(zsp_sheet_t *s, const char *name, int w, int h) {
	zsp_sprite_t *e;
	uint16_t *px;
	int i;
	if (!name[0] || strlen(name) >= ZSP_NAME || zsp_find(s, name) >= 0) return NULL;
	if (w < 1 || h < 1 || w > ZSP_MAX_DIM || h > ZSP_MAX_DIM) return NULL;
	if (s->n == s->cap) {
		int nc = s->cap ? s->cap * 2 : 16;
		zsp_sprite_t *n = realloc(s->spr, sizeof(*n) * (size_t)nc);
		if (!n) return NULL;
		s->spr = n;
		s->cap = nc;
	}
	px = malloc(sizeof(uint16_t) * (size_t)w * (size_t)h);
	if (!px) return NULL;
	for (i = 0; i < w * h; i++) px[i] = ZSP_T;
	e = &s->spr[s->n++];
	memset(e, 0, sizeof(*e));
	strcpy(e->name, name);
	e->w = (uint16_t)w;
	e->h = (uint16_t)h;
	e->px = px;
	return e;
}

void zsp_pixels(const zsp_sheet_t *s, int i, int target, uint8_t *px, uint8_t *opaque) {
	const zsp_sprite_t *e = &s->spr[i];
	int k, n = e->w * e->h;
	for (k = 0; k < n; k++) {
		int v = zsp_reduce(&s->p, e->px[k], target);
		if (opaque) opaque[k] = v != ZSP_T;
		px[k] = v == ZSP_T ? (opaque ? 0 : 0xff) : (uint8_t)v;
	}
}

/* ===================================================================
 * reading
 * =================================================================== */

typedef struct {
	char *err;
	int errlen, line;
} perr_t;

static bool fail(perr_t *e, const char *what) {
	int k = 0, n = e->line, d = 1;
	const char *p = "line ";
	if (!e->err || e->errlen < 2) return false;
	while (*p && k < e->errlen - 1) e->err[k++] = *p++;
	while (d * 10 <= n) d *= 10;
	while (d && k < e->errlen - 1) { e->err[k++] = (char)('0' + n / d % 10); d /= 10; }
	p = ": ";
	while (*p && k < e->errlen - 1) e->err[k++] = *p++;
	while (*what && k < e->errlen - 1) e->err[k++] = *what++;
	e->err[k] = 0;
	return false;
}

static bool is_sp(char c) { return c == ' ' || c == '\t'; }

static int hexv(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* The next word of [*p, end) into w; false at the end of the line. */
static bool word(const char **p, const char *end, char *w, int cap) {
	int n = 0;
	while (*p < end && is_sp(**p)) (*p)++;
	if (*p >= end) return false;
	while (*p < end && !is_sp(**p)) {
		if (n < cap - 1) w[n++] = **p;
		(*p)++;
	}
	w[n] = 0;
	return true;
}

/* A decimal number, with a '!' after it if the artist set it. */
static bool num(const char *w, int *v, bool *bang) {
	int n = 0, k = 0;
	if (!w[0]) return false;
	while (w[k] >= '0' && w[k] <= '9') n = n * 10 + (w[k++] - '0');
	if (k == 0 || n > 100000) return false;
	*bang = w[k] == '!';
	if (w[k] && !(w[k] == '!' && !w[k + 1])) return false;
	*v = n;
	return true;
}

static bool rgb3(const char *w, uint16_t *v) {
	int k, r = 0;
	if (strlen(w) != 3) return false;
	for (k = 0; k < 3; k++) {
		int h = hexv(w[k]);
		if (h < 0) return false;
		r = r << 4 | h;
	}
	*v = (uint16_t)r;
	return true;
}

static bool keep_line(zsp_sheet_t *s, const char *l, const char *end) {
	uint32_t n = (uint32_t)(end - l);
	char *x = realloc(s->extra, s->extra_len + n + 2);
	if (!x) return false;
	s->extra = x;
	memcpy(x + s->extra_len, l, n);
	s->extra_len += n;
	x[s->extra_len++] = '\n';
	x[s->extra_len] = 0;
	return true;
}

bool zsp_parse(zsp_sheet_t *s, const char *t, uint32_t len, char *err, int errlen) {

	const char *e = t + len;
	perr_t pe;
	zsp_sprite_t *cur = NULL;
	int row = 0, curi = -1;
	uint8_t present[256];
	bool had_colours = false;
	int i;

	pe.err = err; pe.errlen = errlen; pe.line = 0;
	if (err && errlen) err[0] = 0;
	zsp_init(s, 2);
	memset(present, 0, sizeof(present));

	while (t < e) {
		const char *l = t, *end, *p;
		char w[32];

		while (t < e && *t != '\n') t++;
		end = t;
		if (t < e) t++;
		pe.line++;
		if (end > l && end[-1] == '\r') end--;

		if (cur) {
			/* a row: tabs before it, then exactly w pixels */
			int need = cur->w * (s->p.colours > 16 ? 2 : 1), x;
			while (l < end && *l == '\t') l++;
			while (end - l > need && *l == ' ') l++;
			while (end - l > need && end[-1] == ' ') end--;
			if (end - l != need) return fail(&pe, end - l < need ? "short row" : "long row");
			for (x = 0; x < cur->w; x++) {
				int v;
				if (s->p.colours > 16) {
					char a = l[x * 2], b = l[x * 2 + 1];
					if (a == '.' && b == '.') v = ZSP_T;
					else if (hexv(a) < 0 || hexv(b) < 0) return fail(&pe, "not two hex digits");
					else v = hexv(a) * 16 + hexv(b);
				} else {
					char c = l[x];
					if (c == '.' || c == ' ') v = ZSP_T;
					else if (s->p.colours == 2) v = c != '0';
					else if ((v = hexv(c)) < 0) return fail(&pe, "not a colour");
				}
				if (v != ZSP_T && v >= s->p.colours) return fail(&pe, "colour past the palette");
				s->spr[curi].px[row * cur->w + x] = (uint16_t)v;
			}
			if (++row == cur->h) cur = NULL;
			continue;
		}

		p = l;
		if (!word(&p, end, w, sizeof(w))) continue;          /* blank */

		if (!strcmp(w, "zsp")) continue;
		if (!strcmp(w, "colours")) {
			int n; bool bang;
			if (!word(&p, end, w, sizeof(w)) || !num(w, &n, &bang) ||
				(n != 2 && n != 4 && n != 16 && n != 64 && n != 256))
				return fail(&pe, "colours is 2, 4, 16, 64 or 256");
			if (s->n) return fail(&pe, "colours after a sprite");
			s->p.colours = n;
			had_colours = true;
			continue;
		}
		if (!strcmp(w, "slots")) {
			for (i = 0; i < 4; i++)
				if (!word(&p, end, w, sizeof(w)) || !rgb3(w, &s->p.slot_rgb[i]))
					return fail(&pe, "slots needs four RGB colours");
			continue;
		}
		if (!strcmp(w, "colour")) {
			int idx, v, f = 0;
			bool bang;
			uint8_t fields[4];
			zsp_colour_t *c;
			if (!word(&p, end, w, sizeof(w)) || !num(w, &idx, &bang) || idx > 255)
				return fail(&pe, "colour needs an index");
			c = &s->p.pal[idx];
			if (!word(&p, end, w, sizeof(w)) || !rgb3(w, &c->rgb))
				return fail(&pe, "colour needs an RGB colour");
			/* the fields this sheet's size has, in order */
			if (s->p.colours == 256) fields[f++] = ZSP_GIVEN_TO64;
			if (s->p.colours >= 64) fields[f++] = ZSP_GIVEN_TO16;
			fields[f++] = ZSP_GIVEN_SLOT;
			fields[f++] = ZSP_GIVEN_INK;
			present[idx] = 0;
			c->given = 0;
			for (i = 0; i < f && word(&p, end, w, sizeof(w)); i++) {
				if (!num(w, &v, &bang)) return fail(&pe, "not a number");
				switch (fields[i]) {
				case ZSP_GIVEN_TO64: if (v > 63) return fail(&pe, "to64 past 63"); c->to64 = (uint8_t)v; break;
				case ZSP_GIVEN_TO16: if (v > 15) return fail(&pe, "to16 past 15"); c->to16 = (uint8_t)v; break;
				case ZSP_GIVEN_SLOT: if (v > 3) return fail(&pe, "slot past 3"); c->slot = (uint8_t)v; break;
				default: if (v > 16) return fail(&pe, "ink past 16"); c->ink = (uint8_t)v; break;
				}
				present[idx] |= fields[i];
				if (bang) c->given |= fields[i];
			}
			continue;
		}
		if (!strcmp(w, "differ")) {
			int a, b; bool bang;
			if (s->p.ndiffer >= ZSP_DIFFER_MAX) return fail(&pe, "too many differ lines");
			if (!word(&p, end, w, sizeof(w)) || !num(w, &a, &bang) || a > 255 ||
				!word(&p, end, w, sizeof(w)) || !num(w, &b, &bang) || b > 255)
				return fail(&pe, "differ needs two indices");
			s->p.differ[s->p.ndiffer][0] = (uint8_t)a;
			s->p.differ[s->p.ndiffer][1] = (uint8_t)b;
			s->p.ndiffer++;
			continue;
		}
		if (!strcmp(w, "sprite")) {
			char name[ZSP_NAME + 8];
			int sw, sh; bool bang;
			if (!word(&p, end, name, sizeof(name))) return fail(&pe, "sprite needs a name");
			if (strlen(name) >= ZSP_NAME) return fail(&pe, "name too long");
			if (!word(&p, end, w, sizeof(w)) || !num(w, &sw, &bang) ||
				!word(&p, end, w, sizeof(w)) || !num(w, &sh, &bang) ||
				sw < 1 || sh < 1 || sw > ZSP_MAX_DIM || sh > ZSP_MAX_DIM)
				return fail(&pe, "sprite needs a width and a height");
			if (zsp_find(s, name) >= 0) return fail(&pe, "a second sprite of that name");
			cur = zsp_add(s, name, sw, sh);
			if (!cur) return fail(&pe, "out of memory");
			curi = s->n - 1;
			row = 0;
			continue;
		}
		/* anything else is kept as it is: anim lines, comments */
		if (!keep_line(s, l, end)) return fail(&pe, "out of memory");
	}

	if (cur) { pe.line++; return fail(&pe, "the sheet ends inside a sprite"); }
	(void)had_colours;

	/* What the file did not say comes from the defaults; what it did
	 * stays, so the art is shown as written. Two passes: an entry's
	 * nearest neighbours need every RGB first. */
	for (i = 0; i < 256; i++) {
		uint8_t g = s->p.pal[i].given;
		s->p.pal[i].given = present[i];
		zsp_defaults(&s->p, i);
		s->p.pal[i].given = g;
	}
	return true;
}

/* ===================================================================
 * writing
 * =================================================================== */

typedef struct {
	char *b;
	uint32_t n, cap;
	bool bad;
} out_t;

static void put(out_t *o, const char *s, uint32_t n) {
	if (o->bad) return;
	if (o->n + n + 1 > o->cap) {
		uint32_t nc = o->cap ? o->cap : 1024;
		char *x;
		while (o->n + n + 1 > nc) nc *= 2;
		x = realloc(o->b, nc);
		if (!x) { o->bad = true; return; }
		o->b = x;
		o->cap = nc;
	}
	memcpy(o->b + o->n, s, n);
	o->n += n;
	o->b[o->n] = 0;
}

static void puts_(out_t *o, const char *s) { put(o, s, (uint32_t)strlen(s)); }

static void putn(out_t *o, int v, bool bang) {
	char b[16];
	int k = 0, d = 1;
	if (v < 0) v = 0;
	while (d * 10 <= v) d *= 10;
	b[k++] = ' ';
	while (d) { b[k++] = (char)('0' + v / d % 10); d /= 10; }
	if (bang) b[k++] = '!';
	put(o, b, (uint32_t)k);
}

static void putrgb(out_t *o, uint16_t v) {
	static const char hx[] = "0123456789abcdef";
	char b[4];
	b[0] = ' ';
	b[1] = hx[(v >> 8) & 15];
	b[2] = hx[(v >> 4) & 15];
	b[3] = hx[v & 15];
	put(o, b, 4);
}

char *zsp_write(const zsp_sheet_t *s, uint32_t *len) {

	static const char hx[] = "0123456789abcdef";
	const zsp_palette_t *p = &s->p;
	out_t o = { NULL, 0, 0, false };
	bool pal = p->colours > 2;
	int i, x, y;

	puts_(&o, "zsp 1\ncolours");
	putn(&o, p->colours, false);
	puts_(&o, "\n");

	/* A 2-colour sheet with the usual black and white says nothing
	 * more: what the pointer's file has always looked like. */
	if (!pal) {
		for (i = 0; i < 2; i++)
			if (p->pal[i].given || p->pal[i].rgb != (i ? 0xfff : 0x000)) pal = true;
	}
	if (p->colours >= 4) {
		puts_(&o, "slots");
		for (i = 0; i < 4; i++) putrgb(&o, p->slot_rgb[i]);
		puts_(&o, "\n");
	}
	if (pal) {
		for (i = 0; i < p->colours; i++) {
			const zsp_colour_t *c = &p->pal[i];
			puts_(&o, "colour");
			putn(&o, i, false);
			putrgb(&o, c->rgb);
			if (p->colours == 256) putn(&o, c->to64, c->given & ZSP_GIVEN_TO64);
			if (p->colours >= 64) putn(&o, c->to16, c->given & ZSP_GIVEN_TO16);
			putn(&o, c->slot, c->given & ZSP_GIVEN_SLOT);
			putn(&o, c->ink, c->given & ZSP_GIVEN_INK);
			puts_(&o, "\n");
		}
	}
	for (i = 0; i < p->ndiffer; i++) {
		puts_(&o, "differ");
		putn(&o, p->differ[i][0], false);
		putn(&o, p->differ[i][1], false);
		puts_(&o, "\n");
	}
	if (s->extra_len) put(&o, s->extra, s->extra_len);

	for (i = 0; i < s->n; i++) {
		const zsp_sprite_t *e = &s->spr[i];
		puts_(&o, "sprite ");
		puts_(&o, e->name);
		putn(&o, e->w, false);
		putn(&o, e->h, false);
		puts_(&o, "\n");
		for (y = 0; y < e->h; y++) {
			put(&o, "\t", 1);
			for (x = 0; x < e->w; x++) {
				int v = e->px[y * e->w + x];
				char b[2];
				if (p->colours > 16) {
					if (v == ZSP_T) { b[0] = b[1] = '.'; }
					else { b[0] = hx[(v >> 4) & 15]; b[1] = hx[v & 15]; }
					put(&o, b, 2);
				} else {
					b[0] = v == ZSP_T ? '.' : hx[v & 15];
					put(&o, b, 1);
				}
			}
			put(&o, "\n", 1);
		}
	}

	if (o.bad) { free(o.b); return NULL; }
	*len = o.n;
	return o.b;
}

/* ===================================================================
 * the palette as bytes (zbm.h, "colour pictures")
 * =================================================================== */

uint32_t zsp_palette_bin_size(const zsp_palette_t *p) {
	return (12u + 8u * (uint32_t)p->colours + 2u * (uint32_t)p->ndiffer + 3u) & ~3u;
}

static void w16(uint8_t *o, uint32_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); }
static uint32_t r16(const uint8_t *i) { return (uint32_t)i[0] | ((uint32_t)i[1] << 8); }

void zsp_palette_to_bin(const zsp_palette_t *p, uint8_t *out) {
	uint32_t n = zsp_palette_bin_size(p), o = 12;
	int i;
	memset(out, 0, n);
	w16(out, (uint32_t)p->colours);
	w16(out + 2, (uint32_t)p->ndiffer);
	for (i = 0; i < 4; i++) w16(out + 4 + 2 * i, p->slot_rgb[i]);
	for (i = 0; i < p->colours; i++, o += 8) {
		const zsp_colour_t *c = &p->pal[i];
		w16(out + o, c->rgb);
		out[o + 2] = c->to64;
		out[o + 3] = c->to16;
		out[o + 4] = c->slot;
		out[o + 5] = c->ink;
		out[o + 6] = c->given;
	}
	for (i = 0; i < p->ndiffer; i++, o += 2) {
		out[o] = p->differ[i][0];
		out[o + 1] = p->differ[i][1];
	}
}

bool zsp_palette_from_bin(zsp_palette_t *p, const uint8_t *in, uint32_t len, uint32_t *used) {
	uint32_t colours, nd, n, o = 12;
	int i;
	if (len < 12) return false;
	colours = r16(in);
	nd = r16(in + 2);
	if (colours != 2 && colours != 4 && colours != 16 && colours != 64 && colours != 256) return false;
	if (nd > ZSP_DIFFER_MAX) return false;
	n = (12u + 8u * colours + 2u * nd + 3u) & ~3u;
	if (n > len) return false;
	zsp_palette_init(p, (int)colours);
	for (i = 0; i < 4; i++) p->slot_rgb[i] = (uint16_t)(r16(in + 4 + 2 * i) & 0xfff);
	for (i = 0; i < (int)colours; i++, o += 8) {
		zsp_colour_t *c = &p->pal[i];
		c->rgb = (uint16_t)(r16(in + o) & 0xfff);
		c->to64 = (uint8_t)(in[o + 2] > 63 ? 63 : in[o + 2]);
		c->to16 = (uint8_t)(in[o + 3] & 15);
		c->slot = (uint8_t)(in[o + 4] & 3);
		c->ink = (uint8_t)(in[o + 5] > 16 ? 16 : in[o + 5]);
		c->given = (uint8_t)(in[o + 6] & ZSP_GIVEN_ALL);
	}
	p->ndiffer = 0;
	for (i = 0; i < (int)nd; i++, o += 2)
		if (in[o] < colours && in[o + 1] < colours) {      /* a pair past the palette: dropped */
			p->differ[p->ndiffer][0] = in[o];
			p->differ[p->ndiffer][1] = in[o + 1];
			p->ndiffer++;
		}
	*used = n;
	return true;
}
