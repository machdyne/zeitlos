/*
 * hex -- a hex editor for files of unlimited size
 *
 *   > run wm
 *   > run hex
 *
 * -- layout --
 *
 *   +---------------------------------------------------+-+
 *   | Offset   00 01 02 03 04 05 06 07  08 ...          |#|
 *   | ------------------------------------------------- | |
 *   | 00000000 7A 65 69 74 6C 6F 73 00  00 ...  zeitlos. | |
 *   | 00000010 ...                                      | |
 *   | ------------------------------------------------- | |
 *   | 00000004/0001F400  6C 'l'  hex  rw                |-|
 *   +---------------------------------------------------+-+
 *
 * A column header, a grid of rows, and a status line, with one
 * vertical scrollbar down the right. Each row is an address, that
 * row's bytes in hex, and the same bytes as characters. The window is
 * resizable and everything is recomputed from its size in layout().
 *
 * -- files of unlimited size --
 *
 * The file is never held in memory, the same constraint sw/apps/read
 * works under and for the same reason. What is held is a CACHE_BYTES
 * window of it, reloaded only when the view moves outside what the
 * cache already covers -- so scrolling a row at a time usually costs
 * no card traffic at all, and jumping to a distant address costs one
 * read of a few sectors regardless of how far the jump was.
 *
 * Every offset in here is a uint32_t, which is not a limitation worth
 * removing: FF_FS_EXFAT is 0 and there is no 64-bit LBA
 * (sw/os/fs/fatfs/ffconf.h), so FAT32's own 4GB ceiling arrives first.
 *
 * -- editing is overwrite-only --
 *
 * Bytes can be changed. Bytes cannot be INSERTED or DELETED, and that
 * is a deliberate limit rather than an unfinished feature: inserting
 * one byte at the front of a file means rewriting every byte after it,
 * which on a large file is minutes of card traffic and a destroyed
 * file if the power goes during it. Every hex editor worth the name is
 * overwrite-only for exactly this reason. Changing the file's SIZE is
 * a separate, explicit operation (see fs_truncate() in zfsapp.h),
 * never a side effect of typing.
 *
 * -- this file, so far --
 *
 * Phase 2: viewing and navigation. The cursor, the two panes and the
 * pane focus are all here because they are what the layout has to
 * accommodate, but nothing writes yet -- see docs/hex_editor.md for
 * the phased plan and what lands next.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"

// -- window --

// 408x280 gives 16 bytes per row at z_font_5x8 with a little slack,
// which is the width every hexdump in the world uses and therefore
// the one people can read without counting columns. The arithmetic:
// 16 bytes needs 76 character cells (see cells_needed()), 380px at
// 5px each, plus margins and the scrollbar.
//
// NOT Z_WIN_FLAG_MIN_IS_CREATE. sw/apps/text pins its floor at its
// creation size because its furniture cannot shrink; this app's can --
// bytes_per_row drops to 8 or 4 and the grid stays correct, just
// narrower. Pinning the floor here would refuse a perfectly usable
// narrow window for no reason.
#define WIN_W   408
#define WIN_H   280

static z_win_t win;

// -- font and metrics --
//
// Both fonts wm keeps resident in glyph memory, so both draw through
// the hardware blitter (docs/window_manager.md, "Fonts in glyph
// memory"). The titlebar font button switches between them, which
// changes the cell width and therefore how many bytes fit on a row.
static const z_font_t *cur_font = &z_font_5x8;

#define CHAR_W      (cur_font->w)
#define LINE_H      (cur_font->h + 1)

#define MARGIN      2

// Address column, in character cells: 8 hex digits and a gap.
#define ADDR_CELLS  8
#define ADDR_GAP    2

// Bytes per group, with one extra cell of space between groups. Eight
// is what makes a 16-byte row scannable -- the eye finds byte 11 by
// landing on the gap and counting three, rather than counting eleven.
#define GROUP       8

// Cells between the last hex column and the character pane.
#define ASCII_GAP   2

// 32 bytes per row at 8 cells of address is 142 cells; nothing can
// reach that on a 640px screen at either font, but the buffer is
// sized for it rather than for what currently fits.
#define MAX_CELLS   160

// -- layout, recomputed on every resize --

static int bytes_per_row = 16;
static int rows;            // data rows visible at once
static int text_w;          // width available to the grid, excluding scrollbar
static int hdr_h;           // header row plus its rule
static int status_h;        // status rule plus its line

static z_scrollbar_t sbar;

// -- the file --

static int fh = -1;                 // open handle, or -1
static uint32_t fsize;
static char path[Z_FLIST_PATH_MAX]; // "" when nothing is open

// True when the file could only be opened for reading -- either it is
// genuinely read-only, or this kernel predates FS_OPEN_RW. Editing is
// refused and the status line says so, rather than accepting
// keystrokes that would silently go nowhere.
static bool read_only = true;

// -- view --

static uint32_t top_row;    // first visible row
static uint32_t cursor;     // byte offset of the caret

// Half of a hex byte has been typed at the caret, so the next hex
// digit is the LOW nibble and completes it.
//
// Two keystrokes per byte, with the caret staying put between them, is
// how every hex editor behaves and it is the only sane reading of a
// fixed-width field: a byte has two digits and you type both. Any caret
// movement abandons the half-typed byte rather than carrying it along.
static bool nibble_low;

// Which pane the caret is in. Editing (phase 3) sends keystrokes to
// one or the other, and they mean completely different things there --
// '4' is a nibble on the left and the character '4' on the right --
// so the distinction has to be visible before it is usable.
static bool in_ascii;

// -- the cache --
//
// One window of the file, kept so that scrolling a row does not
// re-read anything. 4KB is eight sectors: comfortably more than the
// largest possible screenful (51 rows x 16 bytes = 816 bytes), so a
// full screen's worth of scrolling in either direction usually stays
// inside it.
//
// Aligned down to CACHE_ALIGN on load. Not for correctness -- FatFs
// handles any offset -- but because a read that starts mid-sector
// makes FatFs fetch the sector before the one asked for, so an
// unaligned cache costs an extra sector on every reload forever.
#define CACHE_BYTES  4096
#define CACHE_ALIGN  512

static uint8_t cache[CACHE_BYTES];
static uint32_t cache_off;
static uint32_t cache_len;
static bool cache_valid;

// -- the edit journal --
//
// Pending edits, oldest first, as (offset, what was there, what is
// there now). This is the whole editing model, so it is worth saying
// why it is this and not one of the two obvious alternatives.
//
// WRITE-THROUGH -- put each keystroke straight on the card -- needs no
// journal, but costs an SD write and a directory sync per nibble, and
// leaves no way to back out of a session. It also makes the Save
// button a lie: there would be nothing left to save.
//
// A DIRTY PAGE CACHE -- hold pages, flush on save or eviction -- keeps
// Save meaningful only until you scroll far enough, at which point
// eviction commits your edits without being asked. A Save button that
// silently isn't the only thing that saves is worse than no Save
// button.
//
// The journal has neither problem. Memory is O(entries), independent
// of file size, so "unlimited size" survives; the display reads
// through it, so an edit stays visible after scrolling away and back;
// Save is the only thing that writes; and `old` makes it an undo stack
// for free.
//
// 1024 entries is 8KB of .bss and is a lot of pending edits for a
// format where one changed byte is typical. Running out is reported,
// not silently dropped -- see jrn_push().
#define JRN_MAX  1024

typedef struct {
	uint32_t	off;
	uint8_t		old;
	uint8_t		new;
} jrn_t;

static jrn_t jrn[JRN_MAX];
static int jrn_n;

// Set when a save wrote some runs and then failed part-way. It changes
// what undo has to do -- see do_undo().
static bool partial_save;

static bool modified(void) { return jrn_n > 0; }

// Replays the journal over whatever the cache just read off the card.
//
// This is what makes an edit survive scrolling away and back: the card
// still holds the old byte, so a reload would otherwise quietly revert
// the display to it while the journal still claimed the edit existed.
//
// One pass over the journal per cache load, not per byte. A lookup per
// displayed byte would be 816 x 1024 comparisons for one repaint,
// which is why the overlay is applied here and edits also patch the
// cache directly.
static void jrn_apply_to_cache(void) {

	if (!cache_valid) return;

	for (int i = 0; i < jrn_n; i++) {
		if (jrn[i].off < cache_off) continue;
		if (jrn[i].off >= cache_off + cache_len) continue;
		cache[jrn[i].off - cache_off] = jrn[i].new;
	}

}

// -- small formatters --
//
// Hand-rolled rather than snprintf(). Every one of these writes a
// FIXED number of characters into a caller-owned buffer with no
// terminator, because that is what building a fixed-column row wants:
// the row is a field of spaces that things get poked into, and a
// formatter that terminated its output would cut the row in half at
// the address.

static const char hexdig[] = "0123456789ABCDEF";

static void put_hex8(char *out, uint32_t v) {
	for (int i = 7; i >= 0; i--) { out[i] = hexdig[v & 0xf]; v >>= 4; }
}

static void put_hex2(char *out, uint8_t v) {
	out[0] = hexdig[(v >> 4) & 0xf];
	out[1] = hexdig[v & 0xf];
}

// Decimal, right-open: returns how many characters were written.
static int put_dec(char *out, uint32_t v) {
	char tmp[10];
	int n = 0;
	if (v == 0) { out[0] = '0'; return 1; }
	while (v && n < 10) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
	for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
	return n;
}

// Parses an offset or a size.
//
// HEX BY DEFAULT, because that is the base an offset gets quoted in and
// this is a hex editor -- a goto box that read "1000" as a thousand
// would be wrong far more often than right here. A "0x" prefix is
// accepted and ignored; a leading "." forces decimal, for the times the
// number came from a format spec that quotes sizes that way.
//
// `*ok` distinguishes "the user typed 0" from "the user typed
// nonsense", which the return value alone cannot.
static uint32_t parse_number(const char *in, bool *ok) {

	int i = 0;
	bool dec = false;
	uint32_t v = 0;
	bool any = false;

	if (ok) *ok = false;

	while (in[i] == ' ') i++;

	if (in[i] == '.') { dec = true; i++; }
	else if (in[i] == '0' && (in[i + 1] == 'x' || in[i + 1] == 'X')) i += 2;

	for (; in[i]; i++) {

		char c = in[i];
		int d;

		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;

		if (dec && d > 9) break;

		v = v * (dec ? 10u : 16u) + (uint32_t)d;
		any = true;

	}

	if (ok) *ok = any;

	return any ? v : 0;

}

// What a byte looks like in the character pane. Everything outside
// printable ASCII becomes '.', including the high half: this is a 1bpp
// display with a 96-glyph font, so there is nothing to draw for 0x80
// and up, and inventing something would only be misleading.
static char printable(uint8_t b) {
	return (b >= 0x20 && b <= 0x7e) ? (char)b : '.';
}

// -- cell geometry --
//
// Everything about where a field lands is computed from these four,
// and both the drawing and the mouse hit test walk them, so what you
// can see and what you can click are the same columns by construction
// -- the "compute once, share everywhere" rule wm.c's
// close_icon_rect() already follows for the same class of bug.

static int cell_hex_x(void) {
	return ADDR_CELLS + ADDR_GAP;
}

// Cell column of byte `i`'s first hex digit.
static int cell_byte_x(int i) {
	return cell_hex_x() + i * 3 + i / GROUP;
}

static int cell_ascii_x(void) {
	return cell_byte_x(bytes_per_row - 1) + 2 + ASCII_GAP;
}

static int cells_needed(int bpr) {
	int last = cell_hex_x() + (bpr - 1) * 3 + (bpr - 1) / GROUP;
	return last + 2 + ASCII_GAP + bpr;
}

static int cell_px(int cell) {
	return MARGIN + cell * CHAR_W;
}

// -- layout --

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	text_w = cw - Z_SB_THICK - 1;
	if (text_w < 0) text_w = 0;

	// The widest power of two that fits. Powers of two only, because
	// an address ending in a multiple of 10 or 12 bytes is unreadable
	// -- the whole value of the address column is that the low digit
	// tells you the column, which only works when the row length
	// divides a power of sixteen.
	//
	// Recomputed rather than fixed at 16 so a narrow window degrades
	// to a usable 8 or 4 columns instead of clipping half the grid
	// off its own right edge.
	int avail = text_w - 2 * MARGIN;

	bytes_per_row = 4;

	for (int bpr = 8; bpr <= 32; bpr <<= 1) {
		if (cells_needed(bpr) * CHAR_W > avail) break;
		bytes_per_row = bpr;
	}

	hdr_h = LINE_H + 2;
	status_h = LINE_H + 2;

	rows = (ch - hdr_h - status_h) / LINE_H;
	if (rows < 1) rows = 1;

	// Stops short of the resize grip, or clicks meant for the corner
	// land in the scrollbar trough and page the file instead --
	// see Z_WIN_GRIP_INSET in zwm.h, and sw/apps/text's own note.
	int sb_len = ch - Z_WIN_GRIP_INSET;
	if (sb_len < 0) sb_len = 0;

	z_scrollbar_set_geom(&sbar, cw - Z_SB_THICK, 0, sb_len);

}

// Total rows the file occupies. Always at least one, so an empty file
// shows an empty row rather than nothing at all -- "no rows" and "no
// file" would look identical, and one of them is a bug.
static uint32_t total_rows(void) {
	if (fsize == 0) return 1;
	return (fsize + (uint32_t)bytes_per_row - 1) / (uint32_t)bytes_per_row;
}

static uint32_t max_top_row(void) {
	uint32_t tr = total_rows();
	return tr > (uint32_t)rows ? tr - (uint32_t)rows : 0;
}

static void sync_scrollbar(void) {
	z_scrollbar_set_range(&sbar, (int32_t)total_rows(), (int32_t)rows);
	z_scrollbar_set_value(&sbar, (int32_t)top_row);
}

// Keeps the cursor's row on screen. Returns true if the view moved.
static bool scroll_to_cursor(void) {

	uint32_t row = cursor / (uint32_t)bytes_per_row;
	uint32_t old = top_row;

	if (row < top_row) top_row = row;
	if (row >= top_row + (uint32_t)rows) top_row = row - (uint32_t)rows + 1;

	if (top_row > max_top_row()) top_row = max_top_row();

	sync_scrollbar();

	return top_row != old;

}

// -- the cache --

static void cache_drop(void) {
	cache_valid = false;
	cache_off = 0;
	cache_len = 0;
}

// Makes sure [off, off+len) is covered, reloading if it isn't.
// Silently does nothing useful when no file is open, which every
// caller can tolerate: byte_at() reports the miss.
static void cache_cover(uint32_t off, uint32_t len) {

	if (fh < 0) return;

	if (cache_valid && off >= cache_off && off + len <= cache_off + cache_len)
		return;

	uint32_t start = off & ~(uint32_t)(CACHE_ALIGN - 1);

	if (!fs_seek(fh, start)) { cache_drop(); return; }

	uint32_t got = 0;

	while (got < CACHE_BYTES) {
		int n = fs_read_chunk(fh, &cache[got], (int)(CACHE_BYTES - got));
		if (n <= 0) break;
		got += (uint32_t)n;
	}

	cache_off = start;
	cache_len = got;
	cache_valid = true;

	jrn_apply_to_cache();

}

// The byte at `off`, or -1 if it is past the end of the file (or
// unreadable). -1 rather than 0, deliberately: a row that runs off the
// end of the file must draw BLANK there, and a zero byte is a real
// value that must draw as "00". Conflating them would make every file
// look as though it ended in a run of zeros.
static int byte_at(uint32_t off) {

	if (fh < 0 || off >= fsize) return -1;

	if (!cache_valid || off < cache_off || off >= cache_off + cache_len)
		cache_cover(off, 1);

	if (!cache_valid || off < cache_off || off >= cache_off + cache_len)
		return -1;

	return cache[off - cache_off];

}

// -- drawing --

// Content-relative fill, clamped to the content area.
// z_fb_hw_fill_rect() clamps to the SCREEN, not to this window, so
// handing it a content-relative rect directly paints over whatever
// else is on screen the moment the furniture runs past our own edge.
// Same helper and same reason as sw/apps/text's fill_content().
static void fill_content(int cx, int cy, int w, int h, int color) {

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x0 = c.x0 + cx, y0 = c.y0 + cy;
	int x1 = x0 + w - 1, y1 = y0 + h - 1;

	if (x0 < c.x0) x0 = c.x0;
	if (y0 < c.y0) y0 = c.y0;
	if (x1 > c.x1) x1 = c.x1;
	if (y1 > c.y1) y1 = c.y1;
	if (x1 < x0 || y1 < y0) return;

	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);

}

// A horizontal rule across the grid area, content-relative.
//
// Drawn as a 1px filled rect rather than through z_win_hw_line(),
// because that one takes ABSOLUTE SCREEN coordinates while everything
// else here is content-relative -- the footgun docs/window_manager.md
// devotes a section to, which cost sw/apps/logic three shipped
// layouts. Nothing in this file uses the absolute-coordinate family at
// all, so there is no chance of mixing them.
static void draw_rule(int y) {
	fill_content(0, y, text_w, 1, 1);
}

// A 1px frame around a content-relative rect, built from the same
// clamped fill as everything else here.
static void draw_frame(int x, int y, int w, int h) {
	fill_content(x, y, w, 1, 1);
	fill_content(x, y + h - 1, w, 1, 1);
	fill_content(x, y, 1, h, 1);
	fill_content(x + w - 1, y, 1, h, 1);
}

static int row_y(int r) {
	return hdr_h + r * LINE_H;
}

// Builds one row of the grid into `line` as a fixed field of spaces
// with the address, the hex and the characters poked into it.
//
// One string for the whole row, drawn in one call, rather than a call
// per field or per byte. A 16-byte row is 76 cells; as three separate
// draws that is three glyph-blitter setups per row, and as 32 draws
// (two per byte) it is 32. The row is naturally one line of text and
// the only reason to split it would be per-byte colour, which the
// cursor needs for exactly one byte and gets by overdrawing.
//
// Returns the number of cells written.
static int build_row(char *line, uint32_t off) {

	int n = cell_ascii_x() + bytes_per_row;

	for (int i = 0; i < n; i++) line[i] = ' ';
	line[n] = 0;

	put_hex8(line, off);

	int ax = cell_ascii_x();

	for (int i = 0; i < bytes_per_row; i++) {

		int v = byte_at(off + (uint32_t)i);

		// Past the end of the file: leave both panes blank here. The
		// row still exists (its address is real) but these columns
		// hold nothing, and showing "00 ." would claim otherwise.
		if (v < 0) continue;

		put_hex2(&line[cell_byte_x(i)], (uint8_t)v);
		line[ax + i] = printable((uint8_t)v);

	}

	return n;

}

// Draws row `r` of the visible grid, cursor included.
static void draw_row(int r) {

	char line[MAX_CELLS + 1];

	uint32_t off = (top_row + (uint32_t)r) * (uint32_t)bytes_per_row;
	int y = row_y(r);

	// Rows past the end of the file are blank -- no address either.
	// An address column that kept counting past the end of the file
	// would invite clicking on a byte that does not exist.
	if (off >= fsize && !(fsize == 0 && off == 0)) {
		fill_content(0, y, text_w, LINE_H, 0);
		return;
	}

	build_row(line, off);

	// fg on a solid cell, so the row paints its own background and
	// nothing has to be cleared first (see z_win_draw_text2's comment
	// in zwin.h on why the two-colour form exists).
	z_win_draw_text2(&win, cell_px(0), y, line, 1, 0, cur_font);

	// The cursor, overdrawn inverse on the two cells of its hex byte
	// and the one cell of its character. Both panes are marked, so the
	// eye can follow one to the other; the INACTIVE pane is the one
	// that gets the dimmer treatment -- see draw_cursor().
	if (cursor >= off && cursor < off + (uint32_t)bytes_per_row && fsize > 0) {

		int i = (int)(cursor - off);
		int v = byte_at(cursor);
		char h[3], a[2];

		if (v < 0) { h[0] = h[1] = ' '; a[0] = ' '; }
		else { put_hex2(h, (uint8_t)v); a[0] = printable((uint8_t)v); }

		h[2] = 0;
		a[1] = 0;

		// The focused pane draws its cell inverted (ink 0 on a solid
		// cell); the other draws a 1px frame around the cell instead,
		// so exactly one of the two reads as "typing goes here" while
		// both still show where you are. Two identical inversions
		// would be ambiguous about which one has the keyboard, which
		// is the only thing this distinction exists to say.
		if (!in_ascii) {
			z_win_draw_text2(&win, cell_px(cell_byte_x(i)), y, h, 0, 1, cur_font);
			draw_frame(cell_px(cell_ascii_x() + i) - 1, y - 1,
				CHAR_W + 2, cur_font->h + 2);
		} else {
			z_win_draw_text2(&win, cell_px(cell_ascii_x() + i), y, a, 0, 1, cur_font);
			draw_frame(cell_px(cell_byte_x(i)) - 1, y - 1,
				2 * CHAR_W + 2, cur_font->h + 2);
		}

	}

}

static void draw_header(void) {

	char line[MAX_CELLS + 1];

	int n = cell_ascii_x() + bytes_per_row;
	for (int i = 0; i < n; i++) line[i] = ' ';
	line[n] = 0;

	const char *lbl = "Offset";
	for (int i = 0; lbl[i]; i++) line[i] = lbl[i];

	// Column numbers, two digits, aligned with the hex columns they
	// label. The low byte of the index, so a 32-byte row still reads
	// 00..1F rather than running out of digits.
	for (int i = 0; i < bytes_per_row; i++)
		put_hex2(&line[cell_byte_x(i)], (uint8_t)i);

	z_win_draw_text2(&win, cell_px(0), 0, line, 1, 0, cur_font);

	draw_rule(LINE_H + 1);

}

// A one-shot line shown in the status bar instead of the usual
// readout: "read-only", "saved", "zeroing 40%". Borrowed, never
// copied -- it always points at a literal or at a caller's buffer that
// outlives the draw. Cleared by the next caret movement, so a message
// never outstays the action that produced it.
static const char *status_msg;

static void draw_status(void) {

	char line[MAX_CELLS + 1];
	int n = cell_ascii_x() + bytes_per_row;
	if (n > MAX_CELLS) n = MAX_CELLS;

	for (int i = 0; i < n; i++) line[i] = ' ';
	line[n] = 0;

	// The mode is placed FIRST, right-aligned, and everything else is
	// built into what is left.
	//
	// Built left to right instead, it is the field that falls off a
	// narrow window -- which is exactly backwards. An offset that is
	// cut short is an inconvenience; "read-only" going missing means
	// the user believes they can edit, types, and finds nothing
	// happens. The least important field must not be the one that
	// survives.
	const char *mode = read_only ? "read-only" : "rw";

	int mode_len = 0;
	while (mode[mode_len]) mode_len++;

	int avail = n - mode_len;

	if (avail > 0) {
		for (int i = 0; i < mode_len; i++) line[avail + i] = mode[i];
		avail -= 2;              // a gap before it
	} else {
		avail = 0;               // nothing else fits at all
	}

	int p = 0;

	// Where the caret is, and how big the file is -- both in hex,
	// because this is a hex editor and an address the user is about to
	// type into the goto box should be shown in the base they will
	// type it in.
	if (p + 17 <= avail) {
		put_hex8(&line[p], cursor); p += 8;
		line[p++] = '/';
		put_hex8(&line[p], fsize); p += 8;
		p += 2;
	}

	// The byte under the caret, as hex and as a character. This is the
	// one thing a hex editor is asked constantly, and the grid itself
	// answers it only once you have found the column.
	int v = byte_at(cursor);

	if (v >= 0 && p + 6 <= avail) {
		put_hex2(&line[p], (uint8_t)v); p += 2;
		line[p++] = ' ';
		line[p++] = '\'';
		line[p++] = printable((uint8_t)v);
		line[p++] = '\'';
		p += 2;
	}

	// Decimal too. An offset that has to be checked against a struct
	// size or a format spec is usually quoted in decimal, and doing
	// that conversion in your head is exactly the tax this app exists
	// to remove.
	if (p + 12 <= avail) {
		line[p++] = '(';
		p += put_dec(&line[p], cursor);
		line[p++] = ')';
	}

	// A one-shot message replaces the readout entirely rather than
	// squeezing in beside it. At four bytes per row there is not room
	// for both, and "read-only" appearing only on wide windows would
	// be the same bug the right-aligned mode field above exists to
	// avoid.
	if (status_msg) {
		for (int i = 0; i < n; i++) line[i] = ' ';
		for (int i = 0; status_msg[i] && i < n; i++) line[i] = status_msg[i];
	}

	int y = row_y(rows) + 1;

	draw_rule(row_y(rows) - 1);
	fill_content(0, y, text_w, LINE_H, 0);
	z_win_draw_text2(&win, cell_px(0), y, line, 1, 0, cur_font);

}

static void repaint(void) {

	// One read for the whole screenful before drawing any of it.
	// Without this, byte_at() would fault the cache in from whichever
	// row happened to be drawn first, and a screen straddling the
	// cache edge would reload part-way down -- correct, but it turns
	// one read into two for no reason.
	cache_cover(top_row * (uint32_t)bytes_per_row,
		(uint32_t)(rows * bytes_per_row));

	z_win_clear(&win);

	draw_header();

	for (int r = 0; r < rows; r++) draw_row(r);

	draw_status();

	z_scrollbar_draw(&sbar, true);

}
// -- titlebar --

// "*data.bin" -- the last path component, prefixed with '*' when there
// are unsaved edits. Just the basename: "/DOCS/DATA.BIN" is mostly
// slashes at the width wm gives a title.
static void build_title(char *t, int cap, const char *p, bool star) {

	int n = 0;
	const char *base = p[0] ? p : "untitled";

	for (const char *s = p; *s; s++)
		if (*s == '/') base = s + 1;

	if (star && n < cap - 1) t[n++] = '*';

	for (const char *s = base; *s && n < cap - 1; s++) t[n++] = *s;

	t[n] = 0;

}

// The last title actually sent, so a call that would change nothing
// sends nothing.
//
// z_win_set_title() is fire and forget: it queues a message and
// returns, wm then repairs the titlebar strip in its own process and
// asks the owners of every window overlapping it to repaint. A title
// sent just before this app paints is a race between that repaint and
// ours. Suppressing the no-op sends removes most of the exposure for
// free -- see sw/apps/text's update_title(), which learned this the
// hard way.
static char sent_title[32];

static void update_title(void) {

	char t[32];
	build_title(t, (int)sizeof(t), path, modified());

	int i = 0;
	for (; t[i] && t[i] == sent_title[i]; i++);
	if (t[i] == sent_title[i]) return;

	for (i = 0; i < (int)sizeof(sent_title) - 1 && t[i]; i++) sent_title[i] = t[i];
	sent_title[i] = 0;

	z_win_set_title(&win, t);

}

// -- file operations --

static void forward_msg(z_msg_t *msg, void *user);
static void move_to(uint32_t off);
static uint32_t parse_number(const char *in, bool *ok);

static z_dialog_ctx_t dlg_ctx;

// Where the last dialog was, so a second one starts where the first
// left off rather than back at the root.
static char last_dir[Z_FLIST_PATH_MAX] = "/";

static void remember_dir(const char *p) {

	int last = 0;

	for (int i = 0; p[i]; i++) if (p[i] == '/') last = i;

	if (last == 0) { last_dir[0] = '/'; last_dir[1] = 0; return; }

	int i = 0;
	for (; i < last && i < (int)sizeof(last_dir) - 1; i++) last_dir[i] = p[i];

	last_dir[i] = 0;

}

static void close_file(void) {

	// Always, on every path. A leaked handle is a permanently lost
	// slot in a table of Z_FS_MAX_OPEN, board-wide, that nothing
	// sweeps when a process exits (zfs.h).
	if (fh >= 0) fs_close_handle(fh);

	fh = -1;
	fsize = 0;
	read_only = true;
	cache_drop();

}

// Opens `p`, preferring read-write and falling back to read-only.
//
// The two-step is what lets the failure be explained rather than just
// reported. fs_open_rw() returns -1 for "no such file", "no free
// handle", and "this kernel predates FS_OPEN_RW" alike (see
// zfsapp.h), so a bare failure here could mean any of three very
// different things. Checking the size first establishes that the file
// exists; if it does and read-write still fails, read-only is tried,
// and succeeding at THAT says the problem was the write side
// specifically.
static bool open_file(const char *p) {

	uint32_t size = (uint32_t)fs_size((char *)p);

	// fs_size() reports 0 for a file that does not exist AND for one
	// that is genuinely empty -- its own header says so. Opening
	// settles it: a real empty file opens, a missing one does not.
	close_file();

	int h = fs_open_rw(p);
	bool ro = false;

	if (h < 0) {
		h = fs_open_read(p);
		ro = true;
	}

	if (h < 0) {
		z_dialog_confirm(&dlg_ctx, "Open failed",
			"That file could not\nbe opened.", Z_DIALOG_OK_CANCEL);
		return false;
	}

	fh = h;
	fsize = size;
	read_only = ro;

	int i = 0;
	for (; p[i] && i < (int)sizeof(path) - 1; i++) path[i] = p[i];
	path[i] = 0;

	remember_dir(path);

	cursor = 0;
	top_row = 0;
	in_ascii = false;

	cache_drop();
	sync_scrollbar();
	update_title();

	return true;

}

static void do_open(void) {

	char p[Z_FLIST_PATH_MAX];

	if (!z_dialog_open(&dlg_ctx, last_dir, p, sizeof(p))) return;

	if (open_file(p)) repaint();

}

// Switches between the two resident fonts. The cell width changes, so
// the column count changes with it -- and the CURSOR STAYS ON THE SAME
// BYTE, because it is a file offset and owes nothing to the layout.
// That separation is the whole reason the offset is what is stored
// rather than a row and column.
static void do_font(void) {

	cur_font = (cur_font == &z_font_5x8) ? &z_font_6x12 : &z_font_5x8;

	layout();
	scroll_to_cursor();
	repaint();

}

// -- editing --

static void set_status(const char *m) {
	status_msg = m;
	draw_status();
}

static void jrn_clear(void) {
	jrn_n = 0;
	partial_save = false;
}

// Records an edit and applies it. Returns false if the journal is
// full, having changed nothing.
//
// Refusing loudly rather than dropping the oldest entry: silently
// discarding the oldest would break undo (the stack would no longer
// reach back to the original bytes) AND lose an edit that Save was
// going to write. Both failures are invisible at the moment they
// happen and only surface as a file that isn't what you typed.
static bool jrn_push(uint32_t off, uint8_t old, uint8_t val) {

	if (jrn_n >= JRN_MAX) {
		z_dialog_confirm(&dlg_ctx, "Too many edits",
			"Save before making\nmore changes.", Z_DIALOG_OK_CANCEL);
		return false;
	}

	jrn[jrn_n].off = off;
	jrn[jrn_n].old = old;
	jrn[jrn_n].new = val;
	jrn_n++;

	// Patch the cache too. jrn_apply_to_cache() covers a RELOAD; this
	// covers the far more common case where the byte is already
	// resident and the edit has to show immediately.
	if (cache_valid && off >= cache_off && off < cache_off + cache_len)
		cache[off - cache_off] = val;

	return true;

}

// Writes the journal to the file.
//
// Entries are sorted by offset and runs of consecutive offsets are
// coalesced, so changing sixteen adjacent bytes is one seek and one
// write rather than sixteen of each. Scattered edits still cost a
// seek apiece, which is what scattered edits are.
//
// EVERY WRITE IS IDEMPOTENT -- it puts a known byte at a known offset,
// derived from the journal and not from anything read back. So a save
// that fails part-way can simply be retried, and that is what the
// journal is kept for on failure rather than discarded.
static bool do_save(void) {

	if (fh < 0 || !modified()) return true;

	if (read_only) {
		z_dialog_confirm(&dlg_ctx, "Read-only",
			"This file was opened\nfor reading only.", Z_DIALOG_OK_CANCEL);
		return false;
	}

	// Sort an index rather than the journal itself: the journal is an
	// undo stack and its ORDER IS ITS MEANING. Sorting it in place
	// would make undo restore bytes in the wrong sequence, which for
	// two edits to the same byte gives the wrong final value.
	static uint16_t order[JRN_MAX];

	for (int i = 0; i < jrn_n; i++) order[i] = (uint16_t)i;

	// Shell sort. Insertion sort is O(n^2), and 1024 entries of that
	// is a million comparisons on this CPU for something the user is
	// waiting on; qsort() would pull in libc's implementation plus a
	// comparator call per comparison. This is fifteen lines and fast
	// enough that the SD writes dominate, which is the right place for
	// the time to go.
	for (int gap = jrn_n / 2; gap > 0; gap /= 2) {
		for (int i = gap; i < jrn_n; i++) {
			uint16_t v = order[i];
			int j = i;
			while (j >= gap) {
				uint32_t a = jrn[order[j - gap]].off, b = jrn[v].off;
				// Ties broken by journal position, so the LAST edit to
				// an offset sorts last and therefore wins below.
				if (a < b || (a == b && order[j - gap] < v)) break;
				order[j] = order[j - gap];
				j -= gap;
			}
			order[j] = v;
		}
	}

	uint8_t run[256];
	uint32_t run_off = 0;
	int run_len = 0;
	bool ok = true;

	for (int i = 0; i < jrn_n && ok; i++) {

		uint32_t off = jrn[order[i]].off;

		// Duplicate offset: the next entry is a later edit to the same
		// byte, so this one is superseded and must not be written.
		if (i + 1 < jrn_n && jrn[order[i + 1]].off == off) continue;

		bool contiguous = run_len > 0 &&
			off == run_off + (uint32_t)run_len && run_len < (int)sizeof(run);

		if (!contiguous && run_len > 0) {
			// Seek before every write. A redraw serviced between runs
			// reads through the SAME handle, so the file position is
			// not ours to assume.
			if (!fs_seek(fh, run_off) ||
				fs_write_chunk(fh, run, run_len) != run_len) ok = false;
			partial_save = true;
			run_len = 0;
		}

		if (run_len == 0) run_off = off;

		run[run_len++] = jrn[order[i]].new;

	}

	if (ok && run_len > 0) {
		if (!fs_seek(fh, run_off) ||
			fs_write_chunk(fh, run, run_len) != run_len) ok = false;
		partial_save = true;
	}

	// Commit the directory entry as well as the data. Without this the
	// bytes are in the right clusters and the size the directory
	// records is stale, so pulling the card leaves the work unfindable
	// rather than merely unwritten.
	if (ok && !fs_sync(fh)) ok = false;

	if (!ok) {
		z_dialog_confirm(&dlg_ctx, "Save failed",
			"Some changes may not\nhave been written.\nTry again.",
			Z_DIALOG_OK_CANCEL);
		// Journal deliberately kept: every write is idempotent, so
		// saving again is both safe and the right thing to do.
		cache_drop();
		repaint();
		return false;
	}

	jrn_clear();
	update_title();
	set_status("saved");

	return true;

}

// Undoes the most recent edit.
//
// Normally that means popping the entry and putting `old` back. After
// a PARTIALLY FAILED SAVE it cannot: some of those entries are already
// on the card, and popping one would leave the display showing the old
// byte while the file held the new one, with nothing left in the
// journal to ever reconcile them. So in that state undo appends an
// INVERSE edit instead, which the next save will write.
static void do_undo(void) {

	if (!modified()) { set_status("nothing to undo"); return; }

	jrn_t e = jrn[jrn_n - 1];

	if (partial_save) {
		if (!jrn_push(e.off, e.new, e.old)) return;
	} else {
		jrn_n--;
		if (cache_valid && e.off >= cache_off && e.off < cache_off + cache_len)
			cache[e.off - cache_off] = e.old;
	}

	cursor = e.off;
	update_title();
	scroll_to_cursor();
	repaint();

}

// Offers to save when there are unsaved edits. Returns false if the
// user cancelled, meaning whatever prompted this must not proceed.
static bool confirm_discard(void) {

	if (!modified()) return true;

	int r = z_dialog_confirm(&dlg_ctx, "Unsaved changes",
		"Save changes before\ncontinuing?", Z_DIALOG_YES_NO_CANCEL);

	if (r == Z_DIALOG_CANCEL) return false;
	if (r == Z_DIALOG_NO) return true;

	// Yes -- and if that save is cancelled or fails, the whole
	// operation is off. Silently discarding after a failed save would
	// be the worst possible reading of "yes, save it".
	return do_save();

}

// Services wm while a long operation runs, WITHOUT accepting input.
//
// Redraws have to be answered or wm blocks its whole main loop waiting
// for the ack and the desktop freezes (docs/window_manager.md). Keys
// and clicks must NOT be, because the app is mid-operation on the file
// and a keystroke handled here would edit a document that is still
// being created.
static void pump_redraws(void) {

	z_msg_t msg;

	while (z_msg_read(&msg) == Z_OK) {
		switch (msg.subject) {
			case Z_WM_KEY:
			case Z_WM_MOUSE:
			case Z_WM_TITLEBAR_ICON:
				break;
			default:
				forward_msg(&msg, NULL);
				break;
		}
	}

}

// Creates a new file of a given size, filled with zeros.
//
// ASKING FOR A SIZE is the unusual part, so: hex editors that create an
// EMPTY document (HxD, Hex Fiend, 010) all rely on insert mode to grow
// it afterwards, and this editor deliberately has none. Editors without
// insert mode (hexedit, and every disk editor) don't offer New at all
// -- you open something that exists. An empty New here would hand the
// user a document they cannot type a single byte into, which is the
// worst of both. A name and a size is what `truncate -s` and `dd` do,
// and it is the only shape that yields a usable document under
// overwrite-only semantics.
//
// THE ZEROING IS NOT OPTIONAL. fs_truncate() grows a file by allocating
// clusters, not by clearing them, so a file created by truncation alone
// reads back as whatever the previous occupant of those sectors left
// behind -- someone else's deleted data, in a file the user just
// created.
static void do_new(void) {

	if (!confirm_discard()) return;

	char p[Z_FLIST_PATH_MAX];

	if (!z_dialog_save(&dlg_ctx, last_dir, "", p, sizeof(p))) return;

	if (fs_size(p) > 0) {
		if (z_dialog_confirm(&dlg_ctx, "Overwrite?",
			"That file already\nexists. Replace it?",
			Z_DIALOG_YES_NO) != Z_DIALOG_YES) return;
	}

	char in[24];

	if (!z_dialog_prompt(&dlg_ctx, "New file",
		"Size in bytes (hex,\nor .decimal):", "1000", in, sizeof(in))) return;

	bool ok_num = false;
	uint32_t size = parse_number(in, &ok_num);

	if (!ok_num || size == 0) {
		z_dialog_confirm(&dlg_ctx, "New file",
			"That is not a usable\nsize.", Z_DIALOG_OK_CANCEL);
		return;
	}

	// Checked against free space BEFORE anything is created. "I meant
	// 4MB and typed 4GB" deserves an answer, not a very long wait
	// followed by a half-written file.
	uint32_t free_kb = 0;

	if (fs_df(NULL, &free_kb) && free_kb < (size + 1023) / 1024) {
		z_dialog_confirm(&dlg_ctx, "Not enough space",
			"The card does not have\nroom for that file.",
			Z_DIALOG_OK_CANCEL);
		return;
	}

	close_file();
	jrn_clear();

	if (!fs_touch(p)) {
		z_dialog_confirm(&dlg_ctx, "New file",
			"That file could not\nbe created.", Z_DIALOG_OK_CANCEL);
		return;
	}

	int h = fs_open_rw(p);

	if (h < 0 || !fs_truncate(h, size)) {
		if (h >= 0) fs_close_handle(h);
		z_dialog_confirm(&dlg_ctx, "New file",
			"That file could not\nbe sized.", Z_DIALOG_OK_CANCEL);
		return;
	}

	fh = h;
	fsize = size;
	read_only = false;
	cursor = 0;
	top_row = 0;
	in_ascii = false;
	cache_drop();

	int i = 0;
	for (; p[i] && i < (int)sizeof(path) - 1; i++) path[i] = p[i];
	path[i] = 0;

	remember_dir(path);
	sync_scrollbar();
	update_title();
	repaint();

	// Zero it, in chunks, servicing wm between them.
	//
	// One big write would be the longest single trip into FatFs
	// anything in this system makes -- past K_NO_PREEMPT_MAX_TICKS,
	// where the scheduler stops deferring and the card protection this
	// depends on stops holding (docs/filesystem.md). Chunks keep each
	// syscall short and keep the window alive while it happens.
	static uint8_t zeros[512];
	memset(zeros, 0, sizeof(zeros));

	char msg[24];
	uint32_t done = 0;
	bool ok = true;

	while (done < size && ok) {

		uint32_t n = size - done;
		if (n > sizeof(zeros)) n = sizeof(zeros);

		// Seek every time: the redraws serviced below read through this
		// same handle, so the position is not ours to assume.
		if (!fs_seek(fh, done) ||
			fs_write_chunk(fh, zeros, (int)n) != (int)n) { ok = false; break; }

		done += n;

		// Progress roughly every 32KB, which is often enough to look
		// alive and rare enough not to spend the time drawing it.
		if ((done & 0x7fff) == 0 || done >= size) {

			int q = 0;
			const char *w = "zeroing ";
			for (; w[q]; q++) msg[q] = w[q];
			q += put_dec(&msg[q], size ? (done / (size / 100 + 1)) : 100);
			msg[q++] = '%';
			msg[q] = 0;

			cache_drop();
			set_status(msg);
			pump_redraws();

		}

	}

	if (!ok) {
		z_dialog_confirm(&dlg_ctx, "New file",
			"The file could not be\nfully written.", Z_DIALOG_OK_CANCEL);
	} else {
		fs_sync(fh);
	}

	cache_drop();
	status_msg = NULL;
	repaint();

}

// Applies one byte edit at the caret. Shared by both panes, so the
// journal, the read-only check and the repaint live in one place.
static void put_byte(uint32_t off, uint8_t val, bool advance) {

	if (fh < 0 || off >= fsize) return;

	if (read_only) { set_status("read-only"); return; }

	int old = byte_at(off);
	if (old < 0) return;

	bool was = modified();

	if (!jrn_push(off, (uint8_t)old, val)) return;

	if (was != modified()) update_title();

	uint32_t row = off / (uint32_t)bytes_per_row;

	if (row >= top_row && row < top_row + (uint32_t)rows)
		draw_row((int)(row - top_row));

	if (advance && off + 1 < fsize) move_to(off + 1);
	else draw_status();

}

static void do_close(void) {

	if (!confirm_discard()) return;

	close_file();
	z_win_destroy(&win);

	printf("hex: exiting\n");

	exit(0);

}

// -- navigation --

// Moves the caret to `off`, clamped, and repaints whatever changed.
//
// The common case -- moving within the visible grid -- redraws two
// rows and the status line, not the screen. A hex grid is a screenful
// of glyphs and repainting all of it on every arrow key would make
// holding an arrow down visibly stutter.
static void move_to(uint32_t off) {

	if (fsize == 0) { cursor = 0; return; }

	if (off >= fsize) off = fsize - 1;

	if (off == cursor) return;

	// The caret moving is what ends a nibble in progress: half a byte
	// typed at one offset must not finish itself at another.
	nibble_low = false;
	status_msg = NULL;

	uint32_t old = cursor;
	cursor = off;

	uint32_t old_row = old / (uint32_t)bytes_per_row;
	uint32_t new_row = cursor / (uint32_t)bytes_per_row;

	if (scroll_to_cursor()) {
		repaint();
		return;
	}

	if (old_row >= top_row && old_row < top_row + (uint32_t)rows)
		draw_row((int)(old_row - top_row));

	if (new_row != old_row && new_row >= top_row &&
		new_row < top_row + (uint32_t)rows)
		draw_row((int)(new_row - top_row));

	draw_status();

}

// Moves the caret by a signed delta without wrapping past either end.
// Signed arithmetic on a uint32_t offset is exactly where an
// off-by-one becomes an offset of four billion, so the bounds are
// checked before the addition rather than after it.
static void move_by(int32_t delta) {

	if (fsize == 0) return;

	if (delta < 0) {
		uint32_t back = (uint32_t)(-delta);
		move_to(cursor > back ? cursor - back : 0);
	} else {
		uint32_t fwd = (uint32_t)delta;
		move_to(cursor + fwd < cursor ? fsize - 1 : cursor + fwd);
	}

}

static void scroll_to_row(uint32_t row) {

	if (row > max_top_row()) row = max_top_row();
	if (row == top_row) return;

	top_row = row;
	sync_scrollbar();
	repaint();

}

// Jump to an address. Hex by default, because this is a hex editor and
// an offset copied out of a disassembler or a struct layout is hex --
// a "0x" prefix is accepted and ignored, and a leading "." forces
// decimal for the times the number came from a file format spec that
// quotes sizes in decimal.
static void do_goto(void) {

	char in[24];

	if (!z_dialog_prompt(&dlg_ctx, "Go to offset",
		"Address (hex, or\n.decimal):", "", in, sizeof(in))) return;

	bool ok = false;
	uint32_t v = parse_number(in, &ok);

	if (!ok) {
		z_dialog_confirm(&dlg_ctx, "Go to offset",
			"That is not a number.", Z_DIALOG_OK_CANCEL);
		return;
	}

	// Past the end lands on the last byte rather than refusing. "Go to
	// the end" typed as a round number bigger than the file is a
	// perfectly clear request, and answering it with an error box would
	// be pedantry.
	move_to(v);
	scroll_to_cursor();
	repaint();

}

// -- input --

static void handle_key(uint32_t keysym, uint8_t mods) {

	bool ctrl = (mods & Z_KBD_MOD_CTRL) != 0;

	switch (keysym) {

		case Z_KEY_LEFT:   move_by(-1); return;
		case Z_KEY_RIGHT:  move_by(1); return;
		case Z_KEY_UP:     move_by(-bytes_per_row); return;
		case Z_KEY_DOWN:   move_by(bytes_per_row); return;

		case Z_KEY_PAGEUP:   move_by(-(rows * bytes_per_row)); return;
		case Z_KEY_PAGEDOWN: move_by(rows * bytes_per_row); return;

		case Z_KEY_HOME:
			// Ctrl+Home is the file, plain Home is the row -- the
			// convention every editor on every platform uses.
			if (ctrl) move_to(0);
			else move_to(cursor - cursor % (uint32_t)bytes_per_row);
			return;

		case Z_KEY_END:
			if (ctrl) { move_to(fsize ? fsize - 1 : 0); return; }
			{
				uint32_t end = cursor - cursor % (uint32_t)bytes_per_row
					+ (uint32_t)bytes_per_row - 1;
				move_to(end);
			}
			return;

		default: break;

	}

	// Tab switches panes. Both panes always show the caret; which one
	// is inverted says where a keystroke would go.
	if (keysym == '\t') {
		in_ascii = !in_ascii;
		nibble_low = false;
		uint32_t row = cursor / (uint32_t)bytes_per_row;
		if (row >= top_row && row < top_row + (uint32_t)rows)
			draw_row((int)(row - top_row));
		return;
	}

	// Backspace steps back a byte without changing anything.
	//
	// Deliberately NOT "delete the previous byte": there is no delete
	// in an overwrite-only editor, and there is no obviously right
	// substitute either -- zeroing the byte would be a destructive
	// surprise from a key that means "undo my typing" everywhere else.
	// Moving the caret is what the muscle memory of typing a wrong
	// digit actually wants.
	if (keysym == 0x08) { move_by(-1); return; }

	if (ctrl) {
		switch (keysym) {
			case 0x0e: do_new(); return;    // Ctrl+N
			case 0x0f: do_open(); return;   // Ctrl+O
			case 0x13: do_save(); return;   // Ctrl+S
			case 0x07: do_goto(); return;   // Ctrl+G
			case 0x1a: do_undo(); return;   // Ctrl+Z
			case 0x11: do_close(); return;  // Ctrl+Q
			default: return;
		}
	}

	if (fh < 0 || fsize == 0) return;

	// -- the character pane --
	//
	// Any printable character writes that byte. Non-printable input has
	// no character to type, which is what the hex pane is for.

	if (in_ascii) {
		if (keysym >= 0x20 && keysym <= 0x7e)
			put_byte(cursor, (uint8_t)keysym, true);
		return;
	}

	// -- the hex pane --

	int d;

	if (keysym >= '0' && keysym <= '9') d = (int)keysym - '0';
	else if (keysym >= 'a' && keysym <= 'f') d = (int)keysym - 'a' + 10;
	else if (keysym >= 'A' && keysym <= 'F') d = (int)keysym - 'A' + 10;
	else return;

	int old = byte_at(cursor);
	if (old < 0) return;

	if (read_only) { set_status("read-only"); return; }

	if (!nibble_low) {

		// First digit: replace the high nibble and STAY PUT. The byte
		// on screen changes immediately, which is what makes the
		// half-finished state visible rather than a hidden mode.
		put_byte(cursor, (uint8_t)((d << 4) | (old & 0x0f)), false);
		nibble_low = true;

	} else {

		put_byte(cursor, (uint8_t)((old & 0xf0) | d), true);
		nibble_low = false;

	}

}

// Which byte offset a content-relative point lands on, or -1 for
// none. Walks the SAME cell geometry the drawing does, so a click
// cannot land on a column that isn't there.
//
// `*ascii` reports which pane was hit, so a click can move the caret
// and switch panes in one gesture -- clicking a character and then
// finding the keyboard still aimed at the hex pane would be its own
// small betrayal.
static int32_t hit_cell(int cx, int cy, bool *ascii) {

	if (cy < hdr_h) return -1;

	int r = (cy - hdr_h) / LINE_H;
	if (r < 0 || r >= rows) return -1;

	int cell = (cx - MARGIN) / CHAR_W;
	if (cx < MARGIN) return -1;

	for (int i = 0; i < bytes_per_row; i++) {

		if (cell >= cell_byte_x(i) && cell <= cell_byte_x(i) + 1) {
			*ascii = false;
			return (int32_t)((top_row + (uint32_t)r) * (uint32_t)bytes_per_row
				+ (uint32_t)i);
		}

		if (cell == cell_ascii_x() + i) {
			*ascii = true;
			return (int32_t)((top_row + (uint32_t)r) * (uint32_t)bytes_per_row
				+ (uint32_t)i);
		}

	}

	return -1;

}

static void handle_mouse(uint32_t packed) {

	int cx, cy;

	if (!z_win_mouse_content_xy(&win, packed, &cx, &cy)) return;

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// The scrollbar gets first refusal: it owns its own strip, and it
	// needs the button-up sample too, so this runs whether or not a
	// button is down.
	if (z_scrollbar_has_pointer(&sbar, cx, cy) || sbar.dragging) {
		if (z_scrollbar_mouse(&sbar, cx, cy, buttons)) {
			scroll_to_row((uint32_t)sbar.value);
			z_scrollbar_draw(&sbar, false);
		}
		return;
	}

	if (!(buttons & Z_MOUSE_BTN_LEFT)) return;

	bool ascii = in_ascii;
	int32_t off = hit_cell(cx, cy, &ascii);

	if (off < 0) return;
	if ((uint32_t)off >= fsize) return;

	bool pane_changed = (ascii != in_ascii);
	in_ascii = ascii;

	uint32_t before = cursor;
	move_to((uint32_t)off);

	// move_to() redraws nothing when the offset did not change, but a
	// pane switch still has to show.
	if (pane_changed && cursor == before) {
		uint32_t row = cursor / (uint32_t)bytes_per_row;
		if (row >= top_row && row < top_row + (uint32_t)rows)
			draw_row((int)(row - top_row));
	}

}

// -- message handling --
//
// One function, used both by the main loop and, through
// z_dialog_ctx_t, by any dialog that happens to be open. Not a
// convenience: while a dialog is up, wm carries on asking THIS window
// to redraw and blocks waiting for the ack, so an app that only
// serviced redraws from its own main loop would freeze the screen
// every time it opened a dialog (zdialog.h, docs/window_manager.md).
static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:
			// No layout() -- moving changes no size, and every rect
			// here is content-relative.
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:
			// Arrives BEFORE the Z_WM_REDRAW that follows a resize
			// (zwm.h guarantees the ordering), so the layout is
			// already right by the time the repaint is asked for.
			z_win_apply_resized(&win, &msg->obj);
			layout();
			scroll_to_cursor();
			break;

		default:
			break;

	}

}

int main(void) {

	printf("hex: starting\n");

	// Explicit, not left to .bss zero-init -- that has been shown
	// unreliable on this hardware at least once (see
	// docs/app_runtime.md), and a cache full of garbage with
	// cache_valid set would draw a file that does not exist.
	memset(cache, 0, sizeof(cache));
	cache_drop();

	path[0] = 0;

	// Take the launch argument BEFORE creating the window, so the
	// window is created with its final title. Retitling afterwards is
	// fire-and-forget and races with this app's own first paint --
	// sw/apps/text hit exactly that and left a file browser's chrome
	// drawn inside its own window.
	char launch_arg[Z_FLIST_PATH_MAX];
	bool have_arg = z_launch_arg_take(launch_arg, sizeof(launch_arg));

	char initial_title[32];
	build_title(initial_title, (int)sizeof(initial_title),
		have_arg ? launch_arg : "", false);

	for (int i = 0; i < (int)sizeof(sent_title); i++)
		sent_title[i] = initial_title[i];

	// CLOSE_ICON without CLOSE_KILLS_OWNER. This app owns more than
	// one window at a time (a dialog is a window), and the killing
	// form takes every window of a pid down the instant any one of
	// them is clicked closed -- see that flag's warning in zwm.h. It
	// also has to be the non-killing form for a second reason: being
	// killed outright would leak the open file handle, which nothing
	// sweeps.
	if (z_win_create_flags(&win, initial_title, WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		Z_WIN_FLAG_NEW_ICON | Z_WIN_FLAG_OPEN_ICON |
		Z_WIN_FLAG_SAVE_ICON | Z_WIN_FLAG_FONT_ICON) != Z_OK) {
		printf("hex: failed to create window -- is wm running?\n");
		return 1;
	}

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	layout();

	if (have_arg) open_file(launch_arg);

	sync_scrollbar();
	update_title();
	repaint();

	for (;;) {

		z_msg_t msg;

		// Drain the whole queue each pass rather than one message per
		// iteration -- see Z_WM_MOUSE's note in zwm.h on why handling
		// one per loop makes an app fall progressively behind the
		// cursor.
		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:

					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_MOUSE:

					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);

					break;

				case Z_WM_TITLEBAR_ICON: {

					if (msg.obj.type != Z_UINT32) break;

					uint32_t v = msg.obj.val.uint32;
					if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;

					switch (Z_WM_UNPACK_TBICON_KIND(v)) {
						case Z_WM_TBICON_NEW:  do_new(); break;
						case Z_WM_TBICON_OPEN: do_open(); break;
						case Z_WM_TBICON_SAVE: do_save(); break;
						case Z_WM_TBICON_FONT: do_font(); break;
						default: break;
					}

					break;

				}

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id)
						do_close();

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		// Block until something arrives. Everything here is driven by
		// wm messages, so a spinning loop would take a full scheduler
		// share from whatever is in the foreground for no work at all
		// -- see docs/app_runtime.md, "Who blocks, and how".
		z_proc_wait(0);

	}

	return 0;

}
