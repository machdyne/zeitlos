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
	eq("=1/3", "0.33333333");
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

	// -- 0.0.7: operators and functions ------------------------------

	sheet_init(&sh);
	sheet_set(&sh, 1, 1, "10");		// B2
	sheet_set(&sh, 2, 1, "0");		// B3
	eq("=2^10", "1024");
	eq("=2^-2", "0.25");
	eq("=-2^2", "4");
	eq("=2*3^2", "18");
	eq("=1<2", "1");
	eq("=2<=1", "0");
	eq("=B2=10", "1");
	eq("=B2<>10", "0");
	eq("=1+1>1", "1");
	eq("=IF(B2>5,1,2)", "1");
	eq("=IF(B2>50,1,2)", "2");
	eq("=IF(B3=0,0,B2/B3)", "0");		// the branch not taken is not an error
	eq("=IF(B3<>0,B2/B3,-1)", "-1");
	eq("=IF(1,2)", "2");
	eq("=IF(0,2)", "0");
	eq("=B2/B3", "#DIV0");
	eq("=AND(1,B2)", "1");
	eq("=AND(1,0)", "0");
	eq("=OR(0,B3,1)", "1");
	eq("=NOT(B3)", "1");
	eq("=TRUE+TRUE", "2");
	eq("=MOD(7,3)", "1");
	eq("=MOD(-1,3)", "2");
	eq("=MOD(1,0)", "#DIV0");
	eq("=SQRT(2)", "1.41421356");
	eq("=SQRT(144)", "12");
	eq("=SQRT(-1)", "#NUM");
	eq("=POWER(1.5,2)", "2.25");
	// eight decimals: 0.05/12 is right to the cent over 30 years
	eq("=ROUND(PMT(0.05/12,360,200000),2)", "-1073.64");
	eq("=ROUND(PMT(0.0041666,360,200000),2)", "-1073.63");
	eq("=100000*100000", "10000000000");
	eq("=1000000*1000000", "#NUM");
	eq("=0.1+0.2=0.3", "1");
	eq("=ROUND(PMT(0,10,1000),2)", "-100");
	eq("=ROUND(FV(0.06/12,120,-100),2)", "16387.93");
	eq("=ROUND(PV(0.0067,240,-500),2)", "59599.64");
	sheet_set(&sh, 4, 0, "-1000");	// A5..A8
	sheet_set(&sh, 5, 0, "300");
	sheet_set(&sh, 6, 0, "400");
	sheet_set(&sh, 7, 0, "500");
	eq("=ROUND(NPV(0.1,A6:A8),2)", "978.96");
	eq("=ROUND(NPV(0.1,300,400,500)+A5,2)", "-21.04");
	eq("=0^-1", "#DIV0");
	eq("=1.5^2", "2.25");
	// a table: D1:E4
	sheet_set(&sh, 0, 3, "1"); sheet_set(&sh, 0, 4, "100");
	sheet_set(&sh, 1, 3, "5"); sheet_set(&sh, 1, 4, "500");
	sheet_set(&sh, 2, 3, "10"); sheet_set(&sh, 2, 4, "1000");
	eq("=VLOOKUP(5,D1:E3,2)", "500");
	eq("=VLOOKUP(7,D1:E3,2)", "500");
	eq("=VLOOKUP(7,D1:E3,2,0)", "#NUM");
	eq("=VLOOKUP(99,D1:E3,2)", "1000");
	eq("=VLOOKUP(0,D1:E3,2)", "#NUM");
	eq("=VLOOKUP(5,D1:E3,3)", "#REF");
	eq("=INDEX(D1:E3,3,2)", "1000");
	eq("=INDEX(D1:E3,4,1)", "#REF");
	eq("=#REF!+1", "#REF");
	eq("=FOO(1)", "#NAME");

	// -- formats ----------------------------------------------------

	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "1234567.891");
	sheet_set(&sh, 0, 1, "-0.125");
	sheet_set_format(&sh, 0, 0, 0, 0, SHEET_FMT_COMMA, 2);
	{
		char out[64];
		sheet_display(&sh, 0, 0, out, sizeof out, 40, NULL);
		ck("comma 2", out, "1,234,567.89");
		sheet_set_format(&sh, 0, 0, 0, 0, SHEET_FMT_MONEY, 0);
		sheet_display(&sh, 0, 0, out, sizeof out, 40, NULL);
		ck("money 0", out, "$1,234,568");
		sheet_display(&sh, 0, 0, out, sizeof out, 8, NULL);
		ck("too narrow", out, "########");
		sheet_set_format(&sh, 0, 1, 0, 1, SHEET_FMT_PERCENT, 1);
		sheet_display(&sh, 0, 1, out, sizeof out, 40, NULL);
		ck("percent", out, "-12.5%");
		sheet_set_format(&sh, 0, 1, 0, 1, SHEET_FMT_FIXED, 0);
		sheet_display(&sh, 0, 1, out, sizeof out, 40, NULL);
		ck("fixed 0 no -0", out, "0");
		sheet_set_format(&sh, 0, 0, 9, 1, SHEET_FMT_GENERAL, 0);
		ck_int("general clears", sh.nfmts, 0);
		int k, d;
		ck_int("parse ,2", sheet_parse_format(",2", &k, &d) && k == SHEET_FMT_COMMA && d == 2, 1);
		ck_int("parse $", sheet_parse_format("$", &k, &d) && k == SHEET_FMT_MONEY && d == 2, 1);
		ck_int("parse junk", sheet_parse_format("Q", &k, &d), 0);
		// round trip through the file
		sheet_set_format(&sh, 0, 0, 4, 1, SHEET_FMT_COMMA, 1);
		sink_t k1;
		k1.n = 0;
		sheet_write(&sh, sink_emit, &k1);
		ck_int("format written", strstr(k1.buf, "!f A1:B5 ,1\n") != NULL, 1);
		sheet_load_begin(&sh);
		char *line = strtok(k1.buf, "\n");
		while (line) { sheet_load_line(&sh, line); line = strtok(NULL, "\n"); }
		sheet_load_end(&sh);
		sheet_display(&sh, 0, 0, out, sizeof out, 40, NULL);
		ck("format read back", out, "1,234,567.9");
	}

	// -- moving formulas --------------------------------------------

	{
		char o[SHEET_SRC_MAX];
		sheet_rewrite("=A1+$B$2+C$3+$D4", o, sizeof o, SHEET_MOVE_COPY, 1, 1);
		ck("copy rel/abs", o, "=B2+$B$2+D$3+$D5");
		sheet_rewrite("=SUM(A1:A3)*LOG10", o, sizeof o, SHEET_MOVE_COPY, 2, 0);
		ck("copy range", o, "=SUM(A3:A5)*LOG10");
		sheet_rewrite("=A1+1", o, sizeof o, SHEET_MOVE_COPY, -1, 0);
		ck("copy off grid", o, "=#REF!+1");
		sheet_rewrite("=A1+A5+SUM(A2:A9)", o, sizeof o, SHEET_MOVE_INS_ROWS, 2, 3);
		ck("insert rows", o, "=A1+A8+SUM(A2:A12)");
		sheet_rewrite("=A1+A5+SUM(A2:A9)+A3", o, sizeof o, SHEET_MOVE_DEL_ROWS, 2, 3);
		ck("delete rows", o, "=A1+#REF!+SUM(A2:A6)+#REF!");
		sheet_rewrite("=SUM(A3:A5)", o, sizeof o, SHEET_MOVE_DEL_ROWS, 2, 3);
		ck("delete whole range", o, "=SUM(#REF!)");
		sheet_rewrite("=SUM(A1:D1)+$C$1", o, sizeof o, SHEET_MOVE_DEL_COLS, 1, 1);
		ck("delete col", o, "=SUM(A1:C1)+$B$1");
		sheet_rewrite("=sum(b2:c3)", o, sizeof o, SHEET_MOVE_INS_COLS, 0, 2);
		ck("insert cols, case", o, "=sum(D2:E3)");
		sheet_rewrite("=1.5E3+2", o, sizeof o, SHEET_MOVE_COPY, 5, 5);
		ck("numbers alone", o, "=1.5E3+2");
	}

	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "1");			// A1
	sheet_set(&sh, 1, 0, "2");			// A2
	sheet_set(&sh, 2, 0, "3");			// A3
	sheet_set(&sh, 3, 0, "=SUM(A1:A3)");	// A4
	sheet_set_format(&sh, 3, 0, 3, 0, SHEET_FMT_FIXED, 2);
	ck_int("insert row", sheet_insert_rows(&sh, 1, 1), SHEET_OK);
	ck("moved formula", sheet_src(&sh, 4, 0), "=SUM(A1:A4)");
	ck("A2 empty", sheet_src(&sh, 1, 0), "");
	sheet_set(&sh, 1, 0, "10");
	{
		char out[32];
		sheet_display(&sh, 4, 0, out, sizeof out, 20, NULL);
		ck("sum after insert, format moved", out, "16.00");
	}
	ck_int("delete rows", sheet_delete_rows(&sh, 0, 2), SHEET_OK);
	ck("after delete", sheet_src(&sh, 2, 0), "=SUM(A1:A2)");
	ck_int("delete col A", sheet_delete_cols(&sh, 0, 1), SHEET_OK);
	ck_int("now empty", sh.ncells, 0);
	sheet_set(&sh, SHEET_ROWS - 1, 0, "last");
	ck_int("insert would push off", sheet_insert_rows(&sh, 5, 1), SHEET_ERR_FULL);
	ck("still there", sheet_src(&sh, SHEET_ROWS - 1, 0), "last");

	// -- sort ---------------------------------------------------------

	{
		static char tmp[65536];
		sheet_init(&sh);
		const char *names[] = { "pear", "Apple", "fig", "", "banana" };
		const char *qty[] = { "3", "10", "x", "5", "1" };
		for (int r = 0; r < 5; r++) {
			if (*names[r]) sheet_set(&sh, r, 0, names[r]);
			sheet_set(&sh, r, 1, qty[r]);
			sheet_set(&sh, r, 2, "=B1*2");		// relative: moves with its row
			char f[16];
			snprintf(f, sizeof f, "=B%d*2", r + 1);
			sheet_set(&sh, r, 2, f);
		}
		ck_int("sort", sheet_sort(&sh, 0, 0, 4, 2, 1, false, tmp, sizeof tmp), 1);
		ck("sort 1", sheet_src(&sh, 0, 0), "banana");
		ck("sort 2", sheet_src(&sh, 1, 0), "pear");
		ck("sort 3", sheet_src(&sh, 2, 0), "");
		ck("sort 4", sheet_src(&sh, 3, 0), "Apple");
		ck("sort text last", sheet_src(&sh, 4, 0), "fig");
		ck("formula followed", sheet_src(&sh, 3, 2), "=B4*2");
		char out[32];
		sheet_display(&sh, 3, 2, out, sizeof out, 20, NULL);
		ck("formula value", out, "20");
		ck_int("sort desc by name", sheet_sort(&sh, 0, 0, 4, 2, 0, true, tmp, sizeof tmp), 1);
		ck("desc 1", sheet_src(&sh, 0, 0), "pear");
		ck("desc empty last", sheet_src(&sh, 4, 0), "");
		ck_int("scratch too small", sheet_sort(&sh, 0, 0, 4, 2, 0, false, tmp, 16), 0);
		ck("unchanged", sheet_src(&sh, 0, 0), "pear");
	}

	// -- ranges, snapshots --------------------------------------------

	{
		static uint8_t buf[200000];
		sheet_init(&sh);
		sheet_set(&sh, 0, 0, "1");
		sheet_set(&sh, 1, 0, "=A1+1");
		sheet_set(&sh, 1, 1, "=$A$1*B1");
		int n = sheet_range_save(&sh, 0, 0, 1, 1, buf, sizeof buf);
		ck_int("range size", n, sheet_range_size(&sh, 0, 0, 1, 1));
		sheet_range_load(&sh, 5, 2, 2, 2, true, buf, n, true, 0, 0);
		ck("pasted rel", sheet_src(&sh, 6, 2), "=C6+1");
		ck("pasted abs", sheet_src(&sh, 6, 3), "=$A$1*D6");
		sheet_set_format(&sh, 0, 0, 3, 3, SHEET_FMT_MONEY, 2);
		sheet_set_colw(&sh, 2, 15);
		int sn = sheet_snapshot(&sh, buf, sizeof buf);
		ck_int("snapshot size", sn, sheet_snapshot_size(&sh));
		sheet_init(&sh);
		ck_int("restore", sheet_restore(&sh, buf, sn), 1);
		ck("restored cell", sheet_src(&sh, 6, 2), "=C6+1");
		ck_int("restored width", sheet_colw(&sh, 2), 15);
		ck_int("restored format", sheet_format_at(&sh, 1, 1, NULL), SHEET_FMT_MONEY);
		char out[32];
		sheet_display(&sh, 1, 0, out, sizeof out, 20, NULL);
		ck("restored value", out, "$2.00");
		ck_int("snapshot too small", sheet_snapshot(&sh, buf, 20), -1);
		// the chart: saved, read back, moved by an insert
		sheet_init(&sh);
		sheet_set(&sh, 1, 1, "1");
		sh.chart.set = true; sh.chart.kind = SHEET_CHART_PIE;
		sh.chart.r0 = 1; sh.chart.r1 = 5; sh.chart.c0 = 0; sh.chart.c1 = 2;
		sink_t k2;
		k2.n = 0;
		sheet_write(&sh, sink_emit, &k2);
		ck_int("chart written", strstr(k2.buf, "!g pie A2:C6\n") != NULL, 1);
		sheet_load_begin(&sh);
		char *ln = strtok(k2.buf, "\n");
		while (ln) { sheet_load_line(&sh, ln); ln = strtok(NULL, "\n"); }
		ck_int("chart read", sh.chart.set && sh.chart.kind == SHEET_CHART_PIE && sh.chart.r1 == 5 && sh.chart.c1 == 2, 1);
		sheet_insert_rows(&sh, 0, 2);
		ck_int("chart moved", sh.chart.r0 == 3 && sh.chart.r1 == 7, 1);
		sn = sheet_snapshot(&sh, buf, sizeof buf);
		sheet_init(&sh);
		sheet_restore(&sh, buf, sn);
		ck_int("chart in snapshot", sh.chart.set && sh.chart.r0 == 3 && sh.chart.kind == SHEET_CHART_PIE, 1);
	}

	// -- loading is not quadratic: a full sheet ------------------------

	{
		sheet_init(&sh);
		for (int i = 0; i < SHEET_MAX_CELLS; i++) {
			char v[16];
			snprintf(v, sizeof v, "=%d+1", i);
			sheet_set(&sh, i / SHEET_COLS, i % SHEET_COLS, v);
		}
		char out[32];
		sheet_display(&sh, 0, 5, out, sizeof out, 20, NULL);
		ck("full sheet evaluates", out, "6");
		// 64 changes wrap the generation: cached values still right
		for (int i = 0; i < 70; i++) sheet_set(&sh, 0, 0, i & 1 ? "=2" : "=3");
		sheet_display(&sh, 0, 5, out, sizeof out, 20, NULL);
		ck("after generation wrap", out, "6");
		sheet_display(&sh, 0, 0, out, sizeof out, 20, NULL);
		ck("last edit", out, "2");
	}

	printf("\n%d checks, %s\n", checks,
		fails ? "FAILURES" : "all passed");

	return fails ? 1 : 0;

}
