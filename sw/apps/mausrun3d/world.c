/*
 * mausrun3d -- the world (world.h, docs/mausrun3d.md).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- drawing it --
 *
 * The map is cut into cells of 16x16 tiles. A cell's faces -- where a
 * block meets air -- are merged into rectangles (greedy meshing: a run
 * of the same material in a row, then as many rows as match), and kept
 * as seven display lists, one for each way a face can face plus the
 * water: on T2 each list is compiled once and copied to the engine
 * every frame it is drawn (docs/zgl_t2.md).
 *
 * A frame draws only the cells in the camera's view cone, out to the
 * draw distance, and of those only the face sets that can face the
 * eye: a cell's east faces when the eye is east of its westmost one,
 * its tops when the eye is above its lowest. That halves what T2 is
 * sent before it culls a single triangle itself.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zgl.h"
#include "world.h"

/* -- parsing ---------------------------------------------------------------- */

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static const char *next_line(const char *p, const char **end) {
	const char *e = p;
	while (*e && *e != '\n') e++;
	*end = e;
	return *e ? e + 1 : e;
}

static bool word_is(const char *p, const char *e, const char *w) {
	size_t n = strlen(w);
	return (size_t)(e - p) >= n && !memcmp(p, w, n) &&
		((size_t)(e - p) == n || p[n] == ' ' || p[n] == '\r');
}

static int parse_int(const char **pp, const char *e, bool *ok) {
	const char *p = *pp;
	int v = 0, sg = 1;
	while (p < e && *p == ' ') p++;
	if (p < e && *p == '-') { sg = -1; p++; }
	if (p >= e || *p < '0' || *p > '9') { *ok = false; return 0; }
	while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
	*pp = p;
	return v * sg;
}

static uint8_t block_of(char c) {
	switch (c) {
	case 'B': return W3_BOX;
	case '?': return W3_QBOX;
	case '-': return W3_SHELF;
	case 'S': return W3_SPRING;
	case 'K': return W3_BISCUIT;
	case '^': return W3_THORNS;
	default:  return W3_AIR;
	}
}

void w3_free(w3_world_t *w) {
	if (!w) return;
	free(w->ground); free(w->surf); free(w->water); free(w->vox);
	free(w->things); free(w->cells);
	free(w);
}

w3_world_t *w3_parse(const char *text, const char **err, int *line) {
	w3_world_t *w = calloc(1, sizeof(*w));
	const char *p = text, *e;
	int ln = 0, layer = -1, row = 0, i;
	enum { HEAD, LAYER, BLOCKS, THINGS, END } part = HEAD;
	int cap_things = 0;
	*err = NULL;
	if (!w) { *err = "no memory"; return NULL; }
	while (*p && part != END) {
		const char *s = p;
		p = next_line(p, &e);
		ln++;
		if (e > s && e[-1] == '\r') e--;
		if (e == s || *s == '#') continue;
		if (part == LAYER && row < w->n) {
			int x;
			uint8_t *dst = layer == 0 ? w->ground : layer == 1 ? w->surf : w->water;
			if (e - s < w->n) { *err = "a short row"; goto bad; }
			for (x = 0; x < w->n; x++) {
				char c = s[x];
				uint8_t v;
				if (layer == 1) v = (uint8_t)c;
				else if (c == '.') v = W3_NONE;
				else {
					int h = hexval(c);
					if (h < 0) { *err = "not a height (0-9, a-f, .)"; goto bad; }
					v = (uint8_t)h;
				}
				dst[row * w->n + x] = v;
			}
			row++;
			continue;
		}
		if (word_is(s, e, "ground") || word_is(s, e, "surface") || word_is(s, e, "water")) {
			if (!w->n) { *err = "a layer before the size"; goto bad; }
			layer = word_is(s, e, "ground") ? 0 : word_is(s, e, "surface") ? 1 : 2;
			row = 0;
			part = LAYER;
			continue;
		}
		if (part == LAYER) part = HEAD;
		if (word_is(s, e, "blocks")) { part = BLOCKS; continue; }
		if (word_is(s, e, "things")) { part = THINGS; continue; }
		if (word_is(s, e, "end")) { part = END; continue; }
		if (part == BLOCKS || part == THINGS) {
			bool ok = true;
			const char *q = s;
			int x = parse_int(&q, e, &ok), z = parse_int(&q, e, &ok), y = parse_int(&q, e, &ok);
			char c;
			while (q < e && *q == ' ') q++;
			c = q < e ? *q : 0;
			if (!ok || !c) { *err = "expected x z y and a letter"; goto bad; }
			if (x < 0 || z < 0 || x >= w->n || z >= w->n || y < 0 || y >= W3_H) {
				*err = "outside the world"; goto bad;
			}
			if (part == BLOCKS) {
				uint8_t k = block_of(c);
				if (!k) { *err = "not a block (B ? - S K ^)"; goto bad; }
				w->vox[((uint32_t)z * (uint32_t)w->n + (uint32_t)x) * W3_H + (uint32_t)y] = k;
			} else {
				if (w->nthings == cap_things) {
					int nc = cap_things ? cap_things * 2 : 64;
					w3_thing_t *t = realloc(w->things, (size_t)nc * sizeof(*t));
					if (!t) { *err = "no memory"; goto bad; }
					w->things = t;
					cap_things = nc;
				}
				w->things[w->nthings].x = (int16_t)x;
				w->things[w->nthings].z = (int16_t)z;
				w->things[w->nthings].y = (int16_t)y;
				w->things[w->nthings].c = c;
				w->nthings++;
			}
			continue;
		}
		/* the header: key value */
		if (word_is(s, e, "name") || word_is(s, e, "theme")) {
			bool nm = word_is(s, e, "name");
			char *d = nm ? w->name : w->theme;
			size_t cap = nm ? sizeof(w->name) : sizeof(w->theme), n;
			const char *v = s + (nm ? 4 : 5);
			while (v < e && *v == ' ') v++;
			n = (size_t)(e - v) < cap - 1 ? (size_t)(e - v) : cap - 1;
			memcpy(d, v, n);
			d[n] = 0;
			continue;
		}
		if (word_is(s, e, "size")) {
			bool ok = true;
			const char *q = s + 4;
			int n = parse_int(&q, e, &ok);
			size_t nn;
			if (!ok || n < W3_CELL || n > W3_MAXN || n % W3_CELL) {
				*err = "the size: 16 to 512, a multiple of 16"; goto bad;
			}
			if (w->n) { *err = "a second size"; goto bad; }
			w->n = n;
			w->ncell = n / W3_CELL;
			nn = (size_t)n * (size_t)n;
			w->ground = malloc(nn);
			w->surf = malloc(nn);
			w->water = malloc(nn);
			w->vox = calloc(nn, W3_H);
			w->cells = calloc((size_t)w->ncell * (size_t)w->ncell, sizeof(w3_cell_t));
			if (!w->ground || !w->surf || !w->water || !w->vox || !w->cells) {
				*err = "no memory"; goto bad;
			}
			memset(w->ground, W3_NONE, nn);
			memset(w->surf, 'g', nn);
			memset(w->water, W3_NONE, nn);
			continue;
		}
		/* time, music, weather: the game's, not the map's */
		if (word_is(s, e, "time") || word_is(s, e, "music") || word_is(s, e, "weather")) continue;
		*err = "not understood";
		goto bad;
	}
	if (!w->n) { *err = "no size"; goto bad; }
	/* the ground into blocks: a column from the bottom to its top */
	for (i = 0; i < w->n * w->n; i++) {
		int h = w->ground[i] == W3_NONE ? 0 : w->ground[i], y;
		if (h > W3_H) h = W3_H;
		for (y = 0; y < h; y++)
			if (!w->vox[(uint32_t)i * W3_H + (uint32_t)y])
				w->vox[(uint32_t)i * W3_H + (uint32_t)y] = W3_GROUND;
	}
	*line = 0;
	return w;
bad:
	*line = ln;
	w3_free(w);
	return NULL;
}

/* -- colours ---------------------------------------------------------------- */

static uint16_t mix(uint16_t a, uint16_t b, int t, int n) {
	int k;
	uint16_t o = 0;
	for (k = 0; k < 12; k += 4) {
		int x = (a >> k) & 15, y = (b >> k) & 15;
		o |= (uint16_t)((x + ((y - x) * t + n / 2) / n) << k);
	}
	return o;
}

static void ramp(uint16_t pal[256], int first, uint16_t dark, uint16_t light) {
	int i;
	for (i = 0; i < 16; i++) pal[first + i] = mix(dark, light, i, 15);
}

void w3_palette(const w3_world_t *w, uint16_t pal[256]) {
	bool night = w && !strcmp(w->theme, "roofs");
	memset(pal, 0, 256 * sizeof(pal[0]));
	pal[W3_C_WHITE] = 0xfff;
	pal[W3_C_HUD] = 0x2e6;
	pal[W3_C_SHADOW] = night ? 0x011 : 0x232;
	pal[4] = 0xfd2;                 /* the HUD's cheese */
	pal[5] = 0xe33;                 /* hearts */
	if (night) ramp(pal, W3_R_SKY, 0x012, 0x136);
	else ramp(pal, W3_R_SKY, 0x8cf, 0x36c);  /* the horizon to the top */
	ramp(pal, W3_R_GRASS, 0x130, 0x7e4);
	ramp(pal, W3_R_EARTH, 0x210, 0xa63);
	ramp(pal, W3_R_PATH, 0x320, 0xdb7);
	ramp(pal, W3_R_SAND, 0x431, 0xfe9);
	ramp(pal, W3_R_ROCK, 0x222, 0xaab);
	ramp(pal, W3_R_BOX, 0x310, 0xc83);
	ramp(pal, W3_R_QBOX, 0x420, 0xfd3);
	ramp(pal, W3_R_WATER, 0x024, 0x5af);
	ramp(pal, W3_R_MOUSE, 0x222, 0xccd);
	ramp(pal, W3_R_PINK, 0x412, 0xfab);
	ramp(pal, W3_R_CHEESE, 0x420, 0xfe5);
	ramp(pal, W3_R_CAT, 0x310, 0xf93);
	ramp(pal, W3_R_SHELF, 0x321, 0xdb8);
	ramp(pal, W3_R_RED, 0x300, 0xf44);
}

/* -- meshing ---------------------------------------------------------------- */

/* face materials: a ramp each */
enum {
	M_NONE = 0, M_GRASS, M_EARTH, M_PATH, M_SAND, M_ROCK, M_BOX, M_QBOX,
	M_SHELF, M_SPRING, M_BISCUIT, M_THORNS, M_WATER, M_COUNT
};
static const uint8_t mat_ramp[M_COUNT] = {
	0, W3_R_GRASS, W3_R_EARTH, W3_R_PATH, W3_R_SAND, W3_R_ROCK, W3_R_BOX, W3_R_QBOX,
	W3_R_SHELF, W3_R_PINK, W3_R_PATH, W3_R_GRASS, W3_R_WATER
};

static uint8_t face_mat(const w3_world_t *w, int x, int z, uint8_t k, bool top) {
	char s;
	switch (k) {
	case W3_GROUND:
		s = (char)w->surf[z * w->n + x];
		if (top) return s == 'p' ? M_PATH : s == 's' ? M_SAND : s == 'r' ? M_ROCK :
			s == 'd' ? M_EARTH : M_GRASS;
		return s == 's' ? M_SAND : s == 'r' ? M_ROCK : M_EARTH;
	case W3_BOX:     return M_BOX;
	case W3_QBOX:    return M_QBOX;
	case W3_SHELF:   return M_SHELF;
	case W3_SPRING:  return M_SPRING;
	case W3_BISCUIT: return M_BISCUIT;
	case W3_THORNS:  return M_THORNS;
	default:         return M_NONE;
	}
}

/* a merged face: a rectangle u0..u0+du, v0..v0+dv on layer l */
typedef struct { uint8_t u, v, du, dv, l, m; } mq_t;

#define MAXQ 4096
static mq_t qs[MAXQ];
static int nq;
static uint8_t mask[W3_CELL > W3_H ? W3_CELL : W3_H][W3_CELL];

/* greedy: rectangles of one material out of mask[v][u], u < U, v < V */
static void merge(int U, int V, int l) {
	int u, v, k, du, dv;
	for (v = 0; v < V; v++)
		for (u = 0; u < U; u++) {
			uint8_t m = mask[v][u];
			if (!m) continue;
			for (du = 1; u + du < U && mask[v][u + du] == m; du++) ;
			for (dv = 1; v + dv < V; dv++) {
				for (k = 0; k < du; k++) if (mask[v + dv][u + k] != m) break;
				if (k < du) break;
			}
			for (k = 0; k < dv; k++) memset(&mask[v + k][u], 0, (size_t)du);
			if (nq < MAXQ) {
				qs[nq].u = (uint8_t)u; qs[nq].v = (uint8_t)v;
				qs[nq].du = (uint8_t)du; qs[nq].dv = (uint8_t)dv;
				qs[nq].l = (uint8_t)l; qs[nq].m = m;
				nq++;
			}
		}
}

static bool solid(const w3_world_t *w, int x, int y, int z) {
	return w3_block(w, x, y, z) != W3_AIR;
}

/* under the water at tile (x, z), the block at y entirely: the water
 * is opaque, so nothing of it is seen (the sea bed, a pond's banks) */
static bool drowned(const w3_world_t *w, int x, int y, int z) {
	return w3_water(w, x, z) >= y + 2;
}

/* the quads of face set d of the cell at tile (x0, z0), into qs[] */
static void collect(const w3_world_t *w, int d, int x0, int z0, int ymax) {
	int a, u, v;
	nq = 0;
	switch (d) {
	case W3_FY: case W3_FNY:
		for (a = 0; a < ymax; a++) {
			memset(mask, 0, sizeof(mask));
			for (v = 0; v < W3_CELL; v++)
				for (u = 0; u < W3_CELL; u++) {
					int x = x0 + u, z = z0 + v;
					uint8_t k = w3_block(w, x, a, z);
					if (!k) continue;
					if (d == W3_FY ? solid(w, x, a + 1, z) : (a == 0 || solid(w, x, a - 1, z))) continue;
					if (d == W3_FY && drowned(w, x, a, z)) continue;
					mask[v][u] = face_mat(w, x, z, k, d == W3_FY);
				}
			merge(W3_CELL, W3_CELL, a);
		}
		break;
	case W3_FX: case W3_FNX:
		for (a = 0; a < W3_CELL; a++) {
			int x = x0 + a, nx = d == W3_FX ? x + 1 : x - 1;
			memset(mask, 0, sizeof(mask));
			for (v = 0; v < ymax; v++)
				for (u = 0; u < W3_CELL; u++) {
					int z = z0 + u;
					uint8_t k = w3_block(w, x, v, z);
					if (!k || solid(w, nx, v, z)) continue;
					/* the world's own edge is not seen */
					if (nx < 0 || nx >= w->n) continue;
					if (drowned(w, nx, v, z)) continue;
					mask[v][u] = face_mat(w, x, z, k, false);
				}
			merge(W3_CELL, ymax, a);
		}
		break;
	case W3_FZ: case W3_FNZ:
		for (a = 0; a < W3_CELL; a++) {
			int z = z0 + a, nz = d == W3_FZ ? z + 1 : z - 1;
			memset(mask, 0, sizeof(mask));
			for (v = 0; v < ymax; v++)
				for (u = 0; u < W3_CELL; u++) {
					int x = x0 + u;
					uint8_t k = w3_block(w, x, v, z);
					if (!k || solid(w, x, v, nz)) continue;
					if (nz < 0 || nz >= w->n) continue;
					if (drowned(w, x, v, nz)) continue;
					mask[v][u] = face_mat(w, x, z, k, false);
				}
			merge(W3_CELL, ymax, a);
		}
		break;
	case W3_FWATER:
		/* the water's surface, a layer per level */
		for (a = 1; a < W3_H; a++) {
			bool any = false;
			memset(mask, 0, sizeof(mask));
			for (v = 0; v < W3_CELL; v++)
				for (u = 0; u < W3_CELL; u++) {
					int x = x0 + u, z = z0 + v;
					if (w3_water(w, x, z) == a && !solid(w, x, a - 1, z)) {
						mask[v][u] = M_WATER;
						any = true;
					}
				}
			if (any) merge(W3_CELL, W3_CELL, a);
		}
		break;
	}
}

#define WATER_DIP 3      /* the water's surface this far under its block's top */

static void vtx(int x, int y, int z) { glVertex3i(x, y, z); }

/* a quad as world units, its corners counter-clockwise seen from where
 * it faces (GL's front) */
static void emit(int d, const mq_t *q, int x0, int z0) {
	const int T = W3_T;
	int u0 = q->u, v0 = q->v, u1 = q->u + q->du, v1 = q->v + q->dv, l = q->l;
	switch (d) {
	case W3_FY: {
		int Y = (l + 1) * T, xa = (x0 + u0) * T, xb = (x0 + u1) * T, za = (z0 + v0) * T, zb = (z0 + v1) * T;
		vtx(xa, Y, zb); vtx(xb, Y, zb); vtx(xb, Y, za); vtx(xa, Y, za);
		break;
	}
	case W3_FWATER: {
		int Y = l * T - WATER_DIP, xa = (x0 + u0) * T, xb = (x0 + u1) * T, za = (z0 + v0) * T, zb = (z0 + v1) * T;
		vtx(xa, Y, zb); vtx(xb, Y, zb); vtx(xb, Y, za); vtx(xa, Y, za);
		break;
	}
	case W3_FNY: {
		int Y = l * T, xa = (x0 + u0) * T, xb = (x0 + u1) * T, za = (z0 + v0) * T, zb = (z0 + v1) * T;
		vtx(xa, Y, za); vtx(xb, Y, za); vtx(xb, Y, zb); vtx(xa, Y, zb);
		break;
	}
	case W3_FX: case W3_FNX: {
		int X = (x0 + l + (d == W3_FX ? 1 : 0)) * T;
		int za = (z0 + u0) * T, zb = (z0 + u1) * T, ya = v0 * T, yb = v1 * T;
		if (d == W3_FX) { vtx(X, ya, zb); vtx(X, ya, za); vtx(X, yb, za); vtx(X, yb, zb); }
		else            { vtx(X, ya, za); vtx(X, ya, zb); vtx(X, yb, zb); vtx(X, yb, za); }
		break;
	}
	case W3_FZ: case W3_FNZ: {
		int Z = (z0 + l + (d == W3_FZ ? 1 : 0)) * T;
		int xa = (x0 + u0) * T, xb = (x0 + u1) * T, ya = v0 * T, yb = v1 * T;
		if (d == W3_FZ) { vtx(xa, ya, Z); vtx(xb, ya, Z); vtx(xb, yb, Z); vtx(xa, yb, Z); }
		else            { vtx(xb, ya, Z); vtx(xa, ya, Z); vtx(xa, yb, Z); vtx(xb, yb, Z); }
		break;
	}
	}
}

/* the face coordinate in world units: the limit for culling */
static int face_coord(int d, const mq_t *q, int x0, int z0) {
	switch (d) {
	case W3_FY:    return (q->l + 1) * W3_T;
	case W3_FWATER: return q->l * W3_T - WATER_DIP;
	case W3_FNY:   return q->l * W3_T;
	case W3_FX:    return (x0 + q->l + 1) * W3_T;
	case W3_FNX:   return (x0 + q->l) * W3_T;
	case W3_FZ:    return (z0 + q->l + 1) * W3_T;
	default:       return (z0 + q->l) * W3_T;
	}
}

static const int8_t normals[W3_NF][3] = {
	{ 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }
};

void w3_cell_build(w3_world_t *w, int cx, int cz) {
	w3_cell_t *c = &w->cells[cz * w->ncell + cx];
	int x0 = cx * W3_CELL, z0 = cz * W3_CELL, d, i, m, x, z, y;
	int ymax = 0;
	if (c->built) return;
	c->built = true;
	/* nothing solid above ymax: the layers to look at */
	for (z = z0; z < z0 + W3_CELL; z++)
		for (x = x0; x < x0 + W3_CELL; x++)
			for (y = W3_H - 1; y >= ymax; y--)
				if (w3_block(w, x, y, z)) { ymax = y + 1; break; }
	c->ymax = (uint8_t)ymax;
	/* the water's surface too, for the view's test */
	for (z = z0; z < z0 + W3_CELL; z++)
		for (x = x0; x < x0 + W3_CELL; x++)
			if (w3_water(w, x, z) > (int)c->ymax) c->ymax = (uint8_t)w3_water(w, x, z);
	c->list = glGenLists(W3_NF);
	if (!c->list) return;
	for (d = 0; d < W3_NF; d++) {
		bool pos = d == W3_FY || d == W3_FX || d == W3_FZ || d == W3_FWATER;
		int lim = pos ? 32767 : -32768;
		collect(w, d, x0, z0, ymax);
		c->tris[d] = (uint16_t)(nq * 2);
		glNewList(c->list + (GLuint)d, GL_COMPILE);
		if (nq) {
			glNormal3i(normals[d][0], normals[d][1], normals[d][2]);
			/* a material at a time: each is a chunk of its own on T2 */
			for (m = 1; m < M_COUNT; m++) {
				bool any = false;
				for (i = 0; i < nq; i++) {
					if (qs[i].m != m) continue;
					if (!any) {
						GLint mat[3];
						mat[0] = mat_ramp[m] + 3;
						mat[1] = mat[2] = mat_ramp[m] + 15;
						glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, mat);
						glBegin(GL_QUADS);
						any = true;
					}
					emit(d, &qs[i], x0, z0);
				}
				if (any) glEnd();
			}
			for (i = 0; i < nq; i++) {
				int f = face_coord(d, &qs[i], x0, z0);
				if (pos ? f < lim : f > lim) lim = f;
			}
		}
		glEndList();
		c->lim[d] = (int16_t)lim;
	}
}

/* A block changed (a ? box used, a biscuit crumbled or baked again):
 * its cell's lists are made again, and a neighbour's where the block is
 * on the edge between them (a face there comes or goes). */
static void cell_remake(w3_world_t *w, int cx, int cz) {
	w3_cell_t *c;
	if (cx < 0 || cz < 0 || cx >= w->ncell || cz >= w->ncell) return;
	c = &w->cells[cz * w->ncell + cx];
	if (!c->built) return;
	if (c->list) glDeleteLists(c->list, W3_NF);
	c->list = 0;
	c->built = false;
	w3_cell_build(w, cx, cz);
}

void w3_set_block(w3_world_t *w, int x, int y, int z, uint8_t k) {
	int cx = x / W3_CELL, cz = z / W3_CELL;
	if (x < 0 || z < 0 || x >= w->n || z >= w->n || y < 0 || y >= W3_H) return;
	w->vox[((uint32_t)z * (uint32_t)w->n + (uint32_t)x) * W3_H + (uint32_t)y] = k;
	cell_remake(w, cx, cz);
	if (x % W3_CELL == 0) cell_remake(w, cx - 1, cz);
	if (x % W3_CELL == W3_CELL - 1) cell_remake(w, cx + 1, cz);
	if (z % W3_CELL == 0) cell_remake(w, cx, cz - 1);
	if (z % W3_CELL == W3_CELL - 1) cell_remake(w, cx, cz + 1);
}

/* -- a frame of it ------------------------------------------------------------ */

uint32_t w3_calls;

/* The view as five planes through the eye -- left, right, bottom, top,
 * and the draw distance -- each kept as its inward normal (16.16):
 * a point p is inside where (p - eye) . n >= 0 (the far plane's with
 * `far` added). */
typedef struct { int32_t n[5][3]; int32_t far; } frustum_t;

static void frustum(frustum_t *f, uint32_t yaw, uint32_t pitch, int32_t far, int32_t ta, int32_t tb) {
	int32_t sy = zglSin(yaw), cy = zglCos(yaw), sp = zglSin(pitch), cp = zglCos(pitch);
	int32_t fw[3], r[3], u[3];
	int k;
	fw[0] = (int32_t)(((int64_t)sy * cp) >> 16);
	fw[1] = -sp;
	fw[2] = (int32_t)(((int64_t)-cy * cp) >> 16);
	r[0] = cy; r[1] = 0; r[2] = sy;
	u[0] = (int32_t)(((int64_t)sy * sp) >> 16);
	u[1] = cp;
	u[2] = (int32_t)(((int64_t)-cy * sp) >> 16);
	for (k = 0; k < 3; k++) {
		int32_t fa = (int32_t)(((int64_t)fw[k] * ta) >> 16), fb = (int32_t)(((int64_t)fw[k] * tb) >> 16);
		f->n[0][k] = r[k] + fa;          /* left */
		f->n[1][k] = -r[k] + fa;         /* right */
		f->n[2][k] = u[k] + fb;          /* bottom */
		f->n[3][k] = -u[k] + fb;         /* top */
		f->n[4][k] = -fw[k];             /* the draw distance */
	}
	f->far = far;
}

/* a box (world units, relative to the eye) wholly outside a plane? */
static bool box_out(const frustum_t *f, int32_t x0, int32_t y0, int32_t z0,
	int32_t x1, int32_t y1, int32_t z1) {
	int p;
	for (p = 0; p < 5; p++) {
		const int32_t *n = f->n[p];
		/* the corner furthest along n */
		int64_t d = (int64_t)(n[0] >= 0 ? x1 : x0) * n[0] + (int64_t)(n[1] >= 0 ? y1 : y0) * n[1] +
		            (int64_t)(n[2] >= 0 ? z1 : z0) * n[2];
		if (p == 4) d += (int64_t)f->far << 16;
		if (d < 0) return true;
	}
	return false;
}

uint32_t w3_draw(w3_world_t *w, int32_t ex, int32_t ey, int32_t ez, uint32_t yaw,
	uint32_t pitch, int32_t ta, int32_t tb, int32_t far, int build) {
	frustum_t fr;
	const int32_t span = W3_CELL * W3_T;
	uint32_t sent = 0;
	int cx, cz, d, cx0, cx1, cz0, cz1;
	frustum(&fr, yaw, pitch, far, ta, tb);
	w3_calls = 0;
	/* only the cells within the draw distance of the eye are looked at */
	cx0 = (ex - far) / span; cx1 = (ex + far) / span;
	cz0 = (ez - far) / span; cz1 = (ez + far) / span;
	if (cx0 < 0) cx0 = 0;
	if (cz0 < 0) cz0 = 0;
	if (cx1 >= w->ncell) cx1 = w->ncell - 1;
	if (cz1 >= w->ncell) cz1 = w->ncell - 1;
	for (cz = cz0; cz <= cz1; cz++)
		for (cx = cx0; cx <= cx1; cx++) {
			w3_cell_t *c = &w->cells[cz * w->ncell + cx];
			int32_t x0 = cx * span - ex, z0 = cz * span - ez;
			int32_t y1 = (c->built ? c->ymax : W3_H) * W3_T - ey;
			if (box_out(&fr, x0, -ey, z0, x0 + span, y1, z0 + span)) continue;
			if (!c->built) {
				if (build <= 0) continue;
				build--;
				w3_cell_build(w, cx, cz);
			}
			if (!c->list) continue;
			for (d = 0; d < W3_NF; d++) {
				int32_t lim = c->lim[d];
				bool see;
				if (!c->tris[d]) continue;
				switch (d) {
				case W3_FY: case W3_FWATER: see = ey > lim; break;
				case W3_FNY: see = ey < lim; break;
				case W3_FX:  see = ex > lim; break;
				case W3_FNX: see = ex < lim; break;
				case W3_FZ:  see = ez > lim; break;
				default:     see = ez < lim; break;
				}
				if (!see) continue;
				glCallList(c->list + (GLuint)d);
				sent += c->tris[d];
				w3_calls++;
			}
		}
	return sent;
}
