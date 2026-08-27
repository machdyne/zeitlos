/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * `page` -- see page.h for the full design writeup (why the sparse
 * line index exists, why it needed Z_SYS_FS_SEEK, and the
 * single-session-per-process rule).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zport.h"
#include "../../common/zfsapp.h"
#include "page.h"

// screen geometry -- matches `term`'s own fixed VT100 size (VT_ROWS/
// VT_COLS, sw/common/zvt100.h), same as the repl Makefile already
// pins TE_DEFAULT_ROWS/COLS to. Hardcoded rather than #included from
// zvt100.h: this app doesn't otherwise link the VT100 emulator, and
// pulling that header in for two integers would be misleading about
// the dependency.
#define PAGE_ROWS 25
#define PAGE_COLS 80

// how many text lines are visible at once -- one row is reserved at
// the bottom for the status line.
#define PAGE_TEXT_ROWS (PAGE_ROWS - 1)

// read buffer for the byte-at-a-time line scanner below. Purely an
// I/O batching decision: every miss costs one fs_read_chunk() syscall
// (and, underneath it, a FatFs read), so reading a block at a time
// rather than a byte at a time is the difference between a redraw
// being instant and being visibly slow on an SD card.
#define PAGE_BUF_SIZE 512

// one rendered output line, worst case: PAGE_COLS characters plus
// CRLF plus NUL.
#define PAGE_LINE_MAX (PAGE_COLS + 3)

static bool		pg_active = false;
static z_port_t	*pg_port = NULL;
static int		pg_handle = -1;
static char		pg_name[64];

// -- sparse line index (page.h) --
// pg_idx[i] is the byte offset at which line (i * PAGE_IDX_STRIDE)
// starts. pg_idx[0] is always 0. pg_idx_count grows as the reader
// scrolls further into the file and never shrinks during a session.
static uint32_t	pg_idx[PAGE_IDX_MAX];
static uint32_t	pg_idx_count;

// current viewport: the line number at the top of the screen, and the
// byte offset that line starts at (kept together so a redraw never has
// to re-derive one from the other).
static uint32_t	pg_top_line;
static uint32_t	pg_top_off;

// set once the scanner has actually reached end-of-file, along with
// the total line count discovered at that point. Until then the total
// is genuinely unknown -- the file has not been read to the end and
// this module deliberately never pre-scans one (a pre-scan of a 2MB
// book at startup is exactly the "wait, why is this slow" behavior
// paging exists to avoid). The status line says so rather than
// guessing.
static bool		pg_eof_known;
static uint32_t	pg_total_lines;

// -- buffered sequential reader --
//
// pg_buf holds PAGE_BUF_SIZE bytes starting at file offset
// pg_buf_off; pg_buf_len is how many are valid and pg_buf_pos how far
// into it the scanner has consumed. pg_pos (below) is the absolute
// file offset of the next byte page_getc() will return, which is what
// every line-offset recorded in the index is derived from.
static uint8_t	pg_buf[PAGE_BUF_SIZE];
static uint32_t	pg_buf_off;
static uint32_t	pg_buf_len;
static uint32_t	pg_buf_pos;
static uint32_t	pg_pos;

// escape-sequence reassembly for arrow/PgUp/PgDn keys. A VT100 sends
// these as several bytes (ESC '[' final, or ESC '[' digits '~') and
// repl hands us one byte at a time, so the state has to persist
// across page_feed() calls -- same reason te_bridge.c's own input
// path can't treat bytes independently either.
static uint8_t	pg_esc_state;	// 0 = normal, 1 = saw ESC, 2 = saw ESC [
static uint8_t	pg_esc_param;	// first digit of a "ESC [ n ~" sequence

// -- output --

// Every redraw is assembled into ONE buffer and sent with a single
// z_port_send(). Sending each line separately would issue 25 messages
// per keystroke, which both risks Z_PORT_MAX_PENDING_SENDS (zport.h's
// own flow-control note) and is needlessly chatty -- exactly the
// batching te_bridge.c already does for te's many small writes, for
// the same reasons.
//
// Sized for a full screen of worst-case-width lines plus the control
// sequences framing them.
#define PAGE_OUT_MAX (PAGE_ROWS * PAGE_LINE_MAX + 256)

static void page_send(const char *s, uint32_t len) {
	if (!pg_port || !len) return;
	// a failed send is logged rather than silently dropped, matching
	// repl.c's own conn_send_str() -- the connection may simply have
	// gone away mid-redraw, which is not worth tearing the session
	// down over (the next Z_PORT_CLOSE will do that properly).
	if (z_port_send(pg_port, s, len) != Z_OK)
		printf("page: send failed (%lu bytes)\n", (unsigned long)len);
}

// -- file reading --

// refills pg_buf from the file at the current pg_pos. Returns false at
// end-of-file (or on a read error -- deliberately not distinguished
// here: for a read-only viewer both mean "there is nothing more to
// show", and the status line's "no more" is an honest description of
// either).
static bool page_fill(void) {

	if (!fs_seek(pg_handle, pg_pos)) return false;

	int n = fs_read_chunk(pg_handle, pg_buf, PAGE_BUF_SIZE);
	if (n <= 0) return false;

	pg_buf_off = pg_pos;
	pg_buf_len = (uint32_t)n;
	pg_buf_pos = 0;

	return true;

}

// next byte, or -1 at EOF. Refills through the buffer above when the
// current block runs out.
//
// Note this re-seeks on every refill rather than relying on the
// handle's own position advancing naturally. That's deliberate: the
// position is also moved by page_seek_to() below, and having exactly
// one place that establishes where the next read starts is much easier
// to reason about than two mechanisms that have to agree.
static int page_getc(void) {

	if (pg_buf_pos >= pg_buf_len) {
		if (!page_fill()) return -1;
	}

	pg_pos++;
	return (int)pg_buf[pg_buf_pos++];

}

// repositions the scanner to an absolute byte offset, discarding
// whatever the buffer currently holds.
static void page_seek_to(uint32_t off) {
	pg_pos = off;
	pg_buf_len = 0;
	pg_buf_pos = 0;
	pg_buf_off = off;
}

// records `off` as the start of `line` in the sparse index, if `line`
// happens to fall on a stride boundary and the slot isn't filled yet.
// Cheap enough to call for every line the scanner passes -- which is
// what makes the index build itself as a side effect of ordinary
// scrolling, with no separate indexing pass anywhere.
static void page_index_note(uint32_t line, uint32_t off) {

	if (line % PAGE_IDX_STRIDE) return;

	uint32_t slot = line / PAGE_IDX_STRIDE;
	if (slot >= PAGE_IDX_MAX) return;

	// only ever fill the NEXT unfilled slot -- entries are strictly
	// increasing and discovered in order, so a slot that's already set
	// is already correct and rewriting it would only risk corrupting a
	// good anchor with a bad one after some future change.
	if (slot == pg_idx_count) {
		pg_idx[slot] = off;
		pg_idx_count = slot + 1;
	}

}

// reads one line starting at the current position into `out` (at most
// `cap`-1 chars, NUL-terminated), consuming its terminating newline.
// Returns false if there was nothing left to read at all.
//
// Lines longer than the screen are TRUNCATED for display, not wrapped:
// the rest of the line is skipped so the next line still starts on the
// next row. Wrapping would make the line/offset bookkeeping above
// ambiguous (one file line could occupy several screen rows, so "page
// down 24 lines" would no longer mean a fixed number of file lines),
// and a viewer whose scrolling arithmetic depends on content width is
// a much harder thing to keep correct than one that truncates. Worth
// revisiting if reading wide files turns out to matter in practice.
static bool page_read_line(char *out, uint32_t cap) {

	uint32_t n = 0;
	int c = page_getc();

	if (c < 0) return false;

	while (c >= 0 && c != '\n') {
		// '\r' dropped rather than shown: a CRLF file would otherwise
		// render a stray control character at the end of every single
		// line.
		if (c != '\r' && n + 1 < cap) {
			// tabs expanded to a single space, and any other control
			// byte shown as '.' -- a binary file opened by mistake
			// then scrolls harmlessly instead of filling the terminal
			// with escape sequences it will try to interpret.
			if (c == '\t') out[n++] = ' ';
			else if (c < 32 || c == 127) out[n++] = '.';
			else out[n++] = (char)c;
		}
		c = page_getc();
	}

	out[n] = 0;
	return true;

}

// positions the viewport at `line`, using the sparse index to start
// from the nearest anchor at or before it and scanning forward from
// there. Clamps to the last line of the file if `line` is past the
// end. Updates pg_top_line/pg_top_off to whatever it actually reached.
static void page_goto_line(uint32_t line) {

	uint32_t slot = line / PAGE_IDX_STRIDE;
	if (slot >= pg_idx_count) slot = pg_idx_count ? pg_idx_count - 1 : 0;

	uint32_t cur = slot * PAGE_IDX_STRIDE;
	page_seek_to(pg_idx_count ? pg_idx[slot] : 0);

	char tmp[PAGE_LINE_MAX];

	while (cur < line) {

		uint32_t off = pg_pos;
		page_index_note(cur, off);

		if (!page_read_line(tmp, sizeof(tmp))) {
			// hit EOF while scanning forward -- `cur` is now the total
			// line count, which is worth recording: it's the only way
			// this module ever learns the file's length, and it makes
			// the status line and the end-of-file clamp below exact
			// from here on.
			pg_eof_known = true;
			pg_total_lines = cur;
			break;
		}

		cur++;

	}

	page_index_note(cur, pg_pos);

	pg_top_line = cur;
	pg_top_off = pg_pos;

}

// -- rendering --

static void page_draw(void) {

	static char out[PAGE_OUT_MAX];
	uint32_t o = 0;

#define PG_PUT(s) do { \
		const char *_p = (s); \
		while (*_p && o + 1 < sizeof(out)) out[o++] = *_p++; \
	} while (0)

	// home BEFORE erase: VT100_ERASE_SCREEN is "\e[J", which clears
	// from the cursor to the END of the screen -- issued from wherever
	// the cursor happens to be, it would leave everything above it
	// intact.
	PG_PUT(VT100_CURSOR_HOME);
	PG_PUT(VT100_ERASE_SCREEN);

	// redraw always re-reads from pg_top_off rather than trusting the
	// scanner to still be sitting there -- the previous keypress may
	// have left it anywhere.
	page_seek_to(pg_top_off);

	char line[PAGE_LINE_MAX];
	uint32_t shown = 0;
	uint32_t cur = pg_top_line;

	while (shown < PAGE_TEXT_ROWS) {

		page_index_note(cur, pg_pos);

		if (!page_read_line(line, sizeof(line))) {
			pg_eof_known = true;
			pg_total_lines = cur;
			break;
		}

		PG_PUT(line);
		PG_PUT("\r\n");

		cur++;
		shown++;

	}

	// pad the rest of the screen so a short final page doesn't leave
	// the previous screen's text visible below it.
	while (shown < PAGE_TEXT_ROWS) {
		PG_PUT("\r\n");
		shown++;
	}

	// The key hints and the position are fixed-width and always shown;
	// the FILENAME is what gets clamped when the three together don't
	// fit the screen. Deliberately that way round -- someone paging a
	// file already knows what they opened, while the key hints are the
	// only discoverability this viewer has, and the earlier version of
	// this (one snprintf() with the filename first, then a blunt
	// status[PAGE_COLS] = 0) cut the hints off instead for any
	// long-named file.
	static const char *hints = "[spc/b pg  arrows ln  g/G ends  q quit]";

	char pos[24];
	if (pg_eof_known)
		snprintf(pos, sizeof(pos), "%lu/%lu",
			(unsigned long)pg_top_line + 1, (unsigned long)pg_total_lines);
	else
		// no total yet: the file hasn't been read to the end and this
		// module never pre-scans one (see pg_eof_known's own comment),
		// so show the current line alone rather than inventing a
		// denominator.
		snprintf(pos, sizeof(pos), "%lu", (unsigned long)pg_top_line + 1);

	// whatever's left of the row after the hints, the position, and
	// the two-space gaps between the three fields belongs to the name.
	int name_room = PAGE_COLS - (int)strlen(hints) - (int)strlen(pos) - 4;
	if (name_room < 8) name_room = 8;
	if (name_room > PAGE_COLS) name_room = PAGE_COLS;

	// sized for the worst case the clamps above can actually produce
	// (the floor and the ceiling can't both bind at once), with room
	// to spare so this can't become the next silent truncation.
	char status[PAGE_COLS * 2];
	snprintf(status, sizeof(status), "%.*s  %s  %s",
		name_room, pg_name, pos, hints);

	PG_PUT(VT100_REVERSE);
	PG_PUT(status);
	PG_PUT(VT100_ATTR_RESET);

#undef PG_PUT

	page_send(out, o);

}

// -- navigation --

static void page_scroll(int32_t delta) {

	if (delta < 0) {
		uint32_t back = (uint32_t)(-delta);
		uint32_t target = (pg_top_line > back) ? pg_top_line - back : 0;
		page_goto_line(target);
		return;
	}

	// forward from where we already are, rather than through
	// page_goto_line()'s anchor-and-rescan path: scrolling down is the
	// overwhelmingly common case and the scanner is already positioned
	// correctly for it, so this costs one line-read per line moved
	// instead of potentially re-scanning from an anchor far behind.
	page_seek_to(pg_top_off);

	char tmp[PAGE_LINE_MAX];
	uint32_t cur = pg_top_line;
	uint32_t moved = 0;

	while (moved < (uint32_t)delta) {

		page_index_note(cur, pg_pos);

		if (!page_read_line(tmp, sizeof(tmp))) {
			pg_eof_known = true;
			pg_total_lines = cur;
			break;
		}

		cur++;
		moved++;

	}

	// don't scroll so far that the screen would be entirely blank --
	// stop with the last screenful of the file visible, which is what
	// every pager does at the bottom of a document.
	if (pg_eof_known && pg_total_lines > PAGE_TEXT_ROWS &&
		cur > pg_total_lines - PAGE_TEXT_ROWS) {
		page_goto_line(pg_total_lines - PAGE_TEXT_ROWS);
		return;
	}

	if (pg_eof_known && pg_total_lines <= PAGE_TEXT_ROWS) {
		page_goto_line(0);
		return;
	}

	pg_top_line = cur;
	pg_top_off = pg_pos;

}

// jumps to the end of the file. This is the one operation that has to
// read the whole thing (there is no other way to find the last line),
// so it's bound to an explicit keypress and never done implicitly --
// see pg_eof_known's own comment. Once it's been done once, the index
// is fully populated and the total line count is known for the rest of
// the session, which makes every subsequent jump cheap.
static void page_goto_end(void) {

	if (!pg_eof_known) {
		page_seek_to(pg_top_off);
		char tmp[PAGE_LINE_MAX];
		uint32_t cur = pg_top_line;
		while (1) {
			page_index_note(cur, pg_pos);
			if (!page_read_line(tmp, sizeof(tmp))) break;
			cur++;
		}
		pg_eof_known = true;
		pg_total_lines = cur;
	}

	if (pg_total_lines > PAGE_TEXT_ROWS)
		page_goto_line(pg_total_lines - PAGE_TEXT_ROWS);
	else
		page_goto_line(0);

}

// -- public API --

bool page_start(z_port_t *port, const char *filename,
	char *out, uint32_t out_cap) {

	if (pg_active) {
		snprintf(out, out_cap,
			"a page session is already open (one at a time -- quit it "
			"with 'q' first)");
		return false;
	}

	if (!port || !filename || !*filename) {
		snprintf(out, out_cap, "usage: page <filename>");
		return false;
	}

	int sz = fs_size((char *)filename);
	if (sz <= 0) {
		// fs_size() can't tell "missing" from "genuinely empty"
		// (zfsapp.h's own documented ambiguity) -- say both rather
		// than picking one and being wrong half the time.
		snprintf(out, out_cap, "'%s' not found (or empty)", filename);
		return false;
	}

	int h = fs_open_read(filename);
	if (h < 0) {
		snprintf(out, out_cap, "couldn't open '%s'", filename);
		return false;
	}

	pg_handle = h;
	pg_port = port;
	pg_active = true;

	snprintf(pg_name, sizeof(pg_name), "%s", filename);

	pg_idx[0] = 0;
	pg_idx_count = 1;
	pg_top_line = 0;
	pg_top_off = 0;
	pg_eof_known = false;
	pg_total_lines = 0;
	pg_esc_state = 0;
	pg_esc_param = 0;

	page_seek_to(0);
	page_draw();

	out[0] = 0;
	return true;

}

// ends the session and restores the terminal. Split out from
// page_feed() because page_abort() needs the file-handle half of it
// too (see page.h on why leaking a handle actually matters).
static void page_finish(bool restore_screen) {

	if (pg_handle >= 0) fs_close_handle(pg_handle);
	pg_handle = -1;
	pg_active = false;

	if (restore_screen) {
		const char *done =
			VT100_ATTR_RESET VT100_CURSOR_HOME VT100_ERASE_SCREEN;
		page_send(done, (uint32_t)strlen(done));
	}

	pg_port = NULL;

}

bool page_feed(uint8_t byte) {

	if (!pg_active) return false;

	// -- escape sequence reassembly (see pg_esc_state) --
	if (pg_esc_state == 1) {
		if (byte == '[') { pg_esc_state = 2; return true; }
		pg_esc_state = 0;
		return true;	// a bare ESC, or something we don't handle
	}

	if (pg_esc_state == 2) {

		if (byte >= '0' && byte <= '9') {
			pg_esc_param = byte;
			return true;	// wait for the '~'
		}

		pg_esc_state = 0;

		if (byte == '~') {
			if (pg_esc_param == '5') page_scroll(-(int32_t)PAGE_TEXT_ROWS);
			else if (pg_esc_param == '6') page_scroll((int32_t)PAGE_TEXT_ROWS);
			else { pg_esc_param = 0; return true; }
			pg_esc_param = 0;
			page_draw();
			return true;
		}

		switch (byte) {
			case 'A': page_scroll(-1); break;	// up arrow
			case 'B': page_scroll(1); break;	// down arrow
			case 'H': page_goto_line(0); break;	// home
			case 'F': page_goto_end(); break;	// end
			default: return true;				// left/right etc: ignored
		}

		page_draw();
		return true;

	}

	if (byte == 27) { pg_esc_state = 1; pg_esc_param = 0; return true; }

	// -- plain keys --
	switch (byte) {

		case 'q':
		case 'Q':
			page_finish(true);
			return false;

		case ' ':
		case 'f':
			page_scroll((int32_t)PAGE_TEXT_ROWS);
			break;

		case 'b':
			page_scroll(-(int32_t)PAGE_TEXT_ROWS);
			break;

		case 'j':
		case '\r':
		case '\n':
			page_scroll(1);
			break;

		case 'k':
			page_scroll(-1);
			break;

		case 'g':
			page_goto_line(0);
			break;

		case 'G':
			page_goto_end();
			break;

		default:
			// unknown key: deliberately no redraw and no beep, so
			// holding down an unbound key can't flood the connection
			// with full-screen repaints.
			return true;

	}

	page_draw();
	return true;

}

bool page_active(void) {
	return pg_active;
}

void page_abort(void) {
	if (!pg_active) return;
	// no screen restore: the connection this was drawing to is already
	// gone, so there's nothing to restore it on -- same reasoning
	// te_bridge_abort() gives for not trying to save first.
	page_finish(false);
}
