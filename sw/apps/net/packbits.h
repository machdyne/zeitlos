/*
 * Zeitlos
 *
 * PackBits (TIFF/Mac) for the screen streamer: a control byte, then
 * either a literal run or a repeated byte. Runs of one value -- most
 * of a 1bpp desktop stripe -- collapse hard, and the worst case grows
 * the data by only ~1/128, so there is no expansion trap.
 *
 * WHY THE RUN LOOP LOOKS AT WORDS
 *
 * This encoder is the second largest thing net does while a browser
 * watches the desktop. Measured on the board (netprof.h, gpu3d
 * animating, ten stripes changing per frame): 5.8 ms per 1280-byte
 * stripe, 218 cycles per input byte, 27-37% of the whole CPU.
 *
 * The first attempt at fixing that read the source a word at a time
 * and took bytes out of a register, on the theory that a machine with
 * no data cache was paying a bus access per byte. MEASURED, and it
 * was 23% SLOWER (146887 cycles a stripe against 119249). The theory
 * was wrong: this is a PicoRV32, which spends several cycles on every
 * instruction, so what the loop costs is INSTRUCTIONS RETIRED, and
 * hiding a load behind three arithmetic ops is a bad trade.
 *
 * What works on that machine is doing less per byte. A desktop stripe
 * is mostly one repeated value, so the run counter is where nearly
 * all the iterations are: it walks up to 128 identical bytes one at a
 * time. When it is word-aligned it can test four at once against the
 * value splatted into a word -- one load and one compare instead of
 * four of each. Runs cap at 128, which is a multiple of four, so the
 * cap still lands exactly where it did.
 *
 * The output is byte for byte what the plain version produced;
 * tests/test_packbits.c checks that exhaustively, because a browser
 * running the old viewer page has to decode what this build sends.
 *
 * Little-endian only, which RISC-V is here.
 */

#ifndef PACKBITS_H
#define PACKBITS_H

#include <stdint.h>

/* Encode n bytes of `in` into `out`, returning the encoded length. */
static int packbits(const uint8_t *in, int n, uint8_t *out)
{
	const uint32_t *w = (const uint32_t *)(const void *)in;
	const int aligned = (((uintptr_t)in & 3) == 0);
	int i = 0, o = 0;

	while (i < n) {
		const uint8_t v = in[i];
		const uint32_t v4 = (uint32_t)v * 0x01010101u;
		int run = 1;

		while (i + run < n && run < 128) {
			int k = i + run;
			/* four at a time while they line up and fit */
			if (aligned && (k & 3) == 0 && run + 4 <= 128 &&
					k + 4 <= n) {
				if (w[k >> 2] == v4) {
					run += 4;
					continue;
				}
				/* the word differs, so the byte loop below
				 * finishes this run in at most three more
				 * steps and then breaks */
			}
			if (in[k] != v)
				break;
			run++;
		}

		if (run >= 2) {			/* repeat */
			out[o++] = (uint8_t)(257 - run);
			out[o++] = v;
			i += run;
		} else {			/* literal span */
			int j = i, lit = 0;
			while (j < n && lit < 128) {
				/* the old inner loop counted up to three
				 * equal bytes but broke at two, so one
				 * lookahead byte decides it */
				if (j + 1 < n && in[j + 1] == in[j])
					break;	/* a repeat starts here */
				j++; lit++;
			}
			out[o++] = (uint8_t)(lit - 1);
			for (int k = 0; k < lit; k++)
				out[o++] = in[i + k];
			i += lit;
		}
	}
	return o;
}

#endif
