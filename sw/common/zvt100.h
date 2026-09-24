#ifndef ZVT100_H
#define ZVT100_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * A VT100/ANSI terminal emulation core: an 80x25 character screen
 * buffer plus the escape-sequence state machine that drives it.
 * Deliberately independent of windowing (zwin.h) and messaging --
 * feed it bytes (vt_feed()/vt_feed_byte()), read back the resulting
 * cell buffer (vt_screen_t.cells) or which rows changed
 * (vt_row_dirty()). This is on purpose: it can be built and tested
 * standalone (see sw/apps/term/term.c's test harness, which feeds it
 * a hardcoded byte string and dumps the result to the UART console --
 * no window, no wm, no live keyboard needed to validate the parser
 * itself), the same "verify one layer before wiring it to the next"
 * approach docs/user_input.md's keyboard work used. `term` proper
 * wires this to a real window (zwin.h) and Z_WM_KEY input in a later
 * phase -- see docs/ports.md for where this fits in the wider plan.
 *
 * Genuine VT100 (not later ANSI/xterm extensions) is actually a good
 * match for this hardware: the framebuffer is 1bpp (monochrome), and
 * real VT100 had no color either -- just cursor movement, erase, and
 * a handful of text attributes (this implementation supports reverse
 * video, the one attribute a 1bpp framebuffer can represent for free
 * as a bit-flip; bold/underline/blink are accepted -- parsed, not
 * rejected -- but currently no-ops, see vt_feed_byte()'s SGR handling).
 */

#include <stdint.h>
#include <stdbool.h>

#define VT_COLS 80
#define VT_ROWS 25

typedef struct {
	char ch;		// an ISO 8859-15 byte -- 0x20-0x7e or 0xa0-0xff; 0x7f
					// for a character Latin-9 cannot draw (the font's
					// missing-glyph box); VT_CH_WIDE_RIGHT for the right
					// half of a wide one; 0x20 (space) when blank.
					// See "characters" below.
	bool reverse;
	uint16_t cp;	// in the LEFT cell of a wide character: which one it is
					// (Basic Multilingual Plane; 0 beyond it or for any other
					// cell). The glyph byte there stays the box, so a reader
					// that ignores this sees what it always saw.	// the only SGR attribute this 1bpp framebuffer can
					// represent -- see the file header comment
} vt_cell_t;

/* -- scrollback history --
 *
 * Lines that scroll off the top of the screen can be kept in a ring
 * the CALLER owns and hands over with vt_history_attach(). This module
 * still allocates nothing: a test can attach a few kilobytes on the
 * stack, `term` attaches a .bss array, and anything that attaches
 * nothing gets exactly the old behaviour.
 *
 * -- characters --
 *
 * The parser decodes UTF-8 (vt_feed_byte()): what arrives from a shell
 * or an ssh session is UTF-8 (docs/text_encoding.md). A cell holds the
 * ISO 8859-15 byte of the character -- the byte the hardware font
 * draws it with -- or 0x7f, the missing-glyph box, for one Latin-9 has
 * not got. A character that is two columns wide (CJK, kana: see
 * z_cp_width() in zutf8.h) takes two cells, the second holding
 * VT_CH_WIDE_RIGHT, so everything after it lands in the column the
 * remote program expects; the left cell also keeps the character's
 * codepoint (vt_cell_t.cp), so it can be drawn from the Japanese font,
 * copied and read aloud as itself. Combining marks take none.
 *
 * -- a history line is VT_HIST_LINE_BYTES --
 *
 * VT_COLS glyph bytes, then a VT_COLS-bit bitmap of which cells are in
 * reverse video (bit c%8 of byte c/8), then one of which cells start a
 * wide character whose codepoint is kept: for those, the cell's glyph
 * byte and its right neighbour's hold the codepoint's high and low
 * bytes instead of the box and VT_CH_WIDE_RIGHT. The cells used to be packed 7+1
 * bits into one byte each, which stopped fitting when a cell became a
 * whole Latin-9 byte; the bitmap costs a tenth of what 16-bit cells
 * would. Readers never see the storage: vt_doc_cell() and vt_id_cell()
 * return a vt_packed_t, VT_PACK()ed.
 *
 * -- what goes in, and what does not --
 *
 * A line enters history only when a LINEFEED scrolls it off the top.
 *
 * Not when DL (ESC[M) deletes it at row 0, even though that shares
 * scroll_up() with the linefeed. A full-screen editor scrolls with DL
 * (nextvi's term_room()), and saving those would fill the history
 * with successive pictures of the editor instead of the shell output
 * underneath it. Not when ED 2 clears the screen either -- the same
 * choice xterm makes. ED 3 (ESC[3J) clears the history itself, which
 * is also what xterm does with it.
 *
 * -- addressing --
 *
 * Two ways to name a line, for two different jobs:
 *
 *   DOCUMENT index: 0 is the oldest retained history line, count-1
 *   the newest, count..count+VT_ROWS-1 the live screen rows. Dense,
 *   so it is what a scrolled view and a scrollbar want.
 *
 *   ABSOLUTE id: history_pushed - count + document index. Every push
 *   shifts every document index by one, but a given line's absolute
 *   id never changes -- screen row r is always id pushed + r, and the
 *   same text is still that id after it scrolls into history. That is
 *   what lets a selection stay attached to its text while output
 *   keeps arriving. An id below pushed - count has been evicted.
 */
// A cell as the readers get it: the glyph byte in bits 7:0, reverse
// video in bit 8, and for the left cell of a wide character its
// codepoint in bits 31:16 (VT_PACK_WIDE_CP; 0 for every other cell).
typedef uint32_t vt_packed_t;
#define VT_PACK(ch, rev)	((vt_packed_t)((uint8_t)(ch) | ((rev) ? 0x100u : 0u)))
#define VT_PACK_CP(ch, rev, cp)	(VT_PACK(ch, rev) | ((vt_packed_t)(uint16_t)(cp) << 16))
#define VT_PACK_WIDE_CP(b)	((uint32_t)((b) >> 16))
#define VT_PACK_CH(b)		((char)((b) & 0xff))
#define VT_PACK_REV(b)		(((b) & 0x100) != 0)

// The right half of a wide character. A C1 control byte, so never a
// real character's glyph byte; draws as blank.
#define VT_CH_WIDE_RIGHT	((char)0x80)

#define VT_HIST_LINE_BYTES	(VT_COLS + 2 * ((VT_COLS + 7) / 8))

typedef enum {
	VT_PSTATE_NORMAL,	// ordinary bytes -- print, or act on C0 controls
	VT_PSTATE_ESC,		// just saw ESC (0x1b), waiting for '[' (CSI) or
						// another final byte (unsupported final bytes
						// -- anything but '[' -- are accepted and
						// silently dropped, not treated as an error;
						// see vt_feed_byte())
	VT_PSTATE_CSI		// saw ESC '[', collecting "ESC [ params final"
} vt_pstate_t;

#define VT_CSI_MAX_PARAMS 8

typedef struct {

	vt_cell_t cells[VT_ROWS][VT_COLS];

	int cursor_x, cursor_y;	// 0-indexed; cursor_x can transiently
								// equal VT_COLS right after the last
								// column is written (deferred wrap --
								// see vt_feed_byte()), always back in
								// [0,VT_COLS) by the time the next byte
								// is processed

	bool reverse;				// current SGR state, applied to newly
								// written (and newly erased) cells

	// escape sequence parser state -- see vt_pstate_t above
	vt_pstate_t pstate;
	int csi_params[VT_CSI_MAX_PARAMS];
	bool csi_has_param[VT_CSI_MAX_PARAMS];	// distinguishes "0" from
								// "no digits given, use the command's
								// own default" -- CSI's default isn't
								// always 0 (see vt_feed_byte()'s CUP)
	int csi_param_count;

	// which rows changed since the caller last checked -- see
	// vt_row_dirty()/vt_clear_dirty(). a renderer built on top of this
	// (term proper, not the standalone test harness) can redraw only
	// dirty rows instead of the whole 80x25 grid on every byte.
	bool dirty[VT_ROWS];

	// UTF-8 being decoded: the codepoint so far, how many continuation
	// bytes are still due, and the smallest value this length may
	// encode (anything below is an overlong form).
	uint32_t u8_cp;
	uint8_t u8_need;
	uint32_t u8_min;


	/* Rows scrolled off the top since the last vt_take_scrolls().
	 *
	 * Lets a renderer move the pixels that survived a scroll rather
	 * than redrawing every cell -- about 4x cheaper for a full
	 * screen. Purely advisory: a renderer that ignores it is still
	 * correct, just slower.
	 *
	 * Saturates rather than wrapping. Once it exceeds the screen
	 * height nothing survives the scroll anyway, so the exact value
	 * stops mattering and a wrap to a small number would be actively
	 * wrong. */
	uint16_t scrolls;

	/* Scrollback ring -- see "scrollback history" above. hist_buf is
	 * NULL when none is attached. hist_head is the slot the NEXT line
	 * is written to; hist_count <= hist_cap. hist_pushed counts every
	 * line ever pushed and never goes backwards (ED 3 included), so
	 * an absolute id stays meaningful across a clear. */
	uint8_t *hist_buf;
	uint16_t hist_cap;
	uint16_t hist_head;
	uint16_t hist_count;
	uint32_t hist_pushed;

} vt_screen_t;

/* How many rows have scrolled off the top since this was last called,
 * and reset the count.
 *
 * "Take" rather than "get" because reading it clears it: the renderer
 * is claiming responsibility for those scrolls, and leaving them for a
 * second caller would shift the screen twice. */
static inline uint16_t vt_take_scrolls(vt_screen_t *vt) {
	uint16_t n = vt->scrolls;
	vt->scrolls = 0;
	return n;
}

// resets to a blank screen, cursor at (0,0), no pending escape state.
// Detaches any history -- call vt_history_attach() AFTER this.
void vt_init(vt_screen_t *vt);

// Gives the screen a scrollback ring of `cap_lines` lines. `buf` must
// be cap_lines * VT_HIST_LINE_BYTES bytes and outlive the screen.
// Starts empty.
// NULL or 0 detaches.
void vt_history_attach(vt_screen_t *vt, uint8_t *buf, uint16_t cap_lines);

// Lines currently retained, 0..cap.
static inline uint16_t vt_history_count(const vt_screen_t *vt) {
	return vt->hist_count;
}

// Total lines ever pushed. Absolute id of screen row r is this + r.
static inline uint32_t vt_history_pushed(const vt_screen_t *vt) {
	return vt->hist_pushed;
}

// Drops every retained line. hist_pushed is NOT reset -- see above.
void vt_history_clear(vt_screen_t *vt);

// One cell of DOCUMENT line `doc` (0 = oldest retained history line,
// count.. = live screen), packed with VT_PACK(). Out of range reads as
// a blank cell rather than failing, so a renderer never has to guard.
vt_packed_t vt_doc_cell(const vt_screen_t *vt, int doc, int col);

// One cell of ABSOLUTE line `id`. Returns false, and a blank cell, if
// that line has been evicted or is below the bottom of the screen.
bool vt_id_cell(const vt_screen_t *vt, uint32_t id, int col, vt_packed_t *out);

// feed one byte through the parser -- printable characters are
// written at the cursor (with wrap/scroll as needed); C0 controls
// (CR/LF/BS/TAB/BEL) and CSI escape sequences are interpreted; any
// unrecognized escape or CSI final byte is silently absorbed rather
// than either crashing or leaking raw escape bytes into the visible
// grid.
void vt_feed_byte(vt_screen_t *vt, uint8_t c);

// convenience wrapper -- feeds len bytes in order.
void vt_feed(vt_screen_t *vt, const uint8_t *data, uint32_t len);

// true if row (0-indexed) has changed since the last vt_clear_dirty().
bool vt_row_dirty(const vt_screen_t *vt, int row);

// marks every row as not-dirty. call after a renderer has redrawn
// every row vt_row_dirty() reported.
void vt_clear_dirty(vt_screen_t *vt);

// marks every row dirty -- for when something outside this module
// invalidated previously-rendered content (e.g. a caller redrawing a
// window from scratch after it moved, where the framebuffer itself
// needs a full repaint, not just whatever changed since the last
// vt_feed()/vt_feed_byte()).
void vt_mark_all_dirty(vt_screen_t *vt);

#endif
