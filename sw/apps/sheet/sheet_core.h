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

// 4 decimal places, against calc's 6 -- see zfix.h on why this is a
// per-app choice. A spreadsheet holds money and counts far more often
// than the result of dividing by three, and the two places traded away
// here buy two more digits of integer range.
#define SHEET_DP          4

// Non-empty cells, and bytes of source text across all of them.
//
// 1024 x 16 bytes + 16KB is 32KB of .bss, which is the same order as
// sw/apps/text's document buffer and line table. A full 26-column
// block is 39 rows deep at this cap; past that, setting a cell fails
// cleanly (SHEET_ERR_FULL) rather than losing data quietly.
#define SHEET_MAX_CELLS   1024
#define SHEET_ARENA       16384

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

#endif
