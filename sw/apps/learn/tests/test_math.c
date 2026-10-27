/* learn -- formulas: parse and evaluate (host). make -C sw/apps/learn test */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../lmath.h"

static uint8_t mem[1 << 16];
static int fails;

static lm_val_t ev(const char *s) {
	lx_arena_t a; char err[80];
	lx_arena_init(&a, mem, sizeof mem);
	lm_val_t v = lm_eval_str(&a, s, err, sizeof err);
	return v;
}
static void truth(const char *s, int want) {
	lm_val_t v = ev(s);
	int got = v.ok && v.is_bool ? v.truth : -1;
	if (got != want) { printf("FAIL %s: got %d want %d\n", s, got, want); fails++; }
}
static void exact(const char *s, long n, long d) {
	lm_val_t v = ev(s);
	if (!v.ok || !v.exact || v.num != n || v.den != d) {
		printf("FAIL %s: got ok=%d exact=%d %lld/%lld\n", s, v.ok, v.exact, (long long)v.num, (long long)v.den); fails++;
	}
}
static void approx(const char *s, double want, double tol) {
	lm_val_t v = ev(s);
	if (!v.ok || fabs(v.v - want) > tol) { printf("FAIL %s: got %g want %g\n", s, v.v, want); fails++; }
}
static void bad(const char *s) {
	lm_val_t v = ev(s);
	if (v.ok) { printf("FAIL %s: should not compute\n", s); fails++; }
}
static void noparse(const char *s) {
	lx_arena_t a; char err[80];
	lx_arena_init(&a, mem, sizeof mem);
	if (lm_parse(&a, s, (int)strlen(s), err, sizeof err)) { printf("FAIL %s: should not parse\n", s); fails++; }
}

int main(void) {
	truth("3/4 > 2/4", 1);
	truth("2/5 > 2/3", 0);
	truth("2/3 > 2/5", 1);
	truth("1/2 = 2/4", 1);
	truth("0.1 + 0.2 = 0.3", 1);
	truth("0 < 1/3 < 1", 1);
	truth("3/8 != 5/8", 1);
	truth("2^10 = 1024", 1);
	truth("sqrt(16) = 4", 1);
	truth("25% = 1/4", 1);
	truth("2 1/2 = 5/2", 1);
	truth("3 × 4 = 12", 1);
	truth("12 ÷ 4 = 3", 1);
	truth("-3 < -2", 1);
	exact("2 1/2 + 1/4", 11, 4);
	exact("(1/2)^-2", 4, 1);
	exact("6 / 8", 3, 4);
	exact("0.25", 1, 4);
	exact("2(3+4)", 14, 1);
	approx("2 pi sqrt(0.5/9.81)", 1.4185, 0.0001);
	approx("2*pi*sqrt(1/9.81)", 2.006, 0.001);
	approx("sin(pi/2)", 1, 1e-12);
	approx("log(1000)", 3, 1e-12);
	bad("x + 1");
	bad("1/0");
	bad("sqrt(-1)");
	noparse("3 +");
	noparse("(1 + 2");
	noparse("3 $ 4");
	{
		lx_arena_t a; char err[80];
		lx_arena_init(&a, mem, sizeof mem);
		lm_node_t *n = lm_parse(&a, "T = 2 pi sqrt(L/g)", 18, err, sizeof err);
		if (!n || !lm_has_vars(n)) { printf("FAIL vars\n"); fails++; }
		if (!lm_greek("theta")) { printf("FAIL greek\n"); fails++; }
	}
	printf("%s: %d failed\n", fails ? "test_math FAILED" : "test_math ok", fails);
	return fails != 0;
}
