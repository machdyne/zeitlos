/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Scrolling directory listing widget. See zflist.h for what this is
 * and why it isn't part of zwidget.c.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "zeitlos.h"
#include "zwm.h"
#include "zwin.h"
#include "zgfx.h"
#include "zfont.h"
#include "zicon.h"
#include "zkbd.h"
#include "zwidget.h"
#include "zfsapp.h"
#include "zflist.h"

// -- row metrics --
//
// All against z_font_5x8, which is the only font in glyph memory (see
// zicon.h's header comment on wm being its sole owner). Row height is
// the glyph height plus two, which is the difference between a list
// that reads as rows and one that reads as a wall.
#define ROW_H       (z_font_5x8.h + 2)
#define ICON_GAP    3
#define TEXT_X      (Z_ICON_W + ICON_GAP)

// Staging for one listing. Shared across every z_flist_t in a process
// -- it is only live inside load_dir() below, never across a call, so
// two widgets can't be using it at once even in principle (this is a
// single-threaded message loop).
//
// 3KB: Z_FLIST_MAX short names at their worst case, with slack. A
// directory that overflows it comes back truncated, which
// z_flist_truncated() reports, rather than failing.
#define STAGE_SIZE  3072
static char stage_buf[STAGE_SIZE];
static uint8_t stage_types[Z_FLIST_MAX];

// Double-click window, in kernel ticks. Z_TICK_HZ is 732 (zsoc.h), so
// this is a bit over a third of a second -- deliberately generous,
// since the two clicks are being made with whatever pointing device
// happens to be plugged into a USB port, not a calibrated mouse.
#define DBLCLICK_TICKS  256

// -- helpers --

// The bare filename at the end of a "/"-prefixed path. fs_list_into()
// hands back full paths (see zfs.h), but a list draws names.
static const char *basename_of(const char *p) {

	const char *last = p;

	for (const char *s = p; *s; s++)
		if (*s == '/') last = s + 1;

	return last;

}

static void copy_bounded(char *dst, const char *src, int cap) {

	int i = 0;
	for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = 0;

}

// Directories before files, then case-insensitive by name. Insertion
// sort over a bounded array: Z_FLIST_MAX is 128, this runs once per
// directory change, and anything cleverer would be more code than the
// thing it replaces.
static void sort_entries(z_flist_t *fl) {

	for (int i = 1; i < fl->count; i++) {

		char name[Z_FLIST_NAME_MAX];
		uint8_t dir = fl->isdir[i];
		memcpy(name, fl->names[i], Z_FLIST_NAME_MAX);

		int j = i - 1;

		while (j >= 0) {

			bool after;

			if (fl->isdir[j] != dir) {
				// a directory sorts before a file
				after = (dir && !fl->isdir[j]);
			} else {
				// strcasecmp isn't available on every libc this tree
				// builds against, so compare folded bytes directly.
				const char *a = fl->names[j], *b = name;
				int cmp = 0;
				while (*a || *b) {
					char ca = *a, cb = *b;
					if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
					if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
					if (ca != cb) { cmp = (ca < cb) ? -1 : 1; break; }
					a++; b++;
				}
				after = (cmp < 0);
			}

			if (!after) break;

			memcpy(fl->names[j + 1], fl->names[j], Z_FLIST_NAME_MAX);
			fl->isdir[j + 1] = fl->isdir[j];
			j--;

		}

		memcpy(fl->names[j + 1], name, Z_FLIST_NAME_MAX);
		fl->isdir[j + 1] = dir;

	}

}

// how many rows fit in the widget's height
static int visible_rows(const z_flist_t *fl) {
	int n = (fl->h - 2) / ROW_H;
	return n > 0 ? n : 0;
}

// total rows, including the synthetic ".." row when there is one
static int total_rows(const z_flist_t *fl) {
	return fl->count + (fl->path[1] ? 1 : 0);
}

// true if this widget is showing a ".." row (i.e. we're not at the
// root). path is always "/"-prefixed, so path[1] being non-NUL is
// exactly "deeper than the root".
static bool has_updir(const z_flist_t *fl) {
	return fl->path[1] != 0;
}

// Row index of the current selection, in the same numbering the
// display uses (".." is row 0 when present).
static int sel_row(const z_flist_t *fl) {

	if (fl->sel_updir) return 0;
	if (fl->sel < 0) return -1;

	return fl->sel + (has_updir(fl) ? 1 : 0);

}

// Moves the selection to display row `row`, clamped. Scrolls to keep
// it visible.
static void select_row(z_flist_t *fl, int row) {

	int total = total_rows(fl);
	if (total <= 0) return;

	if (row < 0) row = 0;
	if (row >= total) row = total - 1;

	if (has_updir(fl) && row == 0) {
		fl->sel_updir = true;
		fl->sel = -1;
	} else {
		fl->sel_updir = false;
		fl->sel = row - (has_updir(fl) ? 1 : 0);
	}

	int rows = visible_rows(fl);

	if (rows > 0) {
		if (row < fl->top) fl->top = row;
		if (row >= fl->top + rows) fl->top = row - rows + 1;
		if (fl->top > total - rows) fl->top = total - rows;
		if (fl->top < 0) fl->top = 0;
	}

	z_scrollbar_set_value(&fl->sb, fl->top);
	fl->dirty = true;

}

// Reads a directory into the widget. `keep` (may be NULL) names an
// entry to re-select afterwards if it still exists.
static bool load_dir(z_flist_t *fl, const char *path, const char *keep) {

	char want[Z_FLIST_NAME_MAX];
	want[0] = 0;
	if (keep) copy_bounded(want, keep, Z_FLIST_NAME_MAX);

	uint32_t count = 0, truncated = 0;

	if (!fs_list_into(path, stage_buf, STAGE_SIZE, stage_types,
		Z_FLIST_MAX, &count, &truncated)) {

		// An EMPTY directory is not a failure, but fs_list_into()
		// can't tell us that apart from a real one -- the syscall
		// reports failure for both (see k_fs_list()). Treat it as an
		// empty listing when the path is one we can otherwise reach,
		// which is the reading that makes an empty folder browsable
		// rather than a dead end. A genuinely bad path just shows an
		// empty folder too, which is a mild and self-correcting lie.
		count = 0;
		truncated = 0;

	}

	fl->count = 0;

	const char *p = stage_buf;

	for (uint32_t i = 0; i < count && fl->count < Z_FLIST_MAX; i++) {

		size_t l = strlen(p);

		const char *base = basename_of(p);

		// FatFs's f_readdir doesn't report "." or ".." (see
		// sw/os/fs/fatfs), but skip them defensively -- the ".." row
		// this widget draws is synthetic, and a second one coming
		// from the filesystem would be confusing rather than merely
		// redundant.
		if (!(base[0] == '.' && (base[1] == 0 ||
			(base[1] == '.' && base[2] == 0)))) {

			copy_bounded(fl->names[fl->count], base, Z_FLIST_NAME_MAX);
			fl->isdir[fl->count] = stage_types[i];
			fl->count++;

		}

		p += l + 1;

	}

	fl->truncated = truncated || (count >= Z_FLIST_MAX);

	sort_entries(fl);

	if (path && path[0]) copy_bounded(fl->path, path, Z_FLIST_PATH_MAX);
	else copy_bounded(fl->path, "/", Z_FLIST_PATH_MAX);

	// strip a trailing slash, except on the root itself -- every
	// path-building site below appends its own, and "//FOO" is ugly
	// even where FatFs tolerates it.
	{
		int l = (int)strlen(fl->path);
		while (l > 1 && fl->path[l - 1] == '/') fl->path[--l] = 0;
	}

	fl->top = 0;
	fl->sel = -1;
	fl->sel_updir = false;

	if (want[0]) {
		for (int i = 0; i < fl->count; i++) {
			if (!strcmp(fl->names[i], want)) {
				select_row(fl, i + (has_updir(fl) ? 1 : 0));
				break;
			}
		}
	}

	if (fl->sel < 0 && !fl->sel_updir && total_rows(fl) > 0)
		select_row(fl, 0);

	z_scrollbar_set_range(&fl->sb, total_rows(fl), visible_rows(fl));
	z_scrollbar_set_value(&fl->sb, fl->top);

	fl->dirty = true;

	return true;

}

// -- setup --

void z_flist_init(z_flist_t *fl, const z_win_t *win) {

	memset(fl, 0, sizeof(*fl));

	fl->win = win;
	fl->sel = -1;
	fl->last_press_row = -1;
	fl->dirty = true;

	copy_bounded(fl->path, "/", Z_FLIST_PATH_MAX);

	z_scrollbar_init(&fl->sb, win, Z_SB_VERT);

}

void z_flist_set_geom(z_flist_t *fl, int x, int y, int w, int h) {

	fl->x = (int16_t)x;
	fl->y = (int16_t)y;
	fl->w = (int16_t)w;
	fl->h = (int16_t)h;

	// Inset one pixel inside the widget's frame on all three sides.
	//
	// Flush against it, the scrollbar's own rect ends exactly on the
	// frame's right-hand column and spans its full height -- and
	// z_scrollbar_draw() BLANKS its whole rect before painting the
	// thumb, so it erased the right edge of the frame along with the
	// top-right and bottom-right corners every time it drew. The
	// frame is drawn first and the scrollbar last, so the scrollbar
	// always won.
	//
	// The row clip (z_flist_draw()) already stops one pixel short of
	// this, so nothing else has to move.
	z_scrollbar_set_geom(&fl->sb, x + w - Z_SB_THICK - 1, y + 1, h - 2);
	z_scrollbar_set_range(&fl->sb, total_rows(fl), visible_rows(fl));

	fl->dirty = true;

}

bool z_flist_chdir(z_flist_t *fl, const char *path) {
	return load_dir(fl, (path && path[0]) ? path : "/", NULL);
}

bool z_flist_refresh(z_flist_t *fl) {

	const char *keep = z_flist_selected(fl);
	char saved[Z_FLIST_NAME_MAX];

	saved[0] = 0;
	if (keep) copy_bounded(saved, keep, Z_FLIST_NAME_MAX);

	return load_dir(fl, fl->path, saved[0] ? saved : NULL);

}

void z_flist_invalidate(z_flist_t *fl) {
	fl->dirty = true;
	fl->sb.dirty = true;
}

// -- drawing --

void z_flist_draw(z_flist_t *fl, bool force) {

	if (!force && !fl->dirty) {
		z_scrollbar_draw(&fl->sb, false);
		return;
	}

	fl->dirty = false;

	z_clip_t content;
	z_win_content_rect(fl->win, &content);

	int x0 = content.x0 + fl->x;
	int y0 = content.y0 + fl->y;
	int x1 = x0 + fl->w - 1;
	int y1 = y0 + fl->h - 1;

	if (x1 < content.x0 || x0 > content.x1 ||
		y1 < content.y0 || y0 > content.y1) return;

	// Clip rows to the list area MINUS the scrollbar, not to the
	// whole widget: a long filename allowed to run under the
	// scrollbar reads as a rendering bug even though it's only ever
	// one or two glyphs.
	z_clip_t clip;
	clip.x0 = x0 + 1;
	clip.y0 = y0 + 1;
	clip.x1 = x1 - Z_SB_THICK - 1;
	clip.y1 = y1 - 1;

	if (clip.x0 < content.x0) clip.x0 = content.x0;
	if (clip.y0 < content.y0) clip.y0 = content.y0;
	if (clip.x1 > content.x1) clip.x1 = content.x1;
	if (clip.y1 > content.y1) clip.y1 = content.y1;

	// blank the list area, then frame the whole widget
	if (clip.x1 >= clip.x0 && clip.y1 >= clip.y0)
		z_fb_hw_fill_rect(clip.x0, clip.y0,
			clip.x1 - clip.x0 + 1, clip.y1 - clip.y0 + 1, 0);

	z_win_hw_box(fl->win, x0, y0, x1, y1, 1);

	int rows = visible_rows(fl);
	int total = total_rows(fl);
	int selrow = sel_row(fl);

	for (int r = 0; r < rows; r++) {

		int row = fl->top + r;
		if (row >= total) break;

		int ry = y0 + 1 + r * ROW_H;
		bool selected = (row == selrow);

		bool updir = has_updir(fl) && row == 0;
		int entry = row - (has_updir(fl) ? 1 : 0);

		if (selected) {
			// Solid selection bar, then the text drawn in reverse on
			// top of it -- see z_fb_draw_text2() (zgfx.h), which
			// exists for exactly this and costs the same as an
			// ordinary glyph blit.
			int sx1 = clip.x1;
			if (sx1 >= clip.x0)
				z_fb_hw_fill_rect(clip.x0, ry,
					sx1 - clip.x0 + 1,
					ry + ROW_H - 1 <= clip.y1 ? ROW_H : (clip.y1 - ry + 1), 1);
		}

		int icon = updir ? Z_ICON_UPDIR
			: (fl->isdir[entry] ? Z_ICON_FOLDER : Z_ICON_FILE);

		// The icon is 8px and the row is 10, so nudge it down one to
		// sit on the same optical line as the text rather than
		// hugging the row's top edge.
		z_fb_draw_icon(clip.x0 + 1, ry + 1, icon,
			selected ? 0 : 1, selected ? 1 : 0, &clip);

		const char *name = updir ? ".." : fl->names[entry];

		z_fb_draw_text2(clip.x0 + 1 + TEXT_X, ry + 1, name,
			selected ? 0 : 1, selected ? 1 : 0, &z_font_5x8, &clip);

	}

	z_scrollbar_draw(&fl->sb, true);

}

// -- input --

// display row under a content-relative point, or -1
static int row_at(const z_flist_t *fl, int cx, int cy) {

	// -1 to match the scrollbar's inset in z_flist_set_geom()
	if (cx < fl->x + 1 || cx >= fl->x + fl->w - Z_SB_THICK - 1) return -1;
	if (cy < fl->y + 1 || cy >= fl->y + fl->h - 1) return -1;

	int r = (cy - fl->y - 1) / ROW_H;
	int row = fl->top + r;

	if (r < 0 || r >= visible_rows(fl)) return -1;
	if (row >= total_rows(fl)) return -1;

	return row;

}

// Enters the selected directory, or goes up if the ".." row is
// selected. Returns true if the listing actually changed.
static bool enter_selection(z_flist_t *fl) {

	if (fl->sel_updir) {

		// Trim the last path component. The result keeps its leading
		// "/" and never becomes empty -- "/A" goes to "/", not "".
		char up[Z_FLIST_PATH_MAX];
		copy_bounded(up, fl->path, Z_FLIST_PATH_MAX);

		int l = (int)strlen(up);
		while (l > 1 && up[l - 1] != '/') l--;
		if (l > 1) l--;			// drop the slash itself, unless it's the root
		up[l ? l : 1] = 0;
		if (!up[0]) { up[0] = '/'; up[1] = 0; }

		// remember where we came from, so going up leaves the
		// directory we just left selected rather than dumping the
		// user at the top of the parent
		const char *from = basename_of(fl->path);
		char keep[Z_FLIST_NAME_MAX];
		copy_bounded(keep, from, Z_FLIST_NAME_MAX);

		return load_dir(fl, up, keep);

	}

	if (fl->sel < 0 || fl->sel >= fl->count) return false;
	if (!fl->isdir[fl->sel]) return false;

	char sub[Z_FLIST_PATH_MAX];
	int n = 0;

	for (const char *s = fl->path; *s && n < Z_FLIST_PATH_MAX - 1; s++)
		sub[n++] = *s;
	if (n > 0 && sub[n - 1] != '/' && n < Z_FLIST_PATH_MAX - 1)
		sub[n++] = '/';
	for (const char *s = fl->names[fl->sel]; *s && n < Z_FLIST_PATH_MAX - 1; s++)
		sub[n++] = *s;
	sub[n] = 0;

	return load_dir(fl, sub, NULL);

}

int z_flist_mouse(z_flist_t *fl, int cx, int cy, uint8_t buttons) {

	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool was_down = (fl->last_buttons & Z_MOUSE_BTN_LEFT) != 0;

	// The scrollbar gets first refusal, and keeps the pointer for the
	// whole of a drag it started -- otherwise sliding the thumb
	// sideways over the rows would start selecting them.
	if (z_scrollbar_has_pointer(&fl->sb, cx, cy)) {

		fl->last_buttons = buttons;

		if (z_scrollbar_mouse(&fl->sb, cx, cy, buttons)) {
			fl->top = (int)fl->sb.value;
			fl->dirty = true;
			z_flist_draw(fl, false);
		}

		return Z_FLIST_NONE;

	}

	// Keep the scrollbar's own edge detector fed even when the
	// pointer is elsewhere, so a button released outside it still
	// ends its drag.
	z_scrollbar_mouse(&fl->sb, cx, cy, buttons);

	fl->last_buttons = buttons;

	if (!(down && !was_down)) return Z_FLIST_NONE;

	int row = row_at(fl, cx, cy);
	if (row < 0) return Z_FLIST_NONE;

	uint32_t now = z_uptime_ticks();
	bool dbl = (row == fl->last_press_row) &&
		(now - fl->last_press_tick) < DBLCLICK_TICKS;

	fl->last_press_row = row;
	fl->last_press_tick = now;

	select_row(fl, row);
	z_flist_draw(fl, false);

	if (!dbl) return Z_FLIST_SELECTED;

	// Second click on the same row: activate. For a directory that
	// means navigating into it, which enter_selection() has already
	// done by the time we return -- see Z_FLIST_ACTIVATED's own note
	// in zflist.h and z_flist_activated_dir().
	fl->last_act_dir = z_flist_selected_is_dir(fl);

	if (fl->last_act_dir) {
		enter_selection(fl);
		z_flist_draw(fl, true);
		fl->last_press_row = -1;	// don't let a third click re-trigger
	}

	return Z_FLIST_ACTIVATED;

}

int z_flist_key(z_flist_t *fl, uint32_t keysym) {

	int row = sel_row(fl);
	if (row < 0) row = 0;

	int rows = visible_rows(fl);
	if (rows < 1) rows = 1;

	switch (keysym) {

		case Z_KEY_UP:
			select_row(fl, row - 1);
			z_flist_draw(fl, false);
			return Z_FLIST_SELECTED;

		case Z_KEY_DOWN:
			select_row(fl, row + 1);
			z_flist_draw(fl, false);
			return Z_FLIST_SELECTED;

		case Z_KEY_PAGEUP:
			select_row(fl, row - rows);
			z_flist_draw(fl, false);
			return Z_FLIST_SELECTED;

		case Z_KEY_PAGEDOWN:
			select_row(fl, row + rows);
			z_flist_draw(fl, false);
			return Z_FLIST_SELECTED;

		case Z_KEY_HOME:
			select_row(fl, 0);
			z_flist_draw(fl, false);
			return Z_FLIST_SELECTED;

		case Z_KEY_END:
			select_row(fl, total_rows(fl) - 1);
			z_flist_draw(fl, false);
			return Z_FLIST_SELECTED;

		case 0x0d:		// Enter -- see z_kbd_usage_to_keysym()
			fl->last_act_dir = z_flist_selected_is_dir(fl);
			if (fl->last_act_dir) {
				enter_selection(fl);
				z_flist_draw(fl, true);
			}
			return Z_FLIST_ACTIVATED;

		case 0x7f:		// Backspace (DEL, per zkbd.c) -- up a level
			if (has_updir(fl)) {
				fl->sel_updir = true;
				fl->sel = -1;
				enter_selection(fl);
				z_flist_draw(fl, true);
				return Z_FLIST_SELECTED;
			}
			return Z_FLIST_NONE;

		default:
			return Z_FLIST_NONE;

	}

}

// -- queries --

const char *z_flist_selected(const z_flist_t *fl) {

	if (fl->sel_updir) return NULL;
	if (fl->sel < 0 || fl->sel >= fl->count) return NULL;

	return fl->names[fl->sel];

}

bool z_flist_selected_path(const z_flist_t *fl, char *out, int outlen) {

	const char *name = z_flist_selected(fl);
	if (!name || !out || outlen < 2) return false;

	int n = 0;

	for (const char *s = fl->path; *s && n < outlen - 1; s++) out[n++] = *s;
	if (n > 0 && out[n - 1] != '/' && n < outlen - 1) out[n++] = '/';
	for (const char *s = name; *s && n < outlen - 1; s++) out[n++] = *s;

	out[n] = 0;

	// Ran out of room -- report failure rather than handing back a
	// silently truncated path that names a different file, or none.
	return (int)strlen(fl->path) + 1 + (int)strlen(name) < outlen;

}

bool z_flist_selected_is_dir(const z_flist_t *fl) {

	if (fl->sel_updir) return true;
	if (fl->sel < 0 || fl->sel >= fl->count) return false;

	return fl->isdir[fl->sel] != 0;

}

bool z_flist_activated_dir(const z_flist_t *fl) {
	// Recorded at activation time, NOT re-derived from the current
	// selection: activating a directory NAVIGATES INTO it, so by the
	// time the caller asks, the selection is some entry of the new
	// directory and answering from that would be answering a
	// different question entirely.
	return fl->last_act_dir;
}

bool z_flist_truncated(const z_flist_t *fl) {
	return fl->truncated;
}
