/*
 * Host test for sw/common/zexec_inflate.c, against zlib.
 *
 *   cc -std=gnu99 -Wall -g -fsanitize=address,undefined -I sw/common \
 *      -o /tmp/test_zexec_inflate sw/common/tests/test_zexec_inflate.c \
 *      sw/common/zexec_inflate.c -lz && /tmp/test_zexec_inflate [files...]
 *
 * - every file named on the command line (the core apps' .bin), at
 *   levels 1, 6 and 9, and with zlib's fixed-code and stored strategies;
 * - generated data: runs, text-like, random, empty, one byte, a 70KB
 *   stored block split by zlib, back-references at the full 32KB reach;
 * - fed one byte at a time, and in odd-sized pieces;
 * - malformed streams: truncated at every length, every byte flipped
 *   in turn, random garbage, an output buffer one byte too small and one
 *   too large -- each must fail (or, for a flip zlib also accepts, match
 *   zlib), and ASan watches that nothing is read or written outside.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "zexec_inflate.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const uint8_t *d; uint32_t n, at, piece; } src_t;

static uint32_t fill(void *ctx, uint8_t *buf, uint32_t cap) {
	src_t *s = ctx;
	uint32_t k = s->n - s->at;
	if (k > cap) k = cap;
	if (s->piece && k > s->piece) k = s->piece;
	memcpy(buf, s->d + s->at, k);
	s->at += k;
	return k;
}

static uint8_t *deflate_raw(const uint8_t *in, uint32_t n, int level, int strategy, uint32_t *out_n) {
	z_stream z;
	memset(&z, 0, sizeof(z));
	deflateInit2(&z, level, Z_DEFLATED, -15, 9, strategy);
	uLong cap = deflateBound(&z, n) + 16;
	uint8_t *o = malloc(cap);
	z.next_in = (Bytef *)in; z.avail_in = n;
	z.next_out = o; z.avail_out = (uInt)cap;
	deflate(&z, Z_FINISH);
	*out_n = (uint32_t)z.total_out;
	deflateEnd(&z);
	return o;
}

// zlib's own answer for a (possibly broken) stream: bytes out, or -1.
static long zlib_inflate(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap) {
	z_stream z;
	memset(&z, 0, sizeof(z));
	inflateInit2(&z, -15);
	z.next_in = (Bytef *)in; z.avail_in = n;
	z.next_out = out; z.avail_out = cap;
	int r = inflate(&z, Z_FINISH);
	long got = (long)z.total_out;
	inflateEnd(&z);
	return r == Z_STREAM_END ? got : -1;
}

static z_exec_inflate_t st;

static int ours(const uint8_t *c, uint32_t cn, uint8_t *out, uint32_t out_len, uint32_t piece) {
	src_t s = { c, cn, 0, piece };
	return z_exec_inflate(&st, out, out_len, fill, &s);
}

static void round_trip(const char *what, const uint8_t *d, uint32_t n) {
	static const int lv[] = { 1, 6, 9 };
	static const int strat[] = { Z_DEFAULT_STRATEGY, Z_FIXED, Z_HUFFMAN_ONLY, Z_RLE };
	for (int si = 0; si < 4; si++)
	for (int li = 0; li < 3; li++) {
		uint32_t cn;
		uint8_t *c = deflate_raw(d, n, lv[li], strat[si], &cn);
		uint8_t *out = malloc(n + 1);
		int r = ours(c, cn, out, n, 0);
		CHECK(r == 0 && !memcmp(out, d, n), "%s level %d strategy %d", what, lv[li], si);
		if (li == 2 && si == 0) {
			r = ours(c, cn, out, n, 1);
			CHECK(r == 0 && !memcmp(out, d, n), "%s byte by byte", what);
			r = ours(c, cn, out, n, 77);
			CHECK(r == 0 && !memcmp(out, d, n), "%s in 77-byte pieces", what);
			if (n) {
				uint8_t *small = malloc(n - 1 + 1);
				CHECK(ours(c, cn, small, n - 1, 0) != 0, "%s: one byte too little room fails", what);
				free(small);
			}
			CHECK(ours(c, cn, out, n + 1, 0) != 0, "%s: a stream ending short of out_len fails", what);
		}
		free(c);
		free(out);
	}
	// level 0: stored blocks
	uint32_t cn;
	uint8_t *c = deflate_raw(d, n, 0, Z_DEFAULT_STRATEGY, &cn);
	uint8_t *out = malloc(n + 1);
	CHECK(ours(c, cn, out, n, 0) == 0 && !memcmp(out, d, n), "%s stored", what);
	free(c);
	free(out);
}

static void malformed(const char *what, const uint8_t *d, uint32_t n) {
	uint32_t cn;
	uint8_t *c = deflate_raw(d, n, 9, Z_DEFAULT_STRATEGY, &cn);
	uint8_t *out = malloc(n), *ref = malloc(n);
	int bad = 0;
	for (uint32_t k = 0; k < cn; k++) {			// truncated at every length
		uint8_t *t = malloc(k + 1);
		memcpy(t, c, k);
		if (ours(t, k, out, n, 0) == 0) bad++;
		free(t);
	}
	CHECK(!bad, "%s: %d truncations accepted", what, bad);
	bad = 0;
	uint32_t step = cn > 4000 ? cn / 4000 : 1;
	for (uint32_t k = 0; k < cn; k += step) {	// a flipped byte: ours agrees with zlib, or fails
		uint8_t *t = malloc(cn);
		memcpy(t, c, cn);
		t[k] ^= (uint8_t)(0x5a + k);
		int r = ours(t, cn, out, n, 0);
		long z = zlib_inflate(t, cn, ref, n);
		if (r == 0 && (z != (long)n || memcmp(out, ref, n))) bad++;
		free(t);
	}
	CHECK(!bad, "%s: %d corrupted streams accepted that zlib disagrees with", what, bad);
	free(c);
	free(out);
	free(ref);
}

static uint8_t *slurp(const char *p, uint32_t *n) {
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(sz ? sz : 1);
	*n = (uint32_t)fread(b, 1, sz, f);
	fclose(f);
	return b;
}

int main(int argc, char **argv) {
	uint32_t n = 70000;
	uint8_t *d = malloc(n);
	srand(1);

	round_trip("empty", d, 0);
	d[0] = 'x';
	round_trip("one byte", d, 1);
	memset(d, 'a', n);
	round_trip("one long run", d, n);
	for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)rand();
	round_trip("random", d, n);
	for (uint32_t i = 0; i < n; i++) d[i] = "the quick brown fox jumps over "[(i * 7 + i / 31) % 31];
	round_trip("text", d, n);
	for (uint32_t i = 0; i < 32768; i++) d[i] = (uint8_t)rand();
	for (uint32_t i = 32768; i < n; i++) d[i] = d[i - 32768];	// matches at the full reach
	round_trip("32KB back-references", d, n);

	for (int a = 1; a < argc; a++) {
		uint32_t fn;
		uint8_t *f = slurp(argv[a], &fn);
		if (!f) { printf("cannot read %s\n", argv[a]); failures++; continue; }
		round_trip(argv[a], f, fn);
		if (a == 1) malformed(argv[a], f, fn < 20000 ? fn : 20000);
		free(f);
	}

	for (uint32_t i = 0; i < n; i++) d[i] = "the quick brown fox jumps over "[(i * 7 + i / 31) % 31];
	malformed("text", d, 6000);

	int bad = 0;
	uint8_t *out = malloc(65536);
	for (int t = 0; t < 20000; t++) {			// garbage
		uint8_t g[200];
		uint32_t gn = 1 + (uint32_t)rand() % sizeof(g);
		for (uint32_t i = 0; i < gn; i++) g[i] = (uint8_t)rand();
		uint32_t on = (uint32_t)rand() % 65536;
		if (ours(g, gn, out, on, (uint32_t)rand() % 3) == 0) {
			long z = zlib_inflate(g, gn, d, n);
			if (z != (long)on) bad++;
		}
	}
	CHECK(!bad, "%d garbage streams accepted that zlib rejects", bad);

	free(out);
	free(d);
	printf("test_zexec_inflate: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
