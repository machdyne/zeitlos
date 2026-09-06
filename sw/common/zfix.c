/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Decimal fixed-point arithmetic. See zfix.h.
 *
 * This started as the static fix_add/fix_sub/fix_mul/fix_div inside
 * sw/apps/calc/calc_core.c and is a straight lift of them, with the
 * scale turned from a compile-time constant into a parameter. The
 * overflow reasoning in the comments below is that code's, kept
 * because it is the part that took thought.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zfix.h"

const z_fix_t z_fix_scales[Z_FIX_DP_MAX + 1] = {
	1LL,
	10LL,
	100LL,
	1000LL,
	10000LL,
	100000LL,
	1000000LL,
	10000000LL,
	100000000LL,
	1000000000LL,
};

z_fix_t z_fix_scale(int dp) {

	if (dp < 0 || dp > Z_FIX_DP_MAX) return 1;
	return z_fix_scales[dp];

}

bool z_fix_add(z_fix_t a, z_fix_t b, z_fix_t *out) {

	// Signed overflow is undefined behaviour, so this has to be
	// checked BEFORE the addition, not by inspecting the result.
	if (b > 0 && a > INT64_MAX - b) return false;
	if (b < 0 && a < INT64_MIN - b) return false;

	*out = a + b;
	return true;

}

bool z_fix_sub(z_fix_t a, z_fix_t b, z_fix_t *out) {

	if (b < 0 && a > INT64_MAX + b) return false;
	if (b > 0 && a < INT64_MIN + b) return false;

	*out = a - b;
	return true;

}

bool z_fix_mul(z_fix_t a, z_fix_t b, int dp, z_fix_t *out) {

	z_fix_t scale = z_fix_scale(dp);

	if (a == 0 || b == 0) { *out = 0; return true; }

	// a and b are both scaled, so the product is scaled TWICE and has
	// to come back down by `scale`. The intermediate is what
	// overflows, long before either operand does -- two values of a
	// million each are 1e12 scaled, and their product is 1e24.
	//
	// Checked by division rather than by computing and looking,
	// because the overflow itself would be undefined behaviour.
	z_fix_t aa = a < 0 ? -a : a;
	z_fix_t bb = b < 0 ? -b : b;

	if (aa > INT64_MAX / bb) return false;

	z_fix_t p = a * b;

	// Round to nearest on the way back down, rather than truncating.
	// Truncation makes 1.5 * 1.5 come out as 2.249999 on operands
	// that are themselves exact.
	z_fix_t half = scale / 2;

	if (p >= 0) {
		if (p > INT64_MAX - half) return false;
		*out = (p + half) / scale;
	} else {
		if (p < INT64_MIN + half) return false;
		*out = (p - half) / scale;
	}

	return true;

}

bool z_fix_div(z_fix_t a, z_fix_t b, int dp, z_fix_t *out) {

	z_fix_t scale = z_fix_scale(dp);

	if (b == 0) return false;

	// Scale the numerator up before dividing, or every result would
	// be a whole number. That is the step that can overflow.
	z_fix_t aa = a < 0 ? -a : a;

	if (aa > INT64_MAX / scale) return false;

	z_fix_t n = a * scale;

	// Round to nearest, matching z_fix_mul(). The sign of the
	// remainder follows the numerator in C, so the half has to follow
	// it too.
	z_fix_t half = (b < 0 ? -b : b) / 2;

	if (n >= 0) {
		if (n > INT64_MAX - half) return false;
		*out = (n + half) / b;
	} else {
		if (n < INT64_MIN + half) return false;
		*out = (n - half) / b;
	}

	return true;

}

bool z_fix_from_int(int32_t v, int dp, z_fix_t *out) {

	z_fix_t scale = z_fix_scale(dp);
	z_fix_t vv = v < 0 ? -(z_fix_t)v : (z_fix_t)v;

	if (vv > INT64_MAX / scale) return false;

	*out = (z_fix_t)v * scale;
	return true;

}

// -- formatting --

// Rounds `v` (magnitude, already non-negative) so that only `places`
// decimals remain significant, still expressed at `dp`. Returns false
// if the rounding would overflow, which only a value within a
// half-unit of INT64_MAX can do.
static bool round_to_places(z_fix_t v, int dp, int places, z_fix_t *out) {

	if (places >= dp) { *out = v; return true; }

	z_fix_t drop = z_fix_scale(dp - places);	// units being discarded
	z_fix_t half = drop / 2;

	if (v > INT64_MAX - half) return false;

	*out = ((v + half) / drop) * drop;
	return true;

}

// The shared core of z_fix_format() and z_fix_format_len(). Writes
// into `out` when it is non-NULL, counts either way.
//
// One function rather than two so the length can never disagree with
// what actually gets written -- a caller uses the length to decide
// whether the value fits a column, and a formatter that lies by one
// character puts a digit through a cell border.
static int format_into(z_fix_t v, int dp, int places, char *out, int cap) {

	char tmp[24];
	int t = 0, n = 0;
	bool neg = false;

	if (dp < 0) dp = 0;
	if (dp > Z_FIX_DP_MAX) dp = Z_FIX_DP_MAX;
	if (places > dp) places = dp;

	// -- emit one character, bounded --
	//
	// `cap` includes the NUL, so the last writable index is cap-2.
	// Counting continues past the cap so the return value still says
	// how long the number really is.
	//
	// The temporary is load-bearing. Two call sites below pass
	// `tmp[--t]`, and with the store guarded directly by `if (out &&
	// ...)` the argument went unevaluated whenever there was nothing
	// to write to -- so z_fix_format_len(), which passes out = NULL,
	// looped forever on a digit counter that never decremented.
	// Evaluate once, then decide whether to keep it.
	#define PUT(ch) do { \
		char put_c_ = (ch); \
		if (out && n < cap - 1) out[n] = put_c_; \
		n++; \
	} while (0)

	if (v < 0) { neg = true; }

	// INT64_MIN has no positive counterpart, so negating it is
	// undefined. Nothing in this system can produce it (every
	// operation above refuses to reach the extremes), but a value
	// read straight out of a corrupt file could, and the failure
	// would be a hang or garbage rather than an error.
	z_fix_t mag = (v == INT64_MIN) ? INT64_MAX : (neg ? -v : v);

	if (places >= 0) {
		z_fix_t r;
		if (round_to_places(mag, dp, places, &r)) mag = r;
	}

	z_fix_t scale = z_fix_scale(dp);
	z_fix_t ip = mag / scale;
	z_fix_t fp = mag % scale;

	int show = places;

	if (show < 0) {

		// Z_FIX_STRIP: as many places as the value needs. Note this
		// runs AFTER the integer part is split off, so stripping can
		// never disturb it.
		show = dp;
		while (show > 0 && (fp % 10) == 0) { fp /= 10; show--; }

	} else {

		// Re-derive the fraction at exactly `show` digits. The
		// rounding above already moved the value, so this is a plain
		// division with nothing left to decide.
		for (int i = 0; i < dp - show; i++) fp /= 10;

	}

	// A negative value that rounds to zero prints as "0", not "-0".
	// "-0" is not wrong so much as startling, and it turns up
	// immediately once a column shows two decimals of something
	// computed.
	if (neg && ip == 0 && fp == 0) neg = false;

	if (neg) PUT('-');

	if (!ip) tmp[t++] = '0';
	while (ip && t < (int)sizeof(tmp)) {
		tmp[t++] = (char)('0' + (int)(ip % 10));
		ip /= 10;
	}
	while (t) PUT(tmp[--t]);

	if (show > 0) {

		PUT('.');

		char ftmp[Z_FIX_DP_MAX + 1];
		int ft = 0;

		for (int i = 0; i < show; i++) {
			ftmp[ft++] = (char)('0' + (int)(fp % 10));
			fp /= 10;
		}

		while (ft) PUT(ftmp[--ft]);

	}

	#undef PUT

	if (out && cap > 0) out[n < cap - 1 ? n : cap - 1] = 0;

	return n;

}

int z_fix_format(z_fix_t v, int dp, int places, char *out, int cap) {

	if (!out || cap < 1) return 0;

	int want = format_into(v, dp, places, out, cap);

	// The written length, which is what a caller appending to this
	// buffer needs. The return of format_into() is the length the
	// number WANTED, which is what z_fix_format_len() reports.
	return want < cap - 1 ? want : cap - 1;

}

int z_fix_format_len(z_fix_t v, int dp, int places) {
	return format_into(v, dp, places, NULL, 0);
}

// -- parsing --

bool z_fix_parse(const char *s, int dp, z_fix_t *out, const char **end) {

	if (!s) return false;

	if (dp < 0) dp = 0;
	if (dp > Z_FIX_DP_MAX) dp = Z_FIX_DP_MAX;

	const char *p = s;
	bool neg = false;
	bool any = false;
	z_fix_t v = 0;

	while (*p == ' ' || *p == '\t') p++;

	if (*p == '-') { neg = true; p++; }
	else if (*p == '+') p++;

	// Integer part. Scaled as it goes rather than accumulated whole
	// and multiplied at the end: the unscaled accumulation would
	// overflow at a very different (and much larger) number of digits
	// than the scaled value can actually hold, so the refusal would
	// land in the wrong place.
	z_fix_t scale = z_fix_scale(dp);

	while (*p >= '0' && *p <= '9') {

		any = true;

		if (v > INT64_MAX / 10) return false;
		v *= 10;

		z_fix_t d = (z_fix_t)(*p - '0');
		if (v > INT64_MAX - d) return false;
		v += d;

		p++;

	}

	if (v > INT64_MAX / scale) return false;
	v *= scale;

	// Fraction.
	if (*p == '.') {

		p++;

		z_fix_t place = scale;
		int extra_digit = -1;		// the first digit past dp, for rounding

		while (*p >= '0' && *p <= '9') {

			any = true;
			int d = *p - '0';

			if (place > 1) {
				place /= 10;
				z_fix_t add = (z_fix_t)d * place;
				if (v > INT64_MAX - add) return false;
				v += add;
			} else if (extra_digit < 0) {
				extra_digit = d;
			}

			p++;

		}

		// Round the discarded tail rather than flooring it. A file
		// written at six places and read at two should give the
		// nearest value it can, not always the lower one.
		if (extra_digit >= 5) {
			if (v > INT64_MAX - 1) return false;
			v += 1;
		}

	}

	if (!any) return false;

	if (end) *end = p;

	*out = neg ? -v : v;
	return true;

}

bool z_fix_parse_all(const char *s, int dp, z_fix_t *out) {

	const char *end = NULL;
	z_fix_t v;

	if (!z_fix_parse(s, dp, &v, &end)) return false;

	while (*end == ' ' || *end == '\t') end++;
	if (*end) return false;

	*out = v;
	return true;

}
