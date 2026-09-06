/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The spreadsheet model. See sheet_core.h.
 *
 * -- an invariant worth stating once --
 *
 * EVALUATION NEVER MOVES A CELL. It writes `val`, `err` and `state`
 * and nothing else, so an index or a source pointer held across a
 * nested evaluation stays valid. Both the range walker (which holds an
 * index while evaluating cells that may refer anywhere) and the parser
 * (which holds a pointer into the arena across nested evaluations)
 * depend on this. Anything added here that could insert, remove or
 * re-home a cell during evaluation breaks both, silently, and the
 * symptom would be a wrong number rather than a crash.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "sheet_core.h"

// -- evaluation state, private --
#define ST_IDLE   0
#define ST_BUSY   1
#define ST_DONE   2

const char *sheet_err_text(sheet_err_t e) {

	switch (e) {
		case SHEET_OK:          return "";
		case SHEET_ERR_SYNTAX:  return "#ERR";
		case SHEET_ERR_DIV0:    return "#DIV0";
		case SHEET_ERR_CIRC:    return "#CIRC";
		case SHEET_ERR_NAME:    return "#NAME";
		case SHEET_ERR_REF:     return "#REF";
		case SHEET_ERR_NUM:     return "#NUM";
		case SHEET_ERR_DEPTH:   return "#DEEP";
		case SHEET_ERR_FULL:    return "#FULL";
	}

	return "#ERR";

}

// -- coordinates --

static bool in_grid(int row, int col) {
	return row >= 0 && row < SHEET_ROWS && col >= 0 && col < SHEET_COLS;
}

static uint16_t mkkey(int row, int col) {
	return (uint16_t)(row * SHEET_COLS + col);
}

// Index of `key`, or -1. Cells are kept sorted, so this is a binary
// search -- which is what makes a range walk cheap enough to evaluate
// =SUM(A1:A999) without noticing.
static int find(const sheet_t *s, uint16_t key) {

	int lo = 0, hi = s->ncells - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint16_t k = s->cells[mid].key;
		if (k == key) return mid;
		if (k < key) lo = mid + 1;
		else hi = mid - 1;
	}

	return -1;

}

// First index whose key is >= `key` -- the insertion point, and the
// starting point for a range walk along a row.
static int lower_bound(const sheet_t *s, uint16_t key) {

	int lo = 0, hi = s->ncells;

	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (s->cells[mid].key < key) lo = mid + 1;
		else hi = mid;
	}

	return lo;

}

// -- the text arena --

// Every cached value is stale once anything changes, because a formula
// anywhere can depend on any cell. Resetting is a walk of the cell
// array rather than of the grid, so it costs the number of cells that
// exist, not the number that could.
static void invalidate(sheet_t *s) {

	for (int i = 0; i < s->ncells; i++)
		if (s->cells[i].kind == SHEET_KIND_FORMULA)
			s->cells[i].state = ST_IDLE;

}

/*
 * Compaction.
 *
 * The arena is a bump allocator: setting a cell appends its new text
 * and orphans the old. That is the right trade for the common case
 * (editing a cell is a few bytes and no bookkeeping), and it means the
 * arena eventually fills with text nothing points at.
 *
 * Compacting means moving every live string down over the dead ones,
 * which has to happen in ASCENDING OFFSET ORDER or a string gets
 * copied over one that has not been moved yet. Cells are sorted by
 * key, not by offset, so the order has to be worked out here.
 *
 * A stack-local index array, shellsorted. 2KB of a 16KB stack
 * (Z_PROC_STACK_SIZE_DEFAULT), taken at the top of a sheet_set() call
 * rather than anywhere deep, and released immediately. The
 * alternatives were a permanent 2KB of .bss for something that runs
 * rarely, or a repeated minimum scan at 1024^2 comparisons.
 */
static void compact(sheet_t *s) {

	uint16_t idx[SHEET_MAX_CELLS];
	int n = s->ncells;

	for (int i = 0; i < n; i++) idx[i] = (uint16_t)i;

	// Shellsort by arena offset. Ciura's gaps, truncated -- an
	// insertion sort would be O(n^2) on the reversed-ish order an
	// edited sheet produces, which is exactly the case this hits.
	static const int gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };

	for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {

		int gap = gaps[g];
		if (gap >= n) continue;

		for (int i = gap; i < n; i++) {
			uint16_t t = idx[i];
			uint16_t toff = s->cells[t].off;
			int j = i;
			while (j >= gap && s->cells[idx[j - gap]].off > toff) {
				idx[j] = idx[j - gap];
				j -= gap;
			}
			idx[j] = t;
		}

	}

	int dst = 0;

	for (int i = 0; i < n; i++) {

		sheet_cell_t *c = &s->cells[idx[i]];
		int len = c->len + 1;			// including the NUL

		if ((int)c->off != dst)
			memmove(&s->arena[dst], &s->arena[c->off], (size_t)len);

		c->off = (uint16_t)dst;
		dst += len;

	}

	s->arena_used = dst;
	s->arena_live = dst;

}

// Copies `src` into the arena, returning its offset, or -1 if there is
// no room even after compacting.
static int arena_put(sheet_t *s, const char *src, int len) {

	if (s->arena_used + len + 1 > SHEET_ARENA) {

		compact(s);

		if (s->arena_used + len + 1 > SHEET_ARENA) return -1;

	}

	int off = s->arena_used;

	memcpy(&s->arena[off], src, (size_t)len);
	s->arena[off + len] = 0;

	s->arena_used += len + 1;
	s->arena_live += len + 1;

	return off;

}

// -- setup --

void sheet_init(sheet_t *s) {

	// Explicit rather than left to .bss zero-init, which has already
	// been shown unreliable on this hardware at least once -- see
	// docs/app_runtime.md and k_pidreg_init()'s call site in
	// sw/os/kernel.c. A garbage ncells here would index anywhere.
	s->ncells = 0;
	s->arena_used = 0;
	s->arena_live = 0;
	s->modified = false;
	s->depth = 0;
	s->load_row = 0;

	for (int i = 0; i < SHEET_COLS; i++) s->colw[i] = SHEET_COLW_DEF;

	memset(s->cells, 0, sizeof(s->cells));

}

// -- classification --
//
// The one rule, shared by the editor, the loader and the saver. See
// sheet_core.h's header comment on why it matters that there is only
// one of these.
static uint8_t classify(const char *src, z_fix_t *val) {

	*val = 0;

	if (src[0] == '=') return SHEET_KIND_FORMULA;
	if (src[0] == '\'') return SHEET_KIND_TEXT;

	if (z_fix_parse_all(src, SHEET_DP, val)) return SHEET_KIND_NUM;

	*val = 0;
	return SHEET_KIND_TEXT;

}

sheet_err_t sheet_set(sheet_t *s, int row, int col, const char *src) {

	if (!in_grid(row, col)) return SHEET_ERR_REF;

	uint16_t key = mkkey(row, col);
	int i = find(s, key);

	int len = 0;
	if (src) { while (src[len] && len < SHEET_SRC_MAX - 1) len++; }

	// Trailing whitespace is dropped on the way in. It is invisible in
	// the grid and in the file, so keeping it would mean two cells
	// that look identical comparing unequal, and a file that grows
	// every time it round-trips through an editor that strips it.
	while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;

	if (len == 0) {

		// Clearing. The old text is simply orphaned; compaction
		// reclaims it if the arena ever needs the room.
		if (i >= 0) {
			s->arena_live -= s->cells[i].len + 1;
			memmove(&s->cells[i], &s->cells[i + 1],
				(size_t)(s->ncells - i - 1) * sizeof(sheet_cell_t));
			s->ncells--;
			s->modified = true;
			invalidate(s);
		}

		return SHEET_OK;

	}

	if (i < 0 && s->ncells >= SHEET_MAX_CELLS) return SHEET_ERR_FULL;

	int off = arena_put(s, src, len);
	if (off < 0) return SHEET_ERR_FULL;

	if (i < 0) {

		i = lower_bound(s, key);

		memmove(&s->cells[i + 1], &s->cells[i],
			(size_t)(s->ncells - i) * sizeof(sheet_cell_t));

		s->ncells++;

	} else {

		s->arena_live -= s->cells[i].len + 1;

	}

	sheet_cell_t *c = &s->cells[i];

	c->key = key;
	c->off = (uint16_t)off;
	c->len = (uint8_t)len;
	c->kind = classify(&s->arena[off], &c->val);
	c->err = SHEET_OK;
	c->state = (c->kind == SHEET_KIND_FORMULA) ? ST_IDLE : ST_DONE;

	s->modified = true;
	invalidate(s);

	return SHEET_OK;

}

void sheet_clear_range(sheet_t *s, int r0, int c0, int r1, int c1) {

	if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
	if (c0 > c1) { int t = c0; c0 = c1; c1 = t; }

	for (int r = r0; r <= r1; r++)
		for (int c = c0; c <= c1; c++)
			sheet_set(s, r, c, NULL);

}

const char *sheet_src(const sheet_t *s, int row, int col) {

	if (!in_grid(row, col)) return "";

	int i = find(s, mkkey(row, col));
	if (i < 0) return "";

	return &s->arena[s->cells[i].off];

}

bool sheet_empty(const sheet_t *s, int row, int col) {
	return !in_grid(row, col) || find(s, mkkey(row, col)) < 0;
}

sheet_kind_t sheet_kind(const sheet_t *s, int row, int col) {

	if (!in_grid(row, col)) return SHEET_KIND_TEXT;

	int i = find(s, mkkey(row, col));
	if (i < 0) return SHEET_KIND_TEXT;

	return (sheet_kind_t)s->cells[i].kind;

}

uint8_t sheet_colw(const sheet_t *s, int col) {

	if (col < 0 || col >= SHEET_COLS) return SHEET_COLW_DEF;
	return s->colw[col];

}

void sheet_set_colw(sheet_t *s, int col, int w) {

	if (col < 0 || col >= SHEET_COLS) return;

	if (w < SHEET_COLW_MIN) w = SHEET_COLW_MIN;
	if (w > SHEET_COLW_MAX) w = SHEET_COLW_MAX;

	if (s->colw[col] != (uint8_t)w) {
		s->colw[col] = (uint8_t)w;
		s->modified = true;
	}

}

void sheet_extent(const sheet_t *s, int *rows, int *cols) {

	int mr = 0, mc = 0;

	for (int i = 0; i < s->ncells; i++) {
		int r = s->cells[i].key / SHEET_COLS;
		int c = s->cells[i].key % SHEET_COLS;
		if (r + 1 > mr) mr = r + 1;
		if (c + 1 > mc) mc = c + 1;
	}

	if (rows) *rows = mr;
	if (cols) *cols = mc;

}

// -- references --

bool sheet_parse_ref(const char *s, int *row, int *col, const char **end) {

	const char *p = s;
	int c, r = 0;
	bool any = false;

	if (*p == '$') p++;

	if (*p >= 'A' && *p <= 'Z') c = *p - 'A';
	else if (*p >= 'a' && *p <= 'z') c = *p - 'a';
	else return false;

	p++;

	// A second letter means a column past Z, which this grid does not
	// have. Refusing here (rather than stopping after one letter and
	// leaving "AA1" to parse as A, then garbage) is what makes the
	// error #REF rather than a syntax error in a confusing place.
	if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) return false;

	if (*p == '$') p++;

	while (*p >= '0' && *p <= '9') {
		any = true;
		r = r * 10 + (*p - '0');
		if (r > SHEET_ROWS) return false;
		p++;
	}

	if (!any || r < 1) return false;

	if (row) *row = r - 1;
	if (col) *col = c;
	if (end) *end = p;

	return true;

}

int sheet_ref_name(int row, int col, char *out, int cap) {

	if (cap < 5) { if (cap > 0) out[0] = 0; return 0; }

	int n = 0;

	out[n++] = (char)('A' + (col >= 0 && col < SHEET_COLS ? col : 0));

	int r = row + 1;
	char tmp[4];
	int t = 0;

	if (r < 1) r = 1;
	while (r && t < 4) { tmp[t++] = (char)('0' + r % 10); r /= 10; }
	while (t) out[n++] = tmp[--t];

	out[n] = 0;
	return n;

}

// -- evaluation --

typedef struct {
	sheet_t		*s;
	const char	*p;
	sheet_err_t	err;
} pstate_t;

static bool parse_expr(pstate_t *ps, z_fix_t *out);

static void skipws(pstate_t *ps) {
	while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

static bool fail(pstate_t *ps, sheet_err_t e) {
	if (ps->err == SHEET_OK) ps->err = e;
	return false;
}

// Aggregate accumulator -- see the function table below.
typedef struct {
	int		count;
	z_fix_t	sum, minv, maxv;
	bool	bad;
} acc_t;

static void acc_init(acc_t *a) {
	a->count = 0;
	a->sum = 0;
	a->minv = 0;
	a->maxv = 0;
	a->bad = false;
}

static void acc_add(acc_t *a, z_fix_t v) {

	if (a->count == 0) { a->minv = v; a->maxv = v; }
	else {
		if (v < a->minv) a->minv = v;
		if (v > a->maxv) a->maxv = v;
	}

	if (!z_fix_add(a->sum, v, &a->sum)) a->bad = true;

	a->count++;

}

static sheet_err_t eval_cell(sheet_t *s, int row, int col, z_fix_t *out);

// Feeds every NUMERIC cell of a rectangle into `a`.
//
// Text and empty cells are skipped rather than counted as zero: that
// distinction is the whole difference between AVG and "sum over the
// cells I selected", and getting it wrong makes an average of three
// numbers in a ten-cell selection come out as three tenths of the
// right answer.
static sheet_err_t acc_range(sheet_t *s, acc_t *a,
	int r0, int c0, int r1, int c1) {

	if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
	if (c0 > c1) { int t = c0; c0 = c1; c1 = t; }

	for (int r = r0; r <= r1; r++) {

		// Walk the row's own slice of the sorted array rather than
		// probing every coordinate: a range over mostly-empty rows
		// then costs one binary search per row instead of one per
		// cell.
		int i = lower_bound(s, mkkey(r, c0));
		uint16_t last = mkkey(r, c1);

		while (i < s->ncells && s->cells[i].key <= last) {

			if (s->cells[i].kind != SHEET_KIND_TEXT) {

				int cr = s->cells[i].key / SHEET_COLS;
				int cc = s->cells[i].key % SHEET_COLS;

				z_fix_t v;
				sheet_err_t e = eval_cell(s, cr, cc, &v);

				if (e != SHEET_OK) return e;

				acc_add(a, v);

			}

			i++;

		}

	}

	return SHEET_OK;

}

// -- functions --

typedef enum {
	FN_SUM = 0, FN_AVG, FN_MIN, FN_MAX, FN_COUNT,
	FN_ABS, FN_INT, FN_ROUND,
	FN_NONE
} fn_id_t;

static const struct { const char *name; fn_id_t id; } fn_table[] = {
	{ "SUM",     FN_SUM   },
	{ "AVG",     FN_AVG   },
	{ "AVERAGE", FN_AVG   },		// both spellings; people type either
	{ "MIN",     FN_MIN   },
	{ "MAX",     FN_MAX   },
	{ "COUNT",   FN_COUNT },
	{ "ABS",     FN_ABS   },
	{ "INT",     FN_INT   },
	{ "ROUND",   FN_ROUND },
	{ NULL,      FN_NONE  },
};

static bool name_eq(const char *a, int alen, const char *b) {

	int i = 0;

	for (; i < alen && b[i]; i++) {
		char ca = a[i];
		if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
		if (ca != b[i]) return false;
	}

	return i == alen && !b[i];

}

static fn_id_t fn_lookup(const char *name, int len) {

	for (int i = 0; fn_table[i].name; i++)
		if (name_eq(name, len, fn_table[i].name)) return fn_table[i].id;

	return FN_NONE;

}

static bool fn_is_aggregate(fn_id_t f) {
	return f == FN_SUM || f == FN_AVG || f == FN_MIN ||
		f == FN_MAX || f == FN_COUNT;
}

// Parses "(" args ")" and produces the function's value.
static bool parse_call(pstate_t *ps, fn_id_t f, z_fix_t *out) {

	acc_t a;
	z_fix_t argv[2];
	int argc = 0;

	acc_init(&a);

	skipws(ps);
	if (*ps->p != '(') return fail(ps, SHEET_ERR_SYNTAX);
	ps->p++;

	skipws(ps);

	// A call with no arguments at all. SUM() is 0, which is harmless
	// and occasionally what a generated file contains; ABS() is not,
	// and is caught by the arity check below.
	if (*ps->p == ')') {
		ps->p++;
	} else {

		for (;;) {

			skipws(ps);

			// A range, but only where a range means something. Trying
			// the reference first and rewinding is what lets "A1" be
			// an ordinary value and "A1:A9" be a rectangle without two
			// grammars for the same characters.
			const char *save = ps->p;
			int r0, c0, r1, c1;
			const char *after = NULL;
			bool is_range = false;

			if (sheet_parse_ref(ps->p, &r0, &c0, &after)) {

				const char *q = after;
				while (*q == ' ' || *q == '\t') q++;

				if (*q == ':') {
					q++;
					while (*q == ' ' || *q == '\t') q++;
					if (sheet_parse_ref(q, &r1, &c1, &after)) {
						is_range = true;
						ps->p = after;
					}
				}

			}

			if (is_range) {

				if (!fn_is_aggregate(f)) return fail(ps, SHEET_ERR_NUM);

				sheet_err_t e = acc_range(ps->s, &a, r0, c0, r1, c1);
				if (e != SHEET_OK) return fail(ps, e);

			} else {

				ps->p = save;

				z_fix_t v;
				if (!parse_expr(ps, &v)) return false;

				if (fn_is_aggregate(f)) acc_add(&a, v);
				else if (argc < 2) argv[argc++] = v;
				else return fail(ps, SHEET_ERR_NUM);

			}

			skipws(ps);

			if (*ps->p == ',') { ps->p++; continue; }
			if (*ps->p == ')') { ps->p++; break; }

			return fail(ps, SHEET_ERR_SYNTAX);

		}

	}

	if (a.bad) return fail(ps, SHEET_ERR_NUM);

	switch (f) {

		case FN_SUM:
			*out = a.sum;
			return true;

		case FN_AVG:
			// An average of nothing is not zero, it is a question with
			// no answer -- reporting it as 0 would put a plausible
			// number in a cell that has none.
			if (!a.count) return fail(ps, SHEET_ERR_DIV0);
			if (!z_fix_div(a.sum, (z_fix_t)a.count * z_fix_scale(SHEET_DP),
				SHEET_DP, out)) return fail(ps, SHEET_ERR_NUM);
			return true;

		case FN_MIN:
			*out = a.count ? a.minv : 0;
			return true;

		case FN_MAX:
			*out = a.count ? a.maxv : 0;
			return true;

		case FN_COUNT:
			if (!z_fix_from_int(a.count, SHEET_DP, out))
				return fail(ps, SHEET_ERR_NUM);
			return true;

		case FN_ABS:
			if (argc != 1) return fail(ps, SHEET_ERR_NUM);
			*out = argv[0] < 0 ? -argv[0] : argv[0];
			return true;

		case FN_INT: {
			// Toward negative infinity, matching every spreadsheet
			// there has ever been: INT(-1.5) is -2, not -1. C's
			// division truncates toward zero, so the negative case
			// needs the extra step.
			if (argc != 1) return fail(ps, SHEET_ERR_NUM);
			z_fix_t sc = z_fix_scale(SHEET_DP);
			z_fix_t q = argv[0] / sc;
			if (argv[0] < 0 && (argv[0] % sc) != 0) q--;
			*out = q * sc;
			return true;
		}

		case FN_ROUND: {
			if (argc != 2) return fail(ps, SHEET_ERR_NUM);
			z_fix_t sc = z_fix_scale(SHEET_DP);
			int places = (int)(argv[1] / sc);
			if (places < 0) places = 0;
			if (places > SHEET_DP) places = SHEET_DP;
			z_fix_t unit = z_fix_scale(SHEET_DP - places);
			z_fix_t v = argv[0];
			z_fix_t half = unit / 2;
			if (v >= 0) *out = ((v + half) / unit) * unit;
			else        *out = -(((-v + half) / unit) * unit);
			return true;
		}

		default:
			return fail(ps, SHEET_ERR_NAME);

	}

}

static bool parse_primary(pstate_t *ps, z_fix_t *out) {

	skipws(ps);

	if (*ps->p == '(') {

		ps->p++;
		if (!parse_expr(ps, out)) return false;
		skipws(ps);
		if (*ps->p != ')') return fail(ps, SHEET_ERR_SYNTAX);
		ps->p++;
		return true;

	}

	if ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.') {

		const char *end = NULL;
		if (!z_fix_parse(ps->p, SHEET_DP, out, &end))
			return fail(ps, SHEET_ERR_NUM);
		ps->p = end;
		return true;

	}

	// A name: either a function call or a cell reference. Which it is
	// is decided by what follows the letters, so both are read from
	// the same starting point and one of them rewinds.
	if ((*ps->p >= 'A' && *ps->p <= 'Z') ||
		(*ps->p >= 'a' && *ps->p <= 'z') || *ps->p == '$') {

		const char *start = ps->p;
		const char *q = ps->p;

		while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) q++;

		const char *afterword = q;
		while (*afterword == ' ' || *afterword == '\t') afterword++;

		if (*afterword == '(') {

			fn_id_t f = fn_lookup(start, (int)(q - start));
			if (f == FN_NONE) return fail(ps, SHEET_ERR_NAME);

			ps->p = afterword;
			return parse_call(ps, f, out);

		}

		int row, col;
		const char *end = NULL;

		if (!sheet_parse_ref(start, &row, &col, &end))
			return fail(ps, SHEET_ERR_REF);

		ps->p = end;

		sheet_err_t e = eval_cell(ps->s, row, col, out);
		if (e != SHEET_OK) return fail(ps, e);

		return true;

	}

	return fail(ps, SHEET_ERR_SYNTAX);

}

static bool parse_unary(pstate_t *ps, z_fix_t *out) {

	skipws(ps);

	if (*ps->p == '-') {
		ps->p++;
		if (!parse_unary(ps, out)) return false;
		*out = -*out;
		return true;
	}

	if (*ps->p == '+') {
		ps->p++;
		return parse_unary(ps, out);
	}

	return parse_primary(ps, out);

}

static bool parse_term(pstate_t *ps, z_fix_t *out) {

	if (!parse_unary(ps, out)) return false;

	for (;;) {

		skipws(ps);

		char op = *ps->p;
		if (op != '*' && op != '/') return true;

		ps->p++;

		z_fix_t rhs;
		if (!parse_unary(ps, &rhs)) return false;

		if (op == '*') {
			if (!z_fix_mul(*out, rhs, SHEET_DP, out))
				return fail(ps, SHEET_ERR_NUM);
		} else {
			if (rhs == 0) return fail(ps, SHEET_ERR_DIV0);
			if (!z_fix_div(*out, rhs, SHEET_DP, out))
				return fail(ps, SHEET_ERR_NUM);
		}

	}

}

static bool parse_expr(pstate_t *ps, z_fix_t *out) {

	if (!parse_term(ps, out)) return false;

	for (;;) {

		skipws(ps);

		char op = *ps->p;
		if (op != '+' && op != '-') return true;

		ps->p++;

		z_fix_t rhs;
		if (!parse_term(ps, &rhs)) return false;

		bool ok = (op == '+') ? z_fix_add(*out, rhs, out)
		                      : z_fix_sub(*out, rhs, out);

		if (!ok) return fail(ps, SHEET_ERR_NUM);

	}

}

static sheet_err_t eval_cell(sheet_t *s, int row, int col, z_fix_t *out) {

	*out = 0;

	if (!in_grid(row, col)) return SHEET_ERR_REF;

	int i = find(s, mkkey(row, col));
	if (i < 0) return SHEET_OK;			// empty is zero

	sheet_cell_t *c = &s->cells[i];

	if (c->kind != SHEET_KIND_FORMULA) {
		*out = c->val;
		return (sheet_err_t)c->err;
	}

	// A formula that reaches a cell already being evaluated has, by
	// definition, come back to where it started. One comparison, no
	// dependency graph.
	if (c->state == ST_BUSY) return SHEET_ERR_CIRC;

	if (c->state == ST_DONE) {
		*out = c->val;
		return (sheet_err_t)c->err;
	}

	if (s->depth >= SHEET_MAX_DEPTH) {
		// NOT cached: a depth failure is a property of the path taken
		// to get here, not of this cell, so caching it would make an
		// unrelated later evaluation wrong.
		return SHEET_ERR_DEPTH;
	}

	c->state = ST_BUSY;
	s->depth++;

	pstate_t ps;
	ps.s = s;
	ps.p = &s->arena[c->off] + 1;		// past the '='
	ps.err = SHEET_OK;

	z_fix_t v = 0;
	bool ok = parse_expr(&ps, &v);

	if (ok) {
		skipws(&ps);
		if (*ps.p) { ok = false; ps.err = SHEET_ERR_SYNTAX; }
	}

	s->depth--;

	// `c` is still valid: evaluation never moves a cell (see this
	// file's header comment), and the recursion above only ever wrote
	// val/err/state.
	c = &s->cells[i];

	if (!ok) {

		sheet_err_t e = ps.err == SHEET_OK ? SHEET_ERR_SYNTAX : ps.err;

		if (e == SHEET_ERR_DEPTH) {
			c->state = ST_IDLE;
			return e;
		}

		c->err = (uint8_t)e;
		c->val = 0;
		c->state = ST_DONE;

		return e;

	}

	c->err = SHEET_OK;
	c->val = v;
	c->state = ST_DONE;

	*out = v;
	return SHEET_OK;

}

sheet_err_t sheet_value(sheet_t *s, int row, int col, z_fix_t *out) {
	return eval_cell(s, row, col, out);
}

// -- display --

int sheet_display(sheet_t *s, int row, int col, char *out, int cap,
	int width, bool *right_align) {

	if (cap < 1) return 0;

	out[0] = 0;

	if (right_align) *right_align = false;

	if (!in_grid(row, col)) return 0;

	int i = find(s, mkkey(row, col));
	if (i < 0) return 0;

	if (width > cap - 1) width = cap - 1;
	if (width < 1) return 0;

	sheet_cell_t *c = &s->cells[i];

	if (c->kind == SHEET_KIND_TEXT) {

		// The leading apostrophe is the marker that forced this to be
		// text; it is not part of the text.
		const char *t = &s->arena[c->off];
		if (*t == '\'') t++;

		int n = 0;
		while (t[n] && n < width) { out[n] = t[n]; n++; }
		out[n] = 0;

		return n;

	}

	if (right_align) *right_align = true;

	z_fix_t v;
	sheet_err_t e = sheet_value(s, row, col, &v);

	if (e != SHEET_OK) {

		const char *t = sheet_err_text(e);
		int n = 0;
		while (t[n] && n < width) { out[n] = t[n]; n++; }
		out[n] = 0;

		return n;

	}

	// Full precision if it fits, then progressively fewer decimals.
	// Never a truncated number: "1234" in a cell holding 1234567 is a
	// lie, where "####" is a request for a wider column.
	if (z_fix_format_len(v, SHEET_DP, Z_FIX_STRIP) <= width)
		return z_fix_format(v, SHEET_DP, Z_FIX_STRIP, out, cap);

	for (int places = SHEET_DP - 1; places >= 0; places--)
		if (z_fix_format_len(v, SHEET_DP, places) <= width)
			return z_fix_format(v, SHEET_DP, places, out, cap);

	for (int n = 0; n < width; n++) out[n] = '#';
	out[width] = 0;

	return width;

}

// -- native file format --

static bool emit_s(sheet_emit_fn emit, void *ctx, const char *s) {
	return emit(ctx, s, -1);
}

bool sheet_write(const sheet_t *s, sheet_emit_fn emit, void *ctx) {

	char buf[8];

	if (!emit_s(emit, ctx, "zsheet 1\n")) return false;

	// Column widths, only where they differ from the default -- a file
	// full of "!w C 9" lines would be noise in something meant to be
	// read and edited by hand.
	for (int c = 0; c < SHEET_COLS; c++) {

		if (s->colw[c] == SHEET_COLW_DEF) continue;

		if (!emit_s(emit, ctx, "!w ")) return false;

		buf[0] = (char)('A' + c);
		buf[1] = ' ';
		buf[2] = 0;
		if (!emit_s(emit, ctx, buf)) return false;

		int w = s->colw[c], t = 0;
		char tmp[4];
		while (w && t < 3) { tmp[t++] = (char)('0' + w % 10); w /= 10; }
		int n = 0;
		while (t) buf[n++] = tmp[--t];
		buf[n++] = '\n';
		buf[n] = 0;

		if (!emit_s(emit, ctx, buf)) return false;

	}

	for (int i = 0; i < s->ncells; i++) {

		const sheet_cell_t *c = &s->cells[i];

		char ref[8];
		sheet_ref_name(c->key / SHEET_COLS, c->key % SHEET_COLS,
			ref, sizeof(ref));

		if (!emit_s(emit, ctx, ref)) return false;
		if (!emit(ctx, " ", 1)) return false;
		if (!emit(ctx, &s->arena[c->off], c->len)) return false;
		if (!emit(ctx, "\n", 1)) return false;

	}

	return true;

}

void sheet_load_begin(sheet_t *s) {
	sheet_init(s);
}

bool sheet_load_line(sheet_t *s, const char *line) {

	const char *p = line;

	while (*p == ' ' || *p == '\t') p++;

	if (!*p || *p == '\r') return true;			// blank
	if (*p == '#') return true;					// comment

	if (*p == '!') {

		p++;

		// "!w <col> <n>". Anything else is ignored rather than
		// refused: a directive from a later version of the format
		// should cost an old build nothing but the line.
		if ((*p == 'w' || *p == 'W') &&
			(p[1] == ' ' || p[1] == '\t')) {

			p += 2;
			while (*p == ' ' || *p == '\t') p++;

			int col;
			if (*p >= 'A' && *p <= 'Z') col = *p - 'A';
			else if (*p >= 'a' && *p <= 'z') col = *p - 'a';
			else return false;

			p++;
			while (*p == ' ' || *p == '\t') p++;

			int w = 0;
			if (*p < '0' || *p > '9') return false;
			while (*p >= '0' && *p <= '9') { w = w * 10 + (*p - '0'); p++; }

			sheet_set_colw(s, col, w);

		}

		return true;

	}

	// The header line, if present. Its version is not checked against
	// anything: there is one version, and a file claiming a later one
	// is still worth trying to read -- the format is line-oriented, so
	// the worst case is that some lines are skipped.
	if ((p[0] == 'z' || p[0] == 'Z') && !strncmp(p + 1, "sheet", 5) &&
		(p[6] == ' ' || p[6] == 0 || p[6] == '\r'))
		return true;

	int row, col;
	const char *end = NULL;

	if (!sheet_parse_ref(p, &row, &col, &end)) return false;

	// Exactly one space between the reference and the content, and
	// everything after it is the content -- including further spaces,
	// which is how a label with leading whitespace survives.
	if (*end == 0 || *end == '\r') {
		sheet_set(s, row, col, NULL);
		return true;
	}

	if (*end != ' ') return false;

	const char *src = end + 1;

	// Tolerate CRLF without carrying the '\r' into the cell.
	char tmp[SHEET_SRC_MAX];
	int n = 0;

	while (src[n] && src[n] != '\r' && src[n] != '\n' &&
		n < SHEET_SRC_MAX - 1) {
		tmp[n] = src[n];
		n++;
	}

	tmp[n] = 0;

	sheet_set(s, row, col, tmp);

	return true;

}

void sheet_load_end(sheet_t *s) {
	// A freshly loaded document is not a modified one, whatever the
	// setters above thought while it was being built.
	s->modified = false;
}

// -- CSV --

static bool csv_needs_quotes(const char *s) {

	if (!*s) return false;
	if (*s == ' ' || *s == '\t') return true;

	for (const char *p = s; *p; p++) {
		if (*p == ',' || *p == '"' || *p == '\n') return true;
		if (!p[1] && (*p == ' ' || *p == '\t')) return true;
	}

	return false;

}

static bool csv_field(sheet_emit_fn emit, void *ctx, const char *s) {

	if (!csv_needs_quotes(s)) return emit_s(emit, ctx, s);

	if (!emit(ctx, "\"", 1)) return false;

	for (const char *p = s; *p; p++) {
		if (!emit(ctx, p, 1)) return false;
		if (*p == '"' && !emit(ctx, "\"", 1)) return false;
	}

	return emit(ctx, "\"", 1);

}

bool sheet_write_csv(const sheet_t *s, sheet_emit_fn emit, void *ctx) {

	int rows, cols;
	sheet_extent(s, &rows, &cols);

	// Cast away const for the evaluation below. Writing CSV needs
	// VALUES, and computing one caches it -- which is a change to the
	// struct's memory but not to the document, and is exactly why
	// sheet_value() is not const either.
	sheet_t *m = (sheet_t *)s;

	for (int r = 0; r < rows; r++) {

		for (int c = 0; c < cols; c++) {

			if (c && !emit(ctx, ",", 1)) return false;

			int i = find(s, mkkey(r, c));
			if (i < 0) continue;

			const sheet_cell_t *cell = &s->cells[i];

			if (cell->kind == SHEET_KIND_TEXT) {

				const char *t = &s->arena[cell->off];
				if (*t == '\'') t++;
				if (!csv_field(emit, ctx, t)) return false;

			} else {

				z_fix_t v;
				sheet_err_t e = sheet_value(m, r, c, &v);

				char num[32];

				if (e != SHEET_OK) {
					const char *t = sheet_err_text(e);
					int n = 0;
					while (t[n] && n < (int)sizeof(num) - 1) {
						num[n] = t[n]; n++;
					}
					num[n] = 0;
				} else {
					z_fix_format(v, SHEET_DP, Z_FIX_STRIP, num, sizeof(num));
				}

				if (!csv_field(emit, ctx, num)) return false;

			}

		}

		if (!emit(ctx, "\n", 1)) return false;

	}

	return true;

}

void sheet_load_csv_begin(sheet_t *s) {
	sheet_init(s);
	s->load_row = 0;
}

bool sheet_load_csv_line(sheet_t *s, const char *line) {

	if (s->load_row >= SHEET_ROWS) return false;

	int col = 0;
	const char *p = line;

	// A completely empty line still consumes a row: a blank line in
	// the middle of a CSV is a blank row, not a separator to skip, and
	// skipping it would shift everything below it up by one.
	for (;;) {

		char tmp[SHEET_SRC_MAX];
		int n = 0;

		if (*p == '"') {

			p++;

			while (*p) {
				if (*p == '"') {
					if (p[1] == '"') { p++; }		// "" is one quote
					else { p++; break; }
				}
				if (n < SHEET_SRC_MAX - 1) tmp[n++] = *p;
				p++;
			}

		} else {

			while (*p && *p != ',' && *p != '\r' && *p != '\n') {
				if (n < SHEET_SRC_MAX - 1) tmp[n++] = *p;
				p++;
			}

		}

		tmp[n] = 0;

		if (n && col < SHEET_COLS)
			sheet_set(s, s->load_row, col, tmp);

		col++;

		while (*p == ' ' || *p == '\t') p++;

		if (*p == ',') { p++; continue; }

		break;

	}

	s->load_row++;

	return true;

}

void sheet_load_csv_end(sheet_t *s) {
	s->modified = false;
}
