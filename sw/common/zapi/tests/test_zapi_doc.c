/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for zapi_doc.c, the lookup behind (help) and (apropos),
 * against the real zapi.txt that tools/gen_zapi.py makes (see the
 * Makefile beside this), read whole and a byte at a time.
 *
 *   make -C sw/common/zapi/tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../zapi_doc.h"

static int checks, failures;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} \
} while (0)

// -- the file, through a reader shaped like fs_read_chunk() --

typedef struct {
	const char *data;
	int len, pos;
	int step;		// most bytes per call
} src_t;

static int src_read(void *ctx, void *buf, int max) {
	src_t *s = ctx;
	int n = s->len - s->pos;
	if (n > max) n = max;
	if (n > s->step) n = s->step;
	memcpy(buf, s->data + s->pos, (size_t)n);
	s->pos += n;
	return n;
}

// -- stdout, caught --

static char out[65536];

typedef int (*lookup_fn)(const char *, zdoc_read_fn, void *);

static int run(lookup_fn fn, const char *arg, const char *data, int step) {
	src_t s = { data, (int)strlen(data), 0, step };
	fflush(stdout);
	int saved = dup(1);
	FILE *t = tmpfile();
	dup2(fileno(t), 1);
	int r = fn(arg, src_read, &s);
	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	long n = ftell(t);
	if (n < 0) n = 0;
	if (n > (long)sizeof(out) - 1) n = sizeof(out) - 1;
	rewind(t);
	n = (long)fread(out, 1, (size_t)n, t);
	out[n] = 0;
	fclose(t);
	return r;
}

static int lines_in(const char *s) {
	int n = 0;
	for (; *s; s++) if (*s == '\n') n++;
	return n;
}

// entries in the file, counted the plain way: a "(" line after a blank one
static int entries_in(const char *s) {
	int n = 0, prev_blank = 1;
	while (*s) {
		const char *e = strchr(s, '\n');
		if (!e) e = s + strlen(s);
		if (prev_blank && *s == '(') n++;
		prev_blank = (e == s);
		s = *e ? e + 1 : e;
	}
	return n;
}

static void test_file(const char *doc, int step) {

	int r;

	// a plain entry: both call shapes, the description, the example
	r = run(zdoc_help, "ls", doc, step);
	CHECK(r == lines_in(out), "help ls: returned %d for %d lines", r, lines_in(out));
	CHECK(!strncmp(out, "(ls)\n(ls \"path\")\n  ", 19), "help ls: starts wrong:\n%s", out);
	CHECK(strstr(out, "  > (ls ") != NULL, "help ls: no example:\n%s", out);
	CHECK(!strstr(out, "file-size"), "help ls: ran into the next entry:\n%s", out);

	// a name that is the second of an entry's call shapes
	r = run(zdoc_help, "mod", doc, step);
	CHECK(r > 0 && !strncmp(out, "(% a b)\n(mod a b)\n", 18), "help mod:\n%s", out);

	// deep in a long list of call shapes
	r = run(zdoc_help, "cdaddr", doc, step);
	CHECK(r > 28 && !strncmp(out, "(caar x)\n", 9) && strstr(out, "(cdaddr x)\n")
		&& strstr(out, "car and cdr combined"), "help cdaddr:\n%s", out);

	// operators, and names that are a prefix of others
	r = run(zdoc_help, "-", doc, step);
	CHECK(r > 0 && strstr(out, "(- n...)"), "help -:\n%s", out);
	r = run(zdoc_help, "c", doc, step);
	CHECK(r == 0 && !out[0], "help c: found something:\n%s", out);
	r = run(zdoc_help, "random", doc, step);
	CHECK(r > 0 && !strncmp(out, "(random)\n", 9) && !strstr(out, "random-hex"),
		"help random:\n%s", out);
	r = run(zdoc_help, "uart1?", doc, step);
	CHECK(r > 0 && !strncmp(out, "(uart1?)\n", 9), "help uart1?:\n%s", out);

	// the last entry in the file
	r = run(zdoc_help, "uart1-status", doc, step);
	CHECK(r > 0, "help uart1-status: not found");
	r = run(zdoc_help, "random-stir", doc, step);
	CHECK(r > 0 && strstr(out, "  > (random-stir"), "help random-stir:\n%s", out);

	// nothing for a name with no entry, or for words in the header
	r = run(zdoc_help, "frobnicate", doc, step);
	CHECK(r == 0 && !out[0], "help frobnicate:\n%s", out);
	r = run(zdoc_help, "ZAPI", doc, step);
	CHECK(r == 0, "help ZAPI found the header");

	// apropos: a line per match, the name first
	r = run(zdoc_apropos, "uart1", doc, step);
	CHECK(r == 8 && lines_in(out) == 8, "apropos uart1: %d\n%s", r, out);
	CHECK(!strncmp(out, "uart1?           ", 17), "apropos uart1: first line\n%s", out);
	r = run(zdoc_apropos, "UART1-OPEN", doc, step);
	CHECK(r >= 1 && strstr(out, "uart1-open "), "apropos is case-sensitive:\n%s", out);
	r = run(zdoc_apropos, "zzqq", doc, step);
	CHECK(r == 0 && !out[0], "apropos zzqq:\n%s", out);

	// examples are not searched: "Zeitlos" is only in string-append's
	r = run(zdoc_apropos, "Zeitlos", doc, step);
	CHECK(r == 0, "apropos searched an example:\n%s", out);

	// every line fits 80 columns
	for (const char *p = out, *e; *p; p = e + 1) {
		e = strchr(p, '\n');
		if (!e) break;
		CHECK(e - p <= 80, "apropos line of %d: %.*s", (int)(e - p), (int)(e - p), p);
	}

	// everything, capped
	int all = entries_in(doc);
	r = run(zdoc_apropos, "", doc, step);
	CHECK(r == all, "apropos \"\": %d of %d entries", r, all);
	CHECK(lines_in(out) == ZDOC_APROPOS_MAX + 1 && strstr(out, "... and "),
		"apropos \"\": %d lines", lines_in(out));

}

// the reader's edge cases, on a file made here
static void test_format(void) {

	char lng[400];
	memset(lng, 'x', sizeof(lng));
	lng[sizeof(lng) - 1] = 0;

	char doc[2048];
	snprintf(doc, sizeof(doc),
		"A header line\r\n"
		"(not an entry: no blank line before it)\r\n"
		"\r\n"
		"(alpha x)\r\n"
		"  First.\r\n"
		"  > (alpha 1)  =>  1\r\n"
		"\r\n"
		"(beta)\n"
		"  %s\n"
		"\n"
		"(gamma)\n"
		"  Last, with no newline at the end.", lng);

	int r = run(zdoc_help, "alpha", doc, 256);
	CHECK(r == 3 && !strcmp(out, "(alpha x)\n  First.\n  > (alpha 1)  =>  1\n"),
		"CRLF entry:\n%s", out);
	r = run(zdoc_help, "not", doc, 256);
	CHECK(r == 0, "a ( line not after a blank line was taken for an entry");

	r = run(zdoc_help, "beta", doc, 7);
	CHECK(r == 2 && strlen(out) == strlen("(beta)\n") + ZDOC_LINE - 1 + 1,
		"long line not cut at %d: %d bytes", ZDOC_LINE - 1, (int)strlen(out));

	r = run(zdoc_help, "gamma", doc, 3);
	CHECK(r == 2 && strstr(out, "at the end.\n"), "last line:\n%s", out);

	r = run(zdoc_apropos, "", doc, 5);
	CHECK(r == 3, "apropos on the small file: %d\n%s", r, out);
	CHECK(strstr(out, "beta             xxxx") && strstr(out, "xxx...\n"),
		"long description not cut with ...:\n%s", out);

	r = run(zdoc_help, "x", "", 256);
	CHECK(r == 0, "empty file");

}

int main(int argc, char **argv) {

	const char *path = argc > 1 ? argv[1] : "/tmp/zapi_test.txt";
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 2; }
	static char doc[200000];
	size_t n = fread(doc, 1, sizeof(doc) - 1, f);
	doc[n] = 0;
	fclose(f);

	test_file(doc, 256);
	test_file(doc, 1);
	test_file(doc, 100);
	test_format();

	printf("test_zapi_doc: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
