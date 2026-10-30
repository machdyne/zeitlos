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
 * change confined to insert_seq()/buf_delete_range().
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
 * -- characters --
 *
 * The buffer holds UTF-8 and every offset in this file is a BYTE
 * offset, as before. What changed is that a character may now be
 * more than one byte, so the three things that care -- stepping the
 * caret, counting columns, and drawing -- walk characters with
 * next_off()/prev_off() instead of adding one, and count columns with
 * width_at(): a CJK character takes two (drawn from the Japanese font
 * while sw/apps/jfont runs), a combining mark none. A byte that is not
 * valid UTF-8 is kept exactly as it is and counts as one character
 * (drawn as the missing-glyph box), so a file this editor cannot
 * fully decode is saved back byte for byte. A file that is not UTF-8
 * at all is read as ISO 8859-15 and saved back that way; see
 * load_file(), do_save_to() and docs/text_editor.md, "Encodings".
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

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"	// Z_TICK_HZ, for the render instrumentation
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zspeak.h"	// Super+A -- docs/tts.md
#include "../../common/zsayall.h"
#include "../../common/zutf8.h"		// docs/text_encoding.md
#include "../../common/zundo.h"		// undo and redo -- docs/text_editor.md, "Undo"
#include "../../common/zbigline.h"	// big print -- docs/text_editor.md, "Font"
#include "../../common/zprint.h"	// printing -- docs/text_editor.md, "Printing"

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

// -- storage: a gap buffer, in .bss or in the arena --
// docs/text_editor.md, "The document"
//
// The text is doc[0..gs) followed by doc[gs+gl..cap): the gap, all the
// free space there is, sits between. It is always at the END OF A
// PARAGRAPH -- just after a newline, or at the end of the document --
// so no display line, no word and no character ever straddles it, and
// anything that reads a line can take a pointer to it (P()). Typing
// moves only the rest of its own paragraph, wherever that paragraph is
// in the document, and the gap only moves when the edit is in another
// paragraph.
//
// A document of up to TEXT_MAX bytes lives in sbuf, in .bss, as it
// always has; a larger one moves to the app's arena (Z_SYS_ARENA,
// docs/kernel.md "Arenas"), which is sized as the document grows. On a
// board too small for arenas the limit is what it was.

static char sbuf[TEXT_MAX];
static char *doc = sbuf;
static int cap = TEXT_MAX;		// bytes of storage for text
static int gs, gl = TEXT_MAX;	// the gap: [gs, gs + gl) physically
static int len;
static int cursor;			// offset into the text, 0..len
static bool modified;
static bool io_busy;		// a file coming in: repaint() shows none (io_pump())

// The byte at text offset i, and a pointer to it that is good up to
// the gap or the end of the storage, whichever comes first (PEND()).
#define B(i)	(doc[(i) < gs ? (i) : (i) + gl])
static inline char *P(int i) { return &doc[i < gs ? i : i + gl]; }
static inline const char *PEND(int i) { return i < gs ? &doc[gs] : &doc[cap]; }

// How the file on disk is encoded, so it is saved back the same way.
// The buffer is UTF-8 either way (see the header comment).
typedef enum {
	ENC_UTF8 = 0,		// also every file that is plain ASCII
	ENC_LATIN9,			// ISO 8859-15: every byte one character
	ENC_CP1252,			// Windows-1252: Latin-1, plus punctuation in 0x80-0x9f
} enc_t;

static enc_t file_enc;
static bool file_bom;		// the file started with a UTF-8 byte order mark

// -- characters --
//
// Byte offsets of the character after and before `off`. A malformed
// byte is one character on its own, exactly as z_utf8_next() treats
// it, so these two agree with each other and with the drawing code on
// where every character starts.

// A character never straddles the gap (it sits after a newline), so
// decoding from P(off) up to PEND(off) sees the whole of it.
static int next_off(int off) {
	if (off >= len) return len;
	if ((uint8_t)B(off) < 0x80) return off + 1;	// ASCII: the common case
	const char *p = P(off), *q = p;
	const char *end = PEND(off);
	if (end > q + (len - off)) end = q + (len - off);
	z_utf8_next(&p, end);
	return off + (int)(p - q);
}

static int prev_off(int off) {
	if (off <= 0) return 0;
	// Back over at most three continuation bytes to a lead byte, and
	// accept it only if decoding forward from there lands exactly on
	// `off`. Anything else -- a stray continuation byte, a truncated
	// sequence -- is a one-byte character.
	int k = off - 1;
	int lim = off - Z_UTF8_MAX > 0 ? off - Z_UTF8_MAX : 0;
	while (k > lim && ((uint8_t)B(k) & 0xC0) == 0x80) k--;
	if (next_off(k) == off) return k;
	return off - 1;
}

// Columns the character at `off` takes on screen: z_cp_width(), the
// count zgfx draws with and a terminal uses -- two for a CJK character,
// none for a combining mark. A control character (a tab or NUL from a
// loaded file) is drawn as a space, so it takes one.
static int width_at(int off) {
	uint8_t c = (uint8_t)B(off);
	if (c < 0x7F) return 1;		// ASCII, control characters included
	const char *p = P(off);
	const char *end = PEND(off);
	if (end > p + (len - off)) end = p + (len - off);
	uint32_t cp = z_utf8_next(&p, end);
	int w = z_cp_width(cp);
	return (w == 0 && cp < 0x20) ? 1 : w;
}

// Columns buf[a..b) takes.
static int chars_between(int a, int b) {
	int n = 0;
	while (a < b) { n += width_at(a); a = next_off(a); }
	return n;
}

// The offset `col` columns after `a`, but not past `b`. A wide
// character that would straddle `col` is not stepped into: its start
// is the answer.
static int off_after(int a, int b, int col) {
	while (a < b) {
		int w = width_at(a);
		if (w > col) break;
		col -= w;
		a = next_off(a);
	}
	return a;
}

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

// -- the line table: where each display line starts --
// docs/text_editor.md, "Wrapping"
//
// A gap buffer too. Lines [0, lg) are stored as their offset; the free
// slots [lg, lg + lgl) follow; and every line after them is stored as
// its distance from the END of the text, len - offset. An edit happens
// at the gap, so the lines after it -- every line below the caret --
// need no change when the text before them grows or shrinks: their
// distance from the end is what it was. A keystroke rewrites only its
// own paragraph's lines, however long the document; before this table
// had a gap, it shifted every line below by the length typed.
//
// In .bss the table is 16 bits wide, which holds both kinds of entry
// for a TEXT_MAX of 32768; in the arena it is 32 bits and grows with
// the document. `big` says which.
static uint16_t sline[MAX_LINES];
static int nlines;			// lcap - lgl, kept for everything that reads it

static bool big;
static uint32_t *bline;
static int lcap = MAX_LINES;
static int lg, lgl = MAX_LINES;	// the table's gap

// Slot `p`, raw. Out of line: called from dozens of places, and the
// two-way branch inlined at each would cost more flash than the call
// costs time.
static __attribute__((noinline)) int LT(int p) { return big ? (int)bline[p] : (int)sline[p]; }
static __attribute__((noinline)) void LT_SET(int p, int v) {
	if (big) bline[p] = (uint32_t)v;
	else sline[p] = (uint16_t)v;
}

// Where display line `l` starts.
static __attribute__((noinline)) int LO(int l) {
	return l < lg ? LT(l) : len - LT(l + lgl);
}

static char filename[Z_FS_PATH_MAX];	// "" when the document has never been saved

// -- window --

// The size a window opens at. It can be made as small as half that
// (WIN_MIN_W x WIN_MIN_H, z_win_min_size()): about 23 columns of 5x8
// and a dozen lines, or 14 columns of big print -- a note beside other
// windows. All five titlebar buttons still fit at that width (wm drops
// icons that don't, rather than overlapping the title, and the title
// is shortened first). Z_WIN_FLAG_MIN_IS_CREATE stays set: a wm older
// than z_win_min_size() keeps this size as the minimum, as before.
#define WIN_W   320
#define WIN_H   240
#define WIN_MIN_W	(WIN_W / 2)
#define WIN_MIN_H	(WIN_H / 2)

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

// The third size, big print: misc-fixed 10x20, too wide for glyph
// memory (8 pixels at most), so each line is drawn in memory and copied
// to the screen (zbigline.h). Non-NULL while it is the size in use;
// cur_font is then left as it was and not drawn with.
static const z_bigfont_t *big_font;

#define CHAR_W      (big_font ? big_font->w : cur_font->w)
#define GLYPH_H     (big_font ? big_font->h : cur_font->h)
#define LINE_H      (GLYPH_H + 1)
#define TEXT_X0     2			// left margin, content-relative

static int cols;			// characters per display line
static int rows;			// display lines visible at once
static int text_w;			// width of the text area, excluding scrollbar

static z_scrollbar_t sbar;

static int top_line;		// first visible display line

// -- buffer primitives --

// -- undo -- docs/text_editor.md, "Undo" --
//
// Every change to the document goes through edit_insert() and
// edit_delete(), which tell the undo log (sw/common/zundo.h) before
// they touch the buffer. The log keeps the edits, not copies of the
// document, as far back as its two rings reach.
//
// The rings are in the arena (below) whenever the machine has one:
// UNDO_AR_RECORDS edits and UNDO_AR_BYTES of their text to start with,
// grown to fit any one change that is larger -- a big cut, a paste, a
// Replace All -- so that it can be undone (undo_reserve()). Small edits
// never grow them: when they fill, the oldest edits are forgotten, and
// history gets shorter rather than an edit being refused. A new
// document puts them back to the starting size. On a board with no
// arena they are the fixed .bss rings below, as they always were.
//
// One Ctrl+Z undoes a group. A group is a run of the same kind of edit
// at the place the last one left off -- typing a sentence, holding
// Backspace -- ended by moving the caret, by switching between typing
// and deleting, or by Enter. Anything that replaces a selection, and
// every cut and paste, is a group of its own.

#define UNDO_RECORDS  256
#define UNDO_BYTES    6144

#define UNDO_AR_RECORDS  2048			// 40KB of records
#define UNDO_AR_BYTES    (64 * 1024)

static z_undo_rec_t undo_rec[UNDO_RECORDS];
static char undo_bytes[UNDO_BYTES];
static z_undo_t undo;

enum { UK_NONE, UK_TYPE, UK_BACK, UK_FWD, UK_OTHER };
static uint16_t undo_group;
static int undo_kind = UK_NONE;
static int undo_next = -1;		// where a continuing edit of undo_kind would be

// Starts a new group unless this edit continues the last one: the same
// kind, at the place the last one left off.
static void undo_begin(int kind, int pos) {
	if (kind != undo_kind || pos != undo_next || kind == UK_OTHER)
		undo_group++;
	undo_kind = kind;
}

// Ends the current group, whatever comes next -- after Enter, an undo,
// a caret move.
static void undo_break(void) {
	undo_kind = UK_NONE;
	undo_next = -1;
}

// What edits cost, counted for the host test (tests/utf8.c, section
// 18): bytes moved, bytes wrapped, line-table entries shifted.
#ifdef TEXT_HOST_TEST
static long cost_moved, cost_wrapped, cost_shifted;
#define COST(v, n) ((v) += (n))
#else
#define COST(v, n) ((void)0)
#endif

// -- the gap --

// Moves the gap to text offset `pos`. What moves is the text between
// where it was and where it goes.
static void gap_to(int pos) {
	if (pos < gs) memmove(&doc[pos + gl], &doc[pos], (size_t)(gs - pos));
	else if (pos > gs) memmove(&doc[gs], &doc[gs + gl], (size_t)(pos - gs));
	COST(cost_moved, pos < gs ? gs - pos : pos - gs);
	gs = pos;
}

// The text as one run, doc[0..len): for the few things that read the
// whole document -- saving, undo, Replace All. Costs the text after the
// gap, once; typing puts the gap back where it is wanted.
static void doc_flat(void) {
	gap_to(len);
}

// One past the newline that ends the paragraph containing `off`, or
// the end of the text. `off` just after a newline is the start of the
// NEXT paragraph, which is the one an edit there belongs to.
static int para_end(int off) {
	for (int i = off; i < len; i++)
		if (B(i) == '\n') return i + 1;
	return len;
}

// -- the arena: storage past what .bss holds --
//
//   [ text: cap ][ line table: lcap words ][ undo records ][ undo bytes ][ clipboard ]
//
// Each part is there only while it is needed: the text and the line
// table while the document is past TEXT_MAX (`big`), the undo rings
// once the .bss ones are outgrown (ar_rec, ar_bytes), the clipboard
// while text holds a copy larger than wm's (clip_cap). Grown when
// something would not fit -- the text by doubling, see doc_room(). A
// resize may move the arena (zeitlos.h, z_arena_set()), so everything
// in it is held as an offset and the pointers into it are set again
// after every call (arena_point()).
//
// While a file is being opened its bytes are read into the top of the
// text's free space (`stage_tail`, see "opening"), so the tail of the
// storage that every resize keeps is that much longer.

#define ARENA_TEXT_MIN	(2 * TEXT_MAX)
#define UREC_SZ			((int)sizeof(z_undo_rec_t))

static uint8_t *arena;			// base, or NULL with no arena
static int ar_rec, ar_bytes;	// undo rings in the arena: their sizes, 0 while in .bss
static int clip_cap, clip_len;	// the clipboard copy (see "the clipboard")
static int stage_tail;			// bytes at the top of the text storage that are live in the gap

static int round4k(long n) { return (int)((n + 4095) & ~4095L); }

static uint32_t ar_clip_off(void) {
	return (big ? (uint32_t)cap + (uint32_t)lcap * 4u : 0) +
		(uint32_t)ar_rec * (uint32_t)UREC_SZ + (uint32_t)ar_bytes;
}

static void arena_point(void *base) {
	arena = (uint8_t *)base;
	if (big) {
		doc = (char *)arena;
		bline = (uint32_t *)(arena + cap);
	}
	if (ar_rec) {
		uint32_t r = big ? (uint32_t)cap + (uint32_t)lcap * 4u : 0;
		undo.rec = (z_undo_rec_t *)(arena + r);
		undo.rec_cap = ar_rec;
		undo.bytes = (char *)(arena + r + (uint32_t)ar_rec * (uint32_t)UREC_SZ);
		undo.bytes_cap = (uint32_t)ar_bytes;
	} else {
		undo.rec = undo_rec;
		undo.rec_cap = UNDO_RECORDS;
		undo.bytes = undo_bytes;
		undo.bytes_cap = UNDO_BYTES;
	}
}

// The Open and Save dialogs list a big folder after the arena's own
// bytes, which can move it (zdialog.h, arena_moved).
static void arena_dialog_moved(void *base, void *user) {
	(void)user;
	if (arena) arena_point(base);
}

// Moves pieces of the arena to where a new layout puts them. Each is
// (from, to, length); the pieces are in address order and do not
// overlap, before or after. Those moving down go first, lowest first,
// then those moving up, highest first: so no piece is ever written
// over one not yet moved.
typedef struct { uint32_t o, n, len; } ar_move_t;

static void ar_moves(uint8_t *a, const ar_move_t *m, int k) {
	for (int i = 0; i < k; i++)
		if (m[i].len && m[i].n < m[i].o) memmove(a + m[i].n, a + m[i].o, m[i].len);
	for (int i = k - 1; i >= 0; i--)
		if (m[i].len && m[i].n > m[i].o) memmove(a + m[i].n, a + m[i].o, m[i].len);
}

// Lays the arena out again with these sizes: the text `ntc` bytes and
// the line table `nlc` lines (both 0 unless `big`), the undo rings and
// the clipboard. What is in each part is kept -- the text's head and
// tail, the table's two ends, the rings (which must be linear,
// undo_linear(), when their size changes) and the clipboard -- unless
// `keep_doc` is false, when the text and the table are not wanted.
// False, changing nothing, if the machine has not got the memory.
static bool ar_layout(int ntc, int nlc, int nrc, int nbc, int ncc, bool keep_doc) {

	uint32_t otc = big ? (uint32_t)cap : 0, olc = big ? (uint32_t)lcap * 4u : 0;
	uint32_t oR = otc + olc, oU = oR + (uint32_t)ar_rec * UREC_SZ;
	uint32_t oC = oU + (uint32_t)ar_bytes, oT = oC + (uint32_t)clip_cap;
	uint32_t nR = (uint32_t)ntc + (uint32_t)nlc * 4u, nU = nR + (uint32_t)nrc * UREC_SZ;
	uint32_t nC = nU + (uint32_t)nbc, nT = nC + (uint32_t)ncc;

	void *base = arena;
	if (nT > oT || (!arena && nT)) {
		uint32_t got;
		if (z_arena_set(nT, &base, &got) != Z_ARENA_OK) return false;
	}
	uint8_t *a = (uint8_t *)base;

	ar_move_t m[7];
	int k = 0;
	if (big && keep_doc) {
		int tail = len - gs + stage_tail;
		int after = lcap - lg - lgl;
		m[k++] = (ar_move_t){ 0, 0, (uint32_t)gs };
		m[k++] = (ar_move_t){ otc - (uint32_t)tail, (uint32_t)ntc - (uint32_t)tail, (uint32_t)tail };
		m[k++] = (ar_move_t){ otc, (uint32_t)ntc, (uint32_t)lg * 4u };
		m[k++] = (ar_move_t){ otc + (uint32_t)(lcap - after) * 4u,
			(uint32_t)ntc + (uint32_t)(nlc - after) * 4u, (uint32_t)after * 4u };
	}
	m[k++] = (ar_move_t){ oR, nR, (uint32_t)(ar_rec < nrc ? ar_rec : nrc) * UREC_SZ };
	m[k++] = (ar_move_t){ oU, nU, (uint32_t)(ar_bytes < nbc ? ar_bytes : nbc) };
	// The clipboard with the rest, as the highest piece: moved apart,
	// after them, the rings moving up wrote over it before it moved.
	m[k++] = (ar_move_t){ oC, nC, (uint32_t)(clip_len < ncc ? clip_len : ncc) };
	ar_moves(a, m, k);

	if (big && keep_doc) {
		gl = ntc - len;
		cap = ntc;
		lgl += nlc - lcap;
		lcap = nlc;
	}
	ar_rec = nrc;
	ar_bytes = nbc;
	clip_cap = ncc;

	if (nT < oT) {
		if (nT == 0) { z_arena_set(0, NULL, NULL); base = NULL; }
		else z_arena_set(nT, &base, NULL);		// smaller: in place, or moved whole
	}
	arena_point(base);
	return true;
}

// Grows the arena so the text has room for `ncap` bytes and the line
// table for `nlcap` lines. False, changing nothing, if the machine has
// not got it.
static bool arena_resize(int ncap, int nlcap) {
	ncap = round4k(ncap);
	if (ncap < cap) ncap = cap;
	if (nlcap < lcap) nlcap = lcap;
	return ar_layout(ncap, nlcap, ar_rec, ar_bytes, clip_cap, true);
}

// Moves a document that has outgrown sbuf into the arena, sized for
// `need` bytes of text and twice that to grow into.
static bool go_big(int need) {
	int ncap = round4k((long)need * 2 > ARENA_TEXT_MIN ? (long)need * 2 : ARENA_TEXT_MIN);
	int nlcap = MAX_LINES * 2;
	if (nlcap < ncap / 16) nlcap = ncap / 16;
	// Room made below the undo rings and the clipboard, which move up.
	if (!ar_layout(ncap, nlcap, ar_rec, ar_bytes, clip_cap, false)) return false;
	uint8_t *a = arena;
	int tail = len - gs + stage_tail;
	memcpy(a, sbuf, (size_t)gs);
	memcpy(a + ncap - tail, sbuf + TEXT_MAX - tail, (size_t)tail);
	// The line table, its gap kept where it is: the entries before it
	// at the start, the ones after it at the new end.
	uint32_t *nl = (uint32_t *)(a + ncap);
	int after = lcap - lg - lgl;
	for (int l = 0; l < lg; l++) nl[l] = sline[l];
	for (int i = 0; i < after; i++) nl[nlcap - after + i] = sline[lcap - after + i];
	lgl = nlcap - lg - after;
	cap = ncap;
	lcap = nlcap;
	gl = cap - len;
	big = true;
	arena_point(a);
	printf("text: %d bytes: moved to an arena of %d KB\n", need, ncap / 1024);
	return true;
}

// Back to sbuf, and the arena's text and table given back, when a
// document small enough to live there is opened or started. The first
// `keep` bytes of the text are copied down (a file just opened); the
// rest of the text is not wanted. What else the arena holds stays.
static void go_small(int keep) {
	if (!big) return;
	memcpy(sbuf, doc, (size_t)keep);
	if (!ar_layout(0, 0, ar_rec, ar_bytes, clip_cap, false)) {
		// Shrinking cannot fail; if the kernel says otherwise, keep the
		// arena as it is and simply stop using its text.
	}
	big = false;
	bline = NULL;
	doc = sbuf;
	cap = TEXT_MAX;
	lcap = MAX_LINES;
	lg = 0;
	lgl = lcap;
	nlines = 0;
	gs = len;
	gl = cap - len;
	arena_point(arena);
}

// Room for `n` more bytes of text: true if there is, after growing the
// arena if it had to. A document that would pass TEXT_MAX on a board
// that has no arena to give is simply full, as it always was.
static bool doc_room(int n) {
	if (n <= gl) return true;
	long need = (long)len + n;
	if (need > 0x7FFFFFF) return false;
	if (!big) return go_big((int)need);
	long ncap = (long)cap * 2;
	if (ncap < need + need / 4) ncap = need + need / 4;
	return arena_resize((int)ncap, lcap) ||
		arena_resize((int)(need + 4096), lcap);
}

// Room for `n` display lines in the line table.
static bool lines_room(int n) {
	if (n <= lcap) return true;
	if (!big && len == 0) return false;		// no document to move
	if (!big) {
		if (!go_big(len + 1)) return false;
		if (n <= lcap) return true;
	}
	int nl = lcap * 2;
	if (nl < n) nl = n;
	return arena_resize(cap, nl) || arena_resize(cap, n + 256);
}

// -- the undo rings in the arena --

// Reverses b[i..j).
static void rev_bytes(uint8_t *b, uint32_t i, uint32_t j) {
	while (i + 1 < j) { uint8_t t = b[i]; b[i++] = b[--j]; b[j] = t; }
}

// Rotates b[0..n) left by k, in place -- three reversals, because there
// is no room for a second copy.
static void rot_left(uint8_t *b, uint32_t n, uint32_t k) {
	if (!n || !(k %= n)) return;
	rev_bytes(b, 0, k);
	rev_bytes(b, k, n);
	rev_bytes(b, 0, n);
}

// Puts the log's oldest record and oldest byte at the start of their
// rings and numbers everything from 0. A ring's contents are where
// (counter % size) says, so this is what lets a ring change size: after
// it, every counter is below the old size, and so below the new one.
static void undo_linear(void) {
	z_undo_t *u = &undo;
	uint32_t f = u->first, b = u->bfirst;
	rot_left((uint8_t *)u->rec, (uint32_t)u->rec_cap * UREC_SZ,
		(f % (uint32_t)u->rec_cap) * UREC_SZ);
	rot_left((uint8_t *)u->bytes, u->bytes_cap, b % u->bytes_cap);
	for (uint32_t i = 0; i < u->end - f; i++) u->rec[i].log -= b;
	u->top -= f;
	u->end -= f;
	if (u->saved != UINT32_MAX) u->saved -= f;
	u->first = 0;
	u->bfirst = 0;
	u->bend -= b;
}

// The rings at `nrc` records and `nbc` bytes, in the arena, history
// kept. False, changing nothing that matters, if there is no arena to
// be had.
static bool undo_grow(int nrc, int nbc) {
	if (nrc < undo.rec_cap) nrc = undo.rec_cap;
	if (nbc < (int)undo.bytes_cap) nbc = (int)undo.bytes_cap;
	undo_linear();
	bool from_bss = !ar_rec;
	if (!ar_layout(big ? cap : 0, big ? lcap : 0, nrc, nbc, clip_cap, true)) return false;
	if (from_bss) {
		memcpy(undo.rec, undo_rec, sizeof(undo_rec));
		memcpy(undo.bytes, undo_bytes, sizeof(undo_bytes));
	}
	return true;
}

// Before `nrec` edits of `nbytes` bytes are recorded. The rings move to
// the arena, at their starting size, the first time the .bss ones would
// have to forget something; and grow, once there, only for a change
// larger than they are -- big enough for it and a starting size's worth
// of history besides. Otherwise the oldest edits are forgotten, as they
// always were: history gets shorter, an edit is never refused. Where
// the arena cannot be had, nothing changes.
static void undo_room(int nrec, long nbytes) {
	z_undo_t *u = &undo;
	bool full = u->top - u->first + (uint32_t)nrec > (uint32_t)u->rec_cap ||
		u->bend - u->bfirst + (uint32_t)nbytes > u->bytes_cap;
	if (!full || nbytes > 0x7FFFFFF) return;
	if (ar_rec && nrec <= u->rec_cap && nbytes <= (long)u->bytes_cap) return;
	int rc = ar_rec ? ar_rec : UNDO_AR_RECORDS;
	long bc = ar_bytes ? ar_bytes : UNDO_AR_BYTES;
	if (nrec > rc) rc = nrec + UNDO_AR_RECORDS;
	if (nbytes > bc) bc = round4k(nbytes + UNDO_AR_BYTES);
	undo_grow(rc, (int)bc);
}

// Can one change of `nrec` edits and `nbytes` bytes be kept whole, so
// that one undo puts it all back? Grows the rings if it has to.
static bool undo_fits(int nrec, long nbytes) {
	undo_room(nrec, nbytes);
	return nrec <= undo.rec_cap && nbytes <= (long)undo.bytes_cap;
}

// A new document: no history, and the rings back in .bss, so an idle
// window holds no arena for them.
static void undo_base(void) {
	z_undo_reset(&undo);
	if (ar_rec) ar_layout(big ? cap : 0, big ? lcap : 0, 0, 0, clip_cap, true);
}

// -- edits --
//
// The only two places the text changes (loading, undo and Replace All
// aside, which re-lay out what they touch). Each moves the gap to the
// end of the paragraph it edits, if it is not there already, and then
// moves only the rest of that paragraph. Both assume the room is there
// (doc_room()).
//
// Before each one the line table is brought up to date (layout_sync())
// and its gap put just after the line the edit is in (lines_at_edit()),
// so every line below is held as its distance from the end and stays
// right through the edit. What changed is left for layout_sync(): the
// text from dirty_lo to dirty_hi is new, and the table's entries past
// it are already where they belong.

static int dirty_lo = -1, dirty_hi;

// Bumped by every change to the text, so autosave can tell whether
// what it is writing is still the document (see "autosave" below).
static uint32_t edit_gen, edit_tick;

static void layout_sync(void);
static int line_at(int off);

// The table's gap to line `k`: entries crossing it change from offset
// to distance-from-the-end, or back. What moves is the lines between
// where it was and where it goes -- none when the edit is where the
// last one was.
static void lgap_to(int k) {
	COST(cost_shifted, k < lg ? lg - k : k - lg);
	while (lg > k) { lg--; LT_SET(lg + lgl, len - LT(lg)); }
	while (lg < k) { LT_SET(lg, len - LT(lg + lgl)); lg++; }
}

// -- wrapping in the background -- docs/text_editor.md, "Wrapping" --
//
// Wrapping a whole large document -- on loading, a resize, a font
// change, a Replace All -- is done a slice at a time between messages,
// so the window comes up as soon as the screen's worth is wrapped and
// keys and clicks are answered meanwhile. While it runs the table ends
// at the FRONTIER, the start of the next line still to wrap; it is held
// as its distance from the end, like the lines after the table's gap,
// so edits before it leave it right. Anything that needs a line past it
// wraps up to there first (line_at(), lines_through()).

#define WRAP_SLICE	8192		// bytes wrapped per pass of the main loop

static bool wrap_bg;
static int wrap_rest;			// len - the frontier

static int next_line_start(int off);
static bool line_put(int off);

// Wraps on from the frontier until a line past `upto` is in the table,
// and at least `budget` bytes, or to the end.
static void wrap_more(int upto, long budget) {
	if (!wrap_bg) return;
	lgap_to(nlines);
	int off = len - wrap_rest;
	long done = 0;
	while (off >= 0 && (off <= upto || done < budget)) {
		if (!line_put(off)) { off = -1; break; }	// the table is full
		int nx = next_line_start(off);
		done += (nx < 0 ? len : nx) - off;
		off = nx;
	}
	if (off < 0) wrap_bg = false;
	else wrap_rest = len - off;
}

// Display line `l` and the screen after it are in the table, or the
// document has ended first.
static void lines_through(int l) {
	while (wrap_bg && nlines <= l + rows) wrap_more(-1, 2048);
}

// Before an edit of [a, b): the table up to date, wrapped past the end
// of the paragraph b is in -- so the edit's own paragraph is all there
// to wrap again -- and its gap just after a's line.
static void lines_at_edit(int a, int b) {
	layout_sync();
	if (wrap_bg) wrap_more(para_end(b), 0);
	lgap_to(line_at(a) + 1);
}

// Opens n bytes of room at `at` and returns where they are: the gap
// goes to the end of the paragraph, and the rest of the paragraph moves
// up. The bytes are the caller's to fill.
static char *raw_insert(int at, int n) {
	lines_at_edit(at, at);
	gap_to(para_end(at));
	memmove(&doc[at + n], &doc[at], (size_t)(gs - at));
	COST(cost_moved, gs - at);
	gs += n;
	gl -= n;
	len += n;
	dirty_lo = at;
	dirty_hi = at + n;
	edit_gen++;
	edit_tick = z_uptime_ticks();
	return &doc[at];
}

// Removes [a, b). `rec` records it for undo first, while the bytes are
// still there to be kept.
static void raw_delete(int a, int b, bool rec) {
	if (rec) undo_room(1, b - a);		// before any pointer into the arena is taken
	lines_at_edit(a, b);
	gap_to(para_end(b));
	if (rec) z_undo_record(&undo, Z_UNDO_DEL, a, &doc[a], b - a, cursor, undo_group);
	memmove(&doc[a], &doc[b], (size_t)(gs - b));
	COST(cost_moved, gs - b);
	gs -= (b - a);
	gl += (b - a);
	len -= (b - a);
	dirty_lo = a;
	dirty_hi = a;
	edit_gen++;
	edit_tick = z_uptime_ticks();
}

// `s` must not point into the arena: a resize may move it. A paste from
// text's own clipboard copy, which is there, has its own way in
// (paste_private()).
static void edit_insert(int at, const char *s, int n) {
	if (n <= 0) return;
	undo_room(1, n);
	z_undo_record(&undo, Z_UNDO_INS, at, s, n, cursor, undo_group);
	memcpy(raw_insert(at, n), s, (size_t)n);
}

static void edit_delete(int a, int b) {
	if (a < 0) a = 0;
	if (b > len) b = len;
	if (b <= a) return;
	raw_delete(a, b, true);
}

// Removes text [a..b).
static void buf_delete_range(int a, int b) {
	edit_delete(a, b);
}

static void buf_clear(void) {
	len = 0;
	gs = 0;
	stage_tail = 0;
	wrap_bg = false;
	go_small(0);
	gl = cap;
	dirty_lo = -1;
	cursor = 0;
	modified = false;
	filename[0] = 0;
	file_enc = ENC_UTF8;
	file_bom = false;
	if (!undo.rec)					// the first document: set the log up
		z_undo_init(&undo, undo_rec, UNDO_RECORDS, undo_bytes, UNDO_BYTES);
	undo_base();
	undo_break();
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

	// Columns are display columns -- next_off() steps over a whole
	// UTF-8 sequence, and width_at() says how many columns it takes:
	// two for a CJK character, which does not start a line it cannot
	// finish. Space and newline are single bytes, so the break logic
	// below needs no change. Japanese has no spaces to break at; a
	// line of it breaks at the width, between any two characters,
	// which is where Japanese is broken anyway.
	while (i < len) {
		char c = B(i);
		if (c == '\n') { COST(cost_wrapped, i - start); return i + 1; }
		// Printable ASCII, one column and one byte: most text, so
		// it does not pay for a decode.
		if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
			if (count + 1 > cols) break;
			if (c == ' ') last_space = i;
			i++;
			count++;
			continue;
		}
		int w = width_at(i);
		if (count + w > cols) break;
		i = next_off(i);
		count += w;
	}

	COST(cost_wrapped, i - start);
	if (i >= len) return len;

	// The character that didn't fit is a newline -- it ends this line
	// anyway, and there is nothing to wrap.
	if (B(i) == '\n') return i + 1;

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
	while (b < len && B(b) == ' ') b++;
	if (b < len && B(b) == '\n') b++;

	return b;

}

// Rebuilds line_off[] from display line `from` onward. LO(from)
// must already be correct -- which is what makes this safe to call
// with the line at the start of an edited paragraph: wrapping is
// paragraph-local, so that offset cannot have moved.
// The line start after `off` in the sequence wrap_from() produces, or
// -1 after the last line. A document ending in a newline has one more,
// empty, line after it -- that's where the cursor sits after pressing
// Enter at the end, and without it there would be nowhere to put it.
static int next_line_start(int off) {
	if (off >= len) return -1;			// the trailing empty line was last
	int next = wrap_one(off);
	if (next >= len)
		return (len > 0 && B(len - 1) == '\n') ? len : -1;
	return next;
}

// Drops every line from `from` on: the gap then runs to the end.
static void lines_cut(int from) {
	if (from < lg) lg = from;
	else lgap_to(from);
	lgl = lcap - lg;
	nlines = lg;
}

// One more line at the gap, as an offset. False if the table is full
// and cannot grow, which only a document of nearly nothing but
// newlines on a board with no arena reaches: the display then stops,
// the way it always did at MAX_LINES.
static bool line_put(int off) {
	if (!lgl && !lines_room(lcap + 1)) return false;
	LT_SET(lg++, off);
	lgl--;
	nlines = lcap - lgl;
	return true;
}

// Rebuilds the table from display line `from` onward. LO(from) must
// already be correct -- which is what makes this safe to call with the
// line at the start of an edited paragraph: wrapping is paragraph-
// local, so that offset cannot have moved.
static void wrap_from(int from) {

	if (from < 0) from = 0;
	if (from > nlines) from = nlines;

	int off = (from < nlines) ? LO(from) : 0;
	if (from == 0) off = 0;

	// The rest is wrapped in the background: as far as the caret and
	// a slice more now, the remainder between messages.
	lines_cut(from);
	wrap_bg = true;
	wrap_rest = len - off;
	wrap_more(cursor, WRAP_SLICE);

	dirty_lo = -1;

}

// After an edit, the line table again: only its own paragraph is
// wrapped -- from that paragraph's first line to the first paragraph
// start past the change, where the old table takes over. Those old
// lines are after the table's gap (lines_at_edit() put it there), held
// as their distance from the end, so they are already right: nothing
// below the edit is touched at all.
static void layout_sync(void) {

	if (dirty_lo < 0) return;
	int hi = dirty_hi;
	dirty_lo = -1;

	// The paragraph the edit is in starts at or before the line before
	// the gap. Every line before the gap is unchanged, as is the text
	// in front of it.
	int L0 = lg > 0 ? lg - 1 : 0;
	while (L0 > 0 && LO(L0) > 0 && B(LO(L0) - 1) != '\n') L0--;

	// The first line after the gap that starts a paragraph the edit
	// did not touch: the newline before it lies at or past hi. Lines
	// the edit removed are skipped on the way: they map below hi.
	int K = lg;
	while (K < nlines && (LO(K) - 1 < hi || B(LO(K) - 1) != '\n')) K++;
	int stop = (K < nlines) ? LO(K) : wrap_bg ? len - wrap_rest : -2;
	int off = LO(L0);

	// Out with lines [L0, K), in with the paragraph's lines as they
	// now wrap -- written at the gap, which then sits after them.
	int after = K - lg;				// old entries after the gap to drop
	lgl += lg - L0;
	lg = L0;
	lgl += after;
	nlines = lcap - lgl;
	while (off >= 0 && off != stop) {
		if (!line_put(off)) { off = -1; break; }
		off = next_line_start(off);
	}
	// Wrapped to the end after all (only if the frontier was not a
	// line start of the new text, which lines_at_edit() rules out):
	// nothing is left for the background.
	if (off < 0) wrap_bg = false;

}

static void rewrap_all(void) {
	lines_cut(0);
	wrap_from(0);
}

#ifdef TEXT_HOST_TEST
// Is the table as layout_sync() leaves it the table a full rewrap
// builds? For the host test (tests/utf8.c).
static bool layout_check(void) {
	while (wrap_bg) wrap_more(-1, 1L << 30);
	int off = 0, l = 0;
	while (off >= 0 && l < nlines) {
		if (LO(l) != off) return false;
		l++;
		off = next_line_start(off);
	}
	return l == nlines && (off < 0 || lgl == 0);
}
#endif

// One past the last byte of display line `l`.
static int line_end(int l) {

	if (l < 0 || l >= nlines) return len;
	if (l + 1 < nlines) return LO(l + 1);

	return len;

}

// Length of line `l` as DRAWN, in BYTES -- the trailing newline is
// part of the line but is not a glyph.
static int line_draw_len(int l) {

	int a = LO(l);
	int b = line_end(l);

	if (b > a && B(b - 1) == '\n') b--;

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

	int a = LO(l);
	int n = chars_between(a, a + line_draw_len(l));

	return n > cols ? cols : n;

}

// The offset of column `col` on line `l`, clamped to line_col_max().
static int line_pos(int l, int col) {

	int maxcol = line_col_max(l);
	if (col > maxcol) col = maxcol;
	if (col < 0) col = 0;

	int a = LO(l);
	return off_after(a, a + line_draw_len(l), col);

}


// The display line containing `off`. Binary search: this runs on
// every cursor move and every edit, and nlines can be in the
// thousands.
static int line_at(int off) {

	if (wrap_bg && off >= len - wrap_rest) wrap_more(off, 0);

	int lo = 0, hi = nlines - 1, res = 0;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (LO(mid) <= off) { res = mid; lo = mid + 1; }
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

	while (l > 0 && LO(l) > 0 && B(LO(l) - 1) != '\n') l--;

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

// Screen row where draw_caret() last left its rule, or -1. The
// scroll blit MOVES those pixels: without this bookkeeping,
// scroll_repaint() translates the old caret onto a surviving row
// and nothing ever erases it -- visible as a small vertical rule
// ghost at the caret's old column (the left text edge, when moving
// through line starts), stacking up one per scroll. term.c keeps
// the same bookkeeping for its cursor overlay (draw_cursor_x/y);
// this is the editor's version of the same lesson.
static int caret_drawn_row = -1;

static void draw_caret(void) {

	int l = line_at(cursor);

	if (l < top_line || l >= top_line + rows) {
		caret_drawn_row = -1;
		return;
	}

	int col = chars_between(LO(l), cursor);
	if (col > cols) col = cols;

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x = c.x0 + TEXT_X0 + col * CHAR_W;
	int y = c.y0 + (l - top_line) * LINE_H;

	// Clip x to the text area -- a cursor at the very end of a full
	// line otherwise lands on the scrollbar.
	if (x > c.x0 + text_w - 1) x = c.x0 + text_w - 1;

	z_fb_hw_line(x, y, x, y + GLYPH_H - 1, 1, &c);

	caret_drawn_row = l - top_line;

}

/* -- render instrumentation --
 *
 * To the serial console, not the window, so measuring does not perturb
 * what is measured.
 *
 * The question for an editor is different from a terminal's. Typing
 * redraws one row, which is cheap and obviously right. SCROLLING is
 * the suspect: moving the view one line should be a single VRAM to
 * VRAM blit of the whole text area plus one new row, and if it is
 * instead a full re-render then every scroll costs rows x cols glyph
 * blits.
 *
 * At the measured ~112 cycles per glyph, a 40x25 screen re-render is
 * ~112,000 cycles -- 14% of a 60Hz frame for ONE line of scroll. Held
 * arrow-down would then be the slowest thing the editor does.
 *
 * rows_per_second is the number to watch: a burst far above the number
 * of lines actually scrolled means whole-screen redraws.
 */
/* 0 in a normal build. Set to 1 to print render counts to the serial
 * console every two seconds -- see the block below for what the
 * numbers mean and which questions they answer.
 *
 * Off by default because it is console noise once the question it was
 * written for has been answered, not because it is expensive: the
 * counters are increments and the clock is read once per main loop,
 * not per row. (It WAS per row at first, which made every repaint slow
 * enough to see as a flash on each keystroke.) */
#define TEXT_INSTRUMENT 0

#if TEXT_INSTRUMENT
static uint32_t ins_rows, ins_chars, ins_fills, ins_last_report;

static void ins_report(void)
{
	uint32_t now = z_uptime_ticks();

	if (ins_last_report == 0) { ins_last_report = now; return; }
	if (now - ins_last_report < Z_TICK_HZ * 2) return;
	if (ins_rows == 0) { ins_last_report = now; return; }

	printf("text: %lu rows drawn, %lu chars, %lu row fills "
		"(%lu chars/row)\n",
		(unsigned long)ins_rows,
		(unsigned long)ins_chars,
		(unsigned long)ins_fills,
		(unsigned long)(ins_rows ? ins_chars / ins_rows : 0));

	ins_rows = ins_chars = ins_fills = 0;
	ins_last_report = now;
}
#else
#define ins_report() ((void)0)
#endif

// Draws one visible row (0..rows-1), clearing it first.
static void draw_row_big(int r);

// What a row's characters leave uncovered, cleared: the left margin,
// the rest of the row from `x` (content-relative), and the line's gap
// row under the glyphs.
static void row_tail(int cy, int x) {
	fill_content(0, cy, TEXT_X0, GLYPH_H, 0);
	if (x < text_w) fill_content(x, cy, text_w - x, GLYPH_H, 0);
	fill_content(0, cy + GLYPH_H, text_w, LINE_H - GLYPH_H, 0);
}

static void draw_row(int r) {

	if (big_font) { draw_row_big(r); return; }

	if (r < 0 || r >= rows) return;

	int cy = r * LINE_H;

	// Not cleared first. Every character below is drawn as a SOLID
	// cell, background included, so only what the characters do not
	// cover is cleared, after them (row_tail()). Clearing the whole row
	// and then drawing it showed the row blank for a moment on every
	// keystroke -- the flash big print, which never cleared, did not
	// have.

#if TEXT_INSTRUMENT
	/* Counters only. ins_report() is NOT called here: it reads the
	 * clock, which is a syscall, and draw_row() runs once per visible
	 * row -- so a full repaint paid 25 syscalls purely to decide it
	 * was not yet time to print. That made every repaint slow enough
	 * to see, which showed up as the window flashing on each
	 * keystroke. Reporting happens once per main loop instead. */
	ins_rows++;
	ins_fills++;
#endif

	int l = top_line + r;
	if (l >= nlines) { fill_content(0, cy, text_w, LINE_H, 0); return; }

	// n is BYTES, clamped to the first `cols` characters; nc is how
	// many characters that is. Columns are characters (see the header
	// comment), so every x position below comes from a character count.
	int off = LO(l);
	int n = off_after(off, off + line_draw_len(l), cols) - off;
	if (n <= 0) { fill_content(0, cy, text_w, LINE_H, 0); return; }

#if TEXT_INSTRUMENT
	ins_chars += (uint32_t)n;
#endif

	// Copied out because the drawing calls take a NUL-terminated
	// string and the buffer is not terminated anywhere in particular.
	// Bounded by cols characters of up to four bytes each, so this is
	// a screen line's worth, not a document's.
	char tmp[Z_SCREEN_W / 5 * Z_UTF8_MAX + 1];
	if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;

	memcpy(tmp, P(off), (size_t)n);		// a line never straddles the gap
	tmp[n] = 0;

	// Tabs would advance by one glyph cell here, which disagrees with
	// how wrap_one() counts them. Rather than teach both sides about
	// tab stops, the insert path turns Tab into spaces (see
	// handle_key()), so a tab can only appear in text loaded from
	// disk. Drawn as a space so it at least occupies its one column.
	// A NUL in a loaded file would end the copy early; it is drawn as
	// a space too, for the same reason.
	for (int i = 0; i < n; i++)
		if (tmp[i] == '\t' || tmp[i] == 0) tmp[i] = ' ';

	int nc = chars_between(off, off + n);

	if (!has_sel()) {
		z_win_draw_utf8_2(&win, TEXT_X0, cy, tmp, 1, 0, cur_font);
		row_tail(cy, TEXT_X0 + nc * CHAR_W);
		return;
	}

	// Split the line into up to three runs -- before, inside, after
	// the selection -- and draw each. The selected run goes through
	// z_fb_draw_utf8_2() with the colours swapped, which is one
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

	// The runs' x positions, in characters.
	int ac = chars_between(off, off + a);
	int bc = chars_between(off, off + b);

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int px = c.x0 + TEXT_X0;
	int py = c.y0 + cy;

	if (a > 0) {
		char save = tmp[a];
		tmp[a] = 0;
		z_fb_draw_utf8_2(px, py, tmp, 1, 0, cur_font, &c);
		tmp[a] = save;
	}

	if (b > a) {
		char save = tmp[b];
		tmp[b] = 0;
		z_fb_draw_utf8_2(px + ac * CHAR_W, py, tmp + a, 0, 1, cur_font, &c);
		tmp[b] = save;
	}

	if (n > b)
		z_fb_draw_utf8_2(px + bc * CHAR_W, py, tmp + b, 1, 0, cur_font, &c);

	// A selection continuing onto the next line takes the rest of
	// this row with it, past the end of the text. Without this a
	// multi-line selection looks like a stack of ragged fragments
	// rather than one continuous block, and it becomes genuinely hard
	// to see where a line ended.
	if (sel_end() > line_end(l) - 1 && sel_start() <= off + n) {
		int rx = TEXT_X0 + nc * CHAR_W;
		if (rx < text_w)
			fill_content(rx, cy, text_w - rx, GLYPH_H, 1);
		row_tail(cy, text_w);
	} else {
		row_tail(cy, TEXT_X0 + nc * CHAR_W);
	}

}

// A row in big print: the line put together in memory, selection and
// all, and copied to the screen in one blit (zbigline.h). The copy
// writes every pixel of the row, so it is not cleared first.
static void draw_row_big(int r) {

	int cy = r * LINE_H;
	int l = top_line + r;

	z_bigline_begin(text_w, LINE_H);

	if (l < nlines) {
		int off = LO(l);
		int n = off_after(off, off + line_draw_len(l), cols) - off;
		if (n > 0) {
			// A line never straddles the gap (draw_row()).
			z_bigline_text(TEXT_X0, P(off), n, big_font);
			if (has_sel()) {
				int a = sel_start() - off, b = sel_end() - off;
				if (a < 0) a = 0;
				if (a > n) a = n;
				if (b < 0) b = 0;
				if (b > n) b = n;
				int x0 = TEXT_X0 + chars_between(off, off + a) * CHAR_W;
				int x1 = TEXT_X0 + chars_between(off, off + b) * CHAR_W;
				// A selection that goes on to the next line takes the
				// rest of this row, as draw_row() does.
				if (sel_end() > line_end(l) - 1 && sel_start() <= off + n)
					x1 = text_w;
				if (x1 > x0) z_bigline_invert(x0, x1);
			}
		}
	}

	z_clip_t c;
	z_win_content_rect(&win, &c);
	z_clip_t t = c;
	t.x1 = c.x0 + text_w - 1;
	z_win_select(&win);
	z_bigline_put(c.x0, c.y0 + cy, &t);

}

// Redraws rows `from`..bottom. The usual argument after an edit: a
// reflowed paragraph can move every line below it and none above.
static void draw_rows_from(int from) {

	if (from < 0) from = 0;

	for (int r = from; r < rows; r++) draw_row(r);

	draw_caret();

}

/* -- scroll-aware repaint --
 *
 * `drawn_top` is the top_line the text area currently SHOWS. When only
 * the scroll position has changed, the rows that stay on screen are
 * already correct -- they are just in the wrong place -- so the
 * hardware can move them and only the newly exposed rows need drawing.
 *
 * Measured at about 4x cheaper than redrawing every row (see
 * docs/gpu_blitter.md). Not a throughput win, since glyph drawing is
 * around 1% of the CPU either way, but it removes the hitch on every
 * scroll.
 *
 * DELIBERATELY SEPARATE FROM repaint(). repaint() is called on
 * Z_WM_REDRAW, after wm has already cleared the region, so it must not
 * assume anything about what is on screen -- which is exactly the
 * assumption this makes. Conflating the two would blit whatever
 * happened to be underneath the window into the text area.
 *
 * -1 means "nothing known on screen", which forces the full path.
 */
static int drawn_top = -1;

/* Trace the first few scrolls, then stay quiet -- see scroll_repaint().
 * Reset by pressing the key that re-arms it (see handle_key). */
static int scroll_dbg = 0;   /* set nonzero to trace again */

static void repaint(void);

static void scroll_repaint(void) {

	int delta = top_line - drawn_top;

	/* A selection makes the rows that stay on screen NOT unchanged:
	 * draw_row() renders selected runs inverted, so moving old pixels
	 * would carry stale highlighting with them. Cheap to detect and
	 * far cheaper than getting it wrong. */
	/* Re-enabled. The fault was never on this side: coordinates,
	 * arguments and the derived source address were all traced
	 * correct on hardware. The corruption was a stale-ack hazard in
	 * the blitter's VRAM-to-VRAM copy path -- the always-acking
	 * vram_wb plus the arbiter's registered response routing left
	 * the previous transaction's ack visible two cycles after
	 * release, and two transitions in the copy path re-entered the
	 * port after only one. Fixed in rtl/gpu/gpu_blit.v
	 * (ST_MEM_SETTLE1/2), reproduced and verified by
	 * rtl/gpu/bench/tb_system_vscroll.v, which drives the real
	 * blitter + arbiter + vram together. See docs/gpu_blitter.md,
	 * "The fix: a stale ack, not a wrong address".
	 *
	 * The tracing below is left in place and costs nothing while
	 * scroll_dbg is 0; set it nonzero to trace again. */

	/* Tracing, for when this is re-enabled.
	 *
	 * On hardware the blit lands horizontally offset by roughly the
	 * window's own x origin -- content from outside the window
	 * appearing inside it. The RTL is verified correct at these
	 * alignments (rtl/gpu/bench/tb_vscroll.v copies VRAM to VRAM at
	 * x = 32, 44, 12 and 63 with every pixel matching), so the fault
	 * is in the coordinates handed over rather than in the copy.
	 *
	 * These prints report BOTH ends of that handover: what this
	 * function computes, and what z_fb_hw_scroll() passes on. If they
	 * agree and are what fill_content() would use for the same
	 * region, the mismatch is further down; if c.x0 is not what this
	 * expects, it is the content rect.
	 *
	 * The comparison line is the useful one: fill_content(0, ...)
	 * draws the text area, and it resolves x to c.x0 + 0. Anything
	 * the blit does differently from that is the bug. */
	if (scroll_dbg > 0) {
		z_clip_t dc;
		z_win_content_rect(&win, &dc);
		scroll_dbg--;
		printf("text scroll: delta=%d top=%d drawn=%d rows=%d LINE_H=%d\n",
			delta, top_line, drawn_top, rows, LINE_H);
		printf("  content rect: x0=%d y0=%d x1=%d y1=%d  (w=%d h=%d)\n",
			(int)dc.x0, (int)dc.y0, (int)dc.x1, (int)dc.y1,
			(int)(dc.x1 - dc.x0 + 1), (int)(dc.y1 - dc.y0 + 1));
		printf("  text_w=%d TEXT_X0=%d  -> blit x=%d w=%d\n",
			text_w, TEXT_X0, (int)dc.x0, text_w);
		z_fb_scroll_debug = 1;
		z_fb_scroll_dbg_armed = 2;
		printf("  align mode = %d (1 = word-aligned, no bit shifting)\n",
			z_fb_scroll_align);
	}


	if (drawn_top < 0 || delta == 0 || delta <= -rows || delta >= rows ||
		has_sel()) {
		repaint();
		return;
	}

	{
		z_clip_t c;
		z_win_content_rect(&win, &c);

		/* Content moves UP by delta*LINE_H when scrolling forward, so
		 * the sign is negated: a larger top_line means the text moves
		 * up the screen. */
		z_fb_hw_scroll((int)c.x0, (int)c.y0, text_w, rows * LINE_H,
			-delta * LINE_H);

		/* Draw only what scrolled in. */
		if (delta > 0)
			for (int r = rows - delta; r < rows; r++) draw_row(r);
		else
			for (int r = 0; r < -delta; r++) draw_row(r);

		/* The old caret's 1px rule moved with the pixels. If its
		 * translated position landed on a SURVIVING row, redraw that
		 * row to erase it -- rows that scrolled in were fully
		 * redrawn just above and need nothing. Without this, every
		 * scroll leaves a rule ghost at the caret's old column. */
		if (caret_drawn_row >= 0) {
			int g = caret_drawn_row - delta;
			if (g >= 0 && g < rows &&
				!(delta > 0 && g >= rows - delta) &&
				!(delta < 0 && g < -delta))
				draw_row(g);
		}
	}

	draw_caret();
	z_scrollbar_draw(&sbar, true);

	drawn_top = top_line;

}

// The mouse wheel (Z_WM_WHEEL): three lines a notch, up = back, like
// term and every desktop editor. It moves the VIEW, not the caret --
// the same as dragging the scrollbar, and by the same path, so it does
// not go through scroll_to_cursor(), which would pull the view straight
// back to the caret. The next key that moves the caret brings it back
// into view.
static void wheel_scroll(int notches) {

	if (notches < 0) lines_through(top_line - 3 * notches);
	int max = nlines - rows;
	int t = top_line - 3 * notches;

	if (max < 0) max = 0;
	if (t > max) t = max;
	if (t < 0) t = 0;
	if (t == top_line) return;

	top_line = t;
	z_scrollbar_set_value(&sbar, top_line);
	scroll_repaint();

}


// Full repaint. Called on Z_WM_REDRAW, which wm sends after it has
// already cleared the region -- so this must not assume anything
// about what is currently on screen.
static void repaint(void) {

	// A file going in or out (load_file(), write_document()): the page
	// is blank, and the title says how far it has got.
	if (io_busy) {
		int cw0 = z_win_content_w(&win), ch0 = z_win_content_h(&win);
		z_win_fill_rect(&win, 0, 0, cw0, ch0 - Z_WIN_GRIP_INSET, 0);
		z_win_fill_rect(&win, 0, ch0 - Z_WIN_GRIP_INSET, cw0 - Z_WIN_GRIP_INSET, Z_WIN_GRIP_INSET, 0);
		return;
	}

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

	drawn_top = top_line;

	draw_caret();

	z_scrollbar_draw(&sbar, true);

}

// -- titlebar --

// "notes.txt *", truncated to what wm will keep (WM_TITLE_MAX is 24,
// and the title also has to share the bar with three icons).
//
// Split out of update_title() so main() can compute the title BEFORE
// creating the window -- see its call site there for why that
// matters.
static void build_title(char *t, int cap, const char *path, bool star) {

	// Latin-9 is said in the title: it is the encoding the file will be
	// saved in, and the one thing about a file that is otherwise
	// invisible. UTF-8 -- every new document -- says nothing. The
	// name is cut, at a character boundary, to leave room for it.
	const char *enc = (file_enc == ENC_LATIN9) ? " (Latin-9)" :
		(file_enc == ENC_CP1252) ? " (Windows-1252)" : "";
	int room = cap - (int)strlen(enc);
	if (room < 2) room = cap;

	z_win_doc_title(t, room, path, star, "untitled");
	strncat(t, enc, (size_t)(cap - 1 - (int)strlen(t)));

}

// The last title actually sent to wm, so a call that would not change
// anything sends nothing.
//
// z_win_set_title() is FIRE AND FORGET (see its comment in zwin.c):
// it queues a message and returns, and wm then does a titlebar repair
// in its own process -- which redraws chrome for every window
// overlapping that strip and asks their owners to redraw. Those
// owners repaint their whole window, unclipped. So a title message
// sent just before this app paints is a race between that repaint and
// ours, and zwin.c's own comment notes this app calls it "on every
// open, every save, and on the first edit after each".
//
// Suppressing the no-op sends removes most of that exposure for free.
// The startup case is handled separately, by creating the window with
// the right title in the first place.
static char sent_title[32];

static void update_title(void) {

	char t[64];
	build_title(t, (int)sizeof(t), filename, modified);

	int i = 0;
	for (; t[i] && t[i] == sent_title[i]; i++);
	if (t[i] == sent_title[i]) return;	// identical -- nothing to send

	for (i = 0; i < (int)sizeof(sent_title) - 1 && t[i]; i++)
		sent_title[i] = t[i];
	sent_title[i] = 0;

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
static char last_dir[Z_FLIST_PATH_MAX] = Z_DIR_HOME;

static void remember_dir(const char *path) {

	int last = 0;

	for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;

	if (last == 0) { last_dir[0] = '/'; last_dir[1] = 0; return; }

	int i = 0;
	for (; i < last && i < (int)sizeof(last_dir) - 1; i++)
		last_dir[i] = path[i];

	last_dir[i] = 0;

}

// -- encodings -- docs/text_editor.md, "Encodings" --
//
// Two single-byte encodings, in which every byte is a character:
//
// Latin-9 (ISO 8859-15) leaves 0x80-0x9f as C1 control codes. Those are
// kept as the codepoints of the same value (U+0080-U+009F) so that ANY
// file read as Latin-9 is written back byte for byte.
//
// Windows-1252 is what Windows Notepad and Word's "plain text" wrote
// for decades: Latin-1 from 0xa0, and curly quotes, dashes, the euro
// and a few letters in 0x80-0x9f. The five bytes it leaves undefined
// are kept as U+0081 and so on, for the same round trip.

static const uint16_t cp1252_hi[32] = {
	0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
	0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// The codepoint for byte b in the file's single-byte encoding.
static uint32_t sb_byte_to_cp(uint8_t b) {
	if (b >= 0x80 && b < 0xA0) {
		if (file_enc == ENC_CP1252 && cp1252_hi[b - 0x80]) return cp1252_hi[b - 0x80];
		return b;
	}
	return (file_enc == ENC_CP1252) ? b : z_l9_to_cp(b);
}

// The byte for cp in the file's single-byte encoding, or -1 if it has
// none.
static int sb_cp_to_byte(uint32_t cp) {
	if (cp == 0) return 0;
	if (file_enc == ENC_CP1252) {
		if (cp < 0x80) return (int)cp;
		if (cp < 0xA0) return cp1252_hi[cp - 0x80] ? -1 : (int)cp;
		if (cp <= 0xFF) return (int)cp;
		for (int i = 0; i < 32; i++)
			if (cp1252_hi[i] == cp) return 0x80 + i;
		return -1;
	}
	if (cp >= 0x80 && cp < 0xA0) return (int)cp;
	uint8_t b = z_cp_to_l9(cp);
	return b ? b : -1;
}

static const char *enc_name(void) {
	return file_enc == ENC_CP1252 ? "Windows-1252" : "Latin-9";
}

// How to read these bytes. Plain ASCII is UTF-8. Otherwise count the
// multi-byte UTF-8 sequences that decode and the bytes that do not:
// text written as UTF-8 is all the former, text written in a single-
// byte encoding is nearly all the latter (a-umlaut in Latin-1 is the
// lone byte 0xE4, which cannot start a UTF-8 character before a
// letter). A UTF-8 file with a stray bad byte or two stays UTF-8 and
// keeps those bytes as they are.
//
// A single-byte file is Windows-1252 if it has any byte in 0x80-0x9f
// that Windows-1252 gives a character to -- a curly quote, a dash, the
// euro. In Latin-9 those bytes are control codes nobody writes into a
// text file, so their presence is the tell; without them the two read
// the same except for eight letters, and Latin-9 is kept.
//
// Counted as the file comes in (enc_scan(), a piece at a time from the
// loader), so the end of an open is not a second pass over the whole
// file: it was one call, a megabyte in about a third of a second.
typedef struct {
	int		at;			// doc[] up to here is counted
	int		good, bad;
	bool	c1;
} enc_scan_t;

// Counts doc[s->at .. to). Unless `last`, a character that might go on
// past `to` waits for the next piece.
static void enc_scan(enc_scan_t *s, const char *b, int to, bool last) {

	const char *p = b + s->at, *end = b + to;

	while (p < end) {
		uint8_t c = (uint8_t)*p;
		if (c < 0x80) { p++; continue; }
		if (!last && end - p < 4) break;
		if (c < 0xA0 && cp1252_hi[c - 0x80]) s->c1 = true;
		const char *q = p;
		z_utf8_next(&p, end);
		if (p - q == 1) s->bad++;
		else s->good++;
	}
	s->at = (int)(p - b);

}

static enc_t enc_result(const enc_scan_t *s) {
	if (s->bad <= s->good) return ENC_UTF8;
	return s->c1 ? ENC_CP1252 : ENC_LATIN9;
}

// A piece at a time, between the window's messages: below.
static const char *io_name;
static void io_begin(const char *verb, const char *path, bool blank);
static void io_end(void);
static bool io_pump(long done, long total, bool stoppable);

// Converts the single-byte text in doc[0..len) (file_enc; the gap at
// the end, as loading leaves it) to UTF-8 in place. Returns false,
// changing nothing, if the result would not fit -- every accented
// letter doubles, and past TEXT_MAX that needs an arena.
static bool sb_to_utf8_in_place(void) {

	// Two passes over the file: the window's mail is read between
	// pieces (io_pump(), the page blank: it is an open), not stoppable --
	// the second pass rewrites the text in place.
	io_begin("Converting", io_name ? io_name : "the file", true);
	int out = 0;
	for (int i = 0; i < len; i++) {
		char u[Z_UTF8_MAX];
		out += z_utf8_put(sb_byte_to_cp((uint8_t)doc[i]), u);
		if ((i & 0xFFFF) == 0xFFFF) io_pump(i, 2L * len, false);
	}
	if (out > cap && !doc_room(out - len)) { io_end(); return false; }

	// Backwards, so the growing output never overtakes the input still
	// to be read: the write position is always at or after the read
	// position.
	int j = out;
	for (int i = len - 1; i >= 0; i--) {
		char u[Z_UTF8_MAX];
		int k = z_utf8_put(sb_byte_to_cp((uint8_t)doc[i]), u);
		j -= k;
		memcpy(&doc[j], u, (size_t)k);
		if ((i & 0xFFFF) == 0) io_pump(2L * len - i, 2L * len, false);
	}
	io_end();

	len = out;
	gs = len;
	gl = cap - len;
	return true;

}

// Characters in the document that the file's single-byte encoding has
// no byte for.
static int sb_unsavable(void) {

	int lost = 0;
	doc_flat();
	const char *p = doc, *end = doc + len;

	while (p < end) {
		uint32_t cp = z_utf8_next(&p, end);
		if (sb_cp_to_byte(cp) < 0) lost++;
	}

	return lost;

}

// -- opening and saving a large file: a piece at a time --
// docs/text_editor.md, "Opening and saving" --
//
// Reading a megabyte from the card takes seconds, and so does writing
// one. In one call the window could not redraw (wm waits for it) and
// nothing could stop it. Both now go a piece at a time, and every
// ~20 ms between pieces the window's mail is read: redraws are served
// (forward_msg(), as while a dialog is up), the title says how far it
// has got, and Esc stops an open. Keys, clicks and the wheel are let
// go meanwhile: the document is not there yet, or is being written --
// an edit in the middle of a save would be saved half. A close is put
// back, for the main loop once the file is done.

static bool io_stopped;
static uint32_t io_t0;
static int io_pct;
static const char *io_verb, *io_name;

static bool io_on;				// io_begin() .. io_end()

// `blank`: the page shows nothing meanwhile (an open: the document is
// not there yet). A save leaves it drawn.
static void io_begin(const char *verb, const char *path, bool blank) {
	const char *b = strrchr(path, '/');
	io_name = b ? b + 1 : path;
	io_verb = verb;
	io_t0 = z_uptime_ticks();
	io_pct = -1;
	io_stopped = false;
	io_busy = blank;
	io_on = true;
}

static void io_end(void) {
	io_busy = false;
	if (!io_on) return;
	io_on = false;
	if (io_pct < 0) return;			// quick: the title was never changed
	sent_title[0] = 0;				// what the title said meanwhile is not the cache's
	update_title();
}

// Between two pieces: true once Esc has stopped it (only if `stoppable`).
static bool io_pump(long done, long total, bool stoppable) {
	if (z_uptime_ticks() - io_t0 < Z_TICK_HZ / 50) return io_stopped;
	io_t0 = z_uptime_ticks();
	z_msg_t m;
	bool closing = false;
	z_msg_t close_msg;
	while (z_msg_read(&m) == Z_OK) {
		switch (m.subject) {
			case Z_WM_KEY:
				if (stoppable && m.obj.type == Z_UINT32 &&
				    Z_WM_UNPACK_KEY_PRESSED(m.obj.val.uint32) &&
				    Z_WM_UNPACK_KEY_KEYSYM(m.obj.val.uint32) == 0x1b)
					io_stopped = true;
				break;
			case Z_WM_MOUSE: case Z_WM_WHEEL: case Z_WM_TITLEBAR_ICON:
				break;
			case Z_WM_CLOSE:
				if (stoppable) io_stopped = true;
				close_msg = m;
				closing = true;
				break;
			default:
				forward_msg(&m, NULL);
				break;
		}
	}
	if (closing) z_msg_unread(&close_msg);
	int pct = total > 0 ? (int)(done * 100 / total) : 0;
	if (pct != io_pct) {
		io_pct = pct;
		char t[64];
		snprintf(t, sizeof(t), stoppable ? "%s %.24s %d%%  (Esc stops)" : "%s %.24s %d%%",
			io_verb, io_name, pct);
		z_win_set_title(&win, t);
	}
	return io_stopped;
}

// Writes the document to `path` in the file's own encoding: the
// buffer as it is for UTF-8 (after the byte order mark, if the file
// had one), converted a small piece at a time for Latin-9, so that
// saving needs no second copy of the document -- there is no room for
// one (see load_file()). Latin-9 has no byte for some characters;
// those are written as '?', and do_save_to() asks first.
static bool write_document(const char *path) {

	int h = fs_open_write(path);
	if (h < 0) return false;

	bool ok = true;

	// In one run, so the pieces below are plain slices of it.
	doc_flat();

	if (file_enc == ENC_UTF8) {

		if (file_bom && fs_write_chunk(h, "\xEF\xBB\xBF", 3) != 3) ok = false;

		for (int i = 0; ok && i < len; ) {
			int n = len - i > 4096 ? 4096 : len - i;
			// From a buffer of our own when the document is in the
			// arena: what goes to fs travels as a message, and a
			// message carries only memory in the sender's own block
			// (zproc.h, Z_SYS_ARENA). Read straight from the arena,
			// it would be refused.
			const char *src = &doc[i];
			char stage[4096];
			if (big) {
				if (n > (int)sizeof(stage)) n = (int)sizeof(stage);
				memcpy(stage, src, (size_t)n);
				src = stage;
			}
			if (fs_write_chunk(h, src, n) != n) ok = false;
			i += n;
			if (io_on) io_pump(i, len, false);
		}

	} else {

		char out[256];
		int o = 0;
		const char *p = doc, *end = doc + len;

		while (ok && p < end) {
			int b = sb_cp_to_byte(z_utf8_next(&p, end));
			out[o++] = (char)(b < 0 ? '?' : b);
			if (o == (int)sizeof(out) || p >= end) {
				if (fs_write_chunk(h, out, o) != o) ok = false;
				o = 0;
				if (io_on) io_pump((long)(p - doc), len, false);
			}
		}

	}

	if (!fs_close_handle(h)) ok = false;
	return ok;

}


// Saving without risking the file that is there: the document is
// written to NAME.part, and only once that is complete and closed does
// it take the file's name (draw does the same, drawio.c). A full card
// or a power cut mid-save used to leave the only copy truncated; now
// it leaves the old file as it was, and a .part beside it at worst.
//
// FAT will not rename onto a name that exists, so the old file is
// deleted first. Between that and the rename the .part is the only
// copy -- complete, and kept under its own name if the rename fails.
typedef enum { SAVE_OK, SAVE_FAILED, SAVE_KEPT_PART } save_rv_t;

static save_rv_t write_safely_(const char *path, char *part, int part_size);

static save_rv_t write_safely(const char *path, char *part, int part_size) {
	io_begin("Saving", path, false);
	save_rv_t r = write_safely_(path, part, part_size);
	io_end();
	return r;
}

static save_rv_t write_safely_(const char *path, char *part, int part_size) {
	int n = (int)strlen(path);
	if (n + 6 > part_size) return write_document(path) ? SAVE_OK : SAVE_FAILED;
	memcpy(part, path, (size_t)n);
	memcpy(part + n, ".part", 6);
	if (!write_document(part)) {
		fs_unlink(part);
		return SAVE_FAILED;
	}
	int err = 0;
	if (fs_rename(part, path, &err)) return SAVE_OK;
	if (err != Z_FS_ERR_EXIST || !fs_unlink((char *)path)) {
		fs_unlink(part);			// the old file is still there: no harm done
		return SAVE_FAILED;
	}
	return fs_rename(part, path, &err) ? SAVE_OK : SAVE_KEPT_PART;
}

// -- autosave -- docs/text_editor.md, "Autosave" --
//
// Two seconds after the last edit, a copy of the document goes to the
// RAM disk -- /ram, never the card: writing to the card at moments the
// person did not choose is how a hard power-off corrupts it -- written
// AUTOSAVE_CHUNK bytes per pass of the main loop so that typing
// is never held up by it -- an edit in the middle abandons the copy,
// and it starts again at the next pause. It is written as
// T<hash>.NEW and renamed over T<hash>.SAV only when complete, so a
// .SAV is always a whole document. T<hash>.INF beside it says which
// file it is a copy of, and in what encoding that file is saved.
//
// Saving, or choosing not to save, deletes the copy. If text ends any
// other way -- a crash, a kill -- the copy is still there, and opening
// the file offers it back (autosave_offer()). The RAM disk does not
// survive a reboot, and on a board with none there is no autosave.

#define AUTOSAVE_IDLE	(2 * Z_TICK_HZ)
#define AUTOSAVE_CHUNK	4096

static int as_h = -1, as_pos;
static uint32_t as_gen, as_done_gen;
static int8_t as_ram = -1;		// is there a RAM disk? -1: not asked yet

static bool as_have_ram(void) {
	if (as_ram < 0) {
		z_fs_info_t fi;
		as_ram = fs_stat(Z_DIR_RAM, &fi) ? 1 : 0;
	}
	return as_ram > 0;
}

// /ram/T<7 hex digits>.<ext>: 8.3, from an FNV-1a hash of the path ("" for
// a document never saved).
static void as_name(char *out, int cap, const char *path, const char *ext) {
	uint32_t h = 2166136261u;
	for (const char *p = path; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
	snprintf(out, (size_t)cap, Z_DIR_RAM "/T%07lX.%s", (unsigned long)(h & 0x0FFFFFFFu), ext);
}

// What a copy is filed under: the file's path, or, for a document never
// saved, this window's own process -- "untitled <pid> <serial>". Every
// untitled document was filed under "" once, so two untitled windows
// wrote over each other's copy, and a second one started offered the
// first one's live copy back as "a session that did not end" -- and
// deleted it on No. The serial tells a window that is gone from a pid
// used again (z_proc_serial()).
static char as_untitled[40];

static const char *as_key(void) {
	if (filename[0]) return filename;
	if (!as_untitled[0]) {
		uint32_t pid = z_getpid();
		snprintf(as_untitled, sizeof(as_untitled), "untitled %lu %lu",
			(unsigned long)pid, (unsigned long)z_proc_serial(pid));
	}
	return as_untitled;
}

static void as_abort(void) {
	char n[40];
	if (as_h >= 0) { fs_close_handle(as_h); as_h = -1; }
	as_name(n, sizeof(n), as_key(), "NEW");
	fs_unlink(n);
}

// The document is saved, or its changes are being thrown away: the
// copy goes, and nothing is written until the next edit.
static void autosave_discard(void) {
	char n[40];
	as_done_gen = edit_gen;
	if (!as_have_ram()) return;
	as_abort();
	as_name(n, sizeof(n), as_key(), "SAV");
	fs_unlink(n);
	as_name(n, sizeof(n), as_key(), "INF");
	fs_unlink(n);
	as_done_gen = edit_gen;
}

// One step: start a copy after the pause, write a piece of it, or
// finish it. True while a copy is being written, so the main loop
// comes straight back rather than waiting.
static bool autosave_step(void) {

	char n[40], m[40];

	if (as_h >= 0 && as_gen != edit_gen) as_abort();	// edited meanwhile

	if (as_h < 0) {
		if (!modified || edit_gen == as_done_gen ||
			z_uptime_ticks() - edit_tick < AUTOSAVE_IDLE) return false;
		if (!as_have_ram()) { as_done_gen = edit_gen; return false; }
		as_name(n, sizeof(n), as_key(), "NEW");
		if ((as_h = fs_open_write(n)) < 0) {
			as_done_gen = edit_gen;			// cannot write it: not until the next edit
			return false;
		}
		as_gen = edit_gen;
		as_pos = 0;
		return true;
	}

	if (as_pos < len) {
		// A piece of the text, never across the gap, through a buffer
		// of our own: a message carries only memory in our block.
		char stage[AUTOSAVE_CHUNK];
		int k = len - as_pos;
		if (k > AUTOSAVE_CHUNK) k = AUTOSAVE_CHUNK;
		if (as_pos < gs && as_pos + k > gs) k = gs - as_pos;
		memcpy(stage, P(as_pos), (size_t)k);
		if (fs_write_chunk(as_h, stage, k) != k) {
			as_abort();
			as_done_gen = edit_gen;			// the RAM disk is full: wait for the next edit
			return false;
		}
		as_pos += k;
		return true;
	}

	// Complete: in place of the last copy, with its note.
	fs_close_handle(as_h);
	as_h = -1;
	as_name(n, sizeof(n), as_key(), "SAV");
	as_name(m, sizeof(m), as_key(), "NEW");
	fs_unlink(n);
	if (!fs_rename(m, n, NULL)) { fs_unlink(m); as_done_gen = edit_gen; return false; }
	as_name(n, sizeof(n), as_key(), "INF");
	{
		char inf[Z_FS_PATH_MAX + 16];
		int k = snprintf(inf, sizeof(inf), "%s\n%d %d\n", as_key(), (int)file_enc, (int)file_bom);
		fs_write_file(n, inf, k);
	}
	as_done_gen = as_gen;
	printf("text: autosaved %d bytes\n", len);
	return false;

}

static bool load_file(const char *path);
static void update_title(void);

// The copy filed as `sav`, whose note `inf` holds `note` (its first
// line, then "<enc> <bom>"): offered, and on Yes put in the document,
// still unsaved, still belonging to the file and its encoding; on No,
// or once it is in, its files are deleted.
static void offer_copy(const char *sav, const char *inf, const char *rest, bool theirs) {

	// "<enc> <bom>": two digits, read without sscanf, which would bring
	// newlib's scanf into the link.
	int enc = (rest[0] >= '0' && rest[0] <= '2') ? rest[0] - '0' : 0;
	int bom = rest[0] && rest[2] == '1';
	{
		int h = fs_open_read(sav);
		if (h < 0) return;
		fs_close_handle(h);
	}

	int r = z_dialog_confirm(&dlg_ctx, "Recover",
		"Unsaved changes from a\nsession that did not end\nwere kept. Recover them?",
		Z_DIALOG_YES_NO);
	if (r == Z_DIALOG_YES) {
		char keep[Z_FS_PATH_MAX];
		snprintf(keep, sizeof(keep), "%s", filename);
		if (!load_file(sav)) return;
		snprintf(filename, sizeof(filename), "%s", keep);
		if (keep[0]) remember_dir(keep);
		file_enc = (enc_t)enc;
		file_bom = bom != 0;
		z_undo_saved(&undo);
		undo.saved = UINT32_MAX;				// not what is on disk
		modified = false;
		set_modified(true);
		update_title();
	}
	if (r == Z_DIALOG_YES && !theirs) {
		as_done_gen = edit_gen;				// in /ram already, under this key
		return;
	}
	// Declined, or another window's: its files go. A recovered untitled
	// copy is kept from here under this window's own key (as_key()),
	// written at the next pause.
	fs_unlink((char *)sav);
	fs_unlink((char *)inf);
	as_done_gen = r == Z_DIALOG_YES ? edit_gen - 1 : edit_gen;

}

// Is the untitled copy whose note starts `key` from a window that is
// gone? "" is an older text's (it filed every untitled copy so), and
// "untitled <pid> <serial>" one whose process is not running now.
// A decimal number at *p, and *p past it and one separator. Not
// strtoul(): that brings newlib's locale tables, 700 bytes, for this.
static uint32_t as_num(const char **p) {
	uint32_t v = 0;
	while (**p >= '0' && **p <= '9') v = v * 10 + (uint32_t)(*(*p)++ - '0');
	if (**p) (*p)++;
	return v;
}

static bool as_orphan(const char *key) {
	if (!key[0]) return true;
	if (strncmp(key, "untitled ", 9)) return false;
	const char *p = key + 9;
	uint32_t pid = as_num(&p);
	uint32_t ser = as_num(&p);
	return z_proc_serial(pid) != ser;
}

// After a file is opened, or at the start with none: a copy of it left
// by a session that did not end, offered back. With a file, the copy
// filed under its path; with none, the first untitled copy in /ram
// whose window is gone.
static void autosave_offer(void) {

	char sav[40], inf[40], note[Z_FS_PATH_MAX + 16];
	if (!as_have_ram()) return;

	if (filename[0]) {
		as_name(sav, sizeof(sav), as_key(), "SAV");
		as_name(inf, sizeof(inf), as_key(), "INF");
		int n = fs_read_file(inf, note, (int)sizeof(note) - 1);
		if (n <= 0) return;
		note[n] = 0;
		char *nl = strchr(note, '\n');
		if (!nl) return;
		*nl = 0;
		if (strcmp(note, filename)) return;			// a hash that collided
		offer_copy(sav, inf, nl + 1, false);
		return;
	}

	char names[1024];
	uint32_t count = 0;
	if (!fs_list_into(Z_DIR_RAM, names, sizeof(names), NULL, 64, &count, NULL)) return;
	const char *e = names;
	for (uint32_t i = 0; i < count; i++, e += strlen(e) + 1) {
		const char *base = strrchr(e, '/');
		base = base ? base + 1 : e;
		int bl = (int)strlen(base);
		if (base[0] != 'T' || bl != 12 || strcmp(base + 8, ".INF")) continue;
		// T<7 hex>.INF, twelve characters: as_name()'s, nothing else
		memcpy(inf, Z_DIR_RAM "/", sizeof(Z_DIR_RAM));
		memcpy(inf + sizeof(Z_DIR_RAM), base, 13);
		int n = fs_read_file(inf, note, (int)sizeof(note) - 1);
		if (n <= 0) continue;
		note[n] = 0;
		char *nl = strchr(note, '\n');
		if (!nl) continue;
		*nl = 0;
		if (!as_orphan(note)) continue;
		snprintf(sav, sizeof(sav), "%s", inf);
		memcpy(sav + strlen(sav) - 3, "SAV", 3);
		offer_copy(sav, inf, nl + 1, true);
		return;
	}

}

static bool do_save_to(const char *path) {

	// A single-byte file that has gained characters its encoding cannot
	// hold: ask, rather than quietly turning them into question marks.
	// Yes switches the file to UTF-8, which holds everything; No writes
	// the old encoding anyway, with '?' in their place.
	if (file_enc != ENC_UTF8) {
		int lost = sb_unsavable();
		if (lost) {
			char msg[112];
			snprintf(msg, sizeof(msg),
				"%d character%s cannot be saved\nas %s. Save as UTF-8\ninstead?",
				lost, lost == 1 ? "" : "s", enc_name());
			int r = z_dialog_confirm(&dlg_ctx, "Encoding", msg, Z_DIALOG_YES_NO_CANCEL);
			if (r == Z_DIALOG_CANCEL) return false;
			if (r == Z_DIALOG_YES) file_enc = ENC_UTF8;
		}
	}

	// A short or failed write means the card filled up or failed
	// mid-way, which must not be reported as success -- the file on
	// disk is now a truncated version of the document.
	char part[Z_FS_PATH_MAX];
	save_rv_t sv = write_safely(path, part, (int)sizeof(part));
	if (sv == SAVE_FAILED) {
		z_dialog_confirm(&dlg_ctx, "Save failed",
			"The file could not be\nwritten completely.\nThe one on the card is\nas it was.", Z_DIALOG_OK_CANCEL);
		return false;
	}
	if (sv == SAVE_KEPT_PART) {
		char msg[Z_FS_PATH_MAX + 80];
		snprintf(msg, sizeof(msg), "Saved, but as\n%.60s:\nit could not be renamed.", part);
		z_dialog_confirm(&dlg_ctx, "Save", msg, Z_DIALOG_OK_CANCEL);
		return false;
	}

	// The copy in /tmp is of the document under its old name, which
	// is about to change if this is a Save As.
	autosave_discard();

	if (path != filename) {
		int i = 0;
		for (; path[i] && i < (int)sizeof(filename) - 1; i++)
			filename[i] = path[i];
		filename[i] = 0;
	}

	remember_dir(filename);
	set_modified(false);
	as_done_gen = edit_gen;		// on disk now: nothing to keep a copy of
	z_undo_saved(&undo);		// undoing back to here is "unmodified" again
	undo_break();
	update_title();

	return true;

}

// Save As onto another system's format -- report.docx, notes.odt,
// .rtf, .html: the document is written to /tmp as it is (Markdown if it
// is a .md, plain text otherwise) and handed to convert, whose own
// window shows the progress, Cancel and any error (docs/convert.md).
// The document itself keeps its name and its unsaved state: an export
// is a copy, not a save.
// The extension of `path`, lower-cased into `out` (empty if none).
static void ext_of(const char *path, char *out) {
	const char *e = NULL;
	for (const char *p = path; *p; p++) if (*p == '.') e = p + 1; else if (*p == '/') e = NULL;
	int n = 0;
	if (e) for (; e[n] && n < 5; n++) out[n] = (char)(e[n] | 0x20);
	out[n] = 0;
}

static bool export_ext(const char *path) {
	char e[6];
	ext_of(path, e);
	return !strcmp(e, "docx") || !strcmp(e, "odt") || !strcmp(e, "rtf") ||
		!strcmp(e, "html") || !strcmp(e, "htm");
}

static bool do_export(const char *path) {
	char job[Z_FS_PATH_MAX + 80], e[6];
	ext_of(filename, e);
	const char *tmp = !strcmp(e, "md") ? Z_DIR_TMP "/text-export.md" : Z_DIR_TMP "/text-export.txt";
	enc_t enc = file_enc;
	file_enc = ENC_UTF8;			// what convert reads best
	bool ok = write_document(tmp);
	file_enc = enc;
	int n = snprintf(job, sizeof(job), "in=%s\nout=%s\nui=1\n", tmp, path);
	if (ok && n > 0 && n < (int)sizeof(job) &&
		fs_write_file(Z_DIR_TMP "/text-export.job", job, n) == n) {
		z_launch_arg_set("-j " Z_DIR_TMP "/text-export.job");
		if (z_proc_run("convert")) return true;
	}
	z_dialog_confirm(&dlg_ctx, "Export", ok ? "convert could not\nbe started." :
		"Could not write the\ncopy to convert.", Z_DIALOG_OK_CANCEL);
	return false;
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

	// Another file of that name: ask. The save dialog leaves this to
	// the caller (zdialog.h), and Save As is where it matters -- the
	// first save of a new document, or Ctrl+Shift+S onto a name that
	// is already taken.
	if (strcmp(path, filename)) {
		int h = fs_open_read(path);
		if (h >= 0) {
			fs_close_handle(h);
			char msg[112];
			const char *base = path;
			for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
			snprintf(msg, sizeof(msg), "%.40s already exists.\nReplace it?", base);
			if (z_dialog_confirm(&dlg_ctx, "Save As", msg, Z_DIALOG_YES_NO) != Z_DIALOG_YES)
				return false;
		}
	}

	if (export_ext(path)) return do_export(path);

	return do_save_to(path);

}

static bool do_save(void) {

	if (!filename[0]) return do_save_as();

	// A file in /tmp is gone at the next boot -- most often a document
	// convert made from another system's format and opened here
	// (docs/convert.md). Its first save asks where, starting at /home.
	if (!strncmp(filename, Z_DIR_TMP "/", sizeof(Z_DIR_TMP))) {
		strcpy(last_dir, Z_DIR_HOME);
		return do_save_as();
	}

	return do_save_to(filename);

}

// -- printing (docs/text_editor.md, "Printing") --
//
// The print dialog (zdialog.h), then the text -- all of it, or the
// selection -- written to a file in /tmp and handed to print, which
// lays it out on pages itself (docs/print.md, "Printing text"). So text
// carries the dialog and a few hundred bytes, not the page renderer:
// it is in flash, and that would not fit.

static z_print_t printing;

// The document's characters [a, b) to `path`, as UTF-8 -- what print
// reads, whatever the file's own encoding -- in pieces, with the title
// showing how far it has got. False if it could not all be written.
static bool write_text(const char *path, int a, int b) {

	int h = fs_open_write(path);
	if (h < 0) return false;

	bool ok = true;
	doc_flat();
	io_begin("Getting ready to print", path, false);

	for (int i = a; ok && i < b; ) {
		int n = b - i > 4096 ? 4096 : b - i;
		const char *src = &doc[i];
		char stage[4096];
		if (big) {		// from the arena: through a buffer of our own (write_document())
			memcpy(stage, src, (size_t)n);
			src = stage;
		}
		if (fs_write_chunk(h, src, n) != n) ok = false;
		i += n;
		io_pump(i - a, b - a, false);
	}

	if (!fs_close_handle(h)) ok = false;
	io_end();
	if (!ok) fs_unlink((char *)path);
	return ok;

}

static void do_print(void) {

	// the document's name, without folder or ending: the job's title,
	// and the file a PDF is offered as
	char base[64];
	const char *name = filename[0] ? filename : "Untitled";
	const char *slash = strrchr(name, '/');
	if (slash) name = slash + 1;
	snprintf(base, sizeof(base), "%s", name);
	char *dot = strrchr(base, '.');
	if (dot && dot != base) *dot = 0;

	z_print_opts_t o;
	z_print_opts_init(&o);
	o.selection = has_sel();

	char title[80];
	snprintf(title, sizeof(title), "Print %.60s", base);
	if (!z_dialog_print(&dlg_ctx, title, last_dir, base, &o)) return;

	int a = 0, b = len;
	if (o.pages == Z_PRINT_PAGES_SELECTION && has_sel()) {
		a = sel_start();
		b = sel_end();
	}

	char tpath[48];
	snprintf(tpath, sizeof(tpath), Z_DIR_TMP "/prt%06lx.txt",
		(unsigned long)(z_uptime_ticks() & 0xFFFFFF));

	if (!write_text(tpath, a, b)) {
		z_dialog_confirm(&dlg_ctx, "Print",
			"There was no room in " Z_DIR_TMP "\nfor the text to print.", Z_DIALOG_OK_CANCEL);
		return;
	}

	if (!z_print_begin(&printing, base)) {
		fs_unlink(tpath);
		z_dialog_confirm(&dlg_ctx, "Print",
			"The print job could not\nbe made in " Z_DIR_TMP ".", Z_DIALOG_OK_CANCEL);
		return;
	}
	z_print_opts_job(&o, &printing.w);
	z_print_text_job(&printing, tpath);

	// print shows its own window from here: what it is doing, Cancel,
	// and how it ended
	if (!z_print_end(&printing)) {
		fs_unlink(tpath);
		z_dialog_confirm(&dlg_ctx, "Print", printing.result == Z_PRINT_E_GONE ?
			"print is not on the card." : "The print job could not\nbe written.",
			Z_DIALOG_OK_CANCEL);
	}

}

// Offers to save when the document has unsaved changes. Returns false
// if the user cancelled, meaning whatever prompted this (new, open,
// close) must not proceed.
static bool confirm_discard(void) {

	if (!modified) return true;

	int r = z_dialog_confirm(&dlg_ctx, "Unsaved changes",
		"Save changes before\ncontinuing?", Z_DIALOG_YES_NO_CANCEL);

	if (r == Z_DIALOG_CANCEL) return false;
	if (r == Z_DIALOG_NO) { autosave_discard(); return true; }

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
// A file that did not open: the window is an empty, untitled document,
// its line table rebuilt for it (buf_clear() leaves the old one, and
// the next draw read past its end), drawn as such.
static void open_failed(void) {
	buf_clear();
	rewrap_all();
	top_line = 0;
	io_end();
	repaint();
}

static bool load_file(const char *path) {

	int size = fs_size((char *)path);

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

	// Past TEXT_MAX the document goes to the arena, sized for the file
	// (doc_room()); where there is no arena to be had, it is as it
	// always was: too large.
	if (size > cap && !doc_room(size)) {
		fs_close_handle(h);
		open_failed();
		z_dialog_confirm(&dlg_ctx, "Too large",
			"That file is larger than\nthis editor can hold on\nthis machine.",
			Z_DIALOG_OK_CANCEL);
		return false;
	}

	// The gap stays at the end while the file comes in, through a
	// buffer of our own: into the arena because fs writes into a
	// client's block through the grant window, and the arena is not in
	// it; into sbuf too, so CRLF is folded as each piece arrives rather
	// than in a pass over the whole file after. A CR that ends a piece
	// waits for the next one's first byte.
	//
	// Normalize CRLF on the way in. A file written on another machine
	// otherwise shows a stray glyph at the end of every line, and
	// every one of them counts against the wrap width.
	io_begin("Opening", path, true);
	long read_total = 0;
	bool cr_held = false, read_err = false, full = false;
	enc_scan_t es = { 0, 0, 0, false };
	for (;;) {
		char stage[4096];
		int room = cap - len - (cr_held ? 1 : 0);
		if (room <= 0) {
			// The file is longer than it said: room for more, or it
			// does not open -- never half of it under its name.
			if (!doc_room(4096)) { full = true; break; }
			room = cap - len - (cr_held ? 1 : 0);
		}
		int want = room < (int)sizeof(stage) ? room : (int)sizeof(stage);
		int n = fs_read_chunk(h, stage, want);
		if (n < 0) { read_err = true; break; }
		if (n == 0) break;
		read_total += n;
		for (int r = 0; r < n; r++) {
			char c = stage[r];
			if (cr_held) {
				cr_held = false;
				if (c != '\n') doc[len++] = '\r';
			}
			if (c == '\r') { cr_held = true; continue; }
			doc[len++] = c;
		}
		gs = len;
		gl = cap - len;
		enc_scan(&es, doc, len, false);
		if (io_pump(read_total, size, true)) break;
	}
	if (cr_held && !io_stopped) doc[len++] = '\r';
	gs = len;
	gl = cap - len;

	fs_close_handle(h);
	io_end();

	if (io_stopped || read_err || full) {
		open_failed();
		if (read_err)
			z_dialog_confirm(&dlg_ctx, "Open failed",
				"That file could not\nbe read.", Z_DIALOG_OK_CANCEL);
		else if (full)
			z_dialog_confirm(&dlg_ctx, "Too large",
				"That file is larger than\nthis editor can hold on\nthis machine.",
				Z_DIALOG_OK_CANCEL);
		return false;
	}

	// The encoding (see enc_scan()). A UTF-8 byte order mark is
	// taken off -- it is not text, and would sit in front of the first
	// character as a box -- and put back on save.
	if (len >= 3 && (uint8_t)doc[0] == 0xEF && (uint8_t)doc[1] == 0xBB &&
	    (uint8_t)doc[2] == 0xBF) {
		memmove(doc, doc + 3, (size_t)(len - 3));
		len -= 3;
		file_bom = true;
	}
	gs = len;
	gl = cap - len;

	// the last few bytes, and the answer; the BOM's three moved the rest
	// down, but it is UTF-8 then whatever the counts say
	if (!file_bom) enc_scan(&es, doc, len, true);
	file_enc = file_bom ? ENC_UTF8 : enc_result(&es);

	if (file_enc != ENC_UTF8 && !sb_to_utf8_in_place()) {
		open_failed();
		z_dialog_confirm(&dlg_ctx, "Too large",
			"That file is too large for\nthis editor once its accented\nletters are converted.",
			Z_DIALOG_OK_CANCEL);
		return false;
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

	// Another system's document: convert opens it, as Markdown, in a
	// text (or read) of its own (docs/convert.md).
	{
		char e[6];
		ext_of(path, e);
		if (!strcmp(e, "docx") || !strcmp(e, "odt") || !strcmp(e, "rtf") || !strcmp(e, "epub")) {
			z_launch_arg_set(path);
			z_proc_run("convert");
			return;
		}
	}

	if (load_file(path)) {
		autosave_offer();
		repaint();
	}

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

	// 5x8 -> 6x12 -> big print (10x20) -> 5x8. Big print only where the
	// blitter can copy from memory (zbigline.h); elsewhere the two sizes.
	if (big_font) {
		big_font = NULL;
		cur_font = &z_font_5x8;
	} else if (cur_font == &z_font_5x8) {
		cur_font = &z_font_6x12;
	} else if (z_bigline_available()) {
		big_font = &z_bigfont_10x20;
	} else {
		cur_font = &z_font_5x8;
	}

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

	layout_sync();
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

	edit_delete(a, b);

	cursor = a;
	sel_anchor = -1;

	after_edit(para);

	return true;

}

// -- the clipboard -- docs/text_editor.md, "Copy and paste" --
//
// wm's clipboard holds a page, Z_WM_CLIP_MAX - 1 bytes, and keeps the
// start of anything longer. A copy that size or smaller goes there and
// nowhere else. A longer one is also kept whole in text's own arena
// (clip_len bytes at ar_clip_off()), and wm is given its first page
// (clip_prefix bytes, cut at a character). A paste here takes the
// whole copy when wm's clipboard still holds exactly that first page
// -- nothing has been copied since, here or in another app -- and
// wm's otherwise. Other apps get the first page, which the copy says
// when it cannot do better.
//
// A cut deletes only what it can give back: a selection over a page is
// cut only if the whole of it is kept for the paste and the delete can
// be undone; otherwise it is refused, with a reason, and nothing is
// deleted. Before October 2026 the cut went ahead and the text past the
// first page was gone.

#define CLIP_PAGE	(Z_WM_CLIP_MAX - 1)		// what wm keeps

static int clip_prefix;		// bytes of the copy wm was given, while clip_len

static char *clip_at(void) { return (char *)arena + ar_clip_off(); }

// Room in the arena for a copy of `n` bytes; false if it cannot be had.
static bool clip_hold(int n) {
	if (clip_cap >= n) return true;
	return ar_layout(big ? cap : 0, big ? lcap : 0, ar_rec, ar_bytes, round4k(n), true);
}

// The private copy given up, and its memory with it.
static void clip_drop(void) {
	clip_len = 0;
	clip_prefix = 0;
	if (clip_cap) ar_layout(big ? cap : 0, big ? lcap : 0, ar_rec, ar_bytes, 0, true);
}

// How much of `n` bytes from `s` fits a page, ending on a character.
static int page_of(const char *s, int n) {
	if (n <= CLIP_PAGE) return n;
	int k = CLIP_PAGE;
	while (k > 0 && ((uint8_t)s[k] & 0xC0) == 0x80) k--;
	return k;
}

// Copies the selection. True if a paste will give all of it back.
static bool do_copy(void) {

	if (!has_sel()) return true;

	int a = sel_start(), n = sel_end() - a;
	// A selection across the gap is put in one run first; a copy is a
	// one-off, and the next keystroke moves the gap back.
	if (a < gs && a + n > gs) doc_flat();

	if (n <= CLIP_PAGE) {
		if (clip_len) clip_drop();
		// Pointed straight at the document -- z_clip_set() copies into
		// its own staging buffer before sending.
		z_clip_set(P(a), n);
		return true;
	}

	// The arena may move here: P(a) is taken after.
	bool whole = clip_hold(n);
	const char *src = P(a);
	int k = page_of(src, n);
	if (whole) {
		memcpy(clip_at(), src, (size_t)n);
		clip_len = n;
		clip_prefix = k;
	} else if (clip_len) {
		clip_drop();
		src = P(a);
	}
	z_clip_set(src, k);
	return whole;

}

// Ctrl+C and the right button: a copy that other apps, and a paste
// here, will get only the first page of says so.
static void copy_cmd(void) {
	if (do_copy()) return;
	z_dialog_confirm(&dlg_ctx, "Copy",
		"Copied, but there is no\nmemory to keep all of it:\na paste gives the first\n4 KB.", Z_DIALOG_OK_CANCEL);
	repaint();
}

static void do_cut(void) {

	if (!has_sel()) return;

	int n = sel_end() - sel_start();
	const char *why = NULL;
	if (!do_copy())
		why = "Too large to cut here:\nthere is no memory to\nkeep it for the paste.\nCut less than 4 KB.";
	else if (n > CLIP_PAGE && !undo_fits(1, n))
		why = "Too large to cut:\nit could not be undone.\nCut a smaller part.";
	if (why) {
		z_dialog_confirm(&dlg_ctx, "Cut", why, Z_DIALOG_OK_CANCEL);
		repaint();
		return;
	}
	undo_begin(UK_OTHER, sel_start());
	delete_selection();
	undo_break();

}

static void do_paste(void) {

	// One page at a time, matching wm's own clipboard capacity --
	// see Z_WM_CLIP_MAX in zwm.h. On the stack rather than in .bss
	// because it is live only for the duration of this call, and 4KB
	// of a 16KB stack is affordable for one call that makes no
	// further nested calls of consequence. A paste of the private copy
	// goes through it a page at a time too: the copy is in the arena,
	// which the insert may move, so it is never inserted from there.
	char clip[Z_WM_CLIP_MAX];

	int got = z_clip_get(clip, sizeof(clip));
	if (got <= 0) return;

	bool priv = clip_len && got == clip_prefix && !memcmp(clip, clip_at(), (size_t)got);
	int avail = priv ? clip_len : got;
	int n = avail;

	// Replacing a selection is one edit, not two: delete then insert
	// would rewrap twice and repaint twice for a single user action.
	int para;

	// One group, delete and insert both: one Ctrl+Z puts the selection
	// back.
	undo_begin(UK_OTHER, has_sel() ? sel_start() : cursor);

	if (has_sel()) {
		int a = sel_start();
		int b = sel_end();
		para = para_line_at(a);
		edit_delete(a, b);
		cursor = a;
		sel_anchor = -1;
	} else {
		para = para_line_at(cursor);
	}

	if (n > gl && !doc_room(n)) {
		n = gl;
		// Cut at a character boundary: half a UTF-8 sequence would be
		// a malformed byte left in the document.
		const char *s = priv ? clip_at() : clip;
		while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
	}

	if (n > 0 && !priv) {
		edit_insert(cursor, clip, n);
		cursor += n;
	} else if (n > 0) {
		// Room in the log for the whole paste first, so its pieces
		// (which join into one record) do not push each other out.
		undo_fits(1, n);
		for (int done = 0; done < n; ) {
			int k = n - done < (int)sizeof(clip) ? n - done : (int)sizeof(clip);
			memcpy(clip, clip_at() + done, (size_t)k);
			edit_insert(cursor, clip, k);
			cursor += k;
			done += k;
		}
	}
	undo_break();

	after_edit(para);

	// Said out loud rather than silently dropping the tail: a paste
	// that lands partially is worth knowing about.
	if (n < avail)
		z_dialog_confirm(&dlg_ctx, "Full",
			"Only part of the clipboard\nwould fit.", Z_DIALOG_OK_CANCEL);

}

// Inserts `n` copies of the `k`-byte sequence `seq` as ONE edit -- one
// rewrap, one repaint, and at most one "document is full" complaint
// no matter how many characters were asked for. Tab is the reason it
// takes a count: four separate insertions would rewrap and repaint
// four times for a single keypress, and could raise the same dialog
// four times in a row at the buffer limit. A character that does not
// fit whole is not inserted at all -- half a UTF-8 sequence would be
// a malformed byte in the file.
static void insert_seq(const char *seq, int k, int n) {

	// Typing with a selection replaces it -- the standard behaviour,
	// and the reason delete_selection() is factored out. That is a
	// group of its own; plain typing continues the last one.
	undo_begin(has_sel() ? UK_OTHER : UK_TYPE, has_sel() ? sel_start() : cursor);
	delete_selection();

	int para = para_line_at(cursor);
	int done = 0;

	for (; done < n; done++) {
		if (k > gl && !doc_room(k)) break;
		edit_insert(cursor, seq, k);
		cursor += k;
	}

	// Enter ends a group, so undo takes back a line at a time.
	if (k == 1 && seq[0] == '\n') undo_break();
	else { undo_kind = UK_TYPE; undo_next = cursor; }

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

static void insert_run(char c, int n) {
	insert_seq(&c, 1, n);
}

static void insert_char(char c) {
	insert_run(c, 1);
}

// One character, as its codepoint -- anything a keyboard layout can
// type (docs/keyboard_layouts.md).
static void insert_cp(uint32_t cp) {
	char u[Z_UTF8_MAX];
	int k = z_utf8_put(cp, u);
	insert_seq(u, k, 1);
}

// Backspace or Delete with a selection: removing it is a group of its own.
static bool delete_selection_as_edit(void) {
	if (!has_sel()) return false;
	undo_begin(UK_OTHER, sel_start());
	delete_selection();
	undo_break();
	return true;
}

static void delete_back(void) {

	if (delete_selection_as_edit()) return;

	if (cursor == 0) return;

	// The whole character before the caret, however many bytes.
	int from = prev_off(cursor);
	int para = para_line_at(from);

	undo_begin(UK_BACK, cursor);
	buf_delete_range(from, cursor);
	cursor = from;
	undo_next = cursor;

	after_edit(para);

}

static void delete_forward(void) {

	if (delete_selection_as_edit()) return;

	if (cursor >= len) return;

	int para = para_line_at(cursor);

	undo_begin(UK_FWD, cursor);
	buf_delete_range(cursor, next_off(cursor));
	undo_next = cursor;

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
		/* Only the view moved -- the document is unchanged. */
		scroll_repaint();
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
	int col = chars_between(LO(l), cursor);

	int target = l + delta;
	if (target < 0) target = 0;
	lines_through(target);
	if (target >= nlines) target = nlines - 1;
	if (target == l) return;

	move_cursor_ex(line_pos(target, col), extend);

}

static void move_line(int delta) {
	move_line_ex(delta, false);
}

// -- input --

// -- echo: saying what the cursor is on, and what was just typed --
//
// Editing without the screen needs three things said: the line you
// arrive on, the character you step over, and the word you finish.
// Anything more (every character as it is typed) is noise at typing
// speed, and anything less leaves you guessing.
//
// All of it interrupts, because an echo that queues is an echo that
// arrives after you have moved on. And all of it costs a handful of
// instructions with speech off (zspeak.h), so there is no setting.

static void echo_line(void) {

	if (!z_speak_available()) return;

	int l = line_at(cursor);
	int n = line_draw_len(l);

	// A blank line still says so: silence there is indistinguishable
	// from the key not having worked.
	if (n <= 0) {
		z_speak_static("blank", Z_TTS_F_INTERRUPT);
		return;
	}

	z_speak_n(P(LO(l)), (uint32_t)n, Z_TTS_F_INTERRUPT);

}

static void echo_char(int at) {

	if (!z_speak_available()) return;

	if (at < 0 || at >= len) {
		z_speak_static("end", Z_TTS_F_INTERRUPT);
		return;
	}

	if (B(at) == '\n') {
		z_speak_static("new line", Z_TTS_F_INTERRUPT);
		return;
	}

	// One character on its own: text2ph.c names it, punctuation
	// included ("semicolon"), which is exactly what a caret stepping
	// over it should say. The whole character, in UTF-8 -- what goes
	// between processes (docs/text_encoding.md).
	char one[Z_UTF8_MAX + 1];
	int k = next_off(at) - at;
	memcpy(one, P(at), (size_t)k);
	one[k] = 0;
	z_speak(one, Z_TTS_F_INTERRUPT);

}

// The word just finished, said when the key that ended it was a space
// or punctuation. Looks back from the caret over the word that was
// being typed.
static void echo_word(void) {

	if (!z_speak_available()) return;

	int end = cursor - 1;			// the separator just inserted
	while (end > 0 && (B(end - 1) == ' ' || B(end - 1) == '\n')) end--;
	int start = end;
	while (start > 0 && B(start - 1) != ' ' && B(start - 1) != '\n') start--;

	// A word has no newline in it, so never the gap either.
	if (end > start)
		z_speak_n(P(start), (uint32_t)(end - start), Z_TTS_F_INTERRUPT);

}

// -- reading aloud (Super+A) -- docs/tts.md, "Reading a window" --
//
// One unit per DISPLAY line: that is what the view scrolls by, so the
// caret can follow the voice a line at a time, and a display line is
// always short enough for one zspeak slot. A line that wrapped rather
// than ended is sent with Z_TTS_F_CONTINUES, so a paragraph is still
// spoken as one paragraph. Blank lines are sent too (as nothing), so
// the pause between paragraphs is heard.

static int read_get(void *user, int n, const char **text, uint32_t *flags) {

	(void)user;

	if (n >= 0) lines_through(n);
	if (n < 0 || n >= nlines) return -1;

	int a = LO(n);
	int e = line_end(n);

	*text = P(a);
	*flags = (n + 1 < nlines && e > a && B(e - 1) != '\n') ?
		Z_TTS_F_CONTINUES : 0;

	return line_draw_len(n);

}

// The caret follows the voice to the start of each line as it is
// spoken. That is what makes "stop, move, read again" work: when the
// user stops listening the caret is on the line they last heard, and
// the next Super+A starts from wherever they have moved it since.
static void read_at(void *user, int n) {
	(void)user;
	if (n >= 0 && n < nlines) move_cursor(LO(n));
}

static z_sayall_t reader = { read_get, read_at, NULL };

static void read_toggle(void) {

	if (z_sayall_active(&reader)) {
		printf("text: Super+A: reading stopped\n");
		z_sayall_stop(&reader);
		return;
	}

	if (len == 0) {
		z_speak_static("Empty document", Z_TTS_F_INTERRUPT);
		return;
	}

	// From the caret's line -- except with the caret at the very end,
	// where there is nothing left to read. That is also exactly where
	// it is after typing something, and "read me what I wrote" is the
	// commonest reason to ask, so read the whole document instead.
	int from = (cursor >= len) ? 0 : line_at(cursor);

	printf("text: Super+A: reading from line %d of %d\n", from + 1, nlines);
	if (!z_sayall_start(&reader, from)) {
		printf("text: Super+A: could not start (is speech on?)\n");
		z_speak_static("End of document", Z_TTS_F_INTERRUPT);
	}

}

// Super+C, "copy to audio": the highlighted text, said once, without
// touching the clipboard.
static void read_selection(void) {
	z_sayall_stop(&reader);
	if (!has_sel()) {
		z_speak_static("Nothing selected", Z_TTS_F_INTERRUPT);
		return;
	}
	int a = sel_start(), b = sel_end();
	printf("text: Super+C: reading the selection, %d characters\n", b - a);
	if (a < gs && b > gs) doc_flat();
	z_speak_n(P(a), (uint32_t)(b - a), Z_TTS_F_INTERRUPT);
}

// -- undo and redo -- docs/text_editor.md, "Undo" --

// The log's edits, put back through the gap like any other edit
// (zundo.h, z_undo_ops_t): each costs its own paragraph, so undoing a
// word in a megabyte is as quick as typing it was, and undoing a
// Replace All walks the document once.
static bool undo_ins(void *user, const z_undo_t *u, int pos, uint32_t log, int n) {
	(void)user;
	if (n > gl && !doc_room(n)) return false;
	z_undo_bytes_get(u, log, raw_insert(pos, n), n);
	return true;
}

static void undo_del(void *user, int pos, int n) {
	(void)user;
	raw_delete(pos, pos + n, false);
}

static const z_undo_ops_t undo_ops = { undo_ins, undo_del, NULL };

static void do_undo(bool redo) {

	int from = 0;

	bool ok = redo
		? z_undo_redo_ops(&undo, &undo_ops, &cursor, &from)
		: z_undo_undo_ops(&undo, &undo_ops, &cursor, &from);

	// Whatever is typed next starts a group of its own.
	undo_break();
	if (!ok) return;

	if (cursor > len) cursor = len;
	if (cursor < 0) cursor = 0;
	layout_sync();
	after_edit(para_line_at(from));

	// Back at the saved text -- by undo or by redo -- is unmodified.
	set_modified(!z_undo_at_saved(&undo));

}

// -- find -- docs/text_editor.md, "Find" --
//
// Ctrl+F asks for the text and selects the next match after the caret,
// wrapping round to the top; Ctrl+G or F3 finds the same text again.
// ASCII letters match either case -- as the posix shell's wildcards do
// -- and everything else byte for byte, which for UTF-8 is character
// for character, so "Grüße" and Japanese are found as typed.

static char find_text[128];

static char fold_ascii(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// find_text at physical doc[i], for passes that work on the storage
// directly (replace_all_core()).
static bool match_raw(int i, int n) {
	for (int k = 0; k < n; k++)
		if (fold_ascii(doc[i + k]) != fold_ascii(find_text[k])) return false;
	return true;
}

static bool match_at(int i, int n) {
	for (int k = 0; k < n; k++)
		if (fold_ascii(B(i + k)) != fold_ascii(find_text[k])) return false;
	return true;
}

// Selects the first match at or after `start`, wrapping round.
// The next match from `start`, round to the start again: 1 found (and
// selected), 0 not found, -1 stopped. A megabyte is about half a second
// on the board, so the window's mail is read every 8 KB meanwhile
// (io_pump()), the title says how far it has got, and Esc stops it.
#define FIND_SLICE	8192

static int find_from(int start) {

	int n = (int)strlen(find_text);
	if (!n || n > len) return 0;
	if (start < 0 || start > len) start = 0;

	io_begin("Finding", find_text, false);
	io_name = find_text;				// the text, not a path's last part
	long done = 0;
	for (int pass = 0; pass < 2; pass++) {
		int lo = pass ? 0 : start;
		int hi = pass ? start : len - n;
		for (int i = lo; i <= hi && i <= len - n; i++) {
			if (++done % FIND_SLICE == 0 && io_pump(done, len, true)) {
				io_end();
				repaint();
				return -1;
			}
			if (!match_at(i, n)) continue;
			io_end();
			sel_anchor = i;
			cursor = i + n;
			undo_break();
			scroll_to_cursor();
			repaint();
			return 1;
		}
	}
	io_end();
	return 0;

}

static void find_report(void) {
	char msg[160];
	snprintf(msg, sizeof(msg), "\"%.60s\"\nwas not found.", find_text);
	z_dialog_confirm(&dlg_ctx, "Find", msg, Z_DIALOG_OK_CANCEL);
	repaint();
}

static void do_find(void) {

	char q[sizeof(find_text)];
	if (!z_dialog_prompt(&dlg_ctx, "Find", "Find:", find_text, q, sizeof(q)) || !q[0]) {
		repaint();
		return;
	}
	snprintf(find_text, sizeof(find_text), "%s", q);

	// From the start of any selection, so a word already selected is
	// found where it is; from the caret otherwise.
	int start = has_sel() ? sel_start() : cursor;
	if (find_from(start) == 0) find_report();

}

static void find_next(void) {
	if (!find_text[0]) { do_find(); return; }
	// After the current match (or the caret), so it moves on.
	if (find_from(has_sel() ? sel_end() : cursor) == 0) find_report();
}

// -- replace -- docs/text_editor.md, "Replace" --
//
// Ctrl+H asks what to find and what to put in its place (which may be
// nothing), counts the matches, and offers to replace them all at once
// or one at a time. Matching is find's: ASCII letters either case.

static char repl_text[128];

// Matches of find_text, left to right and not overlapping -- the same
// set Replace All replaces.
// How many matches, the same way: -1 if Esc stopped it (`stoppable`).
static int count_matches(bool stoppable) {
	int nf = (int)strlen(find_text), n = 0;
	if (!nf) return 0;
	io_begin("Counting", find_text, false);
	io_name = find_text;
	long step = 0;
	for (int i = 0; i + nf <= len; ) {
		if (++step % FIND_SLICE == 0 && io_pump(i, len, stoppable)) { io_end(); return -1; }
		if (match_at(i, nf)) { n++; i += nf; }
		else i++;
	}
	io_end();
	return n;
}

// Can Replace All of `n` matches be recorded for undo? Each match is a
// delete and an insert, and the log forgets its oldest edits when it
// fills -- which here would forget the start of the replace itself and
// leave an undo that puts back only some of it. Where there is an
// arena, the log is grown to make it so (undo_fits()).
static bool replace_all_undoable(int n) {
	int nf = (int)strlen(find_text), nr = (int)strlen(repl_text);
	return undo_fits(n * (nr ? 2 : 1), (long)n * (nf + nr));
}

// Replaces every match in one pass over the buffer, however many there
// are: the alternative, an edit per match, moves the rest of the
// document once per match. When the replacement is longer the document
// is first moved to the top of the buffer, so the output -- which grows
// -- never overtakes the input it is read from.
//
// Returns the number replaced, or -1, changing nothing, if the result
// would not fit TEXT_MAX. Each replacement is recorded for undo as one
// group when `record`; otherwise the history is cleared, since it no
// longer leads back to anything.
static int replace_all_core(bool record) {

	int nf = (int)strlen(find_text), nr = (int)strlen(repl_text);
	int n = count_matches(false);		// asked already: not stopped now
	if (n <= 0) return 0;
	if (nr > nf) {
		long grow = (long)n * (nr - nf);
		if ((long)len + grow > 0x7FFFFFF || (grow > gl && !doc_room((int)grow))) return -1;
	}

	layout_sync();
	int first = -1;
	for (int i = 0; i + nf <= len; i++) {
		if ((i & 0x3FFF) == 0x3FFF) io_pump(0, 1, false);
		if (match_at(i, nf)) { first = i; break; }
	}
	int para = para_line_at(first);

	// One run, with all the free space after it: the pass below reads
	// and writes doc[] directly. The line table as offsets throughout:
	// everything from the first match on is wrapped again below, and
	// what is before it stays as it is.
	doc_flat();
	lgap_to(nlines);
	int shift = (nr > nf) ? cap - len : 0;
	if (shift) memmove(&doc[shift], doc, (size_t)len);

	int c0 = cursor, newcur = -1;
	int src = shift, end = shift + len, dst = 0, done = 0;

	if (record) undo_begin(UK_OTHER, first);

	// A piece at a time: the window's mail between pieces (the page
	// blank meanwhile -- the text is half rewritten), and Esc stops it:
	// the rest is then copied as it is, so what is done is the matches
	// before that point, one undo group still.
	io_begin("Replacing", find_text, true);
	io_name = find_text;
	long piece = 0;
	while (src < end) {
		if (++piece % FIND_SLICE == 0 && io_pump(src - shift, end - shift, true)) {
			if (newcur < 0) newcur = dst + (c0 - (src - shift) > 0 ? c0 - (src - shift) : 0);
			memmove(&doc[dst], &doc[src], (size_t)(end - src));
			dst += end - src;
			src = end;
			break;
		}
		if (newcur < 0 && src - shift >= c0) newcur = dst;
		if (src + nf <= end && match_raw(src, nf)) {
			if (record) {
				z_undo_record(&undo, Z_UNDO_DEL, dst, &doc[src], nf, c0, undo_group);
				z_undo_record(&undo, Z_UNDO_INS, dst, repl_text, nr, c0, undo_group);
			}
			memmove(&doc[dst], repl_text, (size_t)nr);
			// A caret inside the match goes after its replacement.
			if (newcur < 0 && src - shift + nf > c0) newcur = dst + nr;
			dst += nr;
			src += nf;
			done++;
		} else
			doc[dst++] = doc[src++];
	}
	io_end();

	len = dst;
	gs = len;
	gl = cap - len;
	cursor = newcur < 0 ? len : newcur;
	edit_gen++;
	edit_tick = z_uptime_ticks();
	wrap_from(para);
	if (record) undo_break();
	else {
		z_undo_reset(&undo);
		undo.saved = UINT32_MAX;		// nothing leads back to the file now
		undo_break();
	}
	after_edit(para);
	return done;

}

// The selection is a match: replace it, as one edit of its own.
static bool replace_selection(void) {
	int nr = (int)strlen(repl_text);
	int a = sel_start(), b = sel_end();
	if (nr - (b - a) > gl && !doc_room(nr - (b - a))) return false;
	int para = para_line_at(a);
	undo_begin(UK_OTHER, a);
	edit_delete(a, b);
	if (nr) edit_insert(a, repl_text, nr);
	cursor = a + nr;
	sel_anchor = -1;
	undo_break();
	after_edit(para);
	return true;
}

static void say_full(void) {
	z_dialog_confirm(&dlg_ctx, "Replace",
		"The document would not hold\nthe replacements.", Z_DIALOG_OK_CANCEL);
}

static void do_replace(void) {

	char q[sizeof(find_text)], r[sizeof(repl_text)];

	// The selection, when it is a short piece of one line, is what most
	// people mean to replace.
	const char *initial = find_text;
	char selq[sizeof(find_text)];
	if (has_sel() && sel_end() - sel_start() < (int)sizeof(selq)) {
		int n = 0;
		for (int i = sel_start(); i < sel_end() && B(i) != '\n'; i++) selq[n++] = B(i);
		selq[n] = 0;
		if (n == sel_end() - sel_start()) initial = selq;
	}

	if (!z_dialog_prompt(&dlg_ctx, "Replace", "Find:", initial, q, sizeof(q)) || !q[0]) {
		repaint();
		return;
	}
	if (!z_dialog_prompt_empty_ok(&dlg_ctx, "Replace", "Replace with:", repl_text, r, sizeof(r))) {
		repaint();
		return;
	}
	snprintf(find_text, sizeof(find_text), "%s", q);
	snprintf(repl_text, sizeof(repl_text), "%s", r);

	int n = count_matches(true);
	if (n < 0) { repaint(); return; }		// Esc
	if (!n) { find_report(); return; }

	char msg[160];
	snprintf(msg, sizeof(msg), "%d match%s for \"%.40s\".\nReplace all of them?\n(No: one at a time.)",
		n, n == 1 ? "" : "es", find_text);
	int how = z_dialog_confirm(&dlg_ctx, "Replace", msg, Z_DIALOG_YES_NO_CANCEL);

	if (how == Z_DIALOG_CANCEL) { repaint(); return; }

	if (how == Z_DIALOG_YES) {
		bool record = replace_all_undoable(n);
		if (!record && z_dialog_confirm(&dlg_ctx, "Replace",
				"That many changes cannot\nbe undone. Replace all\nanyway?",
				Z_DIALOG_YES_NO) != Z_DIALOG_YES) {
			repaint();
			return;
		}
		int done = replace_all_core(record);
		if (done < 0) { say_full(); repaint(); return; }
		snprintf(msg, sizeof(msg), "Replaced %d.", done);
		z_dialog_confirm(&dlg_ctx, "Replace", msg, Z_DIALOG_OK_CANCEL);
		repaint();
		return;
	}

	// One at a time: each match selected and shown, then asked about.
	// At most as many steps as there were matches, so saying No to
	// every one ends rather than going round for ever.
	int start = has_sel() ? sel_start() : cursor;
	for (int step = 0; step < n; step++) {
		if (find_from(start) <= 0) break;
		int a = z_dialog_confirm(&dlg_ctx, "Replace", "Replace this one?",
			Z_DIALOG_YES_NO_CANCEL);
		if (a == Z_DIALOG_CANCEL) break;
		if (a == Z_DIALOG_YES) {
			if (!replace_selection()) { say_full(); break; }
			start = cursor;
		} else
			start = sel_end();
	}
	repaint();

}

// -- go to line -- docs/text_editor.md, "Go to line" --
//
// Lines here are the file's lines -- what a compiler or another editor
// numbers -- not the wrapped display lines.

// (the tests' -- the dialog counts in one pass: doc_census())
static __attribute__((unused)) int doc_line_of(int off) {
	int n = 1;
	for (int i = 0; i < off && i < len; i++) if (B(i) == '\n') n++;
	return n;
}

static int doc_line_start(int line) {
	int n = 1, i = 0;
	for (; i < len && n < line; i++) if (B(i) == '\n') n++;
	return i;
}

// Words (runs of anything but spaces, tabs and newlines) and characters.
static __attribute__((unused)) void doc_counts(int *words, int *chars) {
	int w = 0;
	bool in = false;
	for (int i = 0; i < len; i++) {
		char c = B(i);
		bool sp = c == ' ' || c == '\n' || c == '\t';
		if (!sp && !in) w++;
		in = !sp;
	}
	*words = w;
	// The two sides of the gap: no character straddles it.
	*chars = (int)z_utf8_count(doc, (size_t)gs) +
		(int)z_utf8_count(&doc[gs + gl], (size_t)(len - gs));
}

// Past the last line is the last line: doc_line_start() stops at the
// end of the text.
static void goto_line(int line) {
	if (line < 1) line = 1;
	undo_break();
	move_cursor(doc_line_start(line));
	echo_line();
}

// The line the caret is on, how many there are, the words and the
// characters: one pass, in pieces (the window's mail between them; the
// page stays drawn), Esc stops it. False if stopped.
static bool doc_census(int *here, int *total, int *words, int *chars) {
	int n = 1, w = 0, ch = 0;
	bool in = false;
	*here = 1;
	io_begin("Counting", "the document", false);
	for (int i = 0; i < len; i++) {
		if ((i & 0x7FFF) == 0x7FFF && io_pump(i, len, true)) { io_end(); return false; }
		if (i == cursor) *here = n;
		char c = B(i);
		if (c == '\n') n++;
		bool sp = c == ' ' || c == '\n' || c == '\t';
		if (!sp && !in) w++;
		in = !sp;
		if (((uint8_t)c & 0xC0) != 0x80) ch++;
	}
	if (cursor >= len) *here = n;
	io_end();
	*total = n;
	*words = w;
	*chars = ch;
	return true;
}

static void do_goto(void) {

	int words, chars, here, total;
	if (!doc_census(&here, &total, &words, &chars)) { repaint(); return; }

	char msg[160], cur[16], ans[16];
	snprintf(msg, sizeof(msg), "Line %d of %d.\n%d word%s, %d character%s.\nGo to line:",
		here, total, words, words == 1 ? "" : "s", chars, chars == 1 ? "" : "s");
	snprintf(cur, sizeof(cur), "%d", here);

	if (!z_dialog_prompt(&dlg_ctx, "Go to line", msg, cur, ans, sizeof(ans))) {
		repaint();
		return;
	}
	int line = 0;
	for (const char *p = ans; *p; p++) {
		if (*p < '0' || *p > '9') { repaint(); return; }
		if (line < 100000000) line = line * 10 + (*p - '0');
	}
	repaint();
	goto_line(line);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	mods_now = mods;

	if (keysym == Z_KEY_HELP) {	// docs/app_help.md
		static const char help[] =
			"A plain text editor for notes, lists and Markdown.\n\n"
			"Ctrl+N/O/S\tNew, open, save\n"
			"Ctrl+Shift+S\tSave as\n"
			"Ctrl+Q\tClose\n"
			"Ctrl+C/X/V\tCopy, cut, paste\n"
			"Ctrl+A\tSelect all\n"
			"Ctrl+Z/Y\tUndo, redo\n"
			"Ctrl+F, F3\tFind, find again\n"
			"Ctrl+H\tReplace\n"
			"Ctrl+L\tGo to line; counts words\n"
			"Ctrl+P\tPrint, or print to a PDF\n\n"
			"Shift with the arrows, or a drag, selects. "
			"A .docx, .odt or .rtf opens converted, as Markdown in " Z_DIR_TMP
			" (the RAM disk): Save puts it in " Z_DIR_HOME ".";
		z_dialog_help(&dlg_ctx, "Text help", help, "text_editor");
		return;
	}
	if (keysym == Z_KEY_FIND) { find_next(); return; }	// F3: find, then again

	if (mods & Z_KBD_MOD_CTRL) {

		// Ctrl+letter arrives as 0x01..0x1A (zkbd.h), which overlaps
		// the control characters Enter and Tab also produce -- hence
		// checking the modifier first rather than the keysym alone.
		switch (keysym) {
			case 0x0e: do_new(); return;		// Ctrl+N
			case 0x0f: do_open(); return;		// Ctrl+O
			case 0x10: do_print(); return;		// Ctrl+P
			case 0x13:							// Ctrl+S; Ctrl+Shift+S is Save As
				if (mods & Z_KBD_MOD_SHIFT) do_save_as();
				else do_save();
				return;
			case 0x08: do_replace(); return;	// Ctrl+H
			case 0x0c: do_goto(); return;		// Ctrl+L
			case 0x11: do_close(); return;		// Ctrl+Q
			case 0x03: copy_cmd(); return;		// Ctrl+C
			case 0x18: do_cut(); return;		// Ctrl+X
			case 0x16: do_paste(); return;		// Ctrl+V
			case 0x1a:							// Ctrl+Z; Ctrl+Shift+Z is redo
				do_undo((mods & Z_KBD_MOD_SHIFT) != 0);
				return;
			case 0x19: do_undo(true); return;	// Ctrl+Y -- redo
			case 0x06: do_find(); return;		// Ctrl+F
			case 0x07: find_next(); return;		// Ctrl+G -- find again
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

		// Stepping over a character says that character; moving
		// between lines says the whole line.
		case Z_KEY_LEFT:	move_cursor_ex(prev_off(cursor), ext); echo_char(cursor); return;
		case Z_KEY_RIGHT:	move_cursor_ex(next_off(cursor), ext); echo_char(prev_off(cursor)); return;
		case Z_KEY_UP:		move_line_ex(-1, ext); echo_line(); return;
		case Z_KEY_DOWN:	move_line_ex(1, ext); echo_line(); return;

		case Z_KEY_PAGEUP:	move_line_ex(-(rows - 1), ext); echo_line(); return;
		case Z_KEY_PAGEDOWN:	move_line_ex(rows - 1, ext); echo_line(); return;

		case Z_KEY_HOME:
			move_cursor_ex(LO(line_at(cursor)), ext);
			echo_line();
			return;

		case Z_KEY_END: {
			int l = line_at(cursor);
			move_cursor_ex(line_pos(l, line_col_max(l)), ext);
			echo_line();
			return;
		}

		case Z_KEY_DELETE:	echo_char(cursor); delete_forward(); return;

		case 0x7f:			// Backspace -- DEL, see zkbd.c
			// Said BEFORE the delete: afterwards it is gone, and
			// "what did I just remove" is the question.
			echo_char(prev_off(cursor));
			delete_back();
			return;

		case 0x0d:			// Enter -- CR from the keymap
			// Stored as '\n'. One line ending in the buffer, decided
			// here, so nothing downstream has to cope with two.
			insert_char('\n');
			echo_word();
			return;

		case '\t':
			// Spaces, not a tab. wrap_one() counts columns and a real
			// tab has no fixed width in that count -- see draw_row().
			// One edit, not four -- see insert_run().
			insert_run(' ', 4);
			return;

		default:

			// Any character a keyboard layout can type, not just ASCII
			// (zkbd.h's Z_KEY_IS_TEXT: no controls, no named keys).
			if (Z_KEY_IS_TEXT(keysym)) {
				if (keysym < 0x80) insert_char((char)keysym);
				else insert_cp(keysym);
				// A word is finished by the thing that ends it.
				if (keysym == ' ' || keysym == '.' || keysym == ',' ||
				    keysym == '?' || keysym == '!' || keysym == ';' ||
				    keysym == ':')
					echo_word();
			}
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
			lines_through(top_line);
			/* Only the view moved -- the document is unchanged, so
			 * the rows that stay on screen are already correct. */
			scroll_repaint();
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
		copy_cmd();

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

	int pos = line_pos(l, col);

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

		// The part of this window not covered by the windows in
		// front of it. Confines every subsequent draw to it -- see
		// z_win_apply_clip() in zwin.c. The ack it sends is not
		// optional: wm waits for it when a region narrows.
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

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

		// A read's progress -- see read_get(). Routed here rather than
		// only in main()'s loop so a reply arriving while a dialog is
		// up is not lost.
		case Z_TTS_MARK_DONE:
		case Z_TTS_MARK_CANCELLED:
			z_sayall_msg(&reader, msg);
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
	memset(sbuf, 0, sizeof(sbuf));
	memset(sline, 0, sizeof(sline));

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
	// Take the launch argument BEFORE creating the window, so the
	// window can be created with its final title.
	//
	// It used to be created as "untitled" and retitled immediately
	// after the file loaded. That retitle is fire-and-forget (see
	// update_title()), so wm was repairing this window's titlebar --
	// and telling every window underneath to repaint, unclipped --
	// at the same moment this app was painting its content for the
	// first time. Two processes drawing into the same pixels, and
	// whichever finished last won. When the file browser lost, its
	// chrome stayed visible inside this window until something
	// forced a clean redraw.
	//
	// z_launch_arg_take() needs no window, so moving it above the
	// create costs nothing and removes the message entirely: the
	// title is already right, so update_title() below finds nothing
	// to send.
	char launch_arg[sizeof(filename)];
	bool have_arg = z_launch_path_take(launch_arg, sizeof(launch_arg));

	char initial_title[32];
	build_title(initial_title, (int)sizeof(initial_title),
		have_arg ? launch_arg : "", false);

	// Seed the suppression cache with what we are about to create the
	// window with, so the post-load update_title() stays silent.
	for (int i = 0; i < (int)sizeof(sent_title); i++)
		sent_title[i] = initial_title[i];

	z_win_min_size(WIN_MIN_W, WIN_MIN_H);
	if (z_win_create_flags(&win, initial_title, WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		Z_WIN_FLAG_MIN_IS_CREATE |
		Z_WIN_FLAG_NEW_ICON | Z_WIN_FLAG_OPEN_ICON |
		Z_WIN_FLAG_SAVE_ICON | Z_WIN_FLAG_PRINT_ICON | Z_WIN_FLAG_FONT_ICON |
		Z_WIN_FLAG_READABLE) != Z_OK) {
		printf("text: failed to create window -- is wm running?\n");
		return 1;
	}

	z_scrollbar_init(&sbar, &win, Z_SB_VERT);

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;
	dlg_ctx.arena_moved = arena_dialog_moved;

	layout();

	// The file the browser launched us with, taken above. load_file()
	// reports its own failures and leaves an empty buffer, so there
	// is nothing to check here -- but it needs the window to exist,
	// because it reports them in a dialog.
	// A copy left by a session that did not end: of the file opened,
	// or, when none was, of a document that was never saved.
	if (!have_arg || load_file(launch_arg)) autosave_offer();

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

					// Any key ends a read, before it does anything
					// else -- the caret is then on the line last
					// heard, and the key acts from there.
					z_sayall_stop(&reader);

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_WHEEL:

					if (msg.obj.type == Z_UINT32)
						wheel_scroll(Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32));
					break;

				case Z_WM_MOUSE:

					if (msg.obj.type != Z_UINT32) break;

					// A click moves the caret, which a running read
					// would immediately move back.
					if (Z_WM_UNPACK_MOUSE_BUTTONS(msg.obj.val.uint32) & 1)
						z_sayall_stop(&reader);

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
						case Z_WM_TBICON_PRINT: do_print(); break;
						default: break;
					}

					break;

				}

				// Super+A -- see read_toggle().
				case Z_WM_READ:

					if (msg.obj.type == Z_UINT32 &&
						Z_WM_READ_WIN(msg.obj.val.uint32) == win.id) {
						if (Z_WM_READ_WHAT(msg.obj.val.uint32) == Z_WM_READ_SELECTION)
							read_selection();
						else
							read_toggle();
					}

					break;

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

		// A read whose replies have stopped coming gives up.
		z_sayall_poll(&reader);

	
#if TEXT_INSTRUMENT
		ins_report();
#endif

		// A piece of the autosave copy, while one is being written.
		if (autosave_step()) continue;

		// Wrapping in the background: a slice, then the queue again,
		// with no wait while there is more -- the scrollbar grows as
		// it goes (docs/text_editor.md, "Wrapping").
		if (wrap_bg) {
			wrap_more(-1, WRAP_SLICE);
			z_scrollbar_set_range(&sbar, nlines, rows);
			z_scrollbar_draw(&sbar, false);
			continue;
		}

		/* Block until something arrives.
		 *
		 * This loop spun, like term's and wm's before it. An editor
		 * sitting idle with a file open was taking a full scheduler
		 * share from whatever was in the foreground -- see
		 * docs/app_runtime.md on why that costs far more than the
		 * work it does.
		 *
		 * Everything here is driven by wm messages, so a plain
		 * z_proc_wait(0) would be correct. The timeout is insurance
		 * for the periodic work an editor tends to acquire (autosave,
		 * a blinking caret) rather than for anything present today. */
		z_proc_wait(Z_TICK_HZ / 30);
	}

	return 0;

}
