#ifndef ZFIX_H
#define ZFIX_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Decimal fixed-point arithmetic -- the shared version of what
 * sw/apps/calc used to keep to itself.
 *
 * -- why fixed point at all --
 *
 * There is no FPU (rv32im, see sw/common/arch.mk), so floating point
 * means soft-float: a large library pulled in for something an app
 * does once per keypress, and binary fractions that cannot represent
 * 0.1 exactly -- which on a machine whose whole job is to agree with
 * you about decimal numbers is the wrong failure to accept.
 *
 * A value is an int64_t counting 10^-dp units. 1.25 at dp=2 is 125.
 *
 * -- why the precision is a parameter --
 *
 * calc wants six decimal places against a ten-digit display; a
 * spreadsheet wants fewer places and more integer range, because the
 * numbers people put in one are money and counts rather than the
 * result of dividing by three. Both want the same overflow-refusing
 * arithmetic underneath.
 *
 * So `dp` is passed in rather than compiled in. It is a plain int in
 * 0..Z_FIX_DP_MAX, and every function that needs the scale looks it
 * up in a table rather than computing a power of ten -- three of the
 * four operations here need it, and a loop per multiply is not a cost
 * worth paying for a table of ten int64s.
 *
 * MIXING PRECISIONS IS THE CALLER'S PROBLEM. Nothing here records
 * which dp a value was made at, so handing a dp=6 value to a dp=2
 * multiply produces a wrong answer quietly. Each app picks one dp and
 * uses it everywhere; that is what the ZFIX_DP constant in an app's
 * own header is for (calc_core.h's CALC_DP, sheet_core.h's SHEET_DP).
 *
 * -- overflow is refused, never wrapped --
 *
 * Every operation returns false rather than producing a wrong answer.
 * Wrapping is the worst available behaviour here: it yields a number
 * that looks entirely reasonable and is completely wrong, in an app
 * whose only purpose is to be trusted. Signed overflow is also
 * undefined behaviour, so each check happens BEFORE the operation
 * rather than by inspecting its result.
 *
 * -- cost --
 *
 * 64-bit arithmetic pulls libgcc's __muldi3/__divdi3 in on this
 * target. That is a few hundred bytes for exactness, and it is the
 * only non-obvious cost here, so: it is deliberate.
 */

#include <stdint.h>
#include <stdbool.h>

typedef int64_t z_fix_t;

// Largest decimal places supported. Nine, because 10^9 is the largest
// power of ten that leaves an int64 room for a plausible integer part
// (10^18 is already most of the range), and because a tenth entry in
// the table below would buy nobody anything.
#define Z_FIX_DP_MAX   9

// 10^dp, indexed by dp. Declared here so a caller can use it in a
// constant expression; defined once in zfix.c.
extern const z_fix_t z_fix_scales[Z_FIX_DP_MAX + 1];

// The scale for `dp`, or 1 for a dp outside the supported range --
// which is the least destructive answer available, and only reachable
// through a caller bug.
z_fix_t z_fix_scale(int dp);

// -- arithmetic --
//
// All four return false on overflow (and z_fix_div also on a zero
// divisor), leaving *out untouched. dp must match the scale both
// operands were built at.

bool z_fix_add(z_fix_t a, z_fix_t b, z_fix_t *out);
bool z_fix_sub(z_fix_t a, z_fix_t b, z_fix_t *out);
bool z_fix_mul(z_fix_t a, z_fix_t b, int dp, z_fix_t *out);
bool z_fix_div(z_fix_t a, z_fix_t b, int dp, z_fix_t *out);

// Scaled value of a whole number, or false if it doesn't fit.
bool z_fix_from_int(int32_t v, int dp, z_fix_t *out);

// -- formatting --
//
// Writes `v` into `out` NUL-terminated, returning the length written.
//
// `places` controls the fraction:
//
//   Z_FIX_STRIP (-1)  as many places as the value actually needs,
//                     trailing zeros removed -- 1/2 reads as "0.5",
//                     not "0.500000", and a whole number gets no
//                     point at all.
//   0..dp             exactly that many, ROUNDED to nearest (which
//                     can carry into the integer part: 0.999 at 2
//                     places is "1.00").
//
// A `places` above dp is clamped to dp: there is no information down
// there to print, and printing zeros would imply there was.
//
// TRUNCATES rather than failing if `cap` is too small, so a caller
// with a fixed-width field gets the leading digits rather than
// nothing. Check the return against what you asked for if that
// matters -- sheet_core.c's column formatter does.
#define Z_FIX_STRIP   (-1)

int z_fix_format(z_fix_t v, int dp, int places, char *out, int cap);

// How many characters z_fix_format() would write, without writing
// them. For a caller deciding whether a value fits a column before
// committing to a buffer.
int z_fix_format_len(z_fix_t v, int dp, int places);

// -- parsing --
//
// Reads a decimal number: optional sign, digits, optional point,
// digits. No exponent, no thousands separators, no leading '+'
// rejection -- this is for machine-written files and typed cell
// entries, not for a general numeric tower.
//
// Returns true only if at least one digit was seen and the value fits.
// Digits beyond `dp` are ROUNDED away rather than truncated, so a file
// written at dp=6 and read at dp=2 gives the nearest representable
// value rather than silently flooring.
//
// *end (may be NULL) receives the first character not consumed, so a
// caller can tell "1.5" from "1.5x" -- the latter parses successfully
// up to the 'x', and it is the caller's business whether that is a
// number or a piece of text that happens to start like one.
bool z_fix_parse(const char *s, int dp, z_fix_t *out, const char **end);

// True if the WHOLE of `s` is a number -- z_fix_parse() plus "and
// nothing was left over but spaces". This is the test for "did the
// user type a number or a label", which is a different question from
// "does this start with a number", and getting the two confused is
// how "3 apples" becomes 3.
bool z_fix_parse_all(const char *s, int dp, z_fix_t *out);

#endif
