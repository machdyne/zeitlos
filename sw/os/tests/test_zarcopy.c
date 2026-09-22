/*
 * Host test for sw/os/zarcopy.h: the flash-window copy against memcpy.
 *
 *   cc -std=gnu99 -Wall -o /tmp/t sw/os/tests/test_zarcopy.c && /tmp/t
 *
 * Every source alignment, every destination alignment, every length
 * from 0 to 40, and a long one -- and nothing written outside the
 * destination. (Little-endian host, as the controller is. An x86 host
 * also allows the misaligned accesses the RISC-V does not, so this
 * checks the bytes, not the alignment rule; the rule is in the code.)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../zarcopy.h"

int main(void) {
	static uint8_t src[70000], out[70100], want[70100];
	uint32_t s, d, n, i;
	int fails = 0, checks = 0;

	for (i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 131 + (i >> 8) * 7 + 1);
	for (s = 0; s < 4; s++)
		for (d = 0; d < 4; d++)
			for (n = 0; n <= 40; n++) {
				memset(out, 0xEE, sizeof(out));
				memset(want, 0xEE, sizeof(want));
				memcpy(want + 8 + d, src + 16 + s, n);
				zar_copy(out + 8 + d, src + 16 + s, n);
				checks++;
				if (memcmp(out, want, sizeof(out))) {
					fails++;
					if (fails < 5) printf("FAIL: source +%u, destination +%u, %u bytes\n", s, d, n);
				}
			}
	for (s = 0; s < 4; s++) {
		n = 65000 + s;
		memset(out, 0xEE, sizeof(out));
		memset(want, 0xEE, sizeof(want));
		memcpy(want + 12 + s, src + 3 + s, n);
		zar_copy(out + 12 + s, src + 3 + s, n);
		checks++;
		if (memcmp(out, want, sizeof(out))) { fails++; printf("FAIL: a long copy, +%u\n", s); }
	}
	printf("%s: %d checks (4 x 4 alignments x 0..40 bytes, and 65 KB), %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);
	return fails != 0;
}
