/*
 * files -- a file browser
 *
 * Browse the filesystem, open a file with whatever app handles its
 * type, make directories, delete things.
 *
 *   > run wm
 *   > run files
 *
 * Most of this app is the z_flist_t widget (sw/common/zflist.h), which
 * was factored out of the file dialogs on exactly the assumption that
 * a browser would want the same thing: rows with an icon, keyboard and
 * mouse selection, its own scrollbar, and directory navigation. This
 * file is the window around it, three buttons, and the decision about
 * what double-clicking something means.
 *
 * -- opening a file --
 *
 * By extension, through the shared table in sw/common/ztype.h. TXT,
 * ASC and MD open in `text`; ZBM opens in `draw`. A file with no
 * extension is assumed to be a program, and that assumption is
 * CHECKED -- see the note in launch() below, because getting it wrong
 * means executing a data file.
 *
 * The filename is handed to the launched app through wm's pending
 * launch argument (Z_WM_SET_ARG, zwm.h): there is no argv, and one
 * slot in wm is enough because only one app is ever mid-launch at a
 * time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"	// Z_TICK_HZ, for the idle wait in main()
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/ztype.h"

// Sized so the list shows a useful number of rows and the three
// buttons fit across the bottom without crowding -- that second one
// is the binding constraint, and it is why this is the floor rather
// than merely the initial size (Z_WIN_FLAG_MIN_IS_CREATE, zwm.h).
#define WIN_W   300
#define WIN_H   260

static z_win_t win;

static z_flist_t flist;
static z_dialog_ctx_t dlg_ctx;

// -- buttons --

#define BTN_OPEN    0
#define BTN_MKDIR   1
#define BTN_DELETE  2
#define BTN_COUNT   3

#define BTN_W       78
#define BTN_H       16
#define BTN_GAP     4
#define MARGIN      4

static z_widget_t widgets[BTN_COUNT];
static z_widget_set_t wset;

static const char *const btn_labels[BTN_COUNT] = {
	"Open", "New Folder", "Delete"
};

// content-relative layout, recomputed on every resize
static int list_y, list_h, btn_y;

// -- keyboard focus --
//
// Keyboard-only operation is a first-class case in this system (see
// docs/window_manager.md), so every control here has to be reachable
// with Tab. That spans two different things -- the file list, which
// is not a z_widget_t at all, and the three buttons, which are -- so
// the tab order lives here rather than in either of them.
//
// FOCUS_LIST is the list; anything else is a widget index. Tab cycles
// list -> Open -> New Folder -> Delete -> list.
#define FOCUS_LIST  (-1)

static int focus = FOCUS_LIST;

// Draws the list's own focus indicator: a ring just outside its
// frame, matching the one z_widget_draw() puts around a focused
// button. Without it there is no way to tell whether typing an arrow
// key will move the selection or do nothing.
static void draw_list_focus(void) {

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x0 = c.x0 + flist.x - 1;
	int y0 = c.y0 + flist.y - 1;
	int x1 = x0 + flist.w + 1;
	int y1 = y0 + flist.h + 1;

	z_fb_hw_box(x0, y0, x1, y1, focus == FOCUS_LIST ? 1 : 0, &c);

}

static void focus_move(bool backward) {

	if (focus == FOCUS_LIST) {
		// leaving the list -- into the first (or last) button
		z_widget_focus_set(&wset, backward ? BTN_COUNT - 1 : 0);
		focus = wset.focused;
	} else if ((backward && focus == 0) ||
		(!backward && focus == BTN_COUNT - 1)) {
		// off the end of the buttons -- back to the list
		z_widget_focus_set(&wset, -1);
		focus = FOCUS_LIST;
	} else {
		z_widget_focus_next(&wset, backward);
		focus = wset.focused;
	}

	z_widget_draw_all(&wset, false);
	draw_list_focus();

}

static void forward_msg(z_msg_t *msg, void *user);

// ---------------------------------------------------------------
// layout and painting
// ---------------------------------------------------------------

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	// Bottom-up: the buttons have a fixed size and sit on the bottom
	// margin, the list gets everything above them. The other way
	// round is how a window ends up with its buttons half off the
	// edge at small sizes.
	btn_y = ch - MARGIN - BTN_H;

	// Clear of the resize grip's corner -- see Z_WIN_GRIP_INSET in
	// zwm.h. The buttons are laid out from the LEFT precisely so the
	// grip's corner stays free; right-aligning them the way a dialog
	// does would put the last one underneath it.
	list_y = MARGIN + z_font_5x8.h + 3;
	list_h = btn_y - MARGIN - list_y;
	if (list_h < 0) list_h = 0;

	z_flist_set_geom(&flist, MARGIN, list_y, cw - 2 * MARGIN, list_h);

	for (int i = 0; i < BTN_COUNT; i++) {
		widgets[i].x = (int16_t)(MARGIN + i * (BTN_W + BTN_GAP));
		widgets[i].y = (int16_t)btn_y;
	}

	z_widget_invalidate(&wset);

}

// The current directory, above the list. Elided from the LEFT when it
// doesn't fit: the interesting end of a path is the last component.
static void path_draw(void) {

	z_clip_t content;
	z_win_content_rect(&win, &content);

	int cw = z_win_content_w(&win);
	int avail = cw - 2 * MARGIN;
	if (avail <= 0) return;

	int maxch = avail / z_font_5x8.w;
	const char *p = flist.path;
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
	clip.x0 = content.x0 + MARGIN;
	clip.y0 = content.y0 + MARGIN;
	clip.x1 = content.x0 + cw - MARGIN - 1;
	clip.y1 = clip.y0 + z_font_5x8.h - 1;

	if (clip.x1 < clip.x0) return;

	z_fb_hw_fill_rect(clip.x0, clip.y0,
		clip.x1 - clip.x0 + 1, z_font_5x8.h, 0);

	z_fb_draw_text(clip.x0, clip.y0, shown, 1, &z_font_5x8, &clip);

}

static void repaint(void) {

	// Clear everything first. wm clears before most redraws but NOT
	// after a move (repair_drag() in wm.c excludes the window's own
	// final footprint), so anything not actively rewritten keeps its
	// pre-move contents.
	//
	// NOT z_win_clear(), which blanks the content area all the way
	// into the bottom-right corner. Most of the resize grip falls
	// INSIDE the content area -- all but its outer 2px, see
	// Z_WIN_GRIP_INSET in zwm.h -- and wm draws the grip as chrome at
	// create time without redrawing it afterwards, so clearing over
	// it left the corner half-drawn until the next resize. That was
	// visible for as long as this window has been resizable.
	//
	// Two rects instead: everything above the grip's row band at full
	// width, then the band to the left of the grip. Same exclusion
	// sw/apps/text's repaint() makes for the same reason.
	{
		int cw = z_win_content_w(&win);
		int ch = z_win_content_h(&win);

		int above_grip = ch - Z_WIN_GRIP_INSET;
		if (above_grip < 0) above_grip = 0;

		z_win_fill_rect(&win, 0, 0, cw, above_grip, 0);

		int left_of_grip = cw - Z_WIN_GRIP_INSET;
		if (left_of_grip > 0)
			z_win_fill_rect(&win, 0, above_grip, left_of_grip,
				Z_WIN_GRIP_INSET, 0);
	}

	path_draw();

	z_flist_invalidate(&flist);
	z_flist_draw(&flist, true);

	z_widget_draw_all(&wset, true);
	draw_list_focus();

}

// ---------------------------------------------------------------
// actions
// ---------------------------------------------------------------

// Launches whatever handles `path`.
//
// The filename goes through wm's pending launch argument rather than
// any kind of argv, which does not exist -- see Z_WM_SET_ARG in
// zwm.h. Set it BEFORE z_proc_run(): the new process can reach its
// own startup before this one runs again, and it claims the argument
// there.
static void launch(const char *path) {

	const char *app = z_ftype_app_for(path);

	if (app) {
		z_launch_arg_set(path);
		if (!z_proc_run(app))
			z_dialog_confirm(&dlg_ctx, "Launch failed",
				"That application could\nnot be started.", Z_DIALOG_OK_CANCEL);
		return;
	}

	// No extension: it MIGHT be a program. z_ftype_is_executable()
	// actually reads the ZEXE magic rather than trusting the absence
	// of an extension, and that check is not optional -- the loader
	// treats a file without the magic as the legacy raw executable
	// format and will happily load and jump into it (zexec.h,
	// "Backward compatibility"). Without this, double-clicking a
	// README would execute it.
	if (!z_ftype_ext(path)) {

		if (z_ftype_is_executable(path)) {
			if (!z_proc_run(path))
				z_dialog_confirm(&dlg_ctx, "Launch failed",
					"That program could not\nbe started.", Z_DIALOG_OK_CANCEL);
		} else {
			z_dialog_confirm(&dlg_ctx, "Can't open",
				"That file is not a\nprogram.", Z_DIALOG_OK_CANCEL);
		}

		return;

	}

	// A known extension with nothing registered for it. Said out
	// loud, because a double-click that silently does nothing reads
	// as the app being broken.
	z_dialog_confirm(&dlg_ctx, "Can't open",
		"No application is\nassociated with this file.", Z_DIALOG_OK_CANCEL);

}

static void do_open(void) {

	// A directory opens by descending into it, which is what the
	// widget's own Enter handling already does.
	if (z_flist_selected_is_dir(&flist)) {
		z_flist_key(&flist, 0x0d);
		path_draw();
		z_flist_draw(&flist, true);
		return;
	}

	char path[Z_FLIST_PATH_MAX];
	if (!z_flist_selected_path(&flist, path, sizeof(path))) return;

	launch(path);

}

static void do_mkdir(void) {

	char name[32];

	if (!z_dialog_prompt(&dlg_ctx, "New Folder",
		"Name for the new folder:", "", name, sizeof(name)))
		return;

	// Built against the directory currently being browsed. A name
	// containing a '/' would create something elsewhere, or fail
	// confusingly -- refuse it rather than trying to interpret it.
	for (const char *p = name; *p; p++) {
		if (*p == '/') {
			z_dialog_confirm(&dlg_ctx, "Bad name",
				"A folder name cannot\ncontain '/'.", Z_DIALOG_OK_CANCEL);
			return;
		}
	}

	char path[Z_FLIST_PATH_MAX];
	int n = 0;

	for (const char *p = flist.path; *p && n < (int)sizeof(path) - 1; p++)
		path[n++] = *p;
	if (n > 0 && path[n - 1] != '/' && n < (int)sizeof(path) - 1)
		path[n++] = '/';
	for (const char *p = name; *p && n < (int)sizeof(path) - 1; p++)
		path[n++] = *p;
	path[n] = 0;

	if (!fs_mkdir(path)) {
		z_dialog_confirm(&dlg_ctx, "Failed",
			"The folder could not\nbe created.", Z_DIALOG_OK_CANCEL);
		return;
	}

	z_flist_refresh(&flist);
	repaint();

}

static void do_delete(void) {

	const char *name = z_flist_selected(&flist);
	if (!name) return;			// nothing selected, or the ".." row

	char path[Z_FLIST_PATH_MAX];
	if (!z_flist_selected_path(&flist, path, sizeof(path))) return;

	bool isdir = z_flist_selected_is_dir(&flist);

	// The name goes in the question. "Delete this file?" is a
	// question the user cannot actually check the answer to.
	char msg[96];
	int n = 0;

	const char *lead = isdir ? "Delete the folder\n" : "Delete the file\n";
	for (const char *p = lead; *p && n < (int)sizeof(msg) - 2; p++)
		msg[n++] = *p;
	for (const char *p = name; *p && n < (int)sizeof(msg) - 2; p++)
		msg[n++] = *p;
	if (n < (int)sizeof(msg) - 1) msg[n++] = '?';
	msg[n] = 0;

	if (z_dialog_confirm(&dlg_ctx, "Delete", msg, Z_DIALOG_YES_NO)
		!= Z_DIALOG_YES)
		return;

	if (!fs_unlink(path)) {
		// fs_unlink() removes an empty directory but not one with
		// anything in it, and cannot tell us which failure this was
		// -- so the message covers both rather than guessing.
		z_dialog_confirm(&dlg_ctx, "Failed",
			isdir ? "The folder could not be\ndeleted. Is it empty?"
			      : "The file could not\nbe deleted.",
			Z_DIALOG_OK_CANCEL);
		return;
	}

	z_flist_refresh(&flist);
	repaint();

}

// ---------------------------------------------------------------
// input
// ---------------------------------------------------------------

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	int act = z_widget_mouse(&wset, cx, cy, buttons);

	if (act >= 0) {
		switch (act) {
			case BTN_OPEN:   do_open(); break;
			case BTN_MKDIR:  do_mkdir(); break;
			case BTN_DELETE: do_delete(); break;
			default: break;
		}
		return;
	}

	// A click on a button moved widget focus (z_widget_mouse does
	// that); keep this app's own tab position in step with it.
	if (wset.focused >= 0 && focus != wset.focused) {
		focus = wset.focused;
		draw_list_focus();
	}

	// a widget gesture in progress owns the pointer
	if (wset.pressed >= 0) return;

	// A click in the list moves focus back to it, so Tab resumes from
	// where the user is looking rather than from wherever it was.
	if (focus != FOCUS_LIST && (buttons & Z_MOUSE_BTN_LEFT) &&
		cy >= list_y && cy < list_y + list_h) {
		focus = FOCUS_LIST;
		z_widget_focus_set(&wset, -1);
		z_widget_draw_all(&wset, false);
		draw_list_focus();
	}

	int r = z_flist_mouse(&flist, cx, cy, buttons);

	if (r == Z_FLIST_ACTIVATED) {

		// A directory activation has already navigated -- the widget
		// does that itself, which is why z_flist_activated_dir()
		// exists rather than re-reading the selection.
		if (z_flist_activated_dir(&flist)) {
			path_draw();
			return;
		}

		char path[Z_FLIST_PATH_MAX];
		if (z_flist_selected_path(&flist, path, sizeof(path)))
			launch(path);

	}

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	// Tab moves between the list and the buttons; Shift+Tab goes
	// back. This is the whole reason the buttons are reachable at all
	// without a pointer.
	if (keysym == '\t') {
		focus_move((mods & Z_KBD_MOD_SHIFT) != 0);
		return;
	}

	if (focus != FOCUS_LIST) {

		// A focused button takes Enter and Space, and nothing else --
		// arrows deliberately do NOT move between buttons, because
		// they would then be unavailable for the list, which is where
		// they are far more useful. Tab is the only way across.
		if (keysym == 0x0d || keysym == ' ') {

			int act = z_widget_key_activate(&wset);

			switch (act) {
				case BTN_OPEN:   do_open(); break;
				case BTN_MKDIR:  do_mkdir(); break;
				case BTN_DELETE: do_delete(); break;
				default: break;
			}

			// Everything above can repaint, which clears the rings.
			draw_list_focus();
			z_widget_draw_all(&wset, true);

		}

		return;

	}

	if (keysym == 0x0d) {		// Enter -- same as the Open button
		do_open();
		draw_list_focus();
		return;
	}

	int r = z_flist_key(&flist, keysym);

	if (r != Z_FLIST_NONE) path_draw();

}

// ---------------------------------------------------------------

static void widgets_init(void) {

	memset(widgets, 0, sizeof(widgets));

	for (int i = 0; i < BTN_COUNT; i++) {
		widgets[i].type = Z_WIDGET_BUTTON;
		widgets[i].w = BTN_W;
		widgets[i].h = BTN_H;
		widgets[i].label = btn_labels[i];
		widgets[i].enabled = true;
	}

	z_widget_set_init(&wset, widgets, BTN_COUNT, &win);

}

// Handles messages belonging to this app's own window, from both the
// main loop and -- through z_dialog_ctx_t -- from inside any dialog
// that is open. See zdialog.h for why both must go through one
// function.
static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		// The part of this window not covered by the windows in
		// front of it. Confines every subsequent draw to it -- see
		// z_win_apply_clip() in zwin.c. The ack it sends is not
		// optional: wm waits for it when a region narrows.
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:
			z_win_apply_resized(&win, &msg->obj);
			layout();
			break;

		default:
			break;

	}

}

int main(void) {

	printf("files: starting\n");

	// CLOSE_ICON without CLOSE_KILLS_OWNER: this app owns more than
	// one window at a time (a dialog is a window), and the killing
	// form takes every window of a pid down the instant any one is
	// clicked closed. See that flag's warning in zwm.h.
	if (z_win_create_flags(&win, "files", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		Z_WIN_FLAG_MIN_IS_CREATE) != Z_OK) {
		printf("files: failed to create window -- is wm running?\n");
		return 1;
	}

	widgets_init();

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	z_flist_init(&flist, &win);

	layout();

	// A directory to start in, if something launched us with one.
	// Claimed even when unused, so it can't be left pending for
	// whatever the user opens next (Z_WM_SET_ARG, zwm.h).
	{
		char arg[Z_FLIST_PATH_MAX];
		if (!z_launch_arg_take(arg, sizeof(arg))) arg[0] = 0;
		z_flist_chdir(&flist, arg[0] ? arg : "/");
	}

	repaint();

	for (;;) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:

					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_MOUSE:

					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);

					break;

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id) {
						z_win_destroy(&win);
						printf("files: exiting\n");
						exit(0);
					}

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		for (volatile int i = 0; i < 200; i++);	// light throttle

	
		/* Yield. This loop used to spin, so the app was RUNNABLE
		 * forever and took a full scheduler share from whatever was
		 * in the foreground -- see docs/app_runtime.md. a file browser only moves when you tell it to. */
		z_proc_wait(Z_TICK_HZ / 30);
	}

	return 0;

}
