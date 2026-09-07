/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for ecdsa.c's fast (Solinas) reduction, against products
 * reduced in Python.
 *
 * Reduction is the one part of this file where a mistake is quiet.
 * The curve arithmetic on top of it fails loudly -- a wrong point is
 * not on the curve and the signature simply does not verify -- but a
 * reduction that is wrong for SOME inputs passes most signatures and
 * fails a few, which looks like a network problem or a bad
 * certificate.
 *
 * That is not hypothetical: the P-384 table, transcribed by hand from
 * FIPS 186-4, passed 3 of these 30 products. It is now derived from
 * the prime instead.
 *
 * Vectors include the corners -- (p-1)^2, p-1 times 1, zero -- since
 * carry and borrow propagation is where the errors live.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define main ecdsa_main_unused
#include "../ecdsa.c"
#undef main
#include "solinas_vectors.h"
static int fails, checks;
static void run(const char *tag, ec_curve_id_t id, const uint32_t *prod,
	const uint32_t *red, int cases) {
	ec_curve_t *cv = curve_for(id);
	int nl = cv->nl;
	for (int k = 0; k < cases; k++) {
		uint32_t out[12];
		fe_reduce(out, prod + k * 2 * nl, cv);
		checks++;
		if (memcmp(out, red + k * nl, nl * 4)) {
			fails++;
			if (fails < 4) printf("FAIL %s case %d\n", tag, k);
		}
	}
}
int main(void){
	run("p256", EC_CURVE_P256, p256_prod, p256_red, P256_CASES);
	run("p384", EC_CURVE_P384, p384_prod, p384_red, P384_CASES);
	printf("%s: %d reductions, %d wrong\n", fails?"FAIL":"ok", checks, fails);
	return fails?1:0;}
