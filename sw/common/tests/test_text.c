/*
 * Host tests for sw/common/zutf8.h (UTF-8 and ISO 8859-15) and the
 * font indexing in sw/common/zfont.h.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_text \
 *      sw/common/tests/test_text.c sw/common/zfont_data.c
 *   /tmp/test_text
 *
 * See docs/text_encoding.md.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "zutf8.h"
#include "zfont.h"
#include "zjfont.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

// Decodes one codepoint from a byte string of known length and reports
// how many bytes it consumed.
static uint32_t dec(const char *b, size_t len, int *used) {
	const char *s = b;
	uint32_t cp = z_utf8_next(&s, b + len);
	*used = (int)(s - b);
	return cp;
}

static void test_decode(void) {

	int u;

	CHECK(dec("A", 1, &u) == 'A' && u == 1, "ASCII");
	CHECK(dec("\xC3\xA4", 2, &u) == 0xE4 && u == 2, "a-umlaut");
	CHECK(dec("\xE2\x82\xAC", 3, &u) == 0x20AC && u == 3, "euro");
	CHECK(dec("\xE3\x81\x82", 3, &u) == 0x3042 && u == 3, "hiragana a");
	CHECK(dec("\xF0\x9F\x98\x80", 4, &u) == 0x1F600 && u == 4, "4-byte");
	CHECK(dec("\xEF\xBF\xBD", 3, &u) == 0xFFFD && u == 3, "real U+FFFD");

	// Malformed: each consumes exactly one byte.
	CHECK(dec("\xC3", 1, &u) == Z_UTF8_BAD && u == 1, "truncated 2-byte");
	CHECK(dec("\xE2\x82", 2, &u) == Z_UTF8_BAD && u == 1, "truncated 3-byte");
	CHECK(dec("\x80", 1, &u) == Z_UTF8_BAD && u == 1, "lone continuation");
	CHECK(dec("\xC3\x41", 2, &u) == Z_UTF8_BAD && u == 1, "bad continuation");
	CHECK(dec("\xC0\xAF", 2, &u) == Z_UTF8_BAD && u == 1, "overlong 2-byte");
	CHECK(dec("\xE0\x80\xAF", 3, &u) == Z_UTF8_BAD && u == 1, "overlong 3-byte");
	CHECK(dec("\xED\xA0\x80", 3, &u) == Z_UTF8_BAD && u == 1, "surrogate");
	CHECK(dec("\xF4\x90\x80\x80", 4, &u) == Z_UTF8_BAD && u == 1, "above U+10FFFF");
	CHECK(dec("\xFF", 1, &u) == Z_UTF8_BAD && u == 1, "0xFF");
	// Latin-1 text read as UTF-8: the classic mistake it has to catch.
	CHECK(dec("\xE4rger", 5, &u) == Z_UTF8_BAD && u == 1, "Latin-1 a-umlaut");

	// Never reads past end.
	const char two[] = "\xC3\xA4";
	CHECK(dec(two, 1, &u) == Z_UTF8_BAD && u == 1, "stops at end");

	// NUL-terminated form.
	const char *z = "\xC3\xA4x";
	CHECK(z_utf8_next_z(&z) == 0xE4 && *z == 'x', "z form");
	const char *z2 = "\xC3";
	CHECK(z_utf8_next_z(&z2) == Z_UTF8_BAD, "z form truncated at NUL");

	CHECK(z_utf8_valid("Gr\xC3\xBC\xC3\x9F" "e", 7), "valid");
	CHECK(!z_utf8_valid("Gr\xFC\xDF" "e", 5), "Latin-1 is not valid UTF-8");
	CHECK(z_utf8_valid("\xEF\xBF\xBD", 3), "real U+FFFD is valid");
	CHECK(z_utf8_count("Gr\xC3\xBC\xC3\x9F" "e", 7) == 5, "count");

}

static void test_encode(void) {

	// Every codepoint round-trips; spot-check the boundaries densely.
	int bad = 0;
	for (uint32_t cp = 0; cp <= 0x10FFFF; cp += (cp < 0x20000 ? 1 : 97)) {
		if (cp >= 0xD800 && cp < 0xE000) continue;
		char buf[Z_UTF8_MAX];
		int n = z_utf8_put(cp, buf);
		const char *s = buf;
		if (z_utf8_next(&s, buf + n) != cp || s != buf + n) bad++;
	}
	CHECK(bad == 0, "encode/decode round trip");

	char buf[Z_UTF8_MAX];
	CHECK(z_utf8_put(0xD800, buf) == 3 && (uint8_t)buf[0] == 0xEF, "surrogate encodes as U+FFFD");
	CHECK(z_utf8_put(0x110000, buf) == 3, "out of range encodes as U+FFFD");

}

static void test_latin9(void) {

	// Every byte 0x20-0x7E and 0xA0-0xFF round-trips through its
	// codepoint.
	for (int b = 0x20; b < 0x100; b++) {
		if (b >= 0x7F && b < 0xA0) continue;
		CHECK(z_cp_to_l9(z_l9_to_cp((uint8_t)b)) == b, "byte round trip");
	}

	CHECK(z_l9_to_cp(0xA4) == 0x20AC, "0xA4 is the euro");
	CHECK(z_cp_to_l9(0x20AC) == 0xA4, "euro is 0xA4");
	CHECK(z_cp_to_l9(0x00A4) == 0, "currency sign dropped from Latin-9");
	CHECK(z_cp_to_l9(0x0152) == 0xBC, "OE ligature");
	CHECK(z_cp_to_l9(0x00E4) == 0xE4, "a-umlaut");
	CHECK(z_cp_to_l9(0x0085) == 0, "C1 has no byte");
	CHECK(z_cp_to_l9(0x3042) == 0, "hiragana has no byte");

	// Conversions.
	char out[32];
	size_t lost;
	const char l9[] = "Gr\xFC\xDF" "e \xA4";			// "Grüße €"
	size_t n = z_l9_to_utf8(l9, strlen(l9), out, sizeof(out));
	CHECK(n == 11 && !strcmp(out, "Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC"), "Latin-9 to UTF-8");

	n = z_utf8_to_l9(out, n, out, sizeof(out), '?', &lost);
	CHECK(n == 7 && !memcmp(out, l9, 7) && lost == 0, "UTF-8 back to Latin-9");

	n = z_utf8_to_l9("a\xE3\x81\x82" "b", 5, out, sizeof(out), '?', &lost);
	CHECK(n == 3 && !strcmp(out, "a?b") && lost == 1, "unrepresentable counted");

	// Capacity: stops at a character boundary, always terminated.
	n = z_l9_to_utf8("\xE4\xE4\xE4", 3, out, 6);
	CHECK(n == 4 && out[4] == 0, "stops before a split character");

}

static void test_fonts(void) {

	const z_font_t *hw[] = { &z_font_5x8, &z_font_6x12 };

	for (int i = 0; i < 2; i++) {
		const z_font_t *f = hw[i];
		CHECK(z_font_glyph_count(f) == 192, "hardware font has 192 glyphs");
		CHECK(z_font_index(f, ' ') == 0, "space is glyph 0");
		CHECK(z_font_index(f, 0x7E) == 94, "tilde is glyph 94");
		CHECK(z_font_index(f, 0x7F) == 95, "DEL box is glyph 95");
		CHECK(z_font_index(f, 0x80) == -1, "C1 has no glyph");
		CHECK(z_font_index(f, 0x9F) == -1, "C1 has no glyph (end)");
		CHECK(z_font_index(f, 0xA0) == 96, "0xA0 follows DEL");
		CHECK(z_font_index(f, 0xFF) == 191, "0xFF is the last glyph");
		CHECK(z_font_index(f, 0x1F) == -1, "control has no glyph");

		// The euro sign and a-umlaut have ink; NBSP does not.
		int ink_eur = 0, ink_nbsp = 0, ink_box = 0;
		for (int r = 0; r < f->h; r++) {
			ink_eur  |= f->glyphs[z_font_index(f, 0xA4) * f->h + r];
			ink_nbsp |= f->glyphs[z_font_index(f, 0xA0) * f->h + r];
			ink_box  |= f->glyphs[z_font_index(f, Z_GLYPH_MISSING) * f->h + r];
		}
		CHECK(ink_eur, "euro glyph is blank");
		CHECK(!ink_nbsp, "no-break space has ink");
		CHECK(ink_box, "missing-glyph box is blank");
	}

	// Together they fill the font region of glyph memory exactly
	// (zgfx.c's glyph_layout[], Z_ICON_MEM_OFFSET = 3840).
	CHECK(z_font_glyph_count(&z_font_5x8) * 8 + z_font_glyph_count(&z_font_6x12) * 12 == 3840,
		"hardware fonts do not fill glyph memory exactly");

	// The software-only fonts are unchanged: ASCII, no gap.
	CHECK(z_font_glyph_count(&z_font_5x7) == 96, "5x7 is ASCII");
	CHECK(z_font_glyph_count(&z_font_8x16) == 96, "8x16 is ASCII");
	CHECK(z_font_index(&z_font_8x16, 0xE4) == -1, "8x16 has no Latin-9");

}

// -- finding jfont (zjfont.h's z_jfont_find()) --
//
// A fake process list over a real buffer laid out as jfont lays out its
// memory. Built -no-pie, so the buffer's address fits the 32-bit fields
// the real list has. This is the path the drawing code takes on the
// machine; the first version looked for the name "jfont", which the pid
// registry never reports, and drew every kanji as a box.

static uint32_t fake_block[1024];

static void test_jfont_find(void) {

	if ((uintptr_t)fake_block > 0xFFFFFFFFu) {
		printf("skip jfont lookup: needs -no-pie\n");
		return;
	}

	uint32_t base = (uint32_t)(uintptr_t)fake_block;
	// jfont's descriptor, somewhere past its code and data.
	z_jfont_desc_t *d = (z_jfont_desc_t *)&fake_block[300];
	uint32_t at = (uint32_t)(uintptr_t)d;
	d->len = 16;
	d->check = z_jfont_check(at);
	d->magic1 = Z_JFONT_MAGIC1;
	d->magic0 = Z_JFONT_MAGIC0;
	// A stray copy of the magic words elsewhere, which must not match.
	fake_block[10] = Z_JFONT_MAGIC0;
	fake_block[11] = Z_JFONT_MAGIC1;
	fake_block[12] = 0;

	z_proc_info_t p[2];
	memset(p, 0, sizeof(p));
	strcpy(p[0].name, "wm0");
	p[0].base = base; p[0].size = sizeof(fake_block);
	strcpy(p[1].name, Z_JFONT_NAME);
	p[1].base = base; p[1].size = sizeof(fake_block);

	uint32_t phys = 0;
	CHECK(z_jfont_find(p, 2, &phys) == d && phys == at, "found jfont's descriptor");

	// What z_proc_list() actually reports is the REGISTERED name.
	CHECK(!strcmp(Z_JFONT_NAME, "jfont0"), "the registered name is jfont0");
	strcpy(p[1].name, "jfont");
	CHECK(z_jfont_find(p, 2, &phys) == NULL, "a bare \"jfont\" is not what the list says");

	// Gone: a stale descriptor is not found.
	strcpy(p[1].name, Z_JFONT_NAME);
	d->check ^= 1;
	CHECK(z_jfont_find(p, 2, &phys) == NULL, "a broken descriptor is not found");

}

int main(void) {

	test_jfont_find();
	test_decode();
	test_encode();
	test_latin9();
	test_fonts();

	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;

}
