/*
 * Host tests for sw/common/zvt100.c's UTF-8 decoding, the history
 * storage that holds whole Latin-9 bytes, and what a BBS or a curses
 * program needs: private modes, save/restore, insert/delete character,
 * the reports it asks for, character sets, and line drawing.
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

	// ================================================================
	// What a BBS or a curses program sends (docs/terminal.md)
	// ================================================================

	// -- private modes: ESC[?25l hides the cursor and prints nothing --
	reset();
	feed("a\x1b[?25lb");
	CHECK(cell(0, 0) == 'a' && cell(0, 1) == 'b' && cell(0, 2) == ' ',
		"CSI ? 25 l prints nothing (it used to print \"25l\")");
	CHECK(!vt_cursor_visible(&vt), "... and hides the cursor");
	feed("\x1b[?25h");
	CHECK(vt_cursor_visible(&vt), "CSI ? 25 h shows it again");
	feed("\x1b[?1049h\x1b[?1;25;7l");
	CHECK(!vt_cursor_visible(&vt) && vt.cursor_x == 2 && cell(0, 2) == ' ',
		"several private modes at once; the unknown ones absorbed");
	feed("\x1b[?6n");
	CHECK(vt.reply_len == 0, "a private final is not the ANSI one (CSI ? 6 n is not CSI 6 n)");
	feed("\x1b[?25h");
	feed("\x1b[ q\x1b[!p" "c");
	CHECK(cell(0, 2) == 'c', "intermediate bytes (CSI SP q, CSI ! p) are absorbed whole");

	// -- save and restore --
	reset();
	feed("\x1b[5;10H\x1b[7m\x1b" "7\x1b[1;1H\x1b[0mX\x1b" "8Y");
	CHECK(cell(4, 9) == 'Y' && vt.cells[4][9].reverse, "ESC 7 / ESC 8: position and reverse video back");
	CHECK(cell(0, 0) == 'X' && !vt.cells[0][0].reverse, "(written in between, not reverse)");
	reset();
	feed("\x1b[3;4H\x1b[s\x1b[20;70H\x1b[uZ");
	CHECK(cell(2, 3) == 'Z', "CSI s / CSI u");
	reset();
	feed("\x1b[10;10H\x1b" "8Q");
	CHECK(cell(0, 0) == 'Q', "restore with nothing saved: home");

	// -- insert, delete, erase characters --
	reset();
	feed("abcdef\x1b[1;3H\x1b[2@");
	CHECK(cell(0, 0) == 'a' && cell(0, 1) == 'b' && cell(0, 2) == ' ' && cell(0, 3) == ' ' &&
		cell(0, 4) == 'c' && cell(0, 7) == 'f', "ICH: two blanks in, the rest moves right");
	feed("\x1b[3P");
	CHECK(cell(0, 2) == 'd' && cell(0, 4) == 'f' && cell(0, 5) == ' ', "DCH: three out, the line closes up");
	feed("\x1b[1;1H\x1b[2X");
	CHECK(cell(0, 0) == ' ' && cell(0, 1) == ' ' && cell(0, 2) == 'd', "ECH: erased where it stands");
	reset();
	feed("\x1b[1;80HZ\x1b[1;79H\x1b[5@");
	CHECK(cell(0, 78) == ' ' && cell(0, 79) == ' ', "ICH near the edge: what is pushed off is gone");
	reset();
	for (int i = 0; i < VT_COLS; i++) feed("x");
	feed("\x1b[1P");
	CHECK(cell(0, 79) == ' ' && cell(0, 78) == 'x', "DCH with the cursor past the last column (deferred wrap)");

	// -- columns and rows --
	reset();
	feed("\x1b[5;5H\x1b[20GA\x1b[12dB\x1b[2EC\x1b[3FD\x1b[7`E");
	CHECK(cell(4, 19) == 'A', "CHA: column 20");
	CHECK(cell(11, 20) == 'B', "VPA: row 12, column kept");
	CHECK(cell(13, 0) == 'C', "CNL: two down, column 1");
	CHECK(cell(10, 0) == 'D', "CPL: three up, column 1");
	CHECK(cell(10, 6) == 'E', "HPA: column 7");
	feed("\x1b[999G\x1b[999d");
	CHECK(vt.cursor_x == VT_COLS - 1 && vt.cursor_y == VT_ROWS - 1, "clamped to the screen");

	// -- the reports a BBS asks for --
	uint8_t r[64];
	uint32_t rn;
	reset();
	feed("\x1b[12;34H\x1b[6n");
	rn = vt_take_reply(&vt, r, sizeof(r));
	CHECK(rn == 8 && !memcmp(r, "\x1b[12;34R", 8), "CSI 6 n: where the cursor is, 1-indexed");
	CHECK(vt_take_reply(&vt, r, sizeof(r)) == 0, "taken once");
	feed("\x1b[5n\x1b[c\x1b[0c");
	rn = vt_take_reply(&vt, r, sizeof(r));
	CHECK(rn == 4 + 7 + 7 && !memcmp(r, "\x1b[0n\x1b[?1;0c\x1b[?1;0c", rn),
		"CSI 5 n: OK; CSI c: a VT100 with no options");
	reset();
	for (int i = 0; i < VT_COLS; i++) feed("x");
	feed("\x1b[6n");
	rn = vt_take_reply(&vt, r, sizeof(r));
	CHECK(rn == 7 && !memcmp(r, "\x1b[1;80R", 7), "past the last column reports the last column");
	reset();
	for (int i = 0; i < 10; i++) feed("\x1b[25;80H\x1b[6n");
	CHECK(vt.reply_len <= VT_REPLY_MAX, "nobody taking replies: bounded, never overflows");
	rn = vt_take_reply(&vt, r, 3);
	CHECK(rn == 3 && vt.reply_len > 0, "taken a little at a time");
	while (vt_take_reply(&vt, r, sizeof(r))) ;
	feed("\x1b[>c");
	CHECK(vt.reply_len == 0 && vt.cursor_x == VT_COLS - 1, "secondary DA (CSI > c): absorbed, no answer");

	// -- controls inside an escape sequence --
	reset();
	feed("ab\x1b[1\r0Cx");
	CHECK(cell(0, 10) == 'x', "a CR inside a CSI is obeyed, and the CSI goes on");
	reset();
	feed("\x1b[5\x1b[3Cy");
	CHECK(cell(0, 3) == 'y', "an ESC abandons the sequence and starts a new one");
	reset();
	feed("\x1b[5\x18z");
	CHECK(cell(0, 0) == 'z', "CAN cancels it");

	// -- ESC D, E, M; CSI S, T --
	reset();
	feed("top\x1b[1;1H\x1bMnew");
	CHECK(cell(0, 0) == 'n' && cell(1, 0) == 't', "RI at the top scrolls down");
	CHECK(vt_history_count(&vt) == 0, "... and saves nothing");
	feed("\x1b[25;1Hlast\x1b" "D");
	CHECK(cell(23, 0) == 'l' && cell(24, 0) == ' ' && vt_history_count(&vt) == 1,
		"IND at the bottom scrolls up, like a linefeed, into history");
	reset();
	feed("A\x1b[3;5H\x1b" "EB");
	CHECK(cell(3, 0) == 'B', "NEL: next line, column 1");
	reset();
	feed("one\r\ntwo\x1b[2S");
	CHECK(cell(0, 0) != 'o' && vt_history_count(&vt) == 0, "SU scrolls up, and saves nothing (not a linefeed)");
	reset();
	feed("one\x1b[2T");
	CHECK(cell(2, 0) == 'o' && cell(0, 0) == ' ', "SD scrolls down");

	// -- ESC c: a full reset keeps the scrollback --
	reset();
	for (int i = 0; i < 30; i++) feed("line\r\n");
	uint16_t hc = vt_history_count(&vt);
	feed("\x1b[7m\x1b[?25l\x1b(0\x1b" "c");
	CHECK(vt.cursor_x == 0 && vt.cursor_y == 0 && cell(0, 0) == ' ' && !vt.reverse &&
		vt_cursor_visible(&vt) && vt.g0 == VT_CS_ASCII, "RIS: screen, cursor and modes reset");
	CHECK(vt_history_count(&vt) == hc, "... the scrollback kept");

	// -- line drawing from UTF-8 --
	reset();
	feed("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90");                   // ┌─┐
	CHECK(cell(0, 0) == VT_BOX_DR && cell(0, 1) == VT_BOX_H && cell(0, 2) == VT_BOX_DL,
		"┌─┐ are line-drawing codes, one column each");
	feed("\xE2\x95\x94\xE2\x95\x90\xE2\x95\x97");                   // ╔═╗
	CHECK(cell(0, 3) == VT_BOX_DR2 && cell(0, 4) == VT_BOX_H2 && cell(0, 5) == VT_BOX_DL2, "╔═╗ double");
	feed("\xE2\x96\x88\xE2\x96\x80\xE2\x96\x84\xE2\x96\x8C\xE2\x96\x90\xE2\x96\x91\xE2\x96\x92\xE2\x96\x93");
	CHECK(cell(0, 6) == VT_BOX_FULL && cell(0, 7) == VT_BOX_UPPER && cell(0, 8) == VT_BOX_LOWER &&
		cell(0, 9) == VT_BOX_LEFT && cell(0, 10) == VT_BOX_RIGHT && cell(0, 11) == VT_BOX_SHADE1 &&
		cell(0, 12) == VT_BOX_SHADE2 && cell(0, 13) == VT_BOX_SHADE3, "blocks and shades");
	CHECK(vt.cursor_x == 14, "every one of them one column");
	CHECK(vt_box_from_cp(0x2501) == VT_BOX_H && vt_box_from_cp(0x254B) == VT_BOX_VH &&
		vt_box_from_cp(0x2513) == VT_BOX_DL && vt_box_from_cp(0x2521) == VT_BOX_VR,
		"heavy lines drawn light");
	CHECK(vt_box_from_cp(0x2552) == VT_BOX_DR2 && vt_box_from_cp(0x2556) == VT_BOX_DL2 &&
		vt_box_from_cp(0x256A) == VT_BOX_VH2 && vt_box_from_cp(0x2562) == VT_BOX_VL2,
		"mixed single/double drawn double");
	CHECK(vt_box_from_cp(0x256D) == VT_BOX_DR && vt_box_from_cp(0x2574) == VT_BOX_H &&
		vt_box_from_cp(0x2577) == VT_BOX_V && vt_box_from_cp(0x2504) == VT_BOX_H &&
		vt_box_from_cp(0x254E) == VT_BOX_V, "rounded, half and dashed lines");
	CHECK(vt_box_from_cp(0x2571) == 0 && vt_box_from_cp(0x2581) == 0 && vt_box_from_cp(0x25A0) == 0 &&
		vt_box_from_cp('-') == 0, "diagonals, partial blocks, others: none");
	feed("\xE2\x95\xB1");
	CHECK(cell(0, 14) == 0x7f, "... shown as the missing-glyph box");
	// every code from U+2500 to U+2593 lands on a code or on nothing -- never out of range
	{
		int bad = 0;
		for (uint32_t cp = 0x2400; cp < 0x2700; cp++) {
			uint8_t b = vt_box_from_cp(cp);
			if (b && !vt_is_box(b)) bad++;
		}
		CHECK(!bad, "every mapping lands in the code range");
	}

	// -- and back, for copying and speech --
	CHECK(vt_glyph_cp(VT_BOX_DR) == 0x250C && vt_glyph_cp(VT_BOX_VH2) == 0x256C &&
		vt_glyph_cp(VT_BOX_SHADE3) == 0x2593, "codes back to their characters");
	{
		int bad = 0;
		for (int b = VT_BOX_FIRST; b <= VT_BOX_LAST; b++)
			if (vt_box_from_cp(vt_glyph_cp((uint8_t)b)) != b) bad++;
		CHECK(!bad, "every code round-trips through its character");
	}
	CHECK(vt_glyph_cp(0xFC) == 0xFC && vt_glyph_cp(0xA4) == 0x20AC && vt_glyph_cp('A') == 'A',
		"Latin-9 bytes to their characters (0xA4 is the euro)");
	CHECK(vt_glyph_cp(0x7f) == 0xFFFD && vt_glyph_cp((uint8_t)VT_CH_WIDE_RIGHT) == 0,
		"the box and a wide right half");

	// -- line drawing survives history --
	reset();
	feed("\xE2\x95\x9A\xE2\x96\x92\r\n");
	for (int i = 0; i < VT_ROWS; i++) feed("\r\n");
	CHECK((uint8_t)VT_PACK_CH(vt_doc_cell(&vt, 0, 0)) == VT_BOX_UR2 &&
		(uint8_t)VT_PACK_CH(vt_doc_cell(&vt, 0, 1)) == VT_BOX_SHADE2, "into history and back");

	// -- DEC special graphics: ESC ( 0, and ESC ) 0 with SO/SI --
	reset();
	feed("\x1b(0lqk\x1b(Bq");
	CHECK(cell(0, 0) == VT_BOX_DR && cell(0, 1) == VT_BOX_H && cell(0, 2) == VT_BOX_DL && cell(0, 3) == 'q',
		"ESC ( 0: l q k are a box top; ESC ( B: q is q again");
	reset();
	feed("\x1b)0x\x0ex\x0fx");
	CHECK(cell(0, 0) == 'x' && cell(0, 1) == VT_BOX_V && cell(0, 2) == 'x', "ESC ) 0 then SO / SI");
	reset();
	feed("\x1b(0fg}~a_");
	CHECK(cell(0, 0) == 0xB0 && cell(0, 1) == 0xB1 && cell(0, 2) == 0xA3 && cell(0, 3) == 0xB7 &&
		cell(0, 4) == VT_BOX_SHADE2 && cell(0, 5) == ' ', "degree, plus-minus, pound, dot, checker, blank");
	feed("A1");
	CHECK(cell(0, 6) == 'A' && cell(0, 7) == '1', "outside 0x5F-0x7E, unchanged");
	reset();
	feed("\x1b(0\x1b" "7\x1b(B\x1b" "8q");
	CHECK(cell(0, 0) == VT_BOX_H, "the character set is saved and restored with the cursor");

	// -- the glyphs --
	{
		uint8_t g[12];
		// single: a line through the middle to every edge it leaves by
		vt_box_glyph(VT_BOX_H, 5, 8, 0, 0, g);
		CHECK(g[3] == 0xF8 && g[2] == 0 && g[4] == 0, "─ at 5x8: row 3, all five columns");
		vt_box_glyph(VT_BOX_V, 5, 8, 0, 0, g);
		{ int ok = 1; for (int y = 0; y < 8; y++) if (g[y] != 0x20) ok = 0;
		  CHECK(ok, "│ at 5x8: column 2, every row"); }
		vt_box_glyph(VT_BOX_DR, 5, 8, 0, 0, g);
		CHECK(g[0] == 0 && g[2] == 0 && g[3] == 0x38 && g[4] == 0x20 && g[7] == 0x20,
			"┌: right from the middle, then down to the bottom edge");
		vt_box_glyph(VT_BOX_VH, 6, 12, 0, 0, g);
		CHECK(g[5] == 0xFC && g[0] == 0x20 && g[11] == 0x20, "┼ at 6x12: both lines, edge to edge");
		// double: two lines, meeting properly
		vt_box_glyph(VT_BOX_H2, 5, 8, 0, 0, g);
		CHECK(g[2] == 0xF8 && g[3] == 0 && g[4] == 0xF8, "═: rows 2 and 4, nothing between");
		vt_box_glyph(VT_BOX_V2, 5, 8, 0, 0, g);
		{ int ok = 1; for (int y = 0; y < 8; y++) if (g[y] != 0x50) ok = 0;
		  CHECK(ok, "║: columns 1 and 3, every row"); }
		vt_box_glyph(VT_BOX_DR2, 5, 8, 0, 0, g);
		// outer line: row 2 from column 1, column 1 from row 2; inner: row 4 from 3, column 3 from 4
		CHECK(g[0] == 0 && g[1] == 0 && g[2] == 0x78 && g[3] == 0x40 && g[4] == 0x58 &&
			g[5] == 0x50 && g[7] == 0x50, "╔: outer and inner corners, each turning where it should");
		vt_box_glyph(VT_BOX_VH2, 5, 8, 0, 0, g);
		CHECK(g[2] == 0xD8 && g[3] == 0 && g[4] == 0xD8 && g[0] == 0x50 && g[7] == 0x50,
			"╬: four corners, the middle open");
		vt_box_glyph(VT_BOX_VR2, 5, 8, 0, 0, g);
		CHECK(g[0] == 0x50 && g[2] == 0x58 && g[3] == 0x40 && g[4] == 0x58 && g[7] == 0x50,
			"╠: the left line unbroken, the right one opens to the arm");
		// blocks tile; shades follow the screen
		uint8_t a[8], b[8];
		vt_box_glyph(VT_BOX_LEFT, 5, 8, 0, 0, a);
		vt_box_glyph(VT_BOX_RIGHT, 5, 8, 0, 0, b);
		{ int ok = 1; for (int y = 0; y < 8; y++) if ((a[y] | b[y]) != 0xF8 || (a[y] & b[y])) ok = 0;
		  CHECK(ok, "▌ and ▐ are exact halves of █ at an odd width"); }
		vt_box_glyph(VT_BOX_UPPER, 5, 8, 0, 0, a);
		vt_box_glyph(VT_BOX_LOWER, 5, 8, 0, 0, b);
		CHECK(a[3] == 0xF8 && a[4] == 0 && b[3] == 0 && b[4] == 0xF8, "▀ and ▄ halves");
		vt_box_glyph(VT_BOX_SHADE2, 5, 8, 0, 0, a);
		vt_box_glyph(VT_BOX_SHADE2, 5, 8, 1, 0, b);
		CHECK(a[0] == 0xA8 && b[0] == 0x50 && a[1] == 0x50, "▒ checker, shifted for an odd screen x");
		// two ▒ cells side by side at 5 wide: the second starts at an odd x
		CHECK(((a[0] >> 3) & 1) != ((b[0] >> 7) & 1), "... so it continues the checker across the seam");
		vt_box_glyph(VT_BOX_SHADE1, 6, 12, 0, 0, a);
		vt_box_glyph(VT_BOX_SHADE3, 6, 12, 0, 0, b);
		CHECK(a[0] == 0xA8 && a[1] == 0 && b[0] == 0xFC && b[1] == 0xA8, "░ a quarter, ▓ three quarters");
		vt_box_glyph('A', 5, 8, 0, 0, a);
		CHECK(!a[0] && !a[3], "not a code: nothing");
	}

	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;

}
