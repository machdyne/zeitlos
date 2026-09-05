/*
 * Zeitlos -- gpu3d
 *
 * Streaming STL loader.
 *
 * ============================================================
 * The problem this exists to solve
 * ============================================================
 *
 * teapot.stl is about 400KB. This machine has 1MB of main memory
 * TOTAL, shared by the kernel, wm, and every running app, and an
 * app's malloc heap is 16KB (Z_PROC_STACK_SIZE_DEFAULT, see
 * sw/os/kernel.h -- that tier is stack AND heap, together, for the
 * process's whole life). fs_mallocfile() (zfsapp.h), which every
 * other file-reading app in this tree uses, would need 400KB in one
 * allocation. It cannot work here and never will.
 *
 * So nothing is ever held whole. The file is pulled through a
 * 512-byte window with fs_read_chunk() (zfsapp.h), triangle by
 * triangle, and what is KEPT is a bounded, decimated model that fits
 * in MODEL_MAX_VERTS/MODEL_MAX_EDGES (model.h). The ceiling on file
 * size is the SD card's, not RAM's.
 *
 * ============================================================
 * Why decimation is vertex CLUSTERING and not "every Nth triangle"
 * ============================================================
 *
 * The obvious way to fit 8000 triangles into a 1500-edge budget is to
 * keep every 6th one. Do not do this, and it is worth writing down
 * why, because it is the first thing anyone reaches for.
 *
 * Triangles in an STL are an unordered soup that happens to describe
 * a surface; adjacent triangles share vertices exactly. Keeping every
 * 6th triangle keeps a scattering of triangles that share almost
 * nothing -- so the vertex count does NOT drop to a sixth, it stays
 * at very nearly 3 per triangle, because welding has nothing left to
 * weld. Worse, it looks like confetti: a cloud of disconnected
 * triangles floating in the shape of a teapot, with no continuous
 * wireframe anywhere.
 *
 * What this does instead is VERTEX CLUSTERING, the classic
 * Rossignac-Borrel decimation. The model's bounding box is divided
 * into a grid[N]^3 of cells; every vertex is snapped to the cell it
 * falls in; all vertices in one cell collapse to a single kept
 * vertex; triangles whose corners collapse together vanish; and the
 * edges that survive are deduplicated. The result is a genuine
 * lower-resolution wireframe of the WHOLE object -- connected,
 * continuous, and recognisably a teapot -- rather than a sample of
 * pieces of it. It also gets the shared-edge dedup that stride
 * sampling cannot: a closed mesh has each edge in two triangles, so
 * naive per-triangle drawing does every edge twice, and simply not
 * doing that is a 2x frame-rate win before anything else.
 *
 * N is chosen by trying: see the ladder in stl_load(). Too fine and
 * the vertex or edge pool overflows; too coarse and detail is thrown
 * away for nothing. There is no way to predict the right N from the
 * triangle count alone, because it depends on the surface's shape --
 * so it is measured, by attempting the build and retrying coarser if
 * the pool fills. Overflow aborts the pass immediately rather than
 * reading the rest of a file whose result is already being thrown
 * away.
 *
 * ============================================================
 * Passes over the file
 * ============================================================
 *
 * Clustering needs the bounding box before it can quantize anything,
 * and the bounding box is only known after reading every vertex. So:
 *
 *   pass 1   measure the bounding box (no storage at all)
 *   pass 2   build, quantizing against that box
 *   pass 2'  ...repeated with a coarser grid if the pool overflowed
 *
 * Rewinding is fs_seek() (zfsapp.h), not close-and-reopen -- the
 * handle stays owned by this process throughout, and the kernel-side
 * handle table is only Z_FS_MAX_OPEN (8) entries deep, so it is worth
 * not churning it.
 *
 * This is the cost of not having the file in memory: it is read more
 * than once. On a 400KB file that is seconds, which is why stl_load()
 * takes a `pump` callback and calls it every chunk -- see its comment
 * in stl.h for what happens to the whole screen if an app goes quiet
 * for that long.
 *
 * ============================================================
 * Floating point
 * ============================================================
 *
 * STL stores IEEE-754 binary32, and this target has no FPU: arch.mk
 * builds rv32im, no F extension. Every float here is decoded with
 * integer operations straight into Q12 fixed point -- f32_to_fixed()
 * takes the bit pattern apart by hand, parse_fixed() parses ASCII
 * decimals directly.
 *
 * That is not premature optimisation, it is avoiding a large and
 * entirely unnecessary cost: touching a float at all pulls newlib's
 * soft-float support (__addsf3, __mulsf3, and for the ASCII path
 * strtod, which drags in most of stdio's conversion machinery) into
 * an image where every byte of .rodata is a byte of the 1MB pool for
 * the app's whole lifetime. The conversions below are a few dozen
 * integer instructions each and add nothing to the link.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "model.h"
#include "stl.h"

#ifdef STL_HOST_TEST
/* Host-side unit test build -- see test/stl_test.c. Provides the
 * five fs_* calls below against an ordinary FILE*, so the parser and
 * the decimator can be exercised on a workstation against a real
 * teapot instead of only on hardware. */
#include "stl_host_stubs.h"
#else
#include "../../common/zfsapp.h"
#endif

/* ============================================================
 * instrumentation
 * ============================================================
 *
 * rdcycle is a free-running hardware counter on sys_clk. Note it
 * counts WALL cycles and is a single GLOBAL counter, not virtualised
 * per process (see sw/os/sh.c's own note) -- so anything measured
 * here includes cycles spent in other processes while this one was
 * preempted. That is the right thing for "how long did the user
 * wait", which is the question this instrumentation exists to answer.
 */

#ifdef STL_HOST_TEST
static uint32_t stl_cyc(void) { return 0; }
#else
static uint32_t stl_cyc(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}
#endif

static stl_stats_t stats;

const stl_stats_t *stl_last_stats(void) {
	return &stats;
}

/* ============================================================
 * error reporting
 * ============================================================ */

static const char *stl_err = "no error";

const char *stl_last_error(void) {
	return stl_err;
}

/* ============================================================
 * streaming reader
 * ============================================================
 *
 * 4KB, raised from one 512-byte SD sector after a 400KB teapot took
 * ~60 seconds to load.
 *
 * Every chunk costs a Z_SYS_FS_READ_CHUNK syscall (trap, argument
 * marshalling, a FatFs f_read) plus one `pump` callback that drains
 * the wm message queue. At 512 bytes over ~1.2MB of reads -- this
 * makes two to three passes, see stl_load() -- that was ~2400 of each.
 * At 4KB it is ~300. The per-byte SPI cost is unchanged; what goes
 * away is the fixed cost paid per chunk.
 *
 * The ceiling on this is the pump interval, not memory. wm blocks
 * waiting for a redraw ack and gives up after REDRAW_ACK_TIMEOUT
 * (wm.c), so chunks must stay small enough that this app answers well
 * inside that. 4KB is roughly 4ms of SD time -- two orders of
 * magnitude clear of the timeout, so there is headroom to go further
 * if the measurements say I/O is still dominant.
 *
 * The buffer is .bss, not stack: an app's entire stack AND malloc
 * heap is one 16KB tier (sw/os/kernel.h).
 */

#define STL_CHUNK   4096

typedef struct {

	int		handle;
	uint8_t		buf[STL_CHUNK];
	int		len;		/* valid bytes in buf */
	int		idx;		/* read position within buf */
	bool		eof;

	void		(*pump)(void);

} stl_stream_t;

static bool st_fill(stl_stream_t *s) {

	if (s->eof) return false;

	/* Timed narrowly around the syscall, so the reported I/O figure
	 * is SD card plus syscall and nothing else -- the difference
	 * against the total is then unambiguously parse cost. */
	uint32_t t0 = stl_cyc();
	int n = fs_read_chunk(s->handle, s->buf, STL_CHUNK);
	stats.io_cycles += (uint64_t)(stl_cyc() - t0);

	if (n > 0) stats.bytes += (uint32_t)n;

	if (n <= 0) {
		s->len = 0;
		s->idx = 0;
		s->eof = true;
		return false;
	}

	s->len = n;
	s->idx = 0;

	/* Hand the app a chance to service wm before going back for the
	 * next chunk. See stl_load()'s comment in stl.h. */
	if (s->pump) s->pump();

	return true;

}

static int st_getc(stl_stream_t *s) {

	if (s->idx >= s->len && !st_fill(s)) return -1;

	return s->buf[s->idx++];

}

static bool st_read(stl_stream_t *s, void *dst, int n) {

	uint8_t *d = (uint8_t *)dst;

	while (n > 0) {

		if (s->idx >= s->len && !st_fill(s)) return false;

		int avail = s->len - s->idx;
		int take = (n < avail) ? n : avail;

		memcpy(d, s->buf + s->idx, take);

		s->idx += take;
		d += take;
		n -= take;

	}

	return true;

}

static bool st_seek(stl_stream_t *s, uint32_t off) {

	if (!fs_seek(s->handle, off)) return false;

	s->len = 0;
	s->idx = 0;
	s->eof = false;

	return true;

}

/* Reads one whitespace-delimited token. Returns false at EOF. Tokens
 * longer than the buffer are truncated, not split -- the remainder is
 * consumed and discarded, so the stream stays aligned on token
 * boundaries whatever the input looks like. */
static bool st_token(stl_stream_t *s, char *out, int outsz) {

	int c;

	do {
		c = st_getc(s);
		if (c < 0) return false;
	} while (c == ' ' || c == '\t' || c == '\r' || c == '\n');

	int n = 0;

	while (c >= 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
		if (n < outsz - 1) out[n++] = (char)c;
		c = st_getc(s);
	}

	out[n] = 0;

	return true;

}

/* ============================================================
 * float conversion, without a float
 * ============================================================ */

/* IEEE-754 binary32 bit pattern -> Q12.
 *
 * Denormals, zero, infinity and NaN all become 0: none of them is a
 * meaningful vertex coordinate, and a NaN that reached the bounding
 * box would poison the whole model's scale. */
static fixed_t f32_to_fixed(uint32_t bits) {

	int sign = (int)(bits >> 31);
	int exp  = (int)((bits >> 23) & 0xFF);
	uint32_t man = bits & 0x7FFFFFu;

	if (exp == 0 || exp == 255) return 0;

	man |= 0x800000u;	/* implicit leading 1 */

	/* value = man * 2^(exp-127-23); we want it scaled by 2^FIXED_SHIFT */
	int shift = (exp - 127 - 23) + FIXED_SHIFT;

	int64_t v;

	if (shift >= 0) {
		if (shift > 39) return sign ? INT32_MIN : INT32_MAX;
		v = (int64_t)man << shift;
	} else {
		if (shift < -31) return 0;
		v = (int64_t)man >> (-shift);
	}

	if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;

	return (fixed_t)(sign ? -v : v);

}

/* ASCII decimal (with optional sign, fraction and exponent) -> Q12.
 *
 * Mantissa digits are capped at 12 so that (mant << FIXED_SHIFT)
 * cannot overflow int64: 10^12 * 4096 is ~4.1e15 against int64's
 * 9.2e18. Digits past the cap still count towards the decimal
 * exponent, so magnitude survives even when precision does not --
 * which is the right way round for a coordinate. */
static fixed_t parse_fixed(const char *s) {

	int neg = 0;

	if (*s == '+' || *s == '-') {
		neg = (*s == '-');
		s++;
	}

	int64_t mant = 0;
	int digits = 0;
	int e10 = 0;

	while (*s >= '0' && *s <= '9') {
		if (digits < 12) { mant = mant * 10 + (*s - '0'); digits++; }
		else e10++;
		s++;
	}

	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			if (digits < 12) { mant = mant * 10 + (*s - '0'); digits++; e10--; }
			s++;
		}
	}

	if (*s == 'e' || *s == 'E') {
		s++;
		int eneg = 0;
		if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
		int ev = 0;
		while (*s >= '0' && *s <= '9') {
			if (ev < 4096) ev = ev * 10 + (*s - '0');
			s++;
		}
		e10 += eneg ? -ev : ev;
	}

	if (mant == 0) return 0;

	int64_t v = mant << FIXED_SHIFT;

	if (e10 > 0) {
		for (int i = 0; i < e10; i++) {
			if (v > (int64_t)0x0FFFFFFFFFFFFFFFLL) { v = 0x7FFFFFFFLL; break; }
			v *= 10;
		}
	} else if (e10 < 0) {
		int k = -e10;
		if (k > 18) return 0;
		int64_t d = 1;
		for (int i = 0; i < k; i++) d *= 10;
		v /= d;
	}

	if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;

	return (fixed_t)(neg ? -v : v);

}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ============================================================
 * clustering tables
 * ============================================================
 *
 * Both hash tables are open-addressed with linear probing and store
 * index+1, so 0 means empty and no separate occupancy bitmap is
 * needed. Both are sized comfortably larger than the pool they index
 * -- which is not just about load factor, it is what GUARANTEES an
 * empty slot exists so the probe loop always terminates.
 *
 * They track MODEL_MAX_VERTS/MODEL_MAX_EDGES (model.h) and must stay
 * strictly larger than them, which is not merely about load factor:
 * it is what GUARANTEES an empty slot exists so the probe loops below
 * always terminate. If you raise the model budget, raise these too.
 *
 * These are .bss and are only live during a load. They are not
 * unioned with anything: the projection arrays in gpu3d.c are the
 * obvious candidate, but overlapping load-time and render-time state
 * to save 7KB is exactly the sort of saving that turns into a
 * corrupted model the first time someone renders a frame during a
 * load, which is precisely what the `pump` callback makes possible.
 */

#define VHASH_SIZE   512	/* > MODEL_MAX_VERTS (256) */
#define EHASH_SIZE   512	/* > MODEL_MAX_EDGES (384) */

static uint16_t vhash[VHASH_SIZE];
static uint16_t ehash[EHASH_SIZE];

/* the grid cell each kept vertex came from, for hash comparison */
static uint32_t vkey[MODEL_MAX_VERTS];

static uint32_t hash32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/* Integer square root, for the grid estimator in stl_load(). Classic
 * restoring binary method -- no float, no libm, ~16 iterations. */
static uint32_t isqrt32(uint32_t n) {

	uint32_t x = 0;
	uint32_t b = 1u << 30;

	while (b > n) b >>= 2;

	while (b) {
		if (n >= x + b) {
			n -= x + b;
			x = (x >> 1) + b;
		} else {
			x >>= 1;
		}
		b >>= 2;
	}

	return x;

}

/* ============================================================
 * one pass over the file
 * ============================================================ */

typedef struct {

	int		build;		/* 0 = measure bbox, 1 = build */

	/* measure */
	fixed_t		mn[3], mx[3];
	int		any;
	uint32_t	ntris;

	/* build */
	model_t		*m;
	fixed_t		org[3];		/* bbox minimum, the grid's origin */
	fixed_t		cell;		/* grid cell size */
	int		grid;		/* cells per axis */
	int		overflow;	/* pool filled; retry coarser */

} stl_ctx_t;

/* Snaps a vertex to its grid cell and returns the cell's key.
 *
 * The kept position is the FIRST vertex that landed in the cell, not
 * the cell's centre. Both are defensible; a real surface point is
 * preferred because cell centres visibly inflate a curved surface
 * outwards and make a rounded object look faceted in a way the
 * original is not. */
static uint32_t cell_key(stl_ctx_t *c, const vertex3d_t *v) {

	fixed_t comp[3];
	int32_t ci[3];

	comp[0] = v->x;
	comp[1] = v->y;
	comp[2] = v->z;

	for (int a = 0; a < 3; a++) {

		int32_t q = 0;

		if (c->cell > 0) {

			/* 32-bit, NOT int64. This is called nine times per
			 * triangle (three axes, three vertices) on every build
			 * pass, and an int64 numerator here compiles to a call
			 * into libgcc's __divdi3 -- several hundred cycles --
			 * where a 32-bit divide is a single hardware DIV
			 * instruction at ~32. On a 9438-triangle teapot that is
			 * ~85,000 software divisions per pass, and it was the
			 * single largest item in the loader's CPU time.
			 *
			 * Safe in 32 bits by construction: both terms are
			 * fixed_t, and their difference is bounded by the
			 * model's extent along this axis, which stl_load()
			 * already computed AS a fixed_t. If the extent fits,
			 * the difference fits. */
			q = (comp[a] - c->org[a]) / c->cell;

		}

		if (q < 0) q = 0;
		if (q >= c->grid) q = c->grid - 1;

		ci[a] = q;

	}

	return ((uint32_t)ci[2] * (uint32_t)c->grid + (uint32_t)ci[1]) *
		(uint32_t)c->grid + (uint32_t)ci[0];

}

/* Returns the index of the kept vertex for this cell, adding it if
 * this is the first vertex seen there. -1 means the pool is full. */
static int vert_intern(stl_ctx_t *c, uint32_t key, const vertex3d_t *v) {

	model_t *m = c->m;
	uint32_t h = hash32(key) & (VHASH_SIZE - 1);

	for (;;) {

		uint16_t e = vhash[h];

		if (e == 0) {

			if (m->nverts >= MODEL_MAX_VERTS) return -1;

			int idx = m->nverts++;

			m->verts[idx] = *v;
			vkey[idx] = key;
			vhash[h] = (uint16_t)(idx + 1);

			return idx;

		}

		if (vkey[e - 1] == key) return (int)(e - 1);

		h = (h + 1) & (VHASH_SIZE - 1);

	}

}

/* Adds edge (a,b) if it isn't already present. Returns false if the
 * edge pool is full. A degenerate edge (a == b, both corners of the
 * triangle collapsed into one cell) is silently dropped -- that is
 * the decimation working, not an error. */
static bool edge_add(model_t *m, int a, int b) {

	if (a == b) return true;

	uint16_t lo = (uint16_t)((a < b) ? a : b);
	uint16_t hi = (uint16_t)((a < b) ? b : a);

	uint32_t key = ((uint32_t)lo << 16) | (uint32_t)hi;
	uint32_t h = hash32(key) & (EHASH_SIZE - 1);

	for (;;) {

		uint16_t e = ehash[h];

		if (e == 0) {

			if (m->nedges >= MODEL_MAX_EDGES) return false;

			int idx = m->nedges++;

			m->edges[idx].v0 = lo;
			m->edges[idx].v1 = hi;
			ehash[h] = (uint16_t)(idx + 1);

			return true;

		}

		if (m->edges[e - 1].v0 == lo && m->edges[e - 1].v1 == hi) return true;

		h = (h + 1) & (EHASH_SIZE - 1);

	}

}

/* Returns false to abort the pass early (pool overflow only). */
static bool emit_tri(stl_ctx_t *c, const vertex3d_t *v) {

	c->ntris++;

	if (!c->build) {

		for (int i = 0; i < 3; i++) {

			fixed_t comp[3];

			comp[0] = v[i].x;
			comp[1] = v[i].y;
			comp[2] = v[i].z;

			for (int a = 0; a < 3; a++) {
				if (!c->any || comp[a] < c->mn[a]) c->mn[a] = comp[a];
				if (!c->any || comp[a] > c->mx[a]) c->mx[a] = comp[a];
			}

			c->any = 1;

		}

		return true;

	}

	int idx[3];

	for (int i = 0; i < 3; i++) {
		idx[i] = vert_intern(c, cell_key(c, &v[i]), &v[i]);
		if (idx[i] < 0) { c->overflow = 1; return false; }
	}

	if (!edge_add(c->m, idx[0], idx[1]) ||
		!edge_add(c->m, idx[1], idx[2]) ||
		!edge_add(c->m, idx[2], idx[0])) {
		c->overflow = 1;
		return false;
	}

	return true;

}

/* -- binary --
 *
 * 80-byte header, uint32 triangle count, then 50 bytes per triangle:
 * a 3-float normal (ignored -- this is a wireframe renderer, there is
 * nothing to light), three 3-float vertices, and a uint16 attribute
 * word.
 */
static bool pass_binary(stl_stream_t *s, stl_ctx_t *c, uint32_t ntri) {

	if (!st_seek(s, 84)) return false;

	for (uint32_t t = 0; t < ntri; t++) {

		uint8_t rec[50];

		if (!st_read(s, rec, 50)) break;	/* truncated file: keep what we have */

		vertex3d_t v[3];

		for (int i = 0; i < 3; i++) {
			const uint8_t *p = rec + 12 + i * 12;
			v[i].x = f32_to_fixed(le32(p));
			v[i].y = f32_to_fixed(le32(p + 4));
			v[i].z = f32_to_fixed(le32(p + 8));
		}

		if (!emit_tri(c, v)) return false;

	}

	return true;

}

/* -- ASCII --
 *
 * Driven off the "vertex" keyword alone rather than by matching the
 * full facet/outer loop/endloop/endfacet grammar. Every three
 * vertices make a triangle; "facet" resets the group.
 *
 * That is deliberately lenient. Real STL files in the wild vary in
 * whitespace, capitalisation of the solid name, and whether they
 * bother with the normal at all, and none of that variation changes
 * what the geometry is. A strict parser would reject files that every
 * other tool opens.
 */
static bool pass_ascii(stl_stream_t *s, stl_ctx_t *c) {

	if (!st_seek(s, 0)) return false;

	char tok[48];
	vertex3d_t v[3];
	int nv = 0;

	while (st_token(s, tok, sizeof(tok))) {

		if (tok[0] == 'v' && !strcmp(tok, "vertex")) {

			char a[48], b[48], d[48];

			if (!st_token(s, a, sizeof(a))) break;
			if (!st_token(s, b, sizeof(b))) break;
			if (!st_token(s, d, sizeof(d))) break;

			if (nv < 3) {
				v[nv].x = parse_fixed(a);
				v[nv].y = parse_fixed(b);
				v[nv].z = parse_fixed(d);
				nv++;
			}

			if (nv == 3) {
				if (!emit_tri(c, v)) return false;
				nv = 0;
			}

		} else if (tok[0] == 'f' && !strcmp(tok, "facet")) {

			nv = 0;

		}

	}

	return true;

}

/* ============================================================
 * format detection
 * ============================================================
 *
 * NOT "does the file begin with the word solid". That is the usual
 * test and it is wrong often enough to matter: a fair number of
 * exporters write a binary file whose 80-byte header happens to start
 * with the ASCII text "solid ...", and such a file fed to an ASCII
 * parser yields nothing at all.
 *
 * The reliable test is arithmetic. A binary STL's size is exactly
 * 84 + 50*count, where count is the uint32 at offset 80. A text file
 * matching that by coincidence is vanishingly unlikely. Trailing
 * junk after the last triangle is tolerated only when the file does
 * NOT start with "solid", since that combination cannot be ASCII.
 */

#define FMT_ASCII   0
#define FMT_BINARY  1

static int detect_format(stl_stream_t *s, uint32_t fsize, uint32_t *ntri) {

	uint8_t hdr[84];

	*ntri = 0;

	if (!st_seek(s, 0)) return FMT_ASCII;
	if (!st_read(s, hdr, 84)) return FMT_ASCII;

	uint32_t n = le32(hdr + 80);

	if (n == 0) return FMT_ASCII;

	uint64_t expect = (uint64_t)84 + (uint64_t)50 * (uint64_t)n;

	if ((uint64_t)fsize == expect) { *ntri = n; return FMT_BINARY; }

	int looks_ascii = (hdr[0] == 's' && hdr[1] == 'o' && hdr[2] == 'l' &&
		hdr[3] == 'i' && hdr[4] == 'd');

	if (!looks_ascii && (uint64_t)fsize >= expect) { *ntri = n; return FMT_BINARY; }

	return FMT_ASCII;

}

/* ============================================================
 * normalization
 * ============================================================ */

void model_normalize(model_t *m) {

	if (m->nverts <= 0) return;

	fixed_t mn[3], mx[3];

	mn[0] = mx[0] = m->verts[0].x;
	mn[1] = mx[1] = m->verts[0].y;
	mn[2] = mx[2] = m->verts[0].z;

	for (int i = 1; i < m->nverts; i++) {

		fixed_t c[3];

		c[0] = m->verts[i].x;
		c[1] = m->verts[i].y;
		c[2] = m->verts[i].z;

		for (int a = 0; a < 3; a++) {
			if (c[a] < mn[a]) mn[a] = c[a];
			if (c[a] > mx[a]) mx[a] = c[a];
		}

	}

	fixed_t ctr[3];
	fixed_t half = 0;

	for (int a = 0; a < 3; a++) {
		ctr[a] = (fixed_t)(((int64_t)mn[a] + (int64_t)mx[a]) / 2);
		fixed_t h = (fixed_t)(((int64_t)mx[a] - (int64_t)mn[a]) / 2);
		if (h > half) half = h;
	}

	/* A degenerate model (every vertex identical) would divide by
	 * zero here; leave it centred and unscaled instead. */
	if (half <= 0) {
		for (int i = 0; i < m->nverts; i++) {
			m->verts[i].x -= ctr[0];
			m->verts[i].y -= ctr[1];
			m->verts[i].z -= ctr[2];
		}
		return;
	}

	/* Scale so the largest half-extent becomes exactly FIXED_ONE --
	 * the built-in cube's own half-extent, which is what lets the
	 * renderer treat every model identically. */
	for (int i = 0; i < m->nverts; i++) {
		m->verts[i].x = fixed_div(m->verts[i].x - ctr[0], half);
		m->verts[i].y = fixed_div(m->verts[i].y - ctr[1], half);
		m->verts[i].z = fixed_div(m->verts[i].z - ctr[2], half);
	}

}

/* ============================================================
 * entry point
 * ============================================================ */

static void set_name(model_t *m, const char *path) {

	const char *base = path;

	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\') base = p + 1;

	int i = 0;

	while (base[i] && i < MODEL_NAME_MAX - 1) {
		m->name[i] = base[i];
		i++;
	}

	m->name[i] = 0;

}

bool stl_load(const char *path, model_t *m, void (*pump)(void)) {

	stl_err = "no error";

	memset(&stats, 0, sizeof(stats));

	uint32_t t_load = stl_cyc();

	m->nverts = 0;
	m->nedges = 0;

	/* No faces from an STL import -- yet.
	 *
	 * The loader already reads triangles (that is what an STL IS) and
	 * then converts them to a deduplicated edge list, discarding the
	 * faces. Keeping them is a real change: the triangle count after
	 * cluster decimation is far above MODEL_MAX_TRIS, so it needs its
	 * own budget and its own decimation, not just an array.
	 *
	 * Zeroing it here is what makes the renderer degrade gracefully
	 * rather than draw garbage: ntris == 0 means "wireframe only", and
	 * the shaded path falls back on its own. See mtri_t in model.h. */
	m->ntris = 0;
	m->decimated = 0;
	m->name[0] = 0;

	int fsize = fs_size((char *)path);

	if (fsize <= 0) {
		stl_err = "file not found or empty";
		return false;
	}

	static stl_stream_t s;	/* .bss, not stack: 512-byte buffer vs a
				 * 16KB stack+heap tier (kernel.h) */

	s.handle = fs_open_read(path);

	if (s.handle < 0) {
		stl_err = "could not open file";
		return false;
	}

	s.len = 0;
	s.idx = 0;
	s.eof = false;
	s.pump = pump;

	uint32_t ntri = 0;
	int fmt = detect_format(&s, (uint32_t)fsize, &ntri);

	stats.binary = (fmt == FMT_BINARY);

	/* -- pass 1: bounding box -- */

	stl_ctx_t ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.build = 0;

	stats.passes++;

	uint32_t t_bbox = stl_cyc();

	bool ok = (fmt == FMT_BINARY) ? pass_binary(&s, &ctx, ntri)
				      : pass_ascii(&s, &ctx);

	stats.bbox_cycles += (uint64_t)(stl_cyc() - t_bbox);

	if (!ok || !ctx.any || ctx.ntris == 0) {
		stats.total_cycles += (uint64_t)(stl_cyc() - t_load);
		fs_close_handle(s.handle);
		stl_err = "no triangles found";
		return false;
	}

	uint32_t total_tris = ctx.ntris;

	stats.tris = total_tris;

	fixed_t org[3], ext = 0;

	for (int a = 0; a < 3; a++) {
		org[a] = ctx.mn[a];
		fixed_t e = ctx.mx[a] - ctx.mn[a];
		if (e > ext) ext = e;
	}

	/* -- pass 2: build, retrying coarser on overflow --
	 *
	 * The first grid to try: 64 for a model small enough to
	 * plausibly fit whole, where the grid's only real job is welding
	 * exactly-coincident shared vertices (worth doing on its own --
	 * it is the difference between 3 vertices per triangle and
	 * roughly half of one). Anything larger starts at 24, which is
	 * about where a closed surface's occupied-cell count lands near
	 * MODEL_MAX_VERTS.
	 *
	 * When a grid overflows, the next one is ESTIMATED rather than
	 * taken from a fixed ladder, and it is worth explaining why,
	 * because a ladder was what this did first and it was measurably
	 * worse.
	 *
	 * Occupied cells on a surface scale with the square of the grid
	 * resolution -- a surface is two-dimensional, so halving the
	 * cell size roughly quadruples the cells it touches. An overflow
	 * tells us more than "too fine": it tells us the pool filled
	 * after `ctx.ntris` of `total_tris` triangles, i.e. after a
	 * fraction f of the model. The full model at that grid would
	 * therefore have wanted about 1/f times the budget, so the grid
	 * that just fits is about grid*sqrt(f).
	 *
	 * A fixed ladder cannot do this. Stepping 24 -> 17 on the 8257
	 * triangle binary teapot overshot to 249 vertices -- a third of
	 * the budget spent, two thirds of the available detail thrown
	 * away -- because 17 was simply the next rung down, not an
	 * answer to anything measured. The estimate lands near the
	 * budget on the first retry instead, and a finer ladder would
	 * only have paid for the same accuracy with more passes.
	 *
	 * The 19/20 is deliberate undershoot: landing slightly under
	 * budget costs a little detail, landing over costs an entire
	 * extra pass over the file.
	 *
	 * A failed attempt is cheap -- emit_tri() aborts the pass the
	 * moment the pool fills, so it reads a fraction of the file, not
	 * all of it. Only the successful attempt reads to the end. */

	int grid = (total_tris <= 700) ? 64 : 24;
	bool built = false;

	for (int attempt = 0; attempt < 8; attempt++) {

		memset(vhash, 0, sizeof(vhash));
		memset(ehash, 0, sizeof(ehash));

		m->nverts = 0;
		m->nedges = 0;

		memset(&ctx, 0, sizeof(ctx));
		ctx.build = 1;
		ctx.m = m;
		ctx.grid = grid;
		ctx.org[0] = org[0];
		ctx.org[1] = org[1];
		ctx.org[2] = org[2];

		/* +1 so the cell size is a hair over extent/grid: a vertex
		 * exactly on the maximum face then still quantizes to
		 * grid-1 by arithmetic rather than relying on the clamp. */
		ctx.cell = (fixed_t)(ext / grid) + 1;

		stats.passes++;

		uint32_t t_build = stl_cyc();

		ok = (fmt == FMT_BINARY) ? pass_binary(&s, &ctx, ntri)
					 : pass_ascii(&s, &ctx);

		stats.build_cycles += (uint64_t)(stl_cyc() - t_build);

		if (!ctx.overflow && m->nverts > 0) {
			stats.grid = grid;
			m->decimated = (grid != 64);
			built = true;
			break;
		}

		if (grid <= 4) break;

		/* Estimate the grid that would just fit, from where this one
		 * overflowed.
		 *
		 * Occupied cells scale with the SQUARE of grid resolution (a
		 * surface is two-dimensional), so if we knew the final count
		 * at this grid we could scale directly. We do not: the pass
		 * aborted early, at `done` of `total_tris` triangles.
		 *
		 * The tempting extrapolation is linear -- "it filled the pool
		 * after a fraction f, so it wanted 1/f times the budget" --
		 * and that is what this did first. It is badly wrong, and the
		 * error only became visible once the budget was reduced.
		 * Occupied cells SATURATE: early triangles each land in a
		 * fresh cell, later ones mostly hit cells already taken, so
		 * linear extrapolation wildly over-predicts the final count
		 * and the correction overshoots. Measured, it turned an
		 * 8257-triangle teapot into 104 edges against a 384 budget --
		 * a quarter of the detail that fits, thrown away.
		 *
		 * The two honest bounds are: fully saturated (final == what
		 * we already have) and fully linear (final == budget/f).
		 * Their geometric mean, budget/sqrt(f), is the estimate used
		 * here. Since count scales as grid squared, correcting for it
		 * means multiplying the grid by the FOURTH root of f:
		 *
		 *     grid' = grid * (done/total)^(1/4)
		 *
		 * computed below as isqrt(isqrt(grid^4 * done / total)) --
		 * two integer square roots, no floating point anywhere.
		 *
		 * The 31/32 is a deliberate slight undershoot: landing a
		 * little under budget costs a little detail, landing over
		 * costs an entire extra pass over the file. */

		uint64_t done = ctx.ntris ? (uint64_t)ctx.ntris : 1;

		uint64_t g2 = (uint64_t)grid * (uint64_t)grid;
		uint64_t r = (g2 * g2 * done) / (uint64_t)total_tris;

		if (r > 0xFFFFFFFFULL) r = 0xFFFFFFFFULL;

		int next = (int)isqrt32(isqrt32((uint32_t)r));

		next = next * 31 / 32;

		if (next >= grid) next = grid - 1;
		if (next < 4) next = 4;

		grid = next;

	}

	fs_close_handle(s.handle);

	stats.total_cycles += (uint64_t)(stl_cyc() - t_load);

	if (!built || m->nverts == 0 || m->nedges == 0) {
		m->nverts = 0;
		m->nedges = 0;
		stl_err = "model too complex to fit";
		return false;
	}

	model_normalize(m);
	set_name(m, path);

	return true;

}
