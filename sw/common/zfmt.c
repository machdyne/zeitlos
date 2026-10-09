/*
 * Zeitlos
 *
 * zfmt.c -- the printf family, integers only, for the few binaries
 * that opt in: the kernel and the core apps in flash. docs/build.md,
 * "Integer-only printf (ZFMT)".
 *
 * -- why --
 *
 * newlib's printf and snprintf are complete: each can print a double,
 * so each drags in its own formatting engine (_vfprintf_r and
 * _svfprintf_r, ~14 KB apiece), _dtoa_r, the multi-precision helpers
 * and libgcc's soft double arithmetic. Measured, that is 59 KB of wm
 * and of term -- about half of each binary -- for programs with no
 * floating point to print. The integer-only engines newlib also has
 * are two more copies (~7 KB each). This is one, of about 1.5 KB.
 *
 * -- what it is and is not --
 *
 * Only the FORMATTING is replaced. Output to a FILE goes through
 * fwrite() on that FILE, in pieces, so newlib's stdio still does
 * everything it did: line buffering of stdout (it is a tty, per
 * _isatty()), fflush(), the order of output mixed with puts() and
 * putchar(), stderr being unbuffered and staying on the UART, and
 * z_stdout_hook (zeitlos.h) receiving whole lines rather than
 * fragments. The string forms never touch a FILE.
 *
 * Supported: %d %i %u %o %x %X %c %s %p %% %n; flags - + space # 0;
 * width and precision, both also as *; length modifiers hh h l ll j z
 * t. Behaviour is C99's for all of those (tests/test_zfmt.c checks it
 * against the build machine's libc). %p is "0x" and hex digits, as
 * newlib prints it.
 *
 * NOT supported: floating point. %f %F %e %E %g %G %a %A still consume
 * their double, so the arguments after them stay in step, but print a
 * single '?'. A binary that opts in must not need them; the opt-in is
 * per binary for exactly that reason.
 *
 * -- ZFMT_FLOAT --
 *
 * Unless it is built with ZFMT_FLOAT (the app.mk switch of the same
 * name), for a binary that does print doubles -- repl -- but links the
 * soft-float arithmetic anyway. Then %e %f %g (and %E %F %G) work, with
 * every flag, width and precision, in about 2 KB more; %a still does
 * not. The digits come from scaling by a power of ten carried in two
 * doubles (about 106 bits), not from newlib's exact multi-precision
 * conversion: up to 19 significant digits are the exact value's, ties
 * rounded to even as newlib rounds them, and only a value within about
 * 1e-30 of a tie could differ. Past 19 digits (%f of a number over
 * 10^19, %.20g) the rest are zeros, where newlib prints the binary
 * value's exact expansion. tests/test_zfmt.c measures it against the
 * host's printf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The host test builds this with its names changed, so the build
// machine's own printf stays intact to report with.
#ifdef ZFMT_HOST_TEST
#define ZF(name) zf_##name
#else
#define ZF(name) name
#endif

// Where the characters go: a caller's buffer (f == NULL), or a FILE,
// staged in `tmp` and handed over a piece at a time.
typedef struct {
	FILE		*f;
	char		*buf;
	size_t		cap;		// buffer size, including the NUL
	size_t		len;		// characters produced, written or not
	unsigned	tn;
	char		tmp[64];
} zf_out_t;

static void zf_flush(zf_out_t *o) {
	if (o->f && o->tn) fwrite(o->tmp, 1, o->tn, o->f);
	o->tn = 0;
}

static void zf_put(zf_out_t *o, char c) {
	if (o->f) {
		o->tmp[o->tn++] = c;
		if (o->tn == sizeof(o->tmp)) zf_flush(o);
	} else if (o->len + 1 < o->cap) {
		o->buf[o->len] = c;
	}
	o->len++;
}

static void zf_pad(zf_out_t *o, char c, int n) {
	while (n-- > 0) zf_put(o, c);
}

#define ZF_LEFT		0x01
#define ZF_PLUS		0x02
#define ZF_SPACE	0x04
#define ZF_ALT		0x08
#define ZF_ZERO		0x10
#define ZF_PTR		0x20	// %p: 0x even for zero, as newlib prints it

#ifdef ZFMT_FLOAT

// 10^n, n >= 0: exact up to 10^22, a few ulp beyond.
static double zf_p10(int n) {
	double r = 1.0;
	while (n-- > 0) r *= 10.0;
	return r;
}

// a * b exactly, as *hi + *lo (Dekker; no FMA here, and none wanted).
static void zf_mul2(double a, double b, double *hi, double *lo) {
	double c, ah, al, bh, bl, s = 1.0;
	// the split multiplies by 2^27: keep that inside a double's range
	if (a > 1e290 || a < -1e290) { a *= 0x1p-64; s = 0x1p64; }
	if (b > 1e290 || b < -1e290) { b *= 0x1p-64; s *= 0x1p64; }
	c = 134217729.0 * a; ah = c - (c - a); al = a - ah;
	c = 134217729.0 * b; bh = c - (c - b); bl = b - bh;
	*hi = a * b;
	*lo = ((ah * bh - *hi) + ah * bl + al * bh) + al * bl;
	*hi *= s;
	*lo *= s;
}

// (ah + al) * (bh + bl), to about 106 bits, as *h + *l.
static void zf_dmul(double ah, double al, double bh, double bl, double *h, double *l) {
	double p, e;
	zf_mul2(ah, bh, &p, &e);
	e += ah * bl + al * bh;
	*h = p + e;
	*l = e - (*h - p);
}

// 10^n, 0 <= n <= 308, to about 106 bits: exact up to 10^44.
static void zf_dp10(int n, double *h, double *l) {
	*h = 1.0; *l = 0.0;
	while (n >= 22) { zf_dmul(*h, *l, 1e22, 0, h, l); n -= 22; }
	zf_dmul(*h, *l, zf_p10(n), 0, h, l);
}

// (ah + al) / 10^f, 0 <= f <= 308, as *h + *l.
static void zf_ddiv(double ah, double al, int f, double *h, double *l) {
	double th, tl, q, p, e;
	zf_dp10(f, &th, &tl);
	q = ah / th;
	zf_mul2(q, th, &p, &e);
	*h = q;
	*l = ((((ah - p) - e) + al) - q * tl) / th;
}

// d * 10^n, as *hi + *lo, carried to about 106 bits -- so the digits a
// double can have (19 asked of it at most) and the ties between them
// come out as the exact value's would, except within about 1e-30 of a
// tie that the exact value does not quite reach. Up to 10^44 the power
// of ten is itself exact.
static void zf_scale(double d, int n, double *hi, double *lo) {
	double h, l, e;
	if (n >= 0) {
		if (n > 290) {
			// a subnormal: in two steps, as 10^n would overflow
			zf_dp10(150, &h, &l);
			zf_dmul(d, 0, h, l, &d, &e);
			zf_dp10(n - 150, &h, &l);
			zf_dmul(d, e, h, l, hi, lo);
		} else {
			zf_dp10(n, &h, &l);
			zf_dmul(d, 0, h, l, hi, lo);
		}
		return;
	}
	zf_ddiv(d, 0, -n, hi, lo);
}

// floor(), for the small values zf_digits needs it for
static double zf_floor(double v) {
	double i = (double)(long long)v;
	return i > v ? i - 1 : i;
}

// `p` (1-19) significant digits of d (finite, > 0), rounded half to
// even, into dig; returns the decimal exponent of the first one.
static int zf_digits(double d, int p, char *dig) {
	union { double d; uint64_t u; } b = { d };
	int e2 = (int)((b.u >> 52) & 0x7FF) - 1023;
	int x = e2 >= 0 ? e2 * 30103 / 100000 : -((-e2 * 30103 + 99999) / 100000);
	uint64_t lo = 1, hi, n;
	double y, yl, f;
	for (int i = 1; i < p; i++) lo *= 10;
	hi = lo * 10;
	if (e2 == -1023) x = -308 - 16;		// subnormal: start low, step up
	for (;;) {
		zf_scale(d, p - 1 - x, &y, &yl);
		// against y + yl, not y: a value a hair under 10^k must not
		// pass for 10^k
		if (y > (double)hi || (y == (double)hi && yl >= 0)) x++;
		else if (y < (double)lo || (y == (double)lo && yl < 0)) x--;
		else break;
	}
	// the integer part, and what is left over, from y + yl
	n = (uint64_t)y;
	f = (y - (double)n) + yl;
	y = zf_floor(f);
	n += (uint64_t)(long long)y;
	f -= y;
	if (f > 0.5 || (f == 0.5 && (n & 1))) n++;
	if (n >= hi) { n /= 10; x++; }
	for (int i = p - 1; i >= 0; i--) { dig[i] = (char)('0' + n % 10); n /= 10; }
	return x;
}

// %e %f %g. Lays the number out as sign, integer digits, point and
// fraction digits, and an exponent, then pads it as zf_int pads.
static void zf_float(zf_out_t *o, double d, char conv, int flags, int width, int prec) {

	union { double d; uint64_t u; } b = { d };
	char dig[19], body[48];
	int neg = (int)(b.u >> 63), upper = conv <= 'Z', nd = 0, x = 0;
	int ip, fp, ex = 0, total, bn = 0;
	char lc = (char)(conv | 0x20), sign = 0;
	const char *special = NULL;

	if (neg) d = -d;
	if (neg) sign = '-';
	else if (flags & ZF_PLUS) sign = '+';
	else if (flags & ZF_SPACE) sign = ' ';
	if (prec < 0) prec = 6;

	if (d != d) special = upper ? "NAN" : "nan";
	else if (d > 1.7976931348623157e308) special = upper ? "INF" : "inf";
	if (special) {
		total = (sign ? 1 : 0) + 3;
		if (!(flags & ZF_LEFT)) zf_pad(o, ' ', width - total);
		if (sign) zf_put(o, sign);
		for (int i = 0; i < 3; i++) zf_put(o, special[i]);
		if (flags & ZF_LEFT) zf_pad(o, ' ', width - total);
		return;
	}

	// The digits: nd significant ones, the first at 10^x.
	if (lc == 'g' && prec == 0) prec = 1;
	if (lc == 'f') {
		// as many as reach the precision's last place
		if (d == 0) nd = 0;
		else {
			// where the first digit is, before any rounding to the
			// precision (19 digits cannot carry it a place)
			x = zf_digits(d, 19, dig);
			nd = x + 1 + prec;
			if (nd > 19) nd = 19;
			if (nd > 0) x = zf_digits(d, nd, dig);
			else if (nd == 0) {
				// below the last place: 0, or 1 there if over half of it
				double y, yl;
				zf_scale(d, prec, &y, &yl);
				if (y + yl > 0.5 || (y == 0.5 && yl > 0)) { dig[0] = '1'; nd = 1; x = -prec; }
			}
			if (nd < 0) nd = 0;
		}
	} else {
		nd = lc == 'e' ? prec + 1 : prec;
		if (nd > 19) nd = 19;
		if (d == 0) { for (int i = 0; i < nd; i++) dig[i] = '0'; x = 0; }
		else x = zf_digits(d, nd, dig);
		if (lc == 'g') {
			// %e's form for a small or large exponent, %f's otherwise
			if (x < -4 || x >= prec) { lc = 'e'; prec = prec - 1; }
			else { lc = 'f'; prec = prec - 1 - x; }
			if (!(flags & ZF_ALT)) {
				// %g drops trailing zeros, and the point with them
				int keep = nd;
				while (keep > 0 && dig[keep - 1] == '0') keep--;
				int fd = lc == 'e' ? keep - 1 : keep - 1 - x;
				if (fd < 0) fd = 0;
				if (fd < prec) prec = fd;
			}
		}
	}

	// Digit k of the number (k = 0 the units): from dig, else 0.
	#define ZF_DIG(k) ((x - (k)) >= 0 && (x - (k)) < nd ? dig[x - (k)] : '0')

	if (lc == 'e') { ex = (nd && d != 0) ? x : 0; x = ex; ip = 1; }
	else ip = x >= 0 ? x + 1 : 1;
	fp = prec;

	// The exponent's text, kept apart: it is at most "e-308".
	if (lc == 'e') {
		int a = ex < 0 ? -ex : ex;
		body[bn++] = upper ? 'E' : 'e';
		body[bn++] = ex < 0 ? '-' : '+';
		if (a >= 100) body[bn++] = (char)('0' + a / 100);
		body[bn++] = (char)('0' + a / 10 % 10);
		body[bn++] = (char)('0' + a % 10);
	}

	total = (sign ? 1 : 0) + ip + (fp || (flags & ZF_ALT) ? 1 + fp : 0) + bn;

	if (!(flags & ZF_LEFT) && !(flags & ZF_ZERO)) zf_pad(o, ' ', width - total);
	if (sign) zf_put(o, sign);
	if (!(flags & ZF_LEFT) && (flags & ZF_ZERO)) zf_pad(o, '0', width - total);
	if (lc == 'e') zf_put(o, nd ? dig[0] : '0');
	else for (int k = ip - 1; k >= 0; k--) zf_put(o, ZF_DIG(k));
	if (fp || (flags & ZF_ALT)) zf_put(o, '.');
	for (int k = 1; k <= fp; k++) zf_put(o, lc == 'e' ? (k < nd ? dig[k] : '0') : ZF_DIG(-k));
	for (int i = 0; i < bn; i++) zf_put(o, body[i]);
	if (flags & ZF_LEFT) zf_pad(o, ' ', width - total);

	#undef ZF_DIG
}

#endif

// One integer conversion. `neg` is the sign of a signed value whose
// magnitude is `v`.
static void zf_int(zf_out_t *o, unsigned long long v, int neg, int base,
	int upper, int flags, int width, int prec) {

	char digits[24];
	char prefix[3];
	int nd = 0, np = 0, zeros, total;
	const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	// Values that fit in 32 bits -- nearly all of them -- are divided
	// in 32 bits, so libgcc's 64-bit division is linked only by a
	// binary that really prints a 64-bit number.
	if (v >> 32) {
		while (v) { digits[nd++] = hex[v % (unsigned)base]; v /= (unsigned)base; }
	} else {
		uint32_t w = (uint32_t)v;
		while (w) { digits[nd++] = hex[w % (uint32_t)base]; w /= (uint32_t)base; }
	}

	// Precision is the minimum number of digits; the default is 1, and
	// "%.0d" of zero prints no digits at all.
	if (prec < 0) prec = 1;
	zeros = prec > nd ? prec - nd : 0;

	if (neg) prefix[np++] = '-';
	else if (flags & ZF_PLUS) prefix[np++] = '+';
	else if (flags & ZF_SPACE) prefix[np++] = ' ';

	if (flags & ZF_ALT) {
		// "%#o": the first digit is a 0, adding one only if needed.
		if (base == 8 && zeros == 0 && (nd == 0 || digits[nd - 1] != '0')) zeros = 1;
		// "%#x": 0x, only for a nonzero value.
		if (base == 16 && (nd || (flags & ZF_PTR))) {
			prefix[np++] = '0';
			prefix[np++] = upper ? 'X' : 'x';
		}
	}

	total = np + zeros + nd;

	if (!(flags & ZF_LEFT) && !(flags & ZF_ZERO)) zf_pad(o, ' ', width - total);
	for (int i = 0; i < np; i++) zf_put(o, prefix[i]);
	if (!(flags & ZF_LEFT) && (flags & ZF_ZERO)) zf_pad(o, '0', width - total);
	zf_pad(o, '0', zeros);
	while (nd) zf_put(o, digits[--nd]);
	if (flags & ZF_LEFT) zf_pad(o, ' ', width - total);

}

static void zf_str(zf_out_t *o, const char *s, int flags, int width, int prec) {
	int n = 0;
	if (!s) s = "(null)";
	while (s[n] && (prec < 0 || n < prec)) n++;
	if (!(flags & ZF_LEFT)) zf_pad(o, ' ', width - n);
	for (int i = 0; i < n; i++) zf_put(o, s[i]);
	if (flags & ZF_LEFT) zf_pad(o, ' ', width - n);
}

static void zf_format(zf_out_t *o, const char *fmt, va_list ap) {

	for (; *fmt; fmt++) {

		int flags = 0, width = 0, prec = -1, len = 0;
		char c;

		if (*fmt != '%') { zf_put(o, *fmt); continue; }
		fmt++;

		// flags
		for (;; fmt++) {
			if (*fmt == '-') flags |= ZF_LEFT;
			else if (*fmt == '+') flags |= ZF_PLUS;
			else if (*fmt == ' ') flags |= ZF_SPACE;
			else if (*fmt == '#') flags |= ZF_ALT;
			else if (*fmt == '0') flags |= ZF_ZERO;
			else break;
		}

		// width; a negative * means '-' and its magnitude
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) { flags |= ZF_LEFT; width = -width; }
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		}

		// precision; a negative * means none was given
		if (*fmt == '.') {
			fmt++;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				if (prec < 0) prec = -1;
				fmt++;
			} else {
				prec = 0;
				while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
			}
		}

		// length: 1 hh, 2 h, 3 l, 4 ll, 5 j, 6 z, 7 t, 8 L
		if (*fmt == 'h') { len = 2; if (*++fmt == 'h') { len = 1; fmt++; } }
		else if (*fmt == 'l') { len = 3; if (*++fmt == 'l') { len = 4; fmt++; } }
		else if (*fmt == 'j') { len = 5; fmt++; }
		else if (*fmt == 'z') { len = 6; fmt++; }
		else if (*fmt == 't') { len = 7; fmt++; }
		else if (*fmt == 'L') { len = 8; fmt++; }

		c = *fmt;
		if (!c) break;

		// '0' is ignored with '-', and for integers with a precision.
		if (flags & ZF_LEFT) flags &= ~ZF_ZERO;

		switch (c) {

		case 'd': case 'i': {
			long long v;
			switch (len) {
			case 1: v = (signed char)va_arg(ap, int); break;
			case 2: v = (short)va_arg(ap, int); break;
			case 3: v = va_arg(ap, long); break;
			case 4: v = va_arg(ap, long long); break;
			case 5: v = va_arg(ap, intmax_t); break;
			case 6: v = (long)va_arg(ap, size_t); break;
			case 7: v = va_arg(ap, ptrdiff_t); break;
			default: v = va_arg(ap, int); break;
			}
			if (prec >= 0) flags &= ~ZF_ZERO;
			// -(v+1)+1: the magnitude of LLONG_MIN without overflow
			zf_int(o, v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v,
				v < 0, 10, 0, flags, width, prec);
			break;
		}

		case 'u': case 'o': case 'x': case 'X': {
			unsigned long long v;
			switch (len) {
			case 1: v = (unsigned char)va_arg(ap, unsigned); break;
			case 2: v = (unsigned short)va_arg(ap, unsigned); break;
			case 3: v = va_arg(ap, unsigned long); break;
			case 4: v = va_arg(ap, unsigned long long); break;
			case 5: v = va_arg(ap, uintmax_t); break;
			case 6: v = va_arg(ap, size_t); break;
			case 7: v = (unsigned long long)va_arg(ap, ptrdiff_t); break;
			default: v = va_arg(ap, unsigned); break;
			}
			if (prec >= 0) flags &= ~ZF_ZERO;
			// only signed conversions take a sign
			flags &= ~(ZF_PLUS | ZF_SPACE);
			zf_int(o, v, 0, c == 'u' ? 10 : c == 'o' ? 8 : 16, c == 'X', flags, width, prec);
			break;
		}

		case 'p': {
			// newlib's form: 0x and the hex digits, 0x0 for NULL
			uintptr_t v = (uintptr_t)va_arg(ap, void *);
			zf_int(o, v, 0, 16, 0, (flags & (ZF_LEFT | ZF_ZERO)) | ZF_ALT | ZF_PTR,
				width, prec);
			break;
		}

		case 'c': {
			char ch = (char)va_arg(ap, int);
			if (!(flags & ZF_LEFT)) zf_pad(o, ' ', width - 1);
			zf_put(o, ch);
			if (flags & ZF_LEFT) zf_pad(o, ' ', width - 1);
			break;
		}

		case 's':
			zf_str(o, va_arg(ap, const char *), flags, width, prec);
			break;

		case 'n':
			switch (len) {
			case 1: *va_arg(ap, signed char *) = (signed char)o->len; break;
			case 2: *va_arg(ap, short *) = (short)o->len; break;
			case 3: *va_arg(ap, long *) = (long)o->len; break;
			case 4: *va_arg(ap, long long *) = (long long)o->len; break;
			case 6: *va_arg(ap, size_t *) = o->len; break;
			default: *va_arg(ap, int *) = (int)o->len; break;
			}
			break;

#ifdef ZFMT_FLOAT
		case 'f': case 'F': case 'e': case 'E':
		case 'g': case 'G': {
			double d;
			if (len == 8) d = (double)va_arg(ap, long double);
			else d = va_arg(ap, double);
			zf_float(o, d, c, flags, width, prec);
			break;
		}
		case 'a': case 'A':
#else
		case 'f': case 'F': case 'e': case 'E':
		case 'g': case 'G': case 'a': case 'A':
#endif
			// Not supported (see the top of this file); the argument is
			// still taken so the ones after it are read correctly.
			if (len == 8) (void)va_arg(ap, long double);
			else (void)va_arg(ap, double);
			zf_str(o, "?", flags, width, -1);
			break;

		case '%':
			zf_put(o, '%');
			break;

		default:
			// An unknown conversion is printed as written.
			zf_put(o, '%');
			zf_put(o, c);
			break;

		}

	}

}

// -- the string forms --

int ZF(vsnprintf)(char *buf, size_t cap, const char *fmt, va_list ap) {
	zf_out_t o;
	o.f = NULL; o.buf = buf; o.cap = cap; o.len = 0; o.tn = 0;
	zf_format(&o, fmt, ap);
	if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
	return (int)o.len;
}

int ZF(snprintf)(char *buf, size_t cap, const char *fmt, ...) {
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = ZF(vsnprintf)(buf, cap, fmt, ap);
	va_end(ap);
	return r;
}

int ZF(vsprintf)(char *buf, const char *fmt, va_list ap) {
	return ZF(vsnprintf)(buf, (size_t)-1 >> 1, fmt, ap);
}

int ZF(sprintf)(char *buf, const char *fmt, ...) {
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = ZF(vsprintf)(buf, fmt, ap);
	va_end(ap);
	return r;
}

// -- the FILE forms: formatted here, written by stdio --

int ZF(vfprintf)(FILE *f, const char *fmt, va_list ap) {
	zf_out_t o;
	o.f = f; o.buf = NULL; o.cap = 0; o.len = 0; o.tn = 0;
	zf_format(&o, fmt, ap);
	zf_flush(&o);
	return ferror(f) ? -1 : (int)o.len;
}

int ZF(fprintf)(FILE *f, const char *fmt, ...) {
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = ZF(vfprintf)(f, fmt, ap);
	va_end(ap);
	return r;
}

int ZF(vprintf)(const char *fmt, va_list ap) {
	return ZF(vfprintf)(stdout, fmt, ap);
}

int ZF(printf)(const char *fmt, ...) {
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = ZF(vfprintf)(stdout, fmt, ap);
	va_end(ap);
	return r;
}

#ifdef ZFMT_FLOAT

// -- strtod and atof, the other direction --
//
// With ZFMT_FLOAT, because the same scaling does the work and newlib's
// strtod is the rest of the multi-precision code (strtod, gdtoa-gethex
// and -hexnan, mprec: about 15 KB in repl). Decimal, inf, infinity and
// nan, as C99's strtod reads them -- but not C99's hexadecimal floats
// (0x1.8p1) or nan(chars), which no caller here writes: "0x10" reads
// as 0, ending at the x. The first 19 significant digits are used and the rest only
// counted, so a number written with more than that may round
// differently in its last bit; with 19 or fewer it is the nearest
// double, as newlib's is, except within about 1e-30 of a tie.

static int zf_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int zf_word(const char *s, const char *w) {
	int n = 0;
	while (w[n] && zf_lower((unsigned char)s[n]) == w[n]) n++;
	return w[n] ? 0 : n;
}

double ZF(strtod)(const char *str, char **end) {
	const char *s = str, *digits;
	uint64_t m = 0;
	int neg = 0, nd = 0, e = 0, any = 0, k;
	double v;

	while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
	if (*s == '-' || *s == '+') neg = *s++ == '-';

	if ((k = zf_word(s, "inf"))) {
		s += k;
		if ((k = zf_word(s, "inity"))) s += k;
		v = 1e308 * 10.0;
		goto done;
	}
	if ((k = zf_word(s, "nan"))) {
		s += k;
		v = 0.0 / 0.0;
		goto done;
	}

	// decimal: the digits, then the point's and the exponent's places
	digits = s;
	for (;; s++) {
		if (*s == '.' && !any) { any = 1; continue; }
		if (*s < '0' || *s > '9') break;
		if (nd < 19) { if (m || *s != '0') { m = m * 10 + (uint64_t)(*s - '0'); nd++; } if (any) e--; }
		else if (!any) e++;
	}
	if (s == digits || (s == digits + 1 && any)) {
		// no digits at all: no conversion
		if (end) *end = (char *)str;
		return 0.0;
	}
	if (zf_lower(*s) == 'e') {
		const char *t = s + 1;
		int en = 0, ee = 0;
		if (*t == '-' || *t == '+') en = *t++ == '-';
		if (*t >= '0' && *t <= '9') {
			while (*t >= '0' && *t <= '9') { if (ee < 100000) ee = ee * 10 + (*t - '0'); t++; }
			e += en ? -ee : ee;
			s = t;
		}
	}

	if (m == 0) v = 0.0;
	else if (e > 310 - nd + 19) v = 1e308 * 10.0;
	else if (e < -360) v = 0.0;
	else if (m < (1ull << 53) && e >= -22 && e <= 22) {
		// both exact: one correctly rounded step
		v = e >= 0 ? (double)m * zf_p10(e) : (double)m / zf_p10(-e);
	} else {
		// m as two doubles, scaled in two-double precision, rounded once
		double mh = (double)m, ml = (double)(long long)(m - (uint64_t)mh);
		double h, l, er;
		if (e >= 0) {
			zf_dp10(e > 308 ? 308 : e, &h, &l);
			zf_dmul(mh, ml, h, l, &v, &er);
			if (v <= 1.7976931348623157e308) v += er;	// not inf
			if (e > 308) v *= zf_p10(e - 308);
		} else {
			int f = -e;
			double s = 1.0;
			if (f > 300) {
				// two steps, the second scaled up by 2^64 so that its
				// remainder is not lost below the smallest normal double
				zf_ddiv(mh, ml, 300, &mh, &ml);
				mh *= 0x1p64; ml *= 0x1p64; s = 0x1p-64;
				f -= 300;
			}
			zf_ddiv(mh, ml, f, &v, &er);
			if (s == 1.0) v += er;
			else {
				// one rounding, to the subnormal grid if it is one
				double r = (v + er) * s;
				if (r < 0x1p-1022) {
					// half the subnormal step, in the scaled units
					double d, half = 0x1p-1011;
					long long odd;
					r = v * s;
					odd = (long long)(r * 0x1p600 * 0x1p474) & 1;
					d = (v - r * 0x1p64) + er;
					if (d > half || (d == half && odd)) r += 0x1p-1074;
					else if (d < -half || (d == -half && odd)) r -= 0x1p-1074;
				}
				v = r;
			}
		}
	}
	if (v > 1.7976931348623157e308 || (v == 0 && m != 0)) errno = ERANGE;

done:
	if (end) *end = (char *)s;
	return neg ? -v : v;
}

double ZF(atof)(const char *s) {
	return ZF(strtod)(s, NULL);
}

#endif
