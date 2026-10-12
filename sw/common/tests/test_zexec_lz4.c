/*
 * Host test for sw/common/zexec_lz4.c.
 *
 *   cc -std=gnu99 -Wall -g -fsanitize=address,undefined -I sw/common \
 *      -o /tmp/test_zexec_lz4 sw/common/tests/test_zexec_lz4.c \
 *      sw/common/zexec_lz4.c && /tmp/test_zexec_lz4 [files...]
 *
 * The blocks come from a small greedy LZ4 compressor here (the format's
 * end rules kept, as tools/mkzar.py keeps them); test_zar unpacks what
 * mkzar.py itself writes. Checked:
 *
 * - every file named on the command line (the core apps' .bin), and
 *   generated data: empty, one byte, runs (offset 1, overlapping
 *   copies), random (all literals, long literal runs), text, matches at
 *   the full 64KB reach, lengths needing several 255 bytes;
 * - input handed over a byte at a time, in odd pieces, and whole;
 * - out_len one short and one long; truncated at every length; every
 *   byte changed in turn; random garbage -- each must fail or, when it
 *   happens to be a valid block, stay inside its buffers (ASan watches).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zexec_lz4.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// -- a greedy LZ4 block compressor: one hash slot per 4-byte key --

static void put_len(uint8_t *o, uint32_t *k, uint32_t r) {
	while (r >= 255) { o[(*k)++] = 255; r -= 255; }
	o[(*k)++] = (uint8_t)r;
}

static void emit(uint8_t *o, uint32_t *k, const uint8_t *lit, uint32_t ll, uint32_t ml, uint32_t off) {
	o[(*k)++] = (uint8_t)(((ll < 15 ? ll : 15) << 4) | (ml ? (ml - 4 < 15 ? ml - 4 : 15) : 0));
	if (ll >= 15) put_len(o, k, ll - 15);
	memcpy(o + *k, lit, ll);
	*k += ll;
	if (ml) {
		o[(*k)++] = (uint8_t)off;
		o[(*k)++] = (uint8_t)(off >> 8);
		if (ml >= 19) put_len(o, k, ml - 19);
	}
}

static uint32_t compress(const uint8_t *s, uint32_t n, uint8_t *o) {
	static int32_t table[1 << 16];
	uint32_t k = 0, i = 0, anchor = 0;
	for (int t = 0; t < (1 << 16); t++) table[t] = -1;
	while (n >= 13 && i + 12 <= n) {
		uint32_t key = (uint32_t)s[i] | s[i + 1] << 8 | s[i + 2] << 16 | (uint32_t)s[i + 3] << 24;
		uint32_t h = (key * 2654435761u) >> 16;
		int32_t j = table[h];
		table[h] = (int32_t)i;
		if (j >= 0 && i - (uint32_t)j <= 65535 && !memcmp(s + j, s + i, 4)) {
			uint32_t l = 4;
			while (i + l < n - 5 && s[j + l] == s[i + l]) l++;
			emit(o, &k, s + anchor, i - anchor, l, i - (uint32_t)j);
			i += l;
			anchor = i;
		} else {
			i++;
		}
	}
	emit(o, &k, s + anchor, n - anchor, 0, 0);
	return k;
}

// -- feeding it --

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

static z_exec_lz4_t st;

static int unpack(const uint8_t *c, uint32_t cn, uint8_t *out, uint32_t out_len, uint32_t piece) {
	src_t s = { c, cn, 0, piece };
	return z_exec_lz4(&st, out, out_len, fill, &s);
}

static void round_trip(const char *what, const uint8_t *d, uint32_t n) {
	uint8_t *c = malloc(n + n / 255 + 32), *out = malloc(n + 2);
	uint32_t cn = compress(d, n, c);
	static const uint32_t pieces[] = { 0, 1, 7, 300, 4096 };
	for (int i = 0; i < 5; i++) {
		memset(out, 0xAA, n + 2);
		int r = unpack(c, cn, out, n, pieces[i]);
		CHECK(r == 0 && !memcmp(out, d, n), "%s, pieces of %u", what, pieces[i]);
		CHECK(out[n] == 0xAA, "%s: nothing written past the image", what);
	}
	if (n) CHECK(unpack(c, cn, out, n - 1, 0) != 0, "%s: one byte too little room fails", what);
	CHECK(unpack(c, cn, out, n + 1, 0) != 0, "%s: a block short of out_len fails", what);
	free(c);
	free(out);
}

static void malformed(const char *what, const uint8_t *d, uint32_t n) {
	uint8_t *c = malloc(n + n / 255 + 32), *out = malloc(n);
	uint32_t cn = compress(d, n, c);
	int bad = 0;
	for (uint32_t k = 0; k < cn; k++)			// truncated at every length
		if (unpack(c, k, out, n, 0) == 0) bad++;
	CHECK(!bad, "%s: %d truncations accepted", what, bad);
	uint32_t step = cn > 3000 ? cn / 3000 : 1;
	for (uint32_t k = 0; k < cn; k += step) {	// changed bytes: inside the buffers (ASan), whatever comes out
		uint8_t *t = malloc(cn);
		memcpy(t, c, cn);
		t[k] ^= (uint8_t)(0x5a + k);
		unpack(t, cn, out, n, (uint32_t)k % 5);
		free(t);
	}
	free(c);
	free(out);
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
	uint32_t n = 140000;
	uint8_t *d = malloc(n);
	srand(1);

	round_trip("empty", d, 0);
	d[0] = 'x';
	round_trip("one byte", d, 1);
	memset(d, 'a', n);
	round_trip("one long run (offset 1, lengths of many 255s)", d, n);
	for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)"ab"[i & 1];
	round_trip("offset 2, overlapping", d, n);
	for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)rand();
	round_trip("random (literals only)", d, n);
	for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)"the quick brown fox jumps over "[(i * 7 + i / 31) % 31];
	round_trip("text", d, n);
	for (uint32_t i = 0; i < 65535; i++) d[i] = (uint8_t)rand();
	for (uint32_t i = 65535; i < n; i++) d[i] = d[i - 65535];
	round_trip("matches at the full 64KB reach", d, n);
	for (uint32_t i = 0; i < 20; i++) d[i] = (uint8_t)rand();
	round_trip("20 bytes", d, 20);

	for (int a = 1; a < argc; a++) {
		uint32_t fn;
		uint8_t *f = slurp(argv[a], &fn);
		if (!f) { printf("cannot read %s\n", argv[a]); failures++; continue; }
		round_trip(argv[a], f, fn);
		if (a == 1) malformed(argv[a], f, fn < 30000 ? fn : 30000);
		free(f);
	}
	for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)"the quick brown fox jumps over "[(i * 7 + i / 31) % 31];
	malformed("text", d, 8000);

	uint8_t *out = malloc(65536);
	for (int t = 0; t < 50000; t++) {			// garbage: inside the buffers, whatever comes out
		uint8_t g[300];
		uint32_t gn = 1 + (uint32_t)rand() % sizeof(g);
		for (uint32_t i = 0; i < gn; i++) g[i] = (uint8_t)rand();
		unpack(g, gn, out, (uint32_t)rand() % 65536, (uint32_t)rand() % 3);
	}
	CHECK(1, "garbage");

	free(out);
	free(d);
	printf("test_zexec_lz4: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
