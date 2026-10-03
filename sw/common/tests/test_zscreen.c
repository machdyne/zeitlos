/*
 * Host tests for sw/common/zscreen.h: the stripe hash (a vertical line
 * must not hash like a blank stripe), PackBits against its literal-run
 * fallback, the frame trailer, and the DIRTY want/miss masks.
 *
 *   cc -std=gnu99 -Wall -Wextra -I sw/common -I sw/apps/net \
 *      -o /tmp/test_zscreen sw/common/tests/test_zscreen.c \
 *      sw/common/zscreen.c && /tmp/test_zscreen
 */

#include <stdio.h>
#include <string.h>

#include "zscreen.h"
#include "packbits.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static void test_hash(void)
{
	static uint32_t w[ZSCREEN_STRIPE_WORDS];
	uint32_t blank, line, other;
	int zeros = 0;

	memset(w, 0, sizeof w);
	blank = zscreen_hash(w, 0);
	/* word 0 of every row, one bit: a full-height vertical line */
	for (int r = 0; r < 16; r++)
		w[r * 20] = 0x80;
	line = zscreen_hash(w, 0);
	CHECK(line != blank, "a vertical line does not hash as blank");

	memset(w, 0, sizeof w);
	for (int r = 0; r < 16; r++)
		w[r * 20 + 3] = 1u << 15;
	other = zscreen_hash(w, 0);
	CHECK(other != blank && other != line, "a different column hashes differently");
	CHECK(zscreen_hash(w, 1) != other, "the stripe index is part of the hash");

	memset(w, 0, sizeof w);
	blank = zscreen_hash(w, 4);
	w[100] = 1;
	CHECK(zscreen_hash(w, 4) != blank, "one flipped bit changes the hash");

	memset(w, 0, sizeof w);
	for (int i = 0; i < ZSCREEN_STRIPES; i++)
		if (!zscreen_hash(w, i))
			zeros++;
	for (int r = 0; r < 16; r++)
		w[r * 20] = 0xffffffffu;
	for (int i = 0; i < ZSCREEN_STRIPES; i++)
		if (!zscreen_hash(w, i))
			zeros++;
	CHECK(!zeros, "a hash is never 0");
}

static void test_pack(void)
{
	static uint8_t raw[ZSCREEN_STRIPE_BYTES];
	static uint8_t dst[ZSCREEN_PACK_WORST + ZSCREEN_TRAILER];
	static uint8_t pb[ZSCREEN_PACK_WORST + 8];
	int n, pn, ok;

	memset(raw, 0xa5, sizeof raw);
	n = zscreen_pack(raw, dst);
	CHECK(n == 20, "a solid stripe is ten runs of 128");
	CHECK(dst[0] == (uint8_t)(257 - 128) && dst[1] == 0xa5, "repeat control and value");

	memset(raw, 0, sizeof raw);
	for (int i = 0; i < ZSCREEN_STRIPE_BYTES; i += 17)
		raw[i] = (uint8_t)i;
	pn = packbits(raw, ZSCREEN_STRIPE_BYTES, pb);
	n = zscreen_pack(raw, dst);
	CHECK(pn > 0 && pn <= ZSCREEN_LITERAL_LEN, "a mild stripe fits PackBits");
	CHECK(n == pn && !memcmp(dst, pb, (size_t)pn), "PackBits output is unchanged when it fits");

	/* A BB A BB ... : two bytes per three, past the literal-run cap. */
	for (int i = 0; i < ZSCREEN_STRIPE_BYTES; ) {
		raw[i++] = 0x11;
		if (i < ZSCREEN_STRIPE_BYTES)
			raw[i++] = 0x22;
		if (i < ZSCREEN_STRIPE_BYTES)
			raw[i++] = 0x22;
	}
	pn = packbits(raw, ZSCREEN_STRIPE_BYTES, pb);
	n = zscreen_pack(raw, dst);
	CHECK(pn > ZSCREEN_LITERAL_LEN, "the alternating stripe outgrows PackBits");
	CHECK(n == ZSCREEN_LITERAL_LEN, "it is sent as literal runs instead");
	ok = 1;
	for (int run = 0; run < 10; run++) {
		if (dst[run * 129] != 127)
			ok = 0;
		if (memcmp(dst + run * 129 + 1, raw + run * 128, 128))
			ok = 0;
	}
	CHECK(ok, "literal runs are the stripe, 128 bytes at a time");

	memset(raw, 0xa5, sizeof raw);
	n = zscreen_encode(raw, dst, 0x1234, 1);
	CHECK(n == 24, "solid stripe plus trailer");
	CHECK(dst[20] == ZSCREEN_TRAILER_MAGIC && dst[21] == 0x34 &&
		dst[22] == 0x12 && dst[23] == ZSCREEN_TRAILER_LAST, "trailer, last of frame");
	zscreen_trailer(dst, 0x0001, 0);
	CHECK(dst[0] == ZSCREEN_TRAILER_MAGIC && dst[1] == 1 && dst[2] == 0 &&
		dst[3] == 0, "trailer, not last");
}

static void test_masks(void)
{
	zscreen_t s;

	CHECK(zscreen_want(0, 0x5, ZSCREEN_FULL_NONE) == ZSCREEN_ALL,
		"without DIRTY every stripe is read");
	CHECK(zscreen_want(1, 0x5, ZSCREEN_FULL_NONE) == 0x5,
		"with DIRTY only the marked stripes are read");
	CHECK(zscreen_want(1, 0, ZSCREEN_FULL_NONE) == 0,
		"nothing marked is nothing to read");
	CHECK(zscreen_want(1, 0x5, ZSCREEN_FULL_RESET) == ZSCREEN_ALL,
		"a reset reads every stripe");
	CHECK(zscreen_want(1, 0x5, ZSCREEN_FULL_VERIFY) == ZSCREEN_ALL,
		"a verify reads every stripe");
	CHECK(zscreen_miss(0x7, 0x1, 0x2) == 0x4,
		"a change outside the sample and the later register is a miss");
	CHECK(zscreen_miss(0x7, 0x1, 0) == 0x6, "a miss with nothing written during the copy");
	CHECK(zscreen_miss(0x7, 0x7, 0) == 0, "a stripe that was marked is not a miss");
	CHECK(zscreen_miss(0x7, 0x1, 0x6) == 0, "a stripe written during the copy is not a miss");

	zscreen_init(&s);
	CHECK(s.dirty_hw == 0 && s.hash[3] == 0 && s.verify_passes == 0, "init probes later");
	s.hash[3] = 9;
	s.snap[0] = 4;
	s.dirty_hw = 1;
	s.verify_passes = 2;
	zscreen_forget(&s);
	CHECK(s.hash[3] == 0 && s.snap[0] == 4 && s.dirty_hw == 1 &&
		s.verify_passes == 2, "forget clears hashes and nothing else");
}

int main(void)
{
	test_hash();
	test_pack();
	test_masks();
	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
