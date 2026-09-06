/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Calculator entry state. See calc_core.h.
 *
 * The arithmetic itself is in sw/common/zfix.c -- these four
 * operations and the decimal formatter were static functions here
 * until sw/apps/sheet needed the same ones at a different precision.
 * What remains is the part that is about a keypad.
 */

#include <stdint.h>
#include <stdbool.h>

#include "calc_core.h"

// CALC_SCALE is spelled out in the header so it can be used in a
// constant expression (CALC_MAX). This is what stops it drifting away
// from CALC_DP.
_Static_assert(CALC_SCALE == 1000000LL && CALC_DP == 6,
	"CALC_SCALE must be 10^CALC_DP -- see calc_core.h");

void calc_reset(calc_t *c) {

	c->acc = 0;
	c->pending = 0;
	c->entry = 0;
	c->entering = false;
	c->entry_dp = -1;
	c->error = false;
	c->last_op = 0;
	c->last_rhs = 0;
	c->has_last = false;

}

calc_fix_t calc_display(const calc_t *c) {
	return c->entering ? c->entry : c->acc;
}

// True if `v` fits the display. Checked after every result, so a
// number too big to show is reported rather than displayed truncated.
static bool fits(calc_fix_t v) {
	return v <= CALC_MAX && v >= -CALC_MAX;
}

// Applies `op` to a and b. Sets the error flag on overflow or divide
// by zero.
static void apply(calc_t *c, char op, calc_fix_t a, calc_fix_t b) {

	calc_fix_t r = 0;
	bool ok;

	switch (op) {
		case '+': ok = z_fix_add(a, b, &r); break;
		case '-': ok = z_fix_sub(a, b, &r); break;
		case '*': ok = z_fix_mul(a, b, CALC_DP, &r); break;
		case '/': ok = z_fix_div(a, b, CALC_DP, &r); break;
		default:  r = b; ok = true; break;
	}

	if (!ok || !fits(r)) {
		c->error = true;
		return;
	}

	c->acc = r;

}

// -- entry --

void calc_digit(calc_t *c, int d) {

	if (c->error) return;
	if (d < 0 || d > 9) return;

	if (!c->entering) {
		// Starting a fresh number. Anything on the display is a
		// result, and a digit REPLACES it rather than extending it --
		// the alternative is `2 + 3 =` then `4` giving 54.
		c->entry = 0;
		c->entry_dp = -1;
		c->entering = true;
	}

	if (c->entry_dp < 0) {

		// Integer part: shift left and add.
		//
		// A plain integer multiply by 10, NOT z_fix_mul(entry, 10 *
		// CALC_SCALE, CALC_DP). Both scale the value by ten, but z_fix_mul
		// forms the doubly-scaled product first and only then brings
		// it back down -- and that intermediate overflows an int64
		// at around seven typed digits, far short of the ten the
		// display holds. Typing 9999999999 stopped at 999999.
		//
		// Shifting the already-scaled value directly needs no
		// intermediate at all.
		if (c->entry > INT64_MAX / 10) return;

		calc_fix_t next = c->entry * 10;

		if (next > INT64_MAX - (calc_fix_t)d * CALC_SCALE) return;

		next += (calc_fix_t)d * CALC_SCALE;

		if (!fits(next)) return;		// silently refuse, don't error

		c->entry = next;

	} else {

		// Fraction: each digit is worth ten times less than the last.
		// Beyond CALC_DP there is nowhere to put it, so it is
		// dropped -- refusing the keypress is better than rounding
		// something the user is still in the middle of typing.
		if (c->entry_dp >= CALC_DP) return;

		calc_fix_t place = CALC_SCALE;
		for (int i = 0; i <= c->entry_dp; i++) place /= 10;

		calc_fix_t add = (calc_fix_t)d * place;

		c->entry += (c->entry < 0) ? -add : add;
		c->entry_dp++;

	}

}

void calc_point(calc_t *c) {

	if (c->error) return;

	if (!c->entering) {
		c->entry = 0;
		c->entering = true;
	}

	// A second point is a slip, not a question -- ignore it.
	if (c->entry_dp < 0) c->entry_dp = 0;

}

void calc_sign(calc_t *c) {

	if (c->error) return;

	if (c->entering) c->entry = -c->entry;
	else c->acc = -c->acc;

}

void calc_backspace(calc_t *c) {

	if (c->error) return;

	// A result is not something you can backspace into. Doing nothing
	// is right: the alternative is inventing an entry state from a
	// number the user did not type.
	if (!c->entering) return;

	if (c->entry_dp < 0) {

		c->entry = (c->entry / (10 * CALC_SCALE)) * CALC_SCALE;

	} else if (c->entry_dp == 0) {

		// Removing the point itself.
		c->entry_dp = -1;

	} else {

		c->entry_dp--;

		// Drop the digit that is now past the end.
		//
		// The digit being removed sat at CALC_SCALE / 10^(dp+1)
		// BEFORE the decrement -- which is CALC_SCALE / 10^dp after
		// it. So the loop runs dp times, not dp+1: with one division
		// too many the truncation lands on the place still being
		// kept and removes nothing, so "1.5" backspaced twice stayed
		// "1.5" instead of becoming "1".
		calc_fix_t place = CALC_SCALE;
		for (int i = 0; i < c->entry_dp; i++) place /= 10;

		c->entry = (c->entry / place) * place;

	}

}

void calc_op(calc_t *c, char op) {

	if (c->error) return;

	if (c->entering) {

		// A number was typed: fold it into the accumulator through
		// whatever was pending.
		if (c->pending) apply(c, c->pending, c->acc, c->entry);
		else c->acc = c->entry;

		c->entering = false;
		c->entry_dp = -1;

	}

	// Pressing another operator without typing a number in between
	// just changes the pending one -- `2 + *` means `2 *`, which is
	// what every calculator does and what a mis-keyed operator needs.
	if (!c->error) c->pending = op;

	c->has_last = false;

}

void calc_equals(calc_t *c) {

	if (c->error) return;

	if (c->entering) {

		if (c->pending) {
			c->last_op = c->pending;
			c->last_rhs = c->entry;
			c->has_last = true;
			apply(c, c->pending, c->acc, c->entry);
		} else {
			c->acc = c->entry;
			c->has_last = false;
		}

		c->pending = 0;
		c->entering = false;
		c->entry_dp = -1;

		return;

	}

	// '=' with nothing typed repeats the last operation, so `2 + 3 =`
	// then `=` gives 8. Physical calculators do this and fingers
	// expect it.
	if (c->has_last) apply(c, c->last_op, c->acc, c->last_rhs);

}

// -- formatting --

int calc_format(const calc_t *c, char *out, int cap) {

	if (cap < 2) { if (cap > 0) out[0] = 0; return 0; }

	if (c->error) {
		int n = 0;
		const char *e = "error";
		for (; e[n] && n < cap - 1; n++) out[n] = e[n];
		out[n] = 0;
		return n;
	}

	// While ENTERING, the typed decimal places are shown as typed
	// rather than stripped: someone part way through "1.50" should see
	// what they pressed, not have it tidied to "1.5" under their
	// fingers. Everywhere else, trailing zeros go -- 1/2 reads as
	// "0.5", not "0.500000".
	//
	// Asking z_fix_format() for entry_dp places is safe even though it
	// ROUNDS to that many: while entering, every digit below entry_dp
	// is zero by construction (see calc_digit()), so there is nothing
	// for the rounding to do.
	int places = (c->entering && c->entry_dp >= 0)
		? c->entry_dp : Z_FIX_STRIP;

	int n = z_fix_format(calc_display(c), CALC_DP, places, out, cap);

	// The point has been typed but no digit after it yet. Showing it
	// is the only feedback that the keypress registered -- and it is
	// not something a formatter can know, since the value is a whole
	// number at this moment and indistinguishable from any other.
	if (c->entering && c->entry_dp == 0 && n < cap - 1) {
		out[n++] = '.';
		out[n] = 0;
	}

	return n;

}
