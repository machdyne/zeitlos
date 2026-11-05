/*
 * cputest -- the CPU's arithmetic against slow references.
 *
 *   run cputest [seconds]        (default 20)
 *
 * Every M and the fixed-point products zgl builds on ((int64)a * b >> 16,
 * a 64-bit divide), on operands from a generator that never repeats,
 * the results fed at once into the next operation, a store, a load and
 * a branch -- the forwarding and the redirects a pipeline gets wrong if
 * anything. Each is held to a reference made of adds, shifts and
 * compares only. Mismatches are printed (the first 20 in full) with the
 * operation and operands; the totals every two seconds. On the UART
 * (0xf0000000) as everything else's printf.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"

#define NOINLINE __attribute__((noinline))

static uint32_t rs = 0x12345678u;
static inline uint32_t rnd(void) {
	/* xorshift32: adds, shifts and xors only */
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return rs;
}

/* operands that matter: small, large, negative, powers of two, edges */
static uint32_t operand(void) {
	uint32_t r = rnd();
	switch (r & 7u) {
	case 0: return rnd() & 0xffu;
	case 1: return (uint32_t)-(int32_t)(rnd() & 0xffffu);
	case 2: return 1u << (rnd() & 31u);
	case 3: return 0x80000000u;
	case 4: return 0xffffffffu;
	case 5: return rnd() >> (rnd() & 31u);
	default: return rnd();
	}
}

/* -- references: no M instructions -- */

static NOINLINE uint64_t ref_mulu64(uint32_t a, uint32_t b) {
	uint64_t r = 0, x = a;
	int i;
	for (i = 0; i < 32; i++) {
		if (b & 1u) r += x;
		x <<= 1;
		b >>= 1;
	}
	return r;
}

static NOINLINE int64_t ref_muls64(int32_t a, int32_t b) {
	int neg = 0;
	uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
	uint64_t p;
	if (a < 0) { ua = (uint32_t)-(int64_t)a; neg ^= 1; }
	if (b < 0) { ub = (uint32_t)-(int64_t)b; neg ^= 1; }
	p = ref_mulu64(ua, ub);
	return neg ? -(int64_t)p : (int64_t)p;
}

static NOINLINE int64_t ref_mulsu64(int32_t a, uint32_t b) {
	uint32_t ua = (uint32_t)a;
	uint64_t p;
	if (a < 0) {
		ua = (uint32_t)-(int64_t)a;
		p = ref_mulu64(ua, b);
		return -(int64_t)p;
	}
	return (int64_t)ref_mulu64(ua, b);
}

static NOINLINE uint32_t ref_divu(uint32_t a, uint32_t b, uint32_t *rem) {
	uint32_t q = 0, r = 0;
	int i;
	if (b == 0) { *rem = a; return 0xffffffffu; }
	for (i = 31; i >= 0; i--) {
		r = (r << 1) | ((a >> i) & 1u);
		if (r >= b) { r -= b; q |= 1u << i; }
	}
	*rem = r;
	return q;
}

static NOINLINE int32_t ref_div(int32_t a, int32_t b, int32_t *rem) {
	uint32_t ua, ub, q, r;
	if (b == 0) { *rem = a; return -1; }
	if (a == (int32_t)0x80000000 && b == -1) { *rem = 0; return a; }
	ua = a < 0 ? (uint32_t)-(int64_t)a : (uint32_t)a;
	ub = b < 0 ? (uint32_t)-(int64_t)b : (uint32_t)b;
	q = ref_divu(ua, ub, &r);
	*rem = a < 0 ? -(int32_t)r : (int32_t)r;
	return ((a < 0) != (b < 0)) ? -(int32_t)q : (int32_t)q;
}

/* -- the instructions themselves -- */

static inline uint32_t i_mul(uint32_t a, uint32_t b)    { uint32_t r; __asm__ volatile ("mul %0,%1,%2"    : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_mulh(uint32_t a, uint32_t b)   { uint32_t r; __asm__ volatile ("mulh %0,%1,%2"   : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_mulhsu(uint32_t a, uint32_t b) { uint32_t r; __asm__ volatile ("mulhsu %0,%1,%2" : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_mulhu(uint32_t a, uint32_t b)  { uint32_t r; __asm__ volatile ("mulhu %0,%1,%2"  : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_div(uint32_t a, uint32_t b)    { uint32_t r; __asm__ volatile ("div %0,%1,%2"    : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_divu(uint32_t a, uint32_t b)   { uint32_t r; __asm__ volatile ("divu %0,%1,%2"   : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_rem(uint32_t a, uint32_t b)    { uint32_t r; __asm__ volatile ("rem %0,%1,%2"    : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_remu(uint32_t a, uint32_t b)   { uint32_t r; __asm__ volatile ("remu %0,%1,%2"   : "=r"(r) : "r"(a), "r"(b)); return r; }

/* a result straight into a store, a load back and a branch on it */
static volatile uint32_t ring[64];
static unsigned rp;

static uint32_t through_memory(uint32_t v) {
	uint32_t got;
	ring[rp & 63u] = v;
	got = ring[rp & 63u];
	rp++;
	return got;
}

enum { O_MUL, O_MULH, O_MULHSU, O_MULHU, O_DIV, O_DIVU, O_REM, O_REMU, O_FIX, O_FIXDIV, O_CHAIN, O_N };
static const char *const oname[O_N] = {
	"mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu",
	"(int64)a*b>>16", "((int64)a<<16)/b", "chain"
};
static uint32_t ops[O_N], bad[O_N];
static unsigned printed;

static void check(int op, uint32_t a, uint32_t b, uint32_t got, uint32_t want) {
	ops[op]++;
	/* the compare is a branch on the result, as the pipeline sees it */
	if (got != want) {
		bad[op]++;
		if (printed < 20) {
			printed++;
			printf("cputest: MISMATCH %s a=%08lx b=%08lx: %08lx, want %08lx\n", oname[op],
				(unsigned long)a, (unsigned long)b, (unsigned long)got, (unsigned long)want);
		}
	}
}

static void one_round(void) {
	uint32_t a = operand(), b = operand(), r;
	int32_t sr;
	uint32_t ur;
	uint64_t p = ref_mulu64(a, b);
	int64_t ps = ref_muls64((int32_t)a, (int32_t)b);
	int64_t psu = ref_mulsu64((int32_t)a, b);

	check(O_MUL, a, b, through_memory(i_mul(a, b)), (uint32_t)p);
	check(O_MULH, a, b, through_memory(i_mulh(a, b)), (uint32_t)((uint64_t)ps >> 32));
	check(O_MULHSU, a, b, through_memory(i_mulhsu(a, b)), (uint32_t)((uint64_t)psu >> 32));
	check(O_MULHU, a, b, through_memory(i_mulhu(a, b)), (uint32_t)(p >> 32));
	{
		int32_t q = ref_div((int32_t)a, (int32_t)b, &sr);
		uint32_t uq = ref_divu(a, b, &ur);
		check(O_DIV, a, b, through_memory(i_div(a, b)), (uint32_t)q);
		check(O_DIVU, a, b, through_memory(i_divu(a, b)), uq);
		check(O_REM, a, b, through_memory(i_rem(a, b)), (uint32_t)sr);
		check(O_REMU, a, b, through_memory(i_remu(a, b)), ur);
	}
	/* zgl's fixed point, as the compiler writes it */
	{
		volatile int32_t va = (int32_t)a, vb = (int32_t)b;
		int32_t fx = (int32_t)(((int64_t)va * vb) >> 16);
		check(O_FIX, a, b, (uint32_t)fx, (uint32_t)(int32_t)((uint64_t)ps >> 16));
		if (b != 0 && (b & 0xffff0000u) && b != 0xffffffffu) {
			int64_t num = (int64_t)va * 65536;
			int64_t q64 = num / vb;          /* __divdi3: div, mul, branches */
			/* the reference: shifts and subtracts on 64 bits */
			uint64_t un = num < 0 ? (uint64_t)-num : (uint64_t)num;
			uint64_t ud = vb < 0 ? (uint64_t)-(int64_t)vb : (uint64_t)vb;
			uint64_t q = 0, rr = 0;
			int i;
			for (i = 63; i >= 0; i--) {
				rr = (rr << 1) | ((un >> i) & 1u);
				if (rr >= ud) { rr -= ud; q |= (uint64_t)1 << i; }
			}
			if ((num < 0) != (vb < 0)) q = (uint64_t)-(int64_t)q;
			check(O_FIXDIV, a, b, (uint32_t)q64, (uint32_t)q);
		}
	}
	/* a dependent chain: each result the next's operand, at once */
	r = a;
	r = i_mul(r, b | 1u);
	r = i_mulhu(r, a) + r;
	r = i_mul(r, r) ^ b;
	r = i_divu(r, (b >> 3) | 1u) + i_remu(r, (a >> 5) | 1u);
	{
		uint32_t x = a, t1, t2, t3, rq, rm;
		t1 = (uint32_t)ref_mulu64(x, b | 1u);
		t2 = (uint32_t)(ref_mulu64(t1, a) >> 32) + t1;
		t3 = (uint32_t)ref_mulu64(t2, t2) ^ b;
		rq = ref_divu(t3, (b >> 3) | 1u, &rm);
		(void)rm;
		ref_divu(t3, (a >> 5) | 1u, &rm);
		check(O_CHAIN, a, b, through_memory(r), rq + rm);
	}
}

int main(int argc, char **argv) {
	uint32_t secs = argc > 1 ? (uint32_t)atoi(argv[1]) : 20u;
	uint32_t t0 = z_uptime_ticks(), last = t0, rounds = 0;
	int i;
	printf("cputest: %lu seconds of M instructions and fixed point against references\n",
		(unsigned long)secs);
	while (z_uptime_ticks() - t0 < secs * Z_TICK_HZ) {
		one_round();
		rounds++;
		if (z_uptime_ticks() - last >= 2u * Z_TICK_HZ) {
			uint32_t n = 0, nb = 0;
			last = z_uptime_ticks();
			for (i = 0; i < O_N; i++) { n += ops[i]; nb += bad[i]; }
			printf("cputest: %lu rounds, %lu checks, %lu wrong\n",
				(unsigned long)rounds, (unsigned long)n, (unsigned long)nb);
		}
	}
	{
		uint32_t nb = 0;
		for (i = 0; i < O_N; i++) {
			printf("cputest:   %-18s %8lu checks, %lu wrong\n", oname[i],
				(unsigned long)ops[i], (unsigned long)bad[i]);
			nb += bad[i];
		}
		printf("cputest: %s\n", nb ? "FAIL" : "PASS");
	}
	return 0;
}
