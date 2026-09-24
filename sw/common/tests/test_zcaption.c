/*
 * Host test for the caption renderer (sw/common/zcaption.c).
 *
 *   cc -std=gnu99 -Wall -DZCAPTION_HOST -I sw/common -o /tmp/tcap \
 *      sw/common/tests/test_zcaption.c sw/common/zcaption.c sw/common/zfont_data.c
 *   /tmp/tcap /tmp/cap        # also writes /tmp/cap-*.pbm to look at
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "zcaption.h"

#define W 20
#define H 144
static uint32_t bits[H + 1][W];

static int px(int x, int y) { return (bits[y][x >> 5] >> (x & 31)) & 1; }

static void dump(const char *base, const char *name, int w, int h) {
	char path[256];
	if (!base) return;
	snprintf(path, sizeof(path), "%s-%s.pbm", base, name);
	FILE *f = fopen(path, "w");
	fprintf(f, "P1\n%d %d\n", w, h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) fputc(px(x, y) ? '1' : '0', f);
		fputc('\n', f);
	}
	fclose(f);
}

static int render(const char *utf8, uint32_t o, int band, int *w, int *h) {
	char l9[Z_CAPTION_TEXT_MAX];
	z_caption_to_l9(utf8, l9, sizeof(l9));
	memset(bits, 0, sizeof(bits));
	return z_caption_render(l9, o, band, &bits[0][0], W, H, w, h);
}

int main(int argc, char **argv) {
	const char *base = argc > 1 ? argv[1] : NULL;
	int w, h;
	char l9[64];

	// Latin-9: a-umlaut, sharp s, euro; kana is the missing box.
	z_caption_to_l9("\xc3\xa4\xc3\x9f\xe2\x82\xac\xe3\x81\x82", l9, sizeof(l9));
	assert((uint8_t)l9[0] == 0xE4 && (uint8_t)l9[1] == 0xDF &&
	       (uint8_t)l9[2] == 0xA4 && (uint8_t)l9[3] == 0x7F && !l9[4]);

	// Full-width band: exactly the band, frame lit, 1 line at 3x.
	assert(render("Hello, Zeitlos", Z_CAPTION_SCALE(3), 624, &w, &h));
	assert(w == 624 && h == 36 + 2 * 8);
	assert(px(0, 0) && px(623, 0) && px(0, h - 1) && px(623, h - 1));
	assert(!px(1, 1));
	dump(base, "band3", w, h);

	// Same line count, same geometry: consecutive captions reuse pixels.
	int w2, h2;
	assert(render("Something else entirely", Z_CAPTION_SCALE(3), 624, &w2, &h2));
	assert(w2 == w && h2 == h);

	// Wrapping: 3x gives 33 columns in 624 -- this is three lines.
	assert(render("Zeitlos is a computer and an operating system, designed together.",
		Z_CAPTION_SCALE(3), 624, &w, &h));
	assert(h == 3 * 36 + 2 * 6 + 2 * 8);
	dump(base, "wrap3", w, h);

	// Explicit break, 2x, German.
	assert(render("Gr\xc3\xbc\xc3\x9f" "e aus Bayern!\nSch\xc3\xb6nen Tag.",
		Z_CAPTION_SCALE(2), 624, &w, &h));
	assert(h == 2 * 24 + 4 + 2 * 6);
	dump(base, "german2", w, h);

	// Compact, inverse.
	assert(render("Volume 40", Z_CAPTION_SCALE(2) | Z_CAPTION_COMPACT | Z_CAPTION_INVERSE,
		624, &w, &h));
	assert(w == 9 * 12 + 2 * 10);
	assert(px(1, 1));	// lit box
	dump(base, "compact", w, h);

	// Nothing to show.
	assert(!render("", 0, 624, &w, &h));
	assert(!render("   \n  ", 0, 624, &w, &h));

	// Never more than three lines, never taller than the bitmap.
	assert(render("a\nb\nc\nd\ne", Z_CAPTION_SCALE(3), 624, &w, &h));
	assert(h <= H && h == 3 * 36 + 2 * 6 + 2 * 8);

	printf("zcaption: all tests passed\n");
	return 0;
}
