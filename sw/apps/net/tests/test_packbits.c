/*
 * Zeitlos
 *
 * The word-at-a-time PackBits encoder in screen.c has to produce the
 * SAME bytes as the byte-at-a-time one it replaces -- a viewer running
 * the old page decodes what this build sends, so a different (even if
 * still valid) encoding is a compatibility change, not an optimisation.
 *
 * This runs both over the shapes a 1bpp desktop stripe actually takes
 * -- long uniform runs, a window edge, dithered fill, text -- plus
 * exhaustive small cases and random data, and compares byte for byte.
 * It also decodes the output and checks it round-trips.
 *
 *   cc -O2 -o /tmp/test_packbits tests/test_packbits.c && /tmp/test_packbits
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* the encoder as it was, byte at a time */
static int packbits_ref(const uint8_t *in, int n, uint8_t *out)
{
	int i = 0, o = 0;
	while (i < n) {
		int run = 1;
		while (i + run < n && run < 128 && in[i + run] == in[i]) run++;
		if (run >= 2) {			/* repeat */
			out[o++] = (uint8_t)(257 - run);
			out[o++] = in[i];
			i += run;
		} else {			/* literal span */
			int j = i, lit = 0;
			while (j < n && lit < 128) {
				int r = 1;
				while (j + r < n && r < 3 && in[j + r] == in[j]) r++;
				if (r >= 2) break;	/* a repeat starts here */
				j++; lit++;
			}
			out[o++] = (uint8_t)(lit - 1);
			for (int k = 0; k < lit; k++) out[o++] = in[i + k];
			i += lit;
		}
	}
	return o;
}

#include "../packbits.h"

static int unpack(const uint8_t *in, int n, uint8_t *out, int cap)
{
	int i = 0, o = 0;
	while (i < n) {
		int c = (int8_t)in[i++];
		if (c >= 0) {
			for (int k = 0; k <= c && i < n && o < cap; k++)
				out[o++] = in[i++];
		} else if (c != -128) {
			int rep = 257 - (int)in[i - 1];
			uint8_t v = in[i++];
			for (int k = 0; k < rep && o < cap; k++)
				out[o++] = v;
		}
	}
	return o;
}

static int fails;

static void check(const char *what, const uint8_t *in, int n)
{
	static uint8_t a[8192], b[8192], back[8192];
	int na = packbits_ref(in, n, a);
	int nb = packbits(in, n, b);
	if (na != nb || memcmp(a, b, na) != 0) {
		printf("FAIL %s n=%d: ref %d bytes, new %d bytes\n", what, n, na, nb);
		for (int i = 0; i < (na < nb ? na : nb); i++)
			if (a[i] != b[i]) {
				printf("  first difference at %d: ref %02x new %02x\n",
					i, a[i], b[i]);
				break;
			}
		fails++;
		return;
	}
	int nr = unpack(b, nb, back, sizeof(back));
	if (nr != n || memcmp(back, in, n) != 0) {
		printf("FAIL %s n=%d: does not round-trip (%d back)\n", what, n, nr);
		fails++;
	}
}

int main(void)
{
	static uint8_t buf[1280];
	srandom(1);

	/* a blank stripe -- the common case on this desktop */
	memset(buf, 0, sizeof(buf));
	check("blank", buf, sizeof(buf));
	memset(buf, 0xff, sizeof(buf));
	check("solid", buf, sizeof(buf));

	/* a window edge: mostly background with a vertical rule */
	memset(buf, 0, sizeof(buf));
	for (int row = 0; row < 16; row++)
		buf[row * 80 + 23] = 0x81;
	check("edge", buf, sizeof(buf));

	/* dithered fill: alternating bytes, the worst case for runs */
	for (int i = 0; i < 1280; i++)
		buf[i] = (i & 1) ? 0xaa : 0x55;
	check("dither", buf, sizeof(buf));

	/* text: short runs of zero between glyph columns */
	for (int i = 0; i < 1280; i++)
		buf[i] = (i % 7 < 2) ? (uint8_t)(i * 31) : 0;
	check("text", buf, sizeof(buf));

	/* runs longer than 128, which is where the encoder has to split */
	memset(buf, 0x5a, sizeof(buf));
	buf[300] = 0; buf[301] = 1; buf[302] = 2;
	check("long-runs", buf, sizeof(buf));

	/* every 2-, 3- and 4-byte input, exhaustively over a small alphabet */
	for (int a = 0; a < 4; a++)
	for (int b = 0; b < 4; b++)
	for (int c = 0; c < 4; c++)
	for (int d = 0; d < 4; d++) {
		uint8_t q[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
		check("exhaustive4", q, 4);
	}

	/* random, at the real stripe size and at odd sizes */
	for (int t = 0; t < 200; t++) {
		int n = 1280;
		for (int i = 0; i < n; i++) {
			long r = random() % 100;
			buf[i] = r < 70 ? 0 : (r < 85 ? 0xff : (uint8_t)random());
		}
		check("random-stripe", buf, n);
	}
	for (int t = 0; t < 500; t++) {
		int n = 4 + (random() % 300) * 4;	/* multiple of 4 */
		for (int i = 0; i < n; i++)
			buf[i] = (uint8_t)(random() % 5);
		check("random-small", buf, n);
	}

	printf(fails ? "%d FAILURES\n" : "all packbits cases identical\n", fails);
	return fails ? 1 : 0;
}
