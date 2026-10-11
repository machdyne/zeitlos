/*
 * Host test for the rescue console's terminal and keys (sw/os/rescue.c):
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_rescue \
 *      sw/os/tests/test_rescue.c sw/common/zfont_data.c && /tmp/test_rescue
 *
 * The screen is an array here, and the blitter's clear and scroll are
 * done in software; what is checked is where characters land, wrapping,
 * scrolling, the VT100 subset, and the key table.
 */

#include <stdio.h>
#include <string.h>

#define RESCUE_HOST_TEST
static unsigned char vram[640 * 480 / 8];
#define R_VRAM vram
#define Z_TICK_HZ 732

static void r_clear(int y, int h);
static void r_scroll(void);

#include "../rescue.c"

static void r_clear(int y, int h) {
	if (h > 0) memset(vram + y * R_STRIDE, 0, (size_t)h * R_STRIDE);
}

static void r_scroll(void) {
	memmove(vram, vram + 8 * R_STRIDE, (size_t)(r_rows * 8 - 8) * R_STRIDE);
	r_clear(r_rows * 8 - 8, 8);
}

static int failed, run;

#define CHECK(name, cond) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s (line %d)\n", name, __LINE__); } } while (0)

static void reset(void) {
	memset(vram, 0, sizeof(vram));
	r_col = r_row = 0;
	r_esc = 0;
	r_cur = false;
}

static void puts_r(const char *s) {
	while (*s) r_put((uint8_t)*s++);
}

// Does the cell hold character c (as r_glyph() draws it)?
static int cell_is(int col, int row, uint8_t c) {
	const z_font_t *f = &z_font_5x8;
	const uint8_t *g = f->glyphs + z_font_index(f, c) * f->h;
	for (int j = 0; j < 8; j++)
		if (vram[(row * 8 + j) * R_STRIDE + col] != r_rev8(g[j])) return 0;
	return 1;
}

static int cell_blank(int col, int row) {
	for (int j = 0; j < 8; j++)
		if (vram[(row * 8 + j) * R_STRIDE + col]) return 0;
	return 1;
}

int main(void) {

	// -- text --
	reset();
	puts_r("ls\r\n");
	CHECK("first char", cell_is(0, 0, 'l'));
	CHECK("second char", cell_is(1, 0, 's'));
	CHECK("crlf: next row", r_row == 1 && r_col == 0);

	// a full line wraps only when the next character comes
	reset();
	for (int i = 0; i < R_COLS; i++) r_put('x');
	CHECK("80 chars: still row 0", r_row == 0 && r_col == R_COLS);
	r_put('y');
	CHECK("81st wraps", r_row == 1 && r_col == 1 && cell_is(0, 1, 'y'));
	reset();
	for (int i = 0; i < R_COLS; i++) r_put('x');
	puts_r("\r\n");
	CHECK("80 chars then crlf: one row down", r_row == 1 && r_col == 0);

	// -- scrolling --
	reset();
	for (int i = 0; i < R_ROWS; i++) {
		r_put((uint8_t)('A' + i % 26));
		puts_r("\r\n");
	}
	CHECK("scrolled: at the bottom", r_row == R_ROWS - 1);
	CHECK("scrolled: row 0 holds the 2nd line", cell_is(0, 0, 'B'));
	CHECK("scrolled: last text row", cell_is(0, R_ROWS - 2, (uint8_t)('A' + (R_ROWS - 1) % 26)));
	CHECK("scrolled: bottom row blank", cell_blank(0, R_ROWS - 1));

	// -- readline's backspace: ESC[D, space, ESC[D --
	reset();
	puts_r("lsd");
	puts_r("\x1b[D \x1b[D");
	CHECK("bs: column back", r_col == 2);
	CHECK("bs: cell erased", cell_blank(2, 0));
	CHECK("bs: rest kept", cell_is(1, 0, 's'));
	r_put('\b');
	CHECK("ctrl-h", r_col == 1);
	puts_r("\x1b[5D");
	CHECK("left clamps at 0", r_col == 0);

	// -- CSI: home, move, erase --
	reset();
	puts_r("abc\x1b[3;10Hq");
	CHECK("ESC[3;10H", cell_is(9, 2, 'q') && r_row == 2 && r_col == 10);
	puts_r("\x1b[;H");
	CHECK("ESC[;H home", r_row == 0 && r_col == 0);
	r_col = 1;
	puts_r("\x1b[K");
	CHECK("ESC[K keeps the left", cell_is(0, 0, 'a'));
	CHECK("ESC[K clears the right", cell_blank(1, 0) && cell_blank(2, 0));
	puts_r("\x1b[3;1H\x1b[;H\x1b[2J");
	CHECK("ESC[2J clears", cell_blank(9, 2) && cell_blank(0, 0));
	puts_r("\x1b[31mz");
	CHECK("SGR ignored, text drawn", cell_is(0, 0, 'z') && r_col == 1);
	puts_r("\x1b" "c" "w");
	CHECK("ESC + other: dropped", cell_is(1, 0, 'w'));

	// -- tab, controls, high bytes --
	reset();
	puts_r("a\tb");
	CHECK("tab to 8", cell_is(8, 0, 'b'));
	reset();
	r_put(0x07);
	r_put(0x7f);
	CHECK("bell, DEL: nothing", r_col == 0 && cell_blank(0, 0));
	r_put(0x85);	// in the font's C1 gap
	CHECK("no glyph: drawn as ?", cell_is(0, 0, '?'));

	// -- the cursor --
	reset();
	r_cursor();
	CHECK("cursor on", vram[7 * R_STRIDE] == 0xFF && r_cur);
	r_cursor();
	CHECK("cursor off", vram[7 * R_STRIDE] == 0 && !r_cur);
	r_col = R_COLS;
	r_cursor();
	CHECK("cursor past the end: last cell", vram[7 * R_STRIDE + R_COLS - 1] == 0xFF);

	// -- composite video: a 40x30 grid --
	reset();
	r_cols = 40;
	r_rows = 30;
	for (int i = 0; i < 41; i++) r_put('x');
	CHECK("40 cols: wraps at 40", r_row == 1 && cell_is(0, 1, 'x') && cell_blank(40, 0));
	for (int i = 0; i < 40; i++) puts_r("\r\n");
	CHECK("30 rows: bottom at 29", r_row == 29);
	r_put('q');
	CHECK("30 rows: drawn on row 29", cell_is(0, 29, 'q') && cell_blank(0, 30));
	r_cols = R_COLS;
	r_rows = R_ROWS;

	// -- keys --
	CHECK("a", r_ascii(0x04, 0, false) == 'a');
	CHECK("shift a", r_ascii(0x04, 0x02, false) == 'A');
	CHECK("right shift z", r_ascii(0x1d, 0x20, false) == 'Z');
	CHECK("caps a", r_ascii(0x04, 0, true) == 'A');
	CHECK("caps+shift a", r_ascii(0x04, 0x02, true) == 'a');
	CHECK("caps 1 stays 1", r_ascii(0x1e, 0, true) == '1');
	CHECK("shift 1", r_ascii(0x1e, 0x02, false) == '!');
	CHECK("0", r_ascii(0x27, 0, false) == '0');
	CHECK("enter", r_ascii(0x28, 0, false) == '\r');
	CHECK("esc dropped", r_ascii(0x29, 0, false) == 0);
	CHECK("backspace", r_ascii(0x2a, 0, false) == '\b');
	CHECK("tab", r_ascii(0x2b, 0, false) == '\t');
	CHECK("space", r_ascii(0x2c, 0, false) == ' ');
	CHECK("minus/underscore", r_ascii(0x2d, 0, false) == '-' && r_ascii(0x2d, 2, false) == '_');
	CHECK("backslash/pipe", r_ascii(0x31, 0, false) == '\\' && r_ascii(0x31, 2, false) == '|');
	CHECK("quote", r_ascii(0x34, 0, false) == '\'' && r_ascii(0x34, 2, false) == '"');
	CHECK("slash/question", r_ascii(0x38, 0, false) == '/' && r_ascii(0x38, 2, false) == '?');
	CHECK("ctrl c", r_ascii(0x06, 0x01, false) == 3);
	CHECK("right ctrl d", r_ascii(0x07, 0x10, false) == 4);
	CHECK("delete", r_ascii(0x4c, 0, false) == 0x7f);
	CHECK("F1: nothing", r_ascii(0x3a, 0, false) == 0);
	CHECK("modifier usage: nothing", r_ascii(0xe1, 0x02, false) == 0);
	CHECK("table lengths", sizeof(r_lo) == 0x38 - 0x04 + 2 && sizeof(r_hi) == sizeof(r_lo));

	printf("test_rescue: %d checks, %d failed\n", run, failed);
	return failed != 0;
}
