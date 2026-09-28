/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * X25519 and Ed25519 checks on montmul's register file, or Monocypher.
 * docs/z25519.md; the interface in z25519.h.
 *
 * -- the arithmetic --
 *
 * Field elements of 2^255-19 live in the block's registers in
 * Montgomery form, R = 2^(32 * the block's width): 2^384 on a 12-limb
 * block. A MUL is a Montgomery product; ADD and SUB are modulo p. Every
 * command takes the same time whatever its operands (rtl/montmul.v), so
 * the X25519 ladder is constant time in the scalar: its only secret-
 * dependent step, the conditional swap, swaps REGISTER NUMBERS, without
 * a branch.
 *
 * -- the rules --
 *
 * z_ed25519_check() must give Monocypher's verdict on every input --
 * zfed nodes with the block and without it must agree on what is valid.
 * So it follows crypto_eddsa_check_equation() exactly: A and R are
 * decoded negated, non-canonical y (y >= p) accepted and reduced, a
 * point that does not decode rejected; s must be below L; and the check
 * is COFACTORED: [8]([s]B - [h]A - R) must be the identity. The tests
 * compare the two on RFC 8032's vectors, random signatures, and every
 * edge case gen_ed25519_edge.py makes (small-order points, non-
 * canonical encodings, s = L, s >= L).
 *
 * -- the block --
 *
 * Claimed around each call (docs/montmul.md, "Sharing"), with its
 * modulus loaded after the claim; if another process holds it, the
 * call is Monocypher's. Before releasing, every register and the
 * classic operand registers are overwritten with zeros: X25519's
 * intermediates are derived from a secret scalar, and the next owner
 * could read them.
 */
#include <string.h>

#include "z25519.h"
#include "../ext/monocypher/monocypher.h"
#include "../ext/monocypher/monocypher-ed25519.h"

int z25519_last_hw;
int z25519_no_hw;

// -- the block's registers ------------------------------------------

#if defined(Z25519_HW_TEST)
#define Z25519_HW 1
uint32_t z25519_mm_rd(uint32_t word);
void z25519_mm_wr(uint32_t word, uint32_t v);
uint32_t z25519_owner_id(void);
static int mm_present(void) { return 1; }
#define mm_rd z25519_mm_rd
#define mm_wr z25519_mm_wr
#define owner_id z25519_owner_id
#elif defined(__riscv) && !defined(Z25519_NO_HW)
#define Z25519_HW 1
#include "zsoc.h"
#include "zeitlos.h"
static inline uint32_t mm_rd(uint32_t w) {
	return *(volatile uint32_t *)(uintptr_t)(Z_MONTMUL_BASE + 4u * w);
}
static inline void mm_wr(uint32_t w, uint32_t v) {
	*(volatile uint32_t *)(uintptr_t)(Z_MONTMUL_BASE + 4u * w) = v;
}
static int mm_present(void) {
	return z_soc_has_feature2(Z_FEATURE2_MONTMUL) && mm_rd(0) == 0x5A4D4F4Eu;
}
static uint32_t owner_id(void) {
	static uint32_t pid;
	if (!pid) pid = z_getpid() & 0x7FFFFFFFu;
	return pid ? pid : 0x7FFFFFFEu;
}
#endif

#ifdef Z25519_HW

#define W_CONFIG 2u
#define W_N0INV  3u
#define W_OWNER  4u
#define W_RSEL   5u
#define W_RDATA  6u
#define W_CMD    7u
#define W_A      16u
#define W_B      28u
#define W_N      40u
#define RELEASE  0x80000000u
#define OP_MUL   1u
#define OP_ADD   2u
#define OP_SUB   3u
#define NREGS    16

typedef uint32_t fe8[8];		// a value below 2^256, little-endian limbs

static const fe8 k_p      = { 0xffffffedu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0x7fffffffu };
static const fe8 k_d      = { 0x135978a3u, 0x75eb4dcau, 0x4141d8abu, 0x00700a4du, 0x7779e898u, 0x8cc74079u, 0x2b6ffe73u, 0x52036ceeu };
static const fe8 k_d2     = { 0x26b2f159u, 0xebd69b94u, 0x8283b156u, 0x00e0149au, 0xeef3d130u, 0x198e80f2u, 0x56dffce7u, 0x2406d9dcu };
static const fe8 k_sqrtm1 = { 0x4a0ea0b0u, 0xc4ee1b27u, 0xad2fe478u, 0x2f431806u, 0x3dfbd7a7u, 0x2b4d0099u, 0x4fc1df0bu, 0x2b832480u };
static const fe8 k_bx     = { 0x8f25d51au, 0xc9562d60u, 0x9525a7b2u, 0x692cc760u, 0xfdd6dc5cu, 0xc0a4e231u, 0xcd6e53feu, 0x216936d3u };
static const fe8 k_by     = { 0x66666658u, 0x66666666u, 0x66666666u, 0x66666666u, 0x66666666u, 0x66666666u, 0x66666666u, 0x66666666u };
static const fe8 k_L      = { 0x5cf5d3edu, 0x5812631au, 0xa2f79cd6u, 0x14def9deu, 0x00000000u, 0x00000000u, 0x00000000u, 0x10000000u };

static int hw_state = -1;		// -1 not asked, 0 unusable, 1 usable
static uint32_t hw_w;			// the block's width, limbs
static uint32_t hw_n0inv;
static fe8 hw_r2;				// R^2 mod p, R = 2^(32 hw_w)
static uint16_t used;			// registers in use

static int ge8(const uint32_t *a, const uint32_t *b) {
	for (int i = 7; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i];
	return 1;
}
static void sub8(uint32_t *a, const uint32_t *b) {
	uint64_t borrow = 0;
	for (int i = 0; i < 8; i++) {
		uint64_t t = (uint64_t)a[i] - b[i] - borrow;
		a[i] = (uint32_t)t; borrow = (t >> 63) & 1;
	}
}
static int eq8(const uint32_t *a, const uint32_t *b) {
	uint32_t x = 0;
	for (int i = 0; i < 8; i++) x |= a[i] ^ b[i];
	return x == 0;
}
static void from_le(uint32_t *v, const uint8_t *b) {
	for (int i = 0; i < 8; i++)
		v[i] = (uint32_t)b[4*i] | ((uint32_t)b[4*i+1] << 8) | ((uint32_t)b[4*i+2] << 16) | ((uint32_t)b[4*i+3] << 24);
}
static void to_le(uint8_t *b, const uint32_t *v) {
	for (int i = 0; i < 8; i++) {
		b[4*i] = (uint8_t)v[i]; b[4*i+1] = (uint8_t)(v[i] >> 8);
		b[4*i+2] = (uint8_t)(v[i] >> 16); b[4*i+3] = (uint8_t)(v[i] >> 24);
	}
}
// A field element from 32 bytes: the top bit dropped, then below p --
// y >= p (non-canonical) is accepted and reduced, as RFC 7748 asks of
// X25519 and as Monocypher does for Ed25519.
static void fe_bytes(uint32_t *v, const uint8_t *b) {
	from_le(v, b);
	v[7] &= 0x7fffffffu;
	if (ge8(v, k_p)) sub8(v, k_p);
}

static int hw_usable(void) {
	if (hw_state >= 0) return hw_state;
	hw_state = 0;
	if (!mm_present()) return 0;
	uint32_t cfg = mm_rd(W_CONFIG);
	hw_w = cfg & 0xFF;
	if (((cfg >> 8) & 0xFF) < NREGS || hw_w < 8 || hw_w > 12) return 0;
	// n0inv = -p^-1 mod 2^32
	uint32_t inv = 1;
	for (int i = 0; i < 5; i++) inv *= 2u - k_p[0] * inv;
	hw_n0inv = 0u - inv;
	// R^2 mod p: 1 doubled 64 * width times, mod p
	memset(hw_r2, 0, sizeof(hw_r2));
	hw_r2[0] = 1;
	for (uint32_t k = 0; k < 64 * hw_w; k++) {
		uint32_t carry = 0;
		for (int i = 0; i < 8; i++) { uint32_t t = hw_r2[i]; hw_r2[i] = (t << 1) | carry; carry = t >> 31; }
		if (carry || ge8(hw_r2, k_p)) sub8(hw_r2, k_p);
	}
	hw_state = 1;
	return 1;
}

static int rget(void) {
	for (int i = 0; i < NREGS; i++)
		if (!(used & (1u << i))) { used |= (uint16_t)(1u << i); return i; }
	return 0;			// cannot happen: nothing here needs more than 16
}
static void rput(int r) { used &= (uint16_t)~(1u << r); }

static void ld(int r, const uint32_t *v) {
	mm_wr(W_RSEL, (uint32_t)r << 4);
	for (uint32_t i = 0; i < hw_w; i++) mm_wr(W_RDATA, i < 8 ? v[i] : 0);
}
static void st(int r, uint32_t *v) {
	mm_wr(W_RSEL, (uint32_t)r << 4);
	for (int i = 0; i < 8; i++) v[i] = mm_rd(W_RDATA);
}
static void cmd(uint32_t op, int d, int a, int b) {
	mm_wr(W_CMD, (op << 12) | ((uint32_t)d << 8) | ((uint32_t)a << 4) | (uint32_t)b);
	while (mm_rd(W_CMD) & 1u) { }
}
#define MUL(d, a, b) cmd(OP_MUL, (d), (a), (b))
#define ADD(d, a, b) cmd(OP_ADD, (d), (a), (b))
#define SUB(d, a, b) cmd(OP_SUB, (d), (a), (b))

static int hw_begin(void) {
	if (z25519_no_hw || !hw_usable()) return 0;
	uint32_t me = owner_id(), who;
	mm_wr(W_OWNER, me);
	who = mm_rd(W_OWNER);
	if (who != me) return 0;		// held by another (the register file implies OWNER)
	mm_wr(W_N0INV, hw_n0inv);
	for (uint32_t i = 0; i < hw_w; i++) mm_wr(W_N + i, i < 8 ? k_p[i] : 0);
	used = 0;
	return 1;
}

// Zeros over every register and the classic A, B and R, then release.
static void hw_end(void) {
	static const fe8 zero = { 0 };
	for (int r = 0; r < NREGS; r++) ld(r, zero);
	MUL(0, 0, 0);				// A = B = 0, and R = 0
	mm_wr(W_OWNER, owner_id() | RELEASE);
}

// Registers holding constants the operations use.
typedef struct { int r2, one, zero, one_m; } consts_t;

static void consts_load(consts_t *c) {
	static const fe8 one = { 1 }, zero = { 0 };
	c->r2 = rget(); ld(c->r2, hw_r2);
	c->one = rget(); ld(c->one, one);
	c->zero = rget(); ld(c->zero, zero);
	c->one_m = rget(); MUL(c->one_m, c->one, c->r2);		// Montgomery 1 = R mod p
}

// A new register holding v in Montgomery form.
static int load_m(const consts_t *c, const uint32_t *v) {
	int r = rget();
	ld(r, v);
	MUL(r, r, c->r2);
	return r;
}

// The canonical value (not Montgomery) of register r.
static void fetch_plain(const consts_t *c, int r, uint32_t *v) {
	int t = rget();
	MUL(t, r, c->one);
	st(t, v);
	rput(t);
}

// x^((p-5)/8) = x^(2^252-3), into a new register: the addition chain
// Monocypher's invsqrt() uses, 250 squarings and 11 multiplications.
static int sqn(int t, int n) { for (int i = 0; i < n; i++) MUL(t, t, t); return t; }
static int pow22523(int x) {
	int t0 = rget(), t1 = rget(), t2 = rget();
	MUL(t0, x, x);						// 2
	MUL(t1, t0, t0); MUL(t1, t1, t1);	// 8
	MUL(t1, x, t1);						// 9
	MUL(t0, t0, t1);					// 11
	MUL(t0, t0, t0);					// 22
	MUL(t0, t1, t0);					// 2^5 - 1
	MUL(t1, t0, t0); sqn(t1, 4);		MUL(t0, t1, t0);	// 2^10 - 1
	MUL(t1, t0, t0); sqn(t1, 9);		MUL(t1, t1, t0);	// 2^20 - 1
	MUL(t2, t1, t1); sqn(t2, 19);		MUL(t1, t2, t1);	// 2^40 - 1
	MUL(t1, t1, t1); sqn(t1, 9);		MUL(t0, t1, t0);	// 2^50 - 1
	MUL(t1, t0, t0); sqn(t1, 49);		MUL(t1, t1, t0);	// 2^100 - 1
	MUL(t2, t1, t1); sqn(t2, 99);		MUL(t1, t2, t1);	// 2^200 - 1
	MUL(t1, t1, t1); sqn(t1, 49);		MUL(t0, t1, t0);	// 2^250 - 1
	MUL(t0, t0, t0); MUL(t0, t0, t0);	// 2^252 - 4
	MUL(t0, t0, x);						// 2^252 - 3
	rput(t1); rput(t2);
	return t0;
}

// x^(p-2) = x^-1 (0 for 0), into a new register:
// p - 2 = 8 (2^252 - 3) + 3.
static int invert(int x) {
	int t = pow22523(x), x3 = rget();
	MUL(t, t, t); MUL(t, t, t); MUL(t, t, t);
	MUL(x3, x, x); MUL(x3, x3, x);
	MUL(t, t, x3);
	rput(x3);
	return t;
}

// -- X25519 ------------------------------------------------------------

// Swaps two register NUMBERS when b is 1, without a branch.
static void cswap(uint32_t b, int *x, int *y) {
	int m = -(int)b, t = (*x ^ *y) & m;
	*x ^= t; *y ^= t;
}

static void hw_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
	uint8_t k[32];
	uint32_t u[8], res[8];
	static const fe8 a24v = { 121665 };
	consts_t c;
	int x1, a24, x2, z2, x3, z3;
	uint32_t swap = 0;

	memcpy(k, scalar, 32);
	k[0] &= 248; k[31] &= 127; k[31] |= 64;
	fe_bytes(u, point);

	consts_load(&c);
	x1 = load_m(&c, u);
	a24 = load_m(&c, a24v);
	x2 = rget(); MUL(x2, c.one_m, c.one_m);		// 1
	z2 = rget(); MUL(z2, c.zero, c.one_m);		// 0
	x3 = rget(); MUL(x3, x1, c.one_m);			// u
	z3 = rget(); MUL(z3, c.one_m, c.one_m);		// 1
	rput(c.r2); rput(c.zero);

	for (int t = 254; t >= 0; t--) {
		uint32_t kt = (k[t >> 3] >> (t & 7)) & 1u;
		int tA, tB, tC, tD, nx3, nz3, nz2;
		swap ^= kt;
		cswap(swap, &x2, &x3);
		cswap(swap, &z2, &z3);
		swap = kt;
		tA = rget(); ADD(tA, x2, z2);
		tB = rget(); SUB(tB, x2, z2);
		tC = rget(); ADD(tC, x3, z3);
		tD = rget(); SUB(tD, x3, z3);
		rput(x2); rput(z2); rput(x3); rput(z3);
		MUL(tD, tD, tA);						// DA
		MUL(tC, tC, tB);						// CB
		MUL(tA, tA, tA);						// AA
		MUL(tB, tB, tB);						// BB
		nx3 = rget(); ADD(nx3, tD, tC); MUL(nx3, nx3, nx3);
		nz3 = rget(); SUB(nz3, tD, tC); MUL(nz3, nz3, nz3); MUL(nz3, nz3, x1);
		SUB(tD, tA, tB);						// E = AA - BB
		MUL(tC, tA, tB);						// x2 = AA BB
		nz2 = rget(); MUL(nz2, tD, a24); ADD(nz2, nz2, tA); MUL(nz2, nz2, tD);
		rput(tA); rput(tB); rput(tD);
		x2 = tC; z2 = nz2; x3 = nx3; z3 = nz3;
	}
	cswap(swap, &x2, &x3);
	cswap(swap, &z2, &z3);

	{
		int zi = invert(z2), r = rget();
		MUL(r, x2, zi);
		MUL(r, r, c.one);			// out of Montgomery form
		st(r, res);
	}
	to_le(out, res);
	memset(k, 0, sizeof(k));
	memset(res, 0, sizeof(res));
}

// -- Ed25519 check -------------------------------------------------------

// A point in "cached" form, Z = 1: (y + x, y - x, 2 d x y).
typedef struct { int yp, ym, t2d; } cached_t;

// Decodes 32 bytes to the NEGATED point, as Monocypher's
// ge_frombytes_neg_vartime(): -1 if it is not on the curve.
static int decode_neg(const consts_t *c, const uint8_t b[32], cached_t *out) {
	uint32_t y[8], v1[8], v2[8], xp[8];
	int Y, y2, num, den, den3, t, pw, X, chk, dm;
	fe_bytes(y, b);
	Y = load_m(c, y);
	y2 = rget(); MUL(y2, Y, Y);
	num = rget(); SUB(num, y2, c->one_m);		// y^2 - 1
	dm = load_m(c, k_d);
	den = rget(); MUL(den, dm, y2); ADD(den, den, c->one_m);	// d y^2 + 1
	rput(y2);
	den3 = rget(); MUL(den3, den, den); MUL(den3, den3, den);	// den^3
	t = rget(); MUL(t, den3, den3); MUL(t, t, den); MUL(t, t, num);	// num den^7
	pw = pow22523(t);
	rput(t);
	X = rget(); MUL(X, num, den3); MUL(X, X, pw);	// num den^3 (num den^7)^((p-5)/8)
	rput(pw); rput(den3);
	chk = rget(); MUL(chk, X, X); MUL(chk, chk, den);	// den x^2
	st(chk, v1);
	st(num, v2);
	if (!eq8(v1, v2)) {
		ADD(chk, chk, num);						// den x^2 = -num ?
		st(chk, v1);
		static const fe8 zero = { 0 };
		if (!eq8(v1, zero)) {					// neither: not on the curve
			rput(chk); rput(num); rput(den); rput(dm); rput(X); rput(Y);
			return -1;
		}
		int s = load_m(c, k_sqrtm1);
		MUL(X, X, s);
		rput(s);
	}
	rput(chk); rput(num); rput(den);
	// the negated point: negate x when its parity EQUALS the sign bit
	fetch_plain(c, X, xp);
	if ((int)(xp[0] & 1u) == (b[31] >> 7)) SUB(X, c->zero, X);
	out->yp = rget(); ADD(out->yp, Y, X);
	out->ym = rget(); SUB(out->ym, Y, X);
	out->t2d = rget(); MUL(out->t2d, X, Y);
	rput(dm);
	dm = load_m(c, k_d2);
	MUL(out->t2d, out->t2d, dm);
	rput(dm); rput(X); rput(Y);
	return 0;
}

// (X, Y, Z, T) doubled: dbl-2008-hwcd for a = -1, every coordinate
// negated (F' = C - G, H' = A + B) -- the same point, projectively.
static void edw_dbl(int *X, int *Y, int *Z, int *T) {
	int t1 = rget(), t2 = rget();
	MUL(t1, *X, *X);				// A
	MUL(t2, *Y, *Y);				// B
	MUL(*Z, *Z, *Z); ADD(*Z, *Z, *Z);	// C = 2 Z^2
	ADD(*X, *X, *Y); MUL(*X, *X, *X);
	SUB(*X, *X, t1); SUB(*X, *X, t2);	// E = (X + Y)^2 - A - B
	SUB(*Y, t2, t1);				// G = B - A
	SUB(*Z, *Z, *Y);				// F' = C - G
	ADD(t1, t1, t2);				// H' = A + B
	MUL(*T, *X, t1);				// T3 = E H'
	MUL(t2, *X, *Z);				// X3 = E F'
	MUL(*X, *Y, t1);				// Y3 = G H'
	MUL(t1, *Z, *Y);				// Z3 = F' G
	{
		int nx = t2, ny = *X, nz = t1;
		rput(*Y); rput(*Z);
		*X = nx; *Y = ny; *Z = nz;
	}
}

// (X, Y, Z, T) + q, q cached with Z = 1: Monocypher's ge_madd().
static void edw_madd(int *X, int *Y, int *Z, int *T, const cached_t *q) {
	int t1 = rget(), t2 = rget(), t3 = rget();
	ADD(t1, *Y, *X); MUL(t1, t1, q->yp);	// a
	SUB(t2, *Y, *X); MUL(t2, t2, q->ym);	// b
	ADD(t3, t1, t2);				// Y' = a + b
	SUB(t1, t1, t2);				// X' = a - b
	ADD(*Z, *Z, *Z);				// z = 2 Z
	MUL(*T, *T, q->t2d);			// t = T 2d T2
	ADD(*X, *Z, *T);				// A = z + t
	SUB(*Y, *Z, *T);				// B = z - t
	MUL(*T, t1, t3);				// T3 = X' Y'
	MUL(*Z, t1, *Y);				// X3 = X' B
	MUL(t1, t3, *X);				// Y3 = Y' A
	MUL(t3, *X, *Y);				// Z3 = A B
	rput(*X); rput(*Y); rput(t2);
	*X = *Z; *Y = t1; *Z = t3;
}

static int hw_check(const uint8_t sig[64], const uint8_t pk[32], const uint8_t *msg, size_t len) {
	uint32_t s[8], rv[3][8], vx[8], vy[8], vz[8];
	uint8_t hash[64], h[32];
	consts_t c;
	cached_t A, B, R;
	int X, Y, Z, T;

	from_le(s, sig + 32);
	if (ge8(s, k_L)) return -1;					// s >= L

	{
		crypto_sha512_ctx ctx;
		crypto_sha512_init(&ctx);
		crypto_sha512_update(&ctx, sig, 32);
		crypto_sha512_update(&ctx, pk, 32);
		crypto_sha512_update(&ctx, msg, len);
		crypto_sha512_final(&ctx, hash);
		crypto_eddsa_reduce(h, hash);
	}

	consts_load(&c);
	// -R first, kept in memory; then -A, kept in registers
	if (decode_neg(&c, sig, &R)) return -1;
	st(R.yp, rv[0]); st(R.ym, rv[1]); st(R.t2d, rv[2]);
	rput(R.yp); rput(R.ym); rput(R.t2d);
	if (decode_neg(&c, pk, &A)) return -1;
	// B, cached
	{
		int bx = load_m(&c, k_bx), by = load_m(&c, k_by), d2 = load_m(&c, k_d2);
		B.yp = rget(); ADD(B.yp, by, bx);
		B.ym = rget(); SUB(B.ym, by, bx);
		B.t2d = rget(); MUL(B.t2d, bx, by); MUL(B.t2d, B.t2d, d2);
		rput(bx); rput(by); rput(d2);
	}
	// the identity
	X = rget(); MUL(X, c.zero, c.one_m);
	Y = rget(); MUL(Y, c.one_m, c.one_m);
	Z = rget(); MUL(Z, c.one_m, c.one_m);
	T = rget(); MUL(T, c.zero, c.one_m);
	rput(c.r2); rput(c.one);

	// [s]B + [h](-A): s and h are below 2^253
	for (int i = 252; i >= 0; i--) {
		edw_dbl(&X, &Y, &Z, &T);
		if ((s[i >> 5] >> (i & 31)) & 1u) edw_madd(&X, &Y, &Z, &T, &B);
		if ((h[i >> 3] >> (i & 7)) & 1u) edw_madd(&X, &Y, &Z, &T, &A);
	}
	// + (-R), then times 8
	rput(B.yp); rput(B.ym); rput(B.t2d);
	R.yp = rget(); ld(R.yp, rv[0]);
	R.ym = rget(); ld(R.ym, rv[1]);
	R.t2d = rget(); ld(R.t2d, rv[2]);
	edw_madd(&X, &Y, &Z, &T, &R);
	edw_dbl(&X, &Y, &Z, &T);
	edw_dbl(&X, &Y, &Z, &T);
	edw_dbl(&X, &Y, &Z, &T);
	// the identity: X = 0 and Y = Z (Z is never 0: the formulas are complete)
	st(X, vx); st(Y, vy); st(Z, vz);
	{
		static const fe8 zero = { 0 };
		return (eq8(vx, zero) && eq8(vy, vz)) ? 0 : -1;
	}
}
#undef MUL
#undef ADD
#undef SUB
#endif

// -- the interface ---------------------------------------------------------

void z_x25519(uint8_t shared[32], const uint8_t scalar[32], const uint8_t point[32]) {
#ifdef Z25519_HW
	if (hw_begin()) {
		hw_x25519(shared, scalar, point);
		hw_end();
		z25519_last_hw = 1;
		return;
	}
#endif
	z25519_last_hw = 0;
	crypto_x25519(shared, scalar, point);
}

void z_x25519_public_key(uint8_t public_key[32], const uint8_t secret_key[32]) {
	static const uint8_t base[32] = { 9 };
	z_x25519(public_key, secret_key, base);
}

int z_ed25519_check(const uint8_t signature[64], const uint8_t public_key[32],
	const uint8_t *msg, size_t msg_size) {
#ifdef Z25519_HW
	if (hw_begin()) {
		int r = hw_check(signature, public_key, msg, msg_size);
		hw_end();
		z25519_last_hw = 1;
		return r;
	}
#endif
	z25519_last_hw = 0;
	return crypto_ed25519_check(signature, public_key, msg, msg_size);
}
