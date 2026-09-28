/*
 * Host test for sw/common/zmlkem.c. docs/mlkem.md, "Testing".
 *
 *   make -C sw/common/tests -f Makefile.zmlkem
 *
 * mlkem_vectors.txt (gen_mlkem_vectors.py): NIST's official FIPS 203
 * vectors for ML-KEM-768 -- key generation, encapsulation, decapsulation
 * (invalid ciphertexts among them), and both input checks -- and cases
 * from kyber-py, an independent implementation: random ones, tampered
 * ciphertexts (the implicit-rejection secret must match exactly), and
 * keys the checks must refuse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../zmlkem.h"

static int unhex(const char *h, uint8_t *b, int cap) {
	int n = 0;
	for (; h[0] && h[1] && n < cap; h += 2) { unsigned v; sscanf(h, "%2x", &v); b[n++] = (uint8_t)v; }
	return n;
}

int main(void) {
	static char line[20000], src[16], kind[8], a[8000], b[8000], c[8000], d[8000];
	static uint8_t x1[4000], x2[4000], x3[4000], x4[4000], o1[4000], o2[4000];
	FILE *f = fopen("mlkem_vectors.txt", "r");
	if (!f) { printf("no mlkem_vectors.txt\n"); return 1; }
	if (!fgets(line, sizeof(line), f)) return 1;
	int n = atoi(line), bad = 0, count[2][5] = { { 0 } };
	const char *kinds[5] = { "kg", "enc", "dec", "ekc", "dkc" };
	for (int i = 0; i < n; i++) {
		if (!fgets(line, sizeof(line), f)) { printf("short file\n"); return 1; }
		a[0] = b[0] = c[0] = d[0] = 0;
		sscanf(line, "%15s %7s %7999s %7999s %7999s %7999s", src, kind, a, b, c, d);
		int s = !strcmp(src, "nist") ? 0 : 1, k = 0;
		while (k < 5 && strcmp(kind, kinds[k])) k++;
		bool ok = false;
		if (k == 0) {			// d z -> ek dk
			uint8_t coins[64];
			unhex(a, coins, 32); unhex(b, coins + 32, 32);
			unhex(c, x3, 4000); unhex(d, x4, 4000);
			zmlkem_keypair(o1, o2, coins);
			ok = !memcmp(o1, x3, ZMLKEM_EK_BYTES) && !memcmp(o2, x4, ZMLKEM_DK_BYTES);
		} else if (k == 1) {	// ek m -> c k
			unhex(a, x1, 4000); unhex(b, x2, 32); unhex(c, x3, 4000); unhex(d, x4, 32);
			ok = zmlkem_encaps(o1, o2, x1, x2) && !memcmp(o1, x3, ZMLKEM_CT_BYTES) && !memcmp(o2, x4, 32);
		} else if (k == 2) {	// dk c -> k
			unhex(a, x1, 4000); unhex(b, x2, 4000); unhex(c, x3, 32);
			ok = zmlkem_decaps(o1, x2, x1) && !memcmp(o1, x3, 32);
		} else if (k == 3) {
			unhex(a, x1, 4000);
			ok = zmlkem_ek_ok(x1) == (atoi(b) == 1);
			// and encapsulation refuses what the check refuses
			if (ok && atoi(b) == 0) { uint8_t m[32] = { 0 }; ok = !zmlkem_encaps(o1, o2, x1, m); }
		} else if (k == 4) {
			unhex(a, x1, 4000);
			ok = zmlkem_dk_ok(x1) == (atoi(b) == 1);
			if (ok && atoi(b) == 0) ok = !zmlkem_decaps(o1, x2, x1);
		}
		if (k < 5) count[s][k]++;
		if (!ok) { bad++; if (bad < 5) printf("  FAIL line %d: %s %s\n", i + 2, src, kind); }
	}
	printf("NIST FIPS 203 (ML-KEM-768): %d keygen, %d encaps, %d decaps, %d ek checks, %d dk checks\n",
		count[0][0], count[0][1], count[0][2], count[0][3], count[0][4]);
	printf("kyber-py: %d keygen, %d encaps, %d decaps (half tampered), %d ek checks, %d dk checks\n",
		count[1][0], count[1][1], count[1][2], count[1][3], count[1][4]);

	// a round trip, and the time on this machine
	{
		uint8_t coins[64], m[32], ek[ZMLKEM_EK_BYTES], dk[ZMLKEM_DK_BYTES], ct[ZMLKEM_CT_BYTES], s1[32], s2[32];
		for (int i = 0; i < 64; i++) coins[i] = (uint8_t)(i * 7);
		for (int i = 0; i < 32; i++) m[i] = (uint8_t)(i * 13);
		clock_t t0 = clock();
		for (int i = 0; i < 100; i++) { zmlkem_keypair(ek, dk, coins); zmlkem_encaps(ct, s1, ek, m); zmlkem_decaps(s2, ct, dk); }
		double ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000 / 100;
		if (memcmp(s1, s2, 32)) { bad++; printf("  FAIL: a round trip\n"); }
		printf("(keygen + encaps + decaps: %.2f ms on this host)\n", ms);
	}
	printf("zmlkem: %d cases, %d failed\n", n, bad);
	return bad != 0;
}
