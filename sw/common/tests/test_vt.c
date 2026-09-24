/*
 * Host tests for sw/common/zvt100.c's UTF-8 decoding and the history
 * storage that holds whole Latin-9 bytes.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_vt \
 *      sw/common/tests/test_vt.c sw/common/zvt100.c
 *   /tmp/test_vt
 *
 * See zvt100.h, "characters", and docs/terminal.md, "UTF-8".
 */

#include <stdio.h>
#include <string.h>

#include "zvt100.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static vt_screen_t vt;
static uint8_t hist[40 * VT_HIST_LINE_BYTES];

static void feed(const char *s) {
	vt_feed(&vt, (const uint8_t *)s, (uint32_t)strlen(s));
}

static void reset(void) {
	vt_init(&vt);
	vt_history_attach(&vt, hist, 40);
}

static unsigned char cell(int row, int col) {
	return (unsigned char)vt.cells[row][col].ch;
}

int main(void) {

	// -- Latin-9 characters land as their glyph bytes --
	reset();
	feed("Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC");			// "Grüße €"
	CHECK(cell(0, 2) == 0xFC, "u-umlaut is byte 0xFC");
	CHECK(cell(0, 3) == 0xDF, "sharp s is byte 0xDF");
	CHECK(cell(0, 6) == 0xA4, "euro is Latin-9 byte 0xA4");
	CHECK(vt.cursor_x == 7, "seven characters, seven columns");

	// -- split across two feeds: bytes arrive one read at a time --
	reset();
	feed("\xC3");
	CHECK(vt.cursor_x == 0, "half a character writes nothing yet");
	feed("\xA9t\xC3");
	feed("\xA9");
	CHECK(cell(0, 0) == 0xE9 && cell(0, 1) == 't' && cell(0, 2) == 0xE9,
		"a character split across reads is decoded whole");

	// -- not in Latin-9: the box, one column --
	reset();
	feed("a\xE2\x80\x94" "b");								// em dash
	CHECK(cell(0, 1) == 0x7F && cell(0, 2) == 'b', "em dash is one box");
	feed("\xC2\xBD");										// one half: Latin-1, not Latin-9
	CHECK(cell(0, 3) == 0x7F, "one half is a box in Latin-9");

	// -- wide characters take two cells --
	reset();
	feed("\xE3\x81\x82" "x");								// hiragana a, then x
	CHECK(cell(0, 0) == 0x7F, "wide character's left half is the box");
	CHECK(cell(0, 1) == (unsigned char)VT_CH_WIDE_RIGHT, "right half marked");
	CHECK(cell(0, 2) == 'x' && vt.cursor_x == 3, "x lands in column 2, as the remote expects");

	// A wide character that would start in the last column wraps.
	reset();
	for (int i = 0; i < VT_COLS - 1; i++) feed("-");
	feed("\xE4\xB8\xAD");									// a CJK ideograph
	CHECK(cell(0, VT_COLS - 1) == ' ', "last column left blank");
	CHECK(cell(1, 0) == 0x7F && cell(1, 1) == (unsigned char)VT_CH_WIDE_RIGHT,
		"wide character wrapped whole onto the next line");

	// -- combining marks take no cell --
	reset();
	feed("e\xCC\x81" "x");									// e + combining acute
	CHECK(cell(0, 0) == 'e' && cell(0, 1) == 'x', "combining mark takes no column");

	// -- malformed input --
	reset();
	feed("a\xFF" "b");
	CHECK(cell(0, 1) == 0x7F && cell(0, 2) == 'b', "invalid byte is one box");
	reset();
	feed("\xC3" "b");										// lead then ASCII
	CHECK(cell(0, 0) == 0x7F && cell(0, 1) == 'b', "cut-short sequence: box, then the byte as itself");
	reset();
	feed("\xC0\xAF");										// overlong '/'
	CHECK(cell(0, 0) == 0x7F, "overlong form rejected");
	reset();
	feed("\xED\xA0\x80");									// surrogate
	CHECK(cell(0, 0) == 0x7F && vt.cursor_x == 1, "surrogate is one box");
	reset();
	feed("\xE2\x82\x1b[7mX");								// ESC mid-character
	CHECK(cell(0, 0) == 0x7F, "ESC ends a pending character");
	CHECK(cell(0, 1) == 'X' && vt.cells[0][1].reverse, "and still starts its escape sequence");
	reset();
	feed("\xC2\x85" "a");									// C1 NEL, as UTF-8
	CHECK(cell(0, 0) == 'a', "C1 controls are ignored, not drawn");

	// -- ASCII is exactly as before --
	reset();
	feed("hello\r\nworld");
	CHECK(cell(0, 0) == 'h' && cell(1, 4) == 'd', "plain ASCII");

	// -- history keeps whole Latin-9 bytes and reverse video --
	reset();
	feed("\x1b[7m\xC3\xA4\x1b[0m\xE2\x82\xAC\r\n");
	for (int i = 0; i < VT_ROWS; i++) feed("\r\n");			// push it off the top
	CHECK(vt_history_count(&vt) > 0, "line went into history");
	int doc = -1;
	for (int d = 0; d < (int)vt_history_count(&vt); d++)
		if (VT_PACK_CH(vt_doc_cell(&vt, d, 0)) == (char)0xE4) doc = d;
	CHECK(doc >= 0, "a-umlaut kept whole in history (was 7 bits)");
	if (doc >= 0) {
		vt_packed_t a = vt_doc_cell(&vt, doc, 0);
		vt_packed_t b = vt_doc_cell(&vt, doc, 1);
		CHECK(VT_PACK_REV(a), "reverse video kept in history");
		CHECK((uint8_t)VT_PACK_CH(b) == 0xA4 && !VT_PACK_REV(b), "euro kept, not reversed");
	}
	CHECK(VT_HIST_LINE_BYTES == VT_COLS + 20, "history line is 100 bytes");

	// -- wide characters keep their codepoint (phase 7b) --
	reset();
	feed("\xE6\x97\xA5\xE6\x9C\xAC");					// 日本
	CHECK(VT_PACK_WIDE_CP(vt_doc_cell(&vt, 0, 0)) == 0x65E5, "left cell keeps 日");
	CHECK(VT_PACK_WIDE_CP(vt_doc_cell(&vt, 0, 2)) == 0x672C, "left cell keeps 本");
	CHECK(VT_PACK_CH(vt_doc_cell(&vt, 0, 0)) == 0x7f, "glyph byte is still the box");
	CHECK(VT_PACK_WIDE_CP(vt_doc_cell(&vt, 0, 1)) == 0, "right half has no codepoint");
	// Scrolled into history, the codepoint survives.
	feed("\r\n");
	for (int i = 0; i < VT_ROWS; i++) feed("\r\n");
	int hd = -1;
	for (int d = 0; d < (int)vt_history_count(&vt); d++)
		if (VT_PACK_WIDE_CP(vt_doc_cell(&vt, d, 0)) == 0x65E5) hd = d;
	CHECK(hd >= 0, "日 kept in history");
	if (hd >= 0) {
		CHECK((uint8_t)VT_PACK_CH(vt_doc_cell(&vt, hd, 1)) == (uint8_t)VT_CH_WIDE_RIGHT,
			"history right half reads as the right half");
		CHECK(VT_PACK_WIDE_CP(vt_doc_cell(&vt, hd, 2)) == 0x672C, "本 kept in history");
		CHECK((uint8_t)VT_PACK_CH(vt_doc_cell(&vt, hd, 4)) == ' ', "then blank");
	}
	// Writing over the right half leaves a lone box, not a wide char.
	reset();
	feed("\xE6\x97\xA5\x1b[1;2Hx");						// 日, then x at column 2
	CHECK(VT_PACK_WIDE_CP(vt_doc_cell(&vt, 0, 0)) == 0, "half-overwritten pair loses its codepoint");
	CHECK(cell(0, 1) == 'x', "the overwriting character is there");

	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;

}
