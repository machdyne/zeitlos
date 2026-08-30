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
	// smallest this window may be resized to (see the resize block
	// in main() below). Set once at creation from the global
	// Z_WM_MIN_WIDTH/HEIGHT floor, or from the window's own created
	// size if it asked for Z_WIN_FLAG_MIN_IS_CREATE (zwm.h). Stored
	// per-window rather than recomputed on demand because the
	// creation size it may be derived from is not retained anywhere
	// else -- w/h change as soon as the user resizes.
	uint32_t	min_w, min_h;
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
	{ "term",		z_icon_term_data  },
	{ "files",		z_icon_files_data },
	{ "text",		z_icon_text_data  },
	{ "read",		z_icon_read_data  },
	{ "draw",		z_icon_draw_data  },
	{ "info",		z_icon_info_data  },
	{ "calc",		z_icon_calc_data  },
	{ "clock",		z_icon_clock_data },
	{ "track",		z_icon_track_data },
	{ "space3d",	z_icon_space3d_data },
	{ "gpu3d",		z_icon_gpu3d_data },
	{ "settings",	z_icon_settings_data },
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

// -- pending launch argument -- see Z_WM_SET_ARG in zwm.h --
//
// One slot, because only one app is ever mid-launch at a time. The
// bytes are deliberately NOT cleared when the argument is claimed,
// only the valid flag is: the reply is a Z_STR pointing straight at
// this buffer, and the payload is borrowed until the recipient reads
// it (docs/messaging.md). Zeroing it here would hand the claiming app
// an empty string.
static char pending_arg[Z_WM_ARG_MAX];
static bool pending_arg_valid;
static uint32_t pending_arg_tick;
// what an app gets when there is nothing pending. A real, static,
// NUL-terminated string rather than NULL, so the reply is always a
// well-formed Z_STR the receiver can read without a special case.
static char arg_empty[1];

// -- clipboard -- see Z_WM_CLIP_SET in zwm.h --
//
// One buffer for the whole system. Always NUL-terminated, so the
// reply is a well-formed Z_STR whether or not anything has been
// copied yet.
//
// Not cleared on read: a clipboard you can only paste from once is
// not a clipboard.
static char clipboard[Z_WM_CLIP_MAX];

static uint8_t zorder[WM_MAX_WINDOWS];	// back-to-front; zorder[count-1] is frontmost
static uint8_t zorder_count = 0;
static int focused = -1;		// index into windows[], or -1
static int dragging = -1;		// index into windows[], or -1
static int drag_off_x = 0, drag_off_y = 0;

// -- resize state -- deliberately kept separate from the drag state
// above rather than folded into one "gesture" struct: the two can
// never be active at once (both start from a button press on
// different parts of the same window), but they repair the screen
// very differently at release -- a drag knows its window's size never
// changed and can repair only the wake it left behind, while a resize
// changes the content area itself and has to repair the whole union.
// Sharing state between them would invite exactly the kind of "which
// mode am I in" bug that costs an afternoon.
static int resizing = -1;		// index into windows[], or -1
// offset from the cursor to the window's bottom-right corner at the
// moment the grip was grabbed, so the corner doesn't jump to the
// cursor on the first pixel of movement.
static int resize_off_x = 0, resize_off_y = 0;
// the candidate size right now. w/h track this live during the
// gesture (the frame drawn on screen IS the window's frame at the
// candidate size), so these two exist to detect a change from one
// pointer sample to the next rather than to hold a pending value.
static int resize_w = 0, resize_h = 0;
// largest extent reached during this resize, so the release-time
// repair covers everywhere the frame was drawn, not just the final
// rect -- the same reason the drag path tracks its swept bounding box.
static int resize_max_w = 0, resize_max_h = 0;
// the window's size when the gesture began. w/h now track the
// candidate size live, so this is the only remaining record of what
// to repair back over.
static int resize_orig_w = 0, resize_orig_h = 0;

// -- pointer delivery state -- see dispatch_mouse() below --
//
// which window currently owns the pointer, or -1. Set on a button
// press inside a window's content area and held until release, so a
// gesture that wanders outside the window it started in keeps being
// delivered there (see Z_WM_MOUSE's own comment in zwm.h for why that
// matters).
static int mouse_capture = -1;
// last Z_WM_MOUSE payload actually sent, and who it went to -- used
// purely to coalesce: a stationary mouse should cost zero messages,
// and without this wm would send one per main-loop iteration forever.
static uint32_t mouse_last_packed;
static int mouse_last_target = -1;
static bool mouse_last_valid = false;
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
// forward-declared for the same reason as the three above: Alt+Tab
// (next_focusable(), further down) has to skip windows blocked by
// their owner's modal dialog, but the modality helpers live with the
// hit-testing code they otherwise belong next to.
static int blocked_by_modal(int idx);

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

	// Under Z_RASTER_XOR every pixel of this frame must be drawn
	// EXACTLY ONCE, or drawing the frame a second time will not undo
	// it. XOR is its own inverse per pixel, not per shape: a pixel
	// touched twice in one pass is back where it started, so it goes
	// missing from the visible outline, and a pixel touched an odd
	// number of times survives the erase as a leftover speck.
	//
	// z_fb_hw_box() (zgfx.c) handles its own four corners. What is
	// left is the two pieces drawn ON TOP of that box below -- the
	// titlebar separator, whose ends land on the left and right
	// edges, and the resize grip's ticks, whose ends land on the
	// bottom and right edges. Both get pulled in by one pixel for
	// XOR only, so the solid rendering is byte-for-byte what it
	// always was.
	int xin = (color == Z_RASTER_XOR) ? 1 : 0;

	z_fb_hw_box(x0, y0, x1, y1, color, NULL);

	if (!w->no_titlebar) {
		int ty = w->y + Z_WM_TITLEBAR_H;
		z_fb_hw_line(x0 + xin, ty, x1 - xin, ty, color, NULL);	// titlebar separator
	}

	// resize grip -- a few diagonal ticks in the lower-right corner,
	// marking the area hit_resize_grip() below actually tests. Drawn
	// here, as part of the box, rather than in
	// draw_titlebar_content(): it's genuine border, not content, so
	// it should move with the wireframe on every step of a drag
	// (which is exactly what being in this function gets it) instead
	// of vanishing until release the way the title text does.
	//
	// Only for windows that can actually be resized -- an ornamental
	// grip on a fixed-size window is worse than no grip at all, since
	// the one thing a grip promises is that dragging it does
	// something.
	if ((w->flags & Z_WIN_FLAG_RESIZABLE) && !w->no_titlebar) {
		for (int i = 3; i < Z_WM_RESIZE_GRIP; i += 3) {
			int gx = x1 - i, gy = y1 - i;
			// skip any tick that would need a negative coordinate --
			// the rasterizer's registers are unsigned and would wrap
			// it to a huge value rather than clipping, the same
			// hazard the focus ring below already clamps for.
			if (gx < x0 || gy < y0) continue;
			z_fb_hw_line(gx + xin, y1 - xin, x1 - xin, gy + xin, color, NULL);
		}
	}

	// The focused window's highlight ring is deliberately NOT drawn as
	// part of an XOR rubber band, and this is not cosmetic.
	//
	// The ring sits 1px outside the frame and is clamped to the
	// screen, so a window flush against x=0 or y=0 gets a ring edge
	// clamped ONTO the window's own frame edge. Two shapes, same
	// pixels: under set/clear that is invisible (both idempotent),
	// under XOR the second draw cancels the first and that whole edge
	// of the outline disappears -- then reappears as leftover specks
	// when the band is XOR-ed off again. Verified against every
	// window size at every screen corner; dragging a window into the
	// top-left corner is exactly how you would find it.
	//
	// Nothing is lost by omitting it: during a drag the band is
	// unambiguously the window being moved, and repair_region()
	// redraws the real focused frame, ring and all, on release.
	if (is_focused && color != Z_RASTER_XOR) {
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
// horizontal gap between two ADJACENT titlebar icons -- distinct from
// Z_WM_CLOSE_ICON_GAP below, which is the (larger) gap kept between
// the title text and the first icon. Two icons want to read as a
// related group; the title wants to read as separate from them.
#define Z_WM_TITLEBAR_ICON_GAP     2
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

// -- titlebar icon strip --
//
// A window can now show several titlebar icons, not just close (see
// Z_WIN_FLAG_NEW_ICON and friends in zwm.h). They're laid out
// right-to-left from the right edge, in the fixed order below, so:
//
//   - close, when present, sits at exactly the x it always did. That
//     is not a coincidence to be preserved by accident -- it's why
//     the order is anchored at the right rather than the left. Every
//     app that predates this feature draws identically.
//   - an app that asks for new+save+close reads "new save close" left
//     to right, which is the order those words are normally written
//     in, without the app having to specify any order at all.
//
// One table, walked by both the drawing and the hit testing, for the
// same "compute once, share everywhere" reason close_icon_rect() gave
// when close was the only icon there was -- two copies of this layout
// disagreeing by a pixel is a button that looks right and doesn't
// click, which is a genuinely annoying bug to chase.
typedef struct {
	uint32_t	flag;	// Z_WIN_FLAG_*_ICON
	uint8_t		icon;	// z_icon_id_t
	uint8_t		kind;	// Z_WM_TBICON_*, or 0 for close (see below)
} titlebar_icon_t;

// Right-to-left. close is first, hence rightmost.
//
// The rest are ordered so that the common combination reads
// left-to-right as new / open / save / close, which is the order a
// File menu would list them in and the order they are normally
// spoken. An app asking for a subset just gets that subset in the
// same relative order, without specifying anything.
//
// kind 0 means "this is the close icon" -- it keeps its own
// Z_WM_CLOSE message and its own kill-the-owner behavior rather than
// becoming a fifth Z_WM_TBICON_* kind, so nothing that already
// handles Z_WM_CLOSE has to change. See Z_WM_TITLEBAR_ICON in zwm.h.
static const titlebar_icon_t titlebar_icon_table[] = {
	{ Z_WIN_FLAG_CLOSE_ICON, Z_ICON_CLOSE, 0                 },
	{ Z_WIN_FLAG_SAVE_ICON,  Z_ICON_SAVE,  Z_WM_TBICON_SAVE  },
	{ Z_WIN_FLAG_OPEN_ICON,  Z_ICON_OPEN,  Z_WM_TBICON_OPEN  },
	{ Z_WIN_FLAG_NEW_ICON,   Z_ICON_NEW,   Z_WM_TBICON_NEW   },
	{ Z_WIN_FLAG_FONT_ICON,  Z_ICON_FONT,  Z_WM_TBICON_FONT  },
};
#define TITLEBAR_ICON_TABLE_COUNT \
	(int)(sizeof(titlebar_icon_table) / sizeof(titlebar_icon_table[0]))

// one placed icon
typedef struct {
	int		x, y;
	uint8_t	icon;
	uint8_t	kind;
} titlebar_icon_slot_t;

// Fills `out` with this window's icons, in right-to-left order, and
// returns how many were placed. `*leftmost_x` gets the x of the
// left-most icon placed, or the window's own right edge if none were
// -- that's where the title text has to stop.
//
// Icons that don't fit are dropped rather than drawn over the title
// or off the left edge of the window. A narrow window therefore keeps
// its rightmost icons (close first), which is the right priority: if
// only one button fits, it should be the one that gets you out.
static int titlebar_icons(const wm_window_t *w, titlebar_icon_slot_t *out,
	int *leftmost_x) {

	int x1 = (int)(w->x + w->w - 1);
	int x = x1 - Z_WM_CLOSE_ICON_MARGIN_X - Z_ICON_W + 1;
	int y = titlebar_content_y(w, Z_ICON_H);

	// Never let icons run past where the title text starts. Without
	// this a 64px-wide window (Z_WM_MIN_WIDTH) asking for four icons
	// would place some of them off its own left edge, on top of
	// whatever window is behind it.
	int floor_x = (int)w->x + Z_WM_TITLE_TEXT_MARGIN_X;

	int n = 0;

	for (int i = 0; i < TITLEBAR_ICON_TABLE_COUNT; i++) {

		if (!(w->flags & titlebar_icon_table[i].flag)) continue;
		if (x < floor_x) break;

		out[n].x = x;
		out[n].y = y;
		out[n].icon = titlebar_icon_table[i].icon;
		out[n].kind = titlebar_icon_table[i].kind;
		n++;

		x -= Z_ICON_W + Z_WM_TITLEBAR_ICON_GAP;

	}

	// The title stops short of the leftmost icon actually placed. n-1
	// rather than "wherever x ended up": x has already been stepped
	// past the last placed icon, and using it would leave a gap the
	// width of an icon that no longer exists.
	*leftmost_x = n ? out[n - 1].x : (x1 + 1);

	return n;

}

static void draw_titlebar_content(wm_window_t *w) {

	if (w->no_titlebar) return;	// nothing to draw -- see the dock

	int x0 = (int)w->x, y0 = (int)w->y;

	titlebar_icon_slot_t icons[TITLEBAR_ICON_TABLE_COUNT];
	int leftmost_x;
	int n = titlebar_icons(w, icons, &leftmost_x);

	// clip title text to the titlebar strip, and stop it short of the
	// leftmost icon (if any) instead of letting a long title run
	// underneath it. z_fb_draw_text()'s own per-glyph clip (zgfx.c)
	// keeps this pixel-exact for whichever glyph straddles the clip
	// boundary -- the same partial-glyph-falls-back-to-software
	// mechanism every other clipped hardware glyph draw in this
	// codebase already relies on, not something new introduced here.
	z_clip_t clip;
	clip.x0 = x0 + Z_WM_TITLE_TEXT_MARGIN_X;
	clip.y0 = y0;
	clip.x1 = leftmost_x - Z_WM_CLOSE_ICON_GAP - 1;
	clip.y1 = y0 + Z_WM_TITLEBAR_H - 1;

	if (w->title[0] && clip.x1 >= clip.x0)
		z_fb_draw_text(x0 + Z_WM_TITLE_TEXT_MARGIN_X,
			titlebar_content_y(w, z_font_5x8.h),
			w->title, 1, &z_font_5x8, &clip);

	for (int i = 0; i < n; i++)
		// clip=NULL: same as draw_window_box()'s own chrome draws --
		// wm already computed this rect from the window's own bounds,
		// so it's known on-screen and within the titlebar, nothing
		// left to clip against.
		z_fb_draw_icon(icons[i].x, icons[i].y, icons[i].icon, 1, 0, NULL);

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
// Does window `idx` completely cover the rectangle (rx,ry,rw,rh)?
//
// The point is not which windows overlap each other -- it is whether
// anything underneath will still be VISIBLE once `idx` has been
// drawn. If the repaired region lies entirely within `idx`, nothing
// below it shows through, and asking those owners to redraw is work
// whose result is painted over the instant it arrives.
//
// The +1 ring is the focus highlight, which draw_window_box() draws
// just outside a window's own bounds.
static bool window_covers_region(int idx, int rx, int ry, int rw, int rh) {

	if (idx < 0 || !windows[idx].used) return false;

	wm_window_t *w = &windows[idx];

	return rx >= (int)w->x - 1 &&
		ry >= (int)w->y - 1 &&
		rx + rw <= (int)w->x + (int)w->w + 1 &&
		ry + rh <= (int)w->y + (int)w->h + 1;

}

// Paints the parts of a window's interior that are CHROME, not
// content: the titlebar's background, and the 1px margin ring between
// the frame and the content area.
//
// Nobody owned these pixels before. draw_window_box() draws a 1px
// outline and the titlebar separator; the app owns everything from
// z_win_content_rect() inwards, which zwin.c insets by TWO pixels (one
// to clear the border, one as a deliberate blank margin so glyphs
// don't sit against the frame). The ring at inset 1, and the whole
// titlebar interior, were drawn by no one -- so after
// repair_region()'s back-to-front pass they still held whatever the
// window BEHIND had put there. The window looked like it had a
// transparent 1px gap inside its frame and a transparent titlebar.
//
// Only visible when something is actually behind: over the bare
// desktop the leftover pixels are the region fill's own 0 and look
// correct. Overlapping windows, and a dock that can now be covered,
// are what made it obvious.
//
// Called immediately BEFORE draw_window_box(), so the frame and
// separator are drawn on top of this, and before the owner is asked to
// repaint -- content lands on top in turn. It never touches the
// content rect itself, so it is safe even for the windows
// repair_region() skips notifying (exclude_idx, or one hidden behind
// the excluded window): their content is not disturbed.
static void draw_window_chrome_bg(wm_window_t *w) {

	int x = (int)w->x, y = (int)w->y;
	int cw = (int)w->w, ch = (int)w->h;

	// Everything above the content area: for a normal window that is
	// the titlebar interior, the separator row (redrawn by
	// draw_window_box() straight after) and the top margin row. A
	// no-titlebar window has just the margin row.
	int top_h = w->no_titlebar ? 1 : (Z_WM_TITLEBAR_H + 1);

	fill_rect(x + 1, y + 1, cw - 2, top_h, 0);

	// Bottom margin row, then the left and right margin columns
	// between them. fill_rect() clamps, so a window too short for
	// these to exist simply draws nothing.
	fill_rect(x + 1, y + ch - 2, cw - 2, 1, 0);

	int side_y = y + top_h + 1;
	int side_h = ch - top_h - 3;

	fill_rect(x + 1, side_y, 1, side_h, 0);
	fill_rect(x + cw - 2, side_y, 1, side_h, 0);

}

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

	// Will the excluded window cover this entire region on its own?
	//
	// If so, no owner underneath needs to redraw: only the region was
	// cleared, everything outside it is untouched, and the excluded
	// window's chrome will fill the region completely. Their content
	// inside it would be hidden the moment it was drawn.
	//
	// This is the common case, not a corner one. A window being
	// created or brought to the front is repaired using its OWN
	// rect, so the region always fits inside it -- which is exactly
	// when the notifications are pointless.
	//
	// Very visible before: opening a dialog made its parent repaint
	// its entire content just before the dialog covered that area,
	// which read as a flash on every Open. It also made wm block on
	// an ack for a redraw it did not need.
	bool region_hidden =
		window_covers_region(exclude_idx, rx, ry, rw, rh);

	// Where the excluded window sits in the z-order. Only windows
	// BEHIND it are hidden by it -- anything in front is drawn
	// afterwards and still needs its content back. zorder is
	// back-to-front, so "behind" means a lower index.
	int zex = -1;

	if (region_hidden)
		for (int i = 0; i < zorder_count; i++)
			if (zorder[i] == exclude_idx) { zex = i; break; }

	for (int i = 0; i < zorder_count; i++) {
		int idx = zorder[i];
		wm_window_t *w = &windows[idx];
		if (!rects_overlap(rx, ry, rw, rh,
			(int)w->x, (int)w->y, (int)w->w, (int)w->h))
			continue;
		draw_window_chrome_bg(w);
		draw_window_box(w, idx == focused, 1);
		draw_titlebar_content(w);
		if (idx == dock_idx) draw_dock();
		if (w->owner_pid == my_pid) continue;
		if (idx == exclude_idx) continue;

		if (region_hidden && zex >= 0 && i < zex) continue;

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
// matters), in fixed SLOT order rather than z-order. Slot order gives
// a stable, predictable cycle that does not shuffle under the user as
// windows raise each other -- which matters more now that the dock
// takes part in z-order like everything else and can be buried. Being
// in this cycle is what keeps the dock reachable when it is. from=-1
// starts from the first used slot (so Alt+Tab with nothing currently
// focused still does something sensible).
static int next_focusable(int from) {

	int start = (from < 0) ? 0 : (from + 1) % WM_MAX_WINDOWS;

	for (int i = 0; i < WM_MAX_WINDOWS; i++) {
		int idx = (start + i) % WM_MAX_WINDOWS;
		if (!windows[idx].used) continue;
		// A window blocked by its own owner's modal dialog is skipped
		// rather than focused (Z_WIN_FLAG_MODAL, zwm.h). Alt+Tabbing
		// onto it would hand it the keyboard, and since Z_WM_KEY
		// carries no window id its owner would have no way to tell
		// those keystrokes from the dialog's -- which is the exact
		// ambiguity modality exists to remove. The modal window
		// itself stays in the cycle, so the app is still reachable.
		if (blocked_by_modal(idx) >= 0) continue;
		return idx;
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

	// The dock is raised here only if it is what was focused. It used
	// to be forced to the front unconditionally after every reorder;
	// see create_dock()'s call site in main() for why that changed.
	bring_to_front(focused);

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

// -- game mode viewport (rtl/gpu/gpu_video.v, sw/common/zsoc.h) --
//
// Game mode shows a 320x240 window onto the same 640x480 framebuffer,
// pixel-doubled, so a 640x480 desktop becomes legible on a TV. wm owns
// the viewport for the same reason it owns every other global hotkey:
// it is the one process that sees every keystroke before any app does.
//
// NOTHING ELSE IN THIS FILE KNOWS GAME MODE EXISTS, and that is the
// point. WM_SCREEN_W/H stay 640x480, every window keeps its position,
// every clip rect and every repair region is unchanged, and the
// framebuffer is untouched. Switching modes is not a resolution
// change -- it moves a camera. So it cannot destroy a window or
// disturb an app, because there is nothing for it to destroy or
// disturb.
//
// The viewport origin is deliberately NOT reset when leaving game
// mode. Toggling out to see the whole desktop and back again should
// return you to where you were looking.
#define WM_VIEW_STEP 20

// x <= 320 and y <= 240 keep the whole viewport on the framebuffer.
// The hardware clamps to exactly this too (see gpu_video.v), so this
// is not the safety net -- it is here so the wm's own idea of the
// origin matches what is being displayed, since reading the register
// back returns what was written rather than what was clamped.
#define WM_VIEW_MAX_X (WM_SCREEN_W - Z_GAME_VIEW_W)
#define WM_VIEW_MAX_Y (WM_SCREEN_H - Z_GAME_VIEW_H)

static int32_t view_x = 0;
static int32_t view_y = 0;

static void view_apply(void) {
	if (view_x < 0) view_x = 0;
	if (view_y < 0) view_y = 0;
	if (view_x > WM_VIEW_MAX_X) view_x = WM_VIEW_MAX_X;
	if (view_y > WM_VIEW_MAX_Y) view_y = WM_VIEW_MAX_Y;
	z_game_set_view((uint32_t)view_x, (uint32_t)view_y);
}

// Alt+Esc -- toggle game mode, if this bitstream has it.
//
// Silently does nothing on a board without it rather than reporting an
// error: a key combination that is not bound is not a failure, and a
// dialog box would be a strange thing to get for pressing a key the
// machine does not implement. z_game_set_enabled() already answers
// false in that case, so the check is really just about not moving the
// viewport afterwards.
//
// Wrap is left OFF here. Toroidal scrolling is what a game wants; a
// desktop that wrapped from its right edge back to its left would be
// disorienting rather than useful. A game that wants it turns it on
// itself through z_game_set_enabled().
static void game_toggle(void) {

	if (!z_game_available()) return;

	bool on = !z_game_enabled();

	if (on) {
		// Centre the viewport on entry rather than starting at the
		// origin. The dock is bottom-left and most windows cascade
		// from the top-left, so a corner start would put the viewport
		// somewhere with nothing in it about as often as not; the
		// middle is the position from which the least scrolling is
		// needed to reach anything.
		view_x = (WM_SCREEN_W - Z_GAME_VIEW_W) / 2;
		view_y = (WM_SCREEN_H - Z_GAME_VIEW_H) / 2;
		z_game_set_enabled(true, false);
		view_apply();
	} else {
		z_game_set_enabled(false, false);
	}

}

// Ctrl+Alt+Arrow -- move the viewport, in game mode only.
//
// Ctrl+Alt rather than plain Alt because Alt+Arrow is already taken by
// alt_move_focused() above. dispatch_keys() must therefore test for
// this combination FIRST -- Ctrl+Alt+Left also satisfies the Alt+Left
// test, so checking in the other order would move the focused window
// and this would never fire at all.
static void game_move_view(uint32_t keysym) {

	if (!z_game_enabled()) return;

	switch (keysym) {
		case Z_KEY_LEFT:  view_x -= WM_VIEW_STEP; break;
		case Z_KEY_RIGHT: view_x += WM_VIEW_STEP; break;
		case Z_KEY_UP:    view_y -= WM_VIEW_STEP; break;
		case Z_KEY_DOWN:  view_y += WM_VIEW_STEP; break;
		default: return;
	}

	view_apply();

}

// Super (Windows key) held -- the viewport follows the mouse pointer,
// keeping it inside the visible area.
//
// Behind a modifier on purpose. A game in full-screen game mode may
// well use the mouse, and a viewport that chased the pointer on its
// own would fight it constantly. Super is the one modifier nothing
// else in this window manager binds, so it costs no existing shortcut.
//
// Called from the mouse polling path rather than the keyboard one --
// this is a level test on a held modifier, not an edge on a press, so
// it has to be asked every time the pointer moves rather than once
// when the key goes down.
//
// The margin keeps the pointer away from the very edge of the visible
// area: scrolling only once the pointer has actually left would mean
// it was never possible to see what you were about to move onto.
#define WM_VIEW_FOLLOW_MARGIN 40

static void game_follow_pointer(int mx, int my) {

	if (!z_game_enabled()) return;

	if (mx < view_x + WM_VIEW_FOLLOW_MARGIN)
		view_x = mx - WM_VIEW_FOLLOW_MARGIN;
	else if (mx > view_x + Z_GAME_VIEW_W - WM_VIEW_FOLLOW_MARGIN)
		view_x = mx - Z_GAME_VIEW_W + WM_VIEW_FOLLOW_MARGIN;

	if (my < view_y + WM_VIEW_FOLLOW_MARGIN)
		view_y = my - WM_VIEW_FOLLOW_MARGIN;
	else if (my > view_y + Z_GAME_VIEW_H - WM_VIEW_FOLLOW_MARGIN)
		view_y = my - Z_GAME_VIEW_H + WM_VIEW_FOLLOW_MARGIN;

	view_apply();

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

		// Alt+Esc -- toggle game mode. Consumed even on a bitstream
		// without game mode: Escape reaching the focused app only on
		// boards that happen to lack a feature would be a genuinely
		// confusing difference between machines.
		if ((modifiers & Z_KBD_MOD_ALT) && keysym == 0x1b) {
			if (pressed) game_toggle();
			continue;
		}

		// Ctrl+Alt+Arrow -- move the game mode viewport. MUST be
		// tested before the plain Alt+Arrow case directly below:
		// Ctrl+Alt+Left also satisfies that test, so the other order
		// would move the focused window and this would never fire at
		// all. See game_move_view()'s own comment.
		if ((modifiers & Z_KBD_MOD_ALT) && (modifiers & Z_KBD_MOD_CTRL) &&
			(keysym == Z_KEY_LEFT || keysym == Z_KEY_RIGHT ||
			 keysym == Z_KEY_UP   || keysym == Z_KEY_DOWN)) {
			if (pressed) game_move_view(keysym);
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

		// minimum size for a later resize (see the resize block in
		// main()). Z_WIN_FLAG_MIN_IS_CREATE means "never smaller than
		// what I just asked for" -- for an app whose window contains
		// fixed-size furniture it can't shrink below, which the app
		// knows and wm can't work out for itself. See that flag's own
		// comment in zwm.h.
		//
		// The global floor is applied either way, including on top of
		// a requested minimum: a window that asked to be created at
		// 20x20 must still not be resizable down to 20x20, because
		// below Z_WM_MIN_HEIGHT the content area's height goes
		// NEGATIVE and underflows to an enormous unsigned value
		// downstream.
		if (flags & Z_WIN_FLAG_MIN_IS_CREATE) {
			windows[i].min_w = w;
			windows[i].min_h = h;
		} else {
			windows[i].min_w = Z_WM_MIN_WIDTH;
			windows[i].min_h = Z_WM_MIN_HEIGHT;
		}
		if (windows[i].min_w < Z_WM_MIN_WIDTH) windows[i].min_w = Z_WM_MIN_WIDTH;
		if (windows[i].min_h < Z_WM_MIN_HEIGHT) windows[i].min_h = Z_WM_MIN_HEIGHT;

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

	// Read BEFORE the slot is released below -- the focus handoff
	// further down needs both, and by then this slot is free and may
	// legitimately be reused by the next create_window() call.
	bool was_modal = (windows[id].flags & Z_WIN_FLAG_MODAL) != 0;
	uint32_t owner = windows[id].owner_pid;

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

	if (focused == (int)id) {

		focused = -1;

		// A modal window closing should hand the keyboard back to
		// the window it was blocking, not to nothing (Z_WIN_FLAG_
		// MODAL, zwm.h). Without this, dismissing a dialog leaves
		// its owner unfocused, so the app that just regained control
		// of itself receives no keys at all until the user clicks
		// it -- which reads as the app having hung.
		//
		// Frontmost surviving window of the same owner, so an app
		// with several windows gets back whichever one was on top.
		// Deliberately not "whatever was focused before the dialog
		// opened": that would need remembering, and could name a
		// window that no longer exists.
		if (was_modal) {
			for (int i = zorder_count - 1; i >= 0; i--) {
				int idx = zorder[i];
				if (!windows[idx].used) continue;
				if (windows[idx].owner_pid != owner) continue;
				if (dock_idx >= 0 && idx == dock_idx) continue;
				focused = idx;
				break;
			}
		}

	}
	if (dragging == (int)id) dragging = -1;
	// same reasoning as `dragging` above: leaving either of these
	// pointing at a destroyed slot means the next mouse event gets
	// delivered to, or the next resize step redraws the frame of,
	// whatever unrelated window later reuses this index.
	if (resizing == (int)id) resizing = -1;
	if (mouse_capture == (int)id) mouse_capture = -1;
	if (mouse_last_target == (int)id) mouse_last_valid = false;

	printf("wm: destroyed window %ld\n", (long)id);

	// Drain both graphics engines BEFORE repairing.
	//
	// z_fb_hw_line() returns with the line still queued in the
	// rasterizer's FIFO (see z_fb_hw_sync() in zgfx.h). An app that
	// was drawing right up to the moment its window went away still
	// has lines in that queue, and they drain after this function
	// runs -- painting over the repair below and leaving a scribble
	// where the window used to be. The queue outlives its submitter;
	// the repair has to wait for it.
	z_fb_hw_sync();

	// window is already removed from windows[]/zorder above, so this
	// only redraws/notifies whatever else was overlapping its old spot
	repair_region(ox, oy, ow, oh, -1);

}

// -- modality (Z_WIN_FLAG_MODAL, zwm.h) --
//
// The frontmost modal window owned by `pid`, or -1 if that process
// has none. Scanned rather than cached: a process can create and
// destroy dialogs freely, WM_MAX_WINDOWS is 16, and this runs once
// per click, not per pointer sample. A cache here would be a second
// source of truth to keep correct across create/destroy/kill for no
// measurable gain.
//
// Frontmost wins if there are somehow several -- see the flag's own
// comment in zwm.h on why that's a defined outcome rather than a
// supported arrangement.
static int modal_for_owner(uint32_t pid) {

	for (int i = zorder_count - 1; i >= 0; i--) {
		int idx = zorder[i];
		if (!windows[idx].used) continue;
		if (windows[idx].owner_pid != pid) continue;
		if (windows[idx].flags & Z_WIN_FLAG_MODAL) return idx;
	}

	return -1;

}

// If `idx` is blocked by one of its own owner's modal windows,
// returns that modal window's index; otherwise -1.
//
// Deliberately scoped to the same owner: a modal dialog in one app
// must not stop you clicking on a different app, or on the dock. See
// Z_WIN_FLAG_MODAL in zwm.h.
static int blocked_by_modal(int idx) {

	if (idx < 0) return -1;
	if (windows[idx].flags & Z_WIN_FLAG_MODAL) return -1;	// it IS the modal
	if (dock_idx >= 0 && idx == dock_idx) return -1;

	int m = modal_for_owner(windows[idx].owner_pid);
	return (m == idx) ? -1 : m;

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

// true if (cx,cy) landed in window idx's lower-right resize grip --
// the square marked by the diagonal ticks draw_window_box() draws.
// Checked BEFORE the general titlebar/content handling in main()'s
// click dispatch, so grabbing the corner starts a resize rather than
// being delivered to the app as an ordinary click.
//
// Returns false for a window that didn't ask to be resizable, which
// is what keeps this entirely invisible to every app that predates
// the flag: without Z_WIN_FLAG_RESIZABLE the corner is just frame,
// exactly as it always was.
static bool hit_resize_grip(int idx, int cx, int cy) {

	wm_window_t *w = &windows[idx];

	if (w->no_titlebar) return false;
	if (!(w->flags & Z_WIN_FLAG_RESIZABLE)) return false;

	int x1 = (int)(w->x + w->w - 1);
	int y1 = (int)(w->y + w->h - 1);

	return cx > x1 - Z_WM_RESIZE_GRIP && cx <= x1 &&
		cy > y1 - Z_WM_RESIZE_GRIP && cy <= y1;

}

// Window idx's CONTENT area -- the region an app can actually draw
// into, below the titlebar and inset from the frame -- in absolute
// screen coordinates, inclusive bounds.
//
// This duplicates the inset arithmetic in zwin.c's
// z_win_content_rect(), which is unfortunate and worth stating
// plainly: the two live in different processes and there is no shared
// header carrying the formula, so they are kept in sync by hand. That
// exact duplication has already caused one real bug in this codebase
// (see z_win_content_rect()'s own comment). Kept to ONE copy on this
// side at least -- point_in_content() below goes through here rather
// than writing it out again.
static void window_content_rect(int idx, int *x0, int *y0, int *x1, int *y1) {

	wm_window_t *w = &windows[idx];

	int top = w->no_titlebar ? 2 : (Z_WM_TITLEBAR_H + 2);

	*x0 = (int)w->x + 2;
	*y0 = (int)w->y + top;
	*x1 = (int)(w->x + w->w) - 3;
	*y1 = (int)(w->y + w->h) - 3;

}

// true if (cx,cy) is inside window idx's CONTENT area
static bool point_in_content(int idx, int cx, int cy) {

	int x0, y0, x1, y1;
	window_content_rect(idx, &x0, &y0, &x1, &y1);

	return cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1;

}

// Which of window idx's titlebar icons (cx,cy) landed on:
//
//   -1  no icon here
//    0  the close icon
//   >0  a Z_WM_TBICON_* kind
//
// Uses titlebar_icons() (above, next to draw_titlebar_content()) for
// the placement, so what you can click and what you can see are the
// same rects by construction rather than by two functions agreeing.
// Checked BEFORE the general titlebar-drag hit test in main()'s click
// handling below, so clicking an icon activates it instead of
// starting a drag.
static int hit_titlebar_icon(int idx, int cx, int cy) {

	if (windows[idx].no_titlebar) return -1;

	titlebar_icon_slot_t icons[TITLEBAR_ICON_TABLE_COUNT];
	int leftmost_x;
	int n = titlebar_icons(&windows[idx], icons, &leftmost_x);

	for (int i = 0; i < n; i++)
		if (cx >= icons[i].x && cx < icons[i].x + Z_ICON_W &&
			cy >= icons[i].y && cy < icons[i].y + Z_ICON_H)
			return (int)icons[i].kind;

	return -1;

}

// Sends a Z_WM_TITLEBAR_ICON for one of the non-close icons. Never
// called for close -- that goes through handle_close_click() below,
// which has to decide between notifying and killing.
static void handle_titlebar_icon_click(int idx, int kind) {

	uint32_t owner = windows[idx].owner_pid;

	printf("wm: titlebar icon %d clicked for window %d (owner=%ld)\n",
		kind, idx, (long)owner);

	// same self-inflicted-damage guard every other notify path here
	// has -- wm messaging itself would be a confusing way to wedge.
	if (owner == my_pid) return;

	z_msg_new_send(owner, Z_WM_TITLEBAR_ICON, 0,
		z_obj_uint32(Z_WM_PACK_TBICON(idx, kind)));

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

		// Kill the owner FIRST, then destroy the window.
		//
		// This used to be the other way round, on the reasoning that
		// killing first leaves a brief moment where a dead process
		// still has a window on screen. That is true and it is
		// cosmetic; the ordering it produced was not. destroy_window()
		// repairs the region, and with the owner still alive it can
		// draw into that region between the repair and the kill --
		// so a window closed while its app was drawing left the
		// app's last strokes on the desktop behind it.
		//
		// Killing first closes that window entirely, and
		// destroy_window() does not depend on the owner being alive
		// for any of its own bookkeeping (it never waits on the owner
		// -- see repair_region()'s exclude_idx reasoning for the one
		// case that does). It also drains the GPU queues itself, for
		// the drawing that was already submitted before the kill.
		//
		// windows owned by wm itself would never reach here in
		// practice -- neither sets Z_WIN_FLAG_CLOSE_ICON -- but the
		// guard stays for the same reason it always did.
		if (owner != my_pid) z_proc_kill(owner);

		destroy_window(idx);

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

// -- window-rect payloads --
//
// send_win_rect() used to build its Z_MAP with z_obj_map()/
// z_map_set(), which malloc()s: one table struct, two arrays, and a
// copied string for each of the five keys. It then deliberately never
// freed any of it, because the payload is BORROWED until the
// recipient reads it (docs/messaging.md) and wm doesn't wait for that.
//
// That was measured at 384 bytes per call. wm's whole stack-and-heap
// allowance is 8KB (Z_PROC_STACK_SIZE_SMALL, sw/os/kernel.h), and
// notify_moved() calls this once per completed window move -- every
// drag release, every Alt+Arrow step. Around twenty moves exhausted
// the heap, at which point z_obj_map()'s unchecked malloc() wrote
// through a NULL pointer and the machine went down. That is the
// "moving a window around eventually crashes" bug, and it applied
// equally to repeated resizing (Z_WM_WINDOW_RESIZED goes through here
// too).
//
// Static storage instead, so this allocates nothing at all. The
// kernel reads the payload out of the sender's own address space
// (z_translate(), sw/os/msg.c), and .bss is as reachable that way as
// the heap was -- it just doesn't run out.
//
// A RING of slots rather than one, because the borrowed-payload
// window is still real: a slot must not be overwritten between the
// send and the recipient's read. Four means four more of these
// messages would have to be sent, to anyone, before a given one is
// reused -- and wm sends them one at a time, on user-paced events,
// to apps that drain their queues every loop. This does not make the
// borrow race impossible, it makes it require something that doesn't
// happen; the unbounded leak it replaces was a certainty.
#define WIN_RECT_SLOTS   4
#define WIN_RECT_KEYS    5

// Plain literals, pointed at rather than copied -- .rodata translates
// exactly like .bss does.
static const char *const win_rect_key_names[WIN_RECT_KEYS] = {
	"id", "x", "y", "w", "h"
};

static z_obj_t win_rect_keys[WIN_RECT_SLOTS][WIN_RECT_KEYS];
static z_obj_t win_rect_vals[WIN_RECT_SLOTS][WIN_RECT_KEYS];
static z_obj_table_t win_rect_tbl[WIN_RECT_SLOTS];
static int win_rect_slot;

static void send_win_rect(uint32_t to, uint32_t subject, uint32_t tag, int idx) {

	int s = win_rect_slot;
	win_rect_slot = (win_rect_slot + 1) % WIN_RECT_SLOTS;

	z_obj_t *k = win_rect_keys[s];
	z_obj_t *v = win_rect_vals[s];

	// A failure reply carries nothing but the id, same as before --
	// the map is simply one entry long, and z_resolve_obj() walks
	// only `len` of it regardless of how the arrays are sized.
	int n = (idx >= 0) ? WIN_RECT_KEYS : 1;

	for (int i = 0; i < n; i++) {
		k[i].type = Z_STR;
		k[i].val.str = (char *)win_rect_key_names[i];
	}

	v[0].type = Z_INT32;
	v[0].val.int32 = idx;

	if (idx >= 0) {
		v[1].type = Z_UINT32; v[1].val.uint32 = windows[idx].x;
		v[2].type = Z_UINT32; v[2].val.uint32 = windows[idx].y;
		v[3].type = Z_UINT32; v[3].val.uint32 = windows[idx].w;
		v[4].type = Z_UINT32; v[4].val.uint32 = windows[idx].h;
	}

	win_rect_tbl[s].len = (uint32_t)n;
	win_rect_tbl[s].a = k;
	win_rect_tbl[s].b = v;

	z_obj_t msg;
	msg.type = Z_MAP;
	msg.val.ptr = &win_rect_tbl[s];

	z_msg_new_send(to, subject, tag, msg);

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

			// A newly created window is left in front, dock
			// included -- create_window() appends to the front of
			// zorder and that is now allowed to stand. The dock
			// used to be forced back to the front here; see
			// create_dock()'s call site in main().

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

			// A modal window is the exception to the
			// don't-steal-focus rule above, and has to be: it exists
			// precisely to take the keyboard away from its owner's
			// other windows until it's dismissed (Z_WIN_FLAG_MODAL,
			// zwm.h). A dialog you have to click before you can type
			// in it would be a worse version of no dialog at all.
			//
			// Note this steals focus from whatever had it, including
			// another process's window. That's the same thing any
			// click does, it's user-initiated (some app just opened
			// a dialog because the user asked it to), and the
			// alternative -- a modal window that isn't focused --
			// leaves the owner unable to receive keys for EITHER
			// window.
			if (idx >= 0 && (flags & Z_WIN_FLAG_MODAL) &&
				focused != idx) {

				int old_focused = focused;
				focused = idx;

				// Only the TITLEBAR strip, not the whole window.
				//
				// Losing focus changes exactly one thing about a
				// window: how wm draws its titlebar. The content is
				// unaffected, and wm draws the titlebar itself -- so
				// repairing the full rect asked the owner for a
				// complete content redraw it had no reason to do.
				//
				// That was visible. Opening a dialog produced two
				// full refreshes of the parent before the dialog
				// appeared: this one, and the create-time repair
				// below. The second is real work (the new window has
				// to be composited); this one was not.
				//
				// Same reasoning, and the same one-line fix, as
				// Z_WM_SET_TITLE's repair above. Note this is only
				// safe because the z-order is NOT changing here --
				// alt_tab() repairs in full for exactly that reason,
				// since bring_to_front() there may uncover content
				// that really does need redrawing.
				//
				// exclude_idx = idx, NOT -1. The old focused window
				// very often overlaps the window just created (a
				// dialog is centered on its parent, so it always
				// does), and without the exclusion this repair asks
				// the NEW window's owner to redraw it -- while that
				// owner is still inside z_win_create_cb() and has no
				// window id to recognize the request by. It can't
				// ack, so wm blocks here for the full
				// REDRAW_ACK_TIMEOUT on every single dialog that
				// opens. Same reasoning as the create-time repair
				// below; see repair_region()'s own exclude_idx
				// comment.
				if (old_focused >= 0 && windows[old_focused].used)
					repair_region(windows[old_focused].x,
						windows[old_focused].y, windows[old_focused].w,
						Z_WM_TITLEBAR_H, idx);

			}

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

		// A full-screen app is handing the framebuffer back. See
		// Z_WM_REPAINT's own comment in zwm.h for why this takes no
		// rectangle.
		//
		// repair_region() over the whole screen is exactly the right
		// primitive and already does everything needed: desktop
		// background, every window frame, and a Z_WM_REDRAW to each
		// owner so app content comes back too. exclude_idx -1 excludes
		// nothing.
		//
		// Also drops any mouse capture and re-asserts the cursor
		// shape. A game that exited while wm thought a drag was in
		// progress would otherwise leave the next click behaving as
		// the end of that drag.
		case Z_WM_REPAINT:
			mouse_capture = -1;
			repair_region(0, 0, WM_SCREEN_W, WM_SCREEN_H, -1);
			break;

		case Z_WM_CLIP_SET: {

			if (msg->obj.type != Z_STR || !msg->obj.val.str) break;

			// Copied out immediately. The payload is borrowed from
			// the sender until read (docs/messaging.md), and this IS
			// the read -- holding the pointer instead would leave the
			// clipboard pointing into another process's memory, which
			// that process is free to reuse or exit from.
			uint32_t i = 0;
			for (; i < Z_WM_CLIP_MAX - 1 && msg->obj.val.str[i]; i++)
				clipboard[i] = msg->obj.val.str[i];
			clipboard[i] = 0;

			printf("wm: clipboard set, %lu bytes%s\n", (unsigned long)i,
				msg->obj.val.str[i] ? " (truncated)" : "");

			break;

		}

		case Z_WM_CLIP_GET: {

			// Points straight at wm's own buffer -- no copy, and
			// nothing to free. Safe because the buffer is static and
			// only ever overwritten by another SET, which cannot
			// happen between this send and the recipient's read (the
			// recipient is blocked waiting for exactly this).
			z_obj_t reply;
			reply.type = Z_STR;
			reply.val.str = clipboard;

			z_msg_new_send(msg->from, Z_WM_CLIP_DATA, msg->tag, reply);

			break;

		}

		case Z_WM_SET_ARG:

			if (msg->obj.type != Z_STR || !msg->obj.val.str) break;

			strncpy(pending_arg, msg->obj.val.str, Z_WM_ARG_MAX - 1);
			pending_arg[Z_WM_ARG_MAX - 1] = 0;

			pending_arg_valid = true;
			pending_arg_tick = z_uptime_ticks();

			printf("wm: launch arg set: '%s'\n", pending_arg);

			break;

		case Z_WM_GET_ARG: {

			// Expired arguments read as absent -- see Z_WM_ARG_TIMEOUT
			// in zwm.h for why an unclaimed one must not linger.
			if (pending_arg_valid &&
				z_uptime_ticks() - pending_arg_tick > Z_WM_ARG_TIMEOUT) {
				printf("wm: launch arg expired unclaimed\n");
				pending_arg_valid = false;
			}

			// Built by hand rather than with z_obj_str(), which
			// mallocs a copy that nothing can free (the payload is
			// borrowed until the recipient reads it). Once per app
			// launch is bounded, but wm has 8KB for stack and heap
			// together and this is the same leak that took the
			// machine down after twenty window moves -- see
			// send_win_rect() and docs/messaging.md.
			z_obj_t reply;
			reply.type = Z_STR;
			reply.val.str = pending_arg_valid ? pending_arg : arg_empty;

			// Claimed exactly once. Only the flag is cleared, not the
			// bytes -- see pending_arg's own comment.
			pending_arg_valid = false;

			z_msg_new_send(msg->from, Z_WM_ARG, msg->tag, reply);

			break;

		}

		case Z_WM_SET_TITLE: {

			z_obj_t *id = z_map_find(&msg->obj, "id");
			z_obj_t *t = z_map_find(&msg->obj, "title");

			if (!id || id->type != Z_INT32) break;
			if (!t || t->type != Z_STR || !t->val.str) break;

			int idx = id->val.int32;
			if (idx < 0 || idx >= WM_MAX_WINDOWS || !windows[idx].used) break;

			// Only the owner may retitle its own window. Nothing else
			// in this protocol is authenticated either, but there is
			// no reason to add the first way for one app to relabel
			// another's window.
			if (windows[idx].owner_pid != msg->from) break;

			strncpy(windows[idx].title, t->val.str, WM_TITLE_MAX - 1);
			windows[idx].title[WM_TITLE_MAX - 1] = 0;

			// Repair only the titlebar strip, and EXCLUDE THE OWNER.
			//
			// Two separate narrowings, both needed. The region,
			// because repairing the whole window rect would ask
			// every overlapping owner for a full content redraw over
			// a change confined to one strip.
			//
			// And exclude_idx = idx, because wm draws the titlebar
			// itself -- a window retitling its OWN titlebar has
			// nothing to redraw. Without this, every retitle sent
			// the caller a redraw request it would service later,
			// from its main loop, after it had already repainted for
			// its own reasons. In sw/apps/read that was a visible
			// second render of the whole document on every file
			// open; in sw/apps/text it is a repaint on the first
			// keystroke after every save.
			//
			// Windows in FRONT of this one still get notified --
			// they overlap the strip and are drawn after it. Windows
			// behind are covered by the titlebar and are skipped by
			// repair_region()'s own occlusion check.
			repair_region((int)windows[idx].x, (int)windows[idx].y,
				(int)windows[idx].w, Z_WM_TITLEBAR_H, idx);

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

// tells the owner its window is now a different size. Same shape and
// same release-only timing as notify_moved() above -- and, critically,
// called BEFORE the repair that follows a resize, so the app has the
// new w/h in its queue ahead of the Z_WM_REDRAW asking it to redraw at
// that size. See Z_WM_WINDOW_RESIZED's own comment in zwm.h for why
// that ordering is load-bearing rather than incidental.
static void notify_resized(int idx) {

	if (windows[idx].owner_pid == my_pid) return;

	send_win_rect(windows[idx].owner_pid, Z_WM_WINDOW_RESIZED, 0, idx);

}

// -- pointer delivery --
//
// Decides which window, if any, should receive this pointer sample and
// sends it -- see Z_WM_MOUSE in zwm.h for the wire format and the
// coalescing contract this implements.
//
// Two rules, in order:
//
//   1. If a capture is active (a button went down inside some
//      window's content area and hasn't come up yet), that window gets
//      the event no matter where the cursor now is. This is what makes
//      a paint stroke, a slider drag, or a rubber-banded selection
//      survive the cursor leaving the window.
//   2. Otherwise the FOCUSED window gets the event, but only while the
//      cursor is actually over it and it is the frontmost window at
//      that point. Requiring the hit test to agree with `focused` is
//      what stops a window from receiving phantom events through
//      another window stacked on top of it.
//
// Nothing is delivered while wm is running its own drag or resize
// gesture: the pointer belongs to wm for the duration, and forwarding
// those samples would have apps reacting to a gesture aimed at their
// chrome.
static void dispatch_mouse(int cx, int cy, uint8_t btn) {

	if (dragging >= 0 || resizing >= 0) return;

	int target = mouse_capture;

	if (target < 0) {
		int hit = hit_test(cx, cy);
		if (hit >= 0 && hit == focused) target = hit;
	}

	if (target < 0) return;
	if (!windows[target].used) { mouse_capture = -1; return; }

	// wm-owned windows (the dock) have no separate process to notify;
	// wm handles their clicks inline. Same guard notify_moved() and
	// dispatch_keys() already carry.
	if (windows[target].owner_pid == my_pid) return;

	uint32_t packed = Z_WM_PACK_MOUSE(cx, cy, btn,
		point_in_content(target, cx, cy));

	// coalesce -- a mouse that hasn't moved and whose buttons haven't
	// changed produces no traffic at all. Without this, wm would send
	// one message per main-loop iteration to whatever window happened
	// to be under a resting cursor, which floods that app's queue and
	// (since nothing frees message payloads -- see docs/messaging.md)
	// steadily consumes wm's own heap for no reason.
	if (mouse_last_valid && target == mouse_last_target &&
		packed == mouse_last_packed) return;

	z_msg_new_send(windows[target].owner_pid, Z_WM_MOUSE, 0,
		z_obj_uint32(packed));

	mouse_last_packed = packed;
	mouse_last_target = target;
	mouse_last_valid = true;

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
// Blanks everything INSIDE a window's frame -- content area and
// titlebar interior alike -- leaving just the box and its titlebar
// separator.
//
// Called the moment a titlebar drag begins, before the window has
// moved a single pixel. Dragging is a wireframe operation: only the
// border follows the cursor, and the content stays frozen wherever it
// was drawn. Leaving it there means the window's insides visibly
// detach from its frame and sit in the middle of the screen until the
// drag ends, which reads as a rendering fault rather than as
// "content updates on release". Blanking first makes the gesture say
// what it actually does -- you are moving an empty frame, and it
// fills in when you let go.
//
// Redraws the box afterwards because the titlebar separator line lives
// inside the region just cleared and is part of the frame, not part of
// the content.
static void clear_window_interior(int idx) {

	wm_window_t *w = &windows[idx];

	// Three steps, and the order matters.
	//
	// The frame currently on screen was drawn SOLID, and a solid
	// shape cannot be undone by XOR-ing it -- XOR only reverses what
	// XOR drew. So the solid frame is cleared first, the interior is
	// blanked, and the rubber band is then XOR-drawn onto a known
	// blank rectangle, where it renders exactly as the solid frame
	// did. From here every step of the gesture is a matched pair of
	// XOR draws and nothing underneath is ever damaged again.
	//
	// The clear has to come from draw_window_box() rather than the
	// fill below, because the focused window's highlight ring is
	// drawn one pixel OUTSIDE the window's own rect and the fill
	// would not reach it. Left behind, it would then be inverted by
	// the XOR draw instead of erased.
	//
	// The fill covers the whole window rect, border included -- it
	// used to inset by one pixel to preserve the frame it was about
	// to redraw. Everything it touches is inside the drag's swept
	// bounding box and is repaired on release.
	draw_window_box(w, idx == focused, Z_RASTER_CLEAR);

	fill_rect((int)w->x, (int)w->y, (int)w->w, (int)w->h, 0);

	draw_window_box(w, idx == focused, Z_RASTER_XOR);

}

static void repair_drag(int dragged_idx) {

	(void)dragged_idx;	// the swept box below already covers it

	// One repair over everything the window swept through, INCLUDING
	// its own final footprint.
	//
	// This used to repair four strips around the final rect and
	// deliberately skip the rect itself, on the reasoning that the
	// border there was already correct and re-clearing it would
	// flash. That reasoning was incomplete in two ways, and both
	// showed up as visible corruption after a small move:
	//
	//   - Titlebar CONTENT is not part of draw_window_box(). The
	//     wireframe drag redraws the box at each step but never the
	//     title text or icons, so those stay at the position the drag
	//     started from. Move a window a few pixels and the old text
	//     and icons are still sitting inside the new footprint, which
	//     the strips by definition never touch. Alt+Arrow had it
	//     worse: it doesn't erase the old frame at all, so the old
	//     BORDER survived inside the new footprint too.
	//
	//   - Only the dragged window's own owner was asked to redraw.
	//     Any OTHER window overlapping the final footprint was left
	//     as it was.
	//
	// repair_region() already does all of this correctly -- clear,
	// then redraw chrome and titlebar content for every overlapping
	// window in z-order, asking each owner to repaint its content.
	// The strips and the footprint together are exactly the swept
	// bounding box, so this is one call where there were five, and it
	// sends each affected app one redraw instead of up to four.
	//
	// The flash the strips were avoiding is no longer a concern:
	// starting a drag now blanks the window's interior anyway (see
	// clear_window_interior(), called from the click handler), so
	// there is nothing left inside the footprint to preserve.
	repair_region(drag_min_x, drag_min_y,
		drag_max_x - drag_min_x, drag_max_y - drag_min_y, -1);

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
	// Both resident fonts, at the fixed offsets glyph_layout[]
	// (sw/common/zgfx.c) declares. wm is still the only process that
	// writes glyph memory; every other process reads the offsets out
	// of that same table and blits, which is what lets an app pick a
	// font without any of them having to agree at runtime.
	//
	// z_font_6x12 is what sw/apps/text's titlebar font toggle
	// switches to (Z_WIN_FLAG_FONT_ICON, zwm.h). Loading it here
	// rather than on demand keeps the single-owner rule intact --
	// the alternative is an app writing glyph memory mid-session,
	// underneath every other app currently drawing text from it.
	z_gfx_hw_font_load(&z_font_5x8);
	z_gfx_hw_font_load(&z_font_6x12);

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

	// dock -- created last so it STARTS OUT frontmost (see
	// create_dock()'s own comment). It does not stay that way.
	//
	// The dock used to be forced back to the front after every
	// reorder, everywhere. That made it impossible to put a window
	// over it: drop a window across the dock and the dock's own
	// content was painted afterwards, straight over the window's,
	// so the dock showed THROUGH the window sitting on top of it.
	// That is not a drawing bug -- back-to-front repainting was
	// working exactly as specified, the dock was genuinely in front.
	//
	// It now participates in z-order like any other window: clicking
	// it raises it, clicking a window raises that instead, and a
	// window can cover it.
	//
	// A fully covered dock is reachable by Alt+Tab, which cycles in
	// SLOT order and includes the dock (next_focusable()). If burying
	// it turns out to be a nuisance in practice, the middle ground is
	// raising it on click but not on window creation, rather than
	// going back to pinning it.
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

			} else if (hit >= 0 && blocked_by_modal(hit) >= 0) {

				// This window's owner has a modal window open, and
				// this isn't it (Z_WIN_FLAG_MODAL, zwm.h). Swallow
				// the click entirely -- no focus change, no raise, no
				// drag, no resize, no capture, nothing forwarded to
				// the app -- and put the modal window in front
				// instead, so a click aimed at the blocked window at
				// least SHOWS you what's blocking it rather than
				// appearing to do nothing at all.
				int m = blocked_by_modal(hit);

				bool focus_changed = (focused != m);
				int old_focused = focused;
				if (focus_changed) focused = m;

				// same real-reorder test as the ordinary click path
				// below. The snapshot/compare is now belt and braces
				// -- it existed because a following
				// bring_to_front(dock_idx) could undo what
				// bring_to_front(m) reported -- but it is still the
				// honest test of "did z-order actually change", so it
				// stays.
				uint8_t zbefore[WM_MAX_WINDOWS];
				uint8_t zcount_before = zorder_count;
				memcpy(zbefore, zorder, zorder_count);

				bring_to_front(m);

				bool reordered = (zcount_before != zorder_count) ||
					memcmp(zbefore, zorder, zorder_count) != 0;

				if (focus_changed && old_focused >= 0)
					repair_region(windows[old_focused].x, windows[old_focused].y,
						windows[old_focused].w, windows[old_focused].h, -1);

				if (focus_changed || reordered)
					repair_region(windows[m].x, windows[m].y,
						windows[m].w, windows[m].h, -1);

			} else if (hit >= 0 && hit_titlebar_icon(hit, cx, cy) == 0) {

				// close icon click: checked BEFORE the general
				// focus/drag handling below, so it never also starts
				// a drag or reorders anything -- see
				// handle_close_click()'s own comment for what happens
				// next (which itself may destroy this window, so
				// nothing below this branch may assume windows[hit]
				// is still valid).
				handle_close_click(hit);

			} else if (hit >= 0 && hit_titlebar_icon(hit, cx, cy) > 0) {

				// one of the other titlebar icons (new/save/open/
				// font) -- notify the owner and consume the click,
				// same as close does. Never starts a drag.
				handle_titlebar_icon_click(hit, hit_titlebar_icon(hit, cx, cy));

			} else if (hit >= 0) {

				bool focus_changed = (focused != hit);
				int old_focused = focused;
				if (focus_changed) focused = hit;

				// Whether the z-order ACTUALLY ended up different,
				// rather than whether bring_to_front() moved
				// something on the way.
				//
				// This used to be `bool reordered = bring_to_front(hit);`
				// -- which is wrong as soon as the dock gets pushed
				// back to the front immediately afterwards, because
				// for the window sitting directly below the dock the
				// two calls cancel out. That window is the common
				// case, not a corner one: it's whatever the user is
				// working in. So every click in an
				// already-frontmost, already-focused window claimed a
				// reorder and triggered a full repair_region() --
				// which redraws every overlapping window AND BLOCKS
				// on an ack from each. Harmless when apps had one
				// window each; with a dialog open it means two full
				// repaints and two ack round trips per click, which
				// is exactly as slow as it sounds.
				uint8_t zbefore[WM_MAX_WINDOWS];
				uint8_t zcount_before = zorder_count;
				memcpy(zbefore, zorder, zorder_count);

				bring_to_front(hit);

				// keep the dock frontmost -- see its own comment
				// where this same call appears in handle_message().

				bool reordered = (zcount_before != zorder_count) ||
					memcmp(zbefore, zorder, zorder_count) != 0;

				if (focus_changed && old_focused >= 0)
					repair_region(windows[old_focused].x, windows[old_focused].y,
						windows[old_focused].w, windows[old_focused].h, -1);

				if (focus_changed || reordered)
					repair_region(windows[hit].x, windows[hit].y,
						windows[hit].w, windows[hit].h, -1);

				// grip first: it sits inside the window's general
				// body, so a plain content-click test would swallow
				// it. hit_resize_grip() is false for any window that
				// didn't ask for Z_WIN_FLAG_RESIZABLE, so this branch
				// simply never fires for windows that predate the
				// flag.
				if (hit_resize_grip(hit, cx, cy)) {

					resizing = hit;
					// offset from the cursor to the corner, so the
					// corner doesn't snap to the cursor on the first
					// pixel of movement
					resize_off_x = cx - (int)(windows[hit].x + windows[hit].w - 1);
					resize_off_y = cy - (int)(windows[hit].y + windows[hit].h - 1);
					resize_w = (int)windows[hit].w;
					resize_h = (int)windows[hit].h;
					resize_max_w = resize_w;
					resize_max_h = resize_h;
					// The size the window started at. Needed at
					// release for the repair union, and no longer
					// recoverable from w/h -- those now follow the
					// candidate size live (see the resize-update
					// block below).
					resize_orig_w = resize_w;
					resize_orig_h = resize_h;
					// Blank the interior, exactly as starting a
					// titlebar drag does -- see
					// clear_window_interior(). Without it the app's
					// content sits at its old size inside a frame
					// that is changing shape around it.
					clear_window_interior(hit);

				} else if (hit_titlebar(hit, cy)) {
					dragging = hit;
					drag_off_x = cx - windows[hit].x;
					drag_off_y = cy - windows[hit].y;
					drag_min_x = windows[hit].x;
					drag_min_y = windows[hit].y;
					drag_max_x = windows[hit].x + windows[hit].w;
					drag_max_y = windows[hit].y + windows[hit].h;
					// Blank the interior now, before any movement --
					// see clear_window_interior(). Deliberately here,
					// on the press that STARTS a drag, rather than on
					// any titlebar click: a click that merely focuses
					// or raises a window has no reason to throw its
					// content away and make the owner redraw it.
					clear_window_interior(hit);
				} else if (point_in_content(hit, cx, cy)) {
					// press inside the app's own content area: hand
					// the pointer to that window until the button
					// comes up again. See dispatch_mouse() above.
					mouse_capture = hit;
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
				// XOR both ways. The first call removes the band
				// from its old position AND restores whatever it
				// was covering -- which is the whole point: with
				// the old clear-then-set pair, that first call
				// wrote black over every pixel the outline
				// crossed, gouging a trail through any window
				// underneath that survived until repair_drag().
				draw_window_box(&windows[dragging], dragging == focused, Z_RASTER_XOR);
				windows[dragging].x = nx;
				windows[dragging].y = ny;
				draw_window_box(&windows[dragging], dragging == focused, Z_RASTER_XOR);

				if (nx < drag_min_x) drag_min_x = nx;
				if (ny < drag_min_y) drag_min_y = ny;
				if (nx + (int32_t)windows[dragging].w > drag_max_x)
					drag_max_x = nx + (int32_t)windows[dragging].w;
				if (ny + (int32_t)windows[dragging].h > drag_max_y)
					drag_max_y = ny + (int32_t)windows[dragging].h;

			}

		}

		if (btn_down && resizing >= 0) {

			int idx = resizing;

			int nw = cx - resize_off_x - (int)windows[idx].x + 1;
			int nh = cy - resize_off_y - (int)windows[idx].y + 1;

			// the window's own minimum (see create_window()) --
			// this is what Z_WIN_FLAG_MIN_IS_CREATE actually buys an
			// app: draw's tool column and palette can't be shrunk
			// out of existence.
			if (nw < (int)windows[idx].min_w) nw = (int)windows[idx].min_w;
			if (nh < (int)windows[idx].min_h) nh = (int)windows[idx].min_h;

			// and the screen's own bounds -- the top-left corner is
			// pinned during a resize, so only the far edges can leave
			// the screen.
			if ((int)windows[idx].x + nw > WM_SCREEN_W)
				nw = WM_SCREEN_W - (int)windows[idx].x;
			if ((int)windows[idx].y + nh > WM_SCREEN_H)
				nh = WM_SCREEN_H - (int)windows[idx].y;

			if (nw != resize_w || nh != resize_h) {

				// Erase the frame at the current size, adopt the new
				// one, draw the frame again -- the same three steps
				// the move drag uses, and for the same reason: a full
				// clear+redraw+content-notify per step queues redraws
				// faster than apps can drain them.
				//
				// This draws the WHOLE frame at the candidate size,
				// where it used to draw an L over the prospective
				// bottom and right edges only. The L was meant to say
				// "these are the two edges that move", but with the
				// real frame still sitting at the old size the result
				// on screen was two disconnected shapes and no
				// obvious relationship between them. A single closed
				// rectangle that grows and shrinks under the cursor
				// reads immediately, and matches what dragging a
				// window already looks like.
				//
				// w/h ARE updated here, unlike before. That is what
				// makes draw_window_box() draw the candidate rather
				// than a separate preview, and it is safe because
				// nothing else reads them mid-gesture -- pointer
				// dispatch and hit testing are both suspended while
				// `resizing` is set. The app is still told only once,
				// on release.
				draw_window_box(&windows[idx], idx == focused, Z_RASTER_XOR);

				resize_w = nw;
				resize_h = nh;
				windows[idx].w = (uint32_t)nw;
				windows[idx].h = (uint32_t)nh;

				draw_window_box(&windows[idx], idx == focused, Z_RASTER_XOR);

				if (nw > resize_max_w) resize_max_w = nw;
				if (nh > resize_max_h) resize_max_h = nh;

			}

		}

		if (!btn_down && btn_was_down && dragging >= 0) {
			printf("wm: drag release win %d final x=%ld y=%ld\n",
				dragging, (long)windows[dragging].x, (long)windows[dragging].y);
			notify_moved(dragging);
			repair_drag(dragging);
			dragging = -1;
		}

		if (!btn_down && btn_was_down && resizing >= 0) {

			int idx = resizing;

			printf("wm: resize release win %d %ldx%ld -> %dx%d\n",
				idx, (long)windows[idx].w, (long)windows[idx].h,
				resize_w, resize_h);

			// No preview to erase: the frame on screen IS the
			// window's frame, already at the final size, and w/h
			// already agree with it.
			int ow = resize_orig_w, oh = resize_orig_h;

			// clear this BEFORE repairing: repair_region() waits on
			// the owner's redraw ack and services other messages
			// while it does, so wm must not still look mid-gesture by
			// the time any of that runs.
			resizing = -1;

			// tell the app its new size first -- see notify_resized()
			// and Z_WM_WINDOW_RESIZED (zwm.h) on why this must
			// precede the redraw request repair_region() sends.
			notify_resized(idx);

			// repair the union of everywhere the window has been:
			// its original footprint, its final one, and the largest
			// extent it reached in between. Unlike the drag path
			// there's no point excluding the window's own final rect
			// -- its content area genuinely changed size, so all of
			// it needs redrawing anyway.
			int uw = ow > resize_max_w ? ow : resize_max_w;
			int uh = oh > resize_max_h ? oh : resize_max_h;
			if ((int)windows[idx].w > uw) uw = (int)windows[idx].w;
			if ((int)windows[idx].h > uh) uh = (int)windows[idx].h;

			repair_region((int)windows[idx].x, (int)windows[idx].y, uw, uh, -1);

		}

		// forward this pointer sample to whichever app owns it (if
		// any) -- after the gesture handling above, so a press that
		// starts a drag or resize is consumed by wm rather than also
		// reaching the app, and before the capture is released below,
		// so the app still receives the button-up that ends its own
		// gesture.
		dispatch_mouse(cx, cy, btn);

		// Super held -- drag the game mode viewport along with the
		// pointer. A level test on a held modifier, so it belongs here
		// in the pointer path rather than in dispatch_keys(): there is
		// no key event to react to while the key is simply down and
		// the mouse is moving.
		//
		// After dispatch_mouse() deliberately. Scrolling the viewport
		// does not move the pointer, so the sample the app receives is
		// the same either way -- but doing it in this order keeps the
		// rule that apps see the pointer before wm acts on the frame
		// it was sampled in.
		//
		// Reading the modifier byte straight from the HID register
		// rather than tracking it through dispatch_keys(): a modifier
		// that is merely HELD generates no events at all between its
		// press and its release, so an event-derived copy would be
		// stale exactly when it matters.
		if (z_game_enabled()) {
			uint32_t info = (mouse_port() == 0) ? reg_usb0_info : reg_usb1_info;
			uint32_t kinfo = (mouse_port() == 0) ? reg_usb1_info : reg_usb0_info;
			// The keyboard is usually the OTHER port from the mouse,
			// but need not be -- a combo device reports both on one.
			// Accept the modifier from either.
			if (((info | kinfo) & Z_KBD_MOD_GUI) != 0)
				game_follow_pointer(cx, cy);
		}

		if (!btn_down && btn_was_down) mouse_capture = -1;

		last_btn = btn;

		// clears WM_BUSY_STARTUP once net/repl are registered; repaint
		// the dock on the transition so it stops looking disabled
		if (check_core_services() && dock_idx >= 0)
			repair_region(windows[dock_idx].x, windows[dock_idx].y,
				windows[dock_idx].w, windows[dock_idx].h, -1);

		for (volatile int i = 0; i < 2000; i++); // light throttle

		/* Yield the rest of this timeslice.
		 *
		 * wm cannot block indefinitely the way repl can: the pointer
		 * is POLLED from rtl/usb_hid.v's cursor register, not
		 * delivered as a message, so nothing would wake it when the
		 * mouse moves.
		 *
		 * But spinning is not the alternative. Waiting one tick wakes
		 * this loop at Z_TICK_HZ (732Hz), which is more than twelve
		 * times the display's refresh rate -- far finer than anything
		 * a person can see in a pointer -- while handing back the
		 * ~99% of each timeslice that was previously spent re-reading
		 * a register that had not changed.
		 *
		 * A message arriving cuts the wait short, so app requests are
		 * still serviced immediately rather than up to a tick late.
		 *
		 * This matters well beyond wm's own responsiveness: the
		 * scheduler divides the CPU between RUNNABLE processes, so a
		 * spinning wm takes its share out of whatever is in the
		 * foreground. A full-screen app measured a quarter of the
		 * machine with three such spinners running alongside it, and
		 * a quarter of the CPU means a quarter of the frame rate. */
		z_proc_wait(1);

	}

}
