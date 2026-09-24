#ifndef ZARGS_H
#define ZARGS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Splitting a command line into arguments, with quoting -- so that an
 * argument can hold a space, now that file names can (docs/sdcard.md,
 * "Long file names"). One tokenizer for every place that splits a
 * line: the posix shell, the kernel shell, and programs that split the
 * launch argument they were given (zcc, zfpga). docs/posix.md,
 * "Quoting".
 *
 * Header-only: the kernel includes it, and a header needs no Makefile
 * change and costs nothing it does not call.
 *
 * -- the rules, which are the shell's --
 *
 *   spaces and tabs   separate arguments
 *   'single quotes'   everything inside is literal
 *   "double quotes"   literal, except \" and \\, which are " and \
 *   \x                outside quotes: x, literally (\  is a space)
 *
 * A quote left open runs to the end of the line rather than being an
 * error: on a machine where the line came from a small editor, dropping
 * the whole command for a missing quote is worse than the obvious
 * reading. Nothing else is special -- no variables, no $(...).
 *
 * -- wildcards --
 *
 * * ? and [ are wildcards only when unquoted; posix expands them
 * (sw/common/zglob.h), nothing else does. So the tokenizer records, per
 * argument, whether it has an UNQUOTED wildcard (Z_ARG_WILD), and in
 * such an argument writes each QUOTED wildcard character after a
 * Z_ARG_LIT byte, so that the matcher can tell "*" from *. An argument
 * without Z_ARG_WILD never contains Z_ARG_LIT. One that has it must be
 * expanded or passed through z_args_unmark() before it is used.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Per-argument flags.
#define Z_ARG_QUOTED  0x01	// had quotes or a backslash: never an operator
#define Z_ARG_WILD    0x02	// has an unquoted * ? or [

// Marks the next byte as a literal wildcard in a Z_ARG_WILD argument.
// A control character no file name contains.
#define Z_ARG_LIT     '\x01'

static inline bool z_args_is_wild(char c) {
	return c == '*' || c == '?' || c == '[';
}

// Splits `line` into arguments, written into `buf` (capacity `cap`;
// twice the line's length is always enough). argv[i] points into buf;
// flags[i], if flags is not NULL, gets Z_ARG_*. Returns the count, at
// most `max`. `line` is not modified.
static inline int z_args_split(const char *line, char *buf, size_t cap,
	char **argv, uint8_t *flags, int max) {

	size_t o = 0;
	int n = 0;
	const char *p = line;

	if (!cap) return 0;

	while (*p && n < max) {

		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;

		size_t start = o;
		uint8_t fl = 0;
		bool had_lit = false;

		// Leave room for the NUL, and for a Z_ARG_LIT pair.
		#define PUT(ch) do { if (o + 1 < cap) buf[o++] = (ch); } while (0)
		#define PUT_LIT(ch) do { \
			if (z_args_is_wild(ch)) { if (o + 2 < cap) { buf[o++] = Z_ARG_LIT; buf[o++] = (ch); had_lit = true; } } \
			else PUT(ch); } while (0)

		while (*p && *p != ' ' && *p != '\t') {
			char c = *p;
			if (c == '\'') {
				fl |= Z_ARG_QUOTED;
				p++;
				while (*p && *p != '\'') { PUT_LIT(*p); p++; }
				if (*p) p++;
			} else if (c == '"') {
				fl |= Z_ARG_QUOTED;
				p++;
				while (*p && *p != '"') {
					if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
					PUT_LIT(*p);
					p++;
				}
				if (*p) p++;
			} else if (c == '\\' && p[1]) {
				fl |= Z_ARG_QUOTED;
				p++;
				PUT_LIT(*p);
				p++;
			} else {
				if (z_args_is_wild(c)) fl |= Z_ARG_WILD;
				PUT(c);
				p++;
			}
		}

		#undef PUT
		#undef PUT_LIT

		buf[o] = 0;

		// Not a wildcard argument: its marks mean nothing, drop them.
		if (had_lit && !(fl & Z_ARG_WILD)) {
			size_t w = start;
			for (size_t r = start; r < o; r++)
				if (buf[r] != Z_ARG_LIT) buf[w++] = buf[r];
			buf[w] = 0;
			o = w;
		}

		argv[n] = &buf[start];
		if (flags) flags[n] = fl;
		n++;
		if (o + 1 < cap) o++;		// past the NUL
		else break;

	}

	return n;

}

// Removes Z_ARG_LIT marks from an argument, in place -- for a wildcard
// argument used literally (no match, or no globbing here).
static inline void z_args_unmark(char *s) {
	char *w = s;
	for (; *s; s++) if (*s != Z_ARG_LIT) *w++ = *s;
	*w = 0;
}

// Given a pointer to a quote or backslash in a raw line, returns the
// pointer just past the quoted section or escaped character -- for a
// scanner looking for operators (| && || >) that must not find one
// inside quotes. Anything else: p + 1.
static inline const char *z_args_skip(const char *p) {
	if (*p == '\\') return p[1] ? p + 2 : p + 1;
	if (*p == '\'' || *p == '"') {
		char q = *p++;
		while (*p && *p != q) {
			if (q == '"' && *p == '\\' && p[1]) p++;
			p++;
		}
		return *p ? p + 1 : p;
	}
	return p + 1;
}

// Writes one argument quoted if it needs to be -- empty, or holding a
// space, tab, quote, backslash or wildcard -- so that z_args_split()
// gives it back exactly. Returns the bytes written (not counting the
// NUL), or -1 if it does not fit.
static inline int z_args_quote(const char *arg, char *out, size_t cap) {

	bool need = !*arg;
	bool squote = false;
	for (const char *s = arg; *s; s++) {
		if (*s == ' ' || *s == '\t' || *s == '"' || *s == '\\' || *s == '\'' ||
		    z_args_is_wild(*s)) need = true;
		if (*s == '\'') squote = true;
	}

	size_t o = 0;
	#define OUT(ch) do { if (o + 1 >= cap) return -1; out[o++] = (ch); } while (0)
	if (!need) {
		for (const char *s = arg; *s; s++) OUT(*s);
	} else if (!squote) {
		OUT('\'');
		for (const char *s = arg; *s; s++) OUT(*s);
		OUT('\'');
	} else {
		OUT('"');
		for (const char *s = arg; *s; s++) {
			if (*s == '"' || *s == '\\') OUT('\\');
			OUT(*s);
		}
		OUT('"');
	}
	#undef OUT
	if (!cap) return -1;
	out[o] = 0;
	return (int)o;

}

// Joins arguments into one line, each quoted only if it needs to be,
// so that z_args_split() of the result gives them back. Returns false
// if they do not all fit (what did fit is still terminated).
static inline bool z_args_join(int argc, char **argv, char *out, size_t cap) {
	size_t o = 0;
	if (!cap) return false;
	out[0] = 0;
	for (int i = 0; i < argc; i++) {
		if (i) { if (o + 2 >= cap) return false; out[o++] = ' '; out[o] = 0; }
		int k = z_args_quote(argv[i], out + o, cap - o);
		if (k < 0) { out[o] = 0; return false; }
		o += (size_t)k;
	}
	return true;
}

#endif
