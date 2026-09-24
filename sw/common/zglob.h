#ifndef ZGLOB_H
#define ZGLOB_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Matching a file name against a shell wildcard pattern -- posix's
 * globbing (docs/posix.md, "Wildcards"). Header-only.
 *
 *   *        any run of characters, including none
 *   ?        exactly one character (one character, not one byte: ü and
 *            日 are one each -- names are UTF-8)
 *   [abc]    one of the characters listed; [a-z] a range; [!abc] or
 *            [^abc] anything but
 *
 * A Z_ARG_LIT byte (zargs.h) makes the character after it literal: it
 * is how a quoted * reaches here from the tokenizer.
 *
 * ASCII letters match without regard to case. FAT names are case-
 * insensitive -- `README.TXT` and `readme.txt` are the same file -- and
 * 8.3 names are stored in capitals, so a case-sensitive `*.txt` would
 * miss every file named in the old style.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zargs.h"
#include "zutf8.h"

static inline uint32_t z_glob_fold(uint32_t c) {
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

// One character of a NUL-terminated string, advancing *s.
static inline uint32_t z_glob_next(const char **s) {
	return z_utf8_next_z(s);
}

// Matches the class at *pp (just past the '['); advances *pp past the
// closing ']'. An unclosed '[' is a literal '[', which is what
// z_glob_match() treats it as when this returns -1.
static inline int z_glob_class(const char **pp, uint32_t c) {

	const char *p = *pp;
	bool neg = false, hit = false;

	if (*p == '!' || *p == '^') { neg = true; p++; }

	// A ']' first in the class is a member, not the end.
	bool first = true;
	while (*p && (*p != ']' || first)) {
		first = false;
		if (*p == Z_ARG_LIT && p[1]) p++;
		uint32_t lo = z_glob_next(&p), hi = lo;
		if (*p == '-' && p[1] && p[1] != ']') {
			p++;
			if (*p == Z_ARG_LIT && p[1]) p++;
			hi = z_glob_next(&p);
		}
		uint32_t fc = z_glob_fold(c);
		if ((c >= lo && c <= hi) || (fc >= z_glob_fold(lo) && fc <= z_glob_fold(hi)))
			hit = true;
	}

	if (*p != ']') return -1;
	*pp = p + 1;
	return hit != neg;

}

// True if `name` matches `pat`.
static inline bool z_glob_match(const char *pat, const char *name) {

	// Iterative, with one backtrack point for the last *: linear for
	// the patterns people type, and no recursion to overflow a stack.
	const char *p = pat, *n = name;
	const char *star_p = NULL, *star_n = NULL;

	while (*n) {

		const char *pn = n;
		uint32_t c = z_glob_next(&pn);

		if (*p == '*') {
			while (*p == '*') p++;
			star_p = p;
			star_n = n;
			continue;
		}

		if (*p == '?') {
			p++;
			n = pn;
			continue;
		}

		if (*p == '[') {
			const char *q = p + 1;
			int r = z_glob_class(&q, c);
			if (r == 1) { p = q; n = pn; continue; }
			if (r == 0) goto mismatch;
			// unclosed: a literal '['
		}

		{
			const char *pq = p;
			if (*pq == Z_ARG_LIT && pq[1]) pq++;
			if (*pq) {
				uint32_t pc = z_glob_next(&pq);
				if (z_glob_fold(pc) == z_glob_fold(c)) { p = pq; n = pn; continue; }
			}
		}

	mismatch:
		if (!star_p) return false;
		// The last * takes one more character, and the rest is tried
		// again from there.
		const char *sn = star_n;
		z_glob_next(&sn);
		star_n = sn;
		n = sn;
		p = star_p;

	}

	while (*p == '*') p++;
	return !*p;

}

// True if an argument from z_args_split() with Z_ARG_WILD holds a
// wildcard the shell should expand (it always does -- the flag says
// so -- but a pattern reaching here from elsewhere may not).
static inline bool z_glob_has_wild(const char *s) {
	for (; *s; s++) {
		if (*s == Z_ARG_LIT) { if (s[1]) s++; continue; }
		if (z_args_is_wild(*s)) return true;
	}
	return false;
}

#endif
