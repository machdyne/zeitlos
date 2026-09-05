/*
 * Render sw/apps/hex's grid to an image.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/render \
 *      sw/apps/hex/tests/render.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c
 *   /tmp/render /tmp/hex.pbm [win_w win_h font]
 *
 * Needs -no-pie and vm.mmap_min_addr=0 -- see tests/trampoline.h.
 *
 * See sw/common/tests/zrender.h for what this does and what it cannot
 * catch. In short: hex.c, zwin.c and zwidget.c are the REAL sources;
 * only the pixel plotting is software.
 *
 * The synthetic file below matters as much as the harness. An idle
 * grid of zeros would hide exactly the things worth looking for -- a
 * character pane running into the scrollbar, a group gap landing in
 * the wrong place, the last row falling off the bottom -- so it is
 * filled with every byte value in turn, plus a run of readable text,
 * and the caret is parked mid-row rather than at 0.
 */

#include "../../../common/tests/zrender.h"
#include "trampoline.h"

#define main hex_main_unused
#include "../hex.c"
#undef main

// -- the file ----------------------------------------------------
//
// A stub filesystem holding one synthetic file, so the real
// cache_cover()/byte_at() paths run against real reads rather than
// being bypassed. The alignment and short-read behaviour of
// fs_read_chunk() is part of what this exercises.

#define STUB_SIZE  0x1F400u

static uint8_t stub[STUB_SIZE];
static uint32_t stub_pos;

static void stub_init(void) {

	for (uint32_t i = 0; i < STUB_SIZE; i++)
		stub[i] = (uint8_t)(i & 0xff);

	// A run of text, so the character pane has something to be right
	// or wrong about. Placed where the default view lands.
	const char *s = "zeitlos hex editor -- the quick brown fox jumps!";
	for (int i = 0; s[i]; i++) stub[0x40 + i] = (uint8_t)s[i];

	// A run of zeros and a run of 0xFF: the two extremes the character
	// pane collapses to '.', worth seeing next to real text.
	for (int i = 0; i < 16; i++) stub[0x20 + i] = 0x00;
	for (int i = 0; i < 16; i++) stub[0x30 + i] = 0xff;

}

int fs_size(char *name) { (void)name; return (int)STUB_SIZE; }
int fs_open_rw(const char *name) { (void)name; stub_pos = 0; return 3; }
int fs_open_read(const char *name) { (void)name; stub_pos = 0; return 3; }
int fs_close_handle(int h) { (void)h; return 1; }
int fs_sync(int h) { (void)h; return 1; }
int fs_truncate(int h, uint32_t n) { (void)h; (void)n; return 1; }
int fs_touch(const char *p) { (void)p; return 1; }
bool fs_df(uint32_t *t, uint32_t *f) {
	if (t) *t = 1u << 20;
	if (f) *f = 1u << 20;
	return true;
}
int fs_write_chunk(int h, const void *b, int n) { (void)h; (void)b; return n; }

int fs_seek(int h, uint32_t off) {
	(void)h;
	stub_pos = off > STUB_SIZE ? STUB_SIZE : off;
	return 1;
}

int fs_read_chunk(int h, void *buf, int maxlen) {
	(void)h;
	uint32_t left = STUB_SIZE - stub_pos;
	uint32_t n = (uint32_t)maxlen < left ? (uint32_t)maxlen : left;
	memcpy(buf, &stub[stub_pos], n);
	stub_pos += n;
	return (int)n;
}

// -- everything that would need a window or a message ------------
//
// Do-nothing on purpose: this renders geometry, and anything that
// takes a round trip to wm is not geometry.

bool z_dialog_open(const z_dialog_ctx_t *c, const char *d, char *o, int n) {
	(void)c; (void)d; (void)o; (void)n; return false; }
bool z_dialog_save(const z_dialog_ctx_t *c, const char *d, const char *s,
	char *o, int n) { (void)c; (void)d; (void)s; (void)o; (void)n; return false; }
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m,
	int b) { (void)c; (void)t; (void)m; (void)b; return 0; }
bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m,
	const char *i, char *o, int n) {
	(void)c; (void)t; (void)m; (void)i; (void)o; (void)n; return false; }

int main(int argc, char **argv) {

	const char *out = argc > 1 ? argv[1] : "/tmp/hex.pbm";
	int w = argc > 2 ? atoi(argv[2]) : WIN_W;
	int h = argc > 3 ? atoi(argv[3]) : WIN_H;
	int big = argc > 4 ? atoi(argv[4]) : 0;

	if (!z_render_open(&win, w, h)) {
		printf("render: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	// The edits set up below send messages (put_byte() retitles the
	// window), so the runtime's fixed syscall pointer has to be real.
	if (!z_tramp_install()) {
		printf("render: skipped\n");
		return 77;
	}

	stub_init();

	if (big) cur_font = &z_font_6x12;

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	fh = 3;
	fsize = STUB_SIZE;
	read_only = false;
	cache_drop();

	layout();

	// Park the caret somewhere with text around it and a few rows down
	// -- at offset 0 the caret sits in the corner and proves nothing
	// about how it reads against neighbouring columns.
	cursor = 0x48;
	top_row = 0;

	// A few pending edits, and a half-typed byte, so the render shows
	// the states the grid actually spends its time in rather than a
	// pristine read-only view. There is nothing visually distinguishing
	// an edited byte from any other -- that is deliberate, the file is
	// what it is -- so what this checks is that the edits show at all
	// and that the status line reflects them.
	put_byte(0x44, 0xDE, false);
	put_byte(0x45, 0xAD, false);
	put_byte(0x62, 'X', false);

	nibble_low = true;

	scroll_to_cursor();

	repaint();

	z_render_write(out, &win, 2);

	printf("render: %dx%d window, %d bytes/row, %d rows, font %dx%d\n",
		w, h, bytes_per_row, rows, cur_font->w, cur_font->h);

	return 0;

}
