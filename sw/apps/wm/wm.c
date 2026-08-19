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

#define WM_MAX_WINDOWS    16
#define WM_SCREEN_W       640
#define WM_SCREEN_H       480
#define WM_TITLE_MAX      24

typedef struct {
	bool		used;
	uint32_t	owner_pid;
	uint32_t	x, y, w, h;
	char		title[WM_TITLE_MAX];
} wm_window_t;

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
	int ty = w->y + Z_WM_TITLEBAR_H;

	z_fb_hw_box(x0, y0, x1, y1, color, NULL);
	z_fb_hw_line(x0, ty, x1, ty, color, NULL);	// titlebar separator

	if (is_focused) {
		// bolder border for the focused window -- a 1px inset outline.
		// text rendering isn't wired up yet (see docs/window_manager.md),
		// so this is the only visual focus indicator for now.
		z_fb_hw_box(x0 + 1, y0 + 1, x1 - 1, y1 - 1, color, NULL);
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

static int create_window(uint32_t owner_pid, const char *title,
	uint32_t w, uint32_t h) {

	for (int i = 0; i < WM_MAX_WINDOWS; i++) {

		if (windows[i].used) continue;

		windows[i].used = true;
		windows[i].owner_pid = owner_pid;
		windows[i].w = w;
		windows[i].h = h;

		// simple cascade -- see docs/window_manager.md, placement
		// strategy was left for a later pass
		uint32_t n = zorder_count;
		windows[i].x = 20 + (n % 8) * 24;
		windows[i].y = 20 + (n % 8) * 20;

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
	return (cy < (int)(windows[idx].y + Z_WM_TITLEBAR_H));
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

			int idx = create_window(msg->from, title, w, h);

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

	// demo windows so there's something to see/drag before a real
	// client app exists -- see the file header comment.
	int demo1 = create_window(my_pid, "Window 1", 140, 100);
	int demo2 = create_window(my_pid, "Window 2", 140, 100);
	if (demo1 >= 0)
		repair_region(windows[demo1].x, windows[demo1].y, windows[demo1].w, windows[demo1].h, -1);
	if (demo2 >= 0)
		repair_region(windows[demo2].x, windows[demo2].y, windows[demo2].w, windows[demo2].h, -1);

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

			if (hit >= 0) {

				bool focus_changed = (focused != hit);
				int old_focused = focused;
				if (focus_changed) focused = hit;

				bool reordered = bring_to_front(hit);

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
