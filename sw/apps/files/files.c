/*
 * files -- a file browser
 *
 * Browse the filesystem, open a file with whatever app handles its
 * type, make directories, and rename, copy, move and delete things.
 * Each row shows the entry's size and date (docs/file_browser.md).
 *
 *   > run wm
 *   > run files
 *
 * Most of this app is the z_flist_t widget (sw/common/zflist.h), which
 * was factored out of the file dialogs on exactly the assumption that
 * a browser would want the same thing: rows with an icon, keyboard and
 * mouse selection, its own scrollbar, and directory navigation. This
 * file is the window around it, eight buttons, and the decision about
 * what double-clicking something means.
 *
 * -- find, and copying or deleting a whole folder --
 *
 * All three walk a tree, one small step at a time, from this app's
 * event loop: sw/common/zwalk.c, docs/zwalk.md. Each step is one
 * filesystem call, the loop gives the steps a 20ms slice and then
 * reads its messages again, so the window answers throughout and Esc
 * (or Stop) ends a walk before its next step. Found files stream into
 * a results list that replaces the folder while it is up.
 *
 * -- other systems' files --
 *
 * docx, odt, rtf, epub, xlsx, ods, tsv, zip, tar and gz open in
 * `convert` (ztype.c), which converts and opens the result itself
 * (docs/convert.md). The Convert button goes the other way, or any
 * way: it asks for a name, whose ending picks the format, and runs
 * `convert` with its progress window; the list is re-read when it is
 * done. Nothing here waits for it: the browser stays usable meanwhile.
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
#include "../../common/zconvert.h"
#include "../../common/zwalk.h"
#include "../../common/zpaths.h"

// Sized so the list shows a useful number of rows and the eight
// buttons fit across the bottom without crowding -- that second one
// is the binding constraint, and it is why this is the floor rather
// than merely the initial size (Z_WIN_FLAG_MIN_IS_CREATE, zwm.h).
#define WIN_W   380
#define WIN_H   260

static z_win_t win;

static z_flist_t flist;
static z_dialog_ctx_t dlg_ctx;

// -- buttons --

#define BTN_OPEN    0
#define BTN_MKDIR   1
#define BTN_RENAME  2
#define BTN_COPY    3
#define BTN_MOVE    4
#define BTN_DELETE  5
#define BTN_CONVERT 6
#define BTN_FIND    7		// "Stop" while a walk runs
#define BTN_COUNT   8

// Eight across the window's minimum width: (380 - 2*4 - 7*4) / 8.
#define BTN_W       43
#define BTN_H       16
#define BTN_GAP     4
#define MARGIN      4

static z_widget_t widgets[BTN_COUNT];
static z_widget_set_t wset;

static const char *const btn_labels[BTN_COUNT] = {
	"Open", "New Dir", "Rename", "Copy", "Move", "Delete", "Convert", "Find"
};

// The conversion the Convert button started, if one is running, and
// the name to select when it is done.
static z_conv_t conv;
static char conv_sel[Z_FS_NAME_MAX];
static bool conv_done;

// -- walks: find, and copying, moving or deleting a folder --
//
// zwalk.h. One at a time. Its memory is the arena while one runs (and
// while find's results are up), else a small block here: enough for
// folders of a hundred-odd entries and a page of results, on a board
// without arenas.

#define WK_NONE		0
#define WK_FIND		1
#define WK_COPY		2
#define WK_MOVE		3		// a copy, then deleting the source
#define WK_DELETE	4

#define WALK_ARENA		(64u * 1024u)
#define WALK_STATIC		6144

static zwalk_t walk;
static int walk_kind;
static bool walking;
static uint8_t walk_static[WALK_STATIC];
static uint8_t *walk_mem;
static uint32_t walk_cap;
static bool walk_arena;
static char walk_from[Z_FLIST_PATH_MAX];	// move: the source, deleted after
static char walk_sel[Z_FS_NAME_MAX];		// select this when done
static uint32_t walk_drawn;					// tick of the last status line

// find's results: offsets, then the names (relative to find_root)
static z_listbox_t rlist;
static bool results;
static char find_root[Z_FLIST_PATH_MAX];
static uint32_t *res_off;
static char *res_pool;
static int res_n, res_max;
static uint32_t res_used, res_cap;

static void walk_release(void);

// The details column in the list (size and date per row, zflist.h):
// the widget lists through FS_LIST_EX when given somewhere to put it.
static z_fs_info_t finfo[Z_FLIST_MAX];

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
// list -> Open -> New Dir -> Rename -> Copy -> Move -> Delete -> Convert ->
// Find -> list. With results up, "the list" is the results list.
#define FOCUS_LIST  (-1)

static int focus = FOCUS_LIST;

// Draws the list's own focus indicator: a ring just outside its
// frame, matching the one z_widget_draw() puts around a focused
// button. Without it there is no way to tell whether typing an arrow
// key will move the selection or do nothing.
static void draw_list_focus(void) {

	// The results list draws its own ring (zwidget.h).
	if (results) {
		z_listbox_set_focus(&rlist, focus == FOCUS_LIST);
		z_listbox_draw(&rlist, false);
		return;
	}

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
	z_listbox_set_geom(&rlist, MARGIN, list_y, cw - 2 * MARGIN, list_h);

	for (int i = 0; i < BTN_COUNT; i++) {
		widgets[i].x = (int16_t)(MARGIN + i * (BTN_W + BTN_GAP));
		widgets[i].y = (int16_t)btn_y;
	}

	z_widget_invalidate(&wset);

}

// The current directory, above the list. Elided from the LEFT when it
// doesn't fit: the interesting end of a path is the last component.
static void status_draw(void);

static void path_draw(void) {

	if (walking || results) { status_draw(); return; }

	z_clip_t content;
	z_win_content_rect(&win, &content);

	int cw = z_win_content_w(&win);
	int avail = cw - 2 * MARGIN;
	if (avail <= 0) return;

	// A directory with more than Z_FLIST_MAX entries (or longer names
	// than the list's pool holds) is listed only in part. Said here,
	// on the path line, because otherwise a file that is on the card
	// simply is not in the list and nothing hints why.
	const char *more = z_flist_truncated(&flist) ? " (partial list)" : "";
	int morelen = (int)strlen(more);

	int maxch = avail / z_font_5x8.w - morelen;
	const char *p = flist.path;
	int len = (int)strlen(p);

	char shown[Z_FLIST_PATH_MAX + 24];

	if (len <= maxch) {
		memcpy(shown, p, (size_t)len + 1);
	} else if (maxch > 3) {
		shown[0] = shown[1] = shown[2] = '.';
		memcpy(shown + 3, p + len - (maxch - 3), (size_t)(maxch - 3) + 1);
	} else {
		shown[0] = 0;
	}
	strcat(shown, more);

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

	if (results) {
		z_listbox_invalidate(&rlist);
		z_listbox_draw(&rlist, true);
	} else {
		z_flist_invalidate(&flist);
		z_flist_draw(&flist, true);
	}

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

	char name[Z_FS_NAME_MAX];

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

// The current directory joined with `name`. False if it does not fit.
static bool join_dir(const char *name, char *out, int outlen) {

	int n = (int)strlen(flist.path);
	int l = (int)strlen(name);
	bool slash = (n > 0 && flist.path[n - 1] != '/');

	if (n + (slash ? 1 : 0) + l + 1 > outlen) return false;
	memcpy(out, flist.path, (size_t)n);
	if (slash) out[n++] = '/';
	memcpy(out + n, name, (size_t)l + 1);
	return true;

}

// "Can't <verb>\n<reason>." for a Z_FS_ERR_* code, in a dialog.
static void fail_dialog(const char *title, int err) {

	char msg[64];
	const char *why = fs_strerror(err);
	int n = 0;

	for (const char *p = "Not possible:\n"; *p; p++) msg[n++] = *p;
	for (const char *p = why; *p && n < (int)sizeof(msg) - 2; p++)
		msg[n++] = *p;
	msg[n++] = '.';
	msg[n] = 0;

	z_dialog_confirm(&dlg_ctx, title, msg, Z_DIALOG_OK_CANCEL);

}

// After an operation: re-read the directory, keeping `keep` selected
// if it is still here (a renamed file stays under the cursor).
static void reload(const char *keep) {
	z_flist_refresh_select(&flist, keep);
	repaint();
}

// F2, or the Rename button. The new name stays in this directory --
// moving somewhere else is Move's job, so a '/' is refused here as it
// is in New Dir.
static void do_rename(void) {

	const char *name = z_flist_selected(&flist);
	if (!name) return;

	char from[Z_FLIST_PATH_MAX], to[Z_FLIST_PATH_MAX];
	char newname[Z_FS_NAME_MAX];
	int err;

	if (!z_flist_selected_path(&flist, from, sizeof(from))) return;

	if (!z_dialog_prompt(&dlg_ctx, "Rename", "New name:", name,
		newname, sizeof(newname)) || !newname[0])
		return;
	if (!strcmp(newname, name)) return;

	for (const char *p = newname; *p; p++) {
		if (*p == '/') {
			z_dialog_confirm(&dlg_ctx, "Bad name",
				"A name cannot\ncontain '/'.", Z_DIALOG_OK_CANCEL);
			return;
		}
	}

	if (!join_dir(newname, to, sizeof(to))) return;

	if (!fs_rename(from, to, &err)) {
		fail_dialog("Rename", err);
		return;
	}

	reload(newname);

}

static bool walk_begin(int kind, int op, const char *src, const char *dst);
static int cat(char *d, int n, int cap, const char *s);

// Copy and Move ask where to, starting from where the file is now, so
// the common cases are an edit of the end of the line: a new name, or
// another directory in front of the same name. A destination that is
// an existing directory means "into it", as in posix's `mv`/`cp`.
//
// A copy -- of a file or a whole folder -- runs as a walk (zwalk.h):
// a chunk at a time from the event loop, with its progress on the
// line above the list and Esc or Stop to end it, which leaves what
// was copied so far and says so (never half a file). A move within a
// volume is a rename and instant; between volumes (the card and /ram)
// it is that copy and then a walk deleting the source, which starts
// only once the copy is complete.
static void do_copy_move(bool move) {

	const char *title = move ? "Move" : "Copy";
	const char *name = z_flist_selected(&flist);
	if (!name) return;

	char from[Z_FLIST_PATH_MAX], to[Z_FLIST_PATH_MAX];
	z_fs_info_t fi;
	int err = Z_FS_ERR_IO;

	if (!z_flist_selected_path(&flist, from, sizeof(from))) return;
	bool isdir = z_flist_selected_is_dir(&flist);

	if (!z_dialog_prompt(&dlg_ctx, title, move ? "Move to:" : "Copy to:",
		from, to, sizeof(to)) || !to[0])
		return;
	if (!strcmp(to, from)) return;

	if (fs_stat(to, &fi) && fi.type == Z_FS_TYPE_DIR) {
		int n = (int)strlen(to);
		int l = (int)strlen(name);
		if (n > 0 && to[n - 1] == '/') n--;
		if (n + 1 + l + 1 > (int)sizeof(to)) { fail_dialog(title, Z_FS_ERR_INVAL); return; }
		to[n++] = '/';
		memcpy(to + n, name, (size_t)l + 1);
	}

	if (move) {
		if (fs_rename(from, to, &err)) { reload(NULL); return; }
		if (err != Z_FS_ERR_XDEV) { fail_dialog(title, err); return; }
		if (fs_stat(to, &fi)) { fail_dialog(title, Z_FS_ERR_EXIST); return; }
	} else if (fs_stat(to, &fi)) {
		// A copy onto an existing file asks first, as Delete does; a
		// folder is never replaced.
		if (isdir || fi.type == Z_FS_TYPE_DIR) { fail_dialog(title, Z_FS_ERR_EXIST); return; }
		if (z_dialog_confirm(&dlg_ctx, title,
			"Replace the file that\nis already there?", Z_DIALOG_YES_NO)
			!= Z_DIALOG_YES)
			return;
		if (!fs_unlink(to)) { fail_dialog(title, Z_FS_ERR_BUSY); return; }
	}

	cat(walk_from, 0, (int)sizeof(walk_from), from);
	if (walk_begin(move ? WK_MOVE : WK_COPY, ZW_COPY, from, to)) {
		const char *b = strrchr(to, '/');
		// the copy's own name, when it lands in this folder
		cat(walk_sel, 0, (int)sizeof(walk_sel), b ? b + 1 : to);
	}

}

// A file is deleted at once. A folder -- with everything in it -- is a
// walk (zwalk.h): progress above the list, Esc or Stop to end it.
static void do_delete(void) {

	const char *name = z_flist_selected(&flist);
	if (!name) return;			// nothing selected, or the ".." row

	char path[Z_FLIST_PATH_MAX];
	if (!z_flist_selected_path(&flist, path, sizeof(path))) return;

	bool isdir = z_flist_selected_is_dir(&flist);

	// The name goes in the question. "Delete this file?" is a
	// question the user cannot actually check the answer to.
	char msg[128];
	int n = cat(msg, 0, (int)sizeof(msg), isdir ? "Delete the folder\n" : "Delete the file\n");
	n = cat(msg, n, (int)sizeof(msg), name);
	cat(msg, n, (int)sizeof(msg), isdir ? "\nand everything in it?" : "?");

	if (z_dialog_confirm(&dlg_ctx, "Delete", msg, Z_DIALOG_YES_NO)
		!= Z_DIALOG_YES)
		return;

	if (isdir) {
		walk_begin(WK_DELETE, ZW_DELETE, path, NULL);
		return;
	}

	if (!fs_unlink(path)) {
		z_dialog_confirm(&dlg_ctx, "Failed",
			"The file could not be\ndeleted. Is it in use?",
			Z_DIALOG_OK_CANCEL);
		return;
	}

	z_flist_refresh(&flist);
	repaint();

}

// The extension `path` should get by default when converted: the
// other system's format for this system's own files, and this
// system's for the other's. "" for an archive (a folder), NULL for
// something convert does not read.
static const char *convert_default(const char *path, bool *strip_tar) {

	static const char *const map[][2] = {
		{ "TXT", "docx" }, { "MD", "docx" }, { "ASC", "docx" },
		{ "DOCX", "md" }, { "ODT", "md" }, { "RTF", "md" }, { "EPUB", "md" },
		{ "HTM", "md" }, { "HTML", "md" },
		{ "ZSS", "xlsx" }, { "CSV", "xlsx" },
		{ "XLSX", "zss" }, { "ODS", "zss" }, { "TSV", "zss" },
		{ "ZIP", "" }, { "TAR", "" }, { "TGZ", "" }, { "GZ", "" },
	};

	const char *ext = z_ftype_ext(path);
	*strip_tar = false;
	if (!ext) return NULL;

	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
		const char *a = map[i][0], *b = ext;
		while (*a && (*b & ~32) == *a) { a++; b++; }
		if (*a || *b) continue;
		if (!strcmp(map[i][0], "GZ")) {
			// "x.tar.gz" is a tar archive: its folder is "x"
			int n = (int)strlen(path);
			*strip_tar = n > 7 && path[n - 7] == '.' && (path[n - 6] | 32) == 't' &&
				(path[n - 5] | 32) == 'a' && (path[n - 4] | 32) == 'r';
		}
		return map[i][1];
	}
	return NULL;

}

static void do_convert(void) {

	const char *name = z_flist_selected(&flist);
	if (!name || z_flist_selected_is_dir(&flist)) return;

	if (conv.running) {
		z_dialog_confirm(&dlg_ctx, "Convert",
			"A conversion is already\nrunning.", Z_DIALOG_OK_CANCEL);
		return;
	}

	char from[Z_FLIST_PATH_MAX], to[Z_FLIST_PATH_MAX];
	if (!z_flist_selected_path(&flist, from, sizeof(from))) return;

	bool strip_tar;
	const char *ext = convert_default(from, &strip_tar);
	if (!ext) {
		z_dialog_confirm(&dlg_ctx, "Convert",
			"convert does not read\nthis kind of file.", Z_DIALOG_OK_CANCEL);
		return;
	}

	// The same name with the new ending; an archive's folder is its
	// name without one.
	int n = (int)strlen(from);
	memcpy(to, from, (size_t)n + 1);
	char *dot = strrchr(to, '.');
	if (dot && dot > strrchr(to, '/')) *dot = 0;
	if (strip_tar) { dot = strrchr(to, '.'); if (dot) *dot = 0; }
	if (*ext) {
		n = (int)strlen(to);
		if (n + 1 + (int)strlen(ext) < (int)sizeof(to)) {
			to[n++] = '.';
			strcpy(to + n, ext);
		}
	}

	char out[Z_FLIST_PATH_MAX];
	if (!z_dialog_prompt(&dlg_ctx, "Convert",
		*ext ? "To (the ending picks the format):" : "Extract into the folder:",
		to, out, sizeof(out)) || !out[0])
		return;
	if (!strcmp(out, from)) return;

	z_fs_info_t fi;
	if (*ext && fs_stat(out, &fi)) {
		if (fi.type == Z_FS_TYPE_DIR) { fail_dialog("Convert", Z_FS_ERR_EXIST); return; }
		if (z_dialog_confirm(&dlg_ctx, "Convert",
			"Replace the file that\nis already there?", Z_DIALOG_YES_NO) != Z_DIALOG_YES)
			return;
	}

	if (!z_conv_start(&conv, from, out, NULL, Z_CONV_UI)) {
		z_dialog_confirm(&dlg_ctx, "Convert",
			conv.result == Z_CONV_E_GONE ? "convert is not on\nthis card." :
			"Could not start: is\n/tmp writable?", Z_DIALOG_OK_CANCEL);
		return;
	}

	// selected afterwards if it landed in this folder
	const char *slash = strrchr(out, '/');
	const char *base = slash ? slash + 1 : out;
	n = (int)strlen(base);
	if (n >= (int)sizeof(conv_sel)) n = (int)sizeof(conv_sel) - 1;
	memcpy(conv_sel, base, (size_t)n);
	conv_sel[n] = 0;

}

// ---------------------------------------------------------------
// walks: find, copy, move, delete -- zwalk.h, docs/zwalk.md
// ---------------------------------------------------------------

// No printf here (files has none): strings by hand.
static int cat(char *d, int n, int cap, const char *s) {
	while (s && *s && n < cap - 1) d[n++] = *s++;
	d[n] = 0;
	return n;
}

static int cat_u(char *d, int n, int cap, uint32_t v) {
	char t[12];
	int k = 0;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k && n < cap - 1) d[n++] = t[--k];
	d[n] = 0;
	return n;
}

// "1.2 MB", "340 KB", "12 bytes"
static int cat_size(char *d, int n, int cap, uint32_t b) {
	if (b >= 1024u * 1024u) {
		n = cat_u(d, n, cap, b / (1024u * 1024u));
		n = cat(d, n, cap, ".");
		n = cat_u(d, n, cap, (b % (1024u * 1024u)) * 10u / (1024u * 1024u));
		return cat(d, n, cap, " MB");
	}
	if (b >= 1024u) return cat(d, cat_u(d, n, cap, b / 1024u), cap, " KB");
	return cat(d, cat_u(d, n, cap, b), cap, " bytes");
}

// The arena if there is one (64KB, else less), else walk_static.
static void walk_acquire(void) {

	static const uint32_t want[] = { WALK_ARENA, WALK_ARENA / 2, WALK_ARENA / 4 };
	void *base;
	uint32_t got;

	if (walk_mem) return;
	for (int i = 0; i < 3; i++) {
		if (z_arena_set(want[i], &base, &got) == Z_ARENA_OK && base && got) {
			walk_mem = (uint8_t *)base;
			walk_cap = got;
			walk_arena = true;
			return;
		}
	}
	walk_mem = walk_static;
	walk_cap = WALK_STATIC;
	walk_arena = false;

}

static void walk_release(void) {
	if (walk_arena) z_arena_set(0, NULL, NULL);
	walk_mem = NULL;
	walk_cap = 0;
	walk_arena = false;
}

// Buttons for the state we are in: while a walk runs only Find, as
// Stop; with results up, Open and Find.
static void buttons_update(void) {
	for (int i = 0; i < BTN_COUNT; i++)
		widgets[i].enabled = !walking && (!results || i == BTN_OPEN || i == BTN_FIND);
	widgets[BTN_FIND].enabled = true;
	widgets[BTN_FIND].label = walking ? "Stop" : "Find";
	z_widget_invalidate(&wset);
}

// The line above the list, while a walk runs or results are up.
static void status_draw(void) {

	char line[120];
	int n = 0;
	const int cap = (int)sizeof(line);

	switch (walk_kind) {
	case WK_FIND:
		n = cat(line, n, cap, walking ? "Finding... " : "");
		n = cat_u(line, n, cap, (uint32_t)res_n);
		n = cat(line, n, cap, " found");
		if (walking) {
			n = cat(line, n, cap, ", ");
			n = cat_u(line, n, cap, walk.files);
			n = cat(line, n, cap, " files looked at");
		} else {
			n = cat(line, n, cap, " in ");
			n = cat(line, n, cap, find_root);
			if (walk.truncated || res_n >= res_max) n = cat(line, n, cap, " (not all)");
		}
		break;
	case WK_COPY:
	case WK_MOVE:
		n = cat(line, n, cap, walk.op == ZW_DELETE ? "Removing the source... " :
			walk_kind == WK_MOVE ? "Moving... " : "Copying... ");
		n = cat_u(line, n, cap, walk.files);
		n = cat(line, n, cap, " files, ");
		n = cat_size(line, n, cap, walk.bytes);
		break;
	case WK_DELETE:
		n = cat(line, n, cap, "Deleting... ");
		n = cat_u(line, n, cap, walk.files + walk.dirs);
		n = cat(line, n, cap, " deleted");
		break;
	default:
		break;
	}
	n = cat(line, n, cap, walking ? "  (Esc stops)" : "  (Esc: back)");

	z_clip_t content, clip;
	z_win_content_rect(&win, &content);
	int cw = z_win_content_w(&win);
	clip.x0 = content.x0 + MARGIN;
	clip.y0 = content.y0 + MARGIN;
	clip.x1 = content.x0 + cw - MARGIN - 1;
	clip.y1 = clip.y0 + z_font_5x8.h - 1;
	if (clip.x1 < clip.x0) return;
	z_fb_hw_fill_rect(clip.x0, clip.y0, clip.x1 - clip.x0 + 1, z_font_5x8.h, 0);
	z_fb_draw_text(clip.x0, clip.y0, line, 1, &z_font_5x8, &clip);
	walk_drawn = z_uptime_ticks();

}

static const char *res_label(void *user, int i) {
	(void)user;
	return (i >= 0 && i < res_n) ? res_pool + res_off[i] : "";
}

// A match: its path below find_root, into the results.
static void res_add(const char *path) {

	int skip = (int)strlen(find_root);
	if (skip > 1) skip++;			// and the '/'
	else skip = 1;					// "/"
	const char *rel = path + skip;
	uint32_t l = (uint32_t)strlen(rel) + 1;

	if (res_n >= res_max || res_used + l > res_cap) return;
	memcpy(res_pool + res_used, rel, l);
	res_off[res_n++] = res_used;
	res_used += l;
	z_listbox_set_items(&rlist, res_n, res_label, NULL);
	if (res_n == 1) z_listbox_select(&rlist, 0);

}

static void results_leave(void) {
	results = false;
	z_listbox_set_items(&rlist, 0, res_label, NULL);	// its names go with the memory
	if (!walking) { walk_kind = WK_NONE; walk_release(); }
	buttons_update();
	repaint();
}

// The end of a walk, by itself or stopped.
static void walk_finish(int r, bool stopped) {

	char msg[160];
	int n = 0;
	const int cap = (int)sizeof(msg);
	int kind = walk_kind;

	walking = false;

	if (kind == WK_FIND) {
		buttons_update();
		repaint();
		return;
	}

	// a move whose copy is done: now the source
	if (kind == WK_MOVE && r == ZW_DONE && !stopped && walk.op != ZW_DELETE) {
		uint32_t files = walk.files, bytes = walk.bytes;
		if (zw_start(&walk, ZW_DELETE, walk_from, NULL, walk_mem, walk_cap)) {
			walk.bytes = bytes;
			walk.files = files;
			walking = true;
			path_draw();
			return;
		}
		r = ZW_FAIL;
	}

	if (stopped || r == ZW_FAIL) {
		if (stopped) n = cat(msg, n, cap, "Stopped.");
		else {
			n = cat(msg, n, cap, "Stopped: ");
			n = cat(msg, n, cap, zw_strerror(walk.err));
			n = cat(msg, n, cap, "\n");
			n = cat(msg, n, cap, walk.hit);
		}
		if (walk.op == ZW_DELETE)
			n = cat(msg, n, cap, kind == WK_MOVE
				? "\nThe copy is complete; part of\nthe original is still there."
				: "\nPart of it is deleted already.");
		else if (walk.partial)
			n = cat(msg, n, cap, "\nWhat was copied so far is left\nin the destination.");
		z_dialog_confirm(&dlg_ctx, kind == WK_DELETE ? "Delete" :
			kind == WK_MOVE ? "Move" : "Copy", msg, Z_DIALOG_OK_CANCEL);
	}

	walk_kind = WK_NONE;
	walk_release();
	buttons_update();
	reload(walk_sel[0] ? walk_sel : NULL);

}

static void walk_stop(void) {
	if (!walking) return;
	zw_cancel(&walk);
	walk_finish(ZW_DONE, true);
}

// Steps for up to 20ms, then back to the messages.
static void walk_pump(void) {

	uint32_t t0 = z_uptime_ticks();

	while (walking && z_uptime_ticks() - t0 < Z_TICK_HZ / 50) {
		int r = zw_step(&walk);
		if (r == ZW_HIT) {
			res_add(walk.hit);
			z_listbox_draw(&rlist, false);
		} else if (r == ZW_DONE || r == ZW_FAIL) {
			walk_finish(r, false);
			return;
		}
	}

	if (walking && z_uptime_ticks() - walk_drawn > Z_TICK_HZ / 5) status_draw();

}

// Starts a walk. Find keeps part of the memory for its results:
// the listing needs room for one big folder, the results the rest.
static bool walk_begin(int kind, int op, const char *src, const char *dst) {

	walk_acquire();
	uint32_t wcap = walk_cap;
	if (kind == WK_FIND) {
		wcap = walk_cap / 2;
		if (wcap > 24576) wcap = 24576;
		if (wcap < 3584) wcap = 3584;		// zw_start()'s least, and a little
	}
	if (!zw_start(&walk, op, src, dst, walk_mem, wcap)) {
		int err = walk.err;
		walk_release();
		char msg[96];
		int n = cat(msg, 0, (int)sizeof(msg), "Not possible:\n");
		n = cat(msg, n, (int)sizeof(msg), zw_strerror(err));
		cat(msg, n, (int)sizeof(msg), ".");
		z_dialog_confirm(&dlg_ctx, kind == WK_FIND ? "Find" : kind == WK_DELETE ?
			"Delete" : kind == WK_MOVE ? "Move" : "Copy", msg, Z_DIALOG_OK_CANCEL);
		return false;
	}
	walk_kind = kind;
	walking = true;
	walk_drawn = 0;
	walk_sel[0] = 0;

	if (kind == WK_FIND) {
		uint8_t *r = walk_mem + wcap;
		uint32_t rcap = walk_cap - wcap;
		res_max = (int)(rcap / 32);
		res_off = (uint32_t *)(void *)r;
		res_pool = (char *)(r + (uint32_t)res_max * 4u);
		res_cap = rcap - (uint32_t)res_max * 4u;
		res_n = 0;
		res_used = 0;
		z_listbox_set_items(&rlist, 0, res_label, NULL);
		z_listbox_select(&rlist, -1);
		results = true;
		focus = FOCUS_LIST;
		z_widget_focus_set(&wset, -1);
	}

	buttons_update();
	repaint();
	return true;

}

// Ctrl+F, or the Find button: names, "text inside", or both.
static void do_find(void) {

	char q[96], name[ZW_NAME_MAX], text[ZW_TEXT_MAX];
	int nn = 0, tn = 0;

	if (!z_dialog_prompt(&dlg_ctx, "Find",
		"In this folder and below. A name\n"
		"(report, *.md), \"text\" inside files,\n"
		"or both: *.txt \"invoice\"",
		NULL, q, sizeof(q)) || !q[0])
		return;

	// Split: the quoted part is text, the rest the name.
	for (const char *p = q; *p; p++) {
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				if (tn < ZW_TEXT_MAX - 1) text[tn++] = *p;
			if (!*p) break;
		} else if (nn < ZW_NAME_MAX - 1 && (nn > 0 || *p != ' ')) {
			name[nn++] = *p;
		}
	}
	while (nn > 0 && name[nn - 1] == ' ') nn--;
	name[nn] = 0;
	text[tn] = 0;

	// a new find replaces the results up now
	if (walk_kind == WK_FIND) { walk_kind = WK_NONE; results = false; walk_release(); }

	cat(find_root, 0, (int)sizeof(find_root), flist.path);
	if (walk_begin(WK_FIND, ZW_FIND, flist.path, NULL))
		zw_find_set(&walk, name, text);

}

// The full path of result `i`.
static bool res_path(int i, char *out, int cap) {
	if (i < 0 || i >= res_n) return false;
	int n = cat(out, 0, cap, find_root);
	if (n > 1) n = cat(out, n, cap, "/");
	n = cat(out, n, cap, res_pool + res_off[i]);
	return n < cap - 1;
}

// Enter, a double-click, or Open on a result: a folder is gone into
// (leaving the results), a file opened as from the list.
static void res_open(void) {
	char path[Z_FLIST_PATH_MAX];
	z_fs_info_t fi;
	if (!res_path(z_listbox_selected(&rlist), path, sizeof(path))) return;
	if (fs_stat(path, &fi) && fi.type == Z_FS_TYPE_DIR) {
		walk_stop();
		z_flist_chdir(&flist, path);
		results_leave();
		return;
	}
	launch(path);
}

// Backspace on a result: its folder, with it selected.
static void res_goto(void) {
	char path[Z_FLIST_PATH_MAX];
	if (!res_path(z_listbox_selected(&rlist), path, sizeof(path))) return;
	char *slash = strrchr(path, '/');
	if (!slash) return;
	char name[Z_FS_NAME_MAX];
	cat(name, 0, (int)sizeof(name), slash + 1);
	if (slash == path) slash[1] = 0;
	else *slash = 0;
	walk_stop();
	z_flist_chdir(&flist, path);
	results_leave();
	reload(name);
}

// ---------------------------------------------------------------
// input
// ---------------------------------------------------------------

static void do_find(void);
static void res_open(void);

// A button, pressed by pointer or key.
static void button(int act) {
	if (act == BTN_FIND) {
		if (walking) walk_stop();
		else do_find();
		return;
	}
	if (walking) return;
	if (results) { if (act == BTN_OPEN) res_open(); return; }
	switch (act) {
		case BTN_OPEN:   do_open(); break;
		case BTN_MKDIR:  do_mkdir(); break;
		case BTN_RENAME: do_rename(); break;
		case BTN_COPY:   do_copy_move(false); break;
		case BTN_MOVE:   do_copy_move(true); break;
		case BTN_DELETE: do_delete(); break;
		case BTN_CONVERT: do_convert(); break;
		default: break;
	}
}

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	int act = z_widget_mouse(&wset, cx, cy, buttons);

	if (act >= 0) { button(act); return; }

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

	if (results) {
		if (z_listbox_mouse(&rlist, cx, cy, buttons) == Z_LIST_ACTIVATED)
			res_open();
		return;
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

	if (keysym == Z_KEY_F1) {	// docs/app_help.md
		static const char help[] =
			"Browse, open and manage files.\n\n"
			"Enter\tOpen the selection\n"
			"Backspace\tUp a folder\n"
			"F2\tRename\n"
			"Delete\tDelete (asks first)\n"
			"Ctrl+F\tFind by name, \"text\" inside, or both\n"
			"Esc\tStop a find, copy or delete\n"
			"Tab\tTo the buttons; Space presses\n\n"
			"Results: Enter opens, Backspace goes to the "
			"file's folder, Esc goes back.\n\n"
			"Copy and Move ask for the destination, starting from the "
			"file's own path: an existing folder means into it. "
			"Folders copy, move and delete with everything in them. "
			"Convert saves the file in another format, picked by the "
			"ending you give it (.docx, .md, .xlsx, ...).\n\n"
			Z_DIR_HOME " is yours, " Z_DIR_TMP " is the RAM disk (gone at power-off).";
		z_dialog_help(&dlg_ctx, "Files help", help, "file_browser");
		return;
	}

	// Esc stops a walk; with it stopped, it takes the results away.
	if (keysym == 0x1b) {
		if (walking) walk_stop();
		else if (results) results_leave();
		return;
	}

	// Ctrl+F: find (or a new find, over the results).
	if (keysym == 0x06) {
		if (!walking || walk_kind == WK_FIND) { walk_stop(); do_find(); }
		return;
	}

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

			button(z_widget_key_activate(&wset));

			// Everything above can repaint, which clears the rings.
			draw_list_focus();
			z_widget_draw_all(&wset, true);

		}

		return;

	}

	if (results) {
		int r;
		if (keysym == 0x0d) { res_open(); return; }
		if (keysym == 0x08) { res_goto(); return; }	// Backspace: its folder
		r = z_listbox_key(&rlist, keysym);
		if (r != Z_LIST_NONE) z_listbox_draw(&rlist, false);
		if (r == Z_LIST_ACTIVATED) res_open();
		return;
	}

	if (keysym == 0x0d) {		// Enter -- same as the Open button
		do_open();
		draw_list_focus();
		return;
	}

	// While a copy or delete runs, the list can be browsed, and
	// nothing else done to it.
	if (walking && (keysym == Z_KEY_F2 || keysym == Z_KEY_DELETE)) return;

	// The usual shortcuts, in the list: F2 renames, Delete deletes
	// (still asking first).
	if (keysym == Z_KEY_F2) { do_rename(); draw_list_focus(); return; }
	if (keysym == Z_KEY_DELETE) { do_delete(); draw_list_focus(); return; }

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

	// The conversion's progress and end. Its window shows the
	// progress; this only needs the end, and acts on it from the main
	// loop -- this can be running inside a dialog.
	if (z_conv_msg(&conv, msg)) {
		if (!conv.running) conv_done = true;
		return;
	}

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
	flist.info = finfo;
	z_listbox_init(&rlist, &win);

	layout();

	// A directory to start in, if something launched us with one.
	// Claimed even when unused, so it can't be left pending for
	// whatever the user opens next (Z_WM_SET_ARG, zwm.h).
	{
		char arg[Z_FLIST_PATH_MAX];
		if (!z_launch_path_take(arg, sizeof(arg))) arg[0] = 0;
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

				case Z_WM_WHEEL:

					// The list scrolls; the selection stays put (zflist.h).
					if (msg.obj.type != Z_UINT32) break;
					if (results) {
						z_listbox_wheel(&rlist, Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32));
						z_listbox_draw(&rlist, false);
					} else {
						z_flist_wheel(&flist, Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32));
					}

					break;

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id) {
						// A copy in progress leaves no half file.
						if (walking) zw_cancel(&walk);
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

		if (conv.running) {
			z_conv_poll(&conv);
			if (!conv.running) conv_done = true;
		}
		if (conv_done) {
			conv_done = false;
			reload(conv.result == 0 ? conv_sel : NULL);
		}

		// A walk runs in the gaps: a 20ms slice, then the messages
		// again -- and no waiting while it has work.
		if (walking) { walk_pump(); continue; }

		for (volatile int i = 0; i < 200; i++);	// light throttle

	
		/* Yield. This loop used to spin, so the app was RUNNABLE
		 * forever and took a full scheduler share from whatever was
		 * in the foreground -- see docs/app_runtime.md. a file browser only moves when you tell it to. */
		z_proc_wait(Z_TICK_HZ / 30);
	}

	return 0;

}
