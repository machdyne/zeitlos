/*
 * Host test for sw/common/zfmt.c -- docs/build.md, "Integer-only
 * printf (ZFMT)".
 *
 *   cc -std=gnu99 -Wall -DZFMT_HOST_TEST -I sw/common \
 *      -o /tmp/test_zfmt sw/common/tests/test_zfmt.c sw/common/zfmt.c \
 *      && /tmp/test_zfmt
 *
 * zfmt.c is built with its names prefixed (zf_printf, ...) so the build
 * machine's own printf is still there to compare against and report
 * with. Every integer conversion is checked against it EXHAUSTIVELY:
 * all 32 combinations of the five flags, several widths and precisions
 * (and both as '*'), every length modifier, every conversion, over
 * values at every edge a formatter gets wrong -- 0, one, minus one, the
 * limits of each width, and 64-bit numbers that need the 64-bit path.
 * C99 fixes the output of all of those, and zfmt.c claims C99's.
 *
 * Built with -DZFMT_FLOAT as well (the second command below), it checks
 * %e %f %g against the build machine's printf too: fixed values at the
 * edges, and a million random ones. See zfmt.c's ZFMT_FLOAT note for
 * what is exact and what is not.
 *
 *   cc -std=gnu99 -Wall -DZFMT_HOST_TEST -DZFMT_FLOAT -I sw/common \
 *      -o /tmp/test_zfmt_f sw/common/tests/test_zfmt.c sw/common/zfmt.c -lm \
 *      && /tmp/test_zfmt_f
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>
#ifdef ZFMT_FLOAT
#include <stdlib.h>
#include <math.h>
#endif

int zf_snprintf(char *buf, size_t cap, const char *fmt, ...);
int zf_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int zf_sprintf(char *buf, const char *fmt, ...);
int zf_fprintf(FILE *f, const char *fmt, ...);
double zf_strtod(const char *s, char **end);

static long checks, fails;

static void same(const char *fmt, const char *got, int gr, const char *want, int wr) {
	checks++;
	if (strcmp(got, want) || gr != wr) {
		if (fails++ < 20)
			printf("FAIL %-14s got [%s] %d, want [%s] %d\n", fmt, got, gr, want, wr);
	}
}

static void want(const char *label, const char *got, const char *expected) {
	checks++;
	if (strcmp(got, expected)) {
		fails++;
		printf("FAIL %s: got [%s], want [%s]\n", label, got, expected);
	}
}

int main(void) {

	static const char *flagset = "-+ #0";
	static const char *widths[] = { "", "1", "6", "23" };
	static const char *precs[] = { "", ".", ".0", ".1", ".4", ".22" };
	static const char *lens[] = { "hh", "h", "", "l", "ll", "j", "z", "t" };
	static const char convs[] = "diuoxX";
	static const long long vals[] = {
		0, 1, -1, 7, 8, 9, 10, 15, 16, 127, -128, 128, 255, 256,
		32767, -32768, 65535, 65536, INT_MAX, INT_MIN, (long long)UINT_MAX,
		4294967296LL, 1099511627776LL, -1099511627777LL, LLONG_MAX, LLONG_MIN,
	};
	char fmt[48], a[256], b[256];

	// -- integers: every combination --
	for (int fl = 0; fl < 32; fl++)
	for (unsigned w = 0; w < sizeof(widths) / sizeof(*widths); w++)
	for (unsigned p = 0; p < sizeof(precs) / sizeof(*precs); p++)
	for (unsigned l = 0; l < sizeof(lens) / sizeof(*lens); l++)
	for (const char *c = convs; *c; c++)
	for (unsigned v = 0; v < sizeof(vals) / sizeof(*vals); v++) {
		int n = 0, ra, rb;
		fmt[n++] = '%';
		for (int i = 0; i < 5; i++) if (fl & (1 << i)) fmt[n++] = flagset[i];
		n += sprintf(fmt + n, "%s%s%s%c", widths[w], precs[p], lens[l], *c);
		long long x = vals[v];
		// Each length modifier reads its own type; pass that type.
		switch (l) {
		case 0: case 1: case 2:
			ra = zf_snprintf(a, sizeof(a), fmt, (int)x); rb = snprintf(b, sizeof(b), fmt, (int)x); break;
		case 3:
			ra = zf_snprintf(a, sizeof(a), fmt, (long)x); rb = snprintf(b, sizeof(b), fmt, (long)x); break;
		case 4:
			ra = zf_snprintf(a, sizeof(a), fmt, x); rb = snprintf(b, sizeof(b), fmt, x); break;
		case 5:
			ra = zf_snprintf(a, sizeof(a), fmt, (intmax_t)x); rb = snprintf(b, sizeof(b), fmt, (intmax_t)x); break;
		case 6:
			ra = zf_snprintf(a, sizeof(a), fmt, (size_t)x); rb = snprintf(b, sizeof(b), fmt, (size_t)x); break;
		default:
			ra = zf_snprintf(a, sizeof(a), fmt, (ptrdiff_t)x); rb = snprintf(b, sizeof(b), fmt, (ptrdiff_t)x); break;
		}
		same(fmt, a, ra, b, rb);
	}
	printf("integers: %ld combinations\n", checks);

	// -- strings and characters --
	{
		static const char *sf[] = { "%s", "%10s", "%-10s", "%.3s", "%10.3s", "%-10.3s",
			"%.0s", "%.20s", "%1s", "%c", "%5c", "%-5c", "[%s|%c]" };
		static const char *sv[] = { "", "a", "hello", "exactly ten", "a longer string here" };
		for (unsigned i = 0; i < sizeof(sf) / sizeof(*sf); i++)
		for (unsigned j = 0; j < sizeof(sv) / sizeof(*sv); j++) {
			int ra, rb;
			if (strchr(sf[i], 'c') && !strchr(sf[i], 's')) {
				ra = zf_snprintf(a, sizeof(a), sf[i], sv[j][0] ? sv[j][0] : 'Z');
				rb = snprintf(b, sizeof(b), sf[i], sv[j][0] ? sv[j][0] : 'Z');
			} else if (strchr(sf[i], 'c')) {
				ra = zf_snprintf(a, sizeof(a), sf[i], sv[j], 'q');
				rb = snprintf(b, sizeof(b), sf[i], sv[j], 'q');
			} else {
				ra = zf_snprintf(a, sizeof(a), sf[i], sv[j]);
				rb = snprintf(b, sizeof(b), sf[i], sv[j]);
			}
			same(sf[i], a, ra, b, rb);
		}
		zf_snprintf(a, sizeof(a), "%s", (char *)NULL);
		want("NULL string", a, "(null)");
	}

	// -- '*' width and precision, including negative --
	{
		static const int ws[] = { -8, -1, 0, 3, 9 };
		for (unsigned i = 0; i < 5; i++) for (unsigned j = 0; j < 5; j++) {
			int ra = zf_snprintf(a, sizeof(a), "%*.*d|%*.*s", ws[i], ws[j], 42, ws[i], ws[j], "text");
			int rb = snprintf(b, sizeof(b), "%*.*d|%*.*s", ws[i], ws[j], 42, ws[i], ws[j], "text");
			same("%*.*d|%*.*s", a, ra, b, rb);
		}
	}

	// -- %% %n and unknown conversions --
	{
		int n1 = -1, n2 = -1, n3 = -1;
		zf_snprintf(a, sizeof(a), "100%% done%n, %d%n", &n1, 5, &n2);
		want("%%", a, "100% done, 5");
		checks++; if (n1 != 9 || n2 != 12) { fails++; printf("FAIL %%n: %d %d\n", n1, n2); }
		zf_snprintf(a, 4, "abcdefgh%n", &n3);
		checks++; if (n3 != 8) { fails++; printf("FAIL %%n past truncation: %d\n", n3); }
		zf_snprintf(a, sizeof(a), "%q%d", 7);
		want("unknown conversion", a, "%q7");
	}

	// -- %p: newlib's form, 0x and hex, 0x0 for NULL --
	{
		zf_snprintf(a, sizeof(a), "%p", (void *)0x4000abcdUL);
		want("%p", a, "0x4000abcd");
		zf_snprintf(a, sizeof(a), "%p", (void *)0);
		want("%p NULL", a, "0x0");
		zf_snprintf(a, sizeof(a), "[%12p]", (void *)0x1234UL);
		want("%12p", a, "[      0x1234]");
		zf_snprintf(a, sizeof(a), "[%-8p]", (void *)0x12UL);
		want("%-8p", a, "[0x12    ]");
	}

#ifndef ZFMT_FLOAT
	// -- floating point is not supported, but its argument is taken --
	{
		zf_snprintf(a, sizeof(a), "%d %f %d %.3e %d %Lg %d", 1, 2.5, 3, 4.5, 5, (long double)6.5, 7);
		want("floats consumed", a, "1 ? 3 ? 5 ? 7");
		zf_snprintf(a, sizeof(a), "[%5f]", 1.0);
		want("float with width", a, "[    ?]");
	}
#else
	// -- ZFMT_FLOAT: %e %f %g, against the host's printf --
	{
		static const char *ff[] = {
			"%g", "%e", "%f", "%.3g", "%.10g", "%.15g", "%.0e", "%.0f", "%#.0f",
			"%+g", "% e", "%12.4f", "%-12.3e|", "%012g", "%G", "%E", "%.1f",
			"%.2f", "%.0g", "%#.3g", "%.17g", "%.19g", "%-+9.2f|",
		};
		static const double fv[] = {
			0, -0.0, 1, -1, 0.5, 1.5, 2.5, 0.1, 0.15, 0.125, 0.375, 3.14159,
			2.718281828459045, 1e-5, 1e-4, 1e-3, 123456, 1234567, 9.9999996,
			0.000099999, 100, 1000000, 1e15, 1e16, 1e17, 1e21, 1e22, 12345.678,
			-0.0001234, 4.93245, 1.0 / 3, 2.0 / 3, 0.995, 9.5, 99.5, 1e-20,
			1e-100, 1e-300, 5e-324, 2.2250738585072014e-308, 1e300,
			INFINITY, -INFINITY, NAN,
		};
		char fa[400], fb[400];
		for (unsigned i = 0; i < sizeof(ff) / sizeof(ff[0]); i++)
			for (unsigned j = 0; j < sizeof(fv) / sizeof(fv[0]); j++) {
				// %f past 19 digits is zeros (zfmt.c)
				if (strchr(ff[i], 'f') && fabs(fv[j]) >= 1e19) continue;
				int ra = zf_snprintf(fa, sizeof(fa), ff[i], fv[j]);
				int rb = snprintf(fb, sizeof(fb), ff[i], fv[j]);
				same(ff[i], fa, ra, fb, rb);
			}
		// Random values over the whole range, and short decimals: all
		// must match.
		static const char *rf[] = { "%g", "%.3g", "%e", "%.10g", "%.15g", "%.4f" };
		srand(1);
		for (int k = 0; k < 200000; k++) {
			double d = ((double)rand() / RAND_MAX - 0.5) * pow(10, rand() % 601 - 300);
			if (k & 1) d = (rand() % 2000000 - 1000000) / pow(10, rand() % 8);
			for (unsigned i = 0; i < sizeof(rf) / sizeof(rf[0]); i++) {
				// %f past 19 digits is zeros, not the binary value's
				// exact expansion (zfmt.c)
				if (rf[i][strlen(rf[i]) - 1] == 'f' && fabs(d) >= 1e15) continue;
				int ra = zf_snprintf(fa, sizeof(fa), rf[i], d);
				int rb = snprintf(fb, sizeof(fb), rf[i], d);
				same(rf[i], fa, ra, fb, rb);
			}
		}
		zf_snprintf(a, sizeof(a), "%d %La %d", 1, (long double)6.5, 7);
		want("%a is still not supported", a, "1 ? 7");

		// strtod: the same double, and the same end, as the host's
		static const char *sv[] = {
			"0", "1", "-1", "+3.14", "3.14159", "1e10", "1e-10", "1E+5", " \t42x",
			".5", "5.", "-.5e-3", "1e308", "1e309", "1e-320", "4.9e-324",
			"2.2250738585072014e-308", "123456789012345678901234567890",
			"0.000000000000000000000000000001", "inf", "-Infinity", "nan",
			// (no hex floats or nan(chars): zfmt.c reads neither)
			"nanx", "x",
			"", ".", "-", "+.e5", "1e", "1e+", "12abc", "1.5e3.2", "192.168.1.1",
			"9007199254740993", "0.1", "0.3", "1.7976931348623157e308",
			"1.7976931348623159e308", "5e-324", "1e23", "8.98846567431158e307",
		};
		for (unsigned i = 0; i < sizeof(sv) / sizeof(sv[0]); i++) {
			char *e1, *e2;
			double x1 = zf_strtod(sv[i], &e1), x2 = strtod(sv[i], &e2);
			checks++;
			if ((memcmp(&x1, &x2, sizeof(x1)) && !(x1 != x1 && x2 != x2)) || e1 != e2) {
				fails++;
				printf("FAIL strtod [%s]: got %.17g (+%d), want %.17g (+%d)\n",
					sv[i], x1, (int)(e1 - sv[i]), x2, (int)(e2 - sv[i]));
			}
		}
		// ... and every double printed with 17, 15 and 6 digits reads
		// back as the host reads it
		srand(2);
		for (int k = 0; k < 300000; k++) {
			uint64_t u = ((uint64_t)rand() << 33) ^ ((uint64_t)rand() << 11) ^ (uint64_t)rand();
			double d, x1, x2;
			memcpy(&d, &u, sizeof(d));
			if (d != d || d - d != 0) continue;
			snprintf(fa, sizeof(fa), k % 3 == 0 ? "%.17g" : k % 3 == 1 ? "%.15g" : "%.6g", d);
			x1 = zf_strtod(fa, NULL); x2 = strtod(fa, NULL);
			checks++;
			if (memcmp(&x1, &x2, sizeof(x1))) {
				if (fails++ < 20)
					printf("FAIL strtod [%s]: got %.17g, want %.17g\n", fa, x1, x2);
			}
		}
	}
#endif

	// -- truncation and the return value --
	{
		int r = zf_snprintf(a, 6, "%s", "hello world");
		want("truncated", a, "hello");
		checks++; if (r != 11) { fails++; printf("FAIL return on truncation: %d\n", r); }
		memset(a, 'x', 8);
		r = zf_snprintf(a, 0, "%d", 12345);
		checks++; if (r != 5 || a[0] != 'x') { fails++; printf("FAIL cap 0 wrote or returned %d\n", r); }
		r = zf_snprintf(a, 1, "%d", 12345);
		checks++; if (r != 5 || a[0] != 0) { fails++; printf("FAIL cap 1\n"); }
		r = zf_sprintf(a, "%08.3x|%-6d|", 0xab, -42);
		want("sprintf", a, "     0ab|-42   |");
	}

	// -- a FILE: through stdio, in order with everything else on it --
	{
		static char mem[4096];
		FILE *f = fmemopen(mem, sizeof(mem), "w");
		setvbuf(f, NULL, _IOLBF, 64);		// line-buffered, as stdout is
		fputs("start ", f);
		zf_fprintf(f, "[%d]", 1);
		putc('/', f);
		// longer than zfmt.c's 64-byte staging buffer: several fwrite()s
		zf_fprintf(f, "%s%0100d", " long:", 7);
		fputs(" end\n", f);
		zf_fprintf(f, "%s", "");
		fflush(f);
		fclose(f);
		snprintf(b, sizeof(b), "start [1]/ long:%0100d end\n", 7);
		want("FILE output in order", mem, b);
	}

	printf("test_zfmt: %ld checks, %ld failed\n", checks, fails);
	return fails ? 1 : 0;

}
