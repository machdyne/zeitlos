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
 */

#include <stdio.h>
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

		case 'f': case 'F': case 'e': case 'E':
		case 'g': case 'G': case 'a': case 'A':
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
