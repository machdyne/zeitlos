/*
 * term -- VT100 terminal emulator
 *
 * An 80x25 VT100 window (sw/common/zvt100.h) that relays keystrokes to
 * a PORT (sw/common/zport.h, docs/ports.md) and renders whatever comes
 * back. It is a dumb terminal: line editing, echo and command
 * interpretation all belong to whoever is on the other end -- see
 * docs/terminal.md, "Line editing lives at the far end".
 *
 * -- It starts disconnected --
 *
 * term used to connect itself to `repl0` at startup, with a fixed-pid
 * fallback, and drop to local echo if that failed. repl is no longer a
 * core app (it lives on the sdcard now, next to posix -- see
 * docs/flash_apps.md), so a card-less board would have come up with a
 * terminal quietly echoing its own keys, and a board WITH a card has
 * two equally good shells to choose between.
 *
 * So term now opens on a START PANEL: REPL, POSIX and OPEN (F11)
 * buttons, each shell's button live only once that shell has
 * registered. One click, or Tab/Enter, decides. Typing anything else
 * starts the Open bar with it, so `telnet host` can simply be typed.
 * F12 disconnects and returns here from any connection, and so does a
 * connection the far end closes. There is no local echo any more:
 * a terminal connected to nothing has nothing to say.
 *
 * Unless /zeitlos.cfg says otherwise: apps.term.auto_connect (e.g.
 * "port repl0", "telnet bbs.example.com") makes every new window
 * connect there by itself, waiting a few seconds for the provider to
 * register if boot has not got that far yet. See auto_*() below and
 * docs/config.md.
 *
 * -- Scrollback --
 *
 * TERM_HIST_LINES lines (Makefile: SCROLLBACK, default 200 -- eight
 * screens) of what scrolled off the top, one byte per cell in a .bss
 * ring owned here and lent to zvt100. Shift+PgUp/PgDn/Up/Down/Home/End
 * move through it, and so does the scrollbar down the right-hand side,
 * which is why the window is Z_SB_THICK wider than 80 columns.
 * Selection and copy work across it. See docs/terminal.md.
 *
 * -- Other ways in --
 *
 * Z_TERM_SET_PORT (sw/common/zterm.h) still hands this window to
 * another provider: repl's `telnet`/`ssh`/`port` commands, and posix
 * handing the terminal to a child such as `vi`.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zwidget.h"	// the start panel's buttons, the scrollbar
#include "../../common/zfont.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"
#include "../../common/zvt100.h"
#include "../../common/zport.h"
#include "../../common/zterm.h"
#include "../../common/zconnect.h"	// the Open bar -- see bar_*() below
#include "../../common/zcfg.h"		// apps.term.auto_connect

// which font to render with -- override at build time with
// `make term FONT=z_font_6x12`. Defaults to z_font_5x8, adopted after
// real-hardware testing showed z_font_5x7's bottom row cut off and
// z_font_6x12 costing too much blit work and screen margin (see
// docs/user_input.md's "Debugging notes").
//
// wm is the only process that loads glyph data into hardware glyph
// memory, and it only ever loads z_font_5x8. A Z_GFX_HW_BLIT build
// with a different FONT renders z_font_5x8's glyph data reinterpreted
// at the wrong dimensions -- garbled, not just wrong-sized. Don't
// override FONT for a hardware build until wm loads more than one.
#ifndef TERM_FONT_NAME
#define TERM_FONT_NAME z_font_5x8
#endif
#define TERM_FONT TERM_FONT_NAME

// Scrollback depth, in lines. The Makefile's SCROLLBACK sets this.
//
// Costs TERM_HIST_LINES * VT_COLS bytes of .bss per term instance, and
// .bss is RAM for the process's whole lifetime (docs/boot.md, "Memory
// budget"). 200 lines is eight screens and 16,000 bytes.
//
// The upper bound is zvt100's uint16_t line count; the lower bound is
// a screen, because anything less cannot show even the previous page.
#ifndef TERM_HIST_LINES
#define TERM_HIST_LINES 200
#endif
#if TERM_HIST_LINES < VT_ROWS || TERM_HIST_LINES > 8000
#error "TERM_HIST_LINES (Makefile SCROLLBACK) must be 25..8000"
#endif

// How long connect_port() waits for CONNECTED/REFUSED for a
// Z_TERM_SET_PORT Z_MAP handoff. Longer than a local port needs because
// net's telnet provider does not reply until a TCP handshake resolves
// either way, and tcp.c's own retry budget alone is ~31.5s -- a shorter
// timeout here gave up before net had finished trying, so the error
// shown was never the real one. See Z_CONN_TIMEOUT_NETWORK_TICKS.
#define TERM_TELNET_CONNECT_TIMEOUT_TICKS Z_CONN_TIMEOUT_NETWORK_TICKS

// How long a connect runs silently before the bar says it is waiting.
// A local port answers in a few ticks, and a bar that flashes up for
// one frame on every REPL click reads as a glitch; anything slower than
// this is slow enough to need explaining.
#define TERM_CONNECT_QUIET_TICKS (Z_TICK_HZ / 4)

// -- geometry --
//
// The window is sized so the content area (z_win_content_rect(): a
// 2px inset on each side, and Z_WM_TITLEBAR_H + 2 at the top) is
// EXACTLY the 80x25 text grid plus the scrollbar strip. Every content
// pixel therefore belongs either to a glyph cell or to the scrollbar,
// and each of those repaints its own pixels in full -- which is why
// there is no z_win_clear() anywhere in this file.
//
// Runtime values rather than macros because the font is swappable and
// z_font_t's w/h are not preprocessor constants.
static int cell_w, cell_h;		// one character cell
static int text_w, text_h;		// the 80x25 grid
static int term_win_w, term_win_h;

static vt_screen_t vt;
static uint8_t hist_buf[TERM_HIST_LINES * VT_COLS];
static z_win_t win;
static z_port_t port;
static z_scrollbar_t sbar;

static char instance_name[24] = "term";

// apps.term.auto_connect is waiting for its provider -- see auto_*().
static bool auto_pending;

// Lines scrolled back from live. 0 is the live screen; the maximum is
// vt_history_count(). Document line (count - view_off + row) is shown
// at display row `row` -- see zvt100.h on document indices.
static int view_off;

// -- forward declarations --
static void frame(void);
static void handle_key_event(uint32_t packed);
static void handle_mouse_event(uint32_t packed);
static bool connect_port(const char *name, z_obj_t arg,
	uint32_t timeout_ticks, const char *label);
static void auto_cancel(const char *status);

/* -- render instrumentation --
 *
 * 0 in a normal build. Set to 1 to print render counts to the serial
 * console every two seconds. Prints to the console rather than the
 * window because drawing the numbers with the blitter being measured
 * would change the answer.
 *
 * Measured cost per glyph (docs/gpu_blitter.md): ~112 cycles. A full
 * 2000-glyph repaint is ~4.7ms; the hardware scroll that replaces it
 * for a one-line scroll is ~1.07ms plus one row of glyphs. */
#define TERM_INSTRUMENT 0

#if TERM_INSTRUMENT
static uint32_t ins_renders, ins_glyphs, ins_blits, ins_full;
static uint32_t ins_last_report;

static void ins_report(void) {
	uint32_t now = z_uptime_ticks();
	if (ins_last_report == 0) { ins_last_report = now; return; }
	if (now - ins_last_report < Z_TICK_HZ * 2) return;
	if (ins_renders)
		printf("term: %lu renders, %lu glyphs, %lu blits, %lu full\n",
			(unsigned long)ins_renders, (unsigned long)ins_glyphs,
			(unsigned long)ins_blits, (unsigned long)ins_full);
	ins_renders = ins_glyphs = ins_blits = ins_full = 0;
	ins_last_report = now;
}
#define INS(x) (x)
#else
#define ins_report() ((void)0)
#define INS(x) ((void)0)
#endif

// ---------------------------------------------------------------
// selection
// ---------------------------------------------------------------
//
// Reading order, not a column rectangle: partial rows at each end and
// whole rows between, the way a terminal selection works.
//
// Endpoints are ABSOLUTE line ids (zvt100.h), not screen rows. A line
// keeps its id as it scrolls from the screen into history, so a
// selection stays on its text while output keeps arriving and while
// the view scrolls -- screen rows would leave the highlight standing
// still while the text moved out from under it, which is what this
// used to do.
//
// Anchor plus current, so extending backwards needs no special case.

static bool sel_active;			// something is selected
static bool sel_dragging;		// left button held since the press
static uint32_t sel_a_id, sel_c_id;
static int sel_a_col, sel_c_col;

// Normalised bounds, recomputed by sel_prepare() for the render and
// copy that follow it. sel_on is false when nothing selected survives.
static bool sel_on;
static uint32_t sel_id0, sel_id1;
static int sel_col0, sel_col1;

// Previous mouse button mask, for right-button edge detection.
static uint8_t last_buttons;

// Normalises and clamps against eviction. Returns true if that changed
// what is highlighted -- the whole selection scrolled out of history,
// and the cells that showed it must be repainted.
static bool sel_prepare(void) {

	bool was_on = sel_on;

	sel_on = false;
	if (!sel_active) return was_on;

	if (sel_a_id < sel_c_id || (sel_a_id == sel_c_id && sel_a_col <= sel_c_col)) {
		sel_id0 = sel_a_id; sel_col0 = sel_a_col;
		sel_id1 = sel_c_id; sel_col1 = sel_c_col;
	} else {
		sel_id0 = sel_c_id; sel_col0 = sel_c_col;
		sel_id1 = sel_a_id; sel_col1 = sel_a_col;
	}

	uint32_t first = vt_history_pushed(&vt) - vt_history_count(&vt);

	if (sel_id1 < first) {
		sel_active = false;
		return was_on;
	}

	if (sel_id0 < first) { sel_id0 = first; sel_col0 = 0; }

	sel_on = true;
	return false;

}

static bool sel_contains(uint32_t id, int col) {
	if (id < sel_id0 || id > sel_id1) return false;
	if (id == sel_id0 && col < sel_col0) return false;
	if (id == sel_id1 && col > sel_col1) return false;
	return true;
}

// ---------------------------------------------------------------
// the glass model
// ---------------------------------------------------------------
//
// shadow[][] is what is ON THE SCREEN for each cell: character in the
// low 8 bits, inverted in bit 8. render() works out what each cell
// SHOULD show -- the emulator's content at the current scroll offset,
// inverted for the selection and for the cursor -- and draws only the
// cells where the two differ.
//
// Everything that changes the picture goes through that one
// comparison: output, the cursor, the selection, scrolling the view,
// and an overlay (the Open bar, the start panel) arriving or leaving.
// That is deliberate, and it is what fixed the Open bar that would not
// go away: the bar used to be painted straight over the bottom row
// with the shadow left describing the session underneath, so when the
// bar was dismissed every cell compared EQUAL and nothing was redrawn.
// Now any cell an overlay covers is marked GLASS_UNKNOWN, which can
// never compare equal, so the session's cells come back by themselves
// the moment the overlay stops covering them.

#define GLASS_UNKNOWN 0xFFFF

static uint16_t shadow[VT_ROWS][VT_COLS];

// Examine every cell on the next render, not just dirty rows.
static bool render_all;

// What the shadow was last brought up to date against. drawn_valid is
// false after a wm redraw, when the pixels are unknown and nothing may
// be assumed about them -- in particular no hardware scroll.
static bool drawn_valid;
static int drawn_view_off;
static uint16_t drawn_count;
static uint32_t drawn_pushed;
static int drawn_cursor_row = -1;

static void shadow_invalidate(void) {
	for (int r = 0; r < VT_ROWS; r++)
		for (int c = 0; c < VT_COLS; c++)
			shadow[r][c] = GLASS_UNKNOWN;
}

static void shadow_invalidate_rect(int r0, int c0, int r1, int c1) {
	for (int r = r0; r <= r1; r++)
		for (int c = c0; c <= c1; c++)
			shadow[r][c] = GLASS_UNKNOWN;
}

// Moves the shadow by the same amount a hardware scroll just moved the
// pixels: `up` rows up if positive, down if negative. The rows that
// scrolled in hold nothing known.
//
// The blit and this shift MUST agree exactly. If they disagree the
// comparison finds cells equal that are not, and the terminal shows
// stale text with no way to notice.
static void shadow_shift(int up) {

	if (up > 0) {
		for (int r = 0; r + up < VT_ROWS; r++)
			for (int c = 0; c < VT_COLS; c++)
				shadow[r][c] = shadow[r + up][c];
		shadow_invalidate_rect(VT_ROWS - up, 0, VT_ROWS - 1, VT_COLS - 1);
	} else if (up < 0) {
		int dn = -up;
		for (int r = VT_ROWS - 1; r >= dn; r--)
			for (int c = 0; c < VT_COLS; c++)
				shadow[r][c] = shadow[r - dn][c];
		shadow_invalidate_rect(0, 0, dn - 1, VT_COLS - 1);
	}

}

// One cell, straight to the glyph blitter (z_fb_draw_char2(), which
// takes both colours -- see its comment in zgfx.c). `clip` is the
// content rect, fetched once per render rather than once per cell.
static void draw_glyph(const z_clip_t *clip, int col, int row, char ch,
	bool inverted) {

	z_fb_draw_char2(clip->x0 + col * cell_w, clip->y0 + row * cell_h, ch,
		inverted ? 0 : 1, inverted ? 1 : 0, &TERM_FONT, clip);

	INS(ins_glyphs++);

}

// -- overlays --
//
// The Open bar owns the bottom row while it is up; the start panel owns
// a block of cells in the middle while it is visible. Both draw
// themselves (bar_draw(), panel_draw()) after render(), which leaves
// every cell they own alone and marks it GLASS_UNKNOWN.

#define PANEL_C0 12
#define PANEL_C1 67
#define PANEL_R0 5
#define PANEL_R1 19

static bool bar_active;
static bool panel_visible;

static bool overlay_owns(int row, int col) {

	if (bar_active && row == VT_ROWS - 1) return true;

	if (panel_visible && row >= PANEL_R0 && row <= PANEL_R1
		&& col >= PANEL_C0 && col <= PANEL_C1)
		return true;

	return false;

}

// ---------------------------------------------------------------
// render
// ---------------------------------------------------------------

static void render(void) {

	z_clip_t clip;
	z_win_content_rect(&win, &clip);

	INS(ins_renders++);

	uint16_t n = vt_take_scrolls(&vt);
	uint16_t count = vt_history_count(&vt);
	uint32_t pushed = vt_history_pushed(&vt);
	uint32_t p = pushed - drawn_pushed;

	bool dirty[VT_ROWS];
	bool any_dirty = false;
	for (int r = 0; r < VT_ROWS; r++) {
		dirty[r] = vt_row_dirty(&vt, r);
		if (dirty[r]) any_dirty = true;
	}
	vt_clear_dirty(&vt);

	/* Scrolled back while output arrives: stay on the same TEXT.
	 *
	 * Each line pushed shifts every document index by one, so without
	 * this the view would creep towards live one line per line of
	 * output and a long build log would drag whatever you were reading
	 * away from you. Bumping view_off by the push count keeps the top
	 * line's absolute id where it was -- until the ring is full and
	 * starts evicting, at which point there is nothing left to stay on
	 * and the clamp below lets it go. */
	if (view_off > 0 && p > 0) {
		uint32_t v = (uint32_t)view_off + p;
		view_off = (v > count) ? (int)count : (int)v;
	}
	if (view_off > (int)count) view_off = count;
	if (view_off < 0) view_off = 0;

	bool all = render_all || !drawn_valid;
	render_all = false;

	if (sel_prepare()) all = true;

	// The cursor is drawn only while connected: disconnected, there is
	// nothing to type at, and a block sitting in the corner of the old
	// session reads as a prompt that is not there.
	int cur_row = -1, cur_col = 0;
	if (port.connected) {
		cur_col = (vt.cursor_x >= VT_COLS) ? VT_COLS - 1 : vt.cursor_x;
		cur_row = vt.cursor_y + view_off;
		if (cur_row >= VT_ROWS) cur_row = -1;
	}

	/* -- move surviving pixels instead of redrawing them --
	 *
	 * Two cases shift the WHOLE picture uniformly, and only those two
	 * are accelerated:
	 *
	 *   - live before and after, and the screen scrolled n rows. The
	 *     whole display IS the screen, so everything moved up by n.
	 *     That includes DL at row 0, which is how vi scrolls.
	 *
	 *   - no output at all since the last render, and the view offset
	 *     changed. The whole display is the same document shown from a
	 *     different line.
	 *
	 * Anything else -- output arriving while scrolled back, a history
	 * clear, both at once -- is not a uniform shift, and the full
	 * comparison below handles it correctly, just without the blit.
	 *
	 * Never while an overlay is up: its pixels would be carried along
	 * with the text and nothing would put them back.
	 *
	 * z_fb_hw_scroll_allowed() first, because z_fb_hw_scroll() refuses
	 * silently for a partly covered window. Shifting the shadow for a
	 * blit that did not happen is precisely the stale-text failure the
	 * shift exists to avoid. */
	if (drawn_valid && !bar_active && !panel_visible) {

		int shift = 0;

		if (view_off == 0 && drawn_view_off == 0)
			shift = n;
		else if (n == 0 && p == 0 && count == drawn_count)
			shift = drawn_view_off - view_off;

		if (shift != 0 && shift > -VT_ROWS && shift < VT_ROWS &&
			z_fb_hw_scroll_allowed(clip.x0, clip.y0, text_w, text_h)) {

			z_fb_hw_scroll(clip.x0, clip.y0, text_w, text_h,
				-shift * cell_h);
			shadow_shift(shift);
			all = true;
			INS(ins_blits++);

		}

	}

	if (view_off != drawn_view_off || count != drawn_count) all = true;
	if (view_off > 0 && (any_dirty || n)) all = true;

	INS(ins_full += all);

	int doc0 = (int)count - view_off;
	uint32_t top_id = pushed - (uint32_t)view_off;

	for (int row = 0; row < VT_ROWS; row++) {

		// Live and not forced: only rows the emulator touched, plus
		// the rows the cursor is leaving and arriving on.
		if (!all && !dirty[row] && row != cur_row && row != drawn_cursor_row)
			continue;

		int doc = doc0 + row;
		uint32_t id = top_id + (uint32_t)row;

		for (int col = 0; col < VT_COLS; col++) {

			if (overlay_owns(row, col)) {
				shadow[row][col] = GLASS_UNKNOWN;
				continue;
			}

			uint8_t b;
			if (doc >= (int)count) {
				vt_cell_t *cell = &vt.cells[doc - count][col];
				b = VT_PACK(cell->ch, cell->reverse);
			} else {
				b = vt_doc_cell(&vt, doc, col);
			}

			bool inv = VT_PACK_REV(b);
			if (sel_on && sel_contains(id, col)) inv = !inv;
			if (row == cur_row && col == cur_col) inv = !inv;

			uint16_t want = (uint16_t)((uint8_t)VT_PACK_CH(b) | (inv ? 0x100u : 0u));
			if (shadow[row][col] == want) continue;

			shadow[row][col] = want;
			draw_glyph(&clip, col, row, VT_PACK_CH(b), inv);

		}

	}

	drawn_valid = true;
	drawn_view_off = view_off;
	drawn_count = count;
	drawn_pushed = pushed;
	drawn_cursor_row = cur_row;

	ins_report();

}

// -- view --

static void view_set(int off) {
	int max = vt_history_count(&vt);
	if (off < 0) off = 0;
	if (off > max) off = max;
	view_off = off;
}

static void view_scroll_by(int lines) { view_set(view_off + lines); }
static void view_live(void) { view_set(0); }

// ---------------------------------------------------------------
// the Open bar (F11)
// ---------------------------------------------------------------
//
// A one-line prompt across the bottom row where you type a target --
// "telnet 10.0.0.5", "serial 9600", "port posix0", "ssh me@host" --
// and Enter connects. Escape cancels. The four kinds and all the work
// of resolving them live in sw/common/zconnect.h, shared with repl.
//
// A line, not a dialog: a widget panel would need its own window (wm
// has no modal dialogs), and F11 is most useful exactly when whatever
// you were connected to has stopped answering -- a prompt bar needs
// nothing from anyone.
//
// It is an overlay (see overlay_owns()), not text written into the
// emulator: writing it into the vt would destroy a row of the session
// with nothing to restore it from.

#define BAR_MAX 72

static bool bar_dirty;
static char bar_buf[BAR_MAX];
static int  bar_len;
static char bar_msg[VT_COLS + 1];	// shown instead of the prompt when set

static void bar_draw(const z_clip_t *clip) {

	if (!bar_active || !bar_dirty) return;

	char line[VT_COLS + 1];
	int row = VT_ROWS - 1;

	if (bar_msg[0])
		snprintf(line, sizeof(line), "%s", bar_msg);
	else
		snprintf(line, sizeof(line), "open> %s_", bar_buf);

	// Every column, not just the text: the row is overwritten to its
	// last cell so nothing of the session shows through past the end.
	// Length taken once rather than testing line[i], because the bytes
	// past the terminator are stack garbage.
	int len = (int)strlen(line);
	for (int i = 0; i < VT_COLS; i++)
		draw_glyph(clip, i, row, i < len ? line[i] : ' ', true);

	bar_dirty = false;

}

static void bar_open(void) {

	// Reaching for the bar is taking over from auto-connect.
	auto_cancel(NULL);

	bar_active = true;
	bar_buf[0] = 0;
	bar_len = 0;
	bar_msg[0] = 0;
	bar_dirty = true;

	// Ownership starts NOW, not at the next render: the shadow must
	// stop describing the session's bottom row before the bar is drawn
	// over it, or dismissing the bar finds nothing to redraw.
	shadow_invalidate_rect(VT_ROWS - 1, 0, VT_ROWS - 1, VT_COLS - 1);

}

static void bar_dismiss(void) {

	if (!bar_active) return;

	bar_active = false;
	bar_msg[0] = 0;

	// The row's shadow is already GLASS_UNKNOWN, so examining it is
	// all it takes to bring the session's own content back.
	render_all = true;

}

static void bar_set_msg(const char *msg) {
	snprintf(bar_msg, sizeof(bar_msg), "%s", msg);
	bar_dirty = true;
}

static void bar_insert(uint32_t keysym) {
	if (keysym >= 0x20 && keysym < 0x7f && bar_len < BAR_MAX - 1) {
		bar_buf[bar_len++] = (char)keysym;
		bar_buf[bar_len] = 0;
	}
	bar_dirty = true;
}

// Parse what was typed and go. Never called with an empty buffer.
static void bar_submit(void) {

	z_conn_kind_t kind;
	z_conn_target_t target;
	char word[12], err[128], msg[VT_COLS + 1];
	const char *rest;
	size_t wl = 0;

	while (bar_buf[wl] && bar_buf[wl] != ' ' && wl < sizeof(word) - 1) {
		word[wl] = bar_buf[wl];
		wl++;
	}
	word[wl] = 0;

	if (!z_conn_kind_from_word(word, &kind)) {
		bar_set_msg("open: try port|serial|telnet|ssh   (Esc cancels)");
		return;
	}

	rest = bar_buf + wl;
	while (*rest == ' ') rest++;

	// z_conn_prepare() BLOCKS for telnet and ssh -- a DNS lookup or an
	// ssh prepare can take seconds with no messages read and no
	// repaint. Say so on the glass BEFORE calling it, or the terminal
	// simply freezes with no explanation. See zconnect.h.
	snprintf(msg, sizeof(msg), "open: %s %.60s ...", z_conn_kind_name(kind), rest);
	bar_set_msg(msg);
	frame();

	if (!z_conn_prepare(kind, rest, &target, err, sizeof(err))) {
		// Left up rather than dismissed: an error you have to press a
		// key to clear is an error you actually read.
		snprintf(msg, sizeof(msg), "%.80s", err);
		bar_set_msg(msg);
		return;
	}

	connect_port(target.provider, target.arg, target.timeout_ticks,
		target.detail[0] ? target.detail : target.provider);

}

// Returns true if the key was consumed by the bar -- which, while the
// bar is up, is every key: otherwise Escape and F12 would be two
// answers to one question.
static bool bar_key(uint32_t keysym) {

	if (!bar_active) return false;

	// Any key clears a message and returns to editing, so an error
	// does not have to be dismissed separately from the prompt.
	if (bar_msg[0]) {
		bar_msg[0] = 0;
		bar_dirty = true;
		if (keysym == 0x1b) bar_dismiss();
		return true;
	}

	switch (keysym) {

	case 0x1b:					// Esc
		bar_dismiss();
		return true;

	case 0x0d:					// Enter
		if (!bar_len) { bar_dismiss(); return true; }
		bar_submit();
		return true;

	case 0x08:					// Backspace
	case 0x7f:
		if (bar_len) bar_buf[--bar_len] = 0;
		bar_dirty = true;
		return true;

	default:
		bar_insert(keysym);
		return true;

	}

}

// ---------------------------------------------------------------
// the start panel
// ---------------------------------------------------------------
//
// Shown whenever this window is not connected to anything: at startup,
// after F12, and when the far end closes the connection. Three
// buttons -- REPL, POSIX, OPEN -- plus what each is and a status line
// saying why you are here.
//
// A block of cells rather than a separate window, for the same reason
// the Open bar is a line: a second window means a focus question when
// it closes, and this is part of the terminal, not something in front
// of it. The session's old text stays visible around it, and Esc hides
// it to read what it covers.
//
// The shell buttons are enabled only while their provider is
// REGISTERED (pidreg), rechecked twice a second while the panel is up.
// At boot, term can be on screen before init has finished loading repl
// and posix off the card; the buttons coming alive as each one appears
// says that far better than a click that fails. On a card-less board
// they stay disabled, which is the truth -- see docs/flash_apps.md.

enum { PB_REPL = 0, PB_POSIX, PB_OPEN, PB_COUNT };

static z_widget_t panel_items[PB_COUNT];
static z_widget_set_t panel_set;

static bool panel_dirty;
static char panel_status[VT_COLS + 1];

// Set once the user moves focus with Tab or an arrow. Until then focus
// follows the first ready shell -- at boot the panel is often up before
// repl0 has registered, and focus left on OPEN because that was the
// only live button would make Enter do the less likely thing.
static bool panel_focus_user;
static bool repl_up, posix_up;
static uint32_t panel_probe_at;

// Pixel geometry, content-relative. The block is PANEL_C0..C1 x
// PANEL_R0..R1 in cells, so it lands exactly on cell boundaries and
// every pixel it covers belongs to a cell it owns.
#define PANEL_TEXT_X   10
#define PANEL_TITLE_Y   6
#define PANEL_BTN_Y    22
#define PANEL_BTN_W    72
#define PANEL_BTN_H    16
#define PANEL_BTN_GAP  16
#define PANEL_DESC_Y   48
#define PANEL_STATUS_Y 82
#define PANEL_HINT_Y   96

static int panel_px(void) { return PANEL_C0 * cell_w; }
static int panel_py(void) { return PANEL_R0 * cell_h; }
static int panel_pw(void) { return (PANEL_C1 - PANEL_C0 + 1) * cell_w; }
static int panel_ph(void) { return (PANEL_R1 - PANEL_R0 + 1) * cell_h; }

static void panel_layout(void) {

	static const char *labels[PB_COUNT] = { "REPL", "POSIX", "OPEN F11" };

	int total = PB_COUNT * PANEL_BTN_W + (PB_COUNT - 1) * PANEL_BTN_GAP;
	int x = panel_px() + (panel_pw() - total) / 2;

	memset(panel_items, 0, sizeof(panel_items));

	for (int i = 0; i < PB_COUNT; i++) {
		panel_items[i].type = Z_WIDGET_BUTTON;
		panel_items[i].x = (int16_t)(x + i * (PANEL_BTN_W + PANEL_BTN_GAP));
		panel_items[i].y = (int16_t)(panel_py() + PANEL_BTN_Y);
		panel_items[i].w = PANEL_BTN_W;
		panel_items[i].h = PANEL_BTN_H;
		panel_items[i].label = labels[i];
		panel_items[i].enabled = (i == PB_OPEN);
	}

	z_widget_set_init(&panel_set, panel_items, PB_COUNT, &win);

}

// Looks the two shells up. `force` skips the half-second throttle.
static void panel_probe(bool force) {

	uint32_t now = z_uptime_ticks();
	uint32_t pid;

	if (!force && now - panel_probe_at < Z_TICK_HZ / 2) return;
	panel_probe_at = now;

	bool r = z_pid_lookup("repl0", &pid);
	bool p = z_pid_lookup("posix0", &pid);

	if (r == repl_up && p == posix_up && !force) return;

	repl_up = r;
	posix_up = p;

	panel_items[PB_REPL].enabled = r;
	panel_items[PB_POSIX].enabled = p;

	// A focused button that just became disabled would leave Enter
	// doing nothing, and until the user has chosen, the first ready
	// shell is the better default than OPEN.
	int f = panel_set.focused;
	if (!panel_focus_user) {
		int want = r ? PB_REPL : (p ? PB_POSIX : PB_OPEN);
		if (want != f) z_widget_focus_set(&panel_set, want);
	} else if (f < 0 || !panel_items[f].enabled) {
		z_widget_focus_next(&panel_set, false);
	}

	panel_dirty = true;

}

static void panel_show(const char *status) {

	if (status) snprintf(panel_status, sizeof(panel_status), "%s", status);

	if (!panel_visible) {
		shadow_invalidate_rect(PANEL_R0, PANEL_C0, PANEL_R1, PANEL_C1);
		panel_focus_user = false;
	}

	panel_visible = true;
	panel_dirty = true;
	panel_probe(true);

}

static void panel_hide(void) {
	if (!panel_visible) return;
	panel_visible = false;
	render_all = true;
}

static void panel_draw(void) {

	if (!panel_visible) return;

	if (!panel_dirty) {
		// Focus moves and presses mark single widgets dirty.
		z_widget_draw_all(&panel_set, false);
		return;
	}

	z_clip_t clip;
	z_win_content_rect(&win, &clip);

	int x = panel_px(), y = panel_py(), w = panel_pw(), h = panel_ph();
	char line[VT_COLS + 1];

	z_win_fill_rect(&win, x, y, w, h, 0);
	z_win_hw_box(&win, clip.x0 + x + 1, clip.y0 + y + 1,
		clip.x0 + x + w - 2, clip.y0 + y + h - 2, 1);

	snprintf(line, sizeof(line), "%s -- not connected", instance_name);
	z_win_draw_text2(&win, x + (w - (int)strlen(line) * cell_w) / 2,
		y + PANEL_TITLE_Y, line, 1, 0, &TERM_FONT);

	z_widget_draw_all(&panel_set, true);

	snprintf(line, sizeof(line), "%-6s %-31s %11s", "REPL",
		"Scheme and system commands", repl_up ? "ready" : "not running");
	z_win_draw_text2(&win, x + PANEL_TEXT_X, y + PANEL_DESC_Y, line, 1, 0, &TERM_FONT);

	snprintf(line, sizeof(line), "%-6s %-31s %11s", "POSIX",
		"Unix-style shell, zcc and vi", posix_up ? "ready" : "not running");
	z_win_draw_text2(&win, x + PANEL_TEXT_X, y + PANEL_DESC_Y + 10, line, 1, 0, &TERM_FONT);

	snprintf(line, sizeof(line), "%-6s %s", "OPEN",
		"port, serial, telnet or ssh");
	z_win_draw_text2(&win, x + PANEL_TEXT_X, y + PANEL_DESC_Y + 20, line, 1, 0, &TERM_FONT);

	if (panel_status[0])
		z_win_draw_text2(&win, x + PANEL_TEXT_X, y + PANEL_STATUS_Y,
			panel_status, 1, 0, &TERM_FONT);

	z_win_draw_text2(&win, x + PANEL_TEXT_X, y + PANEL_HINT_Y,
		"Tab chooses, Enter connects, or type a target", 1, 0, &TERM_FONT);
	z_win_draw_text2(&win, x + PANEL_TEXT_X, y + PANEL_HINT_Y + 10,
		"F12 disconnects  Esc hides  Shift+PgUp scrolls", 1, 0, &TERM_FONT);

	panel_dirty = false;

}

static void panel_activate(int idx) {

	switch (idx) {
	case PB_REPL:
		connect_port("repl0", z_obj_none(), Z_CONN_TIMEOUT_LOCAL_TICKS, "repl0");
		break;
	case PB_POSIX:
		connect_port("posix0", z_obj_none(), Z_CONN_TIMEOUT_LOCAL_TICKS, "posix0");
		break;
	case PB_OPEN:
		bar_open();
		break;
	default:
		break;
	}

}

static bool panel_contains_px(int cx, int cy) {
	return cx >= panel_px() && cx < panel_px() + panel_pw() &&
		cy >= panel_py() && cy < panel_py() + panel_ph();
}

// Keys while disconnected and the bar is not up.
static void panel_key(uint32_t keysym, uint8_t mods) {

	// Hidden with Esc to read what was underneath: any key brings it
	// back, and is spent doing so rather than acting on a panel the
	// user could not see.
	if (!panel_visible) { panel_show(NULL); return; }

	int idx;

	switch (keysym) {

	case 0x09:					// Tab / Shift+Tab
		z_widget_focus_next(&panel_set, (mods & Z_KBD_MOD_SHIFT) != 0);
		panel_focus_user = true;
		break;

	case Z_KEY_LEFT:
		z_widget_focus_next(&panel_set, true);
		panel_focus_user = true;
		break;

	case Z_KEY_RIGHT:
		z_widget_focus_next(&panel_set, false);
		panel_focus_user = true;
		break;

	case 0x0d:					// Enter
	case ' ':
		idx = z_widget_key_activate(&panel_set);
		if (idx >= 0) panel_activate(idx);
		break;

	case 0x1b:					// Esc
		// While auto-connect is waiting, Esc stops the wait -- the
		// panel says it is waiting, so that is what Esc is for. The
		// next Esc hides the panel as usual.
		if (auto_pending) auto_cancel("auto-connect cancelled");
		else panel_hide();
		break;

	default:
		// Anything printable starts the Open bar with it, so a target
		// can simply be typed: `port posix0`, `telnet myhost`.
		if (keysym > 0x20 && keysym < 0x7f) {
			bar_open();
			bar_insert(keysym);
		}
		break;

	}

}

// ---------------------------------------------------------------
// connections
// ---------------------------------------------------------------

// Set for the duration of connect_port()'s wait. Input handlers check
// it: the wait services keys and the pointer so the window stays
// responsive, but a click or an Enter that started a SECOND connect
// from inside the first would recurse through this function.
static bool connecting;
static bool connect_cancel;

// Leaves whatever this window is connected to and shows the panel.
static void term_disconnect(const char *status) {

	if (port.connected) {
		z_port_close(&port);
		port.connected = false;
	}

	bar_dismiss();
	panel_show(status);

}

static void handle_redraw(uint32_t packed) {

	z_win_apply_redraw(&win, packed);

	// The window has been repainted underneath us, so the shadow no
	// longer describes the glass, and neither overlay is on it.
	shadow_invalidate();
	drawn_valid = false;
	bar_dirty = true;
	panel_dirty = true;
	z_widget_invalidate(&panel_set);
	sbar.dirty = true;

}

/*
 * Closes the current connection (if any) and connects to `name`, a
 * pidreg name ("repl0", "posix0", "net0"). `arg` travels with the
 * CONNECT (z_port_connect_arg()); `label` is what the status line
 * calls the target.
 *
 * On success the panel and bar go away and the view returns to live.
 * On failure the panel says why.
 *
 * -- why this is an inline copy of z_port_connect_arg_timeout() --
 *
 * That function's wait loop discards everything that is not
 * CONNECTED/REFUSED, which is right for its other callers and wrong
 * here. This window keeps receiving Z_WM_REDRAW while it waits, and wm
 * waits for a Z_WM_REDRAW_DONE in return (repair_region()/repair_drag(),
 * wm.c) -- discarding it left wm timing out and this window's cached
 * position stale for the rest of its life, found on real hardware
 * during a 45-second telnet connect. So this services redraws, clip
 * updates, moves and keys while it waits, matching wm.c's own
 * wait_for_redraw_done() convention.
 *
 * Z_PORT_DATA arriving meanwhile is ACKED and dropped: it belongs to
 * the connection just closed, and an unacked DATA holds one of the
 * sender's Z_PORT_MAX_PENDING_SENDS slots forever.
 */
static bool connect_port(const char *name, z_obj_t arg,
	uint32_t timeout_ticks, const char *label) {

	char status[VT_COLS + 1];
	uint32_t target_pid;

	if (connecting) return false;

	// Any connection, however it was asked for, supersedes a pending
	// auto-connect.
	auto_pending = false;

	if (port.connected) {
		z_port_close(&port);
		port.connected = false;
	}

	if (!z_pid_lookup(name, &target_pid)) {
		printf("term: '%s' not found\n", name);
		snprintf(status, sizeof(status), "%.40s is not running", name);
		bar_dismiss();
		panel_show(status);
		return false;
	}

	port.peer_pid = target_pid;
	port.conn_id = 0;
	port.connected = false;

	z_msg_new_send(target_pid, Z_PORT_CONNECT, 0, arg);

	connecting = true;
	connect_cancel = false;

	uint32_t start = z_uptime_ticks();
	bool shown = false;
	bool ok = false;
	status[0] = 0;

	while (!connect_cancel && (z_uptime_ticks() - start) < timeout_ticks) {

		// Only once it has been slow enough to notice -- see
		// TERM_CONNECT_QUIET_TICKS.
		if (!shown && (z_uptime_ticks() - start) >= TERM_CONNECT_QUIET_TICKS) {
			char msg[VT_COLS + 1];
			if (!bar_active) bar_open();
			snprintf(msg, sizeof(msg), "connecting to %.45s ...  (Esc cancels)",
				label);
			bar_set_msg(msg);
			frame();
			shown = true;
		}

		z_msg_t msg;
		if (z_msg_read(&msg) != Z_OK) {
			z_proc_wait(1);
			continue;
		}

		if (msg.subject == Z_PORT_CONNECTED && msg.tag == 0) {
			port.conn_id = msg.obj.val.uint32;
			port.connected = true;
			ok = true;
			break;
		}

		if (msg.subject == Z_PORT_REFUSED && msg.tag == 0) {
			if (msg.obj.type == Z_STR && msg.obj.val.str)
				snprintf(status, sizeof(status), "%.30s refused: %.40s",
					label, msg.obj.val.str);
			else
				snprintf(status, sizeof(status), "%.55s refused the connection",
					label);
			break;
		}

		if (msg.subject == Z_WM_SET_CLIP) {
			// Not optional: wm waits for the ack when a region
			// narrows. See z_win_apply_clip() in zwin.c.
			z_win_apply_clip(&win, &msg.obj);
		} else if (msg.subject == Z_WM_REDRAW) {
			handle_redraw(msg.obj.val.uint32);
			frame();
			z_win_redraw_done(&win);
		} else if (msg.subject == Z_WM_WINDOW_MOVED) {
			z_win_parse_rect(&win, &msg.obj);
		} else if (msg.subject == Z_WM_KEY) {
			handle_key_event(msg.obj.val.uint32);
			if (connect_cancel) break;
			frame();
		} else if (msg.subject == Z_PORT_DATA) {
			z_port_send_ack(&msg);
		}
		// anything else: not relevant to this wait -- the same
		// discard zport.c's own version documents.

	}

	connecting = false;

	if (ok) {
		printf("term: connected to %s (pid %ld, conn %ld)\n", name,
			(long)port.peer_pid, (long)port.conn_id);
		bar_dismiss();
		panel_hide();
		view_live();
		return true;
	}

	if (connect_cancel)
		snprintf(status, sizeof(status), "cancelled connecting to %.50s", label);
	else if (!status[0])
		snprintf(status, sizeof(status), "no answer from %.50s", label);

	printf("term: %s\n", status);

	bar_dismiss();
	panel_show(status);
	return false;

}

// ---------------------------------------------------------------
// auto-connect (apps.term.auto_connect)
// ---------------------------------------------------------------
//
// The same text the Open bar takes -- "port repl0", "serial 9600",
// "telnet host", "ssh me@host" -- read from /zeitlos.cfg when the
// window opens. Empty, absent or "none" means the start panel, as
// before. See docs/config.md.
//
// -- waiting for the provider --
//
// A window opened at boot can be on screen before the thing it wants
// has registered: init loads repl and posix off the card, and wm
// enables the dock as soon as init has STARTED them, not once they are
// listening. Connecting straight away would fail with "repl0 is not
// running" for a shell that is a second from being ready.
//
// So it waits, up to TERM_AUTO_WAIT_TICKS, for the provider's NAME to
// appear -- the port's own name, serial0 for serial, net0 for telnet
// and ssh -- then connects once. The panel says what it is waiting
// for, and Esc stops it. It never retries after a connect has actually
// been attempted: a refusal is an answer, and a window that keeps
// hammering at a provider that said no is worse than one that shows
// why.
//
// Only at startup. F12 and a closed connection still land on the
// panel; auto-connecting again there would make F12 a way back into
// the thing you were trying to leave.

#define TERM_AUTO_WAIT_TICKS (Z_TICK_HZ * 15)

static char auto_text[BAR_MAX];
static char auto_wait_for[24];
static uint32_t auto_deadline;
static uint32_t auto_next_probe;

static void auto_cancel(const char *status) {
	if (!auto_pending) return;
	auto_pending = false;
	if (status) panel_show(status);
}

// Splits "word rest". Returns false if word is not a connection kind.
static bool auto_parse(const char *text, z_conn_kind_t *kind, const char **rest) {

	char word[12];
	size_t wl = 0;

	while (text[wl] && text[wl] != ' ' && wl < sizeof(word) - 1) {
		word[wl] = text[wl];
		wl++;
	}
	word[wl] = 0;

	if (!z_conn_kind_from_word(word, kind)) return false;

	*rest = text + wl;
	while (**rest == ' ') (*rest)++;
	return true;

}

// Reads the setting and, if there is one, starts waiting.
static void auto_begin(void) {

	char v[Z_CFG_VAL_MAX];
	char status[VT_COLS + 1];
	z_conn_kind_t kind;
	const char *rest;

	z_cfg_get("apps.term.auto_connect", v, sizeof(v));

	if (!v[0] || !strcmp(v, "none")) return;

	if (strlen(v) >= sizeof(auto_text) || !auto_parse(v, &kind, &rest)) {
		snprintf(status, sizeof(status),
			"zeitlos.cfg auto_connect: try port|serial|telnet|ssh");
		printf("term: apps.term.auto_connect '%s' not understood\n", v);
		panel_show(status);
		return;
	}

	snprintf(auto_text, sizeof(auto_text), "%.71s", v);	// length checked above

	switch (kind) {
	case Z_CONN_PORT:   snprintf(auto_wait_for, sizeof(auto_wait_for), "%.23s", rest); break;
	case Z_CONN_SERIAL: snprintf(auto_wait_for, sizeof(auto_wait_for), "serial0"); break;
	default:            snprintf(auto_wait_for, sizeof(auto_wait_for), "net0"); break;
	}

	auto_pending = true;
	auto_deadline = z_uptime_ticks() + TERM_AUTO_WAIT_TICKS;
	auto_next_probe = 0;

	snprintf(status, sizeof(status), "auto-connect: %.40s  (Esc cancels)",
		auto_text);
	panel_show(status);

}

// Called every frame while pending. Throttled to four probes a second.
static void auto_poll(void) {

	uint32_t now, pid;
	char status[VT_COLS + 1];

	if (!auto_pending || connecting) return;

	now = z_uptime_ticks();
	if (now < auto_next_probe) return;
	auto_next_probe = now + Z_TICK_HZ / 4;

	if (!z_pid_lookup(auto_wait_for, &pid)) {
		if (now >= auto_deadline) {
			auto_pending = false;
			snprintf(status, sizeof(status),
				"auto-connect: %.24s did not appear -- choose below", auto_wait_for);
			panel_show(status);
		}
		return;
	}

	// Ready. One attempt, through exactly the Open bar's path.
	auto_pending = false;

	z_conn_kind_t kind;
	const char *rest;
	z_conn_target_t target;
	char err[128];

	auto_parse(auto_text, &kind, &rest);

	if (!z_conn_prepare(kind, rest, &target, err, sizeof(err))) {
		snprintf(status, sizeof(status), "auto-connect: %.60s", err);
		panel_show(status);
		return;
	}

	connect_port(target.provider, target.arg, target.timeout_ticks,
		target.detail[0] ? target.detail : target.provider);

}

// ---------------------------------------------------------------
// selection input, copy and paste
// ---------------------------------------------------------------

static void sel_clear(void) {
	if (!sel_active && !sel_dragging) return;
	sel_active = false;
	sel_dragging = false;
	render_all = true;
}

// One buffer for both directions. z_clip_set() copies into zwin.c's own
// buffer before sending, so a copy and a paste never need this at the
// same time -- and two 4KB statics were 4KB of RAM per term instance
// for no reason.
static char clip_io[Z_WM_CLIP_MAX];

// Copies the selection to the system clipboard.
//
// Trailing blanks on each row are dropped -- a terminal grid is padded
// to the full width, and nothing wants that tail pasted back. Rows
// other than the last get a newline. The clipboard holds
// Z_WM_CLIP_MAX - 1 bytes; a longer selection is cut off there.
static void sel_copy(void) {

	sel_prepare();
	if (!sel_on) return;

	int n = 0;
	const int max = (int)sizeof(clip_io) - 1;

	for (uint32_t id = sel_id0; id <= sel_id1 && n < max; id++) {

		int from = (id == sel_id0) ? sel_col0 : 0;
		int to = (id == sel_id1) ? sel_col1 : VT_COLS - 1;
		char line[VT_COLS];
		int last = from - 1;

		for (int col = from; col <= to; col++) {
			uint8_t b;
			vt_id_cell(&vt, id, col, &b);
			line[col] = VT_PACK_CH(b);
			if (line[col] != ' ') last = col;
		}

		for (int col = from; col <= last && n < max; col++) {
			char ch = line[col];
			clip_io[n++] = (ch >= 0x20 && ch < 0x7f) ? ch : ' ';
		}

		if (id != sel_id1 && n < max) clip_io[n++] = '\n';

	}

	clip_io[n] = 0;
	z_clip_set(clip_io, n);

}

// Pastes the clipboard.
//
// Connected: straight down the same path a keystroke takes, with no
// assumption about line structure -- against `sh` each newline submits
// a command, and whether that is right belongs to the far end.
//
// Not connected: the only thing that takes text is the Open bar, so the
// first line goes there. Pasting a hostname is the case that matters.
static void sel_paste(void) {

	int n = z_clip_get(clip_io, sizeof(clip_io));
	if (n <= 0) return;

	if (port.connected) {
		z_port_send(&port, clip_io, (uint32_t)n);
		view_live();
		return;
	}

	if (!bar_active) bar_open();
	for (int i = 0; i < n && clip_io[i] != '\r' && clip_io[i] != '\n'; i++)
		bar_insert((uint8_t)clip_io[i]);

}

// Content-relative pixel -> display cell, clamped to the grid.
static void cell_at(int cx, int cy, int *row, int *col) {

	int r = (cy < 0) ? 0 : cy / cell_h;
	int c = (cx < 0) ? 0 : cx / cell_w;

	if (r >= VT_ROWS) r = VT_ROWS - 1;
	if (c >= VT_COLS) c = VT_COLS - 1;

	*row = r;
	*col = c;

}

static uint32_t id_at_row(int row) {
	return vt_history_pushed(&vt) - (uint32_t)view_off + (uint32_t)row;
}

static void handle_mouse_event(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool right_edge = (buttons & Z_MOUSE_BTN_RIGHT) &&
		!(last_buttons & Z_MOUSE_BTN_RIGHT);

	last_buttons = buttons;

	if (connecting) return;

	// -- a selection drag owns the pointer until release --
	//
	// Checked before the inside test: wm keeps delivering samples
	// outside the window during a drag, so a selection can run off
	// an edge. Off the top or bottom it scrolls the view a line per
	// sample, which is what makes selecting more than a screen
	// possible with a mouse.
	if (sel_dragging) {

		if (!down) {
			sel_dragging = false;
			// A press that never moved selected nothing.
			if (sel_active && sel_a_id == sel_c_id && sel_a_col == sel_c_col)
				sel_clear();
			return;
		}

		if (cy < 0) view_scroll_by(1);
		else if (cy >= text_h) view_scroll_by(-1);

		int row, col;
		cell_at(cx, cy, &row, &col);
		uint32_t id = id_at_row(row);

		if (id == sel_c_id && col == sel_c_col && sel_active) return;

		sel_c_id = id;
		sel_c_col = col;
		sel_active = true;
		render_all = true;
		return;

	}

	// -- the scrollbar --
	//
	// z_scrollbar_has_pointer() is true during its own drag too, so a
	// thumb drag that wanders sideways keeps working.
	if (sbar.dragging || (inside && z_scrollbar_has_pointer(&sbar, cx, cy))) {
		if (z_scrollbar_mouse(&sbar, cx, cy, buttons))
			view_set(vt_history_count(&vt) - sbar.value);
		return;
	}

	// -- the panel's buttons --
	//
	// A press on a button owns the pointer until release, wherever the
	// release lands -- z_widget_mouse() cancels a release outside.
	if (panel_visible && (panel_set.pressed >= 0 ||
		(inside && panel_contains_px(cx, cy)))) {
		int idx = z_widget_mouse(&panel_set, cx, cy, buttons);
		if (idx >= 0) panel_activate(idx);
		return;
	}

	// Samples over the titlebar reach us too -- wm's hit test is the
	// whole window rect.
	if (!inside) return;

	// Right button copies, acting on the press: there is no drag
	// gesture on it, so waiting for the release adds nothing. After
	// the titlebar guard, so a right-click on the titlebar does not.
	if (right_edge) sel_copy();

	if (down && cx < text_w && cy < text_h) {

		int row, col;
		cell_at(cx, cy, &row, &col);

		sel_clear();
		sel_a_id = sel_c_id = id_at_row(row);
		sel_a_col = sel_c_col = col;
		sel_dragging = true;

	}

}

// ---------------------------------------------------------------
// keys
// ---------------------------------------------------------------

// Translates one keysym into 0+ raw bytes for the port. The arrow/nav/
// F-key sequences are the common xterm-ish convention (ESC[A, ESC[5~,
// ESC[15~), not strict 1978 VT100, which had no F5-F12 and no nav
// cluster -- matching what people actually connect to matters more.
//
// Backspace is a single DEL (0x7f), the byte a real terminal sends;
// what it looks like on screen is the far end's echo to decide.
static int key_to_bytes(uint32_t keysym, char *buf, int buflen) {

	static const struct { uint32_t keysym; const char *seq; } table[] = {
		{ Z_KEY_UP,       "\x1b[A" },
		{ Z_KEY_DOWN,     "\x1b[B" },
		{ Z_KEY_RIGHT,    "\x1b[C" },
		{ Z_KEY_LEFT,     "\x1b[D" },
		{ Z_KEY_HOME,     "\x1b[H" },
		{ Z_KEY_END,      "\x1b[F" },
		{ Z_KEY_PAGEUP,   "\x1b[5~" },
		{ Z_KEY_PAGEDOWN, "\x1b[6~" },
		{ Z_KEY_INSERT,   "\x1b[2~" },
		{ Z_KEY_DELETE,   "\x1b[3~" },
		{ Z_KEY_F1,       "\x1bOP" },
		{ Z_KEY_F2,       "\x1bOQ" },
		{ Z_KEY_F3,       "\x1bOR" },
		{ Z_KEY_F4,       "\x1bOS" },
		{ Z_KEY_F5,       "\x1b[15~" },
		{ Z_KEY_F6,       "\x1b[17~" },
		{ Z_KEY_F7,       "\x1b[18~" },
		{ Z_KEY_F8,       "\x1b[19~" },
		{ Z_KEY_F9,       "\x1b[20~" },
		{ Z_KEY_F10,      "\x1b[21~" },
		// F11 and F12 are term's own (the Open bar, disconnect) and
		// never reach here; they stay in the table so it remains a
		// complete record of the ordinary mapping.
		{ Z_KEY_F11,      "\x1b[23~" },
		{ Z_KEY_F12,      "\x1b[24~" },
	};

	for (uint32_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (table[i].keysym != keysym) continue;
		int len = (int)strlen(table[i].seq);
		if (len > buflen) return 0;
		memcpy(buf, table[i].seq, (size_t)len);
		return len;
	}

	if (keysym == Z_KEY_NONE || keysym >= 0x80) return 0;

	buf[0] = (char)keysym;
	return 1;

}

// Shift + a navigation key moves through scrollback. Returns true if
// consumed.
//
// Shifted, because the unshifted keys belong to the far end -- PgUp in
// `read`-style pagers, arrows in every line editor -- and term sent
// the same bytes for shifted and unshifted before, so nothing on the
// far end loses a key it could tell apart. A page is one line short of
// a screen so the line you were reading stays in view.
static bool scroll_key(uint32_t keysym, uint8_t mods) {

	if (!(mods & Z_KBD_MOD_SHIFT)) return false;

	switch (keysym) {
	case Z_KEY_PAGEUP:   view_scroll_by(VT_ROWS - 1);  return true;
	case Z_KEY_PAGEDOWN: view_scroll_by(-(VT_ROWS - 1)); return true;
	case Z_KEY_UP:       view_scroll_by(1);  return true;
	case Z_KEY_DOWN:     view_scroll_by(-1); return true;
	case Z_KEY_HOME:     view_set(vt_history_count(&vt)); return true;
	case Z_KEY_END:      view_live(); return true;
	default:             return false;
	}

}

static void handle_key_event(uint32_t packed) {

	uint32_t keysym = Z_WM_UNPACK_KEY_KEYSYM(packed);
	uint8_t mods = (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(packed);
	bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

	if (!pressed) return;

	// Mid-connect, the only key that means anything is Esc.
	if (connecting) {
		if (keysym == 0x1b) connect_cancel = true;
		return;
	}

	// Ctrl+SHIFT+C/V, not Ctrl+C/V: Ctrl+C is ^C to the far end, the
	// most-used key in a shell. z_kbd_usage_to_keysym() folds
	// Ctrl+letter to 0x01..0x1A regardless of Shift, so the shift bit
	// is what tells these apart. See docs/terminal.md.
	if ((mods & Z_KBD_MOD_CTRL) && (mods & Z_KBD_MOD_SHIFT)) {
		if (keysym == 0x03) { sel_copy(); return; }
		if (keysym == 0x16) { sel_paste(); return; }
	}

	// Before the bar and the selection: scrolling back to find
	// something to type, or to extend a selection with the keyboard
	// view, should disturb neither.
	if (scroll_key(keysym, mods)) return;

	if (keysym == Z_KEY_NONE) return;	// a bare modifier

	// Anything else drops the selection -- the far end is about to
	// echo something and it would stop describing what is on screen.
	if (sel_active) sel_clear();

	if (bar_key(keysym)) return;

	// F11 goes somewhere new; F12 leaves. Neither is a key anything on
	// the far end relies on, and both work whatever this window is
	// connected to -- which is the point: a remote with no quit
	// command of its own, or a child that has hung, still has a way
	// out. (Ctrl-] is the telnet-client convention, but zkbd only
	// folds Ctrl+letter into control codes; extending that to
	// punctuation would be a keyboard-layer change for every app.)
	if (keysym == Z_KEY_F11) { bar_open(); return; }
	if (keysym == Z_KEY_F12) {
		if (port.connected) term_disconnect("disconnected (F12)");
		else panel_show(NULL);
		return;
	}

	if (!port.connected) {
		panel_key(keysym, mods);
		return;
	}

	char buf[8];
	int len = key_to_bytes(keysym, buf, sizeof(buf));
	if (len <= 0) return;

	z_port_send(&port, buf, (uint32_t)len);

	// Typing at the far end means you want to see its answer.
	view_live();

}

// ---------------------------------------------------------------
// frame and main loop
// ---------------------------------------------------------------

static uint16_t sb_count = 0xFFFF;

// Brings everything on the glass up to date: the text, the two
// overlays on top of it, and the scrollbar. The one entry point for
// "draw", so the main loop and connect_port()'s wait cannot disagree
// about what drawing involves.
static void frame(void) {

	z_clip_t clip;

	// Before render(): a successful auto-connect hides the panel, and
	// the cells it covered must be repainted in THIS frame, not left
	// showing the panel until the next one.
	auto_poll();

	render();

	z_win_content_rect(&win, &clip);
	bar_draw(&clip);

	if (panel_visible) panel_probe(false);
	panel_draw();

	uint16_t count = vt_history_count(&vt);
	if (count != sb_count) {
		z_scrollbar_set_range(&sbar, (int32_t)count + VT_ROWS, VT_ROWS);
		sb_count = count;
	}
	z_scrollbar_set_value(&sbar, (int32_t)count - view_off);
	z_scrollbar_draw(&sbar, false);

}

static void term_setup(void) {

	cell_w = TERM_FONT.w;
	cell_h = TERM_FONT.h;
	text_w = VT_COLS * cell_w;
	text_h = VT_ROWS * cell_h;

	// +4 for the 2px left/right content inset, and Z_WM_TITLEBAR_H + 4
	// mirrors z_win_content_rect()'s own y formula (zwin.c) rather than
	// a number that goes stale if the titlebar changes. The scrollbar
	// strip is what makes it wider than 80 columns.
	term_win_w = text_w + Z_SB_THICK + 4;
	term_win_h = text_h + Z_WM_TITLEBAR_H + 4;

}

// Everything after the window exists. Separate from main() so the host
// render test (tests/render.c) runs exactly this.
static void term_start(void) {

	vt_init(&vt);
	vt_history_attach(&vt, hist_buf, TERM_HIST_LINES);

	shadow_invalidate();

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);
	z_scrollbar_set_geom(&sbar, text_w, 0, text_h);

	panel_layout();
	panel_show("choose a shell, or open a connection");

	auto_begin();

}

int main(void) {

	term_setup();

	// Registers "term0", "term1", ... so other processes can find THIS
	// window by name -- repl's `telnet` and posix's handoff reply to it.
	if (!z_pid_register("term", instance_name, sizeof(instance_name)))
		printf("term: pid registration failed, window title won't be unique\n");

	// term owns exactly one window for its lifetime, so the close icon
	// destroying it AND killing this process is exactly right.
	if (z_win_create_flags(&win, instance_name, term_win_w, term_win_h, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("term: failed to create window\n");
		return 1;
	}

	// No z_gfx_hw_font_load(): wm loads z_font_5x8 into glyph memory
	// once and is the only process that ever does -- see TERM_FONT_NAME.

	term_start();

	while (1) {

		z_msg_t msg;
		bool got_redraw = false;

		while (z_msg_read(&msg) == Z_OK) {

			if (msg.subject == Z_WM_REDRAW) {
				handle_redraw(msg.obj.val.uint32);
				got_redraw = true;
			} else if (msg.subject == Z_WM_SET_CLIP) {
				z_win_apply_clip(&win, &msg.obj);
			} else if (msg.subject == Z_WM_WINDOW_MOVED) {
				z_win_parse_rect(&win, &msg.obj);
			} else if (msg.subject == Z_WM_MOUSE) {
				if (msg.obj.type == Z_UINT32)
					handle_mouse_event(msg.obj.val.uint32);
			} else if (msg.subject == Z_WM_KEY) {
				handle_key_event(msg.obj.val.uint32);
			} else if (msg.subject == Z_PORT_DATA) {
				if (port.connected && msg.tag == port.conn_id &&
					msg.from == port.peer_pid) {
					uint32_t len = z_blob_len(&msg.obj);
					void *data = z_blob_data(&msg.obj);
					if (data && len) vt_feed(&vt, (const uint8_t *)data, len);
				}
				// Tells the sender it may free its z_obj_blob(). AFTER
				// vt_feed() has finished reading it, and sent even when
				// the guard did not match -- the sender's pending-send
				// slot needs the ack either way. See zport.h.
				z_port_send_ack(&msg);
			} else if (msg.subject == Z_PORT_DATA_ACK) {
				z_port_handle_ack(&port, &msg);
			} else if (msg.subject == Z_PORT_CLOSE) {
				// The SENDER is checked as well as the tag. Providers
				// number connections from their own tables (repl, posix
				// and portdemo all use slot+1), so a CLOSE from a
				// provider this window has just LEFT can carry a tag
				// that matches the new connection. That once dropped
				// `vi` the moment posix handed the terminal over.
				if (port.connected && msg.tag == port.conn_id &&
					msg.from == port.peer_pid) {
					port.connected = false;
					printf("term: port closed by peer\n");
					term_disconnect("connection closed by the other end");
				}
			} else if (msg.subject == Z_TERM_SET_PORT) {
				// Fire-and-forget (zterm.h). A bare Z_STR is a provider
				// name; a Z_MAP carries "name" and an "arg" for the
				// CONNECT -- repl's telnet/ssh and posix's handoff --
				// and gets the network timeout, since telnet is the
				// case that needs it.
				if (msg.obj.type == Z_STR && msg.obj.val.str) {
					connect_port(msg.obj.val.str, z_obj_none(),
						Z_CONN_TIMEOUT_LOCAL_TICKS, msg.obj.val.str);
				} else if (msg.obj.type == Z_MAP) {
					z_obj_t *name_obj = z_map_find(&msg.obj, "name");
					if (!name_obj || name_obj->type != Z_STR || !name_obj->val.str) {
						printf("term: SET_PORT map with no valid 'name', ignoring\n");
					} else {
						z_obj_t *arg_obj = z_map_find(&msg.obj, "arg");
						connect_port(name_obj->val.str,
							arg_obj ? *arg_obj : z_obj_none(),
							TERM_TELNET_CONNECT_TIMEOUT_TICKS,
							name_obj->val.str);
					}
				} else {
					printf("term: SET_PORT with no name, ignoring\n");
				}
			}

		}

		frame();

		if (got_redraw) z_win_redraw_done(&win);

		/* Block until something arrives, with a timeout.
		 *
		 * This loop used to spin at ~1435 iterations a second while
		 * idle, taking a full scheduler share from whatever was in the
		 * foreground (docs/app_runtime.md). The timeout rather than an
		 * indefinite wait is what drives the panel's twice-a-second
		 * check for repl0/posix0 appearing, and costs nothing
		 * measurable at 30 wakeups a second. */
		z_proc_wait(Z_TICK_HZ / 30);

	}

	return 0;

}
