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
#include "../../common/zsoc.h"	// Z_TICK_HZ, z_cursor_set_busy()
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"
#include "../../common/zicon.h"
#include "dock_icons.h"
#include "win_icons.h"

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
	// Z_WIN_FLAG_* bitmask (zwm.h) from this window's own
	// Z_WM_CREATE_WINDOW request -- currently only the close-icon
	// flags. See close_icon_rect()/hit_close_icon()/
	// draw_titlebar_content()/handle_close_click() below.
	uint32_t	flags;
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

// Everything the dock COULD show. What it actually shows is decided at
// startup by dock_build(), which keeps only the ones that resolve.
//
// An icon for an app that isn't installed is a button that does
// nothing, and which apps exist genuinely varies per machine now: the
// core apps are in flash and always present (sw/os/zar.h), while
// anything else only exists if somebody put it on an sdcard. Listing a
// candidate here is therefore an offer, not a promise -- adding a new
// app to the dock needs no conditional logic, just an entry and an
// icon.
static const dock_app_t dock_candidates[] = {
	{ "term",	z_icon_term_data  },
	{ "gpu3d",	z_icon_gpu3d_data },
};
#define DOCK_CANDIDATE_COUNT \
	(int)(sizeof(dock_candidates) / sizeof(dock_candidates[0]))

// The subset actually present, filled in by dock_build(). Pointers
// into dock_candidates[] rather than copies, so the icon data isn't
// duplicated.
static const dock_app_t *dock_apps[DOCK_CANDIDATE_COUNT];
static int dock_app_count;

// Everything below indexes the live set, so this stays spelled the way
// it always was.
#define DOCK_APP_COUNT   dock_app_count

#define DOCK_ICON_SIZE   32	// fixed icon size, see file header comment
#define DOCK_ICON_GAP     4	// space between adjacent icons
#define DOCK_PADDING      4	// space between icons and the dock's own border
#define DOCK_MARGIN       8	// space between the dock and the screen edges

// index into windows[]/zorder of the dock, or -1 before it's created
// -- set once in main() and never destroyed, so unlike other window
// indices this one doesn't need the usual "does this id still exist"
// checking.
static int dock_idx = -1;

// -- dock keyboard navigation / launch feedback -- see
// docs/window_manager.md, "Keyboard-only operation" --
//
// which dock icon is currently selected for keyboard navigation
// (Left/Right/Up/Down while the dock itself has focus -- see
// dock_handle_key() below), or -1 before the dock has ever had
// keyboard focus. Only actually drawn (as a selection ring, see
// draw_dock()) while the dock IS focused -- see dispatch_keys()'s own
// `focused == dock_idx` check -- so this can stay set to wherever it
// last was even after focus moves elsewhere, and picks up right where
// it left off next time.
static int dock_selected = -1;

// per-slot "an app launched from this icon hasn't created its first
// window yet" state -- drawn as an inverted icon (draw_dock()) and
// used to prevent re-launching the same app a second time from an
// impatient click or Enter press while it's still starting up (see
// dock_launch()). Cleared either when the launched process (matched
// by pid, dock_launching_pid[]) creates its first window (handle_
// message()'s Z_WM_CREATE_WINDOW case) or after DOCK_LAUNCH_TIMEOUT_
// ITERS main-loop iterations with no window (main()'s own loop) --
// see that constant's own comment for why a timeout exists at all.
static bool dock_launching[DOCK_CANDIDATE_COUNT];
static uint32_t dock_launching_pid[DOCK_CANDIDATE_COUNT];
static uint32_t dock_launching_deadline[DOCK_CANDIDATE_COUNT];

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
// forward-declared so the keyboard hotkey handlers (alt_tab()/
// alt_move_focused(), defined ahead of dispatch_keys() further down --
// see their own comments) can reuse the exact same focus/z-order/
// screen-repair machinery the mouse path already uses, rather than
// duplicating it -- these three are otherwise only defined later in
// the file (bring_to_front() in "-- window table --", notify_moved()/
// repair_drag() in "-- app protocol --").
static bool bring_to_front(int idx);
static void notify_moved(int idx);
static void repair_drag(int dragged_idx);

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
		// bolder border for the focused window -- a 1px OUTSET
		// outline, drawn just outside the window's own frame rather
		// than 1px inside it. Used to be inset (x0+1,y0+1,x1-1,y1-1),
		// which put it directly against content -- z_win_content_rect()
		// (zwin.c) had to reserve an entire extra pixel of margin on
		// every content-bearing edge just to keep glyphs from
		// visually gnawing at it. Moving it outward reclaims that
		// pixel for content -- see zwin.c's own updated comment.
		//
		// Clamped to the screen's own bounds before drawing: the
		// hardware rasterizer's coordinate registers are unsigned
		// (rtl/gpu/gpu_raster.v), so a window sitting flush against
		// x=0 or y=0 would otherwise send a negative coordinate that
		// wraps to a huge value instead of clipping, corrupting
		// framebuffer memory far outside the intended region -- same
		// class of risk z_fb_hw_line()'s own header comment already
		// documents for exactly this reason. Ordinary cascaded window
		// placement keeps windows clear of the edges in practice, but
		// a dragged window can still end up flush against one -- this
		// has to hold regardless of how a window got there.
		//
		// repair_region() (below) expands whatever region it's given
		// by this same 1px on every side before clearing/redrawing,
		// specifically so this outward ring's own pixels get properly
		// cleared/redrawn on every focus change -- see its own
		// comment for why that's centralized there instead of at
		// every individual call site.
		int fx0 = x0 - 1, fy0 = y0 - 1, fx1 = x1 + 1, fy1 = y1 + 1;
		if (fx0 < 0) fx0 = 0;
		if (fy0 < 0) fy0 = 0;
		if (fx1 >= WM_SCREEN_W) fx1 = WM_SCREEN_W - 1;
		if (fy1 >= WM_SCREEN_H) fy1 = WM_SCREEN_H - 1;
		z_fb_hw_box(fx0, fy0, fx1, fy1, color, NULL);
	}

}

// -- titlebar text + close icon --
//
// left-aligned window title text ("term0", "gpu3d0", ...) plus an
// optional close icon (Z_WIN_FLAG_CLOSE_ICON, zwm.h) on the right --
// both hardware-glyph-blitted (z_fb_draw_text()/z_fb_draw_icon(),
// zgfx.h), same as every other piece of window chrome wm draws.
// Deliberately NOT part of draw_window_box(): that function is also
// called, with color=0 then color=1, on every single step of a
// wireframe drag (see the dragging block in main() below) to
// cheaply move just the border -- content stays frozen during a
// drag by design (repair_drag()'s own comment), and titlebar
// text/icon are exactly that kind of content, not border. Redrawing
// them on every drag step would defeat the whole point of the
// wireframe-only move (cheap, uninterrupted) for zero visual benefit,
// since the title/icon don't move relative to the border anyway.
// Instead this is called once, from repair_region() below, at
// exactly the same point draw_dock() already is -- after chrome is
// (re)drawn following a create/destroy/focus-change/drag-release, not
// on every intermediate step.
#define Z_WM_TITLE_TEXT_MARGIN_X   3
#define Z_WM_CLOSE_ICON_MARGIN_X   2
#define Z_WM_CLOSE_ICON_GAP        3	// min gap kept between the end
					// of the title text and the icon

// vertical extent of the titlebar's actual INTERIOR -- i.e.
// Z_WM_TITLEBAR_H (zwm.h) minus the 1px top border row (row 0,
// drawn as part of the window's outer box by draw_window_box(), not
// titlebar content). Titlebar content must be centered within THIS,
// not within Z_WM_TITLEBAR_H itself -- treating the border row as
// available space was exactly the bug that made the close icon look
// about 1px too high (it was centered as if 12 rows were free, when
// only 11 -- now 10 -- actually were). See zwm.h's own comment on
// Z_WM_TITLEBAR_H for the full numbers.
#define Z_WM_TITLEBAR_CONTENT_Y0   1
#define Z_WM_TITLEBAR_CONTENT_H    (Z_WM_TITLEBAR_H - Z_WM_TITLEBAR_CONTENT_Y0)

// absolute (screen-relative) y to draw `h` pixels of titlebar content
// at, centered within Z_WM_TITLEBAR_CONTENT_H -- shared by
// draw_titlebar_content()'s title text and close icon (both 8px tall
// right now, z_font_5x8.h/Z_ICON_H, so they land on the exact same
// row and visually line up), and by close_icon_rect() below, so
// nothing computes this independently and risks disagreeing.
static int titlebar_content_y(const wm_window_t *w, int h) {
	return (int)w->y + Z_WM_TITLEBAR_CONTENT_Y0 + (Z_WM_TITLEBAR_CONTENT_H - h) / 2;
}

// computes the close icon's on-screen rect for window `w` -- shared
// between draw_titlebar_content() (below) and hit_close_icon() (see
// hit_titlebar()'s neighborhood below) so the two can never silently
// disagree about where the icon actually is, the same "compute once,
// share everywhere" reasoning zwin.c's own z_win_content_rect()
// comment gives for the exact same class of bug.
static void close_icon_rect(const wm_window_t *w, int *out_x, int *out_y) {
	int x1 = (int)(w->x + w->w - 1);
	*out_x = x1 - Z_WM_CLOSE_ICON_MARGIN_X - Z_ICON_W + 1;
	*out_y = titlebar_content_y(w, Z_ICON_H);
}

static void draw_titlebar_content(wm_window_t *w) {

	if (w->no_titlebar) return;	// nothing to draw -- see the dock

	int x0 = (int)w->x, y0 = (int)w->y;
	int x1 = (int)(w->x + w->w - 1);

	bool has_close = (w->flags & Z_WIN_FLAG_CLOSE_ICON) != 0;
	int close_x = 0, close_y = 0;
	if (has_close) close_icon_rect(w, &close_x, &close_y);

	// clip title text to the titlebar strip, and stop it short of the
	// close icon (if any) instead of letting a long title run
	// underneath it. z_fb_draw_text()'s own per-glyph clip (zgfx.c)
	// keeps this pixel-exact for whichever glyph straddles the clip
	// boundary -- the same partial-glyph-falls-back-to-software
	// mechanism every other clipped hardware glyph draw in this
	// codebase already relies on, not something new introduced here.
	z_clip_t clip;
	clip.x0 = x0 + Z_WM_TITLE_TEXT_MARGIN_X;
	clip.y0 = y0;
	clip.x1 = has_close ? (close_x - Z_WM_CLOSE_ICON_GAP - 1) : x1;
	clip.y1 = y0 + Z_WM_TITLEBAR_H - 1;

	if (w->title[0] && clip.x1 >= clip.x0)
		z_fb_draw_text(x0 + Z_WM_TITLE_TEXT_MARGIN_X,
			titlebar_content_y(w, z_font_5x8.h),
			w->title, 1, &z_font_5x8, &clip);

	if (has_close)
		// clip=NULL: same as draw_window_box()'s own chrome draws --
		// wm already computed this rect from the window's own bounds,
		// so it's known on-screen and within the titlebar, nothing
		// left to clip against.
		z_fb_draw_icon(close_x, close_y, Z_ICON_CLOSE, 1, 0, NULL);

}

// -- system-busy state --
//
// A BITMASK of reasons rather than a bool or a counter, deliberately.
// A bool breaks as soon as two things are busy at once -- whichever
// finishes first clears it while the other is still going. A counter
// fixes that but leaks forever on one unbalanced call, and gives you
// nothing to look at when it does. With named reasons, setting the
// same one twice is harmless, clearing it is definitive, and a stuck
// busy state says on the console exactly which reason is stuck.
//
// The visible effect is the mouse cursor: X normally, Z while busy.
// See wm_busy_set()/wm_busy_clear().
#define WM_BUSY_STARTUP   (1u << 0)   // core services not up yet

static uint32_t wm_busy_mask;

// Reports what it wrote and whether the hardware is even there. This
// is deliberately noisy: the cursor is the only visible effect, so a
// silent failure here looks identical to "the feature doesn't work"
// with nothing to go on. Prints once per state change, not per loop.
static void wm_busy_apply(void) {
	// The cursor sprite is drawn in hardware and composited at scanout
	// (rtl/gpu/gpu_cursor.v), so this register is the only way to
	// change its shape -- wm cannot draw over it.
	//
	// Requires a bitstream with rtl/socctl.v: this is an RTL change, so
	// `make flash`, not `make dev-flash`. Harmless on an older
	// bitstream (the write is acked and discarded), it just leaves the
	// cursor as an X.
	bool busy = (wm_busy_mask != 0);

	z_cursor_set_busy(busy);

	// Read it back. socctl's CTRL register is readable, so this
	// distinguishes the three ways this can fail -- no socctl in the
	// bitstream at all, socctl present but the write not landing, and
	// the write landing fine while something else is wrong -- which
	// otherwise all present identically as "the cursor didn't change".
	if (!z_socctl_present()) {
		printf("wm: busy=%d (no socctl in this bitstream, cursor fixed)\n",
			(int)busy);
		return;
	}

	uint32_t rb = reg_socctl_ctrl & Z_SOCCTL_CURSOR_BUSY;
	printf("wm: busy=0x%lx cursor=%s%s\n", (unsigned long)wm_busy_mask,
		rb ? "Z" : "X",
		((rb != 0) == busy) ? "" : " (READBACK MISMATCH)");
}

static void wm_busy_set(uint32_t reason) {
	if (wm_busy_mask & reason) return;
	wm_busy_mask |= reason;
	wm_busy_apply();
}

static void wm_busy_clear(uint32_t reason) {
	if (!(wm_busy_mask & reason)) return;
	wm_busy_mask &= ~reason;
	wm_busy_apply();
}

static bool wm_is_busy(void) {
	return wm_busy_mask != 0;
}

// -- core service readiness --
//
// `term` connects to `repl` over a port as soon as it starts, and that
// connect has a timeout. Launch it before repl has registered itself
// and the connect simply fails -- term comes up as a blank window with
// no indication of why, which is exactly the confusing failure this
// gating exists to prevent.
//
// Checked by name via the pid registry rather than by fixed pid: the
// fixed Z_PID_* values are a fallback, and registration is what
// actually signals "this service is up and listening".
static bool core_services_ready(void) {

	uint32_t pid;
	bool net_up = z_pid_lookup("net0", &pid);
	bool repl_up = z_pid_lookup("repl0", &pid);

	// Log each service the first time it appears, so a service that
	// never registers is obvious from the console rather than showing
	// up only as a cursor that never changes.
	static bool logged_net, logged_repl;
	if (net_up && !logged_net) { logged_net = true; printf("wm: net0 up\n"); }
	if (repl_up && !logged_repl) { logged_repl = true; printf("wm: repl0 up\n"); }

	return net_up && repl_up;

}

// Polled from the main loop until it goes true, then never again.
// Clearing WM_BUSY_STARTUP is what re-enables the dock.
//
// Returns true on the transition, so the caller can repaint. Doing the
// repaint here would need repair_region() forward-declared, and this
// sits above it purely because the busy state has to be declared
// before dock_launch() uses it -- not worth a declaration just to move
// one line.
static bool check_core_services(void) {

	if (!(wm_busy_mask & WM_BUSY_STARTUP)) return false;
	if (!core_services_ready()) return false;

	printf("wm: core services ready\n");
	wm_busy_clear(WM_BUSY_STARTUP);

	return true;

}

// Decides which candidates the dock actually offers.
//
// Called once at startup. z_exec_exists() asks the kernel's own
// resolver (filesystem first, flash core-app archive underneath), so
// the answer is exactly what z_proc_run() would do -- a candidate is
// kept if and only if clicking it would really launch something.
//
// Not re-run when an sdcard is inserted later. Doing that would mean
// resizing and repainting the dock window underneath whatever the user
// is doing, and the dock is drawn once at a size derived from the
// count. Worth revisiting if hotplug ever becomes a thing people
// actually do; for now a reboot picks up a new card.
static void dock_build(void) {

	dock_app_count = 0;

	for (int i = 0; i < DOCK_CANDIDATE_COUNT; i++) {
		if (!z_exec_exists(dock_candidates[i].name)) {
			printf("wm: dock: '%s' not installed, skipping\n",
				dock_candidates[i].name);
			continue;
		}
		dock_apps[dock_app_count++] = &dock_candidates[i];
	}

	printf("wm: dock: %d of %d apps available\n",
		dock_app_count, DOCK_CANDIDATE_COUNT);

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

// draws `bitmap` INVERTED -- a solid-filled slot with the icon's own
// shape cut out of it, rather than the icon lit up against a dark
// slot. Used for the "launching" state (dock_launching[], see its own
// comment) so a launch in progress is visually obvious without adding
// a whole separate iconography -- see draw_dock() below. The caller
// is responsible for the solid fill itself (z_fb_hw_fill_rect(),
// hardware-accelerated, matching this file's "everything through
// gpu_blit/gpu_raster" convention -- see draw_dock()); this function
// only punches the icon's own ink pixels back out to 0 on top of it,
// same per-bit loop as draw_icon_bitmap() above, just inverted.
static void draw_icon_bitmap_inverted(int x0, int y0, const uint8_t *bitmap) {

	for (int row = 0; row < DOCK_ICON_SIZE; row++) {
		for (int col = 0; col < DOCK_ICON_SIZE; col++) {
			uint8_t byte = bitmap[row * (DOCK_ICON_SIZE / 8) + col / 8];
			int bit = (byte >> (7 - (col % 8))) & 1;
			if (bit) z_fb_set_pixel(x0 + col, y0 + row, 0, NULL);
		}
	}

}

// draws the dock's own keyboard-focus indicator around dock_selected
// (a 1px-outset ring, same visual language as a focused window's own
// outset border -- see draw_window_box()'s own comment) -- only ever
// called while the dock itself has keyboard focus (see draw_dock()
// below), same as a window's focus ring is only ever drawn for the
// currently-focused window.
static void draw_dock_selection_ring(int ix, int iy) {
	z_fb_hw_box(ix - 1, iy - 1, ix + DOCK_ICON_SIZE, iy + DOCK_ICON_SIZE, 1, NULL);
}

static void draw_dock(void) {

	if (dock_idx < 0) return;

	wm_window_t *w = &windows[dock_idx];
	int x0 = (int)w->x, y0 = (int)w->y;

	// see dock_selected's own comment -- the ring only shows while
	// the dock itself is the keyboard-focused item, same as it would
	// be redrawn away the instant focus moves elsewhere (repair_
	// region() clears+redraws whatever it covers regardless).
	bool dock_focused = (focused == dock_idx);

	for (int i = 0; i < DOCK_APP_COUNT; i++) {

		int ix = x0 + DOCK_PADDING + i * (DOCK_ICON_SIZE + DOCK_ICON_GAP);
		int iy = y0 + DOCK_PADDING;

		if (dock_focused && i == dock_selected)
			draw_dock_selection_ring(ix, iy);

		z_fb_hw_box(ix, iy, ix + DOCK_ICON_SIZE - 1, iy + DOCK_ICON_SIZE - 1, 1, NULL);

		if (dock_launching[i]) {
			// see dock_launching[]'s own comment -- solid fill, then
			// the icon's own ink pixels punched back out to 0 on top.
			z_fb_hw_fill_rect(ix, iy, DOCK_ICON_SIZE, DOCK_ICON_SIZE, 1);
			draw_icon_bitmap_inverted(ix, iy, dock_apps[i]->bitmap);
		} else {
			draw_icon_bitmap(ix, iy, dock_apps[i]->bitmap);
		}

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

	// expand by 1px on every side before clearing/redrawing -- the
	// focused window's chrome highlight (draw_window_box()'s own
	// comment) is now drawn just OUTSIDE a window's own (x,y,w,h)
	// bounds, so a region computed from a window's own bounds alone
	// (true for most callers here) would otherwise leave the outward
	// ring's outermost pixel uncleared on losing focus, or undrawn on
	// gaining it. Centralized here, once, rather than at every
	// individual call site, so nothing new can forget it. Clamped to
	// the screen's own bounds before use -- same reasoning
	// draw_window_box() itself documents: the hardware rasterizer's
	// coordinate registers are unsigned, so a negative x/y wraps to a
	// huge value instead of clipping, corrupting framebuffer memory
	// far outside the intended region.
	rx -= 1; ry -= 1; rw += 2; rh += 2;
	if (rx < 0) { rw += rx; rx = 0; }
	if (ry < 0) { rh += ry; ry = 0; }
	if (rx + rw > WM_SCREEN_W) rw = WM_SCREEN_W - rx;
	if (ry + rh > WM_SCREEN_H) rh = WM_SCREEN_H - ry;

	fill_rect(rx, ry, rw, rh, 0);

	for (int i = 0; i < zorder_count; i++) {
		int idx = zorder[i];
		wm_window_t *w = &windows[idx];
		if (!rects_overlap(rx, ry, rw, rh,
			(int)w->x, (int)w->y, (int)w->w, (int)w->h))
			continue;
		draw_window_box(w, idx == focused, 1);
		draw_titlebar_content(w);
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

// -- dock launching (shared by mouse click and keyboard Enter) --

// how many main-loop iterations dock_launching[] is allowed to stay
// set before wm gives up waiting and clears it anyway (see the
// timeout check in main()'s own loop) -- a safety net for an app that
// starts but never creates a window at all (crashes early, isn't a
// GUI app, etc), so a single bad launch can't leave that icon
// permanently stuck inverted and unrelaunchable. Not a precise time
// unit -- same caveat REDRAW_ACK_TIMEOUT's own comment gives -- just
// generously past how long even a slow-loading GUI app should ever
// take to get as far as its first z_win_create() call.
// Now a real time budget rather than a loop-iteration count.
//
// Counting iterations was never reliable: loop rate depends on how
// many other processes are runnable, so this same constant meant
// something different with one app running than with five. Adding the
// idle yield at the bottom of the main loop below would have made that
// worse still (the loop now runs at ~Z_TICK_HZ when idle), so it is
// measured against z_uptime_ticks() instead and no longer cares how
// fast the loop happens to be spinning.
#define DOCK_LAUNCH_TIMEOUT_TICKS   (Z_TICK_HZ * 3)

// launches dock_apps[slot], same as a mouse click on that icon (see
// dock_click() below, which now just maps a click to a slot and calls
// this) -- factored out so keyboard activation (dock_handle_key()'s
// Enter case) gets EXACTLY the same launching/re-launch-prevention/
// visual-feedback behavior, not a second copy of it.
static void dock_launch(int slot) {

	if (slot < 0 || slot >= DOCK_APP_COUNT) return;

	// Nothing launches while the system is busy. Right now the only
	// reason is WM_BUSY_STARTUP -- `term` connects to `repl` the
	// moment it starts, and if repl isn't listening yet that connect
	// times out and term comes up as a blank window with no
	// explanation. Refusing the launch is far better than producing a
	// broken one, and the Z cursor is what tells the user to wait.
	if (wm_is_busy()) {
		printf("wm: dock: busy, not launching '%s' yet\n",
			dock_apps[slot]->name);
		return;
	}

	// already launching -- see dock_launching[]'s own comment. Without
	// this, an impatient double-click, or holding Enter down, could
	// fire off several copies of a slow-loading app before the first
	// one even gets as far as creating its window.
	if (dock_launching[slot]) return;

	// mark this icon "launching" and repaint it inverted BEFORE
	// calling z_proc_run() below -- NOT after, which is where this
	// used to happen and was the reason the invert was never actually
	// visible. z_proc_run() (-> k_proc_run(), sw/os/kernel.c) blocks
	// THIS process -- wm itself -- for as long as it takes to load
	// the app's entire binary off the filesystem, which is the actual
	// slow part of "launching" an app; drawing the inverted icon only
	// after that call returns means the invert only ever covered the
	// (usually imperceptibly fast) remainder -- the newly-started
	// process's own C runtime init plus its first z_win_create() call,
	// no disk I/O involved at that point. The framebuffer write below
	// lands in VRAM immediately and stays there, visible on screen,
	// even while wm's own process sits blocked inside z_proc_run()
	// right after -- the display scans out from VRAM independently of
	// whichever process the CPU happens to be running at the time.
	dock_launching[slot] = true;
	dock_launching_pid[slot] = 0;	// not known yet -- see below
	dock_launching_deadline[slot] = z_uptime_ticks() + DOCK_LAUNCH_TIMEOUT_TICKS;

	if (dock_idx >= 0)
		repair_region(windows[dock_idx].x, windows[dock_idx].y,
			windows[dock_idx].w, windows[dock_idx].h, -1);

	const char *name = dock_apps[slot]->name;
	printf("wm: dock: launching '%s'\n", name);

	uint32_t pid = z_proc_run(name);

	if (!pid) {

		printf("wm: dock: failed to launch '%s' -- file missing, or no free process slot\n", name);

		// undo the inverted state immediately -- z_proc_run() failed
		// synchronously (no process was even created), so there's no
		// pid that will ever create a window and clear this the
		// normal way (handle_message()'s Z_WM_CREATE_WINDOW case), and
		// no reason to wait out the timeout either.
		dock_launching[slot] = false;
		if (dock_idx >= 0)
			repair_region(windows[dock_idx].x, windows[dock_idx].y,
				windows[dock_idx].w, windows[dock_idx].h, -1);

		return;

	}

	printf("wm: dock: launched '%s' as pid %ld\n", name, (long)pid);
	dock_launching_pid[slot] = pid;

}

// handles one keysym while the dock itself has keyboard focus (see
// dispatch_keys()'s own `focused == dock_idx` check) -- Left/Up moves
// the selection to the previous icon, Right/Down to the next (both
// wrapping around), Enter launches the selected one via dock_launch()
// above. Returns true if this keysym was one the dock actually
// consumes (regardless of `pressed` -- a matching key-release is
// swallowed too, same as a matching key-press, since there's nothing
// useful to do with either once handled here); false for anything
// else, which falls through to dispatch_keys()'s normal forward-or-
// drop handling (in practice always dropped for the dock, since its
// owner_pid is wm's own -- see that check -- but returning false
// keeps this function honest about which keys it actually owns rather
// than silently swallowing everything while the dock has focus).
static bool dock_handle_key(uint32_t keysym, bool pressed) {

	if (dock_idx < 0 || DOCK_APP_COUNT <= 0) return false;

	bool prev     = (keysym == Z_KEY_LEFT || keysym == Z_KEY_UP);
	bool next     = (keysym == Z_KEY_RIGHT || keysym == Z_KEY_DOWN);
	bool activate = (keysym == CH_CR);   // Enter -- see zkbd.c's usage 0x28 mapping

	if (!prev && !next && !activate) return false;
	if (!pressed) return true;   // own the key, but only act on press

	if (dock_selected < 0) dock_selected = 0;

	if (prev || next) {

		int old_selected = dock_selected;

		if (prev)
			dock_selected = (dock_selected == 0) ? DOCK_APP_COUNT - 1 : dock_selected - 1;
		else
			dock_selected = (dock_selected == DOCK_APP_COUNT - 1) ? 0 : dock_selected + 1;

		if (dock_selected != old_selected)
			repair_region(windows[dock_idx].x, windows[dock_idx].y,
				windows[dock_idx].w, windows[dock_idx].h, -1);

	} else {
		dock_launch(dock_selected);
	}

	return true;

}

// -- global keyboard hotkeys (Alt+Tab, Alt+Arrow) --
//
// handled entirely here, by wm itself, and NEVER forwarded to any
// app's own Z_WM_KEY stream -- see dispatch_keys()'s own comment on
// why these are intercepted before the normal forward-to-focused-
// window path. This is what makes Zeitlos usable keyboard-only: every
// OTHER piece of window management (focus, moving, launching apps) is
// already reachable without a mouse via these plus dock_handle_key()
// above -- see docs/window_manager.md, "Keyboard-only operation".

// returns the next USED window slot after `from`, wrapping around --
// treats every used slot as equally focusable, dock included (see
// this file's own header comment on why keyboard-only operation
// matters), in fixed SLOT order rather than z-order. Slot order, not
// z-order, specifically because the dock is deliberately kept
// frontmost in z-order at all times (bring_to_front(dock_idx) is
// called after nearly every reorder elsewhere in this file) -- a
// z-order-based cycle would have the dock dominate/distort it. from=
// -1 starts from the first used slot (so Alt+Tab with nothing
// currently focused still does something sensible).
static int next_focusable(int from) {

	int start = (from < 0) ? 0 : (from + 1) % WM_MAX_WINDOWS;

	for (int i = 0; i < WM_MAX_WINDOWS; i++) {
		int idx = (start + i) % WM_MAX_WINDOWS;
		if (windows[idx].used) return idx;
	}

	return -1;

}

// Alt+Tab -- cycles focus to the next window (dock included, see
// next_focusable()'s own comment), bringing it to front exactly the
// way a mouse click on a window already does (see the click-handling
// block in main() below, which this mirrors).
static void alt_tab(void) {

	int next = next_focusable(focused);
	if (next < 0 || next == focused) return;

	int old_focused = focused;
	focused = next;

	// give the dock a sensible default selection the first time it's
	// ever reached this way, rather than requiring an arrow press
	// first just to see where you are -- see dock_selected's own
	// comment.
	if (focused == dock_idx && dock_selected < 0 && DOCK_APP_COUNT > 0)
		dock_selected = 0;

	bring_to_front(focused);
	// keep the dock frontmost regardless -- see its own comment where
	// this same call appears elsewhere in this file (handle_message(),
	// the mouse click handler). A no-op when focused IS the dock.
	if (dock_idx >= 0) bring_to_front(dock_idx);

	if (old_focused >= 0)
		repair_region(windows[old_focused].x, windows[old_focused].y,
			windows[old_focused].w, windows[old_focused].h, -1);
	repair_region(windows[focused].x, windows[focused].y,
		windows[focused].w, windows[focused].h, -1);

}

// pixels moved per Alt+Arrow press -- see alt_move_focused() below.
#define WM_KEY_MOVE_STEP   10

// Alt+Arrow -- moves the FOCUSED window (never the dock -- its
// position is fixed, see create_dock()) by WM_KEY_MOVE_STEP pixels in
// the given direction, clamped to the screen the same way a mouse
// drag already is. This is functionally an instant, single-step
// "drag" with no wireframe preview in between -- it reuses repair_
// drag()'s own sweep-region repair (see that function's own comment)
// by updating the window's position directly first and handing it the
// same before/after bounding-box bookkeeping (drag_min/max_x/y) a
// mouse drag release already produces, rather than duplicating that
// logic here.
static void alt_move_focused(uint32_t keysym) {

	if (focused < 0 || focused == dock_idx || !windows[focused].used) return;

	int dx = 0, dy = 0;
	switch (keysym) {
		case Z_KEY_LEFT:  dx = -WM_KEY_MOVE_STEP; break;
		case Z_KEY_RIGHT: dx =  WM_KEY_MOVE_STEP; break;
		case Z_KEY_UP:    dy = -WM_KEY_MOVE_STEP; break;
		case Z_KEY_DOWN:  dy =  WM_KEY_MOVE_STEP; break;
		default: return;
	}

	wm_window_t *w = &windows[focused];

	int32_t nx = (int32_t)w->x + dx;
	int32_t ny = (int32_t)w->y + dy;
	if (nx < 0) nx = 0;
	if (ny < 0) ny = 0;
	if (nx + (int32_t)w->w > WM_SCREEN_W) nx = WM_SCREEN_W - (int32_t)w->w;
	if (ny + (int32_t)w->h > WM_SCREEN_H) ny = WM_SCREEN_H - (int32_t)w->h;

	if ((uint32_t)nx == w->x && (uint32_t)ny == w->y) return;   // already at the edge

	int old_x = (int)w->x, old_y = (int)w->y;
	int ww = (int)w->w, wh = (int)w->h;

	w->x = (uint32_t)nx;
	w->y = (uint32_t)ny;

	drag_min_x = old_x < nx ? old_x : nx;
	drag_min_y = old_y < ny ? old_y : ny;
	drag_max_x = (old_x + ww > nx + ww) ? old_x + ww : nx + ww;
	drag_max_y = (old_y + wh > ny + wh) ? old_y + wh : ny + wh;

	notify_moved(focused);
	repair_drag(focused);

}

// -- keyboard --
//
// unlike the mouse above, keyboard capture is interrupt-driven, not
// polled -- see sw/os/hid.c. hid_read_key() pops one already-decoded
// press/release edge event at a time from the kernel's small event
// ring, drawn from BOTH usb ports (hid.c decides per-port; this side
// just gets a merged stream). wm drains all of them every main-loop
// iteration (there can be more than one queued since the last time we
// got scheduled).
//
// Global hotkeys (Alt+Tab, Alt+Arrow -- see alt_tab()/
// alt_move_focused() above) and dock navigation (dock_handle_key()
// above, while the dock has focus) are handled here, directly by wm,
// and consumed -- never forwarded to any app. Everything else goes to
// the *focused* window's owner only, translating the raw USB HID
// usage code to a keysym (zkbd.h) first. Demo windows (owned by wm
// itself, see main() below) have no app to notify, same as
// notify_moved()'s check below.
static void dispatch_keys(void) {

	int32_t ev;
	while ((ev = hid_read_key()) >= 0) {

		uint8_t usage     = (ev >> 1) & 0xFF;
		uint8_t modifiers = (ev >> 9) & 0xFF;
		bool    pressed   = (ev & 1) != 0;

		uint32_t keysym = z_kbd_usage_to_keysym(usage, modifiers);
		if (keysym == Z_KEY_NONE) continue;   // bare modifier change, or
		                                       // an unmapped usage code

		// -- global hotkeys -- act on press only; the matching
		// release is silently dropped (nothing to do with it, and it
		// must not fall through to being forwarded as a Tab/arrow
		// keystroke to whatever's focused).
		if ((modifiers & Z_KBD_MOD_ALT) && keysym == '\t') {
			if (pressed) alt_tab();
			continue;
		}
		if ((modifiers & Z_KBD_MOD_ALT) &&
			(keysym == Z_KEY_LEFT || keysym == Z_KEY_RIGHT ||
			 keysym == Z_KEY_UP   || keysym == Z_KEY_DOWN)) {
			if (pressed) alt_move_focused(keysym);
			continue;
		}

		// -- the dock, while focused, owns plain arrows/Enter for its
		// own icon navigation -- see dock_handle_key()'s own comment
		// on exactly which keys it consumes and why.
		if (focused == dock_idx && dock_handle_key(keysym, pressed)) continue;

		if (focused < 0) continue;
		if (windows[focused].owner_pid == my_pid) continue;

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
	uint32_t w, uint32_t h, int32_t fixed_x, int32_t fixed_y, uint32_t flags) {

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
		windows[i].flags = flags;

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

	// No dock at all if nothing resolved. Not just cosmetic: the width
	// below computes (DOCK_APP_COUNT - 1) * DOCK_ICON_GAP in unsigned
	// arithmetic, which underflows to an enormous width at zero. Can't
	// happen while `term` is a flash-resident core app, but the dock
	// contents are data now and this is one subtraction away from
	// being someone's very confusing afternoon.
	if (DOCK_APP_COUNT <= 0) {
		printf("wm: dock: no apps available, not creating dock\n");
		return -1;
	}

	uint32_t w = DOCK_PADDING * 2 + DOCK_APP_COUNT * DOCK_ICON_SIZE +
		(DOCK_APP_COUNT - 1) * DOCK_ICON_GAP;
	uint32_t h = DOCK_PADDING * 2 + DOCK_ICON_SIZE;

	int32_t x = DOCK_MARGIN;
	int32_t y = WM_SCREEN_H - (int32_t)h - DOCK_MARGIN;

	int idx = create_window(my_pid, "Dock", w, h, x, y, 0);
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

// true if (cx,cy) landed on window idx's close icon -- see
// close_icon_rect() (above, near draw_titlebar_content()) for the
// shared rect computation this and the actual draw both use. Checked
// BEFORE the general titlebar-drag hit test in main()'s click
// handling below, so clicking the icon closes the window instead of
// starting a drag.
static bool hit_close_icon(int idx, int cx, int cy) {

	if (windows[idx].no_titlebar) return false;
	if (!(windows[idx].flags & Z_WIN_FLAG_CLOSE_ICON)) return false;

	int ix, iy;
	close_icon_rect(&windows[idx], &ix, &iy);

	return cx >= ix && cx < ix + Z_ICON_W && cy >= iy && cy < iy + Z_ICON_H;

}

// handles a click already known to have landed on window idx's close
// icon (see hit_close_icon() above, and its own call site in main()'s
// click handling below) -- see Z_WIN_FLAG_CLOSE_KILLS_OWNER's own
// comment in zwm.h for the full reasoning behind the two behaviors.
static void handle_close_click(int idx) {

	uint32_t owner = windows[idx].owner_pid;
	uint32_t flags = windows[idx].flags;

	printf("wm: close icon clicked for window %d (owner=%ld, kills_owner=%d)\n",
		idx, (long)owner, (flags & Z_WIN_FLAG_CLOSE_KILLS_OWNER) ? 1 : 0);

	if (flags & Z_WIN_FLAG_CLOSE_KILLS_OWNER) {

		// destroy_window() repairs the screen region itself. kill the
		// owner AFTER that -- destroy_window() doesn't depend on the
		// owner still being alive to do its own bookkeeping (it never
		// waits on the owner for anything -- see repair_region()'s
		// own exclude_idx reasoning for the one case that does), so
		// ordering here doesn't matter for correctness, but killing
		// first would leave a brief window where a dead process still
		// has a window on screen for no reason.
		destroy_window(idx);

		// windows owned by wm itself (the dock, or the commented-out
		// demo windows in main()) would never actually reach here in
		// practice -- neither sets Z_WIN_FLAG_CLOSE_ICON -- but this
		// guard exists for the same reason dispatch_keys()/
		// notify_moved() already have one: wm killing ITSELF here
		// would be a self-inflicted, hard-to-debug way to go down.
		if (owner != my_pid) z_proc_kill(owner);

	} else {

		// let the owner decide -- see Z_WM_CLOSE's own comment
		// (zwm.h). fire-and-forget, same as every other wm->app
		// notification; the window stays open (and interactive)
		// until/unless the owner itself calls z_win_destroy() on it.
		if (owner != my_pid)
			z_msg_new_send(owner, Z_WM_CLOSE, 0, z_obj_uint32((uint32_t)idx));

	}

}

// handles a click already known to have landed inside the dock's own
// rect (see the dock_idx branch in main()'s click handling below) --
// maps the click to an icon slot, if any (clicks in the padding
// between/around icons land on no slot and are ignored), and delegates
// to dock_launch() above -- shared with the keyboard Enter path
// (dock_handle_key()), so both get identical launching/re-launch-
// prevention/visual-feedback behavior. Always spawns a fresh process
// for an icon that isn't currently mid-launch, same as running
// `run <app>` twice from the shell would -- no tracking of whether
// the app is "already running" once it HAS finished launching (see
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

	dock_launch(slot);

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

			// if this is a dock-launched app's FIRST window, the
			// launch is considered complete -- clear the "launching"
			// (inverted icon) state regardless of whether the window
			// creation below actually succeeds (a window-table-full
			// failure still means the PROCESS itself came up fine,
			// which is what "launching" is actually tracking -- see
			// dock_launching[]'s own comment). Independent of
			// anything else this case does, so it runs first.
			for (int di = 0; di < DOCK_APP_COUNT; di++) {
				if (dock_launching[di] && dock_launching_pid[di] == msg->from) {
					dock_launching[di] = false;
					if (dock_idx >= 0)
						repair_region(windows[dock_idx].x, windows[dock_idx].y,
							windows[dock_idx].w, windows[dock_idx].h, -1);
					break;
				}
			}

			char title[WM_TITLE_MAX] = "";
			uint32_t w = Z_WM_DEFAULT_WIDTH, h = Z_WM_DEFAULT_HEIGHT;

			z_obj_t *t = z_map_find(&msg->obj, "title");
			if (t && t->type == Z_STR && t->val.str)
				strncpy(title, t->val.str, WM_TITLE_MAX - 1);

			z_obj_t *wo = z_map_find(&msg->obj, "w");
			if (wo && wo->type == Z_UINT32) w = wo->val.uint32;

			z_obj_t *ho = z_map_find(&msg->obj, "h");
			if (ho && ho->type == Z_UINT32) h = ho->val.uint32;

			// optional exact placement (Zeitlos Scheme API's
			// (win-create ... x y), docs/scheme_api.md) -- both
			// present and well-typed, or fall back to the usual
			// auto-placed cascade exactly like every existing caller
			// that's never sent these (create_window()'s own
			// fixed_x/fixed_y contract: -1,-1 means cascade).
			int32_t fx = -1, fy = -1;
			z_obj_t *xo = z_map_find(&msg->obj, "x");
			z_obj_t *yo = z_map_find(&msg->obj, "y");
			if (xo && xo->type == Z_UINT32 && yo && yo->type == Z_UINT32) {
				fx = (int32_t)xo->val.uint32;
				fy = (int32_t)yo->val.uint32;
			}

			// optional Z_WIN_FLAG_* bitmask (zwm.h) -- see
			// z_win_create_flags() (zwin.c) for the app-facing
			// sender. missing key -> 0 (no close icon), same as
			// every caller that predates this feature.
			uint32_t flags = 0;
			z_obj_t *flo = z_map_find(&msg->obj, "flags");
			if (flo && flo->type == Z_UINT32) flags = flo->val.uint32;

			int idx = create_window(msg->from, title, w, h, fx, fy, flags);

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

	// window titlebar icons (close, and any future minimize/open/save
	// icon -- see win_icons.h) live in the same hardware glyph memory
	// as font data, just in a separate reserved region at the far end
	// of it (zicon.h's Z_ICON_MEM_OFFSET) -- loading them here, right
	// after the font, follows the exact same single-owner discipline
	// the comment above just finished explaining for z_font_5x8: wm
	// is the only process that ever writes to glyph memory, icons
	// included, so there's nothing else that needs its own load call
	// or any reason to fight over what's there.
	z_win_icons_load();

/*
	// demo windows so there's something to see/drag before a real
	// client app exists -- see the file header comment.
	int demo1 = create_window(my_pid, "Window 1", 140, 100, -1, -1, 0);
	int demo2 = create_window(my_pid, "Window 2", 140, 100, -1, -1, 0);
	if (demo1 >= 0)
		repair_region(windows[demo1].x, windows[demo1].y, windows[demo1].w, windows[demo1].h, -1);
	if (demo2 >= 0)
		repair_region(windows[demo2].x, windows[demo2].y, windows[demo2].w, windows[demo2].h, -1);
*/

	// dock -- created last so it starts out frontmost (see
	// create_dock()'s own comment); bring_to_front(dock_idx) calls
	// elsewhere in this file keep it that way as other windows come
	// and go.
	//
	// dock_build() first: create_dock() sizes the window from
	// DOCK_APP_COUNT, so the live set has to be known before the
	// window exists, not after.
	dock_build();
	dock_idx = create_dock();

	// Busy until net and repl register themselves. wm is up (it is
	// this process) but the services term depends on are started by
	// init() alongside it and take a moment to appear.
	wm_busy_set(WM_BUSY_STARTUP);
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

		// -- dock launch timeout -- see DOCK_LAUNCH_TIMEOUT_TICKS'
		// own comment below for why this exists at all. Compared as a
		// subtraction so it stays correct across the 32-bit tick wrap.
		for (int di = 0; di < DOCK_APP_COUNT; di++) {
			if (!dock_launching[di]) continue;
			if ((int32_t)(z_uptime_ticks() - dock_launching_deadline[di]) < 0)
				continue;
			printf("wm: dock: gave up waiting for '%s' (pid %ld) to create a window\n",
				dock_apps[di]->name, (long)dock_launching_pid[di]);
			dock_launching[di] = false;
			if (dock_idx >= 0)
				repair_region(windows[dock_idx].x, windows[dock_idx].y,
					windows[dock_idx].w, windows[dock_idx].h, -1);
		}

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

			} else if (hit >= 0 && hit_close_icon(hit, cx, cy)) {

				// close icon click: checked BEFORE the general
				// focus/drag handling below, so it never also starts
				// a drag or reorders anything -- see
				// handle_close_click()'s own comment for what happens
				// next (which itself may destroy this window, so
				// nothing below this branch may assume windows[hit]
				// is still valid).
				handle_close_click(hit);

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

		// clears WM_BUSY_STARTUP once net/repl are registered; repaint
		// the dock on the transition so it stops looking disabled
		if (check_core_services() && dock_idx >= 0)
			repair_region(windows[dock_idx].x, windows[dock_idx].y,
				windows[dock_idx].w, windows[dock_idx].h, -1);

		for (volatile int i = 0; i < 2000; i++); // light throttle

	}

}
