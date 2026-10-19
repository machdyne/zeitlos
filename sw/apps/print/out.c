/*
 * print -- number formatting for the writers. See out.h.
 */

#include "out.h"

#include <string.h>

int fmt_int(char *b, int32_t v) {
	char t[12];
	int n = 0, i = 0;
	uint32_t u = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
	do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
	if (v < 0) b[i++] = '-';
	while (n) b[i++] = t[--n];
	b[i] = 0;
	return i;
}

// v / 2^frac_bits, rounded to `decimals` places, trailing zeros dropped.
int fmt_dec(char *b, int64_t v, int frac_bits, int decimals) {
	int64_t scale = 1, q, r;
	int i, n;
	bool neg = v < 0;
	for (i = 0; i < decimals; i++) scale *= 10;
	if (neg) v = -v;
	// round to the nearest 1/scale
	q = (v * scale + ((int64_t)1 << (frac_bits - 1))) >> frac_bits;
	r = q % scale;
	q /= scale;
	n = 0;
	if (neg && (q || r)) b[n++] = '-';
	n += fmt_int(b + n, (int32_t)q);
	if (r) {
		char d[12];
		int k;
		for (k = decimals - 1; k >= 0; k--) { d[k] = (char)('0' + r % 10); r /= 10; }
		k = decimals;
		while (k > 0 && d[k - 1] == '0') k--;
		b[n++] = '.';
		memcpy(b + n, d, (size_t)k);
		n += k;
	}
	b[n] = 0;
	return n;
}

bool out_str(out_t *o, const char *s) {
	return out_put(o, s, (uint32_t)strlen(s));
}

bool out_int(out_t *o, int32_t v) {
	char b[16];
	return out_put(o, b, (uint32_t)fmt_int(b, v));
}

bool out_fix(out_t *o, int32_t v) {
	char b[24];
	return out_put(o, b, (uint32_t)fmt_dec(b, v, 8, 2));
}

bool out_f16(out_t *o, int32_t v) {
	char b[24];
	return out_put(o, b, (uint32_t)fmt_dec(b, v, 16, 5));
}
