#ifndef PRINT_OUT_H
#define PRINT_OUT_H

/*
 * print -- where bytes go: a socket to the printer, a file, a test's
 * buffer. Every writer in this app (PWG raster, PDF, PostScript, the
 * HTTP framing around them) writes through one of these, and does not
 * know which. docs/print.md, "Inside".
 */

#include <stdint.h>
#include <stdbool.h>

typedef struct out out_t;

struct out {
	// Takes all n bytes or fails; a sink that must wait (a socket whose
	// peer is slow) waits inside.
	bool		(*write)(out_t *o, const void *d, uint32_t n);
	void		*ctx;
	uint32_t	total;		// bytes taken so far
	bool		failed;
};

static inline bool out_put(out_t *o, const void *d, uint32_t n) {
	if (o->failed) return false;
	if (n && !o->write(o, d, n)) { o->failed = true; return false; }
	o->total += n;
	return true;
}

// Text, a number, a 24.8 number with two decimals (PDF and PostScript
// want decimals, and this app does not link printf).
bool out_str(out_t *o, const char *s);
bool out_int(out_t *o, int32_t v);
bool out_fix(out_t *o, int32_t v24_8);
// a 16.16 number with four decimals (transforms)
bool out_f16(out_t *o, int32_t v16_16);

// The same into a buffer: number text, NUL-terminated; returns length.
int fmt_int(char *b, int32_t v);
int fmt_dec(char *b, int64_t v, int frac_bits, int decimals);

#endif
