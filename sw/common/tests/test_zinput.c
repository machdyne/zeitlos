/*
 * Host tests for the pure half of sw/common/zinput.h: the pointer word,
 * the key word, and the one-second release. The register writes are
 * the same words, applied on the board.
 *
 *   cc -std=gnu99 -Wall -Wextra -I sw/common -o /tmp/test_zinput \
 *      sw/common/tests/test_zinput.c sw/common/zinput.c && /tmp/test_zinput
 */

#include <stdio.h>
#include <stdint.h>

#include "zinput.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

/* zinput.c calls these. The test never sends a packet, so they stay
 * uncalled; the link still needs them. */
void hid_inject(int32_t ev) { (void)ev; }
void z_wm_wake(void) {}
uint32_t z_uptime_ticks(void) { return 0; }

static void test_words(void)
{
	uint32_t w;

	w = zinput_pointer_word(320, 200, 1);
	CHECK(w == ((1u << 24) | (1u << 20) | (200u << 10) | 320u),
		"present, left button, y, x");
	w = zinput_pointer_word(0x7d0, 0x7d0, 0xff);
	CHECK((w & (1u << 24)) != 0, "present is set");
	CHECK(((w >> 20) & 7) == 7, "only the three button bits are stored");
	CHECK((w & 0x3ff) == 0x3d0, "x is ten bits");
	CHECK(((w >> 10) & 0x3ff) == 0x3d0, "y is ten bits");
	/* Bit 7 is the leave. It is not a button, and the word still has
	 * present set: dropping present is zinput_mouse's decision. */
	w = zinput_pointer_word(1, 2, 0x80);
	CHECK((w & (1u << 24)) != 0, "the packed word always sets present");
	CHECK(((w >> 20) & 7) == 0, "bit 7 is not a button");

	CHECK(zinput_key_word(19, 0x04, 1) == ((0x04u << 9) | (19u << 1) | 1u),
		"P, alt, pressed");
	CHECK((zinput_key_word(19, 0x04, 0) & 1u) == 0, "a release clears bit 0");
	CHECK((zinput_key_word(0x1ff, 0x1ff, 2) & ~0x1ffffu) == 0,
		"usage and mods are eight bits, pressed is one");
	CHECK(zinput_key_word(0, 0, 2) == 1u, "any nonzero pressed is down");
}

static void test_stale(void)
{
	CHECK(!zinput_stale(1000, 1000, 732), "just arrived is not stale");
	CHECK(!zinput_stale(1731, 1000, 732), "one tick short of a second");
	CHECK(zinput_stale(1732, 1000, 732), "a second later is stale");
	CHECK(zinput_stale(300, 0xfffffe00u, 732),
		"a wrapped tick count still expires");
	CHECK(!zinput_stale(0xfffffff0u, 100, 732),
		"a backwards gap is not yet a second");
}

int main(void)
{
	test_words();
	test_stale();
	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
