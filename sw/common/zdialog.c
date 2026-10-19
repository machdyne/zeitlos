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
#include "zpaths.h"
#include "zobj.h"
#include "zwm.h"
#include "zspeak.h"
#include "zwin.h"
#include "zgfx.h"
#include "zfont.h"
#include "zkbd.h"
#include "zwidget.h"
#include "zflist.h"
#include "zdialog.h"
#include "zedit.h"
#include "zfsapp.h"		// fs_stat(), for z_dialog_help()

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

// What a kind of dialog does with its window. Reached only through
// this table, so an app links the kinds it opens and no others: a
// help box (z_dialog_help()) does not pull in the file list, and an
// app with only confirm boxes does not pull in help. Section GC
// (docs/build.md) drops whatever no entry point points at.
typedef struct {
	void (*repaint)(void);
	void (*key)(uint32_t keysym, uint8_t mods);
	void (*mouse)(int cx, int cy, uint8_t buttons);
	void (*wheel)(int notches);
} dlg_ops_t;

static struct {

	z_win_t			win;
	const dlg_ops_t	*ops;
	const z_dialog_ctx_t	*ctx;
	dlg_kind_t		kind;

	bool			done;
	int				result;

	// -- open/save -- (the list itself is dlg_flist, below)
	// The text being typed. The buffer stays here -- z_edit_t does
	// not own storage, so an app can keep one in static state without
	// a heap -- and the editing lives in sw/common/zedit.c.
	char			field[DLG_FIELD_MAX];
	z_edit_t		edit;
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

// The open/save dialogs' file list: by far the largest thing here
// (some 7KB), so it stands apart from `dlg` -- an app that only shows
// confirm and help boxes then does not carry it, since section GC
// drops data nothing references as it drops code.
static z_flist_t dlg_flist;

// -- geometry --

#define DLG_MARGIN      6
#define DLG_LINE_H      (z_font_5x8.h + 2)
#define DLG_BTN_W       54
#define DLG_BTN_H       16
#define DLG_BTN_GAP     6
#define DLG_FIELD_H     14
// window width less content width: 2px inset each side (z_win_content_rect())
#define DLG_FRAME_W     4

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
static void dlg_parent(z_msg_t *msg);
static void dlg_mouse(int cx, int cy, uint8_t buttons);
static void dlg_activate_button(int act);
static void dlg_key(uint32_t keysym, uint8_t mods);

// -- the parent's messages --
//
// Everything addressed to the app's own window goes to ctx->on_msg.
// An app with no on_msg (one that only ever opens a help box, say, and
// has no handler factored out of its loop) still has a parent window
// that wm keeps talking to while the dialog is up: its visible region
// changes as the dialog opens and closes, and wm blocks on an ack for
// every redraw. So the dialog keeps that window's region itself and
// acks its redraws without painting -- the caller repaints once the
// dialog returns. Anything else for it is dropped:
// keys and the pointer are the dialog's while it is modal.
static void dlg_parent(z_msg_t *msg) {

	const z_dialog_ctx_t *ctx = dlg.ctx;

	if (!ctx) return;
	if (ctx->on_msg) { ctx->on_msg(msg, ctx->user); return; }
	if (!ctx->parent) return;

	// The parent is the caller's own window, given as const so that
	// centering on it cannot change it; keeping its region current is
	// what the caller's loop would have done with these two.
	z_win_t *pw = (z_win_t *)ctx->parent;

	if (msg->subject == Z_WM_SET_CLIP) {
		z_win_apply_clip(pw, &msg->obj);
	} else if (msg->subject == Z_WM_REDRAW && msg->obj.type == Z_UINT32 &&
		z_win_redraw_id(msg->obj.val.uint32) == pw->id) {
		z_win_apply_redraw(pw, msg->obj.val.uint32);
		z_win_redraw_done(pw);
	}

}

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
				if (dlg.ops) dlg.ops->repaint();
				z_win_redraw_done(&dlg.win);
			} else {
				dlg_parent(msg);
			}

			break;

		}

		case Z_WM_SET_CLIP:

			// z_win_apply_clip() answers "is this mine?" itself: the
			// payload names its window (Z_WM_CLIP_WINDOW, zwm.h) and
			// it refuses one addressed elsewhere without acking. So
			// try this window and pass on what it declines -- the
			// parent's region keeps arriving while the dialog is up,
			// and wm waits for an ack on both.
			//
			// Not optional for the dialog's own: a window that has
			// never been told a region draws nothing (win_use_clip(),
			// zwin.c), so a dialog that drops it comes up as an empty
			// frame with no buttons in it.
			if (!z_win_apply_clip(&dlg.win, &msg->obj))
				dlg_parent(msg);

			break;

		case Z_WM_WINDOW_MOVED: {

			z_obj_t *id = z_map_find(&msg->obj, "id");

			if (id && id->type == Z_INT32 && id->val.int32 == dlg.win.id)
				z_win_parse_rect(&dlg.win, &msg->obj);
			else
				dlg_parent(msg);

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
			} else {
				dlg_parent(msg);
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

			if (dlg.ops)
				dlg.ops->mouse(cx, cy,
					(uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(msg->obj.val.uint32));

			break;

		}

		case Z_WM_WHEEL:
			// The file list and the help text scroll; a prompt or a
			// confirm has nothing to scroll. Ours for the same reason
			// Z_WM_MOUSE is.
			if (msg->obj.type == Z_UINT32 && dlg.ops && dlg.ops->wheel)
				dlg.ops->wheel((int)Z_WM_WHEEL_NOTCHES(msg->obj.val.uint32));
			break;

		case Z_WM_KEY: {

			if (msg->obj.type != Z_UINT32) break;

			uint32_t packed = msg->obj.val.uint32;
			if (!Z_WM_UNPACK_KEY_PRESSED(packed)) break;	// ignore key-up

			if (dlg.ops)
				dlg.ops->key(Z_WM_UNPACK_KEY_KEYSYM(packed),
					(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(packed));

			break;

		}

		default:

			// Z_WM_WINDOW_RESIZED, Z_WM_TITLEBAR_ICON and anything
			// else all belong to the parent -- dialogs are neither
			// resizable nor equipped with extra titlebar icons, so
			// nothing addressed to this window can turn up here.
			dlg_parent(msg);

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
			dlg_parent(&msg);
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

	(void)user;		// dlg.ctx, already set

	dlg_parent(msg);

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

	// Confirm only. A prompt used to be here too, and that was a bug
	// every prompt in the system shared: with OK focused, the "Space
	// presses the focused button" rule in dlg_key() fired while the
	// person was TYPING in the field, so the first space submitted
	// whatever was typed so far. sw/apps/settings is where it was
	// found -- "port " came back as "port" and was rejected mid-edit.
	// A prompt starts in its field, like the file dialogs.
	bool kind_wants_button_focus = (dlg.kind == DLG_KIND_CONFIRM);


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

	// Confirm dialogs start with the affirmative button focused, so
	// Tab has an obvious starting point and Space works immediately.
	// Prompts and the file dialogs deliberately do NOT -- focus starts
	// in the field or the list there, which is where typing goes.
	if (kind_wants_button_focus)
		z_widget_focus_set(&dlg.wset, 0);

}

// -- the text field --
//
// Now sw/common/zedit.c, shared with sw/apps/web's URL bar. This
// file's own comment used to say the field was private "until a
// second caller appears" -- one has, and the version here could not
// scroll, so a filename wider than the box ran off the end of its own
// frame. Both callers get that fixed by sharing one widget.

static void field_set(const char *s) {
	// Re-binds the buffer as well as the text: dlg is reused across
	// dialogs and z_edit_t holds a pointer, so binding once at
	// startup would leave a stale one after the struct is cleared.
	z_edit_init(&dlg.edit, dlg.field, DLG_FIELD_MAX, s);
}

static void field_draw(void) {
	dlg.edit.focus = dlg.field_focus;
	z_edit_draw(&dlg.win, &dlg.edit, DLG_MARGIN, dlg.field_y,
		z_win_content_w(&dlg.win) - 2 * DLG_MARGIN, DLG_FIELD_H,
		&z_font_5x8);
}

static void field_key(uint32_t keysym) {
	if (z_edit_key(&dlg.edit, keysym)) field_draw();
}

// Puts the caret in a prompt's field and takes the focus ring off the
// buttons -- the one state in which typing edits the field.
static void prompt_focus_field(void) {
	dlg.field_focus = true;
	z_widget_focus_set(&dlg.wset, -1);
	z_widget_draw_all(&dlg.wset, false);
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
	const char *p = dlg_flist.path;
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
		z_flist_invalidate(&dlg_flist);
		z_flist_draw(&dlg_flist, true);

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
		if (z_flist_selected_is_dir(&dlg_flist)) {
			z_flist_key(&dlg_flist, 0x0d);		// Enter -- navigate
			path_draw();
			z_flist_draw(&dlg_flist, true);
			return;
		}

		if (!z_flist_selected_path(&dlg_flist, out, outlen)) return;

		dlg.result = Z_DIALOG_YES;
		dlg.done = true;

		return;

	}

	// -- save --

	if (dlg.edit.len == 0) return;		// nothing typed, nothing to do

	// Absolute names are taken as-is; anything else is relative to
	// the directory being browsed. That lets a user type a full path
	// into the field and have it mean what it says.
	int n = 0;

	if (dlg.edit.buf[0] != '/') {
		for (const char *s = dlg_flist.path; *s && n < outlen - 1; s++)
			out[n++] = *s;
		if (n > 0 && out[n - 1] != '/' && n < outlen - 1) out[n++] = '/';
	}

	for (int i = 0; i < dlg.edit.len && n < outlen - 1; i++)
		out[n++] = dlg.edit.buf[i];

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

static bool prompt_empty_ok;	// z_dialog_prompt_empty_ok(): OK with nothing typed

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
		if (act == 0 && (dlg.edit.len > 0 || prompt_empty_ok)) {
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

	// A prompt has a field and two buttons and nothing else. Clicking
	// the field puts the caret back in it; nothing else here is
	// clickable. It must return before the file list below: `dlg` is
	// static, so the list can still hold a directory from an earlier
	// Open or Save dialog in the same app, and a prompt has no business
	// handing it a click.
	if (dlg.kind == DLG_KIND_PROMPT) {
		int cw = z_win_content_w(&dlg.win);
		bool in_field = cx >= DLG_MARGIN && cx < cw - DLG_MARGIN &&
			cy >= dlg.field_y && cy < dlg.field_y + DLG_FIELD_H;
		if ((buttons & Z_MOUSE_BTN_LEFT) && in_field && !dlg.field_focus)
			prompt_focus_field();
		return;
	}

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

	int r = z_flist_mouse(&dlg_flist, cx, cy, buttons);

	if (r == Z_FLIST_NONE) return;

	// A directory activation has already navigated; repaint the path
	// line to match, and for save, leave the field alone (the name
	// being typed survives changing directory, which is what makes
	// "type the name, then go find the folder" work).
	if (r == Z_FLIST_ACTIVATED && z_flist_activated_dir(&dlg_flist)) {
		path_draw();
		return;
	}

	if (dlg.kind == DLG_KIND_SAVE) {

		// Selecting a file copies its name into the field, the way
		// every save dialog does -- it's the fastest way to overwrite
		// an existing file, and the user can still edit it.
		const char *name = z_flist_selected(&dlg_flist);
		if (name && !z_flist_selected_is_dir(&dlg_flist)) {
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
			z_flist_draw(&dlg_flist, true);

			return;

		}

		// A prompt cycles field -> OK -> Cancel -> field, the same shape
		// as the save dialog's field -> list -> buttons.
		if (dlg.kind == DLG_KIND_PROMPT) {

			if (dlg.field_focus) {
				dlg.field_focus = false;
				z_widget_focus_set(&dlg.wset, back ? dlg.widget_count - 1 : 0);
			} else {
				int next = dlg.wset.focused + (back ? -1 : 1);
				if (next < 0 || next >= dlg.widget_count) {
					prompt_focus_field();
					return;
				}
				z_widget_focus_set(&dlg.wset, next);
			}

			z_widget_draw_all(&dlg.wset, false);
			field_draw();
			return;

		}

		z_widget_focus_next(&dlg.wset, back);
		z_widget_draw_all(&dlg.wset, false);

		return;

	}

	// Space presses the focused button -- but never while a text field
	// has the caret, where it is a character. The file dialogs already
	// cleared the button focus when the field took it; a prompt now
	// does too, and the field check makes that impossible to get wrong
	// again from either side.
	// Enter keeps its own meaning per dialog kind below -- in a file
	// dialog it accepts the selection rather than pressing whatever
	// is focused, which is what the hand expects there.
	bool typing = dlg.field_focus &&
		(dlg.kind == DLG_KIND_PROMPT || dlg.kind == DLG_KIND_SAVE);

	if (keysym == ' ' && dlg.wset.focused >= 0 && !typing) {
		dlg_activate_button(dlg.wset.focused);
		return;
	}

	if (dlg.kind == DLG_KIND_PROMPT) {

		// A button has focus (Tab moved it there). Enter presses it,
		// as Space does. Typing a character goes back to the field
		// with that character, rather than being swallowed -- the
		// person plainly meant to type.
		if (!dlg.field_focus) {
			if (keysym == 0x0d) {
				dlg_activate_button(dlg.wset.focused);
				return;
			}
			if (keysym < 0x20 || keysym >= 0x7f) return;
			prompt_focus_field();
		}

		if (keysym == 0x0d) {
			if (dlg.edit.len > 0 || prompt_empty_ok) {
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
		z_flist_draw(&dlg_flist, true);
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

	int r = z_flist_key(&dlg_flist, keysym);

	if (r == Z_FLIST_NONE) return;

	path_draw();

	if (dlg.kind == DLG_KIND_SAVE) {
		const char *name = z_flist_selected(&dlg_flist);
		if (name && !z_flist_selected_is_dir(&dlg_flist)) {
			field_set(name);
			field_draw();
		}
	}

}

static void dlg_wheel(int notches) {
	if (dlg.kind == DLG_KIND_OPEN || dlg.kind == DLG_KIND_SAVE)
		z_flist_wheel(&dlg_flist, notches);
}

// Open, save, confirm and prompt: the handlers above.
static const dlg_ops_t std_ops = { dlg_repaint, dlg_key, dlg_mouse, dlg_wheel };

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
	dlg.ops = &std_ops;

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

	z_flist_init(&dlg_flist, &dlg.win);
	z_flist_set_geom(&dlg_flist, DLG_MARGIN, dlg.list_y,
		cw - 2 * DLG_MARGIN, dlg.list_h);
	// No directory given: the person's own (docs/layout.md, rule 5).
	z_flist_chdir(&dlg_flist, start_dir ? start_dir : Z_DIR_HOME);

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

// A dialog is the one moment the screen demands an answer, so it is
// the one moment speech must not be left to the user to go looking
// for: the title and the question are said as it opens. The buttons
// announce themselves as focus reaches them (zwidget.c).
static void speak_dialog(const char *title, const char *msg) {

	if (!z_speak_available()) return;

	char line[Z_SPEAK_SLOT_MAX];
	line[0] = 0;
	if (title) {
		z_speak_cat(line, sizeof(line), title);
		z_speak_cat(line, sizeof(line), ". ");
	}
	if (msg) z_speak_cat(line, sizeof(line), msg);
	z_speak(line, Z_TTS_F_INTERRUPT);

}

bool z_dialog_open(const z_dialog_ctx_t *ctx, const char *start_dir,
	char *out, int outlen) {

	if (!out || outlen < 2) return false;

	out[0] = 0;

	speak_dialog("Open", "Choose a file.");

	return file_dialog(ctx, DLG_KIND_OPEN, "Open", start_dir, NULL,
		labels_open, out, outlen);

}

bool z_dialog_save(const z_dialog_ctx_t *ctx, const char *start_dir,
	const char *suggested, char *out, int outlen) {

	if (!out || outlen < 2) return false;

	out[0] = 0;

	speak_dialog("Save as", "Type a name.");

	return file_dialog(ctx, DLG_KIND_SAVE, "Save As", start_dir, suggested,
		labels_save, out, outlen);

}

// Buttons, field and focus for a prompt -- separate from
// z_dialog_prompt() so the host test (sw/common/tests/test_dialog.c)
// sets a prompt up exactly the way the real one is, without a window.
static void prompt_setup(const char *initial) {

	dlg_buttons_init(labels_ok_cancel, 2);

	field_set(initial);
	dlg.field_focus = true;		// typing is the entire point

}

static bool prompt_secret;

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
	dlg.ops = &std_ops;

	dlg.msg = msg;
	dlg.button_set = Z_DIALOG_OK_CANCEL;

	int ch = z_win_content_h(&dlg.win);
	dlg.btn_y = ch - DLG_MARGIN - DLG_BTN_H;
	dlg.field_y = dlg.btn_y - DLG_MARGIN - DLG_FIELD_H;

	prompt_setup(initial);
	dlg.edit.secret = prompt_secret;

	dlg_repaint();

	bool ok = dlg_run() == Z_DIALOG_YES;

	int i = 0;
	if (ok)
		for (; i < outlen - 1 && dlg.edit.buf[i]; i++) out[i] = dlg.edit.buf[i];
	out[i] = 0;

	// A password must not outlive the dialog in its buffer.
	if (prompt_secret) {
		volatile char *f = dlg.field;
		for (int k = 0; k < DLG_FIELD_MAX; k++) f[k] = 0;
		dlg.edit.len = dlg.edit.cur = 0;
	}

	return prompt_empty_ok ? ok : out[0] != 0;

}

bool z_dialog_prompt_empty_ok(const z_dialog_ctx_t *ctx, const char *title,
	const char *msg, const char *initial, char *out, int outlen) {
	bool ok;
	prompt_empty_ok = true;
	ok = z_dialog_prompt(ctx, title, msg, initial, out, outlen);
	prompt_empty_ok = false;
	return ok;
}

bool z_dialog_prompt_secret(const z_dialog_ctx_t *ctx, const char *title,
	const char *msg, char *out, int outlen) {
	bool ok;
	prompt_secret = true;
	ok = z_dialog_prompt(ctx, title, msg, NULL, out, outlen);
	prompt_secret = false;
	return ok;
}

int z_dialog_confirm(const z_dialog_ctx_t *ctx, const char *title,
	const char *msg, int buttons) {

	speak_dialog(title, msg);

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
	dlg.ops = &std_ops;

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

// -- help (F1) -- zdialog.h, docs/app_help.md --
//
// A fixed-width box of the app's own text, word-wrapped, with a
// scrollbar when it is longer than the box; Close, and "Read docs"
// beside it when `read` and the app's manual are both there.
//
// The text is the app's: a few lines on what it is for, its keys, and
// how to do the main thing. It lives in the app so that F1 answers on
// a system with no card at all (no /docs, no read), and it is short so
// that it costs the app little. The manual is the long version.
//
// The layout is two rules, both cheap enough to re-run on every paint
// (a help text is a kilobyte or two):
//   '\n' ends a line; an empty line is a gap between paragraphs.
//   '\t' moves to column HELP_TAB, and a line that wraps after one
//        continues at that column -- so "Ctrl+S\tSave the file" lines
//        up as a two-column table of keys without any table code.

#define HELP_W			300		// the whole window: one width for every app
#define HELP_ROWS		16		// at most this many lines show; more scroll
#define HELP_TAB		12		// columns
#define HELP_COLS_MAX	60		// the most a 300px line holds, and then some

static const char *const labels_help_docs[2] = { "Read docs", "Close" };
static const char *const labels_help[1]      = { "Close" };

static struct {
	const char		*text;
	const char		*doc;		// non-NULL: the "Read docs" button is there
	int				cols, rows, total, top;
	int				sb_on;
	z_scrollbar_t	sb;
} help;

// One display line from `p` into `out` (cols wide at most, NUL-ended);
// returns where the next starts. `*hang` carries a tab's column over a
// wrap and is cleared at the end of a source line.
static const char *help_next(const char *p, int cols, int *hang, char *out) {

	int n = 0, brk_n = -1;
	const char *brk_p = NULL;

	while (n < *hang) out[n++] = ' ';

	while (*p && *p != '\n') {

		if (n >= cols) {
			if (brk_p) { n = brk_n; p = brk_p; }
			while (*p == ' ') p++;
			while (n > *hang && out[n - 1] == ' ') n--;
			out[n] = 0;
			return p;
		}

		if (*p == '\t') {
			int to = (n < HELP_TAB) ? HELP_TAB : n + 1;
			while (n < to && n < cols) out[n++] = ' ';
			*hang = n;
			brk_n = n; brk_p = ++p;
			continue;
		}

		if (*p == ' ') { brk_n = n; brk_p = p + 1; }
		out[n++] = *p++;

	}

	out[n] = 0;
	if (*p == '\n') p++;
	*hang = 0;
	return p;

}

static int help_count(int cols) {
	char line[HELP_COLS_MAX + 1];
	int n = 0, hang = 0;
	for (const char *p = help.text; *p; n++) p = help_next(p, cols, &hang, line);
	return n;
}

static void help_draw_text(void) {

	char line[HELP_COLS_MAX + 1];
	int hang = 0, i = 0;
	const char *p = help.text;

	z_win_fill_rect(&dlg.win, DLG_MARGIN, DLG_MARGIN,
		help.cols * z_font_5x8.w, help.rows * DLG_LINE_H, 0);

	for (; *p && i < help.top + help.rows; i++) {
		p = help_next(p, help.cols, &hang, line);
		if (i >= help.top)
			z_win_draw_text(&dlg.win, DLG_MARGIN,
				DLG_MARGIN + (i - help.top) * DLG_LINE_H, line, 1, &z_font_5x8);
	}

}

static void help_repaint(void) {
	z_win_clear(&dlg.win);
	help_draw_text();
	if (help.sb_on) z_scrollbar_draw(&help.sb, true);
	z_widget_draw_all(&dlg.wset, true);
}

static void help_scroll(int to) {
	if (!help.sb_on || !z_scrollbar_set_value(&help.sb, to)) return;
	help.top = help.sb.value;
	help_draw_text();
	z_scrollbar_draw(&help.sb, false);
}

// "/docs/<doc>.md" into `out`, or false if it does not fit.
static bool help_doc_path(const char *doc, char *out, int cap) {
	const char *parts[3] = { Z_DIR_DOCS "/", doc, ".md" };
	int n = 0;
	for (int i = 0; i < 3; i++)
		for (const char *p = parts[i]; *p; p++) {
			if (n >= cap - 1) return false;
			out[n++] = *p;
		}
	out[n] = 0;
	return true;
}

static void help_activate(int act) {

	if (act == 0 && help.doc) {
		char path[Z_WM_ARG_MAX];
		if (help_doc_path(help.doc, path, sizeof(path))) {
			z_launch_arg_set(path);
			z_proc_run("read");
		}
	}

	dlg.done = true;

}

static void help_key(uint32_t keysym, uint8_t mods) {

	int page = help.rows > 1 ? help.rows - 1 : 1;

	switch (keysym) {
		case Z_KEY_UP:       help_scroll(help.top - 1); return;
		case Z_KEY_DOWN:     help_scroll(help.top + 1); return;
		case Z_KEY_PAGEUP:   help_scroll(help.top - page); return;
		case Z_KEY_PAGEDOWN: help_scroll(help.top + page); return;
		case Z_KEY_HOME:     help_scroll(0); return;
		case Z_KEY_END:      help_scroll(help.total); return;
		case '\t':
			z_widget_focus_next(&dlg.wset, (mods & Z_KBD_MOD_SHIFT) != 0);
			z_widget_draw_all(&dlg.wset, false);
			return;
		case ' ':
			if (help.sb_on && dlg.wset.focused < 0) {
				help_scroll(help.top + page);
				return;
			}
			// fall through: Space presses the focused button
		case 0x0d:
			// Enter on Close, or with nothing focused, closes. It never
			// starts read unless "Read docs" is what has the focus.
			help_activate(dlg.wset.focused);
			return;
		case 0x1b:
		case Z_KEY_HELP:		// the key that opened it also closes it
			dlg.done = true;
			return;
		default:
			return;
	}

}

static void help_mouse(int cx, int cy, uint8_t buttons) {

	int act = z_widget_mouse(&dlg.wset, cx, cy, buttons);

	if (act >= 0) { help_activate(act); return; }
	if (dlg.wset.pressed >= 0) return;

	if (help.sb_on && z_scrollbar_mouse(&help.sb, cx, cy, buttons)) {
		help.top = help.sb.value;
		help_draw_text();
	}

}

static void help_wheel(int notches) {
	help_scroll(help.top - 3 * notches);		// up (positive) is back
}

static const dlg_ops_t help_ops = { help_repaint, help_key, help_mouse, help_wheel };

// The text area's width in columns, for a content area `cw` wide.
static int help_cols_for(int cw, bool sb) {
	int c = (cw - 2 * DLG_MARGIN - (sb ? Z_SB_THICK + 2 : 0)) / z_font_5x8.w;
	return c > HELP_COLS_MAX ? HELP_COLS_MAX : c;
}

// Sizes the box for `text` in a content area `cw` wide: columns,
// lines, rows shown, and whether it scrolls. Separate from
// z_dialog_help() so the host test lays it out exactly as the real one.
static void help_layout(const char *text, int cw) {

	help.text = text ? text : "";
	help.top = 0;
	help.sb_on = 0;
	help.cols = help_cols_for(cw, false);
	help.total = help_count(help.cols);

	if (help.total > HELP_ROWS) {
		help.sb_on = 1;
		help.cols = help_cols_for(cw, true);
		help.total = help_count(help.cols);
	}

	help.rows = help.total < HELP_ROWS ? help.total : HELP_ROWS;
	if (help.rows < 1) help.rows = 1;

}

void z_dialog_help(const z_dialog_ctx_t *ctx, const char *title,
	const char *text, const char *doc) {

	// "Read docs" only where it would work: the manual is on the card
	// (a system running from flash has no /docs) and read is installed.
	char path[Z_WM_ARG_MAX];
	z_fs_info_t fi;
	const char *with_doc = NULL;

	if (doc && help_doc_path(doc, path, sizeof(path)) &&
		z_exec_exists("read") && fs_stat(path, &fi))
		with_doc = doc;

	speak_dialog(title, text);

	// Laid out for the content width before the window exists, to know
	// its height; z_win_content_w() of the made window is the same
	// number (the frame is DLG_FRAME_W across, as the confirm box
	// assumes).
	help_layout(text, HELP_W - DLG_FRAME_W);
	help.doc = with_doc;

	int h = Z_WM_TITLEBAR_H + 4 + DLG_MARGIN + help.rows * DLG_LINE_H +
		DLG_MARGIN + DLG_BTN_H + DLG_MARGIN;

	if (!dlg_create(ctx, DLG_KIND_CONFIRM, title ? title : "Help", HELP_W, h))
		return;
	dlg.ops = &help_ops;

	int cw = z_win_content_w(&dlg.win);
	if (cw != HELP_W - DLG_FRAME_W) help_layout(text, cw);

	if (help.sb_on) {
		z_scrollbar_init(&help.sb, &dlg.win, Z_SB_VERT);
		z_scrollbar_set_geom(&help.sb, cw - Z_SB_THICK - 2, DLG_MARGIN,
			help.rows * DLG_LINE_H);
		z_scrollbar_set_range(&help.sb, help.total, help.rows);
	}

	dlg.btn_y = z_win_content_h(&dlg.win) - DLG_MARGIN - DLG_BTN_H;

	if (with_doc) dlg_buttons_init(labels_help_docs, 2);
	else dlg_buttons_init(labels_help, 1);

	// Close has the focus, so Enter and Space close; reading the manual
	// is a deliberate Tab or click away.
	z_widget_focus_set(&dlg.wset, dlg.widget_count - 1);

	help_repaint();
	dlg_run();

}
