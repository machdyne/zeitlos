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
// anywhere can depend on any cell. A cell's state carries the
// generation it was computed in (st_get() below), so moving to the
// next generation forgets them all at once; only when the six-bit
// count wraps are the cells visited, once in 64 changes. Before 0.0.7
// every change visited every cell, which made loading a file -- a
// change per cell -- quadratic.
static void invalidate(sheet_t *s) {

	s->gen = (uint8_t)((s->gen + 1) & 63);

	if (s->gen == 0)
		for (int i = 0; i < s->ncells; i++)
			s->cells[i].state = ST_IDLE;		// generation 0, idle

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
	s->gen = 0;
	s->nfmts = 0;
	memset(&s->chart, 0, sizeof s->chart);

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

	s->modified = true;
	invalidate(s);
	c->state = (uint8_t)((s->gen << 2) | (c->kind == SHEET_KIND_FORMULA ? ST_IDLE : ST_DONE));

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
	// Inside the branch IF did not take: parsed, so the formula is
	// checked whole, but not evaluated -- references are not followed
	// and arithmetic errors do not count. IF(B1=0,0,A1/B1) is the
	// reason: the division must not be an error when B1 is 0.
	int			skip;
} pstate_t;

static bool parse_expr(pstate_t *ps, z_fix_t *out);

static void skipws(pstate_t *ps) {
	while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

static bool fail(pstate_t *ps, sheet_err_t e) {
	if (ps->err == SHEET_OK) ps->err = e;
	return false;
}

// An arithmetic error: an error, except in a branch not taken.
static bool soft(pstate_t *ps, sheet_err_t e, z_fix_t *out) {
	if (ps->skip) { *out = 0; return true; }
	return fail(ps, e);
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

// -- multiplying and dividing --
//
// z_fix_mul() checks its double-scaled intermediate for overflow,
// which at six decimals refuses 4000 * 4000. These split the operands
// at the decimal point instead, so they fail only when the RESULT does
// not fit -- and divide by long division, for the same reason.

static bool fx_mul(z_fix_t a, z_fix_t b, z_fix_t *out) {
	const uint64_t S = (uint64_t)z_fix_scale(SHEET_DP), lim = (uint64_t)INT64_MAX;
	bool neg = (a < 0) != (b < 0);
	uint64_t ua = a < 0 ? (uint64_t)(-a) : (uint64_t)a, ub = b < 0 ? (uint64_t)(-b) : (uint64_t)b;
	uint64_t ai = ua / S, af = ua % S, bi = ub / S, bf = ub % S;
	if (ai && bi > (lim / S) / ai) return false;
	if (ai > lim / S || bi > lim / S) return false;
	uint64_t r = ai * bi * S, t[3] = { ai * bf, af * bi, (af * bf + S / 2) / S };
	for (int i = 0; i < 3; i++) { if (r > lim - t[i]) return false; r += t[i]; }
	*out = neg ? -(z_fix_t)r : (z_fix_t)r;
	return true;
}

static bool fx_div(z_fix_t a, z_fix_t b, z_fix_t *out) {
	const uint64_t S = (uint64_t)z_fix_scale(SHEET_DP);
	if (!b) return false;
	bool neg = (a < 0) != (b < 0);
	uint64_t ua = a < 0 ? (uint64_t)(-a) : (uint64_t)a, ub = b < 0 ? (uint64_t)(-b) : (uint64_t)b;
	uint64_t q = ua / ub, r = ua % ub, f = 0;
	if (q > (uint64_t)INT64_MAX / S || ub > UINT64_MAX / 10) return false;
	for (int i = 0; i < SHEET_DP; i++) { r *= 10; f = f * 10 + r / ub; r %= ub; }
	if (r * 2 >= ub) f++;		// rounded, not truncated
	uint64_t v = q * S + f;
	if (v > (uint64_t)INT64_MAX) return false;
	*out = neg ? -(z_fix_t)v : (z_fix_t)v;
	return true;
}

// -- the financial functions, and powers --
//
// (1 + rate) ^ periods at the sheet's four decimals is a dollar out on
// a mortgage, and even at nine (z_fix's most) the (g - 1) / rate in an
// annuity magnifies the last digit into cents. So these few use
// double precision -- software floating point, a few KB of libgcc, in
// an app that otherwise has none -- and come back to four decimals at
// the end, as every other result does.

static double fx2d(z_fix_t v) { return (double)v / (double)z_fix_scale(SHEET_DP); }

static bool d2fx(double d, z_fix_t *out) {
	d *= (double)z_fix_scale(SHEET_DP);
	if (d != d || d > 9.2e18 || d < -9.2e18) return false;
	*out = (z_fix_t)(d < 0 ? d - 0.5 : d + 0.5);
	return true;
}

// b ^ n for a whole n, by squaring.
static double dpow(double b, int32_t n) {
	bool neg = n < 0;
	uint32_t e = neg ? (uint32_t)(-(int64_t)n) : (uint32_t)n;
	double r = 1.0;
	while (e) {
		if (e & 1) r *= b;
		e >>= 1;
		if (e) b *= b;
	}
	return neg ? 1.0 / r : r;
}

static bool is_int(z_fix_t v) {
	return v % z_fix_scale(SHEET_DP) == 0;
}

static int32_t to_int(z_fix_t v) {
	z_fix_t q = v / z_fix_scale(SHEET_DP);
	if (q > INT32_MAX) return INT32_MAX;
	if (q < INT32_MIN) return INT32_MIN;
	return (int32_t)q;
}

// -- functions --

typedef enum {
	FN_SUM = 0, FN_AVG, FN_MIN, FN_MAX, FN_COUNT,		// aggregates
	FN_ABS, FN_INT, FN_ROUND, FN_MOD, FN_SQRT, FN_POWER,
	FN_IF, FN_AND, FN_OR, FN_NOT,
	FN_PMT, FN_PV, FN_FV, FN_NPV,
	FN_VLOOKUP, FN_INDEX,
	FN_NONE
} fn_id_t;

static const struct { const char *name; fn_id_t id; } fn_table[] = {
	{ "SUM",     FN_SUM    },
	{ "AVG",     FN_AVG    },
	{ "AVERAGE", FN_AVG    },		// both spellings; people type either
	{ "MIN",     FN_MIN    },
	{ "MAX",     FN_MAX    },
	{ "COUNT",   FN_COUNT  },
	{ "ABS",     FN_ABS    },
	{ "INT",     FN_INT    },
	{ "ROUND",   FN_ROUND  },
	{ "MOD",     FN_MOD    },
	{ "SQRT",    FN_SQRT   },
	{ "POWER",   FN_POWER  },
	{ "IF",      FN_IF     },
	{ "AND",     FN_AND    },
	{ "OR",      FN_OR     },
	{ "NOT",     FN_NOT    },
	{ "PMT",     FN_PMT    },
	{ "PV",      FN_PV     },
	{ "FV",      FN_FV     },
	{ "NPV",     FN_NPV    },
	{ "VLOOKUP", FN_VLOOKUP },
	{ "INDEX",   FN_INDEX  },
	{ NULL,      FN_NONE   },
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
	return f <= FN_COUNT;
}

// One argument: a value, or a rectangle.
typedef struct {
	bool	range;
	int		r0, c0, r1, c1;
	z_fix_t	v;
} arg_t;

#define ARGS_MAX 6

// A range at ps->p, if there is one there ("A1:B9").
static bool try_range(pstate_t *ps, arg_t *a) {

	int r0, c0, r1, c1;
	const char *after = NULL;

	if (!sheet_parse_ref(ps->p, &r0, &c0, &after)) return false;

	const char *q = after;
	while (*q == ' ' || *q == '\t') q++;
	if (*q != ':') return false;
	q++;
	while (*q == ' ' || *q == '\t') q++;
	if (!sheet_parse_ref(q, &r1, &c1, &after)) return false;

	ps->p = after;
	a->range = true;
	a->r0 = r0 < r1 ? r0 : r1;
	a->r1 = r0 < r1 ? r1 : r0;
	a->c0 = c0 < c1 ? c0 : c1;
	a->c1 = c0 < c1 ? c1 : c0;
	return true;

}

static bool truth(z_fix_t v) { return v != 0; }

static z_fix_t boolv(bool b) { return b ? z_fix_scale(SHEET_DP) : 0; }

// IF(cond, then, else): the branch not taken is parsed in skip mode.
static bool parse_if(pstate_t *ps, z_fix_t *out) {

	z_fix_t c, a = 0, b = 0;

	if (!parse_expr(ps, &c)) return false;
	skipws(ps);
	if (*ps->p != ',') return fail(ps, SHEET_ERR_SYNTAX);
	ps->p++;

	bool take = truth(c);

	if (!take) ps->skip++;
	bool ok = parse_expr(ps, &a);
	if (!take) ps->skip--;
	if (!ok) return false;

	skipws(ps);
	if (*ps->p == ',') {
		ps->p++;
		if (take) ps->skip++;
		ok = parse_expr(ps, &b);
		if (take) ps->skip--;
		if (!ok) return false;
		skipws(ps);
	}

	if (*ps->p != ')') return fail(ps, SHEET_ERR_SYNTAX);
	ps->p++;

	*out = take ? a : b;
	return true;

}

// The value at (row, col), as a lookup returns it.
static bool cell_value(pstate_t *ps, int row, int col, z_fix_t *out) {
	if (ps->skip) { *out = 0; return true; }
	sheet_err_t e = eval_cell(ps->s, row, col, out);
	if (e != SHEET_OK) return fail(ps, e);
	return true;
}

// Parses "(" args ")" and produces the function's value.
static bool parse_call(pstate_t *ps, fn_id_t f, z_fix_t *out) {

	acc_t a;
	arg_t argv[ARGS_MAX];
	int argc = 0;

	acc_init(&a);

	skipws(ps);
	if (*ps->p != '(') return fail(ps, SHEET_ERR_SYNTAX);
	ps->p++;

	if (f == FN_IF) return parse_if(ps, out);

	skipws(ps);

	// NPV's discounting goes cell by cell, in order, through every
	// value and range after the rate.
	double npv = 0, npv_f = 1;
	int npv_i = 0;

	// A call with no arguments at all. SUM() is 0, which is harmless
	// and occasionally what a generated file contains; ABS() is not,
	// and is caught by the arity checks below.
	if (*ps->p == ')') {
		ps->p++;
	} else {

		for (;;) {

			skipws(ps);

			// A range, but only where a range means something. Trying
			// the reference first and rewinding is what lets "A1" be
			// an ordinary value and "A1:A9" be a rectangle without two
			// grammars for the same characters.
			arg_t one;
			one.range = false;
			one.v = 0;

			if (try_range(ps, &one)) {

				if (fn_is_aggregate(f)) {
					if (!ps->skip) {
						sheet_err_t e = acc_range(ps->s, &a, one.r0, one.c0, one.r1, one.c1);
						if (e != SHEET_OK) return fail(ps, e);
					}
				} else if (f == FN_NPV && argc >= 1) {
					// fed through below, cell by cell
					if (!ps->skip) {
						for (int r = one.r0; r <= one.r1; r++)
							for (int c = one.c0; c <= one.c1; c++) {
								if (sheet_empty(ps->s, r, c) || sheet_kind(ps->s, r, c) == SHEET_KIND_TEXT) continue;
								z_fix_t v;
								if (!cell_value(ps, r, c, &v)) return false;
								npv += fx2d(v) / dpow(npv_f, ++npv_i);
							}
					}
				} else if (argc < ARGS_MAX) {
					argv[argc++] = one;
				} else return fail(ps, SHEET_ERR_NUM);

			} else {

				z_fix_t v;
				if (!parse_expr(ps, &v)) return false;

				if (fn_is_aggregate(f)) acc_add(&a, v);
				else if (f == FN_NPV && argc >= 1) {
					if (!ps->skip) npv += fx2d(v) / dpow(npv_f, ++npv_i);
				} else if (argc < ARGS_MAX) {
					argv[argc].range = false;
					argv[argc++].v = v;
					if (f == FN_NPV) npv_f = 1.0 + fx2d(v);
				} else return fail(ps, SHEET_ERR_NUM);

			}

			skipws(ps);

			if (*ps->p == ',') { ps->p++; continue; }
			if (*ps->p == ')') { ps->p++; break; }

			return fail(ps, SHEET_ERR_SYNTAX);

		}

	}

	if (ps->skip) { *out = 0; return true; }

	if (a.bad) return fail(ps, SHEET_ERR_NUM);

	// Value arguments only, from here, except for the lookups.
	z_fix_t v0 = argc > 0 ? argv[0].v : 0, v1 = argc > 1 ? argv[1].v : 0;
	z_fix_t v2 = argc > 2 ? argv[2].v : 0, v3 = argc > 3 ? argv[3].v : 0;
	if (f != FN_VLOOKUP && f != FN_INDEX)
		for (int i = 0; i < argc; i++)
			if (argv[i].range) return fail(ps, SHEET_ERR_NUM);

	z_fix_t one = z_fix_scale(SHEET_DP);

	switch (f) {

		case FN_SUM:
			*out = a.sum;
			return true;

		case FN_AVG:
			// An average of nothing is not zero, it is a question with
			// no answer -- reporting it as 0 would put a plausible
			// number in a cell that has none.
			if (!a.count) return fail(ps, SHEET_ERR_DIV0);
			if (!fx_div(a.sum, (z_fix_t)a.count * one, out)) return fail(ps, SHEET_ERR_NUM);
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
			*out = v0 < 0 ? -v0 : v0;
			return true;

		case FN_INT: {
			// Toward negative infinity, matching every spreadsheet
			// there has ever been: INT(-1.5) is -2, not -1. C's
			// division truncates toward zero, so the negative case
			// needs the extra step.
			if (argc != 1) return fail(ps, SHEET_ERR_NUM);
			z_fix_t q = v0 / one;
			if (v0 < 0 && (v0 % one) != 0) q--;
			*out = q * one;
			return true;
		}

		case FN_ROUND: {
			if (argc != 2) return fail(ps, SHEET_ERR_NUM);
			int places = (int)(v1 / one);
			if (places < 0) places = 0;
			if (places > SHEET_DP) places = SHEET_DP;
			z_fix_t unit = z_fix_scale(SHEET_DP - places);
			z_fix_t half = unit / 2;
			if (v0 >= 0) *out = ((v0 + half) / unit) * unit;
			else         *out = -(((-v0 + half) / unit) * unit);
			return true;
		}

		case FN_MOD: {
			// The sign of the divisor, as spreadsheets have it:
			// MOD(-1, 3) is 2.
			if (argc != 2) return fail(ps, SHEET_ERR_NUM);
			if (!v1) return fail(ps, SHEET_ERR_DIV0);
			z_fix_t m = v0 % v1;
			if (m && ((m < 0) != (v1 < 0))) m += v1;
			*out = m;
			return true;
		}

		case FN_SQRT: {
			if (argc != 1 || v0 < 0) return fail(ps, SHEET_ERR_NUM);
			// sqrt(v / 10^4) * 10^4 = sqrt(v * 10^4)
			if (v0 > INT64_MAX / one) return fail(ps, SHEET_ERR_NUM);
			uint64_t x = (uint64_t)v0 * (uint64_t)one, r = 0, bit = 1ull << 62;
			while (bit > x) bit >>= 2;
			while (bit) {
				if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
				else r >>= 1;
				bit >>= 2;
			}
			if (x > r) r++;			// to nearest: the remainder is past r + 1/2
			*out = (z_fix_t)r;
			return true;
		}

		case FN_POWER: {
			if (argc != 2) return fail(ps, SHEET_ERR_NUM);
			if (!is_int(v1)) return fail(ps, SHEET_ERR_NUM);
			if (v0 == 0 && v1 < 0) return fail(ps, SHEET_ERR_DIV0);
			if (!d2fx(dpow(fx2d(v0), to_int(v1)), out)) return fail(ps, SHEET_ERR_NUM);
			return true;
		}

		case FN_AND: case FN_OR: {
			if (argc < 1) return fail(ps, SHEET_ERR_NUM);
			bool r = (f == FN_AND);
			for (int i = 0; i < argc; i++) {
				if (f == FN_AND) r = r && truth(argv[i].v);
				else r = r || truth(argv[i].v);
			}
			*out = boolv(r);
			return true;
		}

		case FN_NOT:
			if (argc != 1) return fail(ps, SHEET_ERR_NUM);
			*out = boolv(!truth(v0));
			return true;

		case FN_PMT: case FN_PV: case FN_FV: {
			// PMT(rate, periods, pv [, fv])   PV(rate, periods, pmt [, fv])
			// FV(rate, periods, pmt [, pv]) -- the usual signs: money paid
			// out is negative.
			if (argc < 3 || argc > 4 || !is_int(v1)) return fail(ps, SHEET_ERR_NUM);
			double r = fx2d(v0), n = (double)to_int(v1), x = fx2d(v2), y = fx2d(v3), res;
			if (r == 0) {
				if (f == FN_PMT) { if (n == 0) return fail(ps, SHEET_ERR_DIV0); res = -(x + y) / n; }
				else res = -(y + x * n);
			} else {
				double g = dpow(1.0 + r, to_int(v1)), af = (g - 1.0) / r;
				if (f == FN_PMT) { if (af == 0) return fail(ps, SHEET_ERR_DIV0); res = -(x * g + y) / af; }
				else if (f == FN_PV) res = -(x * af + y) / g;
				else res = -(x * af + y * g);
			}
			if (!d2fx(res, out)) return fail(ps, SHEET_ERR_NUM);
			return true;
		}

		case FN_NPV:
			if (argc < 1) return fail(ps, SHEET_ERR_NUM);
			if (!d2fx(npv, out)) return fail(ps, SHEET_ERR_NUM);
			return true;

		case FN_VLOOKUP: {
			// VLOOKUP(value, range, column [, exact]): the row whose first
			// cell is the value (exact), or the largest not above it
			// (otherwise -- the first column sorted ascending, as 1-2-3
			// and every spreadsheet since have it).
			if (argc < 3 || argv[0].range || !argv[1].range || argv[2].range) return fail(ps, SHEET_ERR_NUM);
			bool exact = argc > 3 && !argv[3].range && v3 == 0;
			arg_t *g = &argv[1];
			int col = to_int(v2) - 1;
			if (col < 0 || g->c0 + col > g->c1) return fail(ps, SHEET_ERR_REF);
			int best = -1;
			z_fix_t bestv = 0;
			for (int r = g->r0; r <= g->r1; r++) {
				if (sheet_empty(ps->s, r, g->c0) || sheet_kind(ps->s, r, g->c0) == SHEET_KIND_TEXT) continue;
				z_fix_t k;
				if (!cell_value(ps, r, g->c0, &k)) return false;
				if (k == v0) { best = r; break; }
				if (!exact && k < v0 && (best < 0 || k > bestv)) { best = r; bestv = k; }
			}
			if (best < 0) return fail(ps, SHEET_ERR_NUM);
			return cell_value(ps, best, g->c0 + col, out);
		}

		case FN_INDEX: {
			// INDEX(range, row [, column]), from 1.
			if (argc < 2 || !argv[0].range || argv[1].range) return fail(ps, SHEET_ERR_NUM);
			arg_t *g = &argv[0];
			int r = g->r0 + to_int(v1) - 1, c = g->c0 + (argc > 2 ? to_int(v2) - 1 : 0);
			if (r < g->r0 || r > g->r1 || c < g->c0 || c > g->c1) return fail(ps, SHEET_ERR_REF);
			return cell_value(ps, r, c, out);
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

	// A reference a row or column deletion took away (sheet_rewrite()).
	if (!strncmp(ps->p, "#REF!", 5)) {
		ps->p += 5;
		return soft(ps, SHEET_ERR_REF, out);
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

		// TRUE and FALSE, as 1 and 0
		if (name_eq(start, (int)(q - start), "TRUE")) { ps->p = q; *out = boolv(true); return true; }
		if (name_eq(start, (int)(q - start), "FALSE")) { ps->p = q; *out = 0; return true; }

		int row, col;
		const char *end = NULL;

		if (!sheet_parse_ref(start, &row, &col, &end))
			return fail(ps, SHEET_ERR_REF);

		ps->p = end;

		if (ps->skip) { *out = 0; return true; }

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

// x ^ n, integer n -- above * and /, as everywhere.
static bool parse_power(pstate_t *ps, z_fix_t *out) {

	if (!parse_unary(ps, out)) return false;

	for (;;) {

		skipws(ps);
		if (*ps->p != '^') return true;
		ps->p++;

		z_fix_t e;
		if (!parse_unary(ps, &e)) return false;
		if (ps->skip) { *out = 0; continue; }
		if (!is_int(e)) return fail(ps, SHEET_ERR_NUM);
		if (*out == 0 && e < 0) return fail(ps, SHEET_ERR_DIV0);
		if (!d2fx(dpow(fx2d(*out), to_int(e)), out)) return fail(ps, SHEET_ERR_NUM);

	}

}

static bool parse_term(pstate_t *ps, z_fix_t *out) {

	if (!parse_power(ps, out)) return false;

	for (;;) {

		skipws(ps);

		char op = *ps->p;
		if (op != '*' && op != '/') return true;

		ps->p++;

		z_fix_t rhs;
		if (!parse_power(ps, &rhs)) return false;

		if (op == '*') {
			if (!fx_mul(*out, rhs, out))
				return soft(ps, SHEET_ERR_NUM, out);
		} else {
			if (rhs == 0) return soft(ps, SHEET_ERR_DIV0, out);
			if (!fx_div(*out, rhs, out))
				return soft(ps, SHEET_ERR_NUM, out);
		}

	}

}

static bool parse_sum(pstate_t *ps, z_fix_t *out) {

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

		if (!ok) return soft(ps, SHEET_ERR_NUM, out);

	}

}

// Comparisons, lowest of all: = <> < > <= >=, giving 1 or 0.
static bool parse_expr(pstate_t *ps, z_fix_t *out) {

	if (!parse_sum(ps, out)) return false;

	for (;;) {

		skipws(ps);

		const char *p = ps->p;
		int op;		// 1 =, 2 <>, 3 <, 4 >, 5 <=, 6 >=
		if (p[0] == '=') op = 1, ps->p += 1;
		else if (p[0] == '<' && p[1] == '>') op = 2, ps->p += 2;
		else if (p[0] == '<' && p[1] == '=') op = 5, ps->p += 2;
		else if (p[0] == '>' && p[1] == '=') op = 6, ps->p += 2;
		else if (p[0] == '<') op = 3, ps->p += 1;
		else if (p[0] == '>') op = 4, ps->p += 1;
		else return true;

		z_fix_t rhs;
		if (!parse_sum(ps, &rhs)) return false;

		z_fix_t l = *out;
		bool r = op == 1 ? l == rhs : op == 2 ? l != rhs : op == 3 ? l < rhs :
			op == 4 ? l > rhs : op == 5 ? l <= rhs : l >= rhs;
		*out = boolv(r);

	}

}

// Evaluation state, tagged with the recalculation it belongs to: the
// low two bits are ST_*, the rest s->gen. A state from an older
// generation reads as ST_IDLE, which is what invalidate() achieves
// without visiting every cell.
static int st_get(const sheet_t *s, const sheet_cell_t *c) {
	return (c->state >> 2) == s->gen ? (c->state & 3) : ST_IDLE;
}

static void st_set(const sheet_t *s, sheet_cell_t *c, int st) {
	c->state = (uint8_t)((s->gen << 2) | st);
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

	int st = st_get(s, c);

	// A formula that reaches a cell already being evaluated has, by
	// definition, come back to where it started. One comparison, no
	// dependency graph.
	if (st == ST_BUSY) return SHEET_ERR_CIRC;

	if (st == ST_DONE) {
		*out = c->val;
		return (sheet_err_t)c->err;
	}

	if (s->depth >= SHEET_MAX_DEPTH) {
		// NOT cached: a depth failure is a property of the path taken
		// to get here, not of this cell, so caching it would make an
		// unrelated later evaluation wrong.
		return SHEET_ERR_DEPTH;
	}

	st_set(s, c, ST_BUSY);
	s->depth++;

	pstate_t ps;
	ps.s = s;
	ps.p = &s->arena[c->off] + 1;		// past the '='
	ps.err = SHEET_OK;
	ps.skip = 0;

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
			st_set(s, c, ST_IDLE);
			return e;
		}

		c->err = (uint8_t)e;
		c->val = 0;
		st_set(s, c, ST_DONE);

		return e;

	}

	c->err = SHEET_OK;
	c->val = v;
	st_set(s, c, ST_DONE);

	*out = v;
	return SHEET_OK;

}

sheet_err_t sheet_value(sheet_t *s, int row, int col, z_fix_t *out) {
	return eval_cell(s, row, col, out);
}


// -- the chart --

const char *sheet_chart_name(int kind) {
	static const char *const n[] = { "bar", "stack", "line", "pie" };
	return (kind >= 0 && kind < SHEET_CHART_KINDS) ? n[kind] : "bar";
}

// -- number formats --

bool sheet_parse_format(const char *p, int *kind, int *dec) {

	while (*p == ' ') p++;

	int k;
	switch (*p) {
		case 'G': case 'g': *kind = SHEET_FMT_GENERAL; *dec = 0; return !p[1] || p[1] == ' ';
		case 'F': case 'f': k = SHEET_FMT_FIXED; break;
		case ',':           k = SHEET_FMT_COMMA; break;
		case '%':           k = SHEET_FMT_PERCENT; break;
		case '$':           k = SHEET_FMT_MONEY; break;
		default: return false;
	}

	p++;
	int d = 2;
	if (*p >= '0' && *p <= '9') { d = *p - '0'; p++; }
	if (d > SHEET_DP) d = SHEET_DP;
	if (*p && *p != ' ') return false;

	*kind = k;
	*dec = d;
	return true;

}

int sheet_format_name(int kind, int dec, char *out, int cap) {

	if (cap < 3) { if (cap) out[0] = 0; return 0; }

	static const char lead[] = "GF,%$";
	out[0] = lead[kind >= 0 && kind <= SHEET_FMT_MONEY ? kind : 0];

	if (kind == SHEET_FMT_GENERAL) { out[1] = 0; return 1; }

	out[1] = (char)('0' + dec);
	out[2] = 0;
	return 2;

}

int sheet_format_at(const sheet_t *s, int row, int col, int *dec) {

	for (int i = s->nfmts - 1; i >= 0; i--) {
		const sheet_fmt_t *f = &s->fmts[i];
		if (row >= f->r0 && row <= f->r1 && col >= f->c0 && col <= f->c1) {
			if (dec) *dec = f->dec;
			return f->kind;
		}
	}

	if (dec) *dec = 0;
	return SHEET_FMT_GENERAL;

}

bool sheet_set_format(sheet_t *s, int r0, int c0, int r1, int c1, int kind, int dec) {

	if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
	if (c0 > c1) { int t = c0; c0 = c1; c1 = t; }
	if (r0 < 0) r0 = 0;
	if (c0 < 0) c0 = 0;
	if (r1 >= SHEET_ROWS) r1 = SHEET_ROWS - 1;
	if (c1 >= SHEET_COLS) c1 = SHEET_COLS - 1;
	if (dec < 0) dec = 0;
	if (dec > SHEET_DP) dec = SHEET_DP;

	// Ranges the new one covers entirely no longer show anywhere.
	int n = 0;
	for (int i = 0; i < s->nfmts; i++) {
		sheet_fmt_t *f = &s->fmts[i];
		if (f->r0 >= r0 && f->r1 <= r1 && f->c0 >= c0 && f->c1 <= c1) continue;
		s->fmts[n++] = *f;
	}
	s->nfmts = n;
	s->modified = true;

	// General on nothing else: nothing to record.
	if (kind == SHEET_FMT_GENERAL) {
		bool under = false;
		for (int i = 0; i < n && !under; i++) {
			sheet_fmt_t *f = &s->fmts[i];
			if (f->r0 <= r1 && f->r1 >= r0 && f->c0 <= c1 && f->c1 >= c0) under = true;
		}
		if (!under) return true;
	}

	if (s->nfmts >= SHEET_MAX_FMTS) return false;

	sheet_fmt_t *f = &s->fmts[s->nfmts++];
	f->r0 = (uint16_t)r0;
	f->r1 = (uint16_t)r1;
	f->c0 = (uint8_t)c0;
	f->c1 = (uint8_t)c1;
	f->kind = (uint8_t)kind;
	f->dec = (uint8_t)dec;
	return true;

}

// `v` as `kind` with `dec` places, into out; its length, or -1 if it
// does not fit `width`.
static int format_num(z_fix_t v, int kind, int dec, char *out, int cap, int width) {

	char digits[40];
	int o = 0;

	if (kind == SHEET_FMT_PERCENT) {
		z_fix_t p;
		if (!fx_mul(v, (z_fix_t)100 * z_fix_scale(SHEET_DP), &p)) return -1;
		v = p;
	}

	bool neg = v < 0;
	int n = z_fix_format(neg ? -v : v, SHEET_DP, dec, digits, sizeof digits);
	if (n <= 0) return -1;

	// Rounded to nothing: no minus sign on "0.00".
	bool zero = true;
	for (int i = 0; i < n; i++) if (digits[i] >= '1' && digits[i] <= '9') zero = false;
	if (zero) neg = false;

	char tmp[64];
	if (neg) tmp[o++] = '-';
	if (kind == SHEET_FMT_MONEY) tmp[o++] = '$';

	int intlen = 0;
	while (intlen < n && digits[intlen] != '.') intlen++;

	for (int i = 0; i < n; i++) {
		tmp[o++] = digits[i];
		bool group = (kind == SHEET_FMT_COMMA || kind == SHEET_FMT_MONEY);
		int left = intlen - 1 - i;
		if (group && i < intlen && left > 0 && left % 3 == 0) tmp[o++] = ',';
	}
	if (kind == SHEET_FMT_PERCENT) tmp[o++] = '%';

	if (o > width || o >= cap) return -1;
	memcpy(out, tmp, (size_t)o);
	out[o] = 0;
	return o;

}

// -- moving formulas --

// A reference at p: [$]col[$]row, one letter. Fills its parts; the
// length, or 0.
static int ref_at(const char *p, int *row, int *col, bool *acol, bool *arow) {

	const char *q = p;
	*acol = *arow = false;
	if (*q == '$') { *acol = true; q++; }
	char c = *q;
	if (c >= 'a' && c <= 'z') c = (char)(c - 32);
	if (c < 'A' || c > 'Z') return 0;
	q++;
	if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) return 0;
	if (*q == '$') { *arow = true; q++; }
	int r = 0, nd = 0;
	while (*q >= '0' && *q <= '9') { r = r * 10 + (*q - '0'); q++; if (++nd > 4) return 0; }
	if (!nd || !r) return 0;
	// not part of a longer name ("A1B", "A1(")
	if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || *q == '(' || *q == '_') return 0;
	*row = r - 1;
	*col = c - 'A';
	return (int)(q - p);

}

static int put_ref(char *o, int n, int cap, int row, int col, bool acol, bool arow) {

	char b[12];
	int k = 0;
	if (acol) b[k++] = '$';
	b[k++] = (char)('A' + col);
	if (arow) b[k++] = '$';
	char t[6];
	int tn = 0, r = row + 1;
	while (r && tn < 5) { t[tn++] = (char)('0' + r % 10); r /= 10; }
	while (tn) b[k++] = t[--tn];
	for (int i = 0; i < k; i++) { if (n >= cap - 1) return -1; o[n++] = b[i]; }
	return n;

}

static int put_str(char *o, int n, int cap, const char *s) {
	while (*s) { if (n >= cap - 1) return -1; o[n++] = *s++; }
	return n;
}

// One coordinate through a structural change: the new value, or -1
// when deleted. `lo`/`hi`: as the start or the end of a range, which
// a deletion clips rather than kills.
static int move_coord(int v, sheet_move_t how, int at, int n, int end) {

	bool ins = (how == SHEET_MOVE_INS_ROWS || how == SHEET_MOVE_INS_COLS);
	if (ins) return v >= at ? v + n : v;

	if (v < at) return v;
	if (v >= at + n) return v - n;
	// inside what went
	if (end == 0) return -1;		// a lone reference
	return end < 0 ? at : at - 1;	// range start: what follows; range end: what precedes

}

bool sheet_rewrite(const char *src, char *out, int cap, sheet_move_t how, int a, int b) {

	if (cap > SHEET_SRC_MAX) cap = SHEET_SRC_MAX;
	int n = 0;
	const char *p = src;
	bool rows = (how == SHEET_MOVE_INS_ROWS || how == SHEET_MOVE_DEL_ROWS);
	int limit = rows ? SHEET_ROWS : SHEET_COLS;

	while (*p) {

		char ch = *p;

		// a name (function) and anything glued to it is copied as it is
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '$') {

			int r0, c0, r1, c1;
			bool ac0, ar0, ac1, ar1;
			int k0 = ref_at(p, &r0, &c0, &ac0, &ar0);

			if (!k0) {
				// copy the whole word, so "LOG10" is not split
				while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
					(*p >= '0' && *p <= '9') || *p == '$' || *p == '_' || *p == '.') {
					if (n >= cap - 1) return false;
					out[n++] = *p++;
				}
				continue;
			}

			int k1 = 0;
			if (p[k0] == ':') k1 = ref_at(p + k0 + 1, &r1, &c1, &ac1, &ar1);

			bool bad = false;

			if (how == SHEET_MOVE_COPY) {
				if (!ar0) r0 += a;
				if (!ac0) c0 += b;
				if (r0 < 0 || r0 >= SHEET_ROWS || c0 < 0 || c0 >= SHEET_COLS) bad = true;
				if (k1) {
					if (!ar1) r1 += a;
					if (!ac1) c1 += b;
					if (r1 < 0 || r1 >= SHEET_ROWS || c1 < 0 || c1 >= SHEET_COLS) bad = true;
				}
			} else {
				int *v0 = rows ? &r0 : &c0, *v1 = rows ? &r1 : &c1;
				if (k1) {
					bool fwd = *v0 <= *v1;
					int lo = fwd ? *v0 : *v1, hi = fwd ? *v1 : *v0;
					int nlo = move_coord(lo, how, a, b, -1), nhi = move_coord(hi, how, a, b, 1);
					if (nhi < nlo || nlo >= limit || nhi >= limit) bad = true;
					*v0 = fwd ? nlo : nhi;
					*v1 = fwd ? nhi : nlo;
				} else {
					*v0 = move_coord(*v0, how, a, b, 0);
					if (*v0 < 0 || *v0 >= limit) bad = true;
				}
			}

			if (bad) n = put_str(out, n, cap, "#REF!");
			else {
				n = put_ref(out, n, cap, r0, c0, ac0, ar0);
				if (k1 && n >= 0) {
					if (n >= cap - 1) return false;
					out[n++] = ':';
					n = put_ref(out, n, cap, r1, c1, ac1, ar1);
				}
			}
			if (n < 0) return false;
			p += k0 + (k1 ? 1 + k1 : 0);
			continue;

		}

		if (ch >= '0' && ch <= '9') {
			// a number: copied whole, so "1E5" or "2.5" are not refs
			while ((*p >= '0' && *p <= '9') || *p == '.') {
				if (n >= cap - 1) return false;
				out[n++] = *p++;
			}
			if ((*p == 'E' || *p == 'e') && (p[1] == '+' || p[1] == '-' || (p[1] >= '0' && p[1] <= '9'))) {
				if (n >= cap - 2) return false;
				out[n++] = *p++;
				out[n++] = *p++;
				while (*p >= '0' && *p <= '9') { if (n >= cap - 1) return false; out[n++] = *p++; }
			}
			continue;
		}

		if (n >= cap - 1) return false;
		out[n++] = *p++;

	}

	out[n] = 0;
	return true;

}

// A formula's text after `how`, into the arena; the cell's text is
// replaced. False if the sheet is full.
static bool rewrite_cell(sheet_t *s, int i, sheet_move_t how, int a, int b) {

	sheet_cell_t *c = &s->cells[i];
	if (c->kind != SHEET_KIND_FORMULA) return true;

	char tmp[SHEET_SRC_MAX];
	if (!sheet_rewrite(&s->arena[c->off], tmp, sizeof tmp, how, a, b)) {
		// would not fit: say so in the cell rather than lose it quietly
		memcpy(tmp, "=#REF!", 7);
	}
	int len = (int)strlen(tmp);
	if (len == c->len && !memcmp(tmp, &s->arena[c->off], (size_t)len)) return true;

	int off = arena_put(s, tmp, len);
	if (off < 0) return false;
	c = &s->cells[i];
	s->arena_live -= c->len + 1;
	c->off = (uint16_t)off;
	c->len = (uint8_t)len;
	return true;

}

// Formats through a structural change.
static void move_formats(sheet_t *s, bool rows, bool ins, int at, int n) {

	int k = 0;
	for (int i = 0; i < s->nfmts; i++) {
		sheet_fmt_t f = s->fmts[i];
		int lo = rows ? f.r0 : f.c0, hi = rows ? f.r1 : f.c1;
		int limit = rows ? SHEET_ROWS : SHEET_COLS;
		sheet_move_t how = rows ? (ins ? SHEET_MOVE_INS_ROWS : SHEET_MOVE_DEL_ROWS)
			: (ins ? SHEET_MOVE_INS_COLS : SHEET_MOVE_DEL_COLS);
		int nlo = move_coord(lo, how, at, n, -1), nhi = move_coord(hi, how, at, n, 1);
		if (nhi < nlo || nlo >= limit) continue;
		if (nhi >= limit) nhi = limit - 1;
		if (rows) { f.r0 = (uint16_t)nlo; f.r1 = (uint16_t)nhi; }
		else { f.c0 = (uint8_t)nlo; f.c1 = (uint8_t)nhi; }
		s->fmts[k++] = f;
	}
	s->nfmts = k;

}

static sheet_err_t structural(sheet_t *s, bool rows, bool ins, int at, int n) {

	int limit = rows ? SHEET_ROWS : SHEET_COLS;
	if (n <= 0 || at < 0 || at >= limit) return SHEET_OK;
	if (at + n > limit) n = limit - at;

	sheet_move_t how = rows ? (ins ? SHEET_MOVE_INS_ROWS : SHEET_MOVE_DEL_ROWS)
		: (ins ? SHEET_MOVE_INS_COLS : SHEET_MOVE_DEL_COLS);

	// Inserting must not push anything off the edge.
	if (ins) {
		for (int i = 0; i < s->ncells; i++) {
			int v = rows ? s->cells[i].key / SHEET_COLS : s->cells[i].key % SHEET_COLS;
			if (v >= limit - n) return SHEET_ERR_FULL;
		}
	}

	// Keys: the order of what remains is unchanged by moving whole rows
	// or columns, so the array stays sorted and is compacted in place.
	int k = 0;
	for (int i = 0; i < s->ncells; i++) {
		sheet_cell_t c = s->cells[i];
		int r = c.key / SHEET_COLS, col = c.key % SHEET_COLS;
		int v = rows ? r : col;
		int nv = move_coord(v, how, at, n, 0);
		if (nv < 0) { s->arena_live -= c.len + 1; continue; }
		if (rows) r = nv; else col = nv;
		c.key = mkkey(r, col);
		s->cells[k++] = c;
	}
	s->ncells = k;

	// Column moves can reorder within a row? No: every column at or past
	// `at` moves by the same amount, so order within a row holds too.

	// Every formula's references.
	for (int i = 0; i < s->ncells; i++)
		if (!rewrite_cell(s, i, how, at, n)) return SHEET_ERR_FULL;

	if (!rows) {
		// widths move with their columns
		if (ins) {
			for (int c = SHEET_COLS - 1; c >= at + n; c--) s->colw[c] = s->colw[c - n];
			for (int c = at; c < at + n; c++) s->colw[c] = SHEET_COLW_DEF;
		} else {
			for (int c = at; c < SHEET_COLS; c++)
				s->colw[c] = (c + n < SHEET_COLS) ? s->colw[c + n] : SHEET_COLW_DEF;
		}
	}

	move_formats(s, rows, ins, at, n);

	// the chart's range, the same way
	if (s->chart.set) {
		sheet_chart_t *g = &s->chart;
		int lo = rows ? g->r0 : g->c0, hi = rows ? g->r1 : g->c1;
		int nlo = move_coord(lo, how, at, n, -1), nhi = move_coord(hi, how, at, n, 1);
		if (nhi < nlo || nlo >= limit) g->set = false;
		else {
			if (nhi >= limit) nhi = limit - 1;
			if (rows) { g->r0 = (uint16_t)nlo; g->r1 = (uint16_t)nhi; }
			else { g->c0 = (uint8_t)nlo; g->c1 = (uint8_t)nhi; }
		}
	}

	s->modified = true;
	invalidate(s);
	return SHEET_OK;

}

sheet_err_t sheet_insert_rows(sheet_t *s, int at, int n) { return structural(s, true, true, at, n); }
sheet_err_t sheet_delete_rows(sheet_t *s, int at, int n) { return structural(s, true, false, at, n); }
sheet_err_t sheet_insert_cols(sheet_t *s, int at, int n) { return structural(s, false, true, at, n); }
sheet_err_t sheet_delete_cols(sheet_t *s, int at, int n) { return structural(s, false, false, at, n); }

// -- ranges as bytes --

static void norm(int *r0, int *c0, int *r1, int *c1) {
	if (*r0 > *r1) { int t = *r0; *r0 = *r1; *r1 = t; }
	if (*c0 > *c1) { int t = *c0; *c0 = *c1; *c1 = t; }
}

int sheet_range_size(const sheet_t *s, int r0, int c0, int r1, int c1) {

	norm(&r0, &c0, &r1, &c1);
	int n = 0;
	for (int r = r0; r <= r1; r++) {
		int i = lower_bound(s, mkkey(r, c0));
		uint16_t last = mkkey(r, c1);
		for (; i < s->ncells && s->cells[i].key <= last; i++) n += 5 + s->cells[i].len;
	}
	return n;

}

int sheet_range_save(const sheet_t *s, int r0, int c0, int r1, int c1, void *buf, int cap) {

	norm(&r0, &c0, &r1, &c1);
	uint8_t *o = buf;
	int n = 0;
	for (int r = r0; r <= r1; r++) {
		int i = lower_bound(s, mkkey(r, c0));
		uint16_t last = mkkey(r, c1);
		for (; i < s->ncells && s->cells[i].key <= last; i++) {
			const sheet_cell_t *c = &s->cells[i];
			if (n + 5 + c->len > cap) return -1;
			int dr = r - r0, dc = c->key % SHEET_COLS - c0;
			o[n++] = (uint8_t)dr;
			o[n++] = (uint8_t)(dr >> 8);
			o[n++] = (uint8_t)dc;
			o[n++] = 0;
			o[n++] = c->len;
			memcpy(o + n, &s->arena[c->off], c->len);
			n += c->len;
		}
	}
	return n;

}

sheet_err_t sheet_range_load(sheet_t *s, int r0, int c0, int h, int w,
	bool clear, const void *buf, int len, bool adjust, int from_r, int from_c) {

	if (clear) sheet_clear_range(s, r0, c0, r0 + h - 1, c0 + w - 1);

	const uint8_t *p = buf;
	sheet_err_t first = SHEET_OK;
	int i = 0;
	while (i + 5 <= len) {
		int dr = p[i] | (p[i + 1] << 8), dc = p[i + 2], l = p[i + 4];
		i += 5;
		if (i + l > len) break;
		char t[SHEET_SRC_MAX];
		memcpy(t, p + i, (size_t)l);
		t[l] = 0;
		i += l;
		int r = r0 + dr, c = c0 + dc;
		if (!in_grid(r, c)) continue;
		if (adjust && t[0] == '=') {
			char m[SHEET_SRC_MAX];
			if (sheet_rewrite(t, m, sizeof m, SHEET_MOVE_COPY, r - (from_r + dr), c - (from_c + dc)))
				memcpy(t, m, strlen(m) + 1);
		}
		sheet_err_t e = sheet_set(s, r, c, t);
		if (e != SHEET_OK && first == SHEET_OK) first = e;
	}
	return first;

}

// -- sorting --

// How a row's key compares: numbers (by value), then text (by its
// letters, ignoring case), then empty.
typedef struct {
	uint8_t		cls;		// 0 number, 1 text, 2 empty
	z_fix_t		v;
	const char	*t;
} skey_t;

static int key_cmp(const skey_t *a, const skey_t *b) {
	if (a->cls != b->cls) return a->cls < b->cls ? -1 : 1;
	if (a->cls == 0) return a->v < b->v ? -1 : a->v > b->v ? 1 : 0;
	if (a->cls == 1) {
		const char *x = a->t, *y = b->t;
		for (;; x++, y++) {
			char cx = *x, cy = *y;
			if (cx >= 'a' && cx <= 'z') cx = (char)(cx - 32);
			if (cy >= 'a' && cy <= 'z') cy = (char)(cy - 32);
			if (cx != cy) return (unsigned char)cx < (unsigned char)cy ? -1 : 1;
			if (!cx) return 0;
		}
	}
	return 0;
}

bool sheet_sort(sheet_t *s, int r0, int c0, int r1, int c1, int key,
	bool desc, void *tmp, int tmpcap) {

	norm(&r0, &c0, &r1, &c1);
	int h = r1 - r0 + 1;
	if (h < 2 || key < c0 || key > c1) return true;

	// Scratch: the keys, the order and its inverse (2 bytes a row each),
	// then the range's text -- all of it the caller's, so a sort costs
	// nothing when none is running.
	int need = sheet_range_size(s, r0, c0, r1, c1);
	int key_bytes = h * (int)sizeof(skey_t);
	int ord_bytes = (h * 4 + 7) & ~7;
	if (key_bytes + ord_bytes + need > tmpcap) return false;
	skey_t *keys = tmp;
	uint16_t *ord = (uint16_t *)((uint8_t *)tmp + key_bytes);
	uint16_t *dest = ord + h;
	uint8_t *saved = (uint8_t *)tmp + key_bytes + ord_bytes;
	tmpcap -= key_bytes + ord_bytes;

	// The keys, evaluated before anything moves.
	for (int r = r0; r <= r1; r++) {
		skey_t *k = &keys[r - r0];
		if (sheet_empty(s, r, key)) { k->cls = 2; continue; }
		if (sheet_kind(s, r, key) == SHEET_KIND_TEXT) {
			k->cls = 1;
			k->t = sheet_src(s, r, key);
			if (*k->t == '\'') k->t++;
			continue;
		}
		k->cls = 0;
		if (sheet_value(s, r, key, &k->v) != SHEET_OK) k->cls = 1, k->t = "";
	}

	// Stable insertion sort of row indices: at most 999 rows, and a
	// sort is a deliberate act, not something done per keystroke.
	for (int i = 0; i < h; i++) ord[i] = (uint16_t)i;
	for (int i = 1; i < h; i++) {
		uint16_t t = ord[i];
		int j = i;
		while (j > 0) {
			int c = key_cmp(&keys[ord[j - 1]], &keys[t]);
			// descending reverses numbers and text, but empty stays last
			if (desc && keys[ord[j - 1]].cls != 2 && keys[t].cls != 2 &&
				keys[ord[j - 1]].cls == keys[t].cls) c = -c;
			if (c <= 0) break;
			ord[j] = ord[j - 1];
			j--;
		}
		ord[j] = t;
	}

	// The text is saved now (the keys point into the arena, which the
	// moves below may compact), then written back row by row.
	int len = sheet_range_save(s, r0, c0, r1, c1, saved, tmpcap);
	if (len < 0) return false;

	// where each old row goes
	for (int i = 0; i < h; i++) dest[ord[i]] = (uint16_t)i;

	sheet_clear_range(s, r0, c0, r1, c1);

	int i = 0;
	while (i + 5 <= len) {
		int dr = saved[i] | (saved[i + 1] << 8), dc = saved[i + 2], l = saved[i + 4];
		i += 5;
		char t[SHEET_SRC_MAX];
		memcpy(t, saved + i, (size_t)l);
		t[l] = 0;
		i += l;
		int nr = r0 + dest[dr];
		if (t[0] == '=') {
			char m[SHEET_SRC_MAX];
			if (sheet_rewrite(t, m, sizeof m, SHEET_MOVE_COPY, nr - (r0 + dr), 0))
				memcpy(t, m, strlen(m) + 1);
		}
		if (sheet_set(s, nr, c0 + dc, t) != SHEET_OK) return false;
	}
	return true;

}

// -- the whole document --

bool sheet_range_stats(sheet_t *s, int r0, int c0, int r1, int c1,
	z_fix_t *sum, int *count) {
	acc_t a;
	acc_init(&a);
	if (acc_range(s, &a, r0, c0, r1, c1) != SHEET_OK || a.bad) return false;
	*sum = a.sum;
	*count = a.count;
	return true;
}

int sheet_snapshot_size(const sheet_t *s) {
	return 8 + SHEET_COLS + s->nfmts * (int)sizeof(sheet_fmt_t) + (int)sizeof(sheet_chart_t) +
		sheet_range_size(s, 0, 0, SHEET_ROWS - 1, SHEET_COLS - 1);
}

int sheet_snapshot(const sheet_t *s, void *buf, int cap) {

	uint8_t *o = buf;
	int fb = s->nfmts * (int)sizeof(sheet_fmt_t) + (int)sizeof(sheet_chart_t);
	if (cap < 8 + SHEET_COLS + fb) return -1;
	memcpy(o, "ZSS1", 4);
	o[4] = (uint8_t)s->nfmts;
	o[5] = o[6] = o[7] = 0;
	memcpy(o + 8, s->colw, SHEET_COLS);
	memcpy(o + 8 + SHEET_COLS, s->fmts, (size_t)fb - sizeof(sheet_chart_t));
	memcpy(o + 8 + SHEET_COLS + fb - (int)sizeof(sheet_chart_t), &s->chart, sizeof(sheet_chart_t));
	int h = 8 + SHEET_COLS + fb;
	int n = sheet_range_save(s, 0, 0, SHEET_ROWS - 1, SHEET_COLS - 1, o + h, cap - h);
	return n < 0 ? -1 : h + n;

}

bool sheet_restore(sheet_t *s, const void *buf, int len) {

	const uint8_t *p = buf;
	if (len < 8 + SHEET_COLS || memcmp(p, "ZSS1", 4)) return false;
	int nf = p[4];
	int fb = nf * (int)sizeof(sheet_fmt_t) + (int)sizeof(sheet_chart_t);
	if (nf > SHEET_MAX_FMTS || len < 8 + SHEET_COLS + fb) return false;

	bool modified = s->modified;
	sheet_init(s);
	memcpy(s->colw, p + 8, SHEET_COLS);
	memcpy(s->fmts, p + 8 + SHEET_COLS, (size_t)nf * sizeof(sheet_fmt_t));
	memcpy(&s->chart, p + 8 + SHEET_COLS + fb - (int)sizeof(sheet_chart_t), sizeof(sheet_chart_t));
	s->nfmts = nf;

	// Straight into the arrays, in key order, as saved: a cell at a
	// time through sheet_set() would be a binary search and a move per
	// cell for no reason.
	int h = 8 + SHEET_COLS + fb, i = h;
	while (i + 5 <= len && s->ncells < SHEET_MAX_CELLS) {
		int r = p[i] | (p[i + 1] << 8), c = p[i + 2], l = p[i + 4];
		i += 5;
		if (i + l > len) break;
		int off = arena_put(s, (const char *)p + i, l);
		i += l;
		if (off < 0) break;
		sheet_cell_t *cell = &s->cells[s->ncells++];
		cell->key = mkkey(r, c);
		cell->off = (uint16_t)off;
		cell->len = (uint8_t)l;
		cell->kind = classify(&s->arena[off], &cell->val);
		cell->err = SHEET_OK;
		cell->state = ST_IDLE;
	}
	invalidate(s);
	s->modified = modified;
	return true;

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

	// A number format, when one applies and fits; else General.
	int dec, kind = sheet_format_at(s, row, col, &dec);
	if (kind != SHEET_FMT_GENERAL) {
		int n = format_num(v, kind, dec, out, cap, width);
		if (n >= 0) return n;
		for (int k = 0; k < width; k++) out[k] = '#';
		out[width] = 0;
		return width;
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

	// Number formats: "!f A1:B9 ,2", in order (later ones win).
	for (int i = 0; i < s->nfmts; i++) {
		const sheet_fmt_t *f = &s->fmts[i];
		char ln[40], a[8], b[8], fm[4];
		sheet_ref_name(f->r0, f->c0, a, sizeof a);
		sheet_ref_name(f->r1, f->c1, b, sizeof b);
		sheet_format_name(f->kind, f->dec, fm, sizeof fm);
		int n = 0;
		for (const char *q = "!f "; *q; q++) ln[n++] = *q;
		for (const char *q = a; *q; q++) ln[n++] = *q;
		if (f->r0 != f->r1 || f->c0 != f->c1) {
			ln[n++] = ':';
			for (const char *q = b; *q; q++) ln[n++] = *q;
		}
		ln[n++] = ' ';
		for (const char *q = fm; *q; q++) ln[n++] = *q;
		ln[n++] = '\n';
		if (!emit(ctx, ln, n)) return false;
	}

	// The chart: "!g bar A1:D9".
	if (s->chart.set) {
		char ln[40], a[8], b[8];
		sheet_ref_name(s->chart.r0, s->chart.c0, a, sizeof a);
		sheet_ref_name(s->chart.r1, s->chart.c1, b, sizeof b);
		int n = 0;
		const char *parts[] = { "!g ", sheet_chart_name(s->chart.kind), " ", a, ":", b, "\n" };
		for (int i = 0; i < 7; i++) for (const char *q = parts[i]; *q; q++) ln[n++] = *q;
		if (!emit(ctx, ln, n)) return false;
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

		// "!g bar A1:D9" -- the chart (0.0.7).
		if ((*p == 'g' || *p == 'G') && (p[1] == ' ' || p[1] == '\t')) {
			p += 2;
			while (*p == ' ') p++;
			int kind = -1;
			for (int k = 0; k < SHEET_CHART_KINDS; k++) {
				const char *nm = sheet_chart_name(k);
				int l = (int)strlen(nm);
				if (!strncmp(p, nm, (size_t)l) && p[l] == ' ') { kind = k; p += l; break; }
			}
			if (kind < 0) return false;
			while (*p == ' ') p++;
			int r0, c0, r1, c1;
			const char *e = NULL;
			if (!sheet_parse_ref(p, &r0, &c0, &e) || *e != ':' || !sheet_parse_ref(e + 1, &r1, &c1, NULL))
				return false;
			s->chart.set = true;
			s->chart.kind = (uint8_t)kind;
			s->chart.r0 = (uint16_t)(r0 < r1 ? r0 : r1);
			s->chart.r1 = (uint16_t)(r0 < r1 ? r1 : r0);
			s->chart.c0 = (uint8_t)(c0 < c1 ? c0 : c1);
			s->chart.c1 = (uint8_t)(c0 < c1 ? c1 : c0);
			return true;
		}

		// "!f A1:B9 ,2" -- a number format on a range (0.0.7).
		if ((*p == 'f' || *p == 'F') && (p[1] == ' ' || p[1] == '\t')) {
			p += 2;
			while (*p == ' ') p++;
			int r0, c0, r1, c1, kind, dec;
			const char *e = NULL;
			if (!sheet_parse_ref(p, &r0, &c0, &e)) return false;
			r1 = r0; c1 = c0;
			if (*e == ':' && !sheet_parse_ref(e + 1, &r1, &c1, &e)) return false;
			if (!sheet_parse_format(e, &kind, &dec)) return false;
			bool mod = s->modified;
			sheet_set_format(s, r0, c0, r1, c1, kind, dec);
			s->modified = mod;
			return true;
		}

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
