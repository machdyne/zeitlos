/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- formulas (docs/learn_format.md, "Formulas").
 *
 * One notation, parsed once, used two ways: learn typesets it
 * (`$3/4$` as a stacked fraction), and learncheck evaluates it, so a
 * claim a lesson makes in a formula -- `(true "3/4 > 2/4")`, a number
 * answer `(from "2*pi*sqrt(0.5/9.81)")` -- is computed rather than
 * trusted. Plain text that reads correctly without the app: no TeX.
 *
 *   3/4        a fraction (typeset stacked)       2 1/2    a mixed number
 *   x^2  x_1   power, subscript                  2x, 2 pi  implied times
 *   sqrt(x)  abs(x)  sin cos tan ln log exp      pi e      constants
 *   theta alpha ... omega                         Greek letters by name
 *   < > <= >= = !=                                comparisons, chainable
 *   *  ×  ÷  ·  -  +  %  ( )                      25% is 1/4
 */

#ifndef LMATH_H
#define LMATH_H

#include <stdint.h>
#include <stdbool.h>

#include "lsx.h"

enum {
	LM_NUM = 0,		// a number as written: `text`; exact value in num/den when rational
	LM_VAR,			// a letter or name: x, L, theta
	LM_ADD, LM_SUB, LM_MUL, LM_IMUL /* implied */, LM_DIV /* stacked */, LM_IDIV /* ÷ */,
	LM_NEG, LM_POW, LM_SUB_SCRIPT, LM_PCT,
	LM_FUNC,		// name(arg): sqrt, abs, sin, ...
	LM_PAREN,		// ( a ): kept so typesetting shows the brackets
	LM_CMP,			// a chain: kids are the operands, ops[] between them
	LM_MIXED,		// 2 1/2: kids whole, num, den
};

enum { LM_LT = 1, LM_GT, LM_LE, LM_GE, LM_EQ, LM_NE };

typedef struct lm_node lm_node_t;
struct lm_node {
	uint8_t		kind;
	uint8_t		op;			// LM_CMP: one per gap, in ops
	const char	*text;		// NUM: digits; VAR/FUNC: the name
	lm_node_t	*a, *b, *c;	// operands (unary: a; binary: a, b; mixed: a b c)
	lm_node_t	*kids[8];	// LM_CMP operands
	uint8_t		ops[7];
	uint8_t		nkids;
};

// Parses `s` (len bytes). NULL with a reason in err on a syntax error.
lm_node_t *lm_parse(lx_arena_t *a, const char *s, int len, char *err, int errcap);

// -- values --

typedef struct {
	bool		ok;			// computable (no unknown variable, no domain error)
	bool		exact;		// num/den hold it exactly
	bool		is_bool;	// the result of a comparison
	bool		truth;
	int64_t		num, den;	// den > 0, in lowest terms
	double		v;
} lm_val_t;

lm_val_t lm_eval(const lm_node_t *n);
// Parse and evaluate in one step: ok false if it does not parse or compute.
lm_val_t lm_eval_str(lx_arena_t *a, const char *s, char *err, int errcap);
// Does the expression mention any variable (a name that is not a constant)?
bool lm_has_vars(const lm_node_t *n);
// is `name` a Greek letter's name (theta, Omega...)? its UTF-8 letter, else NULL
const char *lm_greek(const char *name);

#endif
