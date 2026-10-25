/*
 * Desktop colour's accent palette (sw/common/zaccent.c, docs/color.md
 * "Desktop colour"). Host only:
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_accent \
 *       sw/common/tests/test_accent.c sw/common/zaccent.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

	/* -- themes -- */
	{
		uint16_t pr[2], q[4], ref[4];
		z_accent_spec_t sp;

		CHECK(z_accent_theme_count >= 20, "%d themes", z_accent_theme_count);
		CHECK(z_accent_theme_find("phosphor") == 0, "phosphor is theme 0");
		CHECK(z_accent_theme_find("Nord") > 0, "find ignores case");
		CHECK(z_accent_theme_find("nope") < 0, "unknown theme");
		for (int i = 0; i < z_accent_theme_count; i++)
			CHECK(z_accent_theme_find(z_accent_themes[i].id) == i, "theme %s unique",
				z_accent_themes[i].id);

		CHECK(z_accent_parse_pair("123,CA8", pr) && pr[0] == 0x123 && pr[1] == 0xCA8, "pair");
		CHECK(z_accent_parse_pair(" #0aF  #fff ", pr) && pr[0] == 0x0AF && pr[1] == 0xFFF, "pair, # and spaces");
		CHECK(!z_accent_parse_pair("1234,123", pr), "four digits refused");
		CHECK(!z_accent_parse_pair("12,123", pr), "two digits refused");
		CHECK(!z_accent_parse_pair("123", pr), "one colour refused");
		CHECK(!z_accent_parse_pair("123,456,789", pr), "three refused");
		CHECK(!z_accent_parse_pair("xyz,123", pr), "not hex");

		for (int t = 0; t < z_accent_theme_count; t++) {
			const z_accent_theme_t *th = &z_accent_themes[t];
			memset(&sp, 0, sizeof(sp));
			sp.theme = t;
			CHECK(z_accent_spec_exact(&sp) == !th->full, "%s: exact", th->id);
			for (uint32_t m = 0; m < 4; m++)
				for (uint32_t l = 0; l < 16; l++) {
					z_accent_palette_spec(&sp, m, l, false, q);
					z_accent_palette(m, l, false, ref);
					if (t == 0)
						CHECK(memcmp(q, ref, sizeof(q)) == 0, "phosphor theme = the default");
					if (!th->full && t > 0) {
						CHECK(q[0] == ref[0] && q[1] == ref[1], "%s: the phosphor's black and ink", th->id);
						/* the tint of black is darker than the tint of ink, on paper too */
						if (l >= 3)
							CHECK((luma(q[2]) < luma(q[3])) == (m != 3), "%s %s %u: A0/A1 order",
								th->id, names[m], l);
					}
					if (th->full && l >= 3) {
						CHECK(abs(luma(q[3]) - luma(q[0])) >= 15, "%s %u: black vs A1 too close", th->id, l);
						CHECK(abs(luma(q[2]) - luma(q[1])) >= 15, "%s %u: ink vs A0 too close", th->id, l);
						CHECK(abs(luma(q[1]) - luma(q[0])) >= 30, "%s %u: black vs ink too close", th->id, l);
					}
					/* 1-bit VGA: accents fold onto black or the ink */
					z_accent_palette_spec(&sp, m, l, true, q);
					for (int k = 0; k < 3; k++) {
						CHECK((ch(q[2], k) >= 8) == (ch(q[0], k) >= 8), "%s: A0 VGA", th->id);
						CHECK((ch(q[3], k) >= 8) == (ch(q[1], k) >= 8), "%s: A1 VGA", th->id);
					}
				}
		}

		/* overrides */
		memset(&sp, 0, sizeof(sp));
		sp.have_acc = true; sp.acc[0] = 0x112; sp.acc[1] = 0xEEF;
		z_accent_palette_spec(&sp, 0, 7, false, q);
		CHECK(q[0] == 0x000 && q[1] == 0x888 && q[2] == 0x112 && q[3] == 0xEEF, "custom accents on white");
		z_accent_palette_spec(&sp, 3, 7, false, q);
		CHECK(q[0] == 0x888 && q[1] == 0x000 && q[2] == 0xEEF && q[3] == 0x112, "custom accents swap on paper");
		CHECK(z_accent_spec_exact(&sp), "custom accents keep the ink path");
		sp.have_base = true; sp.base[0] = 0x024; sp.base[1] = 0xDDD;
		z_accent_palette_spec(&sp, 2, 7, false, q);
		CHECK(q[0] == 0x024 && q[1] == 0xDDD && q[2] == 0x112 && q[3] == 0xEEF, "custom base: all four");
		CHECK(!z_accent_spec_exact(&sp), "custom base leaves the ink path");
		sp.theme = z_accent_theme_find("nord"); sp.have_acc = false;
		z_accent_palette_spec(&sp, 0, 7, false, q);
		CHECK(q[0] == 0x024 && q[2] == 0x445, "custom base over a theme's accents");
	}

	printf("accent: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
