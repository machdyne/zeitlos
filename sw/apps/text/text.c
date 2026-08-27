/*
 * text -- a notepad
 *
 * A plain, fast, word-wrapping text editor. Deliberately NOT a
 * programmer's editor: sw/apps/repl already embeds `te`
 * (docs/editor.md), which is modal, vi-flavored and built around
 * lines as the unit of everything. This one is for notes, letters and
 * stories -- so it wraps, it has no modes, and every key that isn't a
 * command inserts itself.
 *
 *   > run wm
 *   > run text
 *
 * -- layout --
 *
 *   +--------------------------------+-+
 *   | the quick brown fox jumps over |#|
 *   | the lazy dog. the quick brown  | |
 *   | fox jumps over the lazy dog.   | |
 *   |                                | |
 *   +--------------------------------+-+
 *
 * Text on the left, one vertical scrollbar on the right
 * (z_scrollbar_t, sw/common/zwidget.h). The window is resizable;
 * everything is recomputed from its size in layout(), and a resize
 * rewraps the document rather than scrolling it.
 *
 * -- the document --
 *
 * One flat char array, TEXT_MAX bytes, with insert and delete done by
 * memmove. Not a gap buffer, and that is a choice rather than an
 * oversight: at 32KB the worst-case move is the whole buffer, which
 * is a few milliseconds on this CPU and only happens when typing at
 * the very start of a full document. A gap buffer would remove that
 * and add a second representation of "where the text is" that every
 * function here would have to understand. If typing ever feels heavy
 * on real hardware, this is the first thing to change, and it is a
 * change confined to buf_insert()/buf_delete().
 *
 * -- wrapping --
 *
 * line_off[] holds the buffer offset each display line starts at,
 * recomputed by wrap_from(). The important property is that WRAPPING
 * IS PARAGRAPH-LOCAL: where a line breaks depends only on the text
 * since the last '\n', so an edit can never change how anything
 * before its own paragraph is laid out. Every edit therefore rewraps
 * from the start of its own paragraph rather than from the top of the
 * document, which is what keeps typing cheap in a long file.
 *
 * -- drawing --
 *
 * Every glyph goes through the hardware glyph blitter, via
 * z_win_draw_text() (-DZ_GFX_HW_BLIT, see the Makefile). Text is
 * drawn a display line at a time, and an edit repaints only from the
 * edited line down -- reflowing a paragraph can move every line after
 * it, but never one before.
 *
 * Two fonts are available -- z_font_5x8 and z_font_6x12 -- and the
 * titlebar font button switches between them. Both are resident in
 * glyph memory at fixed offsets (glyph_layout[] in sw/common/zgfx.c),
 * loaded once by wm, so both render through the hardware blitter.
 * Switching rewraps the document, because the column count changes.
 *
 * -- not implemented --
 *
 * No selection, no clipboard, no undo, no search. Each of those wants
 * a real design rather than a corner of this file, and none is needed
 * to write a note.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"

// -- the document --

// 32KB. Large enough for anything anyone is going to type into a
// notepad on an FPGA, small enough to memmove without thinking about
// it, and it leaves room for the line table below inside an app image
// that also has to fit a file dialog.
//
// The buffer is NOT NUL-terminated as a rule -- `len` is the length,
// and the text may contain any byte. Anything that hands a string to
// a drawing call copies out a bounded piece first.
#define TEXT_MAX     32768

// Display lines, not paragraphs. 4096 is unreachable in practice for
// a 32KB document of prose (which wraps to a few hundred), and is
// only approached by pathological input -- a file of nothing but
// newlines. At the cap the document simply stops being displayed
// past that point; it is not truncated on disk and editing above the
// cap still works. Cost is 8KB of .bss.
#define MAX_LINES    4096

static char buf[TEXT_MAX];
static int len;
static int cursor;			// offset into buf, 0..len
static bool modified;

// -- selection --
//
// An ANCHOR plus the cursor. The selected range is whichever order
// they happen to be in, so extending a selection backwards needs no
// special case and the caret stays the moving end -- which is what
// makes shift+arrow and shift+drag feel right.
//
// -1 means no selection. Deliberately not "anchor == cursor": a
// selection can legitimately collapse to zero width mid-drag without
// ceasing to exist, and treating that as "no selection" makes the
// next shift+arrow start from the wrong place.
static int sel_anchor = -1;

// True between a button press in the text area and its release, so a
// pointer sample can tell "start a selection here" from "extend the
// one in progress".
static bool selecting;

// Modifier state from the last key event, so a shift+CLICK can be
// told from a plain one -- Z_WM_MOUSE carries buttons but no
// modifiers (see zwm.h), so the two have to be correlated here.
static uint8_t mods_now;

// Previous button mask, for edge detection on the right button --
// Z_WM_MOUSE is a level report, not an event, so a held button
// arrives on every sample.
static uint8_t last_buttons;

static bool has_sel(void) {
	return sel_anchor >= 0 && sel_anchor != cursor;
}

static int sel_start(void) {
	return sel_anchor < cursor ? sel_anchor : cursor;
}

static int sel_end(void) {
	return sel_anchor < cursor ? cursor : sel_anchor;
}

// Drops the selection. Returns true if there was one to drop, so a
// caller can tell whether a repaint is needed.
static bool sel_clear(void) {
	if (sel_anchor < 0) return false;
	bool had = has_sel();
	sel_anchor = -1;
	return had;
}

// Offset of the first byte of each display line. uint16_t is exactly
// wide enough for a TEXT_MAX of 32768 and half the size of the
// obvious alternative -- if TEXT_MAX ever goes to 64K this has to
// become uint32_t, since an offset of 65536 would wrap to 0 here and
// the failure would look like the document silently folding in half.
static uint16_t line_off[MAX_LINES];
static int nlines;

static char filename[80];	// "" when the document has never been saved

// -- window --

// Minimum useful size, pinned as the floor by Z_WIN_FLAG_MIN_IS_CREATE
// -- roughly 50 columns and two dozen lines, below which wrapping
// stops producing anything readable. Also comfortably wide enough for
// four titlebar icons (new/open/save/close) plus a filename beside
// them; wm drops icons that don't fit rather than overlapping the
// title, so a narrower floor would silently lose buttons.
#define WIN_W   320
#define WIN_H   240

static z_win_t win;

// -- layout, recomputed on every resize --

// The font in use, and the metrics derived from it. Not constants any
// more: the titlebar font toggle (Z_WM_TBICON_FONT, zwm.h) switches
// between the two fonts wm keeps resident in glyph memory, and
// everything about the layout -- columns, rows, where the caret goes,
// which document offset a click lands on -- follows from these.
//
// Both are resident, so both still render through the hardware glyph
// blitter; see glyph_layout[] in sw/common/zgfx.c. A font that wasn't
// resident would silently fall back to software and still be correct,
// just slow.
static const z_font_t *cur_font = &z_font_5x8;

#define CHAR_W      (cur_font->w)
#define LINE_H      (cur_font->h + 1)
#define TEXT_X0     2			// left margin, content-relative

static int cols;			// characters per display line
static int rows;			// display lines visible at once
static int text_w;			// width of the text area, excluding scrollbar

static z_scrollbar_t sbar;

static int top_line;		// first visible display line

// -- buffer primitives --

static void buf_clear(void) {
	len = 0;
	cursor = 0;
	modified = false;
	filename[0] = 0;
}

// Inserts one byte at `at`. Returns false if the buffer is full --
// which the caller must not ignore, since silently dropping
// keystrokes at 32KB is exactly the kind of thing that gets blamed on
// the keyboard.
static bool buf_insert(int at, char c) {

	if (len >= TEXT_MAX) return false;
	if (at < 0 || at > len) return false;

	memmove(&buf[at + 1], &buf[at], (size_t)(len - at));
	buf[at] = c;
	len++;

	return true;

}

static void buf_delete(int at) {

	if (at < 0 || at >= len) return;

	memmove(&buf[at], &buf[at + 1], (size_t)(len - at - 1));
	len--;

}

// -- word wrap --

// Where the line starting at `start` ends -- i.e. the offset the NEXT
// line begins at.
//
// A line that ends at a '\n' INCLUDES that newline, so line starts
// always land on real text and the caller never has to special-case
// "is the character before me a newline". A line broken by wrapping
// includes the space it broke after, for the same reason: every byte
// of the document belongs to exactly one display line.
static int wrap_one(int start) {

	if (start >= len) return len;
	if (cols < 1) return len;

	int i = start;
	int last_space = -1;
	int count = 0;

	while (i < len && count < cols) {
		char c = buf[i];
		if (c == '\n') return i + 1;
		if (c == ' ') last_space = i;
		i++;
		count++;
	}

	if (i >= len) return len;

	// The character that didn't fit is a newline -- it ends this line
	// anyway, and there is nothing to wrap.
	if (buf[i] == '\n') return i + 1;

	// Break after the last space, if this line had one. Otherwise
	// this is a single word longer than the whole line, and the only
	// options are to break it mid-word or to let it run off the edge.
	// Breaking is the lesser evil: a URL or a line of code should
	// still be READABLE, even if it isn't pretty.
	int b = (last_space >= start) ? last_space + 1 : i;

	// Absorb the whole run of spaces at the break, so a line can
	// never BEGIN with one.
	//
	// Without this, two ordinary cases produce a line holding
	// nothing but spaces, which renders as a blank line in the middle
	// of a paragraph: a word that exactly fills the width followed by
	// a space, and a double space after a full stop that happens to
	// straddle the break. The absorbed spaces make the line longer
	// than `cols`, which is fine and is why line_col_max() exists --
	// trailing spaces are invisible, and draw_row() clamps what it
	// actually paints.
	while (b < len && buf[b] == ' ') b++;
	if (b < len && buf[b] == '\n') b++;

	return b;

}

// Rebuilds line_off[] from display line `from` onward. line_off[from]
// must already be correct -- which is what makes this safe to call
// with the line at the start of an edited paragraph: wrapping is
// paragraph-local, so that offset cannot have moved.
static void wrap_from(int from) {

	if (from < 0) from = 0;
	if (from > nlines) from = nlines;

	int off = (from < nlines) ? (int)line_off[from] : 0;
	if (from == 0) off = 0;

	nlines = from;

	for (;;) {

		if (nlines >= MAX_LINES) break;

		line_off[nlines++] = (uint16_t)off;

		if (off >= len) break;			// this is the trailing empty line

		int next = wrap_one(off);

		if (next >= len) {
			// A document ending in a newline has one more, empty,
			// line after this one -- that's where the cursor sits
			// after pressing Enter at the end, and without it there
			// would be nowhere to put it.
			if (len > 0 && buf[len - 1] == '\n') { off = len; continue; }
			break;
		}

		off = next;

	}

}

static void rewrap_all(void) {
	nlines = 0;
	wrap_from(0);
}

// One past the last byte of display line `l`.
static int line_end(int l) {

	if (l < 0 || l >= nlines) return len;
	if (l + 1 < nlines) return (int)line_off[l + 1];

	return len;

}

// Length of line `l` as DRAWN -- the trailing newline is part of the
// line but is not a glyph.
static int line_draw_len(int l) {

	int a = (int)line_off[l];
	int b = line_end(l);

	if (b > a && buf[b - 1] == '\n') b--;

	return b - a;

}

// The furthest column the caret may sit at on line `l`.
//
// Not simply line_draw_len(): a wrapped line absorbs the run of
// spaces at its break (see wrap_one()), so its length in bytes can
// exceed the window's column count. Letting the caret follow it there
// would put it past the right edge of the text area, on top of the
// scrollbar.
static int line_col_max(int l) {

	int n = line_draw_len(l);

	return n > cols ? cols : n;

}

// The display line containing `off`. Binary search: this runs on
// every cursor move and every edit, and nlines can be in the
// thousands.
static int line_at(int off) {

	int lo = 0, hi = nlines - 1, res = 0;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if ((int)line_off[mid] <= off) { res = mid; lo = mid + 1; }
		else hi = mid - 1;
	}

	return res;

}

// The first display line of the paragraph containing `off` -- i.e.
// walk back over lines that were produced by WRAPPING rather than by
// a real newline. Everything from here on may reflow after an edit;
// nothing before it can.
static int para_line_at(int off) {

	int l = line_at(off);

	while (l > 0 && line_off[l] > 0 && buf[line_off[l] - 1] != '\n') l--;

	return l;

}

// -- layout --

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	text_w = cw - Z_SB_THICK - 1;
	if (text_w < 0) text_w = 0;

	cols = (text_w - TEXT_X0) / CHAR_W;
	if (cols < 1) cols = 1;

	rows = ch / LINE_H;
	if (rows < 1) rows = 1;

	// The scrollbar stops short of the resize grip. This window is
	// resizable, the grip is drawn in the bottom-right corner, and a
	// scrollbar running the full height sits on top of it -- clicks
	// meant for the corner hit the scrollbar's trough instead and
	// page the document, so the window can't be resized at all. See
	// Z_WIN_GRIP_INSET in zwm.h.
	int sb_len = ch - Z_WIN_GRIP_INSET;
	if (sb_len < 0) sb_len = 0;

	z_scrollbar_set_geom(&sbar, cw - Z_SB_THICK, 0, sb_len);

	// A resize changes the column count, so the whole document
	// rewraps. Deliberately from the top: every line can move, which
	// is the one case wrap_from()'s paragraph-local shortcut does not
	// help with.
	rewrap_all();

}

// Keeps `top_line` such that the cursor's line is on screen, and
// syncs the scrollbar to it. Returns true if the view moved.
static bool scroll_to_cursor(void) {

	int l = line_at(cursor);
	int old = top_line;

	if (l < top_line) top_line = l;
	if (l >= top_line + rows) top_line = l - rows + 1;

	if (top_line > nlines - rows) top_line = nlines - rows;
	if (top_line < 0) top_line = 0;

	z_scrollbar_set_range(&sbar, nlines, rows);
	z_scrollbar_set_value(&sbar, top_line);

	return top_line != old;

}

// -- drawing --

// Content-relative rect fill, clamped to the content area.
// z_fb_hw_fill_rect() clamps to the SCREEN, not to this window, so
// handing it a content-relative rect directly would paint over
// whatever else is on screen the moment our furniture ran past our
// own edge. Same helper, same reason, as sw/apps/draw's
// fill_content_rect().
static void fill_content(int cx, int cy, int w, int h, int color) {

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x0 = c.x0 + cx, y0 = c.y0 + cy;
	int x1 = x0 + w - 1, y1 = y0 + h - 1;

	if (x0 < c.x0) x0 = c.x0;
	if (y0 < c.y0) y0 = c.y0;
	if (x1 > c.x1) x1 = c.x1;
	if (y1 > c.y1) y1 = c.y1;
	if (x1 < x0 || y1 < y0) return;

	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);

}

// Draws the caret as a 1px vertical rule between characters, rather
// than as a filled block over one. A block caret hides the character
// it is on, which for an insertion point sitting BETWEEN two
// characters is also just wrong about where the text will go.
static void draw_caret(void) {

	int l = line_at(cursor);

	if (l < top_line || l >= top_line + rows) return;

	int col = cursor - (int)line_off[l];
	if (col > cols) col = cols;

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x = c.x0 + TEXT_X0 + col * CHAR_W;
	int y = c.y0 + (l - top_line) * LINE_H;

	// Clip x to the text area -- a cursor at the very end of a full
	// line otherwise lands on the scrollbar.
	if (x > c.x0 + text_w - 1) x = c.x0 + text_w - 1;

	z_fb_hw_line(x, y, x, y + cur_font->h - 1, 1, &c);

}

// Draws one visible row (0..rows-1), clearing it first.
static void draw_row(int r) {

	if (r < 0 || r >= rows) return;

	int cy = r * LINE_H;

	fill_content(0, cy, text_w, LINE_H, 0);

	int l = top_line + r;
	if (l >= nlines) return;

	int n = line_draw_len(l);
	if (n > cols) n = cols;
	if (n <= 0) return;

	// Copied out because z_win_draw_text() takes a NUL-terminated
	// string and the buffer is not terminated anywhere in particular.
	// Bounded by cols, so this is a screen line's worth, not a
	// document's.
	char tmp[256];
	if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;

	memcpy(tmp, &buf[line_off[l]], (size_t)n);
	tmp[n] = 0;

	// Tabs would advance by one glyph cell here, which disagrees with
	// how wrap_one() counts them. Rather than teach both sides about
	// tab stops, the insert path turns Tab into spaces (see
	// handle_key()), so a tab can only appear in text loaded from
	// disk. Drawn as a space so it at least occupies its one column.
	for (int i = 0; i < n; i++)
		if (tmp[i] == '\t') tmp[i] = ' ';

	int off = (int)line_off[l];

	if (!has_sel()) {
		z_win_draw_text(&win, TEXT_X0, cy, tmp, 1, cur_font);
		return;
	}

	// Split the line into up to three runs -- before, inside, after
	// the selection -- and draw each. The selected run goes through
	// z_fb_draw_text2() with the colours swapped, which is one
	// hardware glyph blit per character exactly like the normal path;
	// inverting afterwards with a fill would be a second pass over
	// the same pixels and would fight the no-flash rule the rest of
	// this app follows.
	// Both ends clamped to [0, n], BOTH sides.
	//
	// This was `if (a < 0) a = 0; if (b > n) b = n;` -- each end
	// clamped in only the direction that mattered for a selection
	// OVERLAPPING this line. For a line entirely outside the
	// selection the other direction is the one that bites: a line
	// after the selection ends gets a NEGATIVE b, and the trailing
	// run below then draws `tmp + b` -- reading before the buffer --
	// at a negative x that clips away entirely. The line simply
	// vanished.
	//
	// Which lines vanished depended on where the selection was, so
	// the symptom looked like "some text isn't displayed" rather than
	// anything to do with selection.
	int a = sel_start() - off;
	int b = sel_end() - off;

	if (a < 0) a = 0;
	if (a > n) a = n;
	if (b < 0) b = 0;
	if (b > n) b = n;

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int px = c.x0 + TEXT_X0;
	int py = c.y0 + cy;

	if (a > 0) {
		char save = tmp[a];
		tmp[a] = 0;
		z_fb_draw_text(px, py, tmp, 1, cur_font, &c);
		tmp[a] = save;
	}

	if (b > a) {
		char save = tmp[b];
		tmp[b] = 0;
		z_fb_draw_text2(px + a * CHAR_W, py, tmp + a, 0, 1, cur_font, &c);
		tmp[b] = save;
	}

	if (n > b)
		z_fb_draw_text(px + b * CHAR_W, py, tmp + b, 1, cur_font, &c);

	// A selection continuing onto the next line takes the rest of
	// this row with it, past the end of the text. Without this a
	// multi-line selection looks like a stack of ragged fragments
	// rather than one continuous block, and it becomes genuinely hard
	// to see where a line ended.
	if (sel_end() > line_end(l) - 1 && sel_start() <= off + n) {
		int rx = TEXT_X0 + n * CHAR_W;
		if (rx < text_w)
			fill_content(rx, cy, text_w - rx, cur_font->h, 1);
	}

}

// Redraws rows `from`..bottom. The usual argument after an edit: a
// reflowed paragraph can move every line below it and none above.
static void draw_rows_from(int from) {

	if (from < 0) from = 0;

	for (int r = from; r < rows; r++) draw_row(r);

	draw_caret();

}

// Full repaint. Called on Z_WM_REDRAW, which wm sends after it has
// already cleared the region -- so this must not assume anything
// about what is currently on screen.
static void repaint(void) {

	// The strip between the text and the scrollbar, plus any partial
	// row at the bottom that draw_row() never covers. wm clears
	// before most redraws but NOT after a move (see repair_drag() in
	// wm.c), so anything not actively rewritten keeps its pre-move
	// contents.
	int ch = z_win_content_h(&win);
	int cw = z_win_content_w(&win);

	// The gutter between the text and the scrollbar, and the
	// scrollbar's own strip -- but NOT down into the resize grip.
	//
	// The grip is chrome, drawn by wm in the window's bottom-right
	// corner, and most of it falls INSIDE the content area (all but
	// its outer 2px -- see Z_WIN_GRIP_INSET in zwm.h). An app that
	// blanks its content area all the way to the bottom-right
	// therefore erases most of the grip and leaves a half-drawn
	// corner. Keeping the scrollbar itself clear of the grip isn't
	// enough; this fill has to stay clear of it too.
	int gutter_h = ch - Z_WIN_GRIP_INSET;
	if (gutter_h < 0) gutter_h = 0;

	fill_content(text_w, 0, Z_SB_THICK + 1, gutter_h, 0);

	// The sliver of gutter still to the LEFT of the grip, alongside
	// it. Three pixels wide as the constants currently stand, and
	// skipped entirely if the arithmetic says there's nothing there.
	int sliver_w = (cw - Z_WIN_GRIP_INSET) - text_w;
	if (sliver_w > 0)
		fill_content(text_w, gutter_h, sliver_w, Z_WIN_GRIP_INSET, 0);

	fill_content(0, rows * LINE_H, text_w, ch - rows * LINE_H, 0);

	for (int r = 0; r < rows; r++) draw_row(r);

	draw_caret();

	z_scrollbar_draw(&sbar, true);

}

// -- titlebar --

// "text - notes.txt *", truncated to what wm will keep (WM_TITLE_MAX
// is 24, and the title also has to share the bar with three icons).
static void update_title(void) {

	char t[32];
	int n = 0;

	const char *base = filename[0] ? filename : "untitled";

	// show just the last path component -- "/DOCS/NOTES.TXT" is
	// mostly slashes at this width
	for (const char *s = filename; *s; s++)
		if (*s == '/') base = s + 1;

	if (modified && n < (int)sizeof(t) - 1) t[n++] = '*';

	for (const char *s = base; *s && n < (int)sizeof(t) - 1; s++)
		t[n++] = *s;

	t[n] = 0;

	z_win_set_title(&win, t);

}

static void set_modified(bool m) {

	if (modified == m) return;

	modified = m;
	update_title();

}

// -- file operations --

static void forward_msg(z_msg_t *msg, void *user);

static z_dialog_ctx_t dlg_ctx;

// The directory the last dialog was in, so opening a second dialog
// starts where the first left off rather than back at the root.
static char last_dir[Z_FLIST_PATH_MAX] = "/";

static void remember_dir(const char *path) {

	int last = 0;

	for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;

	if (last == 0) { last_dir[0] = '/'; last_dir[1] = 0; return; }

	int i = 0;
	for (; i < last && i < (int)sizeof(last_dir) - 1; i++)
		last_dir[i] = path[i];

	last_dir[i] = 0;

}

static bool do_save_to(const char *path) {

	// fs_write_file() creates or truncates, and reports how much it
	// actually wrote. A short write means the card filled up or
	// failed mid-way, which must not be reported as success -- the
	// file on disk is now a truncated version of the document.
	int wrote = fs_write_file((char *)path, buf, len);

	if (wrote != len) {
		z_dialog_confirm(&dlg_ctx, "Save failed",
			"The file could not be\nwritten completely.", Z_DIALOG_OK_CANCEL);
		return false;
	}

	if (path != filename) {
		int i = 0;
		for (; path[i] && i < (int)sizeof(filename) - 1; i++)
			filename[i] = path[i];
		filename[i] = 0;
	}

	remember_dir(filename);
	set_modified(false);
	update_title();

	return true;

}

// Returns false if the user cancelled.
static bool do_save_as(void) {

	char path[80];
	const char *suggest = filename[0] ? filename : "";

	// Suggest the bare name, not the full path -- the dialog's own
	// directory browser is what picks the location.
	for (const char *s = filename; *s; s++)
		if (*s == '/') suggest = s + 1;

	if (!z_dialog_save(&dlg_ctx, last_dir, suggest, path, sizeof(path)))
		return false;

	return do_save_to(path);

}

static bool do_save(void) {

	if (!filename[0]) return do_save_as();

	return do_save_to(filename);

}

// Offers to save when the document has unsaved changes. Returns false
// if the user cancelled, meaning whatever prompted this (new, open,
// close) must not proceed.
static bool confirm_discard(void) {

	if (!modified) return true;

	int r = z_dialog_confirm(&dlg_ctx, "Unsaved changes",
		"Save changes before\ncontinuing?", Z_DIALOG_YES_NO_CANCEL);

	if (r == Z_DIALOG_CANCEL) return false;
	if (r == Z_DIALOG_NO) return true;

	// Yes -- and if the save itself is cancelled or fails, the whole
	// operation is off. Silently discarding after a failed save would
	// be the worst possible reading of "yes, save it".
	return do_save();

}

static void do_new(void) {

	if (!confirm_discard()) return;

	buf_clear();
	rewrap_all();
	top_line = 0;

	update_title();
	scroll_to_cursor();
	repaint();

}

// Reads `path` into the buffer, replacing whatever was there.
// Reports its own failures through a dialog. Returns false with the
// buffer left empty if the file couldn't be used.
//
// Factored out of do_open() so that the launch-argument path in
// main() loads a file exactly the same way the Open dialog does --
// two loaders would be two places for the CRLF handling and the size
// limit to drift apart.
static bool load_file(const char *path) {

	int size = fs_size((char *)path);

	if (size > TEXT_MAX) {
		z_dialog_confirm(&dlg_ctx, "Too large",
			"That file is larger than\nthis editor can hold.",
			Z_DIALOG_OK_CANCEL);
		return false;
	}

	// Read straight into our own buffer rather than through
	// fs_mallocfile(), which would allocate a second copy of the
	// whole file out of a heap that is 16KB shared with the stack
	// (Z_PROC_STACK_SIZE_DEFAULT, sw/os/kernel.h) -- a 20KB document
	// would fail to open for no reason the user could ever guess.
	int h = fs_open_read(path);

	if (h < 0) {
		z_dialog_confirm(&dlg_ctx, "Open failed",
			"That file could not\nbe read.", Z_DIALOG_OK_CANCEL);
		return false;
	}

	buf_clear();

	for (;;) {
		int n = fs_read_chunk(h, &buf[len], TEXT_MAX - len);
		if (n <= 0) break;
		len += n;
		if (len >= TEXT_MAX) break;
	}

	fs_close_handle(h);

	// Normalize CRLF on the way in. A file written on another machine
	// otherwise shows a stray glyph at the end of every line, and
	// every one of them counts against the wrap width.
	{
		int w = 0;
		for (int r = 0; r < len; r++) {
			if (buf[r] == '\r' && r + 1 < len && buf[r + 1] == '\n') continue;
			buf[w++] = buf[r];
		}
		len = w;
	}

	int i = 0;
	for (; path[i] && i < (int)sizeof(filename) - 1; i++) filename[i] = path[i];
	filename[i] = 0;

	remember_dir(filename);

	cursor = 0;
	top_line = 0;
	set_modified(false);

	rewrap_all();
	update_title();
	scroll_to_cursor();

	return true;

}

static void do_open(void) {

	if (!confirm_discard()) return;

	char path[80];

	if (!z_dialog_open(&dlg_ctx, last_dir, path, sizeof(path))) return;

	if (load_file(path)) repaint();

}

// Switches between the two resident fonts and re-lays-out everything
// that depends on the metrics.
//
// The column count changes, so the document has to be rewrapped from
// the top -- the same case a resize is, and the one place
// wrap_from()'s paragraph-local shortcut genuinely cannot help, since
// every line can move.
//
// The caret is kept on the same CHARACTER, not the same screen
// position: `cursor` is a buffer offset and survives the rewrap
// untouched, which is the whole reason the buffer and the line table
// are separate things.
static void do_font(void) {

	cur_font = (cur_font == &z_font_5x8) ? &z_font_6x12 : &z_font_5x8;

	layout();
	scroll_to_cursor();
	repaint();

}

static void do_close(void) {

	if (!confirm_discard()) return;

	z_win_destroy(&win);

	printf("text: exiting\n");

	// Ordinary process exit -- _exit() (sw/common/zeitlos.c) makes
	// the Z_SYS_EXIT syscall. Reached only from the close icon or
	// Ctrl+Q, both of which have already been through
	// confirm_discard() above.
	exit(0);

}

// -- editing --

// Applies an edit that has already happened to the buffer, rewrapping
// from `para` (the paragraph line computed BEFORE the edit) and
// repainting from wherever the change becomes visible.
static void after_edit(int para) {

	// Any edit ends the selection. Without this, a COLLAPSED anchor
	// (set by a click, where anchor == cursor, so has_sel() is false)
	// survives the edit -- and the moment the edit moves the cursor,
	// anchor != cursor and a selection springs into existence that
	// the user never made. Combined with the clamping bug above that
	// showed up as text disappearing after a paste.
	sel_anchor = -1;

	wrap_from(para);
	set_modified(true);

	bool scrolled = scroll_to_cursor();

	if (scrolled) {
		repaint();
		return;
	}

	int r = para - top_line;
	if (r < 0) r = 0;

	draw_rows_from(r);
	z_scrollbar_draw(&sbar, false);

}

// Inserts `n` copies of `c` as ONE edit -- one rewrap, one repaint,
// and at most one "document is full" complaint no matter how many
// characters were asked for. Tab is the reason it takes a count:
// four separate insert_char() calls would rewrap and repaint four
// times for a single keypress, and could raise the same dialog four
// times in a row at the buffer limit.
// Removes the selected range. Returns true if anything was removed.
//
// Shared by every path that replaces a selection -- typing over it,
// Backspace, Delete, Cut, and Paste -- so there is one place that
// knows how a selection turns into an edit.
static bool delete_selection(void) {

	if (!has_sel()) return false;

	int a = sel_start();
	int b = sel_end();

	int para = para_line_at(a);

	memmove(&buf[a], &buf[b], (size_t)(len - b));
	len -= (b - a);

	cursor = a;
	sel_anchor = -1;

	after_edit(para);

	return true;

}

static void do_copy(void) {

	if (!has_sel()) return;

	// Pointed straight at the document -- z_clip_set() copies into
	// its own staging buffer before sending, precisely so a caller
	// can do this rather than needing a spare 4KB of its own.
	z_clip_set(&buf[sel_start()], sel_end() - sel_start());

}

static void do_cut(void) {

	if (!has_sel()) return;

	do_copy();
	delete_selection();

}

static void do_paste(void) {

	// One page at a time, matching wm's own clipboard capacity --
	// see Z_WM_CLIP_MAX in zwm.h. On the stack rather than in .bss
	// because it is live only for the duration of this call, and 4KB
	// of a 16KB stack is affordable for one call that makes no
	// further nested calls of consequence.
	static char clip[Z_WM_CLIP_MAX];

	int avail = z_clip_get(clip, sizeof(clip));
	if (avail <= 0) return;

	int n = avail;

	// Replacing a selection is one edit, not two: delete then insert
	// would rewrap twice and repaint twice for a single user action.
	int para;

	if (has_sel()) {
		int a = sel_start();
		int b = sel_end();
		para = para_line_at(a);
		memmove(&buf[a], &buf[b], (size_t)(len - b));
		len -= (b - a);
		cursor = a;
		sel_anchor = -1;
	} else {
		para = para_line_at(cursor);
	}

	if (n > TEXT_MAX - len) n = TEXT_MAX - len;

	if (n > 0) {
		memmove(&buf[cursor + n], &buf[cursor], (size_t)(len - cursor));
		memcpy(&buf[cursor], clip, (size_t)n);
		len += n;
		cursor += n;
	}

	after_edit(para);

	// Said out loud rather than silently dropping the tail: a paste
	// that lands partially is worth knowing about.
	if (n < avail)
		z_dialog_confirm(&dlg_ctx, "Full",
			"Only part of the clipboard\nwould fit.", Z_DIALOG_OK_CANCEL);

}

static void insert_run(char c, int n) {

	// Typing with a selection replaces it -- the standard behaviour,
	// and the reason delete_selection() is factored out.
	delete_selection();

	int para = para_line_at(cursor);
	int done = 0;

	for (; done < n; done++) {
		if (!buf_insert(cursor, c)) break;
		cursor++;
	}

	if (done) after_edit(para);

	if (done < n) {
		// Full. Say so rather than appearing to ignore the keyboard
		// -- the buffer is a fixed size and there is no recovery
		// except deleting something.
		z_dialog_confirm(&dlg_ctx, "Full",
			"The document is full.", Z_DIALOG_OK_CANCEL);
		repaint();
	}

}

static void insert_char(char c) {
	insert_run(c, 1);
}

static void delete_back(void) {

	if (delete_selection()) return;

	if (cursor == 0) return;

	int para = para_line_at(cursor - 1);

	cursor--;
	buf_delete(cursor);

	after_edit(para);

}

static void delete_forward(void) {

	if (delete_selection()) return;

	if (cursor >= len) return;

	int para = para_line_at(cursor);

	buf_delete(cursor);

	after_edit(para);

}

// -- cursor movement --

// Redraws only what a cursor move affects: the caret's old row and
// its new one.
// Moves the caret, optionally EXTENDING the selection to the new
// position rather than dropping it.
//
// Redraws the union of the old and new selected ranges, not just the
// two rows the caret touched. With a selection those ranges are what
// changed appearance, and repainting the whole text area on every
// shift+arrow -- the easy alternative -- is hundreds of glyph blits
// per keystroke.
static void move_cursor_ex(int to, bool extend) {

	if (to < 0) to = 0;
	if (to > len) to = len;

	int old_lo = has_sel() ? sel_start() : cursor;
	int old_hi = has_sel() ? sel_end() : cursor;

	if (extend) {
		if (sel_anchor < 0) sel_anchor = cursor;
	} else {
		sel_anchor = -1;
	}

	if (to == cursor && !extend && old_lo == old_hi) return;

	cursor = to;

	int new_lo = has_sel() ? sel_start() : cursor;
	int new_hi = has_sel() ? sel_end() : cursor;

	int lo = old_lo < new_lo ? old_lo : new_lo;
	int hi = old_hi > new_hi ? old_hi : new_hi;

	if (scroll_to_cursor()) {
		repaint();
		return;
	}

	int r0 = line_at(lo) - top_line;
	int r1 = line_at(hi) - top_line;

	if (r0 < 0) r0 = 0;
	if (r1 >= rows) r1 = rows - 1;

	for (int r = r0; r <= r1; r++) draw_row(r);

	draw_caret();

}

static void move_cursor(int to) {
	move_cursor_ex(to, false);
}

// Up/down keep the COLUMN, not the offset -- which is what makes
// arrowing down a ragged paragraph feel like moving down a page
// rather than wandering.
static void move_line_ex(int delta, bool extend) {

	int l = line_at(cursor);
	int col = cursor - (int)line_off[l];

	int target = l + delta;
	if (target < 0) target = 0;
	if (target >= nlines) target = nlines - 1;
	if (target == l) return;

	int maxcol = line_col_max(target);
	if (col > maxcol) col = maxcol;

	move_cursor_ex((int)line_off[target] + col, extend);

}

static void move_line(int delta) {
	move_line_ex(delta, false);
}

// -- input --

static void handle_key(uint32_t keysym, uint8_t mods) {

	mods_now = mods;

	if (mods & Z_KBD_MOD_CTRL) {

		// Ctrl+letter arrives as 0x01..0x1A (zkbd.h), which overlaps
		// the control characters Enter and Tab also produce -- hence
		// checking the modifier first rather than the keysym alone.
		switch (keysym) {
			case 0x0e: do_new(); return;		// Ctrl+N
			case 0x0f: do_open(); return;		// Ctrl+O
			case 0x13: do_save(); return;		// Ctrl+S
			case 0x11: do_close(); return;		// Ctrl+Q
			case 0x03: do_copy(); return;		// Ctrl+C
			case 0x18: do_cut(); return;		// Ctrl+X
			case 0x16: do_paste(); return;		// Ctrl+V
			case 0x01:							// Ctrl+A -- select all
				sel_anchor = 0;
				move_cursor_ex(len, true);
				return;
			default: return;
		}

	}

	// Shift turns every movement key into a selection-extending one.
	// Checked once here rather than in each case, because the set of
	// keys this applies to is exactly "the ones that move the caret".
	bool ext = (mods & Z_KBD_MOD_SHIFT) != 0;

	switch (keysym) {

		case Z_KEY_LEFT:	move_cursor_ex(cursor - 1, ext); return;
		case Z_KEY_RIGHT:	move_cursor_ex(cursor + 1, ext); return;
		case Z_KEY_UP:		move_line_ex(-1, ext); return;
		case Z_KEY_DOWN:	move_line_ex(1, ext); return;

		case Z_KEY_PAGEUP:	move_line_ex(-(rows - 1), ext); return;
		case Z_KEY_PAGEDOWN:	move_line_ex(rows - 1, ext); return;

		case Z_KEY_HOME:
			move_cursor_ex((int)line_off[line_at(cursor)], ext);
			return;

		case Z_KEY_END: {
			int l = line_at(cursor);
			move_cursor_ex((int)line_off[l] + line_col_max(l), ext);
			return;
		}

		case Z_KEY_DELETE:	delete_forward(); return;

		case 0x7f:			// Backspace -- DEL, see zkbd.c
			delete_back();
			return;

		case 0x0d:			// Enter -- CR from the keymap
			// Stored as '\n'. One line ending in the buffer, decided
			// here, so nothing downstream has to cope with two.
			insert_char('\n');
			return;

		case '\t':
			// Spaces, not a tab. wrap_one() counts columns and a real
			// tab has no fixed width in that count -- see draw_row().
			// One edit, not four -- see insert_run().
			insert_run(' ', 4);
			return;

		default:

			if (keysym >= 0x20 && keysym <= 0x7e) insert_char((char)keysym);
			return;

	}

}

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Events over our TITLEBAR reach us too.
	//
	// wm forwards a pointer sample to the focused window whenever the
	// cursor is over it, and its hit test is the whole window rect,
	// frame and titlebar included (dispatch_mouse() in wm.c). A drag
	// is suppressed while wm owns it, but a click on a titlebar ICON
	// is not: wm handles the icon and then forwards the same press.
	// The content coordinates it arrives with are NEGATIVE in y, and
	// clamping those to row 0 meant clicking Save also placed the
	// caret at the top of the document and started a selection there.
	//
	// A drag already in progress is exempt -- that is exactly the
	// case where the cursor legitimately leaves the content area and
	// the selection must keep extending (see Z_WM_MOUSE's "inside"
	// bit in zwm.h).
	if (!inside && !selecting && !z_scrollbar_has_pointer(&sbar, cx, cy))
		return;

	// The scrollbar gets first refusal and keeps the pointer for the
	// whole of a drag it started.
	if (z_scrollbar_has_pointer(&sbar, cx, cy)) {

		if (z_scrollbar_mouse(&sbar, cx, cy, buttons)) {
			top_line = (int)sbar.value;
			repaint();
		}

		return;

	}

	// Keep its edge detector fed even when the pointer is elsewhere,
	// so a button released outside it still ends the drag.
	z_scrollbar_mouse(&sbar, cx, cy, buttons);

	// Right button copies the selection, as a shortcut for Ctrl+C.
	// Acted on at PRESS rather than release: there is no drag
	// gesture on this button, so waiting adds nothing, and a copy
	// that happens the instant you click feels more definite.
	if ((buttons & Z_MOUSE_BTN_RIGHT) && !(last_buttons & Z_MOUSE_BTN_RIGHT))
		do_copy();

	last_buttons = buttons;

	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;

	if (!down) {
		// Button up ends a drag. The selection stays as it is --
		// releasing selects, it does not deselect -- but an anchor
		// that never grew into one is dropped, so it cannot become a
		// selection later when something else moves the cursor.
		selecting = false;
		if (!has_sel()) sel_anchor = -1;
		return;
	}

	// Click to place the caret. Clamped rather than ignored when the
	// click lands past the end of a line or below the last one --
	// clicking in the empty space under a short document should put
	// the cursor at the end of it, which is where the user is
	// pointing.
	int r = cy / LINE_H;
	if (r < 0) r = 0;

	int l = top_line + r;
	if (l >= nlines) l = nlines - 1;
	if (l < 0) return;

	int col = (cx - TEXT_X0 + CHAR_W / 2) / CHAR_W;
	if (col < 0) col = 0;

	int maxcol = line_col_max(l);
	if (col > maxcol) col = maxcol;

	int pos = (int)line_off[l] + col;

	if (!selecting) {
		// Press: place the caret and anchor here, so that any drag
		// that follows extends from this point. Shift+click extends
		// from the EXISTING anchor instead, which is how you widen a
		// selection without redoing it.
		bool ext = (mods_now & Z_KBD_MOD_SHIFT) != 0;
		move_cursor_ex(pos, ext);
		if (!ext) sel_anchor = pos;
		selecting = true;
	} else {
		// Drag: extend. wm's pointer capture keeps delivering events
		// after the cursor leaves the window, which is what lets a
		// selection run off the bottom and keep going.
		move_cursor_ex(pos, true);
	}

}

// -- message handling --
//
// One function, used both by the main loop and, through
// z_dialog_ctx_t, by any dialog that happens to be open. That is not
// a convenience: while a dialog is up, wm carries on asking THIS
// window to redraw and blocks waiting for the ack (see zdialog.h and
// docs/window_manager.md). An app that only serviced redraws from its
// own main loop would freeze the screen every time it opened a
// dialog.
static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;

			// Only ours. A dialog's own redraws are handled inside
			// zdialog.c and never reach here, but this is also the
			// message that arrives while a dialog is being CREATED,
			// before it has a window id to compare against.
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:

			// No layout() -- moving doesn't change our size, and
			// every rect we hold is content-relative.
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:

			// Arrives BEFORE the Z_WM_REDRAW that follows a resize
			// (zwm.h guarantees that ordering), so the layout is
			// already correct by the time we're asked to repaint at
			// the new size.
			z_win_apply_resized(&win, &msg->obj);
			layout();
			scroll_to_cursor();
			break;

		default:
			break;

	}

}

int main(void) {

	printf("text: starting\n");

	// Explicit, not left to .bss zero-init -- that has already been
	// shown unreliable on this hardware at least once (see
	// docs/app_runtime.md and k_pidreg_init()'s call site in
	// kernel.c), and a line table full of garbage would draw from
	// arbitrary offsets in the buffer.
	memset(buf, 0, sizeof(buf));
	memset(line_off, 0, sizeof(line_off));

	buf_clear();
	nlines = 0;
	top_line = 0;

	// CLOSE_ICON WITHOUT CLOSE_KILLS_OWNER, deliberately. This app
	// owns more than one window at a time (a dialog is a window), and
	// the killing form takes every window of a pid down the instant
	// any one of them is clicked closed -- see that flag's own
	// warning in zwm.h. It also has to be the non-killing form for a
	// second reason: closing with unsaved changes must get a chance
	// to ask.
	if (z_win_create_flags(&win, "untitled", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		Z_WIN_FLAG_MIN_IS_CREATE |
		Z_WIN_FLAG_NEW_ICON | Z_WIN_FLAG_OPEN_ICON |
		Z_WIN_FLAG_SAVE_ICON | Z_WIN_FLAG_FONT_ICON) != Z_OK) {
		printf("text: failed to create window -- is wm running?\n");
		return 1;
	}

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	layout();

	// A file to open, if something launched us with one -- the file
	// browser does, via wm's pending launch argument (Z_WM_SET_ARG,
	// zwm.h). load_file() reports its own failures, and leaves an
	// empty buffer if it fails, so there is nothing to check here.
	{
		char arg[sizeof(filename)];
		if (z_launch_arg_take(arg, sizeof(arg))) load_file(arg);
	}

	rewrap_all();
	update_title();
	scroll_to_cursor();
	repaint();

	for (;;) {

		z_msg_t msg;

		// Drain the whole queue each pass rather than one message per
		// iteration -- see Z_WM_MOUSE's own note in zwm.h on why
		// handling one per loop makes an app fall progressively
		// behind the real cursor.
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

				case Z_WM_TITLEBAR_ICON: {

					if (msg.obj.type != Z_UINT32) break;

					uint32_t v = msg.obj.val.uint32;
					if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;

					switch (Z_WM_UNPACK_TBICON_KIND(v)) {
						case Z_WM_TBICON_NEW:  do_new(); break;
						case Z_WM_TBICON_OPEN: do_open(); break;
						case Z_WM_TBICON_SAVE: do_save(); break;
						case Z_WM_TBICON_FONT: do_font(); break;
						default: break;
					}

					break;

				}

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id)
						do_close();

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		for (volatile int i = 0; i < 200; i++);	// light throttle

	}

	return 0;

}
