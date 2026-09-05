/*
 * Layout assertions for sw/apps/hex.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_layout \
 *      sw/apps/hex/tests/test_layout.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c
 *   /tmp/test_layout
 *
 * This does NOT replace tests/render.c, and the render does not
 * replace this. A render catches the relationships nobody thought to
 * write down -- text wider than its well, two things two pixels apart
 * that are legal and look wrong. This catches the ones that ARE known
 * and would otherwise only be noticed on hardware, across far more
 * window sizes than anyone will sit and look at.
 *
 * The one worth having above all others is the hit-test round trip.
 * Drawing and clicking are two walks over the same cell geometry, and
 * the failure when they disagree is not a crash -- it is a grid where
 * clicking a byte selects its neighbour, which is the kind of thing
 * that gets lived with and worked around rather than reported.
 */

#include "../../../common/tests/zrender.h"

#define main hex_main_unused
#include "../hex.c"
#undef main

// -- stubs -------------------------------------------------------
//
// Nothing here draws or reads, so these only have to link.

int fs_size(char *n) { (void)n; return 0; }
int fs_open_rw(const char *n) { (void)n; return -1; }
int fs_open_read(const char *n) { (void)n; return -1; }
int fs_close_handle(int h) { (void)h; return 1; }
int fs_sync(int h) { (void)h; return 1; }
int fs_truncate(int h, uint32_t s) { (void)h; (void)s; return 1; }
int fs_touch(const char *p) { (void)p; return 1; }
bool fs_df(uint32_t *t, uint32_t *f) {
	if (t) *t = 1u << 20;
	if (f) *f = 1u << 20;
	return true;
}
int fs_seek(int h, uint32_t o) { (void)h; (void)o; return 1; }
int fs_read_chunk(int h, void *b, int m) { (void)h; (void)b; (void)m; return 0; }
int fs_write_chunk(int h, const void *b, int n) { (void)h; (void)b; return n; }

bool z_dialog_open(const z_dialog_ctx_t *c, const char *d, char *o, int n) {
	(void)c; (void)d; (void)o; (void)n; return false; }
bool z_dialog_save(const z_dialog_ctx_t *c, const char *d, const char *s,
	char *o, int n) { (void)c; (void)d; (void)s; (void)o; (void)n; return false; }
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m,
	int b) { (void)c; (void)t; (void)m; (void)b; return 0; }
bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m,
	const char *i, char *o, int n) {
	(void)c; (void)t; (void)m; (void)i; (void)o; (void)n; return false; }

// -- harness -----------------------------------------------------

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while (0)

// Every window size worth trying: the smallest wm will allow, the
// default, the largest that fits the screen, and awkward sizes in
// between -- odd widths especially, since a cell grid divided into a
// window is exactly where a rounding error hides.
static const int widths[] = {
	64, 100, 137, 156, 180, 231, 260, 313, 408, 409, 477, 500, 573, 640
};
static const int heights[] = { 40, 80, 160, 220, 280, 333, 480 };

static void check_one(int w, int h, const z_font_t *f) {

	cur_font = f;

	memset(&win, 0, sizeof(win));
	win.id = 1;
	win.x = 0;
	win.y = 0;
	win.w = w;
	win.h = h;

	layout();

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	// -- the grid fits the space it was given --

	CHECK(bytes_per_row >= 4, "%dx%d f%d: bpr %d below floor",
		w, h, f->w, bytes_per_row);

	CHECK((bytes_per_row & (bytes_per_row - 1)) == 0,
		"%dx%d f%d: bpr %d is not a power of two -- the address column's "
		"low digit stops naming the row position", w, h, f->w, bytes_per_row);

	// Only meaningful once the window is wide enough to hold the
	// minimum grid at all; below that the grid clips, by design.
	int need = cells_needed(bytes_per_row) * CHAR_W + 2 * MARGIN;

	if (cells_needed(4) * CHAR_W + 2 * MARGIN <= text_w)
		CHECK(need <= text_w,
			"%dx%d f%d: %d bytes/row needs %dpx of %dpx",
			w, h, f->w, bytes_per_row, need, text_w);

	// -- the grid fits the space vertically --

	CHECK(rows >= 1, "%dx%d f%d: %d rows", w, h, f->w, rows);

	if (ch >= hdr_h + status_h + LINE_H)
		CHECK(hdr_h + rows * LINE_H + status_h <= ch,
			"%dx%d f%d: header %d + %d rows x %d + status %d exceeds %d",
			w, h, f->w, hdr_h, rows, LINE_H, status_h, ch);

	// -- the scrollbar leaves the resize grip clickable --
	//
	// A scrollbar running the full height swallows the grip and the
	// window can never be resized again, which is unrecoverable
	// without restarting the app. See Z_WIN_GRIP_INSET in zwm.h.

	CHECK(sbar.x + Z_SB_THICK <= cw,
		"%dx%d f%d: scrollbar at %d+%d overruns content width %d",
		w, h, f->w, sbar.x, Z_SB_THICK, cw);

	if (ch > Z_WIN_GRIP_INSET)
		CHECK(sbar.len <= ch - Z_WIN_GRIP_INSET,
			"%dx%d f%d: scrollbar length %d covers the resize grip",
			w, h, f->w, sbar.len);

	// -- columns do not overlap --

	for (int i = 1; i < bytes_per_row; i++)
		CHECK(cell_byte_x(i) >= cell_byte_x(i - 1) + 3,
			"%dx%d f%d: hex columns %d and %d overlap",
			w, h, f->w, i - 1, i);

	CHECK(cell_ascii_x() >= cell_byte_x(bytes_per_row - 1) + 2,
		"%dx%d f%d: character pane starts inside the hex pane", w, h, f->w);

	CHECK(cell_hex_x() >= ADDR_CELLS,
		"%dx%d f%d: hex pane starts inside the address column", w, h, f->w);

	// -- the hit test agrees with the drawing --
	//
	// For every byte on a row, the pixel where its hex digits and its
	// character are DRAWN must map back to that same byte. This is the
	// assertion that matters: the two walks are separate code, and when
	// they drift the symptom is clicking one byte and selecting
	// another.

	fsize = 0x10000;
	top_row = 0;

	for (int r = 0; r < rows; r++) {
		for (int i = 0; i < bytes_per_row; i++) {

			int want = r * bytes_per_row + i;
			int y = row_y(r) + LINE_H / 2;
			bool ascii;

			// Both cells of the hex pair, not just the first -- the
			// pair is two cells wide and clicking either must work.
			for (int d = 0; d < 2; d++) {
				int x = cell_px(cell_byte_x(i) + d) + CHAR_W / 2;
				if (x >= text_w) continue;
				int32_t got = hit_cell(x, y, &ascii);
				CHECK(got == want,
					"%dx%d f%d: hex cell r%d b%d digit %d hit %d, wanted %d",
					w, h, f->w, r, i, d, (int)got, want);
				CHECK(!ascii, "%dx%d f%d: hex cell r%d b%d reported as character pane",
					w, h, f->w, r, i);
			}

			int x = cell_px(cell_ascii_x() + i) + CHAR_W / 2;
			if (x >= text_w) continue;
			int32_t got = hit_cell(x, y, &ascii);
			CHECK(got == want,
				"%dx%d f%d: character cell r%d b%d hit %d, wanted %d",
				w, h, f->w, r, i, (int)got, want);
			CHECK(ascii, "%dx%d f%d: character cell r%d b%d reported as hex pane",
				w, h, f->w, r, i);

		}
	}

	// -- the header is not part of the grid --
	//
	// A click on the column labels must not select a byte. It reads as
	// a row of numbers like any other and is the obvious thing to click
	// on by mistake.

	for (int i = 0; i < bytes_per_row; i++) {
		bool ascii;
		int x = cell_px(cell_byte_x(i)) + CHAR_W / 2;
		if (x >= text_w) continue;
		CHECK(hit_cell(x, 0, &ascii) < 0,
			"%dx%d f%d: clicking the column header selected a byte",
			w, h, f->w);
	}

}

// cells_needed() has to be monotonic in bytes_per_row, or layout()'s
// widening loop can stop at a width that fits while a wider one also
// would -- and the symptom is a window that shows 8 columns with room
// for 16, which reads as a layout bug rather than an arithmetic one.
static void check_monotonic(void) {

	int prev = 0;

	for (int bpr = 4; bpr <= 32; bpr <<= 1) {
		int n = cells_needed(bpr);
		CHECK(n > prev, "cells_needed(%d) = %d is not greater than %d",
			bpr, n, prev);
		prev = n;
	}

}

int main(void) {

	// Only for the VRAM mapping zwin.c's clip handling touches; nothing
	// here draws.
	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("test_layout: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	check_monotonic();

	int n = 0;

	for (unsigned wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++)
		for (unsigned hi = 0; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
			check_one(widths[wi], heights[hi], &z_font_5x8);
			check_one(widths[wi], heights[hi], &z_font_6x12);
			n += 2;
		}

	printf("test_layout: %d configurations, %d failures\n", n, failures);

	return failures ? 1 : 0;

}
