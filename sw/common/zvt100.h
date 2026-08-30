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
	char ch;		// 0x20-0x7e, or 0x20 (space) for an erased/blank cell
	bool reverse;	// the only SGR attribute this 1bpp framebuffer can
					// represent -- see the file header comment
} vt_cell_t;

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
void vt_init(vt_screen_t *vt);

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
