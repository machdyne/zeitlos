/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Modal dialog boxes. See zdialog.h for what a dialog is here, why
 * these block, and what the caller's on_msg callback is responsible
 * for.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zwm.h"
#include "zwin.h"
#include "zgfx.h"
#include "zfont.h"
#include "zkbd.h"
#include "zwidget.h"
#include "zflist.h"
#include "zdialog.h"

// -- shared dialog state --
//
// One instance, file-static, because exactly one dialog can be open
// at a time: every entry point here blocks until its own dialog is
// dismissed, so a second cannot start while a first is running. That
// isn't a limitation being worked around, it's what "modal" means --
// and it lets the file list (over 3KB) be shared rather than
// duplicated per dialog type.
//
// The alternative, a per-call struct on the stack, would put that
// same 3KB on a stack that is 16KB in total for most apps
// (Z_PROC_STACK_SIZE_DEFAULT, sw/os/kernel.h), underneath whatever
// the app was already doing when it opened the dialog.

#define DLG_FIELD_MAX   64

typedef enum {
	DLG_KIND_OPEN = 0,
	DLG_KIND_SAVE,
	DLG_KIND_CONFIRM,
	DLG_KIND_PROMPT,
} dlg_kind_t;

static struct {

	z_win_t			win;
	const z_dialog_ctx_t	*ctx;
	dlg_kind_t		kind;

	bool			done;
	int				result;

	// -- open/save --
	z_flist_t		flist;
	char			field[DLG_FIELD_MAX];	// save: the filename being typed
	int				field_len;
	int				field_cur;
	bool			field_focus;			// caret in the field vs the list

	// -- confirm --
	const char		*msg;
	int				button_set;

	// -- buttons, shared --
	z_widget_t		widgets[3];
	z_widget_set_t	wset;
	int				widget_count;

	// content-relative layout, recomputed once at create time (these
	// windows are not resizable, so once is enough)
	int				list_y, list_h;
	int				field_y;
	int				btn_y;

} dlg;

// -- geometry --

#define DLG_MARGIN      6
#define DLG_LINE_H      (z_font_5x8.h + 2)
#define DLG_BTN_W       54
#define DLG_BTN_H       16
#define DLG_BTN_GAP     6
#define DLG_FIELD_H     14

// The file dialogs are a fixed size. Deliberately: a resizable dialog
// means handling Z_WM_WINDOW_RESIZED and relaying out mid-loop, for a
// window the user is looking at for a few seconds. The size below
// shows a useful number of rows at z_font_5x8 without crowding the
// screen -- and, unlike an app window, a dialog that doesn't fit
// somebody's document is not a problem it has to solve.
#define DLG_FILE_W      288
#define DLG_FILE_H      196

#define DLG_CONFIRM_W   248

// -- forward decls --

static void dlg_repaint(void);
static void dlg_mouse(int cx, int cy, uint8_t buttons);
static void dlg_activate_button(int act);
static void dlg_key(uint32_t keysym, uint8_t mods);

// -- message routing --
//
// See zdialog.h for the full table of what gets routed how, and why
// keys are unconditionally ours.

static void dlg_dispatch(z_msg_t *msg) {

	switch (msg->subject) {

		case Z_WM_REDRAW: {

			if (msg->obj.type != Z_UINT32) break;

			uint32_t packed = msg->obj.val.uint32;

			if (z_win_redraw_id(packed) == dlg.win.id) {
				z_win_apply_redraw(&dlg.win, packed);
				dlg_repaint();
				z_win_redraw_done(&dlg.win);
			} else if (dlg.ctx && dlg.ctx->on_msg) {
				dlg.ctx->on_msg(msg, dlg.ctx->user);
			}

			break;

		}

		case Z_WM_WINDOW_MOVED: {

			z_obj_t *id = z_map_find(&msg->obj, "id");

			if (id && id->type == Z_INT32 && id->val.int32 == dlg.win.id)
				z_win_parse_rect(&dlg.win, &msg->obj);
			else if (dlg.ctx && dlg.ctx->on_msg)
				dlg.ctx->on_msg(msg, dlg.ctx->user);

			break;

		}

		case Z_WM_CLOSE:

			// The dialog's own titlebar close icon means cancel --
			// same answer as the Cancel button and as Escape, so
			// there is exactly one "backed out" result no matter how
			// the user got there.
			if (msg->obj.type == Z_UINT32 &&
				(int32_t)msg->obj.val.uint32 == dlg.win.id) {
				dlg.result = Z_DIALOG_CANCEL;
				dlg.done = true;
			} else if (dlg.ctx && dlg.ctx->on_msg) {
				dlg.ctx->on_msg(msg, dlg.ctx->user);
			}

			break;

		case Z_WM_MOUSE: {

			if (msg->obj.type != Z_UINT32) break;

			// Unconditionally ours. wm only delivers pointer events
			// to the FOCUSED window (or to whatever holds a capture
			// -- dispatch_mouse() in wm.c), a modal dialog is focused
			// for as long as it exists, and clicks on the parent are
			// swallowed by wm's own modal check. So anything arriving
			// here is either over this dialog or part of a drag this
			// dialog started -- and a scrollbar drag that wandered
			// outside the window is exactly the case that must NOT be
			// filtered out by a bounds test.
			int cx, cy;
			z_win_mouse_content_xy(&dlg.win, msg->obj.val.uint32, &cx, &cy);

			dlg_mouse(cx, cy,
				(uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(msg->obj.val.uint32));

			break;

		}

		case Z_WM_KEY: {

			if (msg->obj.type != Z_UINT32) break;

			uint32_t packed = msg->obj.val.uint32;
			if (!Z_WM_UNPACK_KEY_PRESSED(packed)) break;	// ignore key-up

			dlg_key(Z_WM_UNPACK_KEY_KEYSYM(packed),
				(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(packed));

			break;

		}

		default:

			// Z_WM_WINDOW_RESIZED, Z_WM_TITLEBAR_ICON and anything
			// else all belong to the parent -- dialogs are neither
			// resizable nor equipped with extra titlebar icons, so
			// nothing addressed to this window can turn up here.
			if (dlg.ctx && dlg.ctx->on_msg)
				dlg.ctx->on_msg(msg, dlg.ctx->user);

			break;

	}

}

// Runs until the dialog sets done. Drains the whole queue each pass
// rather than one message per iteration, for the same reason
// sw/apps/draw does: pointer events arrive faster than a redraw can
// service them, and handling one per loop falls progressively further
// behind the real cursor.
static int dlg_run(void) {

	while (!dlg.done) {

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			dlg_dispatch(&msg);
			if (dlg.done) break;
		}

		for (volatile int i = 0; i < 200; i++);	// light throttle

	}

	// Whatever is still queued was generated while this dialog had
	// focus, so its pointer coordinates are over the DIALOG. Left in
	// the queue, the app's own loop would pick them up moments later
	// and read them against its own window -- a click meant for the
	// Cancel button placing a caret somewhere unrelated.
	//
	// So drop the input, but keep everything else: a Z_WM_REDRAW
	// discarded here is one wm is blocking on an ack for, which is
	// the exact failure z_win_create_cb() exists to avoid. Done
	// BEFORE the destroy below, while this window still has an id to
	// route by.
	{
		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_WM_MOUSE || msg.subject == Z_WM_KEY)
				continue;
			dlg_dispatch(&msg);
		}
	}

	z_win_destroy(&dlg.win);

	// Destroying the window makes wm repair the region it occupied,
	// which means asking the PARENT to redraw and blocking on that
	// ack. The caller is in no position to service that: the very
	// next thing z_dialog_save()'s caller does is write a file, which
	// on this hardware is an SD card round trip long enough to blow
	// REDRAW_ACK_TIMEOUT. So the redraw is serviced here, before
	// returning.
	//
	// Bounded, and it stops at the first redraw rather than waiting
	// for a particular one: wm sends exactly one per repaired window,
	// and if it never arrives at all (nothing overlapped the dialog,
	// so nothing needed repairing) the spin count is the way out. The
	// numbers are a fraction of wm's own timeout -- long enough to
	// cover the round trip, short enough not to be felt.
	for (int i = 0; i < 200; i++) {

		bool saw_redraw = false;
		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_WM_REDRAW) saw_redraw = true;
			if (dlg.ctx && dlg.ctx->on_msg)
				dlg.ctx->on_msg(&msg, dlg.ctx->user);
		}

		if (saw_redraw) break;

		for (volatile int j = 0; j < 500; j++);

	}

	return dlg.result;

}

// -- creation --

// Centers `w` x `h` on the context's parent window, or on the screen
// if there isn't one, and clamps the result on screen.
static void dlg_center(const z_dialog_ctx_t *ctx, int w, int h,
	int32_t *out_x, int32_t *out_y) {

	int px, py, pw, ph;

	if (ctx && ctx->parent && ctx->parent->id >= 0) {
		px = (int)ctx->parent->x;
		py = (int)ctx->parent->y;
		pw = (int)ctx->parent->w;
		ph = (int)ctx->parent->h;
	} else {
		px = 0; py = 0; pw = Z_SCREEN_W; ph = Z_SCREEN_H;
	}

	int x = px + (pw - w) / 2;
	int y = py + (ph - h) / 2;

	// A parent near an edge (or larger than the screen) would
	// otherwise put the dialog partly off it, where its buttons
	// cannot be clicked at all. wm clamps window POSITION on drag but
	// does not move a window that was created off-screen.
	if (x + w > Z_SCREEN_W) x = Z_SCREEN_W - w;
	if (y + h > Z_SCREEN_H) y = Z_SCREEN_H - h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;

	*out_x = x;
	*out_y = y;

}

// Forwards to the app while the dialog's own window is still being
// created -- see z_win_create_cb() (zwin.h) for why creating a window
// mid-session cannot use the plain z_win_create() path.
static void dlg_create_cb(z_msg_t *msg, void *user) {

	const z_dialog_ctx_t *ctx = (const z_dialog_ctx_t *)user;

	if (ctx && ctx->on_msg) ctx->on_msg(msg, ctx->user);

}

static bool dlg_create(const z_dialog_ctx_t *ctx, dlg_kind_t kind,
	const char *title, int w, int h) {

	memset(&dlg, 0, sizeof(dlg));

	dlg.ctx = ctx;
	dlg.kind = kind;
	dlg.result = Z_DIALOG_CANCEL;

	int32_t x, y;
	dlg_center(ctx, w, h, &x, &y);

	// CLOSE_ICON without CLOSE_KILLS_OWNER: clicking it must cancel
	// the dialog, not kill the app. The killing form is explicitly
	// documented as wrong for any process owning more than one window
	// (zwm.h), and an app showing a dialog owns at least two.
	//
	// Not resizable, no other titlebar icons.
	if (z_win_create_cb(&dlg.win, title, (uint32_t)w, (uint32_t)h, x, y,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_MODAL,
		dlg_create_cb, (void *)ctx) != Z_OK)
		return false;

	return dlg.win.id >= 0;

}

// -- buttons --

static void dlg_buttons_init(const char *const *labels, int count) {

	bool kind_wants_button_focus =
		(dlg.kind == DLG_KIND_CONFIRM || dlg.kind == DLG_KIND_PROMPT);


	memset(dlg.widgets, 0, sizeof(dlg.widgets));

	int cw = z_win_content_w(&dlg.win);

	// Right-aligned, in the order given, so the affirmative button
	// (always first in every set below) ends up leftmost of the
	// group and Cancel sits nearest the corner -- which is where a
	// hand reaching to back out expects it.
	int total = count * DLG_BTN_W + (count - 1) * DLG_BTN_GAP;
	int bx = cw - DLG_MARGIN - total;
	if (bx < DLG_MARGIN) bx = DLG_MARGIN;

	for (int i = 0; i < count; i++) {
		dlg.widgets[i].type = Z_WIDGET_BUTTON;
		dlg.widgets[i].x = (int16_t)(bx + i * (DLG_BTN_W + DLG_BTN_GAP));
		dlg.widgets[i].y = (int16_t)dlg.btn_y;
		dlg.widgets[i].w = DLG_BTN_W;
		dlg.widgets[i].h = DLG_BTN_H;
		dlg.widgets[i].label = labels[i];
		dlg.widgets[i].enabled = true;
	}

	dlg.widget_count = count;

	z_widget_set_init(&dlg.wset, dlg.widgets, count, &dlg.win);

	// Confirm and prompt dialogs start with the affirmative button
	// focused, so Tab has an obvious starting point and Space works
	// immediately. The file dialogs deliberately do NOT -- focus
	// starts in the field or the list there, which is where typing
	// should go.
	if (kind_wants_button_focus)
		z_widget_focus_set(&dlg.wset, 0);

}

// -- the filename field (save dialog only) --
//
// A single-line text entry, kept private to this file rather than
// promoted into zwidget.h. There is exactly one caller, and the
// version a general widget would need -- selection, scrolling for
// text wider than the box, a caret that survives losing focus -- is
// several times this size. If a second caller appears, that is the
// moment to move it, not before.

static void field_set(const char *s) {

	dlg.field_len = 0;

	if (s)
		for (; s[dlg.field_len] && dlg.field_len < DLG_FIELD_MAX - 1;
			dlg.field_len++)
			dlg.field[dlg.field_len] = s[dlg.field_len];

	dlg.field[dlg.field_len] = 0;
	dlg.field_cur = dlg.field_len;

}

static void field_draw(void) {

	z_clip_t content;
	z_win_content_rect(&dlg.win, &content);

	int cw = z_win_content_w(&dlg.win);

	int x0 = content.x0 + DLG_MARGIN;
	int y0 = content.y0 + dlg.field_y;
	int x1 = content.x0 + cw - DLG_MARGIN - 1;
	int y1 = y0 + DLG_FIELD_H - 1;

	if (x1 <= x0 || y1 <= y0) return;

	// Everything here is drawn with the BLITTER -- including the
	// frame, which would more naturally be a z_win_hw_box().
	//
	// The blitter and the line rasterizer are independent engines
	// writing the same VRAM with no ordering between them:
	// z_fb_hw_fill_rect() waits for the BLITTER to finish, while
	// z_fb_hw_line() only waits for FIFO space and returns with the
	// line still queued. The blitter read-modify-writes whole 32-bit
	// words, so a rasterizer pixel that hasn't landed when the blitter
	// reads a word is lost when it writes the word back.
	//
	// This field is unusually exposed to that. Its left border sits at
	// content.x0 + DLG_MARGIN, which for a dialog at x=88 is absolute
	// x=96 -- exactly a 32-pixel word boundary -- and the first six
	// glyphs (x=99 through 123) all fall inside that same word. Frame
	// and text therefore share one word, drawn by two engines, in an
	// order nothing enforces.
	//
	// HONEST CAVEAT: that the two engines can race over a shared word
	// is demonstrable from the code above; that it is the whole
	// explanation for the reported symptom (the outline partly
	// missing, filling in as characters were typed, complete at six)
	// is NOT confirmed. A pure race would if anything get worse with
	// more glyphs, not better. Something about the observed pattern is
	// still unaccounted for.
	//
	// What this change does is remove the ambiguity rather than
	// diagnose it: with one engine, program order IS drawing order and
	// there is nothing left to race. If the outline still misbehaves
	// after this, the cause is somewhere else entirely and this
	// comment should be corrected rather than trusted.

	// interior
	z_fb_hw_fill_rect(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1, 0);

	// frame, as four one-pixel fills
	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, 1, 1);			// top
	z_fb_hw_fill_rect(x0, y1, x1 - x0 + 1, 1, 1);			// bottom
	z_fb_hw_fill_rect(x0, y0, 1, y1 - y0 + 1, 1);			// left
	z_fb_hw_fill_rect(x1, y0, 1, y1 - y0 + 1, 1);			// right

	z_clip_t clip;
	clip.x0 = x0 + 2;
	clip.y0 = y0 + 1;
	clip.x1 = x1 - 2;
	clip.y1 = y1 - 1;

	if (clip.x1 < clip.x0) return;

	z_fb_draw_text(x0 + 3, y0 + 3, dlg.field, 1, &z_font_5x8, &clip);

	// Caret. Only drawn while the field has focus -- otherwise a
	// dialog with a caret in the field AND a highlighted row in the
	// list gives two equally strong claims on where typing goes.
	//
	// A one-pixel fill rather than a rasterizer line, for the same
	// single-engine reason as the frame above.
	if (dlg.field_focus) {
		int cx = x0 + 3 + dlg.field_cur * z_font_5x8.w;
		if (cx <= clip.x1)
			z_fb_hw_fill_rect(cx, y0 + 2, 1, y1 - y0 - 3, 1);
	}

}

static void field_key(uint32_t keysym) {

	switch (keysym) {

		case Z_KEY_LEFT:
			if (dlg.field_cur > 0) dlg.field_cur--;
			break;

		case Z_KEY_RIGHT:
			if (dlg.field_cur < dlg.field_len) dlg.field_cur++;
			break;

		case Z_KEY_HOME:
			dlg.field_cur = 0;
			break;

		case Z_KEY_END:
			dlg.field_cur = dlg.field_len;
			break;

		case 0x7f:		// Backspace -- DEL, see zkbd.c
			if (dlg.field_cur > 0) {
				memmove(&dlg.field[dlg.field_cur - 1],
					&dlg.field[dlg.field_cur],
					(size_t)(dlg.field_len - dlg.field_cur + 1));
				dlg.field_cur--;
				dlg.field_len--;
			}
			break;

		case Z_KEY_DELETE:
			if (dlg.field_cur < dlg.field_len) {
				memmove(&dlg.field[dlg.field_cur],
					&dlg.field[dlg.field_cur + 1],
					(size_t)(dlg.field_len - dlg.field_cur));
				dlg.field_len--;
			}
			break;

		default:

			if (keysym < 0x20 || keysym > 0x7e) return;
			if (dlg.field_len >= DLG_FIELD_MAX - 1) return;

			memmove(&dlg.field[dlg.field_cur + 1],
				&dlg.field[dlg.field_cur],
				(size_t)(dlg.field_len - dlg.field_cur + 1));
			dlg.field[dlg.field_cur] = (char)keysym;
			dlg.field_cur++;
			dlg.field_len++;

			break;

	}

	field_draw();

}

// -- painting --

// Draws the current directory above the list, elided from the LEFT
// when it doesn't fit: the interesting end of a path is the last
// component, not the first.
static void path_draw(void) {

	z_clip_t content;
	z_win_content_rect(&dlg.win, &content);

	int cw = z_win_content_w(&dlg.win);
	int avail = cw - 2 * DLG_MARGIN;
	if (avail <= 0) return;

	int maxch = avail / z_font_5x8.w;
	const char *p = dlg.flist.path;
	int len = (int)strlen(p);

	char shown[Z_FLIST_PATH_MAX + 4];

	if (len <= maxch) {
		memcpy(shown, p, (size_t)len + 1);
	} else if (maxch > 3) {
		shown[0] = shown[1] = shown[2] = '.';
		memcpy(shown + 3, p + len - (maxch - 3), (size_t)(maxch - 3) + 1);
	} else {
		shown[0] = 0;
	}

	z_clip_t clip;
	clip.x0 = content.x0 + DLG_MARGIN;
	clip.y0 = content.y0 + DLG_MARGIN;
	clip.x1 = content.x0 + cw - DLG_MARGIN - 1;
	clip.y1 = clip.y0 + z_font_5x8.h - 1;

	if (clip.x1 < clip.x0) return;

	z_fb_hw_fill_rect(clip.x0, clip.y0,
		clip.x1 - clip.x0 + 1, z_font_5x8.h, 0);

	z_fb_draw_text(clip.x0, clip.y0, shown, 1, &z_font_5x8, &clip);

}

// Word-less line splitter for the confirm dialog's message: splits on
// '\n' only. Callers write their own line breaks, which for the two
// or three sentences a confirm box holds is both simpler and gives
// better results than automatic wrapping would.
static int msg_lines(const char *s, const char **starts, int *lens, int max) {

	int n = 0;

	while (*s && n < max) {
		starts[n] = s;
		int l = 0;
		while (s[l] && s[l] != '\n') l++;
		lens[n] = l;
		n++;
		s += l;
		if (*s == '\n') s++;
	}

	return n;

}

static void dlg_repaint(void) {

	// Clear the whole content area first. wm clears a region before
	// asking for a redraw in most cases but NOT after a window move
	// (repair_drag() in wm.c deliberately excludes the window's own
	// final footprint) -- so on a drag, whatever was last drawn here
	// is still on screen at the old offset, and only pixels actively
	// rewritten get corrected. Everything this function doesn't paint
	// over would keep its pre-move contents.
	z_win_clear(&dlg.win);

	if (dlg.kind == DLG_KIND_CONFIRM || dlg.kind == DLG_KIND_PROMPT) {

		z_clip_t content;
		z_win_content_rect(&dlg.win, &content);

		const char *starts[Z_DIALOG_MSG_LINES];
		int lens[Z_DIALOG_MSG_LINES];
		int n = msg_lines(dlg.msg ? dlg.msg : "", starts, lens,
			Z_DIALOG_MSG_LINES);

		z_clip_t clip;
		clip.x0 = content.x0 + DLG_MARGIN;
		clip.y0 = content.y0;
		clip.x1 = content.x1 - DLG_MARGIN;
		clip.y1 = content.y1;

		for (int i = 0; i < n; i++) {

			// Copied into a bounded buffer because z_fb_draw_text()
			// takes a NUL-terminated string and the source is one
			// long string with embedded newlines.
			char line[64];
			int l = lens[i];
			if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;
			memcpy(line, starts[i], (size_t)l);
			line[l] = 0;

			z_fb_draw_text(content.x0 + DLG_MARGIN,
				content.y0 + DLG_MARGIN + i * DLG_LINE_H,
				line, 1, &z_font_5x8, &clip);

		}

		if (dlg.kind == DLG_KIND_PROMPT) field_draw();

	} else {

		path_draw();
		z_flist_invalidate(&dlg.flist);
		z_flist_draw(&dlg.flist, true);

		if (dlg.kind == DLG_KIND_SAVE) field_draw();

	}

	z_widget_draw_all(&dlg.wset, true);

}

// -- what OK means --

// Builds the answer for an open/save dialog and finishes, or returns
// without finishing if there is nothing to answer with.
static void dlg_accept(char *out, int outlen) {

	if (dlg.kind == DLG_KIND_OPEN) {

		// A directory is not an answer -- descend into it instead,
		// which is what a user pressing OK on a folder means.
		if (z_flist_selected_is_dir(&dlg.flist)) {
			z_flist_key(&dlg.flist, 0x0d);		// Enter -- navigate
			path_draw();
			z_flist_draw(&dlg.flist, true);
			return;
		}

		if (!z_flist_selected_path(&dlg.flist, out, outlen)) return;

		dlg.result = Z_DIALOG_YES;
		dlg.done = true;

		return;

	}

	// -- save --

	if (dlg.field_len == 0) return;		// nothing typed, nothing to do

	// Absolute names are taken as-is; anything else is relative to
	// the directory being browsed. That lets a user type a full path
	// into the field and have it mean what it says.
	int n = 0;

	if (dlg.field[0] != '/') {
		for (const char *s = dlg.flist.path; *s && n < outlen - 1; s++)
			out[n++] = *s;
		if (n > 0 && out[n - 1] != '/' && n < outlen - 1) out[n++] = '/';
	}

	for (int i = 0; i < dlg.field_len && n < outlen - 1; i++)
		out[n++] = dlg.field[i];

	out[n] = 0;

	dlg.result = Z_DIALOG_YES;
	dlg.done = true;

}

// The caller's `out` buffer, stashed so the shared input handlers can
// write into it without every one of them carrying it as a parameter.
// Safe for the same reason `dlg` itself is: one dialog at a time, and
// the buffer outlives the blocking call it was passed to.
static char *dlg_out;
static int dlg_outlen;

// -- input --

// Acts on button `act` being pressed, whether by pointer or keyboard.
//
// Factored out so the two input paths cannot drift: a dialog where
// Space did something subtly different from a click would be a
// genuinely nasty thing to debug.
static void dlg_activate_button(int act) {

	if (act < 0) return;

	if (dlg.kind == DLG_KIND_PROMPT) {
		// OK only counts with something typed -- an empty name is
		// not an answer, and silently accepting one would have the
		// caller create "" and fail.
		if (act == 0 && dlg.field_len > 0) {
			dlg.result = Z_DIALOG_YES;
			dlg.done = true;
		} else if (act != 0) {
			dlg.result = Z_DIALOG_CANCEL;
			dlg.done = true;
		}
		return;
	}

	if (dlg.kind == DLG_KIND_CONFIRM) {

		// Button order matches the labels passed to
		// dlg_buttons_init() for each set -- see z_dialog_confirm().
		switch (dlg.button_set) {
			case Z_DIALOG_YES_NO:
				dlg.result = (act == 0) ? Z_DIALOG_YES : Z_DIALOG_NO;
				break;
			case Z_DIALOG_YES_NO_CANCEL:
				dlg.result = (act == 0) ? Z_DIALOG_YES :
					(act == 1) ? Z_DIALOG_NO : Z_DIALOG_CANCEL;
				break;
			case Z_DIALOG_OK_CANCEL:
			default:
				dlg.result = (act == 0) ? Z_DIALOG_YES : Z_DIALOG_CANCEL;
				break;
		}

		dlg.done = true;
		return;

	}

	// file dialogs
	if (act == 0) dlg_accept(dlg_out, dlg_outlen);
	else { dlg.result = Z_DIALOG_CANCEL; dlg.done = true; }

}

static void dlg_mouse(int cx, int cy, uint8_t buttons) {

	int act = z_widget_mouse(&dlg.wset, cx, cy, buttons);

	if (act >= 0) {
		dlg_activate_button(act);
		return;
	}

	// a widget gesture in progress owns the pointer
	if (dlg.wset.pressed >= 0) return;

	if (dlg.kind == DLG_KIND_CONFIRM) return;

	// The filename field takes focus when clicked, and gives it back
	// when the list is.
	if (dlg.kind == DLG_KIND_SAVE && (buttons & Z_MOUSE_BTN_LEFT)) {

		int cw = z_win_content_w(&dlg.win);
		bool in_field = cx >= DLG_MARGIN && cx < cw - DLG_MARGIN &&
			cy >= dlg.field_y && cy < dlg.field_y + DLG_FIELD_H;

		if (in_field && !dlg.field_focus) {
			dlg.field_focus = true;
			field_draw();
			return;
		}

		if (!in_field && dlg.field_focus &&
			cy >= dlg.list_y && cy < dlg.list_y + dlg.list_h) {
			dlg.field_focus = false;
			field_draw();
		}

	}

	int r = z_flist_mouse(&dlg.flist, cx, cy, buttons);

	if (r == Z_FLIST_NONE) return;

	// A directory activation has already navigated; repaint the path
	// line to match, and for save, leave the field alone (the name
	// being typed survives changing directory, which is what makes
	// "type the name, then go find the folder" work).
	if (r == Z_FLIST_ACTIVATED && z_flist_activated_dir(&dlg.flist)) {
		path_draw();
		return;
	}

	if (dlg.kind == DLG_KIND_SAVE) {

		// Selecting a file copies its name into the field, the way
		// every save dialog does -- it's the fastest way to overwrite
		// an existing file, and the user can still edit it.
		const char *name = z_flist_selected(&dlg.flist);
		if (name && !z_flist_selected_is_dir(&dlg.flist)) {
			field_set(name);
			field_draw();
		}

	}

	if (r == Z_FLIST_ACTIVATED) dlg_accept(dlg_out, dlg_outlen);

}

static void dlg_key(uint32_t keysym, uint8_t mods) {

	(void)mods;

	// Escape always backs out, from every dialog, with the same
	// result the Cancel button gives.
	if (keysym == 0x1b) {
		dlg.result = (dlg.kind == DLG_KIND_CONFIRM &&
			dlg.button_set == Z_DIALOG_YES_NO) ? Z_DIALOG_NO : Z_DIALOG_CANCEL;
		dlg.done = true;
		return;
	}

	// Tab moves the button focus, in every kind of dialog.
	//
	// Enter and Escape have always covered the common answers, but
	// "the common answers" is not the same as "all of them" -- a
	// three-button confirm has a middle option reachable no other way
	// without a pointer, and keyboard-only operation is a first-class
	// case here (docs/window_manager.md).
	//
	// In the file dialogs Tab has a prior meaning -- moving between
	// the filename field and the list -- so there it cycles field ->
	// list -> buttons -> field rather than jumping straight to the
	// buttons.
	if (keysym == '\t') {

		bool back = (mods & Z_KBD_MOD_SHIFT) != 0;

		if (dlg.kind == DLG_KIND_OPEN || dlg.kind == DLG_KIND_SAVE) {

			// Three stops for save (field, list, buttons), two for
			// open (list, buttons).
			if (dlg.wset.focused >= 0) {
				// leaving the buttons
				int next = z_widget_focus_next(&dlg.wset, back);
				if (next == (back ? dlg.widget_count - 1 : 0)) {
					z_widget_focus_set(&dlg.wset, -1);
					dlg.field_focus = (dlg.kind == DLG_KIND_SAVE);
				}
			} else if (dlg.kind == DLG_KIND_SAVE && dlg.field_focus) {
				dlg.field_focus = false;		// into the list
			} else {
				z_widget_focus_next(&dlg.wset, back);
			}

			z_widget_draw_all(&dlg.wset, false);
			field_draw();
			z_flist_draw(&dlg.flist, true);

			return;

		}

		z_widget_focus_next(&dlg.wset, back);
		z_widget_draw_all(&dlg.wset, false);

		return;

	}

	// Space presses the focused button, wherever focus happens to be.
	// Enter keeps its own meaning per dialog kind below -- in a file
	// dialog it accepts the selection rather than pressing whatever
	// is focused, which is what the hand expects there.
	if (keysym == ' ' && dlg.wset.focused >= 0) {
		dlg_activate_button(dlg.wset.focused);
		return;
	}

	if (dlg.kind == DLG_KIND_PROMPT) {

		if (keysym == 0x0d) {
			if (dlg.field_len > 0) {
				dlg.result = Z_DIALOG_YES;
				dlg.done = true;
			}
			return;
		}

		field_key(keysym);
		return;

	}

	if (dlg.kind == DLG_KIND_CONFIRM) {

		// Enter takes the affirmative, which is always the first
		// button in every set here.
		if (keysym == 0x0d) {
			dlg.result = Z_DIALOG_YES;
			dlg.done = true;
		}

		// Y/N as shortcuts, for the sets where they read as such.
		if (dlg.button_set != Z_DIALOG_OK_CANCEL) {
			if (keysym == 'y' || keysym == 'Y') {
				dlg.result = Z_DIALOG_YES;
				dlg.done = true;
			} else if (keysym == 'n' || keysym == 'N') {
				dlg.result = Z_DIALOG_NO;
				dlg.done = true;
			}
		}

		return;

	}

	// Tab moves between the filename field and the list, in the save
	// dialog. Nothing else has keyboard focus here, so this is a
	// two-way toggle rather than a focus ring.
	if (dlg.kind == DLG_KIND_SAVE && keysym == '\t') {
		dlg.field_focus = !dlg.field_focus;
		field_draw();
		z_flist_draw(&dlg.flist, true);
		return;
	}

	if (keysym == 0x0d) {
		dlg_accept(dlg_out, dlg_outlen);
		return;
	}

	if (dlg.kind == DLG_KIND_SAVE && dlg.field_focus) {
		field_key(keysym);
		return;
	}

	int r = z_flist_key(&dlg.flist, keysym);

	if (r == Z_FLIST_NONE) return;

	path_draw();

	if (dlg.kind == DLG_KIND_SAVE) {
		const char *name = z_flist_selected(&dlg.flist);
		if (name && !z_flist_selected_is_dir(&dlg.flist)) {
			field_set(name);
			field_draw();
		}
	}

}

// -- entry points --

static const char *const labels_ok_cancel[2]     = { "OK", "Cancel" };
static const char *const labels_yes_no[2]        = { "Yes", "No" };
static const char *const labels_yes_no_cancel[3] = { "Yes", "No", "Cancel" };
static const char *const labels_open[2]          = { "Open", "Cancel" };
static const char *const labels_save[2]          = { "Save", "Cancel" };

// Shared setup for the two file dialogs, which differ only in whether
// there is a filename field taking up room above the buttons.
static bool file_dialog(const z_dialog_ctx_t *ctx, dlg_kind_t kind,
	const char *title, const char *start_dir, const char *suggested,
	const char *const *labels, char *out, int outlen) {

	bool is_save = (kind == DLG_KIND_SAVE);

	int h = DLG_FILE_H + (is_save ? (DLG_FIELD_H + DLG_MARGIN) : 0);

	if (!dlg_create(ctx, kind, title, DLG_FILE_W, h)) return false;

	// Stashed for the shared input handlers rather than threaded
	// through every one of them -- see dlg_out's own comment.
	dlg_out = out;
	dlg_outlen = outlen;

	int ch = z_win_content_h(&dlg.win);
	int cw = z_win_content_w(&dlg.win);

	// Laid out from the bottom up: the buttons have a fixed size and
	// must sit on the bottom margin, the field (if any) sits above
	// them, and the list gets everything left over. Doing it the
	// other way round -- list first, buttons wherever they land --
	// is how a dialog ends up with its buttons half off the edge.
	dlg.btn_y = ch - DLG_MARGIN - DLG_BTN_H;

	dlg.field_y = is_save
		? (dlg.btn_y - DLG_MARGIN - DLG_FIELD_H)
		: dlg.btn_y;

	dlg.list_y = DLG_MARGIN + z_font_5x8.h + 3;
	dlg.list_h = dlg.field_y - DLG_MARGIN - dlg.list_y;
	if (dlg.list_h < 0) dlg.list_h = 0;

	z_flist_init(&dlg.flist, &dlg.win);
	z_flist_set_geom(&dlg.flist, DLG_MARGIN, dlg.list_y,
		cw - 2 * DLG_MARGIN, dlg.list_h);
	z_flist_chdir(&dlg.flist, start_dir);

	dlg_buttons_init(labels, 2);

	if (is_save) {
		field_set(suggested);
		// The field starts focused: the overwhelmingly common thing
		// to do in a save dialog is type a name, and starting in the
		// list would mean pressing Tab first every single time.
		dlg.field_focus = true;
	}

	dlg_repaint();

	return dlg_run() == Z_DIALOG_YES;

}

bool z_dialog_open(const z_dialog_ctx_t *ctx, const char *start_dir,
	char *out, int outlen) {

	if (!out || outlen < 2) return false;

	out[0] = 0;

	return file_dialog(ctx, DLG_KIND_OPEN, "Open", start_dir, NULL,
		labels_open, out, outlen);

}

bool z_dialog_save(const z_dialog_ctx_t *ctx, const char *start_dir,
	const char *suggested, char *out, int outlen) {

	if (!out || outlen < 2) return false;

	out[0] = 0;

	return file_dialog(ctx, DLG_KIND_SAVE, "Save As", start_dir, suggested,
		labels_save, out, outlen);

}

bool z_dialog_prompt(const z_dialog_ctx_t *ctx, const char *title,
	const char *msg, const char *initial, char *out, int outlen) {

	if (!out || outlen < 2) return false;

	out[0] = 0;

	const char *starts[Z_DIALOG_MSG_LINES];
	int lens[Z_DIALOG_MSG_LINES];
	int nlines = msg_lines(msg ? msg : "", starts, lens, Z_DIALOG_MSG_LINES);
	if (nlines < 1) nlines = 1;

	// Same sizing as z_dialog_confirm(), plus a row for the field.
	int w = DLG_CONFIRM_W;
	int need = 2 * DLG_BTN_W + DLG_BTN_GAP + 2 * DLG_MARGIN + 4;
	if (need > w) w = need;

	int longest = 0;
	for (int i = 0; i < nlines; i++) if (lens[i] > longest) longest = lens[i];
	int text_need = longest * z_font_5x8.w + 2 * DLG_MARGIN + 4;
	if (text_need > w) w = text_need;
	if (w > Z_SCREEN_W - 8) w = Z_SCREEN_W - 8;

	int h = Z_WM_TITLEBAR_H + 4 + DLG_MARGIN + nlines * DLG_LINE_H +
		DLG_MARGIN + DLG_FIELD_H + DLG_MARGIN + DLG_BTN_H + DLG_MARGIN;

	if (!dlg_create(ctx, DLG_KIND_PROMPT, title ? title : "", w, h))
		return false;

	dlg.msg = msg;
	dlg.button_set = Z_DIALOG_OK_CANCEL;

	int ch = z_win_content_h(&dlg.win);
	dlg.btn_y = ch - DLG_MARGIN - DLG_BTN_H;
	dlg.field_y = dlg.btn_y - DLG_MARGIN - DLG_FIELD_H;

	dlg_buttons_init(labels_ok_cancel, 2);

	field_set(initial);
	dlg.field_focus = true;		// typing is the entire point

	dlg_repaint();

	if (dlg_run() != Z_DIALOG_YES) return false;

	int i = 0;
	for (; i < outlen - 1 && dlg.field[i]; i++) out[i] = dlg.field[i];
	out[i] = 0;

	return out[0] != 0;

}

int z_dialog_confirm(const z_dialog_ctx_t *ctx, const char *title,
	const char *msg, int buttons) {

	const char *starts[Z_DIALOG_MSG_LINES];
	int lens[Z_DIALOG_MSG_LINES];
	int nlines = msg_lines(msg ? msg : "", starts, lens, Z_DIALOG_MSG_LINES);
	if (nlines < 1) nlines = 1;

	int nbtn = (buttons == Z_DIALOG_YES_NO_CANCEL) ? 3 : 2;

	// Wide enough for the buttons AND for the longest message line,
	// whichever needs more. A confirm box narrower than its own
	// message would clip the very text the user has to read to
	// answer it.
	int w = DLG_CONFIRM_W;
	int need = nbtn * DLG_BTN_W + (nbtn - 1) * DLG_BTN_GAP + 2 * DLG_MARGIN + 4;
	if (need > w) w = need;

	int longest = 0;
	for (int i = 0; i < nlines; i++) if (lens[i] > longest) longest = lens[i];
	int text_need = longest * z_font_5x8.w + 2 * DLG_MARGIN + 4;
	if (text_need > w) w = text_need;
	if (w > Z_SCREEN_W - 8) w = Z_SCREEN_W - 8;

	int h = Z_WM_TITLEBAR_H + 4 + DLG_MARGIN + nlines * DLG_LINE_H +
		DLG_MARGIN + DLG_BTN_H + DLG_MARGIN;

	if (!dlg_create(ctx, DLG_KIND_CONFIRM, title ? title : "", w, h))
		return Z_DIALOG_CANCEL;

	dlg.msg = msg;
	dlg.button_set = buttons;

	dlg.btn_y = z_win_content_h(&dlg.win) - DLG_MARGIN - DLG_BTN_H;

	switch (buttons) {
		case Z_DIALOG_YES_NO:
			dlg_buttons_init(labels_yes_no, 2);
			break;
		case Z_DIALOG_YES_NO_CANCEL:
			dlg_buttons_init(labels_yes_no_cancel, 3);
			break;
		case Z_DIALOG_OK_CANCEL:
		default:
			dlg_buttons_init(labels_ok_cancel, 2);
			break;
	}

	dlg_repaint();

	return dlg_run();

}
