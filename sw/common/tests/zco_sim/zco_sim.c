/*
 * zco on the RISC-V CPU, under sim/zsim-headless: the register switch
 * itself. A coroutine keeps its locals (in registers and on its stack)
 * across hundreds of yields, nested calls yield from deep down, and a
 * JPEG decoded inside a coroutine, handed its input a chunk at a time,
 * is the JPEG decoded in one call. Prints "zco: ok" and exits 0.
 *
 *   make -C sw/common/tests/zco_sim
 *   sim/zsim-headless -n 0 sw/common/tests/zco_sim/zco_sim.bin
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../zco.h"
#include "../../zimg.h"

extern const uint8_t jpg[];
extern const uint32_t jpg_len;

static int fails;
#define ck(c, m) do { if (!(c)) { fails++; printf("FAIL %s\n", m); } } while (0)

static uint8_t stack1[8192], stack2[16384];

// -- locals across yields --
static volatile uint32_t seen;
static uint32_t deep(uint32_t n, uint32_t acc) {
	uint32_t a = n * 3 + 1, b = acc ^ 0x5a5a5a5a;
	if (!n) { zco_yield(); return acc; }
	zco_yield();
	return deep(n - 1, acc + a) + (b & 1) + a;
}
static void counter(void *arg) {
	uint32_t i, s = 0, *out = arg;
	for (i = 0; i < 300; i++) {
		s += i * i;
		seen = i;
		zco_yield();
	}
	*out = s + deep(20, 7);
}

// -- a JPEG through a coroutine --
typedef struct { const uint8_t *p; uint32_t n, pos, avail; uint32_t sum; int rows; z_img_t *im; } dec_t;
static int rd(void *c, uint8_t *b, int len) {
	dec_t *d = c;
	int n = 0;
	while (n < len) {
		uint32_t k;
		if (d->pos == d->avail) {
			if (d->avail == d->n) break;
			zco_yield();		// the "main loop" hands over more
			continue;
		}
		k = d->avail - d->pos;
		if (k > (uint32_t)(len - n)) k = (uint32_t)(len - n);
		memcpy(b + n, d->p + d->pos, k);
		d->pos += k;
		n += (int)k;
	}
	return n;
}
static void row(void *u, const uint8_t *g, int y, int w) {
	dec_t *d = u;
	int x;
	for (x = 0; x < w; x++) d->sum = d->sum * 31 + g[x] + (uint32_t)y;
	if (++d->rows % 3 == 0) zco_yield();
}
static void decode(void *arg) {
	dec_t *d = arg;
	z_img_decode_gray(d->im, z_img_sniff(d->p, 64), row, d);
}
static uint32_t decode_direct(void) {
	static z_img_t im;
	dec_t d;
	memset(&d, 0, sizeof(d));
	memset(&im, 0, sizeof(im));
	d.p = jpg; d.n = d.avail = jpg_len; d.im = &im;
	im.read = rd; im.ctx = &d; im.doc_w = Z_IMG_MAX_W; im.doc_h = Z_IMG_MAX_H;
	// outside a coroutine zco_yield() returns at once
	z_img_decode_gray(&im, z_img_sniff(jpg, 64), row, &d);
	return d.sum;
}

// -- running off the stack --
// A chain of calls deeper than its stack. The stack sits at the top of
// a larger buffer, so what it overwrites below is ours.
static uint8_t region[8192];
static volatile uint32_t sink;
static uint32_t burn(int n) {
	volatile uint8_t pad[96];
	for (int i = 0; i < 96; i++) pad[i] = (uint8_t)(n + i);	// every byte, as real locals are used
	if (n == 0) { zco_yield(); return pad[0]; }
	return burn(n - 1) + pad[95];
}
static void hog(void *arg) { (void)arg; sink = burn(40); zco_yield(); sink++; }

int main(void) {
	zco_t co;
	uint32_t out = 0, expect, resumes = 0;
	uint32_t i, s = 0;
	for (i = 0; i < 300; i++) s += i * i;
	{
		// what counter() computes, without yields
		uint32_t (*f)(uint32_t, uint32_t) = deep;
		expect = s + f(20, 7);
	}
	zco_init(&co, counter, &out, stack1, sizeof(stack1));
	while (zco_resume(&co)) {
		resumes++;
		ck(zco_current() == NULL, "the main loop is not in a coroutine");
	}
	ck(out == expect, "locals kept across yields, deep calls too");
	ck(resumes == 300 + 21, "every yield came back here");
	printf("zco: %lu resumes, sum %lu\n", (unsigned long)resumes, (unsigned long)out);

	{
		static z_img_t im;
		dec_t d;
		uint32_t whole = decode_direct(), turns = 0;
		memset(&d, 0, sizeof(d));
		memset(&im, 0, sizeof(im));
		d.p = jpg; d.n = jpg_len; d.avail = 0; d.im = &im;
		im.read = rd; im.ctx = &d; im.doc_w = Z_IMG_MAX_W; im.doc_h = Z_IMG_MAX_H;
		zco_init(&co, decode, &d, stack2, sizeof(stack2));
		while (zco_resume(&co)) {
			turns++;
			if (d.pos == d.avail && d.avail < d.n) d.avail = d.avail + 1024 > d.n ? d.n : d.avail + 1024;
		}
		ck(d.sum == whole && whole != 0, "a JPEG decoded in slices == decoded whole");
		printf("zco: the JPEG in %lu slices, %dx%d\n", (unsigned long)turns, im.out_w, im.out_h);
	}
	printf("zco: the JPEG used %lu of %lu stack bytes\n",
		(unsigned long)zco_stack_used(&co), (unsigned long)sizeof(stack2));
	ck(!co.overflow && zco_stack_used(&co) > 0 && zco_stack_used(&co) < sizeof(stack2),
		"the high-water mark is believable");

	{
		uint32_t n = 0;
		zco_init(&co, hog, NULL, region + 6144, 2048);	// ~5KB of calls, 2KB of stack
		while (zco_resume(&co) && n < 100) n++;
		ck(co.overflow && co.done, "running off the stack is caught");
		ck(!zco_resume(&co), "and it is never resumed again");
		ck(sink == 0, "it did not get past the yield it overflowed at");
		printf("zco: overflow caught after %lu resumes, used %lu, sink %lu\n", (unsigned long)n, (unsigned long)zco_stack_used(&co), (unsigned long)sink);
	}

	printf(fails ? "zco: FAILED\n" : "zco: ok\n");
	return fails ? 1 : 0;
}
