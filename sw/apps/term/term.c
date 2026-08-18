/*
 * term -- VT100 terminal emulator
 *
 * Phase 3: a real 80x25-character window (sw/common/zwin.h) driven by
 * live keyboard input (Z_WM_KEY, see docs/user_input.md) through the
 * VT100 core (sw/common/zvt100.h/.c, whose own correctness is
 * verified independently -- see sw/test/test_zvt100.c, run with
 * `make test-zvt100` from sw/test/, entirely on the host).
 *
 * Phase 4: connects to a port (sw/common/zport.h/.c, docs/ports.md)
 * at startup -- sw/apps/portdemo if it's running (started
 * automatically at boot, see sw/os/sh.c's init()), which just sends a
 * banner and echoes back whatever it receives. Typed keys go out
 * through the port; whatever comes back in (Z_PORT_DATA) is what
 * actually reaches the VT100 parser now, not the keystroke directly --
 * this is the real "keyboard -> wm -> term -> port -> (something) ->
 * term -> screen" path, not a standalone loop anymore.
 *
 * If there's no port provider running (or it doesn't answer within
 * z_port_connect()'s timeout), term falls back to local echo -- typed
 * keys get fed straight back into the VT100 parser, same as phase 3 --
 * so it's still usable standalone without portdemo.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"
#include "../../common/zvt100.h"
#include "../../common/zport.h"

// which font to render with -- override at build time with
// `make term FONT=z_font_6x12` (or any other font declared in
// zfont.h) to switch sizes without touching this file. Defaults to
// the smallest currently available (z_font_5x7, sw/data/font/font5x7.mem
// -- a public-domain BDF conversion, see that file's own header) since
// that's what's actually been settled on after testing 6x12 (too much
// blit work per redraw, too little screen margin for wm to drag
// comfortably -- see docs/user_input.md's "Debugging notes" for the
// history) -- pass a different FONT to go back to 6x12 or try
// something else entirely.
#ifndef TERM_FONT_NAME
#define TERM_FONT_NAME z_font_5x7
#endif
#define TERM_FONT TERM_FONT_NAME

// window size such that z_win_content_rect()'s inset (2px left/right,
// 1px below the titlebar, 3px bottom -- see zwin.c) leaves EXACTLY
// VT_COLS*font.w x VT_ROWS*font.h of content area: content width =
// win.w-4, content height = win.h-15 (see zwin.c's z_win_content_rect()).
// This is why there's no separate z_win_clear() call anywhere below:
// the 80x25 grid tiles the content area exactly, no leftover padding
// pixels to clear separately, so a full dirty-cell redraw already
// covers every pixel. Computed at runtime (not a macro) now that the
// font itself is swappable -- TERM_FONT.w/.h aren't preprocessor
// constants.
static int term_win_w, term_win_h;

static vt_screen_t vt;
static z_win_t win;
static z_port_t port;

// last position the cursor overlay (see render() below) was actually
// drawn at -- -1 means "not drawn yet, or just invalidated (a wm
// redraw happened)". kept separate from vt.cursor_x/y because the
// overlay needs to know where to ERASE from, not just where to draw.
static int draw_cursor_x = -1;
static int draw_cursor_y = -1;

// draws one character cell at content-relative (col,row), honoring
// `reverse` by swapping which color is foreground vs. background.
// Goes through z_fb_draw_char2() (zgfx.h) -- hardware-accelerated via
// the GPU glyph blitter (rtl/gpu/gpu_blit.v) when built with
// Z_GFX_HW_BLIT, which this app's Makefile does. An earlier version
// of this function rendered every pixel itself via z_fb_set_pixel(),
// because z_fb_draw_char()'s hardware path hardcodes its background
// to 0 -- but the blitter's fg_color_reg/bg_color_reg are genuinely
// independent registers in hardware (see z_fb_draw_char2()'s own
// comment in zgfx.c), z_fb_draw_char() just never exposed the second
// one. That per-pixel software path was correct but visibly slow
// redrawing a full 80x25 grid; z_fb_draw_char2() does the same thing
// in hardware.
static void draw_cell(int col, int row, char ch, bool reverse) {

	z_clip_t clip;
	z_win_content_rect(&win, &clip);

	int x = clip.x0 + col * TERM_FONT.w;
	int y = clip.y0 + row * TERM_FONT.h;

	int fg = reverse ? 0 : 1;
	int bg = reverse ? 1 : 0;

	z_fb_draw_char2(x, y, ch, fg, bg, &TERM_FONT, &clip);

}

// redraws whatever actually changed: dirty cells (from vt_feed()
// since the last call) plus the cursor overlay, which needs its own
// tracking since moving the cursor (e.g. an arrow key) doesn't dirty
// any cell at all.
static void render(void) {

	bool was_dirty[VT_ROWS];
	bool any_dirty = false;
	for (int row = 0; row < VT_ROWS; row++) {
		was_dirty[row] = vt_row_dirty(&vt, row);
		if (was_dirty[row]) any_dirty = true;
	}

	for (int row = 0; row < VT_ROWS; row++) {
		if (!was_dirty[row]) continue;
		for (int col = 0; col < VT_COLS; col++) {
			vt_cell_t *cell = &vt.cells[row][col];
			draw_cell(col, row, cell->ch, cell->reverse);
		}
	}
	vt_clear_dirty(&vt);

	// clamp for the deferred-wrap pending state (cursor_x can
	// transiently equal VT_COLS right after the last column is
	// written -- see zvt100.h) -- the visual cursor has nowhere
	// sensible to sit past the last real column.
	int cur_x = (vt.cursor_x >= VT_COLS) ? VT_COLS - 1 : vt.cursor_x;
	int cur_y = vt.cursor_y;

	bool cursor_moved = (cur_x != draw_cursor_x || cur_y != draw_cursor_y);
	if (!any_dirty && !cursor_moved) return;

	// erase the old cursor cell back to its real (non-inverted)
	// appearance -- unless that row was already covered by the
	// dirty-cell redraw above, which already drew it correctly
	if (draw_cursor_y >= 0 && draw_cursor_y < VT_ROWS && !was_dirty[draw_cursor_y]) {
		vt_cell_t *old_cell = &vt.cells[draw_cursor_y][draw_cursor_x];
		draw_cell(draw_cursor_x, draw_cursor_y, old_cell->ch, old_cell->reverse);
	}

	// draw the new cursor cell inverted
	vt_cell_t *cur_cell = &vt.cells[cur_y][cur_x];
	draw_cell(cur_x, cur_y, cur_cell->ch, !cur_cell->reverse);

	draw_cursor_x = cur_x;
	draw_cursor_y = cur_y;

}

static void feed_and_echo(const char *s) {
	vt_feed(&vt, (const uint8_t *)s, (uint32_t)strlen(s));
}

// translates one keysym into 0+ raw bytes to send onward -- to the
// port if connected, or fed straight back as local echo if not (see
// this file's own header comment). the arrow/nav/F-key sequences are
// the common xterm-ish convention (ESC[A, ESC[1~, ESC[15~, etc), not
// strict original VT100 -- real VT100 only had PF1-PF4 (no F5-F12, no
// nav cluster at all, those came later). this matches what most
// things people actually connect to expect, which matters more here
// than strict fidelity to 1978 hardware.
//
// backspace maps to a single raw DEL (0x7f) -- the byte a real
// terminal actually sends. the nicer "move left, blank, move left"
// local-VISUAL erase trick some terminals do is handled separately in
// handle_key_event(), only in the no-port fallback path, since it's a
// rendering convenience for THIS display, not something that belongs
// in the byte stream itself -- a real remote decides what backspace
// should look like from its own echo, same as any other byte.
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

// handles one Z_WM_KEY event: translates it to bytes (key_to_bytes()
// above) and either sends them out through the port, or -- no port
// connected -- feeds them straight back into our own vt_screen_t as
// local echo. only acts on key-down events; releases and auto-repeat
// aren't handled.
static void handle_key_event(uint32_t packed) {

	uint32_t keysym = Z_WM_UNPACK_KEY_KEYSYM(packed);
	bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

	if (!pressed) return;

	char buf[8];
	int len = key_to_bytes(keysym, buf, sizeof(buf));
	if (len <= 0) return;

	if (port.connected) {
		z_port_send(&port, buf, (uint32_t)len);
		return;
	}

	// no port -- local echo fallback
	if (len == 1 && buf[0] == 0x7f) {
		// backspace, standalone mode only -- see key_to_bytes()'s
		// comment on why this doesn't apply when actually connected.
		// matches zeitlos.c's own readline() convention for the same
		// "visually erase" trick (docs/app_runtime.md).
		feed_and_echo("\x08 \x08");
		return;
	}

	vt_feed(&vt, (const uint8_t *)buf, (uint32_t)len);

	if (len == 1 && buf[0] == '\r') {
		// local-echo-only convenience: a real port/remote would decide
		// its own CR/LF handling (docs/ports.md) -- with nothing on
		// the other end, also feed '\n' so Enter visibly moves to a
		// new line while testing standalone.
		vt_feed_byte(&vt, '\n');
	}

}

int main(void) {

	term_win_w = VT_COLS * TERM_FONT.w + 4;
	term_win_h = VT_ROWS * TERM_FONT.h + 15;

	if (z_win_create(&win, "term", term_win_w, term_win_h) != Z_OK) {
		printf("term: failed to create window\n");
		return 1;
	}

	// required for Z_GFX_HW_BLIT builds (see draw_cell()'s comment) --
	// pushes TERM_FONT's glyph data into hardware glyph memory so the
	// blitter has something to read; a documented no-op in
	// software-only builds. must happen before the first render()
	// call below. same pattern as hello_win.c's own startup.
	z_gfx_hw_font_load(&TERM_FONT);

	vt_init(&vt);   // already marks every row dirty, so the first
	                // render() below draws the full (blank) screen

	// connect to a port provider if one's running (see this file's
	// own header comment) -- must happen here, before the main
	// message loop below, since z_port_connect() discards any
	// unrelated message that arrives while it's waiting (same
	// accepted limitation as z_win_create()/z_msg_wait()).
	if (z_port_connect(&port, Z_PID_PORTDEMO) == Z_OK) {
		printf("term: connected to port at pid %ld (conn %ld)\n",
			(long)port.peer_pid, (long)port.conn_id);
	} else {
		printf("term: no port provider answered -- local echo only\n");
	}

	while (1) {

		z_msg_t msg;
		bool got_redraw = false;

		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_WM_REDRAW) {
				z_win_apply_redraw(&win, msg.obj.val.uint32);
				got_redraw = true;
			} else if (msg.subject == Z_WM_WINDOW_MOVED) {
				z_win_parse_rect(&win, &msg.obj);
			} else if (msg.subject == Z_WM_KEY) {
				handle_key_event(msg.obj.val.uint32);
			} else if (msg.subject == Z_PORT_DATA) {
				if (!port.connected || msg.tag != port.conn_id) continue;
				uint32_t len = z_blob_len(&msg.obj);
				void *data = z_blob_data(&msg.obj);
				if (data && len) vt_feed(&vt, (const uint8_t *)data, len);
			} else if (msg.subject == Z_PORT_CLOSE) {
				if (port.connected && msg.tag == port.conn_id) {
					port.connected = false;
					printf("term: port closed by peer -- local echo only from here on\n");
				}
			}
		}

		if (got_redraw) {
			// the framebuffer under/around this window may have
			// changed entirely (a move, or another window that used
			// to overlap us) -- simplest correct thing is a full
			// repaint, same convention hello_win.c's draw_static()
			// uses on its own Z_WM_REDRAW handling.
			vt_mark_all_dirty(&vt);
			draw_cursor_x = -1;   // force the cursor overlay to redraw too
			draw_cursor_y = -1;
		}

		render();

		if (got_redraw) z_win_redraw_done(&win);

	}

	return 0;

}
