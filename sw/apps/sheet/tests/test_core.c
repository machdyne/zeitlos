/*
 * Tests for sw/apps/sheet/sheet_core.c. Runs on the HOST:
 *
 *   cd sw/apps/sheet && make test
 *
 * A spreadsheet that is subtly wrong is worse than no spreadsheet --
 * nobody re-checks its arithmetic, which is the entire point of having
 * one. sheet_core.c is deliberately free of drawing, windows and
 * messages so that this can exist.
 *
 * The tests are written against the cell grid rather than against the
 * parser directly, because that is where the interesting failures are:
 * a formula is only correct in the context of the cells it reads, and
 * calling parse_expr() in isolation would never catch a stale cached
 * value or a range that walks the wrong row.
 */

#include <stdio.h>
#include <string.h>

#include "../sheet_core.h"

static int checks, fails;

static sheet_t sh;

static void ck(const char *what, const char *got, const char *want) {

	checks++;

	if (strcmp(got, want)) {
		printf("  FAIL  %-28s got \"%s\"  want \"%s\"\n", what, got, want);
		fails++;
	}

}

static void ck_int(const char *what, long got, long want) {

	checks++;

	if (got != want) {
		printf("  FAIL  %-28s got %ld  want %ld\n", what, got, want);
		fails++;
	}

}

// Sets A1 to `formula` and reports what the cell shows in a wide
// column -- the whole round trip, evaluation and formatting together.
static const char *shows(const char *formula) {

	static char out[64];

	sheet_set(&sh, 0, 0, formula);
	sheet_display(&sh, 0, 0, out, sizeof(out), 40, NULL);

	return out;

}

static void eq(const char *formula, const char *want) {
	ck(formula, shows(formula), want);
}

// -- a buffer emitter, standing in for fs_write_chunk() --

typedef struct { char buf[8192]; int n; } sink_t;

static bool sink_emit(void *ctx, const char *s, int len) {

	sink_t *k = ctx;

	if (len < 0) len = (int)strlen(s);
	if (k->n + len >= (int)sizeof(k->buf)) return false;

	memcpy(k->buf + k->n, s, (size_t)len);
	k->n += len;
	k->buf[k->n] = 0;

	return true;

}

// Feeds a whole text to the line-at-a-time loader, the way sheet.c's
// chunked reader will.
static void load_text(sheet_t *s, const char *text) {

	char line[256];
	int n = 0;

	sheet_load_begin(s);

	for (const char *p = text;; p++) {

		if (*p == '\n' || !*p) {
			line[n] = 0;
			sheet_load_line(s, line);
			n = 0;
			if (!*p) break;
			continue;
		}

		if (n < (int)sizeof(line) - 1) line[n++] = *p;

	}

	sheet_load_end(s);

}

int main(void) {

	char buf[64];

	// -- references ------------------------------------------------

	printf("references:\n");

	{
		int r, c;
		const char *end;

		ck_int("parse A1", sheet_parse_ref("A1", &r, &c, &end), 1);
		ck_int("  A1 row", r, 0);
		ck_int("  A1 col", c, 0);

		sheet_parse_ref("Z999", &r, &c, &end);
		ck_int("Z999 row", r, 998);
		ck_int("Z999 col", c, 25);

		sheet_parse_ref("$b$7", &r, &c, &end);
		ck_int("$b$7 row", r, 6);
		ck_int("$b$7 col", c, 1);

		ck_int("AA1 refused", sheet_parse_ref("AA1", &r, &c, &end), 0);
		ck_int("A0 refused", sheet_parse_ref("A0", &r, &c, &end), 0);
		ck_int("A refused", sheet_parse_ref("A", &r, &c, &end), 0);
		ck_int("1A refused", sheet_parse_ref("1A", &r, &c, &end), 0);
		ck_int("A1000 refused", sheet_parse_ref("A1000", &r, &c, &end), 0);

		sheet_ref_name(0, 0, buf, sizeof(buf));   ck("name(0,0)", buf, "A1");
		sheet_ref_name(998, 25, buf, sizeof(buf));ck("name(998,25)", buf, "Z999");
		sheet_ref_name(11, 2, buf, sizeof(buf));  ck("name(11,2)", buf, "C12");
	}

	// -- entry and classification ----------------------------------

	printf("entry:\n");

	sheet_init(&sh);

	sheet_set(&sh, 0, 0, "12.5");
	ck_int("12.5 is a number", sheet_kind(&sh, 0, 0), SHEET_KIND_NUM);

	sheet_set(&sh, 0, 0, "hello");
	ck_int("hello is text", sheet_kind(&sh, 0, 0), SHEET_KIND_TEXT);

	sheet_set(&sh, 0, 0, "=1+1");
	ck_int("=1+1 is a formula", sheet_kind(&sh, 0, 0), SHEET_KIND_FORMULA);

	sheet_set(&sh, 0, 0, "3 apples");
	ck_int("3 apples is text", sheet_kind(&sh, 0, 0), SHEET_KIND_TEXT);

	sheet_set(&sh, 0, 0, "'007");
	ck_int("'007 is text", sheet_kind(&sh, 0, 0), SHEET_KIND_TEXT);
	sheet_display(&sh, 0, 0, buf, sizeof(buf), 20, NULL);
	ck("'007 shows as 007", buf, "007");
	ck("'007 keeps its source", sheet_src(&sh, 0, 0), "'007");

	sheet_set(&sh, 0, 0, "  trailing   ");
	ck("trailing space dropped", sheet_src(&sh, 0, 0), "  trailing");

	sheet_set(&sh, 0, 0, NULL);
	ck_int("cleared", sheet_empty(&sh, 0, 0), 1);
	ck("cleared source", sheet_src(&sh, 0, 0), "");

	// -- arithmetic ------------------------------------------------

	printf("arithmetic:\n");

	sheet_init(&sh);

	eq("=1+1", "2");
	eq("=10-4", "6");
	eq("=6*7", "42");
	eq("=1/8", "0.125");
	eq("=1/3", "0.3333");
	eq("=2+3*4", "14");
	eq("=(2+3)*4", "20");
	eq("=-5+2", "-3");
	eq("=--5", "5");
	eq("=10/4", "2.5");
	eq("=0.1+0.2", "0.3");
	eq("=1.5*1.5", "2.25");
	eq("=100*100*100", "1000000");
	eq("=2*-3", "-6");
	eq("= 1 + 2 ", "3");

	// Plain values, not formulas -- the same display path.
	eq("42", "42");
	eq("-0.5", "-0.5");
	eq("1.00000", "1");
	eq("00012", "12");

	// -- errors ----------------------------------------------------

	printf("errors:\n");

	eq("=1/0", "#DIV0");
	eq("=1+", "#ERR");
	eq("=(1+2", "#ERR");
	eq("=FOO(1)", "#NAME");
	eq("=1 2", "#ERR");
	// A two-letter column is a reference to somewhere this grid does
	// not have, which is a better answer than "syntax error".
	eq("=AA1", "#REF");
	eq("=ABS(1,2)", "#NUM");
	eq("=ABS()", "#NUM");

	// -- references between cells ----------------------------------

	printf("cell references:\n");

	sheet_init(&sh);

	sheet_set(&sh, 0, 1, "10");			// B1
	sheet_set(&sh, 1, 1, "32");			// B2
	sheet_set(&sh, 2, 1, "=B1+B2");		// B3

	sheet_display(&sh, 2, 1, buf, sizeof(buf), 20, NULL);
	ck("B3 = B1+B2", buf, "42");

	// An edit anywhere invalidates every cached value.
	sheet_set(&sh, 0, 1, "20");
	sheet_display(&sh, 2, 1, buf, sizeof(buf), 20, NULL);
	ck("B3 after editing B1", buf, "52");

	// An empty cell is zero, and text is zero.
	sheet_set(&sh, 3, 1, "=B9+1");
	sheet_display(&sh, 3, 1, buf, sizeof(buf), 20, NULL);
	ck("empty cell is zero", buf, "1");

	sheet_set(&sh, 8, 1, "label");
	sheet_display(&sh, 3, 1, buf, sizeof(buf), 20, NULL);
	ck("text cell is zero", buf, "1");

	// Chained references.
	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "2");
	sheet_set(&sh, 1, 0, "=A1*2");
	sheet_set(&sh, 2, 0, "=A2*2");
	sheet_set(&sh, 3, 0, "=A3*2");
	sheet_display(&sh, 3, 0, buf, sizeof(buf), 20, NULL);
	ck("chain of four", buf, "16");

	// -- cycles ----------------------------------------------------

	printf("cycles:\n");

	sheet_init(&sh);

	sheet_set(&sh, 0, 0, "=A1");
	sheet_display(&sh, 0, 0, buf, sizeof(buf), 20, NULL);
	ck("self reference", buf, "#CIRC");

	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "=A2");
	sheet_set(&sh, 1, 0, "=A3");
	sheet_set(&sh, 2, 0, "=A1");
	sheet_display(&sh, 0, 0, buf, sizeof(buf), 20, NULL);
	ck("three-cell cycle", buf, "#CIRC");

	// Breaking the cycle has to actually clear it, which is the case
	// a cached error would get wrong.
	sheet_set(&sh, 2, 0, "7");
	sheet_display(&sh, 0, 0, buf, sizeof(buf), 20, NULL);
	ck("cycle broken", buf, "7");

	// A long chain is not a cycle, but it is deeper than the cap.
	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "1");
	for (int r = 1; r < SHEET_MAX_DEPTH + 6; r++) {
		char f[16];
		snprintf(f, sizeof(f), "=A%d+1", r);
		sheet_set(&sh, r, 0, f);
	}
	sheet_display(&sh, SHEET_MAX_DEPTH + 5, 0, buf, sizeof(buf), 20, NULL);
	ck("over-deep chain", buf, "#DEEP");

	// -- functions -------------------------------------------------

	printf("functions:\n");

	sheet_init(&sh);

	for (int r = 0; r < 13; r++) {
		char v[8];
		snprintf(v, sizeof(v), "%d", r + 1);
		sheet_set(&sh, r, 0, v);			// A1..A13 = 1..13
	}

	sheet_set(&sh, 0, 2, "=SUM(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("SUM(A1:A13)", buf, "91");

	sheet_set(&sh, 0, 2, "=sum(a1:a13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("lowercase sum", buf, "91");

	sheet_set(&sh, 0, 2, "=AVG(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("AVG(A1:A13)", buf, "7");

	sheet_set(&sh, 0, 2, "=MIN(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("MIN", buf, "1");

	sheet_set(&sh, 0, 2, "=MAX(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("MAX", buf, "13");

	sheet_set(&sh, 0, 2, "=COUNT(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("COUNT", buf, "13");

	// Text and blanks inside a range are skipped, not counted as zero
	// -- which is the difference between an average and a wrong one.
	sheet_set(&sh, 4, 0, "note");
	sheet_set(&sh, 0, 2, "=COUNT(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("COUNT skips text", buf, "12");

	sheet_set(&sh, 0, 2, "=SUM(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("SUM skips text", buf, "86");

	// Mixed argument forms.
	sheet_set(&sh, 4, 0, "5");
	sheet_set(&sh, 0, 2, "=SUM(A1:A3,100,A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("SUM of ranges and values", buf, "119");

	sheet_set(&sh, 0, 2, "=SUM(A1:A13)/COUNT(A1:A13)");
	sheet_display(&sh, 0, 2, buf, sizeof(buf), 20, NULL);
	ck("nested calls", buf, "7");

	// A range spanning several columns.
	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "1"); sheet_set(&sh, 0, 1, "2");
	sheet_set(&sh, 1, 0, "3"); sheet_set(&sh, 1, 1, "4");
	sheet_set(&sh, 5, 0, "=SUM(A1:B2)");
	sheet_display(&sh, 5, 0, buf, sizeof(buf), 20, NULL);
	ck("rectangular range", buf, "10");

	// Reversed corners mean the same rectangle.
	sheet_set(&sh, 5, 0, "=SUM(B2:A1)");
	sheet_display(&sh, 5, 0, buf, sizeof(buf), 20, NULL);
	ck("reversed range", buf, "10");

	// An empty range is a sum of nothing, but an average of nothing
	// has no answer.
	sheet_set(&sh, 5, 0, "=SUM(D1:D9)");
	sheet_display(&sh, 5, 0, buf, sizeof(buf), 20, NULL);
	ck("SUM of empty range", buf, "0");

	sheet_set(&sh, 5, 0, "=AVG(D1:D9)");
	sheet_display(&sh, 5, 0, buf, sizeof(buf), 20, NULL);
	ck("AVG of empty range", buf, "#DIV0");

	// A range containing an error propagates it.
	sheet_set(&sh, 3, 0, "=1/0");
	sheet_set(&sh, 5, 0, "=SUM(A1:A4)");
	sheet_display(&sh, 5, 0, buf, sizeof(buf), 20, NULL);
	ck("error inside a range", buf, "#DIV0");

	sheet_init(&sh);
	eq("=ABS(-3.5)", "3.5");
	eq("=ABS(3.5)", "3.5");
	eq("=INT(1.9)", "1");
	eq("=INT(-1.5)", "-2");
	eq("=INT(4)", "4");
	eq("=ROUND(1.2345,2)", "1.23");
	eq("=ROUND(1.235,2)", "1.24");
	eq("=ROUND(-1.235,2)", "-1.24");
	eq("=ROUND(1.5,0)", "2");
	eq("=MIN(D1:D9)", "0");

	// A range that CONTAINS the formula's own cell is a cycle, and
	// this is the easy way to write one by accident -- putting a total
	// at the bottom of the column it totals and catching one row too
	// many. It has to be reported, not quietly evaluated as whatever
	// the cell held a moment ago.
	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "=SUM(A1:A9)");
	sheet_display(&sh, 0, 0, buf, sizeof(buf), 20, NULL);
	ck("range including itself", buf, "#CIRC");

	// The same range one cell down is fine.
	sheet_init(&sh);
	sheet_set(&sh, 9, 0, "=SUM(A1:A9)");
	sheet_display(&sh, 9, 0, buf, sizeof(buf), 20, NULL);
	ck("SUM of an empty range", buf, "0");

	// A range where a range is not allowed.
	eq("=ABS(A1:A9)", "#NUM");

	// -- column fitting --------------------------------------------

	printf("column fitting:\n");

	sheet_init(&sh);

	sheet_set(&sh, 0, 0, "1234.5678");

	sheet_display(&sh, 0, 0, buf, sizeof(buf), 9, NULL);
	ck("fits at 9", buf, "1234.5678");

	sheet_display(&sh, 0, 0, buf, sizeof(buf), 7, NULL);
	ck("rounds at 7", buf, "1234.57");

	sheet_display(&sh, 0, 0, buf, sizeof(buf), 4, NULL);
	ck("integer at 4", buf, "1235");

	sheet_display(&sh, 0, 0, buf, sizeof(buf), 3, NULL);
	ck("hashes at 3", buf, "###");

	{
		bool right = false;
		sheet_display(&sh, 0, 0, buf, sizeof(buf), 9, &right);
		ck_int("numbers right-align", right, 1);

		sheet_set(&sh, 0, 0, "hello there");
		sheet_display(&sh, 0, 0, buf, sizeof(buf), 9, &right);
		ck("text truncates", buf, "hello the");
		ck_int("text left-aligns", right, 0);
	}

	// -- extent ----------------------------------------------------

	printf("extent:\n");

	{
		int rows, cols;

		sheet_init(&sh);
		sheet_extent(&sh, &rows, &cols);
		ck_int("empty rows", rows, 0);
		ck_int("empty cols", cols, 0);

		sheet_set(&sh, 4, 2, "x");
		sheet_extent(&sh, &rows, &cols);
		ck_int("extent rows", rows, 5);
		ck_int("extent cols", cols, 3);

		sheet_set(&sh, 4, 2, NULL);
		sheet_extent(&sh, &rows, &cols);
		ck_int("extent after clear", rows, 0);
	}

	// -- the file format -------------------------------------------

	printf("file format:\n");

	{
		sink_t sink = { .n = 0 };

		sheet_init(&sh);
		sheet_set(&sh, 0, 0, "Item");
		sheet_set(&sh, 0, 1, "Cost");
		sheet_set(&sh, 1, 0, "widget");
		sheet_set(&sh, 1, 1, "12.5");
		sheet_set(&sh, 2, 0, "gizmo");
		sheet_set(&sh, 2, 1, "8.25");
		sheet_set(&sh, 3, 0, "'total");
		sheet_set(&sh, 3, 1, "=SUM(B2:B3)");
		sheet_set_colw(&sh, 0, 12);

		sheet_write(&sh, sink_emit, &sink);

		ck("written file", sink.buf,
			"zsheet 1\n"
			"!w A 12\n"
			"A1 Item\n"
			"B1 Cost\n"
			"A2 widget\n"
			"B2 12.5\n"
			"A3 gizmo\n"
			"B3 8.25\n"
			"A4 'total\n"
			"B4 =SUM(B2:B3)\n");

		// Round trip: load it back and write it again.
		sheet_t sh2;
		load_text(&sh2, sink.buf);

		ck_int("round trip not modified", sh2.modified, 0);
		ck_int("round trip column width", sheet_colw(&sh2, 0), 12);

		sheet_display(&sh2, 3, 1, buf, sizeof(buf), 20, NULL);
		ck("round trip total", buf, "20.75");

		sink_t s2 = { .n = 0 };
		sheet_write(&sh2, sink_emit, &s2);
		ck("round trip is identical", s2.buf, sink.buf);
	}

	// Hand-edited input: comments, blank lines, indentation, CRLF, an
	// unknown directive, and a line that is simply wrong.
	{
		sheet_t s3;

		load_text(&s3,
			"zsheet 1\r\n"
			"# a comment\n"
			"\n"
			"!w B 20\r\n"
			"!futurething 1\n"
			"  A1 hello\r\n"
			"B1 =2*21\n"
			"garbage line\n"
			"C1 7\n");

		ck("hand-edited A1", sheet_src(&s3, 0, 0), "hello");
		ck_int("hand-edited width", sheet_colw(&s3, 1), 20);

		sheet_display(&s3, 0, 1, buf, sizeof(buf), 20, NULL);
		ck("hand-edited B1", buf, "42");

		// The bad line must not have eaten the good one after it.
		sheet_display(&s3, 0, 2, buf, sizeof(buf), 20, NULL);
		ck("line after a bad one", buf, "7");
	}

	// -- CSV -------------------------------------------------------

	printf("csv:\n");

	{
		sink_t sink = { .n = 0 };

		sheet_init(&sh);
		sheet_set(&sh, 0, 0, "name");
		sheet_set(&sh, 0, 1, "qty");
		sheet_set(&sh, 1, 0, "widget, large");
		sheet_set(&sh, 1, 1, "3");
		sheet_set(&sh, 2, 0, "say \"hi\"");
		sheet_set(&sh, 2, 1, "=B2*2");

		sheet_write_csv(&sh, sink_emit, &sink);

		ck("csv out", sink.buf,
			"name,qty\n"
			"\"widget, large\",3\n"
			"\"say \"\"hi\"\"\",6\n");

		sheet_t s4;
		sheet_load_csv_begin(&s4);

		char line[256];
		int n = 0;
		for (const char *p = sink.buf;; p++) {
			if (*p == '\n' || !*p) {
				line[n] = 0;
				if (n || *p == '\n') sheet_load_csv_line(&s4, line);
				n = 0;
				if (!*p) break;
				continue;
			}
			line[n++] = *p;
		}

		sheet_load_csv_end(&s4);

		ck("csv in, quoted comma", sheet_src(&s4, 1, 0), "widget, large");
		ck("csv in, quoted quotes", sheet_src(&s4, 2, 0), "say \"hi\"");
		ck("csv in, header", sheet_src(&s4, 0, 1), "qty");

		sheet_display(&s4, 2, 1, buf, sizeof(buf), 20, NULL);
		ck("csv in, value not formula", buf, "6");
		ck_int("csv value is a number", sheet_kind(&s4, 2, 1), SHEET_KIND_NUM);
	}

	// -- storage limits --------------------------------------------

	printf("storage:\n");

	{
		// Rewriting one cell many times orphans arena text, which
		// compaction has to reclaim. Without it this fails at around
		// the 400th edit.
		sheet_init(&sh);

		for (int i = 0; i < 4000; i++) {
			char v[32];
			snprintf(v, sizeof(v), "value number %d", i);
			if (sheet_set(&sh, 3, 3, v) != SHEET_OK) {
				printf("  FAIL  arena exhausted at edit %d\n", i);
				fails++;
				break;
			}
			checks++;
			checks--;			// counted once below, not 4000 times
		}

		checks++;
		ck("survives 4000 edits", sheet_src(&sh, 3, 3), "value number 3999");

		// And the cells around it are undisturbed by the compaction.
		sheet_init(&sh);
		sheet_set(&sh, 0, 0, "first");
		sheet_set(&sh, 9, 9, "last");
		for (int i = 0; i < 3000; i++) {
			char v[32];
			snprintf(v, sizeof(v), "churn %d", i);
			sheet_set(&sh, 5, 5, v);
		}
		ck("compaction keeps A1", sheet_src(&sh, 0, 0), "first");
		ck("compaction keeps J10", sheet_src(&sh, 9, 9), "last");

		// Filling the cell array reports rather than corrupting.
		sheet_init(&sh);
		int placed = 0;
		sheet_err_t e = SHEET_OK;
		for (int r = 0; r < SHEET_ROWS && e == SHEET_OK; r++)
			for (int c = 0; c < SHEET_COLS && e == SHEET_OK; c++) {
				e = sheet_set(&sh, r, c, "1");
				if (e == SHEET_OK) placed++;
			}

		ck_int("cell cap reported", e, SHEET_ERR_FULL);
		ck_int("cells placed", placed, SHEET_MAX_CELLS);

		// An existing cell can still be edited when the array is full,
		// which is what makes a full sheet recoverable rather than
		// stuck.
		ck_int("edit when full", sheet_set(&sh, 0, 0, "2"), SHEET_OK);
		ck_int("clear when full", sheet_set(&sh, 0, 0, NULL), SHEET_OK);
	}

	// -- out of range ----------------------------------------------

	ck_int("set outside grid", sheet_set(&sh, -1, 0, "x"), SHEET_ERR_REF);
	ck_int("set past last col",
		sheet_set(&sh, 0, SHEET_COLS, "x"), SHEET_ERR_REF);

	printf("\n%d checks, %s\n", checks,
		fails ? "FAILURES" : "all passed");

	return fails ? 1 : 0;

}
