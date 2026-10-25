/*
 * Desktop colour's accent palette (sw/common/zaccent.c, docs/color.md
 * "Desktop colour"). Host only:
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_accent \
 *       sw/common/tests/test_accent.c sw/common/zaccent.c
 */

#include <stdio.h>
#include "zaccent.h"

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int ch(uint16_t c, int k) { return (c >> (8 - 4 * k)) & 15; }
static int luma(uint16_t c) { return 3 * ch(c, 0) + 6 * ch(c, 1) + ch(c, 2); }

int main(void) {
	uint16_t p[4];
	static const char *names[4] = { "white", "amber", "green", "paper" };

	/* the defaults, as drawn in the proposal */
	z_accent_palette(0, 7, false, p);
	CHECK(p[0] == 0x000 && p[1] == 0x888 && p[2] == 0x123 && p[3] == 0xCA8,
		"white at 7: %03x %03x %03x %03x", p[0], p[1], p[2], p[3]);
	z_accent_palette(3, 7, false, p);
	CHECK(p[0] == 0x888 && p[1] == 0x000, "paper at 7: %03x %03x", p[0], p[1]);
	z_accent_palette(1, 7, false, p);
	CHECK(p[1] == 0xC90, "amber ink at 7: %03x (D2,9D rounded)", p[1]);
	z_accent_palette(0, 15, false, p);
	CHECK(p[1] == 0xFFF, "white at 15: %03x", p[1]);

	/* entry -> colour: bits 0 and 4 only */
	for (uint32_t i = 0; i < 256; i++) {
		int want = (int)((i & 1) | ((i >> 3) & 2));
		CHECK(z_accent_entry_colour(i) == want, "entry %u", i);
	}

	for (uint32_t m = 0; m < 4; m++)
		for (uint32_t l = 0; l < 16; l++) {
			/* the pointer: black <-> A1 and ink <-> A0 contrast */
			z_accent_palette(m, l, false, p);
			if (l >= 3) {
				CHECK(luma(p[3]) - luma(p[0]) >= 15 || luma(p[0]) - luma(p[3]) >= 15,
					"%s %u: black vs A1 too close", names[m], l);
				CHECK(luma(p[2]) - luma(p[1]) >= 15 || luma(p[1]) - luma(p[2]) >= 15,
					"%s %u: ink vs A0 too close", names[m], l);
			}
			/* 1-bit VGA: every accent folds onto black or the ink */
			z_accent_palette(m, l, true, p);
			for (int k = 0; k < 3; k++) {
				int b0 = ch(p[0], k) >= 8, b1 = ch(p[1], k) >= 8;
				CHECK((ch(p[2], k) >= 8) == b0, "%s %u: A0 channel %d on VGA", names[m], l, k);
				CHECK((ch(p[3], k) >= 8) == b1, "%s %u: A1 channel %d on VGA", names[m], l, k);
			}
			CHECK(p[0] != p[1], "%s %u: black == ink with vga", names[m], l);
		}

	printf("accent: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
