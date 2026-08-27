#ifndef ZDIALOG_H
#define ZDIALOG_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Modal dialog boxes -- open, save, confirm.
 *
 * -- what a dialog actually is here --
 *
 * There is no dialog server and no special kind of window. A dialog
 * is an ORDINARY window, created by the app itself, with
 * Z_WIN_FLAG_MODAL set (zwm.h). The app opens it, runs a message loop
 * inside these functions until the user answers, destroys the window,
 * and carries on. Nothing else in the system needs to know dialogs
 * exist.
 *
 * That makes each of these a BLOCKING call: z_dialog_save() does not
 * return until the user has picked a filename or cancelled. Which is
 * what you want at the call site -- "if (z_dialog_save(...)) write the
 * file;" is the whole of the logic -- but it has one consequence the
 * caller cannot ignore, below.
 *
 * -- the parent window keeps running --
 *
 * While a dialog is up, wm carries on sending the app messages about
 * its OTHER window: Z_WM_REDRAW when something uncovers it, and so
 * on. Those cannot simply be dropped. wm blocks waiting for a redraw
 * ack (docs/window_manager.md, "Content z-order") and an app that
 * stops acking freezes the whole screen until REDRAW_ACK_TIMEOUT
 * fires.
 *
 * So a dialog needs to know how to hand those back. That's what
 * z_dialog_ctx_t's on_msg callback is: everything not addressed to
 * the dialog goes to it, and it is expected to do exactly what the
 * app's normal message loop would have done -- repaint on
 * Z_WM_REDRAW, then z_win_redraw_done(). In practice an app factors
 * one function out of its main loop and points both at it. See
 * sw/apps/text for the pattern.
 *
 * Routing works out like this, and it's worth knowing why each case
 * can be decided at all:
 *
 *   Z_WM_REDRAW            by window id -- the payload has always
 *                          carried one (Z_WM_PACK_XY), it just took
 *                          a second window to need it.
 *                          z_win_redraw_id() (zwin.h) reads it.
 *   Z_WM_WINDOW_MOVED,
 *   Z_WM_WINDOW_RESIZED,
 *   Z_WM_CLOSE,
 *   Z_WM_TITLEBAR_ICON     by the window id in the payload/map.
 *   Z_WM_MOUSE             by coordinates -- the payload is absolute
 *                          screen position and we know the dialog's
 *                          rect.
 *   Z_WM_KEY               ALWAYS the dialog's. This one has no
 *                          window id and no coordinates, so there is
 *                          nothing to route by -- which is precisely
 *                          why dialogs are modal rather than merely
 *                          on top. See Z_WIN_FLAG_MODAL in zwm.h.
 *
 * -- storage --
 *
 * These allocate nothing. The file list inside the open/save dialogs
 * is a z_flist_t (zflist.h) in this file's own .bss, shared between
 * the two since only one dialog can be open at a time. That costs
 * around 3.5KB of the app's image and is the reason it's shared
 * rather than one per dialog.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zwin.h"

// Everything a dialog needs to know about the app that opened it.
typedef struct {

	// the window to center on. May be NULL, in which case the dialog
	// centers on the screen instead.
	const z_win_t	*parent;

	// Where messages that aren't the dialog's own go. MUST at
	// minimum handle Z_WM_REDRAW for the parent and ack it -- see
	// this header's own comment above on what happens otherwise.
	// May be NULL only for an app that has no other window.
	void (*on_msg)(z_msg_t *msg, void *user);

	void		*user;

} z_dialog_ctx_t;

// z_dialog_confirm() button sets
#define Z_DIALOG_OK_CANCEL       0
#define Z_DIALOG_YES_NO          1
#define Z_DIALOG_YES_NO_CANCEL   2

// z_dialog_confirm() results. Cancel is 0 so that the common "did
// they agree?" test is just `if (z_dialog_confirm(...))`, and so that
// every way of backing out -- the Cancel button, Escape, the
// titlebar close icon -- lands on the same value without the caller
// enumerating them.
#define Z_DIALOG_CANCEL   0
#define Z_DIALOG_YES      1		// also OK
#define Z_DIALOG_NO       2

// File-open dialog. Browses the filesystem; on OK, writes the chosen
// file's full path into `out` and returns true. Returns false if the
// user cancelled.
//
// `start_dir` (may be NULL for the root) is where browsing begins.
bool z_dialog_open(const z_dialog_ctx_t *ctx, const char *start_dir,
	char *out, int outlen);

// File-save dialog. Same browser plus an editable filename field,
// pre-filled with `suggested` (may be NULL). On OK, writes the full
// path into `out` and returns true.
//
// Does NOT check whether the file already exists or ask about
// overwriting -- that is the caller's decision to make and its
// z_dialog_confirm() to show, since only the caller knows whether
// overwriting is a problem in its case.
bool z_dialog_save(const z_dialog_ctx_t *ctx, const char *start_dir,
	const char *suggested, char *out, int outlen);

// Message box with 2 or 3 buttons. `title` appears in the titlebar,
// `msg` in the body -- embedded '\n' starts a new line, and the
// dialog grows to fit up to Z_DIALOG_MSG_LINES of them.
//
// Returns one of Z_DIALOG_YES/_NO/_CANCEL. Escape and the titlebar
// close icon both give the least destructive answer available:
// Z_DIALOG_CANCEL where the button set has one, Z_DIALOG_NO
// otherwise.
int z_dialog_confirm(const z_dialog_ctx_t *ctx, const char *title,
	const char *msg, int buttons);

// Single-line text prompt -- a message, an editable field, OK and
// Cancel. Returns true and writes the entered text into `out` if the
// user accepted, false if they cancelled or left the field empty.
//
// `initial` (may be NULL) pre-fills the field. `msg` follows
// z_dialog_confirm()'s rules: split on '\n' only, up to
// Z_DIALOG_MSG_LINES lines.
//
// Added for the file browser's New Folder button, so that creating a
// directory doesn't require a permanently-visible text box on the
// browser window itself for something used once in a while.
bool z_dialog_prompt(const z_dialog_ctx_t *ctx, const char *title,
	const char *msg, const char *initial, char *out, int outlen);

#define Z_DIALOG_MSG_LINES   4

#endif
