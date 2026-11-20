// zvenc_core.c -- ZV base-layer encoder (zvenc_core.h, docs/zv.md).
//
// A frame is 8x8 blocks. Each is SKIP (unchanged), V1 (a 4x4 entry of
// the V1 codebook, doubled), V4 (four 4x4 entries of the V4 codebook)
// or a run of REP (the same V1 entry). Both codebooks have 256 entries
// and carry over from frame to frame; an entry is sent again only when
// moving it pays for its 17 bytes.
//
// Codebooks: vector quantisation by Lloyd/LBG iterations (Linde, Buzo
// and Gray, 1980), warm-started from the last frame's books, with
// unused entries reseeded at the worst-coded vectors. Decisions: the
// cheapest of distortion + lambda x bits. Rate control moves lambda.

#include "zvenc_core.h"
#include "zv.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UPD_BITS   (8.0 * (1 + ZV_VEC))
#define UPD16_BITS (8.0 * (1 + ZV_VEC16))
static int n_threads = 1;   // for par_for()

#define BITS_SKIP  0.25
#define BITS_V1    9.0
#define BITS_REP   0.5
#define BITS_V4    33.0
#define BITS_V16   128.5

// A book's entries in order of their sums: the search for a vector's
// nearest entry starts at its own sum and works outward, and stops when
// the difference of sums alone rules the rest out -- by Cauchy-Schwarz,
// |a - b|^2 >= (sum a - sum b)^2 / dim. Exact, and several times faster
// than trying every entry.
typedef struct book {
	uint8_t (*cb)[ZV_VEC];
	int dim;
	int32_t sum[ZV_CB_N];       // sorted
	uint8_t ord[ZV_CB_N];       // entry of each
	uint8_t pos[ZV_CB_N];       // place of each entry
} book_t;

struct zve {
	zve_opts_t o;
	int bw, bh, nb;
	uint8_t cb1[ZV_CB_N][ZV_VEC], cb4[ZV_CB_N][ZV_VEC], cb16[ZV_CB_N][ZV_VEC];
	uint32_t age1[ZV_CB_N], age4[ZV_CB_N], age16[ZV_CB_N];
	struct book b1, b4, b16;
	const uint8_t *cur;         // the frame being coded
	uint32_t probe_blocks;
	double probe_bits;
	uint32_t *vd1, *d1, *d4, *d16;
	uint8_t *i1, *i4, *i16;
	zv_dec_t dec;
	uint8_t *recon;
	// scratch, per frame
	uint8_t *v1;        // nb x 16: 2x2 means
	uint8_t *v4;        // nb x 4 x 16: quadrants
	uint8_t *v16;       // nb x 16 x 4: 2x2 cells
	uint32_t *dskip;
	uint8_t *act;       // block is worth coding
	uint8_t *type;      // 0 skip, 1 v1, 2 v4, 3 v16
	uint8_t *ix;        // nb x 16
	const uint8_t **set;
	uint32_t *d0, *dn;
	int *as;
	double lambda;
	int started;
	int ref_left, ref_row, ref_rows;    // the refresh after a seek point
	uint8_t *force;                     // block in this frame's refresh band
	uint32_t frame;
	uint32_t rng;
};

static uint32_t rnd(zve_t *e) {
	e->rng = e->rng * 1664525u + 1013904223u;
	return e->rng >> 8;
}

size_t zve_max_payload(int w, int h) {
	size_t nb = (size_t)(w / 8) * (h / 8);
	return 6 + 2 * ZV_CB_N * (1 + ZV_VEC) + ZV_CB_N * (1 + ZV_VEC16) + nb * 17 + 64;
}

zve_t *zve_new(const zve_opts_t *o) {
	zve_t *e = calloc(1, sizeof(*e));
	zv_header_t h;
	size_t nb;
	if (!e) return NULL;
	e->o = *o;
	if (e->o.iters <= 0) e->o.iters = 2;
	if (e->o.key_iters <= 0) e->o.key_iters = 6;
	e->bw = o->w / 8;
	e->bh = o->h / 8;
	e->nb = e->bw * e->bh;
	nb = (size_t)e->nb;
	e->recon = calloc((size_t)o->w * o->h, 1);
	e->v1 = malloc(nb * 16);
	e->v4 = malloc(nb * 64);
	e->v16 = malloc(nb * 64);
	e->dskip = malloc(nb * sizeof(uint32_t));
	e->act = malloc(nb);
	e->force = calloc(nb, 1);
	e->type = malloc(nb);
	e->ix = calloc(nb * 16, 1);
	e->set = malloc(nb * 16 * sizeof(*e->set));
	e->d0 = malloc(nb * 16 * sizeof(uint32_t));
	e->dn = malloc(nb * 16 * sizeof(uint32_t));
	e->as = calloc(nb * 16, sizeof(int));
	e->vd1 = malloc(nb * sizeof(uint32_t));
	e->d1 = malloc(nb * sizeof(uint32_t));
	e->d4 = malloc(nb * sizeof(uint32_t));
	e->d16 = malloc(nb * sizeof(uint32_t));
	e->i1 = malloc(nb);
	e->i4 = malloc(nb * 4);
	e->i16 = malloc(nb * 16);
	n_threads = o->threads > 0 ? o->threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (n_threads < 1) n_threads = 1;
	e->lambda = o->lambda > 0 ? o->lambda : 60.0;
	e->rng = 12345;
	memset(&h, 0, sizeof(h));
	h.w = (uint16_t)o->w;
	h.h = (uint16_t)o->h;
	h.fps_num = 24;
	h.fps_den = 1;
	zv_dec_init(&e->dec, &h, ZV_VIEW_GREY_FULL, ZV_MASK_BAYER8, 220);
	return e;
}

void zve_free(zve_t *e) {
	if (!e) return;
	free(e->recon); free(e->v1); free(e->v4); free(e->v16); free(e->dskip); free(e->act);
	free(e->vd1); free(e->d1); free(e->d4); free(e->d16); free(e->i1); free(e->i4); free(e->i16);
	free(e->type); free(e->ix); free(e->force); free(e->set); free(e->d0); free(e->dn); free(e->as);
	free(e);
}

const uint8_t *zve_recon(const zve_t *e) { return e->recon; }

// Books are 256 rows of 16 bytes; a V16 book uses the first `dim` = 4.
static inline uint32_t dvec(const uint8_t *a, const uint8_t *b, int dim) {
	uint32_t s = 0;
	int k;
	for (k = 0; k < dim; k++) { int t = a[k] - b[k]; s += (uint32_t)(t * t); }
	return s;
}

static void book_index(book_t *b, uint8_t cb[ZV_CB_N][ZV_VEC], int dim) {
	int i, j, k;
	int32_t s[ZV_CB_N];
	b->cb = cb;
	b->dim = dim;
	for (i = 0; i < ZV_CB_N; i++) {
		int32_t t = 0;
		for (k = 0; k < dim; k++) t += cb[i][k];
		s[i] = t;
	}
	// insertion sort: 256 entries, mostly in order from the last time
	for (i = 0; i < ZV_CB_N; i++) {
		int32_t v = s[i];
		for (j = i; j > 0 && b->sum[j - 1] > v; j--) { b->sum[j] = b->sum[j - 1]; b->ord[j] = b->ord[j - 1]; }
		b->sum[j] = v;
		b->ord[j] = (uint8_t)i;
	}
	for (i = 0; i < ZV_CB_N; i++) b->pos[b->ord[i]] = (uint8_t)i;
}

static inline uint32_t dpart(const uint8_t *v, const uint8_t *c, int dim, uint32_t bd) {
	uint32_t s = 0;
	int k;
	for (k = 0; k < dim; k += 4) {
		int t0 = v[k] - c[k], t1 = v[k + 1] - c[k + 1], t2 = v[k + 2] - c[k + 2], t3 = v[k + 3] - c[k + 3];
		s += (uint32_t)(t0 * t0 + t1 * t1 + t2 * t2 + t3 * t3);
		if (s >= bd) return s;
	}
	return s;
}

static int nearest(const uint8_t *v, const book_t *b, int guess, uint32_t *dist) {
	int dim = b->dim, best = guess, lo, hi, k;
	int32_t sv = 0;
	uint32_t bd = dvec(v, b->cb[guess], dim);
	for (k = 0; k < dim; k++) sv += v[k];
	// start at the place of the vector's sum
	lo = 0; hi = ZV_CB_N;
	while (lo < hi) { int m = (lo + hi) >> 1; if (b->sum[m] < sv) lo = m + 1; else hi = m; }
	hi = lo;
	lo = lo - 1;
	while (bd && (lo >= 0 || hi < ZV_CB_N)) {
		int dl = lo >= 0 ? sv - b->sum[lo] : 0x7fffffff, dh = hi < ZV_CB_N ? b->sum[hi] - sv : 0x7fffffff;
		int j, ds;
		if (dl <= dh) { j = b->ord[lo--]; ds = dl; } else { j = b->ord[hi++]; ds = dh; }
		if ((uint32_t)((int64_t)ds * ds / dim) >= bd) break;    // and everything further out
		if (j != guess) {
			uint32_t s = dpart(v, b->cb[j], dim, bd);
			if (s < bd) { bd = s; best = j; }
		}
	}
	*dist = bd;
	return best;
}

// ---- a parallel for, over independent items ----------------------------------

typedef void (*job_fn)(void *ctx, int from, int to);
typedef struct { job_fn fn; void *ctx; int from, to; } job_t;

static void *job_run(void *a) {
	job_t *j = a;
	j->fn(j->ctx, j->from, j->to);
	return NULL;
}


static void par_for(int n, job_fn fn, void *ctx) {
	pthread_t th[64];
	job_t jb[64];
	int t, nt = n_threads, made = 0;
	if (nt > 64) nt = 64;
	if (nt <= 1 || n < 256) { fn(ctx, 0, n); return; }
	for (t = 0; t < nt; t++) {
		jb[t].fn = fn; jb[t].ctx = ctx;
		jb[t].from = (int)((int64_t)n * t / nt);
		jb[t].to = (int)((int64_t)n * (t + 1) / nt);
	}
	for (t = 1; t < nt; t++) if (pthread_create(&th[t], NULL, job_run, &jb[t]) == 0) made |= 1 << (t & 31); else job_run(&jb[t]);
	job_run(&jb[0]);
	for (t = 1; t < nt; t++) if (made & (1 << (t & 31))) pthread_join(th[t], NULL);
}

typedef struct { const uint8_t **set; const book_t *bk; int *as; uint32_t *dist; int wrap; } assign_t;

static void assign_job(void *c, int from, int to) {
	assign_t *a = c;
	int i;
	for (i = from; i < to; i++)
		a->as[i] = nearest(a->set[i], a->bk, a->wrap ? a->as[i] % ZV_CB_N : a->as[i], &a->dist[i]);
}

static void assign(const uint8_t **set, int n, const book_t *bk, int *as, uint32_t *dist, int wrap) {
	assign_t a;
	a.set = set; a.bk = bk; a.as = as; a.dist = dist; a.wrap = wrap;
	par_for(n, assign_job, &a);
}

// Refine codebook `cb` (the decoder's) on vectors set[0..n) into a
// tentative book; keep the entries whose move pays for itself (or all,
// on a key frame). `scale` is how much a vector's distance counts in
// pixels (V1 vectors are means of 2x2: 4). Marks changed entries in chg.
static int refine(zve_t *e, uint8_t cb[ZV_CB_N][ZV_VEC], int dim, uint32_t *age, const uint8_t **set, int n,
                  int iters, int key, double scale, double upd_bits, uint8_t *chg) {
	static uint8_t t[ZV_CB_N][ZV_VEC];
	static uint32_t sum[ZV_CB_N][ZV_VEC];
	static uint32_t cnt[ZV_CB_N];
	static double gain[ZV_CB_N];
	static book_t bk;
	int i, j, k, it, nchg = 0;
	memcpy(t, cb, sizeof(t));
	memset(chg, 0, ZV_CB_N);
	if (n == 0) return 0;
	// start: every vector's best under the decoder's book
	book_index(&bk, t, dim);
	assign(set, n, &bk, e->as, e->d0, 1);
	for (it = 0; it < iters; it++) {
		int free_n = 0;
		memset(sum, 0, sizeof(sum));
		memset(cnt, 0, sizeof(cnt));
		if (it > 0) {
			book_index(&bk, t, dim);
			assign(set, n, &bk, e->as, e->dn, 0);
		} else
			memcpy(e->dn, e->d0, (size_t)n * sizeof(uint32_t));
		for (i = 0; i < n; i++) {
			int a = e->as[i];
			cnt[a]++;
			for (k = 0; k < dim; k++) sum[a][k] += set[i][k];
		}
		for (j = 0; j < ZV_CB_N; j++)
			if (cnt[j])
				for (k = 0; k < dim; k++) t[j][k] = (uint8_t)((sum[j][k] + cnt[j] / 2) / cnt[j]);
		// reseed entries nobody uses (and nobody has used for a while)
		for (j = 0; j < ZV_CB_N; j++) if (!cnt[j] && (key || age[j] > 4)) free_n++;
		if (free_n) {
			int lim = key ? free_n : (free_n < 24 ? free_n : 24);
			for (j = 0; j < ZV_CB_N && lim > 0; j++) {
				uint32_t wd = 0;
				int wi = -1, tries;
				if (cnt[j] || (!key && age[j] <= 4)) continue;
				// the worst-coded of a few random samples
				for (tries = 0; tries < 64; tries++) {
					int r = (int)(rnd(e) % (uint32_t)n);
					if (e->dn[r] > wd) { wd = e->dn[r]; wi = r; }
				}
				if (wi < 0 || wd * scale < 256) break;
				memcpy(t[j], set[wi], (size_t)dim);
				e->dn[wi] = 0;
				e->as[wi] = j;
				cnt[j] = 1;
				lim--;
			}
		}
	}
	book_index(&bk, t, dim);
	assign(set, n, &bk, e->as, e->dn, 0);
	if (key) {
		memcpy(cb, t, sizeof(t));
		memset(chg, 1, ZV_CB_N);
		for (j = 0; j < ZV_CB_N; j++) age[j] = 0;
		return ZV_CB_N;
	}
	for (j = 0; j < ZV_CB_N; j++) gain[j] = 0;
	memset(cnt, 0, sizeof(cnt));
	for (i = 0; i < n; i++) {
		gain[e->as[i]] += ((double)e->d0[i] - (double)e->dn[i]) * scale;
		cnt[e->as[i]]++;
	}
	for (j = 0; j < ZV_CB_N; j++) {
		if (memcmp(t[j], cb[j], (size_t)dim) && gain[j] > e->lambda * upd_bits) {
			memcpy(cb[j], t[j], (size_t)dim);
			chg[j] = 1;
			nchg++;
		}
		if (cnt[j]) age[j] = 0; else age[j]++;
	}
	return nchg;
}

// ---- each block's best of each kind (in parallel) ------------------------------

static void eval_v1(void *c, int from, int to) {
	zve_t *e = c;
	int b, x, y, W = e->o.w;
	for (b = from; b < to; b++) {
		int bx = b % e->bw, by = b / e->bw, i1;
		const uint8_t *src = e->cur + (by * 8) * W + bx * 8;
		uint32_t d = 0;
		i1 = nearest(e->v1 + b * 16, &e->b1, e->ix[b * 16], &e->vd1[b]);
		for (y = 0; y < 8; y++)
			for (x = 0; x < 8; x++) {
				int t = src[y * W + x] - e->cb1[i1][(y >> 1) * 4 + (x >> 1)];
				d += (uint32_t)(t * t);
			}
		e->i1[b] = (uint8_t)i1;
		e->d1[b] = d;
	}
}

static void eval_v4(void *c, int from, int to) {
	zve_t *e = c;
	int b, q;
	for (b = from; b < to; b++) {
		uint32_t d4 = 0, dq;
		for (q = 0; q < 4; q++) {
			e->i4[b * 4 + q] = (uint8_t)nearest(e->v4 + b * 64 + q * 16, &e->b4, e->ix[b * 16 + q], &dq);
			d4 += dq;
		}
		e->d4[b] = d4;
	}
}

static void eval_v16(void *c, int from, int to) {
	zve_t *e = c;
	int b, q;
	for (b = from; b < to; b++) {
		uint32_t d16 = 0, dq;
		if (!(e->d4[b] > e->lambda * (BITS_V16 - BITS_V4))) { e->d16[b] = 0xffffffffu; continue; }
		for (q = 0; q < 16; q++) {
			e->i16[b * 16 + q] = (uint8_t)nearest(e->v16 + b * 64 + q * 4, &e->b16, e->ix[b * 16 + q], &dq);
			d16 += dq;
		}
		e->d16[b] = d16;
	}
}

size_t zve_frame(zve_t *e, const uint8_t *g, int key, uint8_t *out, size_t cap, zve_stats_t *st) {
	int W = e->o.w, bw = e->bw, nb = e->nb;
	int b, i, n, q, scene = 0, iters;
	uint8_t chg1[ZV_CB_N], chg4[ZV_CB_N], chg16[ZV_CB_N];
	uint8_t *p;
	uint32_t n1 = 0, n4 = 0, n16 = 0;
	size_t len;
	double sse = 0;
	zve_stats_t s;
	memset(&s, 0, sizeof(s));
	double lam_saved;
	int train;                  // train the books from scratch: the first frame only
	if (cap < zve_max_payload(e->o.w, e->o.h)) return 0;
	if (!e->started) key = 1;
	train = !e->started;
	// a seek point is coded coarser (it is large, and the frames after it
	// refine it): smaller spikes for the board's reading
	lam_saved = e->lambda;
	if (key && e->started && e->o.key_lambda_mul > 0) e->lambda *= e->o.key_lambda_mul;
	// A seek point sends the books whole; the picture is then refreshed
	// a band of block rows a frame, so neither the bytes nor the work
	// come in one lump. A player that seeks decodes the band frames
	// quietly before it shows anything.
	if (key) {
		int k = e->o.refresh > 0 ? e->o.refresh : 1;
		e->ref_left = k;
		e->ref_row = 0;
		e->ref_rows = (e->bh + k - 1) / k;
	}
	memset(e->force, 0, (size_t)nb);
	if (e->ref_left > 0) {
		int r0 = e->ref_row, r1 = e->ref_row + e->ref_rows, rr, bxx;
		if (r1 > e->bh) r1 = e->bh;
		for (rr = r0; rr < r1; rr++) for (bxx = 0; bxx < bw; bxx++) e->force[rr * bw + bxx] = 1;
		e->ref_row = r1;
		e->ref_left--;
	}

	// per block: vectors and the cost of leaving it alone
	for (b = 0; b < nb; b++) {
		int bx = b % bw, by = b / bw, x, y;
		const uint8_t *src = g + (by * 8) * W + bx * 8;
		const uint8_t *pr = e->recon + (by * 8) * W + bx * 8;
		uint32_t ds = 0;
		uint8_t *v1 = e->v1 + b * 16, *v4 = e->v4 + b * 64, *v16 = e->v16 + b * 64;
		for (y = 0; y < 8; y++)
			for (x = 0; x < 8; x++) {
				int t = src[y * W + x] - pr[y * W + x];
				ds += (uint32_t)(t * t);
				v4[((y >> 2) * 2 + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3)] = src[y * W + x];
				v16[((y >> 1) * 4 + (x >> 1)) * 4 + (y & 1) * 2 + (x & 1)] = src[y * W + x];
			}
		for (y = 0; y < 4; y++)
			for (x = 0; x < 4; x++) {
				const uint8_t *c = src + (y * 2) * W + x * 2;
				v1[y * 4 + x] = (uint8_t)((c[0] + c[1] + c[W] + c[W + 1] + 2) >> 2);
			}
		e->dskip[b] = ds;
	}
	// a scene change: train harder (it is not a seek point)
	if (!key) {
		int big = 0;
		for (b = 0; b < nb; b++) if (e->dskip[b] > 64u * 400u) big++;
		scene = big * 2 > nb;
	}
	iters = train || scene ? e->o.key_iters : e->o.iters;

	// first frame: seed the books with random vectors of it
	if (!e->started) {
		for (i = 0; i < ZV_CB_N; i++) {
			memcpy(e->cb1[i], e->v1 + (rnd(e) % (uint32_t)nb) * 16, 16);
			memcpy(e->cb4[i], e->v4 + (rnd(e) % (uint32_t)(nb * 4)) * 16, 16);
			memcpy(e->cb16[i], e->v16 + (rnd(e) % (uint32_t)(nb * 16)) * 4, 4);
		}
		e->started = 1;
	}

	// which blocks are worth coding
	for (b = 0; b < nb; b++)
		e->act[b] = train || e->force[b] || e->dskip[b] > e->lambda * BITS_V1;

	// V1 book, on the active blocks
	for (n = 0, b = 0; b < nb; b++) if (e->act[b]) { e->set[n] = e->v1 + b * 16; e->as[n] = e->ix[b * 16]; n++; }
	n1 = (uint32_t)refine(e, e->cb1, 16, e->age1, e->set, n, iters, train, 4.0, UPD_BITS, chg1);
	book_index(&e->b1, e->cb1, 16);
	e->cur = g;
	par_for(nb, eval_v1, e);

	// V4 book, on the active blocks V1 codes badly
	for (n = 0, b = 0; b < nb; b++) {
		if (!e->act[b]) continue;
		if (train || e->force[b] || e->vd1[b] * 4.0 > e->lambda * (BITS_V4 - BITS_V1))
			for (q = 0; q < 4; q++) { e->set[n] = e->v4 + b * 64 + q * 16; e->as[n] = e->ix[b * 16 + q]; n++; }
	}
	n4 = (uint32_t)refine(e, e->cb4, 16, e->age4, e->set, n, iters, train, 1.0, UPD_BITS, chg4);
	book_index(&e->b4, e->cb4, 16);
	par_for(nb, eval_v4, e);

	// V16 book, on the active blocks V4 codes badly
	for (n = 0, b = 0; b < nb; b++) {
		if (!e->act[b]) continue;
		if (train || e->force[b] || e->d4[b] > e->lambda * (BITS_V16 - BITS_V4))
			for (q = 0; q < 16; q++) { e->set[n] = e->v16 + b * 64 + q * 4; e->as[n] = e->ix[b * 16 + q]; n++; }
	}
	n16 = (uint32_t)refine(e, e->cb16, 4, e->age16, e->set, n, iters, train, 1.0, UPD16_BITS, chg16);
	// a seek point sends the books whole, as they are
	if (key) {
		memset(chg1, 1, sizeof(chg1)); memset(chg4, 1, sizeof(chg4)); memset(chg16, 1, sizeof(chg16));
		n1 = n4 = n16 = ZV_CB_N;
	}
	book_index(&e->b16, e->cb16, 4);
	par_for(nb, eval_v16, e);

	// decisions, with the books as the decoder will have them
	{
		int prev1 = -1;
		for (b = 0; b < nb; b++) {
			int best, i1 = e->i1[b];
			double j1, j4, j16, js, jb;
			js = e->force[b] ? 1e300 : e->dskip[b] + e->lambda * BITS_SKIP;
			j1 = e->d1[b] + e->lambda * (i1 == prev1 ? BITS_REP : BITS_V1);
			j4 = e->d4[b] + e->lambda * BITS_V4;
			j16 = e->d16[b] != 0xffffffffu ? e->d16[b] + e->lambda * BITS_V16 : 1e300;
			best = 0; jb = js;
			if (j1 < jb) { best = 1; jb = j1; }
			if (j4 < jb) { best = 2; jb = j4; }
			if (j16 < jb) { best = 3; jb = j16; }
			e->type[b] = (uint8_t)best;
			if (e->o.motion_probe && best != 0) {
				// what a copy from the last picture at an even offset
				// up to 8 would cost instead (not coded: a measurement)
				int bx = b % bw, by = b / bw, dx, dy, x, y;
				double jm = 1e300, rb = best == 1 ? (i1 == prev1 ? BITS_REP : BITS_V1) : best == 2 ? BITS_V4 : BITS_V16;
				for (dy = -8; dy <= 8; dy += 2)
					for (dx = -8; dx <= 8; dx += 2) {
						int sx = bx * 8 + dx, sy = by * 8 + dy;
						uint32_t d = 0;
						if (sx < 0 || sy < 0 || sx + 8 > W || sy + 8 > e->o.h || (!dx && !dy)) continue;
						for (y = 0; y < 8; y++)
							for (x = 0; x < 8; x++) {
								int t = g[(by * 8 + y) * W + bx * 8 + x] - e->recon[(sy + y) * W + sx + x];
								d += (uint32_t)(t * t);
							}
						if (d + e->lambda * 9.5 < jm) jm = d + e->lambda * 9.5;
					}
				if (jm < jb) { e->probe_blocks++; e->probe_bits += rb - 9.5; }
			}
			prev1 = -1;
			if (best == 1) { e->ix[b * 16] = (uint8_t)i1; prev1 = i1; }
			else if (best == 2) for (q = 0; q < 4; q++) e->ix[b * 16 + q] = e->i4[b * 4 + q];
			else if (best == 3) for (q = 0; q < 16; q++) e->ix[b * 16 + q] = e->i16[b * 16 + q];
		}
	}

	// the payload
	p = out;
	p[0] = (uint8_t)n1; p[1] = (uint8_t)(n1 >> 8);
	p[2] = (uint8_t)n4; p[3] = (uint8_t)(n4 >> 8);
	p[4] = (uint8_t)n16; p[5] = (uint8_t)(n16 >> 8);
	p += 6;
	for (i = 0; i < ZV_CB_N; i++) if (chg1[i]) { *p++ = (uint8_t)i; memcpy(p, e->cb1[i], 16); p += 16; }
	for (i = 0; i < ZV_CB_N; i++) if (chg4[i]) { *p++ = (uint8_t)i; memcpy(p, e->cb4[i], 16); p += 16; }
	for (i = 0; i < ZV_CB_N; i++) if (chg16[i]) { *p++ = (uint8_t)i; memcpy(p, e->cb16[i], 4); p += 4; }
	b = 0;
	while (b < nb) {
		int t = e->type[b], r = 1;
		if (t == 0) {
			while (b + r < nb && r < 64 && e->type[b + r] == 0) r++;
			*p++ = (uint8_t)(ZV_CMD_SKIP | (r - 1));
			s.skip += (uint32_t)r;
		} else if (t == 2) {
			while (b + r < nb && r < 64 && e->type[b + r] == 2) r++;
			*p++ = (uint8_t)(ZV_CMD_V4 | (r - 1));
			for (i = 0; i < r; i++) { memcpy(p, e->ix + (b + i) * 16, 4); p += 4; }
			s.v4 += (uint32_t)r;
		} else if (t == 3) {
			while (b + r < nb && r < 32 && e->type[b + r] == 3) r++;
			*p++ = (uint8_t)(ZV_CMD_V16 | (r - 1));
			for (i = 0; i < r; i++) { memcpy(p, e->ix + (b + i) * 16, 16); p += 16; }
			s.v16 += (uint32_t)r;
		} else {
			// a run of one entry -> REP; otherwise V1 until the next run
			while (b + r < nb && r < 32 && e->type[b + r] == 1 && e->ix[(b + r) * 16] == e->ix[b * 16]) r++;
			if (r >= 2) {
				*p++ = (uint8_t)(ZV_CMD_REP | (r - 1));
				*p++ = e->ix[b * 16];
				s.rep_blocks += (uint32_t)r;
			} else {
				while (b + r < nb && r < 64 && e->type[b + r] == 1 &&
				       !(b + r + 1 < nb && e->type[b + r + 1] == 1 && e->ix[(b + r + 1) * 16] == e->ix[(b + r) * 16]))
					r++;
				*p++ = (uint8_t)(ZV_CMD_V1 | (r - 1));
				for (i = 0; i < r; i++) *p++ = e->ix[(b + i) * 16];
			}
			s.v1 += (uint32_t)r;
		}
		b += r;
	}
	len = (size_t)(p - out);

	// the picture every decoder will have
	if (zv_dec_video(&e->dec, out, len, 1, e->recon, W) != 0) return 0;
	for (i = 0; i < W * e->o.h; i++) { int t = g[i] - e->recon[i]; sse += (double)(t * t); }

	s.blocks = (uint32_t)nb;
	s.upd1 = n1;
	s.upd4 = n4;
	s.upd16 = n16;
	s.bytes = (uint32_t)len;
	s.sse = sse;
	s.lambda = e->lambda;
	s.probe_blocks = e->probe_blocks;
	s.probe_bits = e->probe_bits;
	if (st) *st = s;
	e->lambda = lam_saved;
	// what the board's decoder does with it: a block of each kind, an
	// entry drawn again, and the frame's own overhead
	s.cost = ZVE_COST_FRAME + ZVE_COST_V1 * (double)(s.v1 + s.rep_blocks) + ZVE_COST_V4 * (double)s.v4 +
	         ZVE_COST_V16 * (double)s.v16 + ZVE_COST_UPD * (double)(s.upd1 + s.upd4) +
	         ZVE_COST_UPD16 * (double)s.upd16;
	if (st) st->cost = s.cost;

	// rate control, on inter frames
	if (e->o.target_bytes > 0 && !key) {
		double r = (len + 1.0) / e->o.target_bytes;
		if (e->o.target_cost > 0 && s.cost / e->o.target_cost > r) r = s.cost / e->o.target_cost;
		double f = pow(r, 0.35);
		if (f < 0.7) f = 0.7;
		if (f > 1.4) f = 1.4;
		e->lambda *= f;
		if (e->lambda < 2) e->lambda = 2;
		if (e->lambda > 20000) e->lambda = 20000;
	}
	e->frame++;
	return len;
}
