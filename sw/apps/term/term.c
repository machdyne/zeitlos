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
 * at startup -- sw/apps/repl if it's running (started automatically
 * at boot, see sw/os/sh.c's init()), Zeitlos's command interpreter
 * (sw/apps/repl/repl.c). Typed keys go out through the port; whatever
 * comes back in (Z_PORT_DATA) is what actually reaches the VT100
 * parser now, not the keystroke directly -- this is the real
 * "keyboard -> wm -> term -> port -> repl -> term -> screen" path,
 * not a standalone loop anymore. `repl` does its own line editing,
 * echo, and backspace handling on the other end of the port (see
 * sw/common/zline.h's header comment for why that has to live on
 * repl's side, not here) -- term itself stays exactly as "dumb" a
 * VT100 renderer as before, just relaying bytes.
 *
 * Before repl existed, term connected to sw/apps/portdemo instead (a
 * raw echo/banner test harness, no command interpreter, no line
 * editing of its own -- see its own header comment) -- portdemo is
 * still there and still useful as a minimal test harness for the
 * port protocol itself, just no longer what term looks for by
 * default.
 *
 * If there's no port provider running (or it doesn't answer within
 * z_port_connect()'s timeout), term falls back to local echo -- typed
 * keys get fed straight back into the VT100 parser, same as phase 3 --
 * so it's still usable standalone without repl.
 *
 * Phase 5 (telnet, docs/networking.md/docs/ports.md): `repl`'s
 * `telnet <ip>` command redirects a term instance from repl to
 * sw/apps/net's telnet port provider instead (still via
 * Z_TERM_SET_PORT, sw/common/zterm.h -- see connect_port() below for
 * the mechanics). Once connected to a real remote that way, there's
 * no "quit"/"exit" command to hand control back with the way
 * repl/portdemo have -- so handle_key_event() below reserves F12 as a
 * fixed, always-available escape hotkey back to "repl0", intercepted
 * before it ever reaches the port. See that function's own comment
 * for why F12 specifically (not the classic telnet-client Ctrl-]).
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
#include "../../common/zterm.h"
#include "../../common/zrepl.h"	// for Z_PID_REPL, the fixed-pid
									// fallback below -- term itself
									// never sends/receives REPL_EVAL,
									// it only needs this one constant

// which font to render with -- override at build time with
// `make term FONT=z_font_6x12` (or any other font declared in
// zfont.h) to switch sizes without touching this file. Defaults to
// z_font_5x8 (sw/data/font/font5x8.mem) -- one row taller than the
// original z_font_5x7 (sw/data/font/font5x7.mem, a public-domain BDF
// conversion, see that file's own header), adopted after real-
// hardware testing showed z_font_5x7's bottom pixel row getting cut
// off on screen (see zfont.h's own z_font_5x8 comment) -- that's what
// was actually settled on after ALSO testing 6x12 (too much blit work
// per redraw, too little screen margin for wm to drag comfortably --
// see docs/user_input.md's "Debugging notes" for that history) --
// pass a different FONT to go back to 6x12 or try something else
// entirely.
//
// IMPORTANT as of the pid-registry/dock work: wm is now the only
// process that ever loads glyph data into hardware glyph memory (see
// its Makefile's own comment, and z_gfx_hw_font_load() in main()
// below -- there isn't one anymore), and it only ever loads
// z_font_5x8. Building term with a different FONT now means its
// hardware-blitted text (Z_GFX_HW_BLIT builds) will render using
// z_font_5x8's glyph *data* reinterpreted at this font's dimensions
// -- garbled, not just wrong-sized. Software-only builds (no
// Z_GFX_HW_BLIT) aren't affected, since those read glyph data
// straight from this process's own zfont_data.o, not shared hardware
// state. Don't override FONT for a Z_GFX_HW_BLIT build until wm loads
// more than one font.
#ifndef TERM_FONT_NAME
#define TERM_FONT_NAME z_font_5x8
#endif
#define TERM_FONT TERM_FONT_NAME

// how long connect_port() (below) waits for CONNECTED/REFUSED when
// connecting through the Z_TERM_SET_PORT Z_MAP form specifically --
// currently only ever `repl`'s `telnet <ip>` command. Longer than
// zport.h's own Z_PORT_CONNECT_TIMEOUT_TICKS default (~2s, right for
// a provider that's simply up-or-not) because net.c's telnet port
// provider doesn't reply CONNECTED/REFUSED until an actual TCP
// handshake to the remote server resolves one way or the other --
// that can legitimately take up to net's own tcp.c worst-case retry
// budget (TCP_RTO_TICKS_BASE/_MAX_SHIFT/_MAX_RETRIES there sum to
// ~31.5s before giving up). 45s -- ~13.5s of margin over that 31.5s
// worst case, room for scheduling/message-passing overhead on top of
// tcp.c's own numbers without needing to track them exactly. Found on
// real hardware: without this, a telnet connect to a genuinely
// unreachable/non-listening target always timed out on term's own
// side (this constant, at its old 2s) before net's TCP layer had
// even gotten partway through its own retries, let alone given up
// and sent an explicit REFUSED -- see zport.c's own
// Z_PORT_CONNECT_TIMEOUT_TICKS comment for the fuller story.
#define TERM_TELNET_CONNECT_TIMEOUT_TICKS (732 * 45)

// window size such that z_win_content_rect()'s inset (2px on every
// content-bearing edge -- see zwin.c) leaves EXACTLY VT_COLS*font.w x
// VT_ROWS*font.h of content area: content width = win.w-4, content
// height = win.h - (Z_WM_TITLEBAR_H+4) -- currently win.h-15, since
// Z_WM_TITLEBAR_H is 11 (zwm.h) -- see zwin.c's z_win_content_rect().
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

	// NOTE: this used to also call a resweep_right_of_cursor()
	// mitigation here (re-stamping a bounded run of columns after
	// every dirty-row redraw, gated behind a TERM_RESWEEP_MITIGATION
	// build flag) for a horizontal-garbage-near-typed-text artifact.
	// Removed now that the actual root cause has been found and fixed
	// at the source: rtl/gpu/gpu_blit.v's straddling-glyph state
	// machine could capture the WRONG framebuffer word's data into a
	// high-word read-modify-write, due to too narrow a bus-settle gap
	// between the low-word write and the high-word read (see that
	// file's own ST_GLYPH_HI_SETTLE1/2 states, and docs/gpu_blitter.md,
	// "Bugs found (and fixed)" #5, for the full writeup and how this
	// was actually confirmed via simulation this time, not just
	// theorized). If garbage reappears after this fix on real
	// hardware, that's strong evidence this specific fix isn't (the
	// whole of) the cause after all -- see git history for
	// resweep_right_of_cursor()'s implementation, which is safe to
	// reintroduce (it can only ever redraw correct content, never
	// destroy any) while investigating further.

}

static void feed_and_echo(const char *s) {
	vt_feed(&vt, (const uint8_t *)s, (uint32_t)strlen(s));
}

// closes the current port connection (if any -- harmless no-op via
// z_port_close()'s own `if (!port->connected) return;` if there isn't
// one) and attempts a new one to `name` (a pidreg name, e.g.
// "repl0"/"portdemo0"), falling back to `fallback_pid` ONLY if that
// lookup fails and `fallback_pid` is nonzero -- pass 0 for a
// caller-specified name (Z_TERM_SET_PORT below) where there's no
// sensible fixed-pid guess to fall back to, the way there is for the
// well-known startup default (see main()'s own call to this).
//
// blocks for up to z_port_connect()'s own timeout either way (same
// accepted "discards unrelated messages while waiting" limitation
// that already applied to the startup connection -- see
// z_port_connect()'s own comment, zport.c) -- called from the main
// message loop for Z_TERM_SET_PORT, not just at startup, so a
// SET_PORT-triggered reconnect can now genuinely stall this term's
// responsiveness to keystrokes/redraws for that same window, same as
// it already could during the one that happens before the main loop
// even starts.
// `arg` is forwarded as-is into z_port_connect_arg() -- Z_NONE for
// every existing caller (the startup connection, and repl's `port
// <name>` command), non-Z_NONE only for the Z_TERM_SET_PORT Z_MAP
// form (see zterm.h) -- e.g. repl's `telnet <ip>` command, which
// needs `net` to see the target IP as part of the CONNECT itself.
// `timeout_ticks`: how long to wait for CONNECTED/REFUSED --
// Z_PORT_CONNECT_TIMEOUT_TICKS (zport.h) for the common case (default
// arg, or a provider expected to answer almost immediately), longer
// for a provider known to do something slow before it can reply
// either way -- see the Z_MAP call site below (telnet) for the
// motivating case and the actual number used.
// forward declaration -- handle_key_event() itself is defined later
// in this file (it needs `vt`/state connect_port() doesn't otherwise
// depend on), but connect_port()'s own wait loop (below) needs to
// call it to service Z_WM_KEY while blocked on a slow connect, same
// as the main loop already does.
static void handle_key_event(uint32_t packed);

static bool connect_port(const char *name, uint32_t fallback_pid, z_obj_t arg,
	uint32_t timeout_ticks) {

	if (port.connected) z_port_close(&port);

	uint32_t target_pid;
	if (!z_pid_lookup(name, &target_pid)) {
		if (!fallback_pid) {
			printf("term: '%s' not found -- local echo only\n", name);
			return false;
		}
		target_pid = fallback_pid;
	}

	// inline reimplementation of z_port_connect_arg_timeout() (zport.c)
	// rather than a direct call to it -- that function's own wait loop
	// discards anything that isn't Z_PORT_CONNECTED/Z_PORT_REFUSED
	// ("not a reply to our CONNECT -- discard and keep waiting"), which
	// is the right call for its OTHER callers (a short, fixed ~2s
	// timeout, and no window/keyboard traffic to lose in the first
	// place for most of them) but wrong here: this function's own
	// header comment already flagged, before TERM_TELNET_CONNECT_
	// TIMEOUT_TICKS existed, that a SET_PORT-triggered reconnect could
	// "stall this term's responsiveness to keystrokes/redraws for that
	// same window" -- a real, accepted limitation at the original ~2s
	// window. Extending that window to 45s for telnet specifically
	// (TERM_TELNET_CONNECT_TIMEOUT_TICKS's own comment) turned a small,
	// easy-to-miss accepted gap into a ~22x larger one: real-hardware
	// symptom this was chasing was `wm`'s own "timed out waiting for
	// pid N to ack a redraw" firing during an active telnet connect
	// attempt (repair_region()/repair_drag(), wm.c) -- because THIS
	// wait loop was discarding the exact Z_WM_REDRAW message that
	// carries this window's own updated (x,y) after a move
	// (z_win_apply_redraw(), below) and that `wm` needs a
	// Z_WM_REDRAW_DONE reply to before it'll stop waiting. `win`'s own
	// cached position went stale for the rest of this term's lifetime
	// as a result -- silently wrong content-vs-chrome placement at
	// best, a real, still only partially understood contributor to
	// harder crashes reported on real hardware at worst.
	//
	// Services the same subset of messages the main loop below does
	// (Z_WM_REDRAW/Z_WM_WINDOW_MOVED/Z_WM_KEY), matching wm.c's own
	// wait_for_redraw_done() convention ("keeps servicing every other
	// message normally while waiting... rather than discarding them").
	// Z_PORT_DATA/Z_PORT_DATA_ACK/Z_PORT_CLOSE/Z_TERM_SET_PORT are
	// deliberately NOT serviced here: the port this call is in the
	// middle of (re)establishing isn't connected yet (or is the OLD
	// one, already closed above), so there's no sensible connection
	// for incoming data to belong to, and a nested SET_PORT arriving
	// mid-connect is an edge case rare enough not to be worth this
	// function calling itself recursively to handle.

	port.peer_pid = target_pid;
	port.conn_id = 0;
	port.connected = false;

	z_msg_new_send(target_pid, Z_PORT_CONNECT, 0, arg);

	uint32_t start = z_uptime_ticks();
	z_rv result = Z_FAIL;
	bool refused = false;

	while ((z_uptime_ticks() - start) < timeout_ticks) {

		z_msg_t msg;
		if (z_msg_read(&msg) != Z_OK) continue;

		if (msg.subject == Z_PORT_CONNECTED && msg.tag == 0) {
			port.conn_id = msg.obj.val.uint32;
			port.connected = true;
			result = Z_OK;
			break;
		}

		if (msg.subject == Z_PORT_REFUSED && msg.tag == 0) {
			if (msg.obj.type == Z_STR && msg.obj.val.str)
				printf("zport: connect to pid %ld refused: %s\n",
					(long)target_pid, msg.obj.val.str);
			else
				printf("zport: connect to pid %ld refused (no reason given)\n",
					(long)target_pid);
			refused = true;
			break;
		}

		if (msg.subject == Z_WM_REDRAW) {
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			vt_mark_all_dirty(&vt);
			draw_cursor_x = -1;
			draw_cursor_y = -1;
			render();
			z_win_redraw_done(&win);
		} else if (msg.subject == Z_WM_WINDOW_MOVED) {
			z_win_parse_rect(&win, &msg.obj);
		} else if (msg.subject == Z_WM_KEY) {
			handle_key_event(msg.obj.val.uint32);
		}
		// anything else: same "not relevant to this wait" discard the
		// generic zport.c version already documents.

	}

	if (result == Z_OK) {
		printf("term: connected to port at pid %ld (conn %ld)\n",
			(long)port.peer_pid, (long)port.conn_id);
		return true;
	}

	if (!refused) {
		printf("zport: connect to pid %ld timed out after %ld ticks -- "
			"provider never replied\n", (long)target_pid, (long)timeout_ticks);
	}

	printf("term: no port provider answered at pid %ld -- local echo only\n",
		(long)target_pid);
	return false;

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
		{ Z_KEY_F12,      "\x1b[24~" },	// unreachable in practice --
										// handle_key_event() intercepts
										// F12 itself before calling
										// this function at all (see
										// its own comment); kept here
										// so the table stays a
										// complete, honest record of
										// the "ordinary" VT100/xterm
										// mapping regardless of what
										// this app currently does
										// with that specific key.
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

	// F12: a fixed, term-local escape hotkey back to "repl0",
	// intercepted here BEFORE key_to_bytes()/the port -- regardless
	// of what term is currently connected to (or not connected to at
	// all). Exists because a real remote (e.g. a telnet server via
	// `net`, docs/networking.md) has no equivalent of portdemo's/
	// repl's own "quit"/"exit" commands to hand control back with --
	// once term is relaying raw bytes to some arbitrary remote, there
	// was otherwise no way out except the remote itself closing the
	// connection. The classic telnet-client convention is Ctrl-],
	// but Ctrl is only special-cased for letters in
	// sw/common/zkbd.c's usage-to-keysym translation (Ctrl+A..Z ->
	// 0x01..0x1A) -- extending that to punctuation would be a
	// keyboard-layer change every app inherits, for a feature only
	// this one app needs, so F12 (unused by anything term itself
	// sends -- its own key_to_bytes() table entry for F12 is simply
	// never reached now) stays entirely local to this file instead.
	// No-op (falls through to the normal path below, so an F12 with
	// no port connected still local-echoes nothing, same as any
	// other unmapped key) if already talking to "repl0" -- harmless
	// either way, connect_port() itself is safe to call redundantly.
	if (keysym == Z_KEY_F12) {
		connect_port("repl0", Z_PID_REPL, z_obj_none(), Z_PORT_CONNECT_TIMEOUT_TICKS);
		return;
	}

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
	// +4 for the same left/right 2px content inset; +Z_WM_TITLEBAR_H+4
	// mirrors z_win_content_rect()'s own y0 formula (zwin.c) exactly,
	// rather than a hardcoded number that would silently go stale
	// again if Z_WM_TITLEBAR_H (zwm.h) ever changes -- see this file's
	// own comment on term_win_w/term_win_h above for why this has to
	// land on an exact multiple of the font's cell size.
	term_win_h = VT_ROWS * TERM_FONT.h + Z_WM_TITLEBAR_H + 4;

	// register this instance under a kernel-numbered name ("term0",
	// "term1", ...) -- see sw/os/pidreg.h -- so other processes can
	// find THIS particular term by name instead of relying on a fixed
	// pid (which only ever worked for wm/net, started once each in a
	// known boot order; doesn't work for something the user can start
	// any number of times, like term). Falls back to the literal
	// "term" as the window title if registration ever fails (e.g. the
	// registry is full) -- not fatal, just loses the disambiguation.
	char instance_name[24] = "term";
	if (!z_pid_register("term", instance_name, sizeof(instance_name)))
		printf("term: pid registration failed, window title won't be unique\n");

	// Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER (zwm.h):
	// term owns exactly one window for its entire lifetime, so
	// clicking its titlebar close icon destroying that window AND
	// killing this process outright is exactly right -- see
	// Z_WIN_FLAG_CLOSE_KILLS_OWNER's own comment for why that's NOT
	// the default (an app that can own several windows off one pid,
	// e.g. repl's Scheme `win-create`, needs the other behavior
	// instead).
	if (z_win_create_flags(&win, instance_name, term_win_w, term_win_h, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("term: failed to create window\n");
		return 1;
	}

	// no z_gfx_hw_font_load() call here anymore -- wm now loads
	// z_font_5x8 into hardware glyph memory exactly once, at its own
	// startup, and is the only process that ever does (see
	// TERM_FONT_NAME's own comment above, and wm's Makefile). As long
	// as this stays built against the default TERM_FONT_NAME
	// (z_font_5x8), the glyph data wm already loaded is exactly what
	// this needs -- nothing to push here.
	vt_init(&vt);   // already marks every row dirty, so the first
	                // render() below draws the full (blank) screen

	// connect to a port provider if one's running (see this file's
	// own header comment) -- must happen here, before the main
	// message loop below, since z_port_connect() discards any
	// unrelated message that arrives while it's waiting (same
	// accepted limitation as z_win_create()/z_msg_wait()).
	//
	// looks up "repl0" (see sw/os/pidreg.h) instead of assuming the
	// fixed Z_PID_REPL constant -- falls back to it if lookup fails
	// (repl isn't running, hasn't registered yet, or is an old build
	// that predates the registry). This is the only place a fixed-pid
	// fallback makes sense -- "repl0" is the well-known default
	// provider, same reasoning Z_PID_PORTDEMO existed for before it.
	// A runtime switch to some OTHER, caller-specified provider
	// (Z_TERM_SET_PORT, connect_port() above, this file's own header
	// comment) has no such well-known fallback to guess -- see there.
	connect_port("repl0", Z_PID_REPL, z_obj_none(), Z_PORT_CONNECT_TIMEOUT_TICKS);

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
				if (port.connected && msg.tag == port.conn_id) {
					uint32_t len = z_blob_len(&msg.obj);
					void *data = z_blob_data(&msg.obj);
					if (data && len) vt_feed(&vt, (const uint8_t *)data, len);
				}
				// tells whoever sent this it's now safe to free its own
				// z_obj_blob() allocation -- see z_port_send_ack()'s own
				// comment (zport.h) for why this has to come AFTER
				// vt_feed() actually finishes reading `data`, not right
				// after z_msg_read() produced `msg`. Sent unconditionally,
				// even when the guard above didn't match -- that's still
				// a message this process will never look at again, and
				// the sender's own pending-sends slot for it
				// (Z_PORT_MAX_PENDING_SENDS, zport.h) needs an ack to
				// ever be freed regardless.
				z_port_send_ack(&msg);
			} else if (msg.subject == Z_PORT_DATA_ACK) {
				z_port_handle_ack(&port, &msg);
			} else if (msg.subject == Z_PORT_CLOSE) {
				if (port.connected && msg.tag == port.conn_id) {
					port.connected = false;
					printf("term: port closed by peer -- local echo only from here on\n");
				}
			} else if (msg.subject == Z_TERM_SET_PORT) {
				// see zterm.h -- fire-and-forget, no reply sent either
				// way; the result shows up in this printf() log (via
				// connect_port()) and, if it worked, in what actually
				// starts arriving over the new connection.
				//
				// two payload shapes (zterm.h): a bare Z_STR is just
				// the provider name (original form); a Z_MAP carries
				// an additional "arg", forwarded into
				// connect_port()'s own z_port_connect_arg() call --
				// e.g. repl's `telnet <ip>` command, which needs
				// `net` to see the target IP as part of the CONNECT
				// itself, not a separate message.
				if (msg.obj.type == Z_STR && msg.obj.val.str) {
					connect_port(msg.obj.val.str, 0, z_obj_none(), Z_PORT_CONNECT_TIMEOUT_TICKS);
				} else if (msg.obj.type == Z_MAP) {
					z_obj_t *name_obj = z_map_find(&msg.obj, "name");
					if (!name_obj || name_obj->type != Z_STR || !name_obj->val.str) {
						printf("term: SET_PORT map with no valid 'name', ignoring\n");
					} else {
						z_obj_t *arg_obj = z_map_find(&msg.obj, "arg");
						// the Z_MAP form is currently telnet-only (see
						// this block's own header comment) -- longer
						// timeout, see TERM_TELNET_CONNECT_TIMEOUT_TICKS's
						// own comment for why.
						connect_port(name_obj->val.str, 0,
							arg_obj ? *arg_obj : z_obj_none(),
							TERM_TELNET_CONNECT_TIMEOUT_TICKS);
					}
				} else {
					printf("term: SET_PORT with no name, ignoring\n");
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
