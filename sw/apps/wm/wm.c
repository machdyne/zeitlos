/*
 * wm -- Zeitlos window manager
 *
 * Owns the screen: draws windows as a 5-line box (4-line border +
 * 1-line titlebar separator), lets the user drag them by the
 * titlebar, and tracks focus/z-order. Apps don't draw their own
 * content yet -- see docs/window_manager.md for the phased plan --
 * so this phase is entirely about window chrome and the
 * create/destroy/moved protocol in zwm.h.
 *
 * Expected to be started before any client apps that want a window
 * (they'll fail to connect otherwise) -- typically right after boot:
 *
 *   > run wm
 *
 * Registers itself as "wm0" (see sw/os/pidreg.h) so clients can find
 * it by name (zwin.c's z_win_create() looks this up, falling back to
 * the fixed Z_PID_WM constant in zwm.h if lookup fails -- e.g. an old
 * build of wm that predates registration). wm no longer assumes it's
 * literally pid 1 internally either -- see my_pid below, queried via
 * z_getpid() at startup and used for wm's own "is this window mine?"
 * checks instead of comparing against Z_PID_WM directly.
 *
 * Until a real client app exists, wm creates a couple of windows for
 * itself on startup so there's something to look at and drag.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"
#include "dock_icons.h"

#define WM_MAX_WINDOWS    16
#define WM_SCREEN_W       640
#define WM_SCREEN_H       480
#define WM_TITLE_MAX      24

typedef struct {
	bool		used;
	uint32_t	owner_pid;
	uint32_t	x, y, w, h;
	char		title[WM_TITLE_MAX];
	// true for windows drawn without the titlebar/separator line and
	// exempt from titlebar-drag hit testing -- see hit_titlebar() and
	// draw_window_box() below. only the dock uses this right now (see
	// "-- dock --" below), but this is a general window property, not
	// dock-specific, in case something else wants a chromeless window
	// later.
	bool		no_titlebar;
} wm_window_t;

// -- dock --
//
// a small always-on-top, non-draggable, non-focusable window (owned
// -- dock --
//
// a small always-on-top, non-draggable, non-focusable window (owned
// by wm itself, like the demo windows below) anchored to the bottom
// left of the screen, holding one 32x32 icon per app. Clicking an
// icon launches that app via z_proc_run() (see zeitlos.h) -- the
// syscall added alongside this feature specifically so a running
// process (wm) can start another one; previously only the kernel
// shell could do that. Hardcoded for now, per the file's own
// "phased plan" framing -- see the header comment above and
// docs/window_manager.md. Adding an app is one entry in dock_apps[]
// below, PLUS a 32x32 icon -- see dock_icons.h's own header comment
// (and sw/data/icons/gen_dock_icon_data.py, its generator) for the
// full steps.
//
// `name` is the bare filename on the FAT filesystem (same name
// sh.c's `run <file>` and z_proc_run() expect -- no path, no
// extension). `bitmap` is the icon's 32x32 1bpp pixel data (see
// dock_icons.h) -- generate it from a source PNG with
// sw/data/icons/gen_dock_icon_data.py rather than writing it by
// hand.
typedef struct {
	const char	*name;
	const uint8_t	*bitmap;	// 32x32 1bpp, MSB-first -- see dock_icons.h
} dock_app_t;

static const dock_app_t dock_apps[] = {
	{ "term",	z_icon_term_data  },
	{ "gpu3d",	z_icon_gpu3d_data },
};
#define DOCK_APP_COUNT   (int)(sizeof(dock_apps) / sizeof(dock_apps[0]))

#define DOCK_ICON_SIZE   32	// fixed icon size, see file header comment
#define DOCK_ICON_GAP     4	// space between adjacent icons
#define DOCK_PADDING      4	// space between icons and the dock's own border
#define DOCK_MARGIN       8	// space between the dock and the screen edges

// index into windows[]/zorder of the dock, or -1 before it's created
// -- set once in main() and never destroyed, so unlike other window
// indices this one doesn't need the usual "does this id still exist"
// checking.
static int dock_idx = -1;

static wm_window_t windows[WM_MAX_WINDOWS];

// this process's own actual pid -- queried via z_getpid() at startup
// (see main()), NOT the Z_PID_WM constant (zwm.h). Z_PID_WM is still
// what OTHER processes use to reach wm (zwin.c looks it up by name
// now, falling back to Z_PID_WM -- see docs/window_manager.md), but
// wm's own "is this window mine?" checks below need wm's REAL pid,
// not an assumption that it's always started first. Previously these
// compared directly against Z_PID_WM, which only ever worked because
// that assumption happened to hold in practice.
static uint32_t my_pid;

static uint8_t zorder[WM_MAX_WINDOWS];	// back-to-front; zorder[count-1] is frontmost
static uint8_t zorder_count = 0;
static int focused = -1;		// index into windows[], or -1
static int dragging = -1;		// index into windows[], or -1
static int drag_off_x = 0, drag_off_y = 0;
// bounding box swept by the dragged window since the drag started --
// see the drag-update block below for why this is tracked instead of
// just the start/end rects.
static int drag_min_x, drag_min_y, drag_max_x, drag_max_y;

static void send_win_rect(uint32_t to, uint32_t subject, uint32_t tag, int idx);
static void handle_message(z_msg_t *msg);

// lightweight redraw notification -- no heap allocation (see
// Z_WM_REDRAW in zwm.h for why this matters). safe to call as often
// as repair_region() below does.
static void send_redraw(uint32_t to, int idx) {
	uint32_t packed = Z_WM_PACK_XY(idx, windows[idx].x, windows[idx].y);
	z_msg_new_send(to, Z_WM_REDRAW, 0, z_obj_uint32(packed));
}

// -- gpu / screen --
//
// chrome (border/titlebar) drawing goes through zgfx.h's
// z_fb_hw_line()/z_fb_hw_box() now, not a local copy of this logic --
// see zgfx.h's file header comment for why that needed IRQ masking to
// be safe for concurrent access from multiple processes (gpu3d/
// gpudemo also draw through the same hardware rasterizer while they
// run). clip=NULL below: wm always draws chrome unclipped, at
// coordinates it already knows are valid.

static void clear_screen(void) {
	z_fb_hw_fill_rect(0, 0, WM_SCREEN_W, WM_SCREEN_H, 0);
}

static void draw_window_box(wm_window_t *w, bool is_focused, int color) {

	int x0 = w->x, y0 = w->y;
	int x1 = w->x + w->w - 1, y1 = w->y + w->h - 1;

	z_fb_hw_box(x0, y0, x1, y1, color, NULL);

	if (!w->no_titlebar) {
		int ty = w->y + Z_WM_TITLEBAR_H;
		z_fb_hw_line(x0, ty, x1, ty, color, NULL);	// titlebar separator
	}

	if (is_focused) {
		// bolder border for the focused window -- a 1px inset outline.
		// text rendering isn't wired up yet (see docs/window_manager.md),
		// so this is the only visual focus indicator for now.
		z_fb_hw_box(x0 + 1, y0 + 1, x1 - 1, y1 - 1, color, NULL);
	}

}

// draws the dock's content (one 32x32 icon slot per dock_apps[]
// entry) -- called from repair_region() below, same place/timing as
// an app's own Z_WM_REDRAW-triggered redraw, except synchronous and
// in-process (the dock is owned by wm itself, so there's no
// message round trip -- see the owner_pid == my_pid check
// elsewhere in this file). Always redraws every icon slot; there's
// no partial-content tracking, same as every other window's redraw
// in this codebase so far.
//
// unlike an earlier version of this function, this does NOT call
// z_gfx_hw_font_load() itself -- wm loads z_font_5x8 exactly once, in
// main(), and that's now the only font any process on the board loads
// into hardware glyph memory at all (see main()'s comment, and
// "Hardware glyph blitting" in docs/window_manager.md). draw_dock()
// just draws, trusting that font data is already there and stays
// there.
// blits a 32x32 1bpp bitmap (see dock_icons.h for the exact format)
// at (x0,y0), one z_fb_set_pixel() call per bit. Only ever called for
// the dock's icon slots -- a handful of calls per repair_region()
// overlap, not a hot path (contrast sw/apps/term, which moved off
// z_fb_set_pixel()-per-pixel for its OWN per-character redraws
// specifically because that WAS hot enough to be visibly laggy while
// typing -- see z_fb_draw_char2()'s comment in zgfx.h). If dock icons
// ever need to redraw far more often than "something overlapped the
// dock", worth revisiting the same way.
static void draw_icon_bitmap(int x0, int y0, const uint8_t *bitmap) {

	for (int row = 0; row < DOCK_ICON_SIZE; row++) {
		for (int col = 0; col < DOCK_ICON_SIZE; col++) {
			uint8_t byte = bitmap[row * (DOCK_ICON_SIZE / 8) + col / 8];
			int bit = (byte >> (7 - (col % 8))) & 1;
			if (bit) z_fb_set_pixel(x0 + col, y0 + row, 1, NULL);
		}
	}

}

static void draw_dock(void) {

	if (dock_idx < 0) return;

	wm_window_t *w = &windows[dock_idx];
	int x0 = (int)w->x, y0 = (int)w->y;

	for (int i = 0; i < DOCK_APP_COUNT; i++) {

		int ix = x0 + DOCK_PADDING + i * (DOCK_ICON_SIZE + DOCK_ICON_GAP);
		int iy = y0 + DOCK_PADDING;

		z_fb_hw_box(ix, iy, ix + DOCK_ICON_SIZE - 1, iy + DOCK_ICON_SIZE - 1, 1, NULL);

		draw_icon_bitmap(ix, iy, dock_apps[i].bitmap);

	}

}

// returns true if rectangles a and b (given as x,y,w,h) overlap at all
static bool rects_overlap(int ax, int ay, int aw, int ah,
	int bx, int by, int bw, int bh) {
	return !(ax + aw <= bx || bx + bw <= ax || ay + ah <= by || by + bh <= ay);
}

static void fill_rect(int x, int y, int w, int h, int color) {
	// hardware blitter fill (zgfx.h) instead of a software VRAM loop
	// now -- see docs/gpu_blitter.md and docs/app_runtime.md, "The
	// GPU line rasterizer" (the fill-mode writeup applies the same
	// reasoning) for why this matters beyond raw speed: this used to
	// be a tight, uninterrupted software loop spanning many timer
	// ticks for anything but a small rect, which turned out to be
	// implicated in an intermittent crash during exactly this kind of
	// sustained, CPU-bound looping (see boot_picorv32.S's irq_stack
	// comment). z_fb_hw_fill_rect() finishes the same fill in a
	// small, roughly constant number of hardware operations instead.
	z_fb_hw_fill_rect(x, y, w, h, color);
}

// bound on how long repair_region() will block waiting for one app to
// ack a redraw (see wait_for_redraw_done() below) before giving up
// and moving on. not a precise time unit -- see docs/window_manager.md.
#define REDRAW_ACK_TIMEOUT   500

// blocks until `pid` sends Z_WM_REDRAW_DONE, or the timeout above is
// hit. keeps servicing every other message normally while waiting
// (via handle_message()) rather than discarding them -- unlike
// z_msg_wait(), which would drop any other app's requests that
// arrived during the wait.
static void wait_for_redraw_done(uint32_t pid) {

	for (int waited = 0; waited < REDRAW_ACK_TIMEOUT; waited++) {

		z_msg_t msg;
		bool got_ack = false;

		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_WM_REDRAW_DONE && msg.from == pid)
				got_ack = true;
			else
				handle_message(&msg);
		}

		if (got_ack) return;

		for (volatile int i = 0; i < 2000; i++);

	}

	printf("wm: timed out waiting for pid %ld to ack a redraw\n", (long)pid);

}

// clears just the given screen region and redraws chrome + waits for
// a content redraw from every window whose rect overlaps it, strictly
// back-to-front -- nothing outside this region is touched, and
// nothing jumps the queue. this replaced an earlier full-screen clear
// + redraw-everyone approach, which meant any window changing (even a
// click that only changed focus) made every other window on screen
// flash, whether or not it was anywhere near the change. the
// back-to-front ordering (via wait_for_redraw_done()) is what
// actually keeps a window's content from momentarily showing through
// a window that's supposed to be in front of it -- without it, each
// app redraws whenever its process happens to get scheduled, with no
// guarantee that's in z-order. see docs/window_manager.md, "targeted
// redraw" and "content z-order".
//
// exclude_idx: skip the redraw-notify+wait step for this one window
// index (still draws its chrome), or -1 to not exclude anything. only
// needed right after create_window() -- see its caller below: that
// window's owner is still blocked waiting for Z_WM_WINDOW_CREATED at
// this point, so it isn't listening for Z_WM_REDRAW yet and couldn't
// possibly reply, which would otherwise stall every window creation
// for the full wait_for_redraw_done() timeout.
static void repair_region(int rx, int ry, int rw, int rh, int exclude_idx) {

	fill_rect(rx, ry, rw, rh, 0);

	// temporary diagnostic -- isolating whether a hang is inside
	// fill_rect() itself or the loop below. remove once resolved.
	printf("wm: repair_region: fill_rect(%d,%d,%d,%d) done\n", rx, ry, rw, rh);

	for (int i = 0; i < zorder_count; i++) {
		int idx = zorder[i];
		wm_window_t *w = &windows[idx];
		if (!rects_overlap(rx, ry, rw, rh,
			(int)w->x, (int)w->y, (int)w->w, (int)w->h))
			continue;
		// temporary diagnostic instrumentation -- see repair_drag()'s
		// own comment. remove once resolved.
		printf("wm: repair_region: draw_window_box idx=%d (%ld,%ld,%ld,%ld) focused=%d\n",
			idx, (long)w->x, (long)w->y, (long)w->w, (long)w->h, idx == focused);
		draw_window_box(w, idx == focused, 1);
		if (idx == dock_idx) draw_dock();
		if (w->owner_pid == my_pid) continue;
		if (idx == exclude_idx) continue;
		send_redraw(w->owner_pid, idx);
		wait_for_redraw_done(w->owner_pid);
	}

}

// -- mouse --
//
// reg_usbN_cursor's x/y fields come from a hardware cursor tracker
// (rtl/usb_hid.v) clamped to [0,639]/[0,479] -- exactly the native
// 640x480 framebuffer resolution, so no scaling is needed at all
// (this used to be a /2, back when curs_x/curs_y's native range was
// [0,1023]/[0,767], double the then-512x384 framebuffer -- see
// rtl/usb_hid.v's own comment on why that clamp changed).
//
// there are two independent USB HID ports now (see zeitlos.h), and
// no fixed port-to-device mapping -- either port might be the mouse.
// mouse_port() picks whichever port currently reports itself as a
// mouse (typ==2); if neither does (nothing plugged in yet) or,
// unusually, both do, it prefers port 0 -- matching the same
// tie-breaking rule rtl/sysctl.v's hardware cursor-sprite mux uses,
// so the software click math and the on-screen pointer never disagree
// about which port is "the" mouse.
//
// typ lives at bits[25:24] of the info register (rtl/usb_hid.v:
// { report[31], 5'b0[30:26], typ[25:24], 16'b0[23:8], modifiers[7:0] }),
// NOT bits[23:22] -- that range falls entirely inside the constant
// 16'b0 padding and always reads 0 regardless of what's plugged in.
static inline int mouse_port(void) {
	uint8_t typ0 = (reg_usb0_info >> 24) & 0x3;
	uint8_t typ1 = (reg_usb1_info >> 24) & 0x3;
	if (typ0 == 2) return 0;
	if (typ1 == 2) return 1;
	return 0;
}

static inline int get_cursor_x(void) {
	uint32_t cursor = (mouse_port() == 0) ? reg_usb0_cursor : reg_usb1_cursor;
	return cursor & 0x3FF;
}

static inline int get_cursor_y(void) {
	uint32_t cursor = (mouse_port() == 0) ? reg_usb0_cursor : reg_usb1_cursor;
	return (cursor >> 10) & 0x3FF;
}

static inline uint8_t get_mouse_btn(void) {
	uint32_t cursor = (mouse_port() == 0) ? reg_usb0_cursor : reg_usb1_cursor;
	return (cursor >> 20) & 0x0F;
}

// -- keyboard --
//
// unlike the mouse above, keyboard capture is interrupt-driven, not
// polled -- see sw/os/hid.c. hid_read_key() pops one already-decoded
// press/release edge event at a time from the kernel's small event
// ring, drawn from BOTH usb ports (hid.c decides per-port; this side
// just gets a merged stream). wm drains all of them every main-loop
// iteration (there can be more than one queued since the last time we
// got scheduled) and forwards each to the *focused* window's owner
// only, translating the raw USB HID usage code to a keysym (zkbd.h)
// first. Demo windows (owned by wm itself, see main() below) have no
// app to notify, same as notify_moved()'s check below.
static void dispatch_keys(void) {

	int32_t ev;
	while ((ev = hid_read_key()) >= 0) {

		uint8_t usage     = (ev >> 1) & 0xFF;
		uint8_t modifiers = (ev >> 9) & 0xFF;
		bool    pressed   = (ev & 1) != 0;

		if (focused < 0) continue;
		if (windows[focused].owner_pid == my_pid) continue;

		uint32_t keysym = z_kbd_usage_to_keysym(usage, modifiers);
		if (keysym == Z_KEY_NONE) continue;   // bare modifier change, or
		                                       // an unmapped usage code

		uint32_t packed = Z_WM_PACK_KEY(keysym, modifiers, pressed);
		z_msg_new_send(windows[focused].owner_pid, Z_WM_KEY, 0, z_obj_uint32(packed));

	}

}

// -- window table --

// fixed_x/fixed_y: pass -1,-1 for the usual auto-placed cascade
// (the only thing every existing caller wanted); pass both >= 0 to
// place the window at that exact position instead -- the dock (see
// create_dock() below) is the only caller that needs this, since it
// has one fixed spot (bottom left) rather than wanting to cascade
// with everything else.
static int create_window(uint32_t owner_pid, const char *title,
	uint32_t w, uint32_t h, int32_t fixed_x, int32_t fixed_y) {

	for (int i = 0; i < WM_MAX_WINDOWS; i++) {

		if (windows[i].used) continue;

		windows[i].used = true;
		windows[i].owner_pid = owner_pid;
		windows[i].w = w;
		windows[i].h = h;
		// reset explicitly -- this slot may have been a no-titlebar
		// window (the dock) before being destroyed and reused, and
		// there's nothing else that clears it back to the normal
		// default.
		windows[i].no_titlebar = false;

		if (fixed_x >= 0 && fixed_y >= 0) {
			windows[i].x = (uint32_t)fixed_x;
			windows[i].y = (uint32_t)fixed_y;
		} else {
			// simple cascade -- see docs/window_manager.md, placement
			// strategy was left for a later pass
			uint32_t n = zorder_count;
			windows[i].x = 20 + (n % 8) * 24;
			windows[i].y = 20 + (n % 8) * 20;
		}

		if (title) {
			strncpy(windows[i].title, title, WM_TITLE_MAX - 1);
			windows[i].title[WM_TITLE_MAX - 1] = 0;
		} else {
			windows[i].title[0] = 0;
		}

		zorder[zorder_count++] = i;

		printf("wm: created window %d '%s' owner=%ld at (%ld,%ld) %ldx%ld\n",
			i, windows[i].title, (long)owner_pid,
			(long)windows[i].x, (long)windows[i].y,
			(long)windows[i].w, (long)windows[i].h);

		return i;

	}

	printf("wm: create_window failed -- no free window slots\n");
	return -1;

}

// creates the dock -- called once from main(), after any other
// startup windows so it's naturally frontmost in zorder to begin
// with (create_window() always appends to the front -- see its own
// comment). Position is fixed (bottom left, see the dock's own
// header comment for the constants), so this goes through
// create_window()'s fixed_x/fixed_y path rather than the normal
// cascade every other window uses. owner_pid is my_pid, same as the
// demo windows -- see main().
static int create_dock(void) {

	uint32_t w = DOCK_PADDING * 2 + DOCK_APP_COUNT * DOCK_ICON_SIZE +
		(DOCK_APP_COUNT - 1) * DOCK_ICON_GAP;
	uint32_t h = DOCK_PADDING * 2 + DOCK_ICON_SIZE;

	int32_t x = DOCK_MARGIN;
	int32_t y = WM_SCREEN_H - (int32_t)h - DOCK_MARGIN;

	int idx = create_window(my_pid, "Dock", w, h, x, y);
	if (idx < 0) return -1;

	windows[idx].no_titlebar = true;

	return idx;

}

// returns true if idx's z-order position actually changed
static bool bring_to_front(int idx) {

	int found = -1;
	for (int i = 0; i < zorder_count; i++) {
		if (zorder[i] == idx) { found = i; break; }
	}
	if (found < 0) return false;
	if (found == zorder_count - 1) return false;	// already frontmost

	for (int i = found; i < zorder_count - 1; i++)
		zorder[i] = zorder[i + 1];
	zorder[zorder_count - 1] = idx;

	return true;

}

static void destroy_window(uint32_t id) {

	if (id >= WM_MAX_WINDOWS || !windows[id].used) return;

	int ox = (int)windows[id].x, oy = (int)windows[id].y;
	int ow = (int)windows[id].w, oh = (int)windows[id].h;

	windows[id].used = false;

	int found = -1;
	for (int i = 0; i < zorder_count; i++) {
		if (zorder[i] == id) { found = i; break; }
	}
	if (found >= 0) {
		for (int i = found; i < zorder_count - 1; i++)
			zorder[i] = zorder[i + 1];
		zorder_count--;
	}

	if (focused == (int)id) focused = -1;
	if (dragging == (int)id) dragging = -1;

	printf("wm: destroyed window %ld\n", (long)id);

	// window is already removed from windows[]/zorder above, so this
	// only redraws/notifies whatever else was overlapping its old spot
	repair_region(ox, oy, ow, oh, -1);

}

// returns the frontmost window containing (cx,cy), or -1
static int hit_test(int cx, int cy) {

	for (int i = zorder_count - 1; i >= 0; i--) {
		int idx = zorder[i];
		wm_window_t *w = &windows[idx];
		if (cx >= (int)w->x && cx <= (int)(w->x + w->w - 1) &&
			cy >= (int)w->y && cy <= (int)(w->y + w->h - 1))
			return idx;
	}

	return -1;

}

static bool hit_titlebar(int idx, int cy) {
	if (windows[idx].no_titlebar) return false;	// nothing to hit -- see the dock
	return (cy < (int)(windows[idx].y + Z_WM_TITLEBAR_H));
}

// handles a click already known to have landed inside the dock's own
// rect (see the dock_idx branch in main()'s click handling below) --
// maps the click to an icon slot, if any (clicks in the padding
// between/around icons land on no slot and are ignored), and launches
// that slot's app via z_proc_run(). Always spawns a fresh process,
// same as running `run <app>` twice from the shell would -- no
// tracking of whether the app is "already running" (see
// docs/window_manager.md for anything that changes about that later).
static void dock_click(int cx, int cy) {

	wm_window_t *w = &windows[dock_idx];

	int local_x = cx - (int)w->x - DOCK_PADDING;
	int local_y = cy - (int)w->y - DOCK_PADDING;

	if (local_y < 0 || local_y >= DOCK_ICON_SIZE) return;	// in the padding, not an icon
	if (local_x < 0) return;

	int stride = DOCK_ICON_SIZE + DOCK_ICON_GAP;
	int slot = local_x / stride;
	int slot_x = local_x % stride;

	if (slot < 0 || slot >= DOCK_APP_COUNT) return;
	if (slot_x >= DOCK_ICON_SIZE) return;	// in the gap between icons

	const char *name = dock_apps[slot].name;
	printf("wm: dock: launching '%s'\n", name);

	uint32_t pid = z_proc_run(name);
	if (!pid)
		printf("wm: dock: failed to launch '%s' -- file missing, or no free process slot\n", name);
	else
		printf("wm: dock: launched '%s' as pid %ld\n", name, (long)pid);

}

// -- app protocol --

static void send_win_rect(uint32_t to, uint32_t subject, uint32_t tag, int idx) {

	z_obj_t msg = z_obj_map(5);

	if (idx >= 0) {
		z_map_set(&msg, "id", z_obj_int32(idx));
		z_map_set(&msg, "x", z_obj_uint32(windows[idx].x));
		z_map_set(&msg, "y", z_obj_uint32(windows[idx].y));
		z_map_set(&msg, "w", z_obj_uint32(windows[idx].w));
		z_map_set(&msg, "h", z_obj_uint32(windows[idx].h));
	} else {
		z_map_set(&msg, "id", z_obj_int32(-1));
	}

	z_msg_new_send(to, subject, tag, msg);

	// note: `msg` is intentionally never freed here. it's a borrowed
	// payload until the recipient reads it (see docs/messaging.md) --
	// wm doesn't wait for that, so freeing immediately would race.
	// same accepted-leak tradeoff as the ping/pong demo.

}

static void handle_message(z_msg_t *msg) {

	switch (msg->subject) {

		case Z_WM_CREATE_WINDOW: {

			char title[WM_TITLE_MAX] = "";
			uint32_t w = Z_WM_DEFAULT_WIDTH, h = Z_WM_DEFAULT_HEIGHT;

			z_obj_t *t = z_map_find(&msg->obj, "title");
			if (t && t->type == Z_STR && t->val.str)
				strncpy(title, t->val.str, WM_TITLE_MAX - 1);

			z_obj_t *wo = z_map_find(&msg->obj, "w");
			if (wo && wo->type == Z_UINT32) w = wo->val.uint32;

			z_obj_t *ho = z_map_find(&msg->obj, "h");
			if (ho && ho->type == Z_UINT32) h = ho->val.uint32;

			int idx = create_window(msg->from, title, w, h, -1, -1);

			// keep the dock frontmost -- create_window() always
			// appends new windows to the front of zorder (see its
			// own comment), which would otherwise let a freshly
			// created app window cover the dock.
			if (idx >= 0 && dock_idx >= 0 && idx != dock_idx)
				bring_to_front(dock_idx);

			// auto-focus a newly created window if nothing is
			// currently focused -- without this, a session with no
			// working mouse (no mouse plugged in, or neither USB port
			// currently reporting itself as one -- see mouse_port()
			// above) has no way to ever focus a window at all, since
			// focus is otherwise only ever set by a mouse click. only
			// fires when nothing else is focused yet, so it won't
			// steal focus from an already-focused window when a
			// second app creates one later.
			if (idx >= 0 && focused < 0) focused = idx;

			// draw this window's chrome (and repair anything it now
			// covers) BEFORE replying -- otherwise the owner's
			// z_win_create() can return, and its first drawing calls
			// can run, before the wm has drawn so much as a border
			// or titlebar for it at all. exclude_idx=idx: see
			// repair_region()'s own comment for why this window
			// specifically must skip the redraw-notify+wait step.
			if (idx >= 0)
				repair_region(windows[idx].x, windows[idx].y,
					windows[idx].w, windows[idx].h, idx);

			send_win_rect(msg->from, Z_WM_WINDOW_CREATED, msg->tag, idx);

			break;

		}

		case Z_WM_DESTROY_WINDOW:

			if (msg->obj.type == Z_UINT32)
				destroy_window(msg->obj.val.uint32);	// repairs its own region

			break;

		default:
			break;

	}

}

static void notify_moved(int idx) {

	// demo windows (owned by wm itself) have no app to notify
	if (windows[idx].owner_pid == my_pid) return;

	send_win_rect(windows[idx].owner_pid, Z_WM_WINDOW_MOVED, 0, idx);

}

// repairs a completed drag's swept region (drag_min_x/y..drag_max_x/y,
// see the drag-update block below) -- deliberately EXCLUDING the
// dragged window's own final footprint. that footprint's border is
// already correct: the wireframe drag kept it in sync at every step,
// so clearing and redrawing it here would be pure waste, and visibly
// flashes something that didn't need to change. what genuinely does
// need repairing is the "wake" the window left behind -- the parts of
// the swept region it's no longer sitting on, which still have stale
// old content in them (content stays frozen during a drag, by
// design -- see docs/window_manager.md) -- decomposed into up to four
// surrounding strips (top/bottom/left/right of the window's final
// rect). the window's own content still needs a fresh redraw (it was
// frozen too), so that's requested directly, without touching chrome.
static void repair_drag(int dragged_idx) {

	wm_window_t *w = &windows[dragged_idx];
	int fx = (int)w->x, fy = (int)w->y, fw = (int)w->w, fh = (int)w->h;

	// temporary diagnostic instrumentation -- tracking down a
	// sometimes-crash-on-release bug. low frequency (once per drag
	// release, not per drag-update step), so shouldn't itself
	// perturb timing the way heavier instrumentation has elsewhere
	// in this project's history. remove once resolved.
	printf("wm: repair_drag win %d final=(%d,%d,%d,%d) swept=(%d,%d)-(%d,%d)\n",
		dragged_idx, fx, fy, fw, fh, drag_min_x, drag_min_y, drag_max_x, drag_max_y);

	if (drag_min_y < fy) {
		printf("wm: repair_drag: top strip (%d,%d,%d,%d)\n",
			drag_min_x, drag_min_y, drag_max_x - drag_min_x, fy - drag_min_y);
		repair_region(drag_min_x, drag_min_y, drag_max_x - drag_min_x, fy - drag_min_y, -1);
	}
	if (fy + fh < drag_max_y) {
		printf("wm: repair_drag: bottom strip (%d,%d,%d,%d)\n",
			drag_min_x, fy + fh, drag_max_x - drag_min_x, drag_max_y - (fy + fh));
		repair_region(drag_min_x, fy + fh, drag_max_x - drag_min_x, drag_max_y - (fy + fh), -1);
	}
	if (drag_min_x < fx) {
		printf("wm: repair_drag: left strip (%d,%d,%d,%d)\n",
			drag_min_x, fy, fx - drag_min_x, fh);
		repair_region(drag_min_x, fy, fx - drag_min_x, fh, -1);
	}
	if (fx + fw < drag_max_x) {
		printf("wm: repair_drag: right strip (%d,%d,%d,%d)\n",
			fx + fw, fy, drag_max_x - (fx + fw), fh);
		repair_region(fx + fw, fy, drag_max_x - (fx + fw), fh, -1);
	}

	printf("wm: repair_drag: strips done\n");

	if (w->owner_pid != my_pid) {
		send_redraw(w->owner_pid, dragged_idx);
		wait_for_redraw_done(w->owner_pid);
	}

}

// -- main loop --

int main(void) {

	my_pid = z_getpid();

	// registers as "wm0" (see sw/os/pidreg.h) -- what zwin.c's
	// z_win_create() looks up now, instead of assuming Z_PID_WM.
	// Not fatal if this fails (registry full -- shouldn't happen in
	// practice, wm is normally the very first thing started): callers
	// still have the Z_PID_WM fallback, so wm just runs without a
	// discoverable name in that unlikely case.
	char wm_name[24];
	if (z_pid_register("wm", wm_name, sizeof(wm_name)))
		printf("wm: starting as pid %ld, registered as '%s'.\n", (long)my_pid, wm_name);
	else
		printf("wm: starting as pid %ld (name registration failed).\n", (long)my_pid);

	for (int i = 0; i < WM_MAX_WINDOWS; i++)
		windows[i].used = false;

	// one explicit clear here, in case something else left stale
	// pixels in the framebuffer before wm started -- repair_region()
	// (used everywhere else from here on) deliberately never touches
	// more of the screen than it has to.
	clear_screen();

	// loads z_font_5x8's glyph data into hardware glyph memory once,
	// here, at startup -- and NOWHERE else on the whole board. This
	// is a deliberate change from the earlier per-app convention
	// (hello_win/term each called z_gfx_hw_font_load() themselves
	// before their own first draw): that convention only worked by
	// accident, since the shared glyph memory (rtl/mem/glyph.v) holds
	// exactly one font's data at a time with no arbitration over
	// whose load call "wins" if two processes using different fonts
	// are both drawing around the same time -- see
	// docs/window_manager.md, "Hardware glyph blitting". Now wm is
	// the sole owner: it loads z_font_5x8 once and never reloads or
	// swaps it, and every app (including the dock, see draw_dock())
	// is expected to only ever draw with z_font_5x8 -- there's
	// currently no second font in use anywhere, by design, precisely
	// so nothing else needs its own z_gfx_hw_font_load() call or has
	// any reason to fight over what's in glyph memory. If a genuine
	// need for a second font shows up later, this single-owner
	// approach needs real rethinking (per-window glyph regions, or a
	// load-before-every-draw-you-actually-own discipline like an
	// earlier version of draw_dock() used) -- it does not extend to
	// "just call z_gfx_hw_font_load() again from wherever needs it".
	//
	// z_font_5x8 replaced z_font_5x7 here (and in every other
	// Z_GFX_HW_BLIT consumer -- sw/apps/term, sw/apps/hello_win, see
	// their own comments) after real-hardware testing showed the
	// bottom pixel row of z_font_5x7 glyphs getting cut off on
	// screen -- see zfont.h's own z_font_5x8 comment. wm/term/
	// hello_win must all agree on which font is currently loaded
	// (this is exactly the single-owner constraint the paragraph
	// above describes), so this had to change in all three together,
	// not just here.
	z_gfx_hw_font_load(&z_font_5x8);

/*
	// demo windows so there's something to see/drag before a real
	// client app exists -- see the file header comment.
	int demo1 = create_window(my_pid, "Window 1", 140, 100, -1, -1);
	int demo2 = create_window(my_pid, "Window 2", 140, 100, -1, -1);
	if (demo1 >= 0)
		repair_region(windows[demo1].x, windows[demo1].y, windows[demo1].w, windows[demo1].h, -1);
	if (demo2 >= 0)
		repair_region(windows[demo2].x, windows[demo2].y, windows[demo2].w, windows[demo2].h, -1);
*/

	// dock -- created last so it starts out frontmost (see
	// create_dock()'s own comment); bring_to_front(dock_idx) calls
	// elsewhere in this file keep it that way as other windows come
	// and go.
	dock_idx = create_dock();
	if (dock_idx >= 0)
		repair_region(windows[dock_idx].x, windows[dock_idx].y,
			windows[dock_idx].w, windows[dock_idx].h, -1);

	uint8_t last_btn = 0;

	// diagnostic: prints whenever either USB HID port's device type
	// changes (rtl/ext/usb_hid_host/src/usb_hid_host.v's `typ`
	// register -- 0=none, 1=keyboard, 2=mouse, 3=gamepad). there are
	// two independent ports (see zeitlos.h) with no fixed
	// port-to-device mapping -- mouse_port() above and dispatch_keys()
	// each decide dynamically which port is which. watch these lines
	// on the UART console to confirm what the board currently sees on
	// each port.
	uint8_t last_hid_typ0 = 0xFF, last_hid_typ1 = 0xFF;	// impossible value -- forces the first print
	static const char *hid_typ_names[4] = { "none", "keyboard", "mouse", "gamepad" };

	while (1) {

		uint8_t hid_typ0 = (reg_usb0_info >> 24) & 0x3;
		if (hid_typ0 != last_hid_typ0) {
			printf("wm: usb port 0 device type -> %s\n", hid_typ_names[hid_typ0]);
			last_hid_typ0 = hid_typ0;
		}

		uint8_t hid_typ1 = (reg_usb1_info >> 24) & 0x3;
		if (hid_typ1 != last_hid_typ1) {
			printf("wm: usb port 1 device type -> %s\n", hid_typ_names[hid_typ1]);
			last_hid_typ1 = hid_typ1;
		}

		// -- drain incoming requests (non-blocking) --
		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK)
			handle_message(&msg);

		// -- keyboard (interrupt-captured, drained here) --
		dispatch_keys();

		// -- mouse --
		int cx = get_cursor_x();
		int cy = get_cursor_y();
		uint8_t btn = get_mouse_btn();
		bool btn_down = (btn & 1) != 0;
		bool btn_was_down = (last_btn & 1) != 0;

		if (btn_down && !btn_was_down) {

			int hit = hit_test(cx, cy);

			// temporary debug instrumentation -- see docs/window_manager.md
			// "cursor calibration" note. remove once the coordinate mapping
			// is confirmed against real hardware. raw comes from
			// whichever port mouse_port() decided is the mouse, same as
			// the cx/cy computed from it.
			int mp = mouse_port();
			printf("wm: click port=%d raw=0x%08lx cx=%d cy=%d", mp,
				(unsigned long)(mp == 0 ? reg_usb0_cursor : reg_usb1_cursor), cx, cy);
			if (hit >= 0) {
				printf(" -> hit win %d (x=%ld y=%ld w=%ld h=%ld) titlebar=%d\n",
					hit, (long)windows[hit].x, (long)windows[hit].y,
					(long)windows[hit].w, (long)windows[hit].h,
					hit_titlebar(hit, cy));
			} else {
				printf(" -> no hit\n");
			}

			if (dock_idx >= 0 && hit == dock_idx) {

				// dock click: never changes focus, z-order, or
				// starts a drag (hit_titlebar() would say no anyway,
				// since no_titlebar is set -- see its own comment) --
				// just figure out which icon slot, if any, was
				// clicked, and launch that app. see dock_click() below.
				dock_click(cx, cy);

			} else if (hit >= 0) {

				bool focus_changed = (focused != hit);
				int old_focused = focused;
				if (focus_changed) focused = hit;

				bool reordered = bring_to_front(hit);

				// keep the dock frontmost -- see its own comment
				// where this same call appears in handle_message().
				if (dock_idx >= 0) bring_to_front(dock_idx);

				if (focus_changed && old_focused >= 0)
					repair_region(windows[old_focused].x, windows[old_focused].y,
						windows[old_focused].w, windows[old_focused].h, -1);

				if (focus_changed || reordered)
					repair_region(windows[hit].x, windows[hit].y,
						windows[hit].w, windows[hit].h, -1);

				if (hit_titlebar(hit, cy)) {
					dragging = hit;
					drag_off_x = cx - windows[hit].x;
					drag_off_y = cy - windows[hit].y;
					drag_min_x = windows[hit].x;
					drag_min_y = windows[hit].y;
					drag_max_x = windows[hit].x + windows[hit].w;
					drag_max_y = windows[hit].y + windows[hit].h;
				}

			}

		}

		if (btn_down && dragging >= 0) {

			int32_t nx = cx - drag_off_x;
			int32_t ny = cy - drag_off_y;

			if (nx < 0) nx = 0;
			if (ny < 0) ny = 0;
			if (nx + (int32_t)windows[dragging].w > WM_SCREEN_W)
				nx = WM_SCREEN_W - windows[dragging].w;
			if (ny + (int32_t)windows[dragging].h > WM_SCREEN_H)
				ny = WM_SCREEN_H - windows[dragging].h;

			if ((uint32_t)nx != windows[dragging].x ||
				(uint32_t)ny != windows[dragging].y) {

				// wireframe drag: move just this window's own border,
				// cheaply, instead of a full-screen clear+redraw+
				// content-notify on every step -- that was queuing up
				// redraw messages faster than apps could drain them,
				// which is what made content look like it was playing
				// back in slow motion after the fact. content (this
				// window's own, and anything underneath the border's
				// old position) is left alone until the drag
				// completes, at which point one repair_region() over
				// everywhere the window passed through puts it all
				// back correctly. see docs/window_manager.md.
				draw_window_box(&windows[dragging], dragging == focused, 0);
				windows[dragging].x = nx;
				windows[dragging].y = ny;
				draw_window_box(&windows[dragging], dragging == focused, 1);

				if (nx < drag_min_x) drag_min_x = nx;
				if (ny < drag_min_y) drag_min_y = ny;
				if (nx + (int32_t)windows[dragging].w > drag_max_x)
					drag_max_x = nx + (int32_t)windows[dragging].w;
				if (ny + (int32_t)windows[dragging].h > drag_max_y)
					drag_max_y = ny + (int32_t)windows[dragging].h;

			}

		}

		if (!btn_down && btn_was_down && dragging >= 0) {
			printf("wm: drag release win %d final x=%ld y=%ld\n",
				dragging, (long)windows[dragging].x, (long)windows[dragging].y);
			notify_moved(dragging);
			repair_drag(dragging);
			dragging = -1;
		}

		last_btn = btn;

		for (volatile int i = 0; i < 2000; i++); // light throttle

	}

}
