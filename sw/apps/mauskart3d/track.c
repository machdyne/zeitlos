/*
 * mauskart3d -- the track: a closed centreline, smoothed (Catmull-Rom)
 * and cut into segments of SEG_LEN world units; the road, its kerbs and
 * the posts beside it as display lists a chunk of segments each; the
 * grass as big cells under it (docs/mauskart3d.md, "The track").
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zgl.h"
#include "../mausrun3d/world.h"
#include "../mausrun/sprites.h"
#include "mauskart3d.h"

seg_t tk_seg[MAX_SEGS];
int tk_n;
uint32_t tk_len;
thing_t tk_thing[MAX_THINGS];
int tk_nthings;
uint8_t tk_map_x[MAX_SEGS], tk_map_y[MAX_SEGS];
uint32_t tk_calls;

/* The Meadow Ring: a long straight past the start, a fast sweep, a
 * hairpin, an S through the middle and a long left back round. World
 * units, clockwise seen from above (x east, z south). */
static const int16_t ring[][2] = {
	{ 0, 0 }, { 640, 0 }, { 1180, 40 }, { 1560, 260 }, { 1700, 640 },
	{ 1560, 980 }, { 1240, 1080 }, { 1000, 900 }, { 880, 640 },
	{ 640, 500 }, { 380, 620 }, { 240, 900 }, { 0, 1120 },
	{ -420, 1180 }, { -820, 1000 }, { -1020, 640 }, { -980, 260 },
	{ -700, 20 }, { -360, -20 }
};

/* Sandy Bay: a long fast top, a sweeping right, a chicane at the bottom
 * of the bay and a long hairpin back -- on sand, more bushes than trees */
static const int16_t bay[][2] = {
	{ 0, 0 }, { 700, 0 }, { 1200, -80 }, { 1600, 100 }, { 1720, 480 },
	{ 1500, 760 }, { 1100, 760 }, { 820, 600 }, { 560, 720 }, { 300, 960 },
	{ -200, 1000 }, { -640, 860 }, { -900, 520 }, { -820, 160 }, { -420, -20 }
};

static const struct {
	const char *name;
	const int16_t (*pts)[2];
	int n;
	uint8_t ground;            /* the ground's ramp */
	uint8_t trees;             /* percent of the scenery that is trees */
} tracks[] = {
	{ "THE MEADOW RING", ring, (int)(sizeof(ring) / sizeof(ring[0])), W3_R_GRASS, 67 },
	{ "SANDY BAY", bay, (int)(sizeof(bay) / sizeof(bay[0])), W3_R_SAND, 25 },
};
#define NTRACKS ((int)(sizeof(tracks) / sizeof(tracks[0])))
static int cur;                /* the track */
#define NRING (tracks[cur].n)

int tk_tracks(void) { return NTRACKS; }
void tk_select(int t) { cur = ((t % NTRACKS) + NTRACKS) % NTRACKS; }
int tk_selected(void) { return cur; }
const char *tk_name(void) { return tracks[cur].name; }
int tk_ground(void) { return tracks[cur].ground; }

/* -- small integer geometry -- */

static uint32_t isqrt64(uint64_t v) {
	uint64_t r = 0, b = (uint64_t)1 << 62;
	while (b > v) b >>= 2;
	while (b) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return (uint32_t)r;
}

/* turn16 of the direction (dx, dz): 0 north (-z), a quarter east */
uint16_t mk_atan2(int32_t dx, int32_t dz) {
	/* CORDIC, vectoring: rotate (x, y) to the x axis, summing the angles */
	static const uint16_t at[14] = {
		8192, 4836, 2555, 1297, 651, 326, 163, 81, 41, 20, 10, 5, 3, 1
	};
	int64_t x = -dz, y = dx;         /* angle measured from north, clockwise */
	int32_t a = 0;
	int i;
	if (x < 0) {                     /* into the right half first */
		if (y >= 0) { int64_t t = x; x = y; y = -t; a = 16384; }
		else        { int64_t t = x; x = -y; y = t; a = -16384; }
	}
	x *= 256; y *= 256;
	for (i = 0; i < 14; i++) {
		int64_t nx, ny;
		if (y > 0) { nx = x + (y >> i); ny = y - (x >> i); a += at[i]; }
		else       { nx = x - (y >> i); ny = y + (x >> i); a -= at[i]; }
		x = nx; y = ny;
	}
	return (uint16_t)a;
}

/* a closed Catmull-Rom through the track's points, at span i and t (0..65535): Q16 */
static void cr(int i, uint32_t t, int64_t *ox, int64_t *oz) {
	int i0 = (i + NRING - 1) % NRING, i1 = i % NRING, i2 = (i + 1) % NRING, i3 = (i + 2) % NRING;
	int64_t t1 = t, t2 = (t1 * t1) >> 16, t3 = (t2 * t1) >> 16;
	int k;
	for (k = 0; k < 2; k++) {
		int64_t p0 = tracks[cur].pts[i0][k], p1 = tracks[cur].pts[i1][k], p2 = tracks[cur].pts[i2][k], p3 = tracks[cur].pts[i3][k];
		int64_t v = 2 * p1 * 65536 + (-p0 + p2) * t1 + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
		            (-p0 + 3 * p1 - 3 * p2 + p3) * t3;
		v /= 2;
		if (k == 0) *ox = v; else *oz = v;
	}
}

void tk_build(void) {
	int i, s;
	int64_t px, pz, acc = 0;
	uint32_t at = 0;
	int32_t minx = 1 << 30, maxx = -(1 << 30), minz = 1 << 30, maxz = -(1 << 30), span;
	tk_n = 0;
	cr(0, 0, &px, &pz);
	tk_seg[0].x = (int32_t)(px >> 16);
	tk_seg[0].z = (int32_t)(pz >> 16);
	tk_n = 1;
	/* walk the curve finely, a segment each SEG_LEN of arc */
	for (i = 0; i < NRING && tk_n < MAX_SEGS; i++) {
		for (s = 1; s <= 64; s++) {
			int64_t x, z, dx, dz;
			uint32_t d;
			cr(i, (uint32_t)(s * 1024), &x, &z);
			if (s == 64 && i == NRING - 1) break;   /* back at the start */
			dx = (x - px) >> 8; dz = (z - pz) >> 8;  /* Q8 */
			d = isqrt64((uint64_t)(dx * dx + dz * dz));
			acc += d;
			px = x; pz = z;
			if (acc >= (int64_t)SEG_LEN * 256 && tk_n < MAX_SEGS) {
				acc -= (int64_t)SEG_LEN * 256;
				at += SEG_LEN;
				tk_seg[tk_n].x = (int32_t)(x >> 16);
				tk_seg[tk_n].z = (int32_t)(z >> 16);
				tk_seg[tk_n].at = at;
				tk_n++;
			}
		}
	}
	tk_len = at + SEG_LEN;
	/* directions: from the one before to the one after */
	for (i = 0; i < tk_n; i++) {
		const seg_t *a = &tk_seg[(i + tk_n - 1) % tk_n], *b = &tk_seg[(i + 1) % tk_n];
		int32_t dx = b->x - a->x, dz = b->z - a->z;
		uint32_t d = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dz * dz));
		if (d == 0) d = 1;
		tk_seg[i].dx = (int16_t)(dx * 16384 / (int32_t)d);
		tk_seg[i].dz = (int16_t)(dz * 16384 / (int32_t)d);
		tk_seg[i].yaw = mk_atan2(dx, dz);
	}
	for (i = 0; i < tk_n; i++)
		tk_seg[i].curve = (int16_t)(tk_seg[(i + 1) % tk_n].yaw - tk_seg[i].yaw);
	/* the AI's speed: what the steering can hold through the sharpest
	 * of the next few segments (a kart turns at most about 600 turn16 a
	 * tick; a segment is SEG_LEN * Q of travel) */
	for (i = 0; i < tk_n; i++) {
		int32_t most = 1, v;
		int k;
		for (k = 0; k < 5; k++) {
			int16_t c = tk_seg[(i + k) % tk_n].curve;
			if (c < 0) c = (int16_t)-c;
			if (c > most) most = c;
		}
		v = (int32_t)(560L * SEG_LEN * 256 / most);
		if (v > 1344) v = 1344;
		if (v < 560) v = 560;
		tk_seg[i].vmax = (uint16_t)v;
	}
	/* the minimap's points */
	for (i = 0; i < tk_n; i++) {
		if (tk_seg[i].x < minx) minx = tk_seg[i].x;
		if (tk_seg[i].x > maxx) maxx = tk_seg[i].x;
		if (tk_seg[i].z < minz) minz = tk_seg[i].z;
		if (tk_seg[i].z > maxz) maxz = tk_seg[i].z;
	}
	span = maxx - minx > maxz - minz ? maxx - minx : maxz - minz;
	if (span < 1) span = 1;
	for (i = 0; i < tk_n; i++) {
		tk_map_x[i] = (uint8_t)((tk_seg[i].x - minx) * 63 / span);
		tk_map_y[i] = (uint8_t)((tk_seg[i].z - minz) * 63 / span);
	}
	/* things: ? boxes across the road, cheese in lines, springs, posts */
	tk_nthings = 0;
	{
		static const struct { int16_t seg; int8_t across; uint8_t kind; } put[] = {
			{ 14, -20, TH_QBOX }, { 14, 0, TH_QBOX }, { 14, 20, TH_QBOX },
			{ 30, -8, TH_CHEESE }, { 32, -8, TH_CHEESE }, { 34, -8, TH_CHEESE }, { 36, -8, TH_CHEESE },
			{ 70, -20, TH_QBOX }, { 70, 0, TH_QBOX }, { 70, 20, TH_QBOX },
			{ 96, 10, TH_CHEESE }, { 98, 10, TH_CHEESE }, { 100, 10, TH_CHEESE },
			{ 120, 0, TH_SPRING },
			{ 140, -20, TH_QBOX }, { 140, 0, TH_QBOX }, { 140, 20, TH_QBOX },
			{ 162, -12, TH_CHEESE }, { 164, -12, TH_CHEESE }, { 166, -12, TH_CHEESE },
			{ 190, -20, TH_QBOX }, { 190, 0, TH_QBOX }, { 190, 20, TH_QBOX },
		};
		int k;
		for (k = 0; k < (int)(sizeof(put) / sizeof(put[0])); k++) {
			int sg = put[k].seg * tk_n / 230 % tk_n;    /* placed for 230 segments */
			thing_t *t = &tk_thing[tk_nthings++];
			const seg_t *g = &tk_seg[sg];
			/* across: + right of the direction, which is (-dz, dx) */
			t->x = g->x + (-(int32_t)g->dz * put[k].across) / 16384;
			t->z = g->z + ((int32_t)g->dx * put[k].across) / 16384;
			t->seg = (int16_t)sg;
			t->kind = put[k].kind;
			t->gone = 0;
			t->yaw = g->yaw;
		}
	}
}

int tk_nearest(int32_t x, int32_t z, int hint, int32_t *across) {
	int best = 0, i, k, n;
	uint32_t bd = 0xffffffffu;
	/* round the hint (a kart moves under a segment a tick): no division,
	 * 32-bit distances (a segment's neighbourhood is a few hundred units) */
	if (hint < 0) { i = 0; n = tk_n; }
	else { i = hint - 3; if (i < 0) i += tk_n; n = 7; }
	for (k = 0; k < n; k++) {
		int32_t dx = x - tk_seg[i].x, dz = z - tk_seg[i].z;
		uint32_t d;
		if (dx < 0) dx = -dx;
		if (dz < 0) dz = -dz;
		if (dx > 30000 || dz > 30000) d = 0xfffffffeu;
		else d = (uint32_t)(dx * dx) + (uint32_t)(dz * dz);
		if (d < bd) { bd = d; best = i; }
		if (++i == tk_n) i = 0;
	}
	if (across) {
		const seg_t *g = &tk_seg[best];
		*across = ((x - g->x) * -g->dz + (z - g->z) * g->dx) >> 14;
	}
	return best;
}

/* -- textures (as mausrun3d's: 16x16 tones of a ramp, ZGL_TEX_TONE) -- */

enum { TX_ROAD = 1, TX_UNUSED, TX_BOARD, TX_QBOX, TX_BOX, TX_START, TX_MATCH, TX_TREE, TX_BUSH, TX_N };

static void tex_tile(uint8_t *t, const uint32_t *rows, uint8_t ink, uint8_t paper) {
	int x, y;
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			t[y * 16 + x] = (rows[y] >> x) & 1 ? ink : paper;
}

bool tk_textures(void) {
	static uint8_t t[256];
	bool ok = true;
	int x, y;
	/* the road across, kerbs and all, 64 x 16: a texel its ramp and its
	 * tone (ZGL_TEX_RAMP); the kerbs (four texels each side) red and
	 * white by turns along */
	{
		static uint8_t road[64 * 16];
		for (y = 0; y < 16; y++)
			for (x = 0; x < 64; x++) {
				uint32_t h = ((uint32_t)(y * 64 + x) + 77u) * 0x9E3779B1u;
				int tone;
				h ^= h >> 15; h *= 0x85EBCA77u; h ^= h >> 13;
				tone = h % 100 < 12 ? 9 : h % 100 < 22 ? 13 : 11;
				if (x < 4 || x >= 60) road[y * 64 + x] = (y & 8) ? (uint8_t)(W3_R_MOUSE | 15) : (uint8_t)(W3_R_RED | 13);
				else if (x == 31 && (y & 8)) road[y * 64 + x] = (uint8_t)(W3_R_MOUSE | 13);  /* the middle line */
				else road[y * 64 + x] = (uint8_t)(W3_R_ROCK | tone);
			}
		ok &= zglTexImage(TX_ROAD, 6, 4, road, ZGL_TEX_RAMP, false);
	}
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			t[y * 16 + x] = (y & 3) == 3 ? 9 : ((x + (y >> 2) * 5) % 16 == 0 ? 11 : 15);
	ok &= zglTexImage(TX_BOARD, 4, 4, t, ZGL_TEX_TONE, false);
	tex_tile(t, mr_tile_qbox, 15, 9);             ok &= zglTexImage(TX_QBOX, 4, 4, t, ZGL_TEX_TONE, false);
	tex_tile(t, mr_tile_box, 15, 10);             ok &= zglTexImage(TX_BOX, 4, 4, t, ZGL_TEX_TONE, false);
	for (y = 0; y < 16; y++)                      /* the start line: a chequer */
		for (x = 0; x < 16; x++)
			t[y * 16 + x] = ((x >> 2) ^ (y >> 2)) & 1 ? 2 : 15;
	ok &= zglTexImage(TX_START, 4, 4, t, ZGL_TEX_TONE, false);
	for (y = 0; y < 16; y++)                      /* a matchbox's label: a band, a flame */
		for (x = 0; x < 16; x++) {
			int band = y >= 5 && y <= 10;
			int flame = band && x >= 6 && x <= 9 && (y - 5) >= (x == 6 || x == 9 ? 2 : 0);
			t[y * 16 + x] = flame ? 4 : band ? 15 : 9;
		}
	ok &= zglTexImage(TX_MATCH, 4, 4, t, ZGL_TEX_TONE, false);
	/* a tree and a bush: cards, each texel its ramp and tone, 0 not drawn */
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++) {
			int dx = 2 * x - 15, dy = 2 * y - 11;      /* the crown round (7.5, 5.5) */
			uint8_t v = 0;
			if (dx * dx + dy * dy * 5 / 4 < 15 * 15) {
				int tone = 8 + ((x * 7 + y * 13) % 5) - (dx > 4 ? 2 : 0) + (dy < -4 ? 3 : 0);
				v = (uint8_t)(W3_R_GRASS | (tone > 15 ? 15 : tone));
			} else if (y >= 11 && x >= 7 && x <= 8)
				v = (uint8_t)(W3_R_EARTH | (x == 7 ? 9 : 6));
			t[y * 16 + x] = v;
		}
	ok &= zglTexImage(TX_TREE, 4, 4, t, ZGL_TEX_RAMP, true);
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++) {
			int dx = 2 * x - 15, dy = 2 * y - 31;      /* a dome on the ground */
			uint8_t v = 0;
			if (dx * dx + dy * dy / 4 < 15 * 15 && y > 2) {
				int tone = 7 + ((x * 5 + y * 3) % 4) + (y < 7 ? 3 : 0);
				v = (uint8_t)(((x + y) % 7 == 0 && y < 12 ? W3_R_PINK : W3_R_GRASS) | tone);
			}
			t[y * 16 + x] = v;
		}
	ok &= zglTexImage(TX_BUSH, 4, 4, t, ZGL_TEX_RAMP, true);
	return ok;
}

/* -- the lists -- */

#define CHUNK 8                      /* segments a list */
#define LOD_NEAR 224                 /* world units: nearer, the near list */
#define MAX_CHUNKS (MAX_SEGS / CHUNK + 1)
static GLuint ch_list[MAX_CHUNKS][2], ch_scen[MAX_CHUNKS][2], ch_both[MAX_CHUNKS][2];
static uint16_t ch_stris[MAX_CHUNKS][2];
bool tk_scenery = true;
static uint16_t ch_tris[MAX_CHUNKS][2];
static int32_t ch_x[MAX_CHUNKS], ch_z[MAX_CHUNKS], ch_r[MAX_CHUNKS];
static int nchunks;
static int ntri;

void mk_material(int ramp) {
	GLint m[3];
	m[0] = ramp + 3;
	m[1] = m[2] = ramp + 15;
	glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, m);
}

void mk_flat(int c) {
	GLint m[3];
	m[0] = m[1] = m[2] = c;
	glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, m);
}

/* an edge point of segment i, `off` world units right of the centre */
static void edge(int i, int32_t off, int32_t *x, int32_t *z) {
	const seg_t *g = &tk_seg[(i % tk_n + tk_n) % tk_n];
	*x = g->x + (-(int32_t)g->dz * off) / 16384;
	*z = g->z + ((int32_t)g->dx * off) / 16384;
}

/* a quad on the ground, a..d counter-clockwise from above, its texture
 * coordinates from (u0, v0) in world units */
static void flat_quad(const int32_t *a, const int32_t *b, const int32_t *c, const int32_t *d, int y,
                      int32_t ux, int32_t uz) {
	glNormal3i(0, 1, 0);
	glTexCoord2i(a[0] - ux, a[1] - uz); glVertex3i(a[0], y, a[1]);
	glTexCoord2i(b[0] - ux, b[1] - uz); glVertex3i(b[0], y, b[1]);
	glTexCoord2i(c[0] - ux, c[1] - uz); glVertex3i(c[0], y, c[1]);
	glTexCoord2i(a[0] - ux, a[1] - uz); glVertex3i(a[0], y, a[1]);
	glTexCoord2i(c[0] - ux, c[1] - uz); glVertex3i(c[0], y, c[1]);
	glTexCoord2i(d[0] - ux, d[1] - uz); glVertex3i(d[0], y, d[1]);
	ntri += 2;
}

/* a chunk's list: near, the road a segment at a time and the posts;
 * far, the road two segments at a time, the boards on the bends alone */
static void chunk(int c, int far, GLuint into) {
	int s0 = c * CHUNK, s1 = s0 + CHUNK, i;
	int32_t ux = tk_seg[s0 % tk_n].x, uz = tk_seg[s0 % tk_n].z;
	int step = far ? 2 : 1;
	int32_t hw = ROAD_HW + KERB_W;
	if (s1 > tk_n) s1 = tk_n;
	if (into) { ntri = 0; glNewList(into, GL_COMPILE); }
	/* the road and its kerbs: the left edge to the right one, a segment
	 * to the next (two, far). Seen from above, x east and z south, going
	 * along (dx, dz) with right (-dz, dx): l0 r0 r1 l1 is counter-
	 * clockwise (its normal up). The texture once across (s 0..64), eight
	 * texels a segment along: a kerb's stripe a segment long. */
	glBindTexture(GL_TEXTURE_2D, TX_ROAD);
	mk_material(W3_R_ROCK);
	glBegin(GL_TRIANGLES);
	glNormal3i(0, 1, 0);
	for (i = s0; i < s1; i += step) {
		int32_t l0[2], r0[2], l1[2], r1[2];
		int i0 = i == 0 ? 1 : i, i1 = i + step;
		int t0 = (i0 - s0) * 8, t1 = (i1 - s0) * 8;
		if (i0 >= i1) continue;
		edge(i0, -hw, &l0[0], &l0[1]); edge(i0, hw, &r0[0], &r0[1]);
		edge(i1, -hw, &l1[0], &l1[1]); edge(i1, hw, &r1[0], &r1[1]);
		glTexCoord2i(0, t0); glVertex3i(l0[0], 0, l0[1]);
		glTexCoord2i(64, t0); glVertex3i(r0[0], 0, r0[1]);
		glTexCoord2i(64, t1); glVertex3i(r1[0], 0, r1[1]);
		glTexCoord2i(0, t0); glVertex3i(l0[0], 0, l0[1]);
		glTexCoord2i(64, t1); glVertex3i(r1[0], 0, r1[1]);
		glTexCoord2i(0, t1); glVertex3i(l1[0], 0, l1[1]);
		ntri += 2;
	}
	glEnd();
	if (s0 == 0) {                   /* the start line, chequered */
		int32_t l0[2], r0[2], l1[2], r1[2];
		glBindTexture(GL_TEXTURE_2D, TX_START);
		mk_material(W3_R_MOUSE);
		glBegin(GL_TRIANGLES);
		edge(0, -hw, &l0[0], &l0[1]); edge(0, hw, &r0[0], &r0[1]);
		edge(1, -hw, &l1[0], &l1[1]); edge(1, hw, &r1[0], &r1[1]);
		flat_quad(l0, r0, r1, l1, 0, ux, uz);
		glEnd();
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	/* posts every fourth segment, by turns left and right, facing the
	 * road; a board on the outside of each bend */
	mk_material(W3_R_SHELF);
	glBindTexture(GL_TEXTURE_2D, TX_BOARD);
	glBegin(GL_TRIANGLES);
	for (i = s0; i < s1; i++) {
		int side, h;
		int32_t a[2], b[2], nx, nz;
		const seg_t *g = &tk_seg[i];
		int16_t turn = 0;
		int k;
		bool board;
		for (k = -2; k <= 2; k++) turn += tk_seg[(i + k + tk_n) % tk_n].curve;
		board = i % 8 == 4 && (turn > 900 || turn < -900);
		if ((i & 3) && !board) continue;
		if (far && !board) continue;
		if (board) {
			side = turn > 0 ? -1 : 1;    /* a board on the outside of the bend */
			h = 14;
		} else {
			side = (i & 4) ? 1 : -1;
			h = 9;
		}
		edge(i, side * (ROAD_HW + KERB_W + 10), &a[0], &a[1]);
		edge(i + 1, side * (ROAD_HW + KERB_W + 10), &b[0], &b[1]);
		if (h == 9) {                /* a post: narrow */
			b[0] = a[0] + (b[0] - a[0]) / 6;
			b[1] = a[1] + (b[1] - a[1]) / 6;
		}
		/* facing the road: its normal towards the centre */
		nx = -(-(int32_t)g->dz * side); nz = -((int32_t)g->dx * side);
		glNormal3i(nx, 0, nz);
		if (side > 0) {              /* right side: the front faces left */
			glTexCoord2i(0, 16); glVertex3i(b[0], 0, b[1]);
			glTexCoord2i(16, 16); glVertex3i(a[0], 0, a[1]);
			glTexCoord2i(16, 0); glVertex3i(a[0], h, a[1]);
			glTexCoord2i(0, 16); glVertex3i(b[0], 0, b[1]);
			glTexCoord2i(16, 0); glVertex3i(a[0], h, a[1]);
			glTexCoord2i(0, 0); glVertex3i(b[0], h, b[1]);
		} else {
			glTexCoord2i(0, 16); glVertex3i(a[0], 0, a[1]);
			glTexCoord2i(16, 16); glVertex3i(b[0], 0, b[1]);
			glTexCoord2i(16, 0); glVertex3i(b[0], h, b[1]);
			glTexCoord2i(0, 16); glVertex3i(a[0], 0, a[1]);
			glTexCoord2i(16, 0); glVertex3i(b[0], h, b[1]);
			glTexCoord2i(0, 0); glVertex3i(a[0], h, a[1]);
		}
		ntri += 2;
	}
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
	if (into) { glEndList(); ch_tris[c][far] = (uint16_t)ntri; }
}

/* -- the scenery: trees and bushes beside the road, cards facing back
 * along the track (towards the karts coming), off the road everywhere --
 * a list a chunk, near (all) and far (the trees) */

static uint32_t hash32(uint32_t h) {
	h *= 0x9E3779B1u; h ^= h >> 15; h *= 0x85EBCA77u; h ^= h >> 13;
	return h;
}

/* a card w wide and h high standing at (x, z), facing back along seg g */
static void scen_card(const seg_t *g, int32_t x, int32_t z, int w, int h, int tex) {
	int32_t rx = (-(int32_t)g->dz * w / 2) / 16384, rz = ((int32_t)g->dx * w / 2) / 16384;
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	glBegin(GL_TRIANGLES);
	glNormal3i(0, 1, 0);
	glTexCoord2i(0, 16); glVertex3i(x - rx, 0, z - rz);
	glTexCoord2i(16, 16); glVertex3i(x + rx, 0, z + rz);
	glTexCoord2i(16, 0); glVertex3i(x + rx, h, z + rz);
	glTexCoord2i(0, 16); glVertex3i(x - rx, 0, z - rz);
	glTexCoord2i(16, 0); glVertex3i(x + rx, h, z + rz);
	glTexCoord2i(0, 0); glVertex3i(x - rx, h, z - rz);
	glEnd();
	ntri += 2;
}

static void scenery(int c, int far, GLuint into) {
	int s0 = c * CHUNK, s1 = s0 + CHUNK, i;
	if (s1 > tk_n) s1 = tk_n;
	if (into) { ntri = 0; glNewList(into, GL_COMPILE); }
	mk_material(W3_R_GRASS);
	for (i = s0; i < s1; i++) {
		int k;
		for (k = 0; k < 2; k++) {
			uint32_t h = hash32((uint32_t)(i * 2 + k) + 991u);
			int side = (h & 1) ? 1 : -1, tree = (int)((h >> 1) % 100) < tracks[cur].trees;
			int box = !tree && (h >> 28) % 3 == 0;
			int32_t off = side * (ROAD_HW + KERB_W + 26 + (int32_t)((h >> 4) % 90));
			int32_t x, z, across;
			const seg_t *g = &tk_seg[i];
			if ((h >> 12) % 100 >= 17) continue;                /* 17%: about 3 a chunk */
			if (far && !tree && !box) continue;
			x = g->x + (-(int32_t)g->dz * off) / 16384;
			z = g->z + ((int32_t)g->dx * off) / 16384;
			/* not on (or by) another part of the road */
			tk_nearest(x, z, -1, &across);
			if (across < 0) across = -across;
			if (across < ROAD_HW + KERB_W + 18) continue;
			if (tree) {
				int sz = 36 + (int)((h >> 20) % 24);
				scen_card(g, x, z, sz * 3 / 4, sz, TX_TREE);
			} else if (box) {            /* mausrun's boxes, one on another */
				mk_material(W3_R_BOX);
				scen_card(g, x, z, 16, 16, TX_BOX);
				if ((h >> 26) & 1) {
					int32_t rx = (-(int32_t)g->dz * 16) / 16384, rz = ((int32_t)g->dx * 16) / 16384;
					scen_card(g, x + rx, z + rz, 16, 16, TX_BOX);
				}
				mk_material(W3_R_GRASS);
			} else scen_card(g, x, z, 18, 10, TX_BUSH);
		}
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	if (into) { glEndList(); ch_stris[c][far] = (uint16_t)ntri; }
}

void tk_lists(void) {
	int c, i;
	/* again for another track: the last one's lists given back */
	for (c = 0; c < nchunks; c++) {
		glDeleteLists(ch_list[c][0], 2);
		glDeleteLists(ch_scen[c][0], 2);
		glDeleteLists(ch_both[c][0], 2);
	}
	nchunks = (tk_n + CHUNK - 1) / CHUNK;
	for (c = 0; c < nchunks; c++) {
		int s0 = c * CHUNK, s1 = s0 + CHUNK;
		int32_t x0 = 1 << 30, x1 = -(1 << 30), z0 = 1 << 30, z1 = -(1 << 30);
		if (s1 > tk_n) s1 = tk_n;
		for (i = s0; i <= s1; i++) {
			const seg_t *g = &tk_seg[i % tk_n];
			if (g->x < x0) x0 = g->x;
			if (g->x > x1) x1 = g->x;
			if (g->z < z0) z0 = g->z;
			if (g->z > z1) z1 = g->z;
		}
		ch_x[c] = (x0 + x1) / 2; ch_z[c] = (z0 + z1) / 2;
		ch_r[c] = ((x1 - x0) > (z1 - z0) ? (x1 - x0) : (z1 - z0)) / 2 + ROAD_HW + KERB_W + 24;
		ch_list[c][0] = glGenLists(2);
		ch_list[c][1] = ch_list[c][0] + 1;
		chunk(c, 0, ch_list[c][0]);
		chunk(c, 1, ch_list[c][1]);
		ch_scen[c][0] = glGenLists(2);
		ch_scen[c][1] = ch_scen[c][0] + 1;
		scenery(c, 0, ch_scen[c][0]);
		scenery(c, 1, ch_scen[c][1]);
		/* the road and its scenery, one list: a chunk one call where
		 * two when both are in view (on lakritz_console a call is a
		 * few thousand cycles) */
		ch_both[c][0] = glGenLists(2);
		ch_both[c][1] = ch_both[c][0] + 1;
		for (i = 0; i < 2; i++) {
			glNewList(ch_both[c][i], GL_COMPILE);
			chunk(c, i, 0);
			scenery(c, i, 0);
			glEndList();
		}
	}
}

uint32_t tk_draw(int32_t ex, int32_t ez, uint32_t yaw, int32_t far, int seg_hint) {
	/* each chunk's place in the view once, in 32 bits (a track is a few
	 * thousand units across: x 65536 fits): along (sin, -cos), across
	 * (cos, sin). The 64-bit sums twice a chunk were a Lakritz cost. */
	int32_t s = zglSin(yaw), co = zglCos(yaw);
	int32_t far_s = far * 3 / 4;
	int c;
	uint32_t tris = 0;
	(void)seg_hint;
	tk_calls = 0;
	/* the grass is the sky list's, below the horizon: the road on it */
	glEnable(GL_DEPTH_TEST);
	for (c = 0; c < nchunks; c++) {
		int32_t dx = ch_x[c] - ex, dz = ch_z[c] - ez, r = ch_r[c], rs = r + 60;
		int32_t al = (dx * s - dz * co) >> 16, ac = (dx * co + dz * s) >> 16;
		int lod = al - r > LOD_NEAR;
		bool road, scen;
		if (ac < 0) ac = -ac;
		road = !(al < -r || al - r > far || ac - r > al + al / 5 + r);   /* a 100-degree view */
		scen = tk_scenery && ch_stris[c][lod] &&
		       !(al < -rs || al - rs > far_s || ac - rs > al + al / 5 + rs);
		if (road && scen) {
			glCallList(ch_both[c][lod]);
			tris += ch_tris[c][lod] + ch_stris[c][lod];
		} else if (road) {
			glCallList(ch_list[c][lod]);
			tris += ch_tris[c][lod];
		} else if (scen) {
			glCallList(ch_scen[c][lod]);
			tris += ch_stris[c][lod];
		} else continue;
		tk_calls++;
	}
	return tris;
}
