/*
 * posix -- regular expressions for sed. See rx.h.
 *
 * The expression is parsed by recursive descent -- bounded by its own
 * nesting, which is short -- and compiled to instructions:
 *
 *   CHAR c   CHARI c   ANY   SET i       one character (CHARI: either case)
 *   BOL   EOL                             a position
 *   SPLIT x y                             try x; if that fails, y
 *   JMP x
 *   SAVE k                                group bound k := here
 *   LSET k / LCHK k x                     a loop's start; out to x if no progress
 *   MATCH
 *
 * A quantifier wraps the code its atom has just emitted, so code is
 * inserted before it and the jump targets after the insertion point
 * move with it (insert()). `e*` is
 *
 *   L1: SPLIT L2 L3
 *   L2: LSET k; e; LCHK k L3; JMP L1
 *   L3:
 *
 * where LCHK ends a loop whose body matched nothing rather than going
 * round forever (`(a*)*`) -- after that one empty pass, as Perl and
 * Python do, so a group in it is set to the empty string. `e+` is `e` followed by a copy of it starred, and
 * `e?` is SPLIT round it.
 *
 * The machine backtracks with an explicit stack: an alternative to try
 * (pc, position), or an undo record for a group bound or loop start it
 * changed. Failing pops undo records until it reaches an alternative.
 */
#include <string.h>
#include <stdio.h>

#include "rx.h"

enum { I_CHAR = 1, I_CHARI, I_ANY, I_SET, I_BOL, I_EOL, I_SPLIT, I_JMP, I_SAVE, I_LSET, I_LCHK, I_MATCH };

#define RX_LOOPS    32
#define UNSET       0xFFFFu

// -- the tokens --

enum { T_END, T_CHAR, T_ANY, T_SET, T_LP, T_RP, T_ALT, T_STAR, T_PLUS, T_QUES };

typedef struct {
	rx_t *r;
	const char *p, *end;
	bool ext, icase;
	int depth;
	const char *err;
	// the token in hand
	int tok;
	uint8_t c;              // T_CHAR
	bool esc;               // ... written with a backslash
	int set;                // T_SET
	bool at_start;          // nothing yet in this branch: ^ is an anchor, * literal
} cp_t;

static void set_add(uint8_t *s, int c) { s[c >> 3] |= (uint8_t)(1u << (c & 7)); }

static bool class_add(uint8_t *s, const char *name, int n) {
	for (int c = 0; c < 256; c++) {
		bool in;
#define IS(w) (n == (int)sizeof(w) - 1 && !memcmp(name, w, (size_t)n))
		if (IS("alpha")) in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		else if (IS("digit")) in = c >= '0' && c <= '9';
		else if (IS("alnum")) in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		else if (IS("upper")) in = c >= 'A' && c <= 'Z';
		else if (IS("lower")) in = c >= 'a' && c <= 'z';
		else if (IS("space")) in = c == ' ' || (c >= '\t' && c <= '\r');
		else if (IS("blank")) in = c == ' ' || c == '\t';
		else if (IS("punct")) in = (c > 32 && c < 127) && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
		else if (IS("xdigit")) in = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		else if (IS("print")) in = c >= 32 && c < 127;
		else if (IS("cntrl")) in = c < 32 || c == 127;
		else return false;
#undef IS
		if (in) set_add(s, c);
	}
	return true;
}

// A bracket expression, p just past the '['.
static bool parse_set(cp_t *c) {
	if (c->r->nsets >= RX_SETS) { c->err = "too many [...] in one expression"; return false; }
	uint8_t *s = c->r->sets[c->r->nsets];
	bool neg = false, first = true;
	memset(s, 0, 32);
	if (c->p < c->end && *c->p == '^') { neg = true; c->p++; }
	for (;;) {
		if (c->p >= c->end) { c->err = "unterminated ["; return false; }
		uint8_t ch = (uint8_t)*c->p;
		if (ch == ']' && !first) { c->p++; break; }
		first = false;
		if (ch == '[' && c->p + 1 < c->end && c->p[1] == ':') {
			const char *q = c->p + 2;
			while (q + 1 < c->end && !(q[0] == ':' && q[1] == ']')) q++;
			if (q + 1 >= c->end || !class_add(s, c->p + 2, (int)(q - (c->p + 2)))) {
				c->err = "unknown [:class:]";
				return false;
			}
			c->p = q + 2;
			continue;
		}
		c->p++;
		if (c->p + 1 < c->end && *c->p == '-' && c->p[1] != ']') {
			uint8_t hi = (uint8_t)c->p[1];
			if (hi < ch) { c->err = "a range backwards in [...]"; return false; }
			for (int k = ch; k <= hi; k++) set_add(s, k);
			c->p += 2;
			continue;
		}
		set_add(s, ch);
	}
	if (c->icase)			// either case of every letter in it
		for (int k = 'A'; k <= 'Z'; k++)
			if ((s[k >> 3] & (1u << (k & 7))) || (s[(k + 32) >> 3] & (1u << ((k + 32) & 7)))) {
				set_add(s, k);
				set_add(s, k + 32);
			}
	if (neg) for (int i = 0; i < 32; i++) s[i] = (uint8_t)~s[i];
	c->set = c->r->nsets++;
	return true;
}

static uint8_t escaped(uint8_t ch) {
	switch (ch) { case 'n': return '\n'; case 't': return '\t'; default: return ch; }
}

static bool next(cp_t *c) {
	c->esc = false;
	if (c->p >= c->end) { c->tok = T_END; return true; }
	uint8_t ch = (uint8_t)*c->p++;
	if (ch == '\\') {
		if (c->p >= c->end) { c->err = "a trailing backslash"; return false; }
		uint8_t e = (uint8_t)*c->p++;
		if (!c->ext) {
			switch (e) {
			case '(': c->tok = T_LP; return true;
			case ')': c->tok = T_RP; return true;
			case '|': c->tok = T_ALT; return true;
			case '+': c->tok = T_PLUS; return true;
			case '?': c->tok = T_QUES; return true;
			case '{': c->err = "\\{m,n\\} is not supported"; return false;
			}
		}
		if (e >= '1' && e <= '9') { c->err = "back-references in a pattern are not supported"; return false; }
		// \w \W \s \S, as GNU's: a word character, white space
		if (e == 'w' || e == 'W' || e == 's' || e == 'S') {
			if (c->r->nsets >= RX_SETS) { c->err = "too many [...] in one expression"; return false; }
			uint8_t *s = c->r->sets[c->r->nsets];
			memset(s, 0, 32);
			if (e == 'w' || e == 'W') { class_add(s, "alnum", 5); set_add(s, '_'); }
			else class_add(s, "space", 5);
			if (e == 'W' || e == 'S') for (int i = 0; i < 32; i++) s[i] = (uint8_t)~s[i];
			c->set = c->r->nsets++;
			c->tok = T_SET;
			return true;
		}
		c->tok = T_CHAR;
		c->c = escaped(e);
		c->esc = true;
		return true;
	}
	switch (ch) {
	case '.': c->tok = T_ANY; return true;
	case '[': c->tok = T_SET; return parse_set(c);
	case '*': c->tok = T_STAR; return true;
	}
	if (c->ext) {
		switch (ch) {
		case '(': c->tok = T_LP; return true;
		case ')': c->tok = T_RP; return true;
		case '|': c->tok = T_ALT; return true;
		case '+': c->tok = T_PLUS; return true;
		case '?': c->tok = T_QUES; return true;
		case '{': c->err = "{m,n} is not supported"; return false;
		}
	}
	c->tok = T_CHAR;
	c->c = ch;
	return true;
}

// -- emitting --

static int emit(cp_t *c, uint8_t op, uint8_t a, uint16_t x, uint16_t y) {
	rx_t *r = c->r;
	if (r->n >= RX_PROG) { c->err = "expression too long"; return -1; }
	r->prog[r->n] = (rx_ins_t){ op, a, x, y };
	return r->n++;
}

static bool is_jump(uint8_t op) { return op == I_SPLIT || op == I_JMP || op == I_LCHK; }

// Opens `k` instructions at `at`, moving what is there and every jump
// to it or past it.
static bool insert(cp_t *c, int at, int k) {
	rx_t *r = c->r;
	if (r->n + k > RX_PROG) { c->err = "expression too long"; return false; }
	memmove(&r->prog[at + k], &r->prog[at], sizeof(rx_ins_t) * (size_t)(r->n - at));
	r->n += k;
	// A jump from before `at` to `at` itself means "what follows", and
	// what follows now begins with the new instructions: it stays. One
	// from the code being wrapped (now after them) moves with it.
	for (int i = 0; i < r->n; i++) {
		rx_ins_t *in = &r->prog[i];
		if (!is_jump(in->op) || (i >= at && i < at + k)) continue;
		int lo = i >= at + k ? at : at + 1;
		if (in->x != UNSET && in->x >= lo) in->x = (uint16_t)(in->x + k);
		if (in->op == I_SPLIT && in->y != UNSET && in->y >= lo) in->y = (uint16_t)(in->y + k);
	}
	return true;
}

// Wraps [start, n) in a loop: e*
static bool star(cp_t *c, int start) {
	if (c->r->nloops >= RX_LOOPS) { c->err = "too many * and +"; return false; }
	uint8_t k = (uint8_t)c->r->nloops++;
	if (!insert(c, start, 2)) return false;
	c->r->prog[start] = (rx_ins_t){ I_SPLIT, 0, (uint16_t)(start + 1), UNSET };
	c->r->prog[start + 1] = (rx_ins_t){ I_LSET, k, 0, 0 };
	int chk = emit(c, I_LCHK, k, UNSET, 0);
	if (chk < 0 || emit(c, I_JMP, 0, (uint16_t)start, 0) < 0) return false;
	c->r->prog[start].y = (uint16_t)c->r->n;
	c->r->prog[chk].x = (uint16_t)c->r->n;
	return true;
}

static bool parse_regex(cp_t *c);

static bool parse_piece(cp_t *c) {
	int start = c->r->n;
	bool was_start = c->at_start;
	c->at_start = false;

	switch (c->tok) {
	case T_CHAR: {
		uint8_t ch = c->c;
		bool letter = (ch | 32) >= 'a' && (ch | 32) <= 'z';
		if (emit(c, c->icase && letter ? I_CHARI : I_CHAR, c->icase && letter ? (uint8_t)(ch | 32) : ch, 0, 0) < 0)
			return false;
		break;
	}
	case T_ANY:
		if (emit(c, I_ANY, 0, 0, 0) < 0) return false;
		break;
	case T_SET:
		if (emit(c, I_SET, (uint8_t)c->set, 0, 0) < 0) return false;
		break;
	case T_STAR:
		// at the start of a branch, a literal * (as POSIX has it)
		if (!was_start) { c->err = "nothing for * to repeat"; return false; }
		if (emit(c, I_CHAR, '*', 0, 0) < 0) return false;
		break;
	case T_LP: {
		if (c->r->ngroups >= RX_GROUPS - 1) { c->err = "more than nine groups"; return false; }
		if (++c->depth > 16) { c->err = "groups nested too deep"; return false; }
		int g = ++c->r->ngroups;
		if (emit(c, I_SAVE, (uint8_t)(2 * g), 0, 0) < 0) return false;
		if (!next(c)) return false;
		c->at_start = true;
		if (!parse_regex(c)) return false;
		if (c->tok != T_RP) { c->err = "unmatched ("; return false; }
		if (emit(c, I_SAVE, (uint8_t)(2 * g + 1), 0, 0) < 0) return false;
		c->depth--;
		break;
	}
	default:
		c->err = "nothing to repeat";
		return false;
	}
	if (!next(c)) return false;

	while (c->tok == T_STAR || c->tok == T_PLUS || c->tok == T_QUES) {
		int t = c->tok;
		if (t == T_STAR) {
			if (!star(c, start)) return false;
		} else if (t == T_PLUS) {
			// e e*: a copy of e, starred
			int len = c->r->n - start, at = c->r->n;
			if (at + len > RX_PROG) { c->err = "expression too long"; return false; }
			for (int i = 0; i < len; i++) {
				rx_ins_t in = c->r->prog[start + i];
				if (is_jump(in.op)) {
					if (in.x != UNSET) in.x = (uint16_t)(in.x + len);
					if (in.op == I_SPLIT && in.y != UNSET) in.y = (uint16_t)(in.y + len);
				}
				c->r->prog[at + i] = in;
			}
			c->r->n += len;
			if (!star(c, at)) return false;
		} else {
			if (!insert(c, start, 1)) return false;
			c->r->prog[start] = (rx_ins_t){ I_SPLIT, 0, (uint16_t)(start + 1), (uint16_t)c->r->n };
		}
		if (!next(c)) return false;
	}
	return true;
}

static bool parse_branch(cp_t *c) {
	c->at_start = true;
	while (c->tok != T_END && c->tok != T_ALT && c->tok != T_RP) {
		// ^ at the start of a branch, $ at its end: anchors; elsewhere,
		// the characters themselves
		if (c->tok == T_CHAR && c->c == '^' && !c->esc && c->at_start) {
			if (emit(c, I_BOL, 0, 0, 0) < 0 || !next(c)) return false;
			continue;
		}
		if (c->tok == T_CHAR && c->c == '$' && !c->esc) {
			const char *q = c->p;
			bool last = q >= c->end ||
				(c->ext && (*q == '|' || *q == ')')) ||
				(!c->ext && q + 1 < c->end && q[0] == '\\' && (q[1] == '|' || q[1] == ')'));
			if (last) {
				if (emit(c, I_EOL, 0, 0, 0) < 0 || !next(c)) return false;
				continue;
			}
		}
		if (!parse_piece(c)) return false;
	}
	return true;
}

static bool parse_regex(cp_t *c) {
	int start = c->r->n;
	if (!parse_branch(c)) return false;
	while (c->tok == T_ALT) {
		// SPLIT round what came before; it jumps past what comes after
		if (!insert(c, start, 1)) return false;
		c->r->prog[start] = (rx_ins_t){ I_SPLIT, 0, (uint16_t)(start + 1), UNSET };
		int j = emit(c, I_JMP, 0, UNSET, 0);
		if (j < 0) return false;
		c->r->prog[start].y = (uint16_t)c->r->n;
		if (!next(c) || !parse_branch(c)) return false;
		c->r->prog[j].x = (uint16_t)c->r->n;
	}
	return true;
}

bool rx_compile(rx_t *r, const char *pat, int n, bool extended, char *err, int errcap) {
	return rx_compile_flags(r, pat, n, extended ? RX_EXTENDED : 0, err, errcap);
}

bool rx_compile_flags(rx_t *r, const char *pat, int n, int flags, char *err, int errcap) {
	cp_t c;
	memset(r, 0, sizeof(*r));
	memset(&c, 0, sizeof(c));
	c.r = r;
	c.p = pat;
	c.end = pat + n;
	c.ext = (flags & RX_EXTENDED) != 0;
	c.icase = (flags & RX_ICASE) != 0;
	emit(&c, I_SAVE, 0, 0, 0);
	if (!next(&c) || !parse_regex(&c) || (c.tok != T_END && !c.err)) {
		if (!c.err) c.err = "unmatched )";
	}
	if (!c.err) emit(&c, I_SAVE, 1, 0, 0);
	if (!c.err) emit(&c, I_MATCH, 0, 0, 0);
	if (c.err) { snprintf(err, (size_t)errcap, "%s", c.err); return false; }
	r->anchored = r->n > 1 && r->prog[1].op == I_BOL;
	return true;
}

// -- running --

enum { S_ALT = 1, S_CAP, S_LOOP };
typedef struct { uint8_t kind, slot; uint16_t pc; int32_t v; } st_t;

#define STACK   1024
#define STEPS   200000

static st_t stack[STACK];

static bool run(const rx_t *r, const char *s, int len, int at, int *cap, bool *toolong, long *steps) {
	int loops[RX_LOOPS];
	int sp = 0, pc = 0, pos = at;
	for (int i = 0; i < 2 * RX_GROUPS; i++) cap[i] = -1;

	for (;;) {
		if (++*steps > STEPS) { *toolong = true; return false; }
		const rx_ins_t *in = &r->prog[pc];
		bool ok = true;
		switch (in->op) {
		case I_CHAR: ok = pos < len && (uint8_t)s[pos] == in->a; pos++; pc++; break;
		case I_CHARI: ok = pos < len && ((uint8_t)s[pos] | 32) == in->a; pos++; pc++; break;
		case I_ANY: ok = pos < len && s[pos] != '\n'; pos++; pc++; break;
		case I_SET: ok = pos < len && (r->sets[in->a][(uint8_t)s[pos] >> 3] & (1u << ((uint8_t)s[pos] & 7))); pos++; pc++; break;
		case I_BOL: ok = pos == 0; pc++; break;
		case I_EOL: ok = pos == len; pc++; break;
		case I_JMP: pc = in->x; break;
		case I_SPLIT:
			if (sp >= STACK) { *toolong = true; return false; }
			stack[sp++] = (st_t){ S_ALT, 0, in->y, pos };
			pc = in->x;
			break;
		case I_SAVE:
			if (sp >= STACK) { *toolong = true; return false; }
			stack[sp++] = (st_t){ S_CAP, in->a, 0, cap[in->a] };
			cap[in->a] = pos;
			pc++;
			break;
		case I_LSET:
			if (sp >= STACK) { *toolong = true; return false; }
			stack[sp++] = (st_t){ S_LOOP, in->a, 0, loops[in->a] };
			loops[in->a] = pos;
			pc++;
			break;
		// an iteration that matched nothing is the last: out of the loop
		case I_LCHK: pc = pos == loops[in->a] ? in->x : pc + 1; break;
		case I_MATCH: return true;
		default: return false;
		}
		if (ok) continue;
		// back to the last alternative, undoing on the way
		for (;;) {
			if (!sp) return false;
			st_t e = stack[--sp];
			if (e.kind == S_CAP) cap[e.slot] = e.v;
			else if (e.kind == S_LOOP) loops[e.slot] = e.v;
			else { pc = e.pc; pos = e.v; break; }
		}
	}
}

bool rx_exec(const rx_t *r, const char *s, int len, int from, int cap[2 * RX_GROUPS], bool *toolong) {
	long steps = 0;
	*toolong = false;
	// a literal first character: skip the places it cannot start
	int lit = r->n > 1 && r->prog[1].op == I_CHAR ? r->prog[1].a : -1;
	for (int at = from; at <= len; at++) {
		if (r->anchored && at > 0) return false;
		if (lit >= 0 && (at >= len || (uint8_t)s[at] != lit)) continue;
		if (run(r, s, len, at, cap, toolong, &steps)) return true;
		if (*toolong) return false;
	}
	return false;
}
