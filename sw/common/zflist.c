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
#include "zutf8.h"
#include "zsoc.h"		// Z_TICK_HZ, for type-to-find's timeout

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
// The listing's entry types, one byte each. The names themselves are
// listed straight into the widget's own pool -- see load_dir().
static uint8_t stage_types[Z_FLIST_MAX];

// One page of a listing read from a source (z_flist_load_step()), held
// while it is merged in: at most LOAD_PAGE entries a step.
#define LOAD_PAGE  64
static uint16_t page_off[LOAD_PAGE];
static uint8_t page_dir[LOAD_PAGE];
static z_fs_info_t page_info[LOAD_PAGE];

// -- the details column (fl->info) --

// How many characters of name a row must keep before the details
// column is shown at all. Below that the name matters more.
#define DETAIL_MIN_NAME  12

// "  12.3K 2026-09-26" into `out`; returns its length in characters.
// Sizes in the smallest unit that keeps them to four figures, dates
// as ISO (UTC -- docs/filesystem.md, "Timestamps"), a directory as
// "<dir>", and a missing date as blanks so the columns still line up.
// Appends `v` in decimal, at least `width` digits (zero-padded).
// Hand-rolled rather than snprintf(): every app with a file dialog
// links this widget, and a formatted print is several kilobytes an app
// may not otherwise carry.
static char *put_u(char *o, uint32_t v, int width) {
	char t[10];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n < width) t[n++] = '0';
	while (n) *o++ = t[--n];
	return o;
}

static int details_text(const z_fs_info_t *fi, char *out) {

	char sz[8], *o = sz;
	uint32_t v = fi->size;
	char unit = 0;

	if (fi->type == Z_FS_TYPE_DIR) {
		memcpy(sz, "<dir>", 6);
		o = sz + 5;
	} else {
		// Tenths of a KB, then of a MB, rounded to nearest and split
		// so that a 4GB file (FAT32's limit) does not overflow.
		if (v >= 10000) {
			unit = 'K';
			v = (fi->size / 1024) * 10 + ((fi->size % 1024) * 10 + 512) / 1024;
			if (v >= 10000) {
				unit = 'M';
				v = (fi->size >> 20) * 10 +
					(((fi->size & 0xFFFFF) >> 10) * 10 + 512) / 1024;
			}
		}
		if (unit && v < 1000) {
			o = put_u(o, v / 10, 1); *o++ = '.'; o = put_u(o, v % 10, 1);
		} else {
			o = put_u(o, unit ? (v + 5) / 10 : v, 1);
		}
		if (unit) *o++ = unit;
	}
	*o = 0;

	// right-align the size in six columns, then the date
	int n = (int)(o - sz);
	char *p = out;
	for (int i = n; i < 6; i++) *p++ = ' ';
	memcpy(p, sz, (size_t)n); p += n;
	*p++ = ' ';
	if (fi->fdate) {
		p = put_u(p, 1980 + (fi->fdate >> 9), 4); *p++ = '-';
		p = put_u(p, (fi->fdate >> 5) & 15, 2); *p++ = '-';
		p = put_u(p, fi->fdate & 31, 2);
	} else {
		for (int i = 0; i < 10; i++) *p++ = ' ';
	}
	*p = 0;
	return (int)(p - out);

}

const z_fs_info_t *z_flist_selected_info(const z_flist_t *fl) {
	if (!fl->info || fl->sel_updir || fl->sel < 0 || fl->sel >= fl->count)
		return NULL;
	return &fl->info[fl->sel];
}

// Entry i's name, in the pool.
#define NAME(fl, i)  ((fl)->pool + (fl)->name_off[i])

// Double-click window, in kernel ticks. Z_TICK_HZ is 732 (zsoc.h), so
// this is a bit over a third of a second -- deliberately generous,
// since the two clicks are being made with whatever pointing device
// happens to be plugged into a USB port, not a calibrated mouse.
#define DBLCLICK_TICKS  Z_DOUBLE_CLICK_TICKS	// zwm.h -- shared with the titlebar

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
// sort over a bounded array: Z_FLIST_MAX is 256, this runs once per
// directory change, and anything cleverer would be more code than the
// thing it replaces.
// Does entry (dir_a, a) sort after (dir_b, b)? Directories first, then
// names with ASCII folded -- strcasecmp isn't available on every libc
// this tree builds against. Other UTF-8 compares by byte, which keeps
// each script's letters together, in Unicode order.
static bool sorts_after(uint8_t dir_a, const char *a, uint8_t dir_b, const char *b) {
	if (dir_a != dir_b) return dir_b && !dir_a;
	while (*a || *b) {
		unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 32);
		if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 32);
		if (ca != cb) return ca > cb;
		a++; b++;
	}
	return false;
}

// Entries [from, to) sorted in place, by moving their 2-byte offsets
// (and details); the names stay where they are in the pool.
static void sort_range(z_flist_t *fl, int from, int to) {

	for (int i = from + 1; i < to; i++) {

		uint16_t off = fl->name_off[i];
		uint8_t dir = fl->isdir[i];
		const char *name = fl->pool + off;
		z_fs_info_t inf;
		if (fl->info) inf = fl->info[i];

		int j = i - 1;

		// move each earlier entry up while it sorts AFTER this one
		while (j >= from && sorts_after(fl->isdir[j], NAME(fl, j), dir, name)) {
			fl->name_off[j + 1] = fl->name_off[j];
			fl->isdir[j + 1] = fl->isdir[j];
			if (fl->info) fl->info[j + 1] = fl->info[j];
			j--;
		}

		fl->name_off[j + 1] = off;
		fl->isdir[j + 1] = dir;
		if (fl->info) fl->info[j + 1] = inf;

	}

}

static void sort_entries(z_flist_t *fl) {
	sort_range(fl, 0, fl->count);
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

// -- reading a listing from a source, a page at a time --

static void set_path(z_flist_t *fl, const char *path) {
	if (path && path[0]) copy_bounded(fl->path, path, Z_FLIST_PATH_MAX);
	else copy_bounded(fl->path, "/", Z_FLIST_PATH_MAX);
	int l = (int)strlen(fl->path);
	while (l > 1 && fl->path[l - 1] == '/') fl->path[--l] = 0;
}

void z_flist_load_stop(z_flist_t *fl) {
	if (!fl->loading) return;
	fl->loading = false;
	if (fl->src && fl->src->close) fl->src->close(fl->src->ctx);
}

// Merges the page at [from, count), sorted, into the sorted [0, from):
// from the back, so each entry moves once. The selected entry stays
// selected, at the same row on the screen.
static void merge_page(z_flist_t *fl, int from) {

	int k = fl->count - from;
	if (k <= 0) return;

	sort_range(fl, from, fl->count);

	int sel_off = fl->sel >= 0 ? fl->name_off[fl->sel] : -1;
	int old_sel = fl->sel;

	for (int i = 0; i < k; i++) {
		page_off[i] = fl->name_off[from + i];
		page_dir[i] = fl->isdir[from + i];
		if (fl->info) page_info[i] = fl->info[from + i];
	}

	int i = from - 1, j = k - 1, w = fl->count - 1;
	while (j >= 0) {
		if (i >= 0 && sorts_after(fl->isdir[i], NAME(fl, i), page_dir[j], fl->pool + page_off[j])) {
			fl->name_off[w] = fl->name_off[i];
			fl->isdir[w] = fl->isdir[i];
			if (fl->info) fl->info[w] = fl->info[i];
			i--;
		} else {
			fl->name_off[w] = page_off[j];
			fl->isdir[w] = page_dir[j];
			if (fl->info) fl->info[w] = page_info[j];
			j--;
		}
		w--;
	}

	if (sel_off >= 0) {
		for (int n = 0; n < fl->count; n++)
			if (fl->name_off[n] == (uint16_t)sel_off) { fl->sel = n; break; }
		// The first row is where a listing starts: it stays the first,
		// whatever sorts in before it. Anything further down, the person
		// has moved to: that entry stays selected, where it is on the
		// screen.
		if (old_sel == 0 && fl->top == 0) fl->sel = 0;
		else fl->top += fl->sel - old_sel;
	}

}

// Reads up to `entries` from the source into the store, sorted in.
// False once the listing has ended (or the store is full and cannot
// grow).
static bool load_more(z_flist_t *fl, int entries) {

	if (!fl->loading) return false;
	if (entries > LOAD_PAGE) entries = LOAD_PAGE;

	int from = fl->count;
	bool more = true;

	for (int n = 0; n < entries; n++) {

		const char *name;
		uint32_t nl;
		z_fs_info_t inf;

		if (!fl->src->next(fl->src->ctx, &name, &nl, &inf)) { more = false; break; }
		if ((name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))))
			continue;
		if (nl >= Z_FLIST_NAME_MAX) nl = Z_FLIST_NAME_MAX - 1;

		// room: the store, grown by the app if it can be
		while (fl->count >= fl->cap || fl->pool_used + nl + 1 > fl->pool_cap) {
			// the page so far into place first: grow() may move the store
			merge_page(fl, from);
			from = fl->count;
			if (!fl->src->grow || !fl->src->grow(fl->src->ctx, fl)) {
				fl->truncated = true;
				more = false;
				break;
			}
		}
		if (!more) break;

		memcpy(fl->pool + fl->pool_used, name, nl);
		fl->pool[fl->pool_used + nl] = 0;
		fl->name_off[fl->count] = (uint16_t)fl->pool_used;
		fl->isdir[fl->count] = inf.type;
		if (fl->info) fl->info[fl->count] = inf;
		fl->pool_used += nl + 1;
		fl->count++;

	}

	merge_page(fl, from);

	// the name to select, once it is in
	if (fl->want[0]) {
		for (int i = 0; i < fl->count; i++)
			if (!strcmp(NAME(fl, i), fl->want)) {
				select_row(fl, i + (has_updir(fl) ? 1 : 0));
				fl->want[0] = 0;
				break;
			}
	}

	if (fl->sel < 0 && !fl->sel_updir && total_rows(fl) > 0) select_row(fl, 0);

	{
		int rows = visible_rows(fl), total = total_rows(fl);
		if (fl->top > total - rows) fl->top = total - rows;
		if (fl->top < 0) fl->top = 0;
	}
	z_scrollbar_set_range(&fl->sb, total_rows(fl), visible_rows(fl));
	z_scrollbar_set_value(&fl->sb, fl->top);
	fl->dirty = true;
	fl->rows_dirty = true;

	if (!more) z_flist_load_stop(fl);
	return more;

}

static bool load_begin(z_flist_t *fl, const char *path, const char *want) {

	z_flist_load_stop(fl);

	set_path(fl, path);
	fl->count = 0;
	fl->pool_used = 0;
	fl->truncated = false;
	fl->top = 0;
	fl->sel = -1;
	fl->sel_updir = false;
	fl->find_len = 0;
	copy_bounded(fl->want, want ? want : "", Z_FLIST_NAME_MAX);

	// An empty or unreadable folder reads as empty, as with FS_LIST
	fl->loading = fl->src->open(fl->src->ctx, fl->path);
	if (fl->loading) load_more(fl, LOAD_PAGE);
	else {
		if (total_rows(fl) > 0) select_row(fl, 0);
		z_scrollbar_set_range(&fl->sb, total_rows(fl), visible_rows(fl));
		z_scrollbar_set_value(&fl->sb, 0);
		fl->dirty = true;
		fl->rows_dirty = true;
	}
	return true;

}

bool z_flist_load_step(z_flist_t *fl, int entries) {
	return load_more(fl, entries);
}

void z_flist_set_source(z_flist_t *fl, const z_flist_source_t *src) {
	z_flist_load_stop(fl);
	fl->src = src;
}

void z_flist_set_store(z_flist_t *fl, char *pool, uint32_t pool_cap,
	uint16_t *name_off, uint8_t *isdir, z_fs_info_t *info, int cap) {
	if (pool_cap > 65535) pool_cap = 65535;
	fl->pool = pool;
	fl->pool_cap = pool_cap;
	fl->name_off = name_off;
	fl->isdir = isdir;
	fl->info = info;
	fl->cap = cap;
	if (fl->count > cap) fl->count = cap;
}

// Reads a directory into the widget. `keep` (may be NULL) names an
// entry to re-select afterwards if it still exists.
static bool load_dir(z_flist_t *fl, const char *path, const char *keep) {

	char want[Z_FLIST_NAME_MAX];
	want[0] = 0;
	if (keep) copy_bounded(want, keep, Z_FLIST_NAME_MAX);

	uint32_t count = 0, truncated = 0;

	// Listed straight into the pool. Each entry arrives as a full path;
	// it is cut down to its name and moved to the write position, which
	// never passes the read position -- a name is never longer than
	// the path it came in -- so this needs no second buffer.
	// With a details column the listing carries each entry's size and
	// date as well (FS_LIST_EX); the type comes from that too.
	// Through a source: the first page now, the rest from
	// z_flist_load_step().
	if (fl->src) return load_begin(fl, path, want);

	int max = fl->info ? fl->cap : (fl->cap < Z_FLIST_MAX ? fl->cap : Z_FLIST_MAX);
	int listed = fl->info
		? fs_list_ex(path, fl->pool, fl->pool_cap, fl->info,
			(uint32_t)max, &count, &truncated)
		: fs_list_into(path, fl->pool, fl->pool_cap, stage_types,
			(uint32_t)max, &count, &truncated);

	if (!listed) {

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

	const char *p = fl->pool;
	size_t w = 0;

	for (uint32_t i = 0; i < count && fl->count < max; i++) {

		size_t l = strlen(p);

		const char *base = basename_of(p);
		size_t bl = l - (size_t)(base - p);

		// FatFs's f_readdir doesn't report "." or ".." (see
		// sw/apps/fs/fatfs), but skip them defensively -- the ".." row
		// this widget draws is synthetic, and a second one coming
		// from the filesystem would be confusing rather than merely
		// redundant.
		if (!(base[0] == '.' && (base[1] == 0 ||
			(base[1] == '.' && base[2] == 0)))) {

			memmove(fl->pool + w, base, bl + 1);
			fl->name_off[fl->count] = (uint16_t)w;
			if (fl->info) {
				// compacted in step with the names ("." and ".."
				// skipped above): entry fl->count's details
				fl->info[fl->count] = fl->info[i];
				fl->isdir[fl->count] = fl->info[i].type;
			} else {
				fl->isdir[fl->count] = stage_types[i];
			}
			fl->count++;
			w += bl + 1;

		}

		p += l + 1;

	}

	fl->truncated = truncated || (count >= (uint32_t)max);

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
			if (!strcmp(NAME(fl, i), want)) {
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
	fl->rows_dirty = true;		// another listing: any row may differ
	fl->find_len = 0;

	return true;

}

// -- setup --

void z_flist_init(z_flist_t *fl, const z_win_t *win) {

	memset(fl, 0, sizeof(*fl));

	fl->pool = fl->pool_store;
	fl->name_off = fl->name_off_store;
	fl->isdir = fl->isdir_store;
	fl->cap = Z_FLIST_MAX;
	fl->pool_cap = Z_FLIST_POOL;

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
	fl->drawn = false;			// moved or resized: paint it whole

}

bool z_flist_chdir(z_flist_t *fl, const char *path) {
	return load_dir(fl, (path && path[0]) ? path : "/", NULL);
}

// Re-reads the current directory and selects `name` if it is there
// (a file just renamed or copied), else the first row.
bool z_flist_refresh_select(z_flist_t *fl, const char *name) {
	char path[Z_FLIST_PATH_MAX];
	copy_bounded(path, fl->path, Z_FLIST_PATH_MAX);
	return load_dir(fl, path, name);
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
	fl->drawn = false;
	fl->sb.dirty = true;
}

// -- drawing --

// Fills the inclusive rect clamped to `c`; nothing if it is empty.
// z_fb_hw_fill_rect() clamps to the screen only.
static void fill_clamped(const z_clip_t *c, int x0, int y0, int x1, int y1,
	int color) {
	if (x0 < c->x0) x0 = c->x0;
	if (y0 < c->y0) y0 = c->y0;
	if (x1 > c->x1) x1 = c->x1;
	if (y1 > c->y1) y1 = c->y1;
	if (x1 < x0 || y1 < y0) return;
	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);
}

// Paints visible row `r` (0 = the top one shown) whole, in place:
// icon, name and details as two-colour cells, and the background in
// the gaps between them, so every pixel of the row is written once and
// nothing is blanked first. A row past the end is painted background.
// `untint`: also take the desktop colour off an unselected row -- a
// whole draw has untinted the list already.
static void paint_row(const z_flist_t *fl, int y0, const z_clip_t *clip,
	int r, bool untint) {

	if (clip->x1 < clip->x0) return;

	int row = fl->top + r;
	int ry = y0 + 1 + r * ROW_H;
	int ry1 = ry + ROW_H - 1;
	if (ry > clip->y1) return;
	if (ry1 > clip->y1) ry1 = clip->y1;

	if (row < 0 || row >= total_rows(fl)) {
		fill_clamped(clip, clip->x0, ry, clip->x1, ry1, 0);
		if (untint && z_win_accent_ok())
			z_win_accent_rect(fl->win, clip->x0, ry, clip->x1, ry1, false);
		return;
	}

	bool selected = (row == sel_row(fl));
	bool updir = has_updir(fl) && row == 0;
	int entry = row - (has_updir(fl) ? 1 : 0);
	int fg = selected ? 0 : 1, bg = selected ? 1 : 0;
	int ty0 = ry + 1, ty1 = ry + z_font_5x8.h;		// the glyph band

	// Above and below the glyph band, the full width.
	fill_clamped(clip, clip->x0, ry, clip->x1, ry, bg);
	fill_clamped(clip, clip->x0, ty1 + 1, clip->x1, ry1, bg);

	// The icon is 8px and the row is 10, so it sits one down, on the
	// same optical line as the text rather than hugging the row's top.
	int ix = clip->x0 + 1;
	int nx = clip->x0 + 1 + TEXT_X;
	fill_clamped(clip, clip->x0, ty0, ix - 1, ty1, bg);
#ifdef Z_GFX_HW_BLIT
	// The blit writes both colours, so the cell needs no fill -- unless
	// it is clipped, which z_fb_draw_icon() answers by drawing nothing.
	if (ix + Z_ICON_W - 1 > clip->x1 || ty0 + Z_ICON_H - 1 > clip->y1)
		fill_clamped(clip, ix, ty0, ix + Z_ICON_W - 1, ty1, bg);
#else
	// No icon blitter (zgfx.h: a no-op): the cell is background.
	fill_clamped(clip, ix, ty0, ix + Z_ICON_W - 1, ty1, bg);
#endif
	z_fb_draw_icon(ix, ty0, updir ? Z_ICON_UPDIR
		: (fl->isdir[entry] ? Z_ICON_FOLDER : Z_ICON_FILE), fg, bg, clip);
	fill_clamped(clip, ix + Z_ICON_W, ty0, nx - 1, ty1, bg);

	const char *name = updir ? ".." : NAME(fl, entry);

	// The details column, right-aligned, when there is one and the
	// row is wide enough that the name keeps DETAIL_MIN_NAME
	// characters in front of it. The name is then clipped short of
	// the column rather than running underneath it.
	z_clip_t nclip = *clip;
	int rest = clip->x1;		// last pixel the name's background reaches
	if (fl->info && !updir) {
		char det[24];
		int dw = details_text(&fl->info[entry], det);
		int dx = clip->x1 - dw * z_font_5x8.w;
		if (dx - nx >= DETAIL_MIN_NAME * z_font_5x8.w) {
			z_fb_draw_text2(dx, ty0, det, fg, bg, &z_font_5x8, clip);
			fill_clamped(clip, dx + dw * z_font_5x8.w, ty0, clip->x1, ty1, bg);
			nclip.x1 = dx - z_font_5x8.w;
			rest = dx - 1;
		}
	}

	// UTF-8: a long file name can be anything (docs/sdcard.md).
	int nend = nx + z_utf8_cols(name, strlen(name)) * z_font_5x8.w;
	if (nend > nclip.x1 + 1) nend = nclip.x1 + 1;
	z_fb_draw_utf8_2(nx, ty0, name, fg, bg, &z_font_5x8, &nclip);
	fill_clamped(clip, nend, ty0, rest, ty1, bg);

	// Desktop colour: the selected row tinted (zwin.h,
	// z_win_accent_rect()), a row that just lost it untinted.
	if ((selected || untint) && z_win_accent_ok())
		z_win_accent_rect(fl->win, clip->x0, ry, clip->x1, ry1, selected);

}

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
		y1 < content.y0 || y0 > content.y1) {
		fl->drawn = false;
		return;
	}

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

	int rows = visible_rows(fl);
	int total = total_rows(fl);
	int selrow = sel_row(fl);

	if (!force && fl->drawn) {

		// -- in place: only the rows that change, nothing blanked --
		//
		// Every draw used to blank the list and paint it again, and with
		// no back buffer that showed as a flash on every arrow key and
		// on every folder opened. A row is now painted whole, once, over
		// what is there.
		if (fl->rows_dirty || fl->top != fl->drawn_top) {
			for (int r = 0; r < rows; r++) {
				// empty now and empty before: background already
				if (fl->top + r >= total && fl->drawn_top + r >= fl->drawn_total)
					continue;
				paint_row(fl, y0, &clip, r, true);
			}
		} else if (selrow != fl->drawn_sel) {
			int s0 = fl->drawn_sel - fl->top, s1 = selrow - fl->top;
			if (s0 >= 0 && s0 < rows) paint_row(fl, y0, &clip, s0, true);
			if (s1 >= 0 && s1 < rows) paint_row(fl, y0, &clip, s1, true);
		}

		z_scrollbar_draw(&fl->sb, false);

	} else {

		// -- whole: after the window was cleared, or the first time --

		if (clip.x1 >= clip.x0 && clip.y1 >= clip.y0)
			z_fb_hw_fill_rect(clip.x0, clip.y0,
				clip.x1 - clip.x0 + 1, clip.y1 - clip.y0 + 1, 0);

		z_win_hw_box(fl->win, x0, y0, x1, y1, 1);

		// Desktop colour: the rows untinted, then the selected one
		// tinted as it is drawn (paint_row()).
		if (clip.x1 >= clip.x0 && clip.y1 >= clip.y0 && z_win_accent_ok())
			z_win_accent_rect(fl->win, clip.x0, clip.y0, clip.x1, clip.y1, false);

		for (int r = 0; r < rows && fl->top + r < total; r++)
			paint_row(fl, y0, &clip, r, false);

		z_scrollbar_draw(&fl->sb, true);

	}

	fl->drawn = true;
	fl->rows_dirty = false;
	fl->drawn_top = fl->top;
	fl->drawn_sel = selrow;
	fl->drawn_total = total;

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
	for (const char *s = NAME(fl, fl->sel); *s && n < Z_FLIST_PATH_MAX - 1; s++)
		sub[n++] = *s;
	sub[n] = 0;

	return load_dir(fl, sub, NULL);

}

void z_flist_wheel(z_flist_t *fl, int notches) {

	int rows = visible_rows(fl);
	int max = total_rows(fl) - rows;
	int top = fl->top - 3 * notches;		// up (positive) is back

	if (max < 0) max = 0;
	if (top > max) top = max;
	if (top < 0) top = 0;
	if (top == fl->top) return;

	fl->top = top;
	z_scrollbar_set_value(&fl->sb, fl->top);
	fl->dirty = true;
	z_flist_draw(fl, false);

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

	// Painted once, in place (z_flist_draw()). This was a forced
	// draw straight after the click's own one: the list blanked and
	// was drawn again, a visible flash for every folder opened.
	if (fl->last_act_dir) {
		enter_selection(fl);
		z_flist_draw(fl, false);
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
				z_flist_draw(fl, false);	// in place; see z_flist_mouse()
			}
			return Z_FLIST_ACTIVATED;

		case 0x7f:		// Backspace (DEL, per zkbd.c) -- up a level
			if (has_updir(fl)) {
				fl->sel_updir = true;
				fl->sel = -1;
				enter_selection(fl);
				z_flist_draw(fl, false);
				return Z_FLIST_SELECTED;
			}
			return Z_FLIST_NONE;

		default:
			return Z_FLIST_NONE;

	}

}

// -- type-to-find --
//
// z_listbox_key()'s rules (zwidget.c, lb_find()), over the entries:
// a letter alone goes to the FIRST entry starting with it, or to the
// next one if the selection already starts with it; letters less than
// a second apart build a prefix matched from the top. ASCII is folded,
// other bytes compare as they are. The ".." row is never a match.

#define FIND_TICKS  Z_TICK_HZ

static char fold(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool has_prefix(const char *t, const char *p, int n) {
	for (int i = 0; i < n; i++)
		if (!t[i] || fold(t[i]) != fold(p[i])) return false;
	return true;
}

int z_flist_find_key(z_flist_t *fl, uint32_t keysym) {

	if (!((keysym >= 'a' && keysym <= 'z') || (keysym >= 'A' && keysym <= 'Z') ||
		(keysym >= '0' && keysym <= '9') || keysym == '_' || keysym == '-' ||
		keysym == '.' || (keysym == ' ' && fl->find_len > 0)))
		return Z_FLIST_NONE;
	if (fl->count <= 0) return Z_FLIST_NONE;

	char c = (char)keysym;
	uint32_t now = z_uptime_ticks();
	bool fresh = fl->find_len == 0 || (now - fl->find_tick) > FIND_TICKS;
	bool same = fl->find_len == 1 && fold(fl->find[0]) == fold(c);
	int to = -1;

	fl->find_tick = now;

	if (fresh || same) {
		fl->find[0] = c;
		fl->find_len = 1;
		bool on_it = fl->sel >= 0 && has_prefix(NAME(fl, fl->sel), fl->find, 1);
		int start = on_it ? fl->sel + 1 : 0;
		for (int i = 0; i < fl->count && to < 0; i++) {
			int e = (start + i) % fl->count;
			if (has_prefix(NAME(fl, e), fl->find, 1)) to = e;
		}
	} else {
		if (fl->find_len < sizeof(fl->find)) fl->find[fl->find_len++] = c;
		for (int e = 0; e < fl->count && to < 0; e++)
			if (has_prefix(NAME(fl, e), fl->find, fl->find_len)) to = e;
	}

	if (to < 0 || (to == fl->sel && !fl->sel_updir)) return Z_FLIST_NONE;

	select_row(fl, to + (has_updir(fl) ? 1 : 0));
	z_flist_draw(fl, false);
	return Z_FLIST_SELECTED;

}

// -- queries --

const char *z_flist_selected(const z_flist_t *fl) {

	if (fl->sel_updir) return NULL;
	if (fl->sel < 0 || fl->sel >= fl->count) return NULL;

	return NAME(fl, fl->sel);

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
