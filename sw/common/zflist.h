#ifndef ZFLIST_H
#define ZFLIST_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * A scrolling, selectable directory listing that draws itself into a
 * rectangle of an app's window.
 *
 * -- why this is its own file --
 *
 * It is a widget and it could have lived in zwidget.c, but it needs
 * the filesystem (zfsapp.h), and zwidget.c is linked by apps that
 * have no business dragging a filesystem dependency in -- sw/apps/draw
 * links it today purely for buttons and swatches. Section GC would
 * drop the unused code, but only after the app's Makefile has already
 * been made to link zfsapp.o. Separate file, separate object, nobody
 * pays for what they don't use.
 *
 * -- what it is --
 *
 * The list part of a file dialog, factored out on the assumption that
 * a standalone file browser app wants exactly the same thing: rows of
 * names with a folder/file icon, keyboard and mouse selection, its own
 * vertical scrollbar, and directory navigation including a ".." row.
 * The dialogs in zdialog.h are its first two callers; the browser is
 * the intended third.
 *
 * It owns fixed storage for a bounded number of entries (see
 * Z_FLIST_MAX) and allocates nothing at any point -- an app's whole
 * stack-and-heap allowance is 16KB by default (Z_PROC_STACK_SIZE_
 * DEFAULT, sw/os/kernel.h), and a widget that mallocs every time you
 * open a folder is a slow leak waiting to happen. A directory with
 * more entries than fit is truncated, and z_flist_truncated() says so
 * -- see that function.
 *
 * -- drawing --
 *
 * Rows go through the hardware glyph blitter (z_win_draw_text()) and
 * the row icons through the hardware icon blitter (z_fb_draw_icon(),
 * zgfx.h) using Z_ICON_FOLDER/_FILE/_UPDIR, which wm has already
 * loaded into glyph memory. Selection is drawn as an inverted row via
 * z_fb_draw_text2(), the same two-color-cell trick sw/apps/term uses
 * for reverse video, rather than a per-pixel invert.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zwin.h"
#include "zwidget.h"

// Bounds, chosen against the SD cards this actually runs on rather
// than in the abstract. 128 entries is a comfortable multiple of what
// a Zeitlos root directory holds; 8.3 short names plus a "/" prefix
// and a NUL fit in 16 bytes with room to spare (FatFs is built with
// FF_SFN_BUF = 12, see sw/os/fs/fatfs/ffconf.h), and 24 leaves
// headroom if long filename support is ever turned on.
//
// Cost is Z_FLIST_MAX * Z_FLIST_NAME_MAX = 3KB of .bss per instance,
// plus the shared staging buffer in zflist.c. Worth knowing before
// putting three of these in one app.
#define Z_FLIST_MAX        128
#define Z_FLIST_NAME_MAX    24
#define Z_FLIST_PATH_MAX    64

// z_flist_mouse()/z_flist_key() results
#define Z_FLIST_NONE        0	// nothing happened
#define Z_FLIST_SELECTED    1	// the selection moved
#define Z_FLIST_ACTIVATED   2	// an entry was chosen (double click,
								// or Enter) -- for a directory the
								// widget has ALREADY navigated into it
								// and this is really "the listing
								// changed"; see z_flist_activated_dir()

typedef struct {

	const z_win_t	*win;

	// content-relative rect the whole widget occupies, scrollbar
	// included
	int16_t		x, y, w, h;

	// current directory, always "/"-prefixed and without a trailing
	// slash except at the root ("/")
	char		path[Z_FLIST_PATH_MAX];

	// Entries, sorted directories-first then by name. Stored as bare
	// display names (no path), because that is what gets drawn and
	// what a caller usually wants to show; z_flist_selected_path()
	// rebuilds the full path on demand.
	char		names[Z_FLIST_MAX][Z_FLIST_NAME_MAX];
	uint8_t		isdir[Z_FLIST_MAX];
	int			count;

	// true if the directory held more than Z_FLIST_MAX entries, or
	// more than the staging buffer could hold
	bool		truncated;

	// index into names[], or -1. Note ".." is NOT an entry -- it's
	// drawn as a synthetic row above index 0 whenever path isn't the
	// root, so that navigating up doesn't depend on the filesystem
	// actually reporting a ".." entry (FatFs's f_readdir does not).
	int			sel;

	// whether the synthetic ".." row is currently the selection
	bool		sel_updir;

	int			top;			// first visible row
	z_scrollbar_t	sb;

	// double-click detection. A "row I last pressed on" plus a tick
	// stamp, rather than anything wm-side: there is no click-count
	// in Z_WM_MOUSE (zwm.h) and no system-wide double-click interval
	// to consult, so this is the widget's own business.
	int			last_press_row;
	uint32_t	last_press_tick;

	// whether the most recent activation was a directory -- latched
	// at the moment it happened, because activating a directory
	// changes the selection out from under anyone who tried to work
	// it out afterwards. See z_flist_activated_dir().
	bool		last_act_dir;

	uint8_t		last_buttons;
	bool		dirty;

} z_flist_t;

// binds the widget to a window. Does not read the filesystem -- call
// z_flist_chdir() for that.
void z_flist_init(z_flist_t *fl, const z_win_t *win);

// content-relative rect, scrollbar included. Call from the app's own
// layout() on every resize.
void z_flist_set_geom(z_flist_t *fl, int x, int y, int w, int h);

// reads `path` (NULL or "" means the root) and resets the selection
// to the top. Returns false if the directory couldn't be read, in
// which case the widget keeps its previous contents -- an unreadable
// directory shouldn't blank a list the user was successfully looking
// at a moment ago.
bool z_flist_chdir(z_flist_t *fl, const char *path);

// re-reads the current directory, preserving the selected NAME where
// it still exists. For after a save, so the new file shows up.
bool z_flist_refresh(z_flist_t *fl);

void z_flist_draw(z_flist_t *fl, bool force);

// marks everything dirty -- after a resize, or any time the window's
// content was cleared underneath the widget.
void z_flist_invalidate(z_flist_t *fl);

// pointer sample, content-relative, `buttons` from
// Z_WM_UNPACK_MOUSE_BUTTONS. Returns one of Z_FLIST_*.
int z_flist_mouse(z_flist_t *fl, int cx, int cy, uint8_t buttons);

// one keysym (zkbd.h) -- Up/Down/PageUp/PageDown/Home/End move the
// selection, Enter activates, Backspace goes up a directory. Returns
// one of Z_FLIST_*; Z_FLIST_NONE for a key this widget doesn't use,
// so the caller can go on to handle it itself.
int z_flist_key(z_flist_t *fl, uint32_t keysym);

// the selected entry's bare NAME, or NULL if nothing is selected or
// the ".." row is. Points into the widget -- valid until the next
// chdir/refresh.
const char *z_flist_selected(const z_flist_t *fl);

// the selected entry's FULL path, written into `out`. Returns false
// if nothing is selected, the ".." row is, or `out` is too small.
bool z_flist_selected_path(const z_flist_t *fl, char *out, int outlen);

// true if the selection is a directory (or the ".." row)
bool z_flist_selected_is_dir(const z_flist_t *fl);

// true if the last Z_FLIST_ACTIVATED was a directory the widget
// navigated into, rather than a file the caller should act on. A
// dialog's OK button wants exactly this test: activating a folder
// means "show me inside", activating a file means "this is my
// answer".
bool z_flist_activated_dir(const z_flist_t *fl);

// true if the current listing was cut short -- more entries exist on
// disk than the widget can hold. Worth surfacing somewhere in any UI
// built on this: the alternative is a file that is definitely there
// and simply never appears.
bool z_flist_truncated(const z_flist_t *fl);

#endif
