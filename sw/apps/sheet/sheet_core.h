#ifndef SHEET_CORE_H
#define SHEET_CORE_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The spreadsheet itself -- cells, formulas, evaluation, and the file
 * format. No drawing, no windows, no messages, nothing that needs
 * hardware.
 *
 * Separate from sheet.c for the same reason calc_core.c is separate
 * from calc.c, and it matters more here: a spreadsheet that is subtly
 * wrong is worse than no spreadsheet, because the whole point of one
 * is that nobody re-checks its arithmetic. Everything here is
 * deterministic and self-contained, so it can be exercised properly on
 * the build machine -- see tests/test_core.c.
 *
 * -- storage --
 *
 * SPARSE. A 26x999 dense grid would be 26,000 cells and there is not
 * the memory for it; a sheet with nine numbers in it should cost what
 * nine numbers cost. So cells live in one flat array sorted by
 * `key = row * SHEET_COLS + col`, found by binary search and inserted
 * by memmove -- the same deliberate tradeoff sw/apps/text makes for
 * its document buffer, and for the same reason: the worst case is a
 * few milliseconds on a rare operation, against a second
 * representation of "where the data is" that every function here
 * would have to understand.
 *
 * Cell SOURCE text (what the user typed) lives in one append-only
 * arena, referenced by offset. Overwriting or clearing a cell orphans
 * its old text rather than moving anything; the arena is COMPACTED
 * when it fills. See sheet_compact() in the .c for why that is a
 * separate pass rather than a free list.
 *
 * -- what a cell holds --
 *
 * Exactly what was typed, and nothing else. Whether "12.5" is a number
 * and "=SUM(A1:A9)" is a formula is decided by looking at the text,
 * every time it is set, by the same rule the file format uses:
 *
 *   leading '='   formula
 *   leading '\''  text, forced -- the apostrophe is not part of the
 *                 text, and is how you store "007" or "1-2" as a
 *                 label rather than having it read as a number
 *   parses whole  number
 *   otherwise     text
 *
 * That one rule being shared between the editor, the loader and the
 * saver is what makes the file format identical to what you type.
 *
 * -- evaluation --
 *
 * On demand, memoised, with cycle detection by a per-cell state flag
 * rather than a dependency graph. A formula that refers to a cell
 * currently being evaluated is a cycle by definition, so the check is
 * one comparison at the point it matters and there is no graph to
 * build, invalidate or get wrong. Any edit resets every cached value
 * (a loop over the cell array, not a memset of the grid).
 *
 * Recursion is bounded by SHEET_MAX_DEPTH -- an app gets its heap and
 * stack out of one 16KB allocation (Z_PROC_STACK_SIZE_DEFAULT,
 * sw/os/kernel.h), so an unbounded evaluator is a silent stack
 * overflow rather than an error message.
 *
 * -- files --
 *
 * Reading and writing are both STREAMED, one line at a time, and that
 * is not a stylistic choice: the whole-file helpers in zfsapp.h
 * (fs_mallocfile()) allocate the file's size on the heap, and the heap
 * here shares 16KB with the stack. A sheet can easily be bigger than
 * that. So the writer takes an emit callback and the reader takes one
 * line at a time; neither ever holds more than a line.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zfix.h"

// -- dimensions --

// Columns A..Z. Two-letter columns would need nothing here but a
// different sheet_parse_ref()/sheet_ref_name() and a wider key; they
// are left out because 26 columns is already more than fits on a
// 640px screen and the reference syntax stays trivial.
#define SHEET_COLS        26

// Rows 1..999 as the user sees them; 0..998 internally.
//
// Chosen so that `key` fits a uint16_t (998 * 26 + 25 = 25973) and so
// that a row number is at most three characters, which is what the row
// header is sized for.
#define SHEET_ROWS        999

// 8 decimal places. It was 4 until 0.0.7, to buy integer range -- but
// a monthly interest rate is 0.05/12 = 0.0041666..., which at four
// places is 0.0042 (a 30-year mortgage payment five dollars out) and
// at six still a few cents. At eight it is right to the cent.
//
// The range left is +-92 thousand million, ample for the books this
// is for; a result past it is #NUM, never a wrong number. The range
// the four places bought was only ever needed because z_fix_mul()
// checks its double-scaled intermediate rather than its result:
// sheet_core.c's fx_mul() splits at the decimal point instead, so any
// product that fits is computed.
#define SHEET_DP          8

// Non-empty cells, and bytes of source text across all of them.
//
// 4096 x 16 bytes + 40KB is 104KB of .bss: a ledger of 800 rows by
// five columns, which is the size of thing a sheet like this is for.
// (0.0.6 had 1024 and 16KB -- 39 full rows -- which was a demo, not a
// tool.) Past either, setting a cell fails cleanly (SHEET_ERR_FULL)
// rather than losing data quietly. The arena's offsets are 16 bits, so
// it can grow to 64KB without a format change.
#ifndef SHEET_MAX_CELLS
#define SHEET_MAX_CELLS   4096
#endif
#ifndef SHEET_ARENA
#define SHEET_ARENA       40960
#endif

// Number-format ranges (sheet_set_format()): later ones win.
#define SHEET_MAX_FMTS    64

// Longest source text for one cell, including the NUL. Also the size
// of the app's edit buffer -- they have to agree, or a cell could hold
// something the editor cannot open.
#define SHEET_SRC_MAX     96

// Nested formula references. A chain longer than this is far more
// likely to be a mistake than a model, and the alternative to a cap is
// a stack overflow with no message.
#define SHEET_MAX_DEPTH   24

// Column widths, in CHARACTERS of the current font.
#define SHEET_COLW_MIN    2
#define SHEET_COLW_MAX    40
#define SHEET_COLW_DEF    9

// -- errors --
//
// A cell's error is a VALUE, not an exception: it is cached like any
// other result and propagates through anything that refers to it, so
// one bad cell shows up everywhere it actually matters rather than
// zeroing itself out silently.
typedef enum {
	SHEET_OK = 0,
	SHEET_ERR_SYNTAX,		// "#ERR"   -- unparseable formula
	SHEET_ERR_DIV0,			// "#DIV0"  -- division by zero
	SHEET_ERR_CIRC,			// "#CIRC"  -- cell refers back to itself
	SHEET_ERR_NAME,			// "#NAME"  -- unknown function
	SHEET_ERR_REF,			// "#REF"   -- reference outside the grid
	SHEET_ERR_NUM,			// "#NUM"   -- overflow, or a bad argument
	SHEET_ERR_DEPTH,		// "#DEEP"  -- SHEET_MAX_DEPTH exceeded
	SHEET_ERR_FULL,			// "#FULL"  -- out of cells or arena
} sheet_err_t;

// Short display text for an error, "" for SHEET_OK. Deliberately at
// most five characters: these have to fit a column.
const char *sheet_err_text(sheet_err_t e);

// -- cells --

typedef enum {
	SHEET_KIND_TEXT = 0,
	SHEET_KIND_NUM,
	SHEET_KIND_FORMULA,
} sheet_kind_t;

typedef struct {

	uint16_t	key;		// row * SHEET_COLS + col; the sort order
	uint16_t	off;		// source text, offset into arena
	uint8_t		len;		// source length, excluding the NUL
	uint8_t		kind;		// sheet_kind_t
	uint8_t		err;		// cached sheet_err_t
	uint8_t		state;		// evaluation state, private to sheet_core.c

	z_fix_t		val;		// cached value

} sheet_cell_t;

// -- number formats --
//
// Applied to rectangles, the way 1-2-3's /Range Format is: a list of
// (range, format), the last that covers a cell winning. A list rather
// than a byte per cell because the cell struct has no room and most
// sheets format a few columns, not thousands of cells one by one.
typedef enum {
	SHEET_FMT_GENERAL = 0,	// as short as it can be (the default)
	SHEET_FMT_FIXED,		// n decimals:            1234.50
	SHEET_FMT_COMMA,		// thousands, n decimals: 1,234.50
	SHEET_FMT_PERCENT,		// x100, n decimals, %:   12.5%
	SHEET_FMT_MONEY,		// $, thousands, n dec:   $1,234.50
} sheet_fmt_kind_t;

typedef struct {
	uint16_t	r0, r1;		// rows, inclusive
	uint8_t		c0, c1;		// columns, inclusive
	uint8_t		kind;		// sheet_fmt_kind_t
	uint8_t		dec;		// decimals, 0..SHEET_DP
} sheet_fmt_t;

// -- the chart --
//
// One per sheet, saved with it ("!g bar A1:D9"): a range and a kind.
// sw/apps/sheet/chart.c draws it.
typedef enum {
	SHEET_CHART_BAR = 0,	// bars side by side, a pattern per series
	SHEET_CHART_STACK,		// bars stacked
	SHEET_CHART_LINE,		// lines, a marker per series
	SHEET_CHART_PIE,		// the first series, as a pie
	SHEET_CHART_KINDS
} sheet_chart_kind_t;

typedef struct {
	bool		set;
	uint8_t		kind;
	uint16_t	r0, r1;
	uint8_t		c0, c1;
} sheet_chart_t;

const char *sheet_chart_name(int kind);		// "bar", "stack", "line", "pie"

typedef struct {

	sheet_cell_t	cells[SHEET_MAX_CELLS];
	int				ncells;

	char			arena[SHEET_ARENA];
	int				arena_used;		// bytes handed out, live or orphaned
	int				arena_live;		// bytes actually referenced

	uint8_t			colw[SHEET_COLS];

	// Set by anything that changes the document, cleared by the app
	// when it saves. Lives here rather than in sheet.c so the loader
	// and the setters can maintain it in one place.
	bool			modified;

	// Evaluation recursion depth, so the cap is enforced in one place.
	int				depth;

	// Row cursor for the streaming CSV reader, which has nowhere else
	// to keep it: a CSV line says nothing about which row it is, so
	// the count has to live between calls.
	int				load_row;

	// Which recalculation a cached value belongs to. Bumped by every
	// change instead of walking every cell (sheet_core.c, invalidate()).
	uint8_t			gen;

	sheet_fmt_t		fmts[SHEET_MAX_FMTS];
	int				nfmts;

	sheet_chart_t	chart;

} sheet_t;

// Empties the sheet: no cells, default column widths, not modified.
// Call before anything else; nothing here relies on .bss zero-init,
// which has been shown unreliable on this hardware (see
// docs/app_runtime.md).
void sheet_init(sheet_t *s);

// -- reading --

// The source text of a cell, exactly as typed -- "" for an empty cell.
// Always NUL-terminated. Points into the arena and stays valid until
// the next sheet_set()/sheet_clear()/load, so a caller holding it
// across an edit must copy it first.
const char *sheet_src(const sheet_t *s, int row, int col);

bool sheet_empty(const sheet_t *s, int row, int col);

sheet_kind_t sheet_kind(const sheet_t *s, int row, int col);

// The numeric value of a cell. Empty and text cells are 0 with
// SHEET_OK -- which is what a spreadsheet means by them, and is why
// COUNT exists separately from a non-empty test.
//
// NOT const: evaluation caches its results.
sheet_err_t sheet_value(sheet_t *s, int row, int col, z_fix_t *out);

// What to draw in a cell `width` characters wide, NUL-terminated.
//
// Numbers are fitted to the column: full precision if it fits, then
// progressively fewer decimals, and finally a row of '#' if even the
// integer part doesn't -- never a truncated number, which would be a
// lie rather than a hint. Text is truncated. Errors print as their
// short name.
//
// *right_align (may be NULL) says how the caller should place it:
// numbers and errors right, text left, the usual convention.
//
// Returns the length written.
int sheet_display(sheet_t *s, int row, int col, char *out, int cap,
	int width, bool *right_align);

// Highest row/column holding anything, +1 -- i.e. the extent to scroll
// over. Both are 0 for an empty sheet.
void sheet_extent(const sheet_t *s, int *rows, int *cols);

// -- writing --
//
// `src` is taken exactly as typed. NULL or "" clears the cell.
//
// Returns SHEET_OK, SHEET_ERR_REF for coordinates outside the grid, or
// SHEET_ERR_FULL if the cell array or the text arena is exhausted --
// the latter only after a compaction has already been tried.
sheet_err_t sheet_set(sheet_t *s, int row, int col, const char *src);

// Clears a rectangle, inclusive of both corners, in either order.
void sheet_clear_range(sheet_t *s, int r0, int c0, int r1, int c1);

uint8_t sheet_colw(const sheet_t *s, int col);
void sheet_set_colw(sheet_t *s, int col, int w);

// -- references --

// Parses "A1", "$B$7", "z12" (case-insensitive; '$' accepted and
// ignored, so a file exported from something else loads). Writes
// 0-based row/col. *end, if non-NULL, receives the first unconsumed
// character.
//
// The '$' is ignored rather than rejected because nothing here copies
// formulas between cells, so absolute and relative mean the same
// thing -- and refusing to load a file over a distinction that has no
// effect would be the worse answer.
bool sheet_parse_ref(const char *s, int *row, int *col, const char **end);

// "A1" for (0,0). Returns the length written.
int sheet_ref_name(int row, int col, char *out, int cap);

// -- files --
//
// See docs/sheet_app.md for the format. In short: one line per
// non-empty cell, "REF<space><exactly what you typed>", plus '!'
// directives for column widths and '#' comments.
//
// The writer emits through a callback so it never holds more than a
// line: the app hands it fs_write_chunk() (zfsapp.h), the tests hand
// it a buffer append. `len` of -1 means "to the NUL". Returning false
// aborts the write, which sheet_write() then reports.
typedef bool (*sheet_emit_fn)(void *ctx, const char *s, int len);

bool sheet_write(const sheet_t *s, sheet_emit_fn emit, void *ctx);

// Reading is line at a time, for the same reason. The sequence is
// begin, a line per line of the file (no trailing newline; a trailing
// '\r' is tolerated), then end.
//
// sheet_load_line() returns false only for a line it could not make
// sense of at all. A file with one bad line still loads the rest --
// refusing the whole document because line 40 is malformed would be
// the wrong trade for a format people are invited to hand-edit.
void sheet_load_begin(sheet_t *s);
bool sheet_load_line(sheet_t *s, const char *line);
void sheet_load_end(sheet_t *s);

// CSV, for interchange. Formulas are written as their VALUES, which is
// what every other tool expects to read; that is exactly why CSV is
// not the native format, since saving to it would quietly destroy the
// model.
bool sheet_write_csv(const sheet_t *s, sheet_emit_fn emit, void *ctx);

// Reading CSV: one call per line, rows assigned in order from 0.
// Fields become cell source verbatim, so a CSV that happens to contain
// "=SUM(A1:A9)" loads as a formula -- which is what someone who typed
// that into a CSV by hand meant.
void sheet_load_csv_begin(sheet_t *s);
bool sheet_load_csv_line(sheet_t *s, const char *line);
void sheet_load_csv_end(sheet_t *s);

// -- number formats --

// Formats the rectangle (either corner order). SHEET_FMT_GENERAL
// removes formatting there. Ranges the new one covers entirely are
// dropped; false when the list is full.
bool sheet_set_format(sheet_t *s, int r0, int c0, int r1, int c1, int kind, int dec);
// The format of a cell (GENERAL when none applies); *dec its decimals.
int sheet_format_at(const sheet_t *s, int row, int col, int *dec);
// "F2", ",0", "%1", "$2", "G" -- what a person types, and the file holds.
bool sheet_parse_format(const char *s, int *kind, int *dec);
int sheet_format_name(int kind, int dec, char *out, int cap);

// -- moving formulas --
//
// A formula refers to cells as RELATIVE (A1: "the cell two to my
// left") or ABSOLUTE ($A$1, or $A1 / A$1 for one half). Copying a
// formula moves its relative references with it; inserting or deleting
// rows or columns moves every reference that points past the change.

typedef enum {
	SHEET_MOVE_COPY = 0,	// relative refs move by (dr, dc)
	SHEET_MOVE_INS_ROWS,	// rows >= at move down by n
	SHEET_MOVE_DEL_ROWS,	// rows at..at+n-1 go: refs to them become #REF!
	SHEET_MOVE_INS_COLS,
	SHEET_MOVE_DEL_COLS,
} sheet_move_t;

// Rewrites `src` (a formula, "=...") into `out`. A reference moved off
// the grid, or into deleted cells, becomes #REF!, which evaluates to
// the #REF error. False if the result does not fit `cap` or
// SHEET_SRC_MAX.
bool sheet_rewrite(const char *src, char *out, int cap, sheet_move_t how,
	int a, int b);	// COPY: a = dr, b = dc; others: a = at, b = n

// Inserts or deletes whole rows or columns, rewriting every formula
// and format range. Inserting refuses (SHEET_ERR_FULL) when it would
// push a cell off the grid.
sheet_err_t sheet_insert_rows(sheet_t *s, int at, int n);
sheet_err_t sheet_delete_rows(sheet_t *s, int at, int n);
sheet_err_t sheet_insert_cols(sheet_t *s, int at, int n);
sheet_err_t sheet_delete_cols(sheet_t *s, int at, int n);

// Sorts the rows of a rectangle by column `key` (inside it): numbers
// first, in order, then text, alphabetically, ignoring case, then
// empty -- or the reverse with `desc`. Stable. Formulas move as if
// copied. `tmp` is scratch: 24 bytes a row plus the rectangle's text
// (sheet_range_size()); false (and nothing changed) when it is too
// small.
bool sheet_sort(sheet_t *s, int r0, int c0, int r1, int c1, int key,
	bool desc, void *tmp, int tmpcap);

// The sum and count of the numbers in a rectangle -- what the edit bar
// shows for a selection. False when one of them is an error.
bool sheet_range_stats(sheet_t *s, int r0, int c0, int r1, int c1,
	z_fix_t *sum, int *count);

// -- serialising: undo, clipboard, sort --
//
// A range's cells as bytes: per non-empty cell, row and column
// relative to (r0, c0) as two bytes each, a length byte and the text.
// sheet_range_size() is what sheet_range_save() needs.
int sheet_range_size(const sheet_t *s, int r0, int c0, int r1, int c1);
int sheet_range_save(const sheet_t *s, int r0, int c0, int r1, int c1,
	void *buf, int cap);
// Writes saved cells at (r0, c0); with `clear`, the rectangle h x w is
// emptied first. With `adjust`, formulas are moved as if copied from
// (from_r, from_c). Returns SHEET_OK or the first error (FULL).
sheet_err_t sheet_range_load(sheet_t *s, int r0, int c0, int h, int w,
	bool clear, const void *buf, int len, bool adjust, int from_r, int from_c);

// The whole document: cells, widths and formats. sheet_snapshot()
// returns the length, or -1 if `cap` is too small.
int sheet_snapshot_size(const sheet_t *s);
int sheet_snapshot(const sheet_t *s, void *buf, int cap);
bool sheet_restore(sheet_t *s, const void *buf, int len);

#endif
