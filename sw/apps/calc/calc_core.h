#ifndef CALC_CORE_H
#define CALC_CORE_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Calculator arithmetic and entry state -- no drawing, no windows, no
 * messages, nothing that needs hardware.
 *
 * Separate from calc.c precisely so it can be compiled and tested on a
 * host (see calc_test.c). A calculator that is subtly wrong is worse
 * than no calculator: nobody checks its answers, that is the entire
 * point of it. Everything here is deterministic and self-contained, so
 * the arithmetic can be exercised properly rather than eyeballed on
 * the screen.
 *
 * -- fixed point, not floating --
 *
 * There is no FPU (rv32im, see sw/common/arch.mk), so floating point
 * means soft-float: a large library pulled in for something a
 * calculator does once per keypress, and binary fractions that cannot
 * represent 0.1 exactly -- which on a machine whose whole job is to
 * agree with you about decimal numbers is the wrong failure to accept.
 *
 * A value is a 64-bit integer of CALC_SCALE-ths. 6 decimal places
 * chosen against the display: this app is deliberately narrow (screen
 * space is scarce), the display holds CALC_DIGITS characters, and
 * spending more of them on fraction than integer would be the wrong
 * trade for a pocket calculator.
 *
 * 64-bit arithmetic does pull in libgcc's __muldi3/__divdi3 on this
 * target. That is a few hundred bytes for exactness, and it is the
 * only place in this app where the cost is not obvious, so: it is
 * deliberate.
 */

#include <stdint.h>
#include <stdbool.h>

typedef int64_t calc_fix_t;

// 6 decimal places.
#define CALC_DP     6
#define CALC_SCALE  1000000LL

// Widest result the display will show, in significant digits. A
// result needing more is reported as an error rather than shown
// rounded: a calculator that silently drops digits is lying.
#define CALC_DIGITS 10

// Largest magnitude representable in CALC_DIGITS digits, scaled.
// 9999999999 / 10^6 is not it -- CALC_DIGITS counts the digits SHOWN,
// integer and fraction together, so the integer part alone is bounded
// by 10^CALC_DIGITS.
#define CALC_MAX    (9999999999LL * CALC_SCALE)

typedef struct {

	// The running total, and the operation waiting on it.
	calc_fix_t	acc;
	char		pending;	// 0, '+', '-', '*', '/'

	// The number being typed. `entering` distinguishes "the user has
	// started typing a number" from "the display is showing a
	// result" -- they look the same but behave differently, and
	// conflating them is the classic calculator bug where typing a
	// digit after `=` appends to the answer.
	calc_fix_t	entry;
	bool		entering;

	// Decimal places typed so far, or -1 if no point has been typed.
	// Needed separately from `entry` because "1.0" and "1" are the
	// same value but not the same entry state: the next digit lands
	// in a different place.
	int			entry_dp;

	// Set by overflow or division by zero, and sticky until cleared.
	// Every operation is a no-op while it is set, so an error cannot
	// be silently arithmetic'd away into a plausible-looking number.
	bool		error;

	// The last operation and operand, for repeating on '='. Pressing
	// '=' twice after `2 + 3` gives 8, which is what a physical
	// calculator does and what muscle memory expects.
	char		last_op;
	calc_fix_t	last_rhs;
	bool		has_last;

} calc_t;

// Clears everything -- the C key.
void calc_reset(calc_t *c);

// Digit 0-9.
void calc_digit(calc_t *c, int d);

// Decimal point. A second one is ignored rather than treated as an
// error; it is a slip, not a question.
void calc_point(calc_t *c);

// Negate. Applies to whatever is on the display -- the entry if one
// is being typed, otherwise the accumulator.
void calc_sign(calc_t *c);

// Remove the last typed character. Only meaningful while entering; a
// result is not a thing you can backspace into.
void calc_backspace(calc_t *c);

// Apply a pending operation and remember `op` as the next one.
void calc_op(calc_t *c, char op);

// Apply the pending operation, or repeat the last one.
void calc_equals(calc_t *c);

// What the display should show right now.
calc_fix_t calc_display(const calc_t *c);

// Formats the display value into `out`, NUL-terminated. Returns the
// length written.
//
// Trailing fractional zeros are stripped, so 1/2 shows "0.5" rather
// than "0.500000", and a whole number shows no point at all. Writes
// "error" when the error flag is set.
int calc_format(const calc_t *c, char *out, int cap);

#endif
