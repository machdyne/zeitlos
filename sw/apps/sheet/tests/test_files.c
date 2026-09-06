/*
 * File I/O tests for sw/apps/sheet.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_files \
 *      sw/apps/sheet/tests/test_files.c sw/apps/sheet/sheet_core.c \
 *      sw/common/ztype.c sw/common/zwin.c sw/common/zwidget.c \
 *      sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
 *      sw/common/zfix.c
 *   /tmp/test_files
 *
 * tests/test_core.c already covers the FORMAT -- what a line means,
 * what round-trips, what a bad line does. This covers the layer above
 * it, which is `sheet.c`'s own and which test_core.c cannot reach:
 * load_path() reading a file in 256-byte chunks and splitting it into
 * lines, and do_save_to() streaming one back out through
 * fs_write_chunk().
 *
 * That splitter is worth testing on its own because its failure mode
 * is invisible in the common case. A line that happens to straddle a
 * chunk boundary is the only thing that exercises the carry-over
 * between reads, and whether it does depends entirely on how long the
 * lines before it were -- so a sheet can round-trip perfectly for
 * months and then lose a cell when somebody renames a label.
 *
 * The stub filesystem below is deliberately a real one (bytes in,
 * bytes out, handles, positions) rather than a set of no-ops: the
 * point is to run the actual read/write loops, not to skip them.
 */

#include "../../../common/tests/zrender.h"
#include "../../../common/tests/ztramp.h"

#define main sheet_main_unused
#include "../sheet.c"
#undef main

// -- a small in-memory filesystem --------------------------------

// Generous: every test below leaves its file behind, and a stub that
// runs out of slots fails as "the save did not work", which is a
// confusing way to be told the TEST is full rather than the app.
#define STUB_FILES   12
#define STUB_CAP     65536

static char stub_name[STUB_FILES][64];
static char stub_data[STUB_FILES][STUB_CAP];
static int  stub_len[STUB_FILES];
static int  stub_pos[STUB_FILES];
static bool stub_open[STUB_FILES];

static int stub_find(const char *name) {

	for (int i = 0; i < STUB_FILES; i++)
		if (stub_name[i][0] && !strcmp(stub_name[i], name)) return i;

	return -1;

}

static int stub_make(const char *name) {

	int i = stub_find(name);
	if (i >= 0) return i;

	for (i = 0; i < STUB_FILES; i++)
		if (!stub_name[i][0]) {
			snprintf(stub_name[i], sizeof(stub_name[i]), "%s", name);
			stub_len[i] = 0;
			return i;
		}

	return -1;

}

// Puts a file on the stub filesystem directly, without going through
// the app -- the input side of a load test.
static void stub_put(const char *name, const char *text) {

	int i = stub_make(name);
	int n = (int)strlen(text);

	memcpy(stub_data[i], text, (size_t)n);
	stub_len[i] = n;

}

int fs_open_read(const char *p) {

	int i = stub_find(p);
	if (i < 0) return -1;

	stub_pos[i] = 0;
	stub_open[i] = true;

	return i;

}

int fs_open_write(const char *p) {

	int i = stub_make(p);
	if (i < 0) return -1;

	stub_len[i] = 0;			// FA_CREATE_ALWAYS truncates
	stub_pos[i] = 0;
	stub_open[i] = true;

	return i;

}

int fs_read_chunk(int h, void *buf, int maxlen) {

	if (h < 0 || h >= STUB_FILES || !stub_open[h]) return -1;

	int left = stub_len[h] - stub_pos[h];
	int n = maxlen < left ? maxlen : left;

	if (n <= 0) return 0;

	memcpy(buf, &stub_data[h][stub_pos[h]], (size_t)n);
	stub_pos[h] += n;

	return n;

}

int fs_write_chunk(int h, const void *buf, int n) {

	if (h < 0 || h >= STUB_FILES || !stub_open[h]) return -1;
	if (stub_len[h] + n > STUB_CAP) return -1;

	memcpy(&stub_data[h][stub_len[h]], buf, (size_t)n);
	stub_len[h] += n;

	return n;

}

int fs_close_handle(int h) {

	if (h < 0 || h >= STUB_FILES) return 0;

	stub_open[h] = false;
	stub_data[h][stub_len[h]] = 0;

	return 1;

}

int fs_sync(int h) { (void)h; return 1; }

// ztype.c is linked for real, so is_csv() is exercised rather than
// stubbed -- but z_ftype_is_executable() would read a file through the
// stub above, which nothing here wants. It is never called on this
// path; this is here only because ztype.c references them.
int fs_size(char *name) { (void)name; return 0; }

// -- dialogs ------------------------------------------------------
//
// Counted rather than ignored: a save that silently fails and a save
// that reports a failure are very different, and the difference is
// exactly a dialog.

static int dialogs;

bool z_dialog_open(const z_dialog_ctx_t *c, const char *d, char *o, int n) {
	(void)c; (void)d; (void)o; (void)n; return false; }
bool z_dialog_save(const z_dialog_ctx_t *c, const char *d, const char *s,
	char *o, int n) { (void)c; (void)d; (void)s; (void)o; (void)n; return false; }
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m,
	int b) { (void)c; (void)t; (void)m; (void)b; dialogs++; return 0; }
bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m,
	const char *i, char *o, int n) {
	(void)c; (void)t; (void)m; (void)i; (void)o; (void)n; return false; }

// -- harness ------------------------------------------------------

static int checks, fails;

static void ck(const char *what, const char *got, const char *want) {

	checks++;

	if (strcmp(got, want)) {
		printf("  FAIL  %-30s got \"%s\"  want \"%s\"\n", what, got, want);
		fails++;
	}

}

static void ok(const char *what, bool cond) {

	checks++;

	if (!cond) {
		printf("  FAIL  %s\n", what);
		fails++;
	}

}

static const char *shown(int r, int c) {

	static char buf[64];

	sheet_display(&sh, r, c, buf, sizeof(buf), 40, NULL);

	return buf;

}

int main(void) {

	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("test_files: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	// load_path()/do_save_to() call update_title(), which sends a
	// message -- see ztramp.h.
	if (!z_tramp_install()) {
		printf("test_files: skipped\n");
		return 77;
	}

	z_scrollbar_init(&vsb, &win, Z_SB_VERT);
	z_scrollbar_init(&hsb, &win, Z_SB_HORZ);

	layout();

	// -- a plain load ---------------------------------------------

	printf("loading:\n");

	stub_put("/A.ZSS",
		"zsheet 1\n"
		"!w B 14\n"
		"A1 Item\n"
		"B1 12.5\n"
		"B2 =B1*2\n");

	ok("load reports success", load_path("/A.ZSS"));
	ck("loaded label", sheet_src(&sh, 0, 0), "Item");
	ck("loaded formula", shown(1, 1), "25");
	ok("loaded column width", sheet_colw(&sh, 1) == 14);
	ok("loaded file is not modified", !sh.modified);
	ck("filename remembered", filename, "/A.ZSS");
	ok("cursor reset to A1", cur_row == 0 && cur_col == 0);

	// A missing file reports rather than half-loading.
	dialogs = 0;
	ok("missing file fails", !load_path("/NOPE.ZSS"));
	ok("missing file reports", dialogs == 1);

	// -- no trailing newline --------------------------------------
	//
	// The last line has to be flushed after the read loop ends, and
	// forgetting that loses exactly one cell -- the last one, which is
	// very often the total.

	stub_put("/B.ZSS", "A1 first\nB9 =1+1");

	load_path("/B.ZSS");
	ck("last line without a newline", shown(8, 1), "2");

	// -- CRLF ------------------------------------------------------

	stub_put("/C.ZSS", "zsheet 1\r\nA1 hello\r\nA2 42\r\n");

	load_path("/C.ZSS");
	ck("CRLF label", sheet_src(&sh, 0, 0), "hello");
	ck("CRLF number", shown(1, 0), "42");

	// -- the chunk boundary ----------------------------------------
	//
	// The read loop takes 256 bytes at a time. Every line length in a
	// window around that boundary is tried, so a line ending exactly
	// at, either side of, and across the seam is covered rather than
	// hoped for.

	printf("chunk boundaries:\n");

	for (int pad = 0; pad < 40; pad++) {

		char text[4096];
		int n = 0;

		// One long label, sized so the NEXT line's start walks across
		// the 256-byte boundary as `pad` varies.
		n += snprintf(text + n, sizeof(text) - n, "A1 ");
		for (int i = 0; i < 200 + pad; i++) text[n++] = 'x';
		text[n++] = '\n';

		n += snprintf(text + n, sizeof(text) - n,
			"A2 marker\nA3 =1+2\n");
		text[n] = 0;

		stub_put("/D.ZSS", text);
		load_path("/D.ZSS");

		checks++;

		if (strcmp(sheet_src(&sh, 1, 0), "marker") ||
			strcmp(shown(2, 0), "3")) {
			printf("  FAIL  pad %d: A2=\"%s\" A3=\"%s\"\n", pad,
				sheet_src(&sh, 1, 0), shown(2, 0));
			fails++;
		}

		// The long label is longer than a cell can hold, so it is
		// truncated at SHEET_SRC_MAX -- but it must still be a label,
		// not a fragment that ran into the next line.
		checks++;

		if ((int)strlen(sheet_src(&sh, 0, 0)) != SHEET_SRC_MAX - 1) {
			printf("  FAIL  pad %d: over-long label is %d bytes\n", pad,
				(int)strlen(sheet_src(&sh, 0, 0)));
			fails++;
		}

	}

	// -- saving ----------------------------------------------------

	printf("saving:\n");

	sheet_init(&sh);
	filename[0] = 0;

	sheet_set(&sh, 0, 0, "Item");
	sheet_set(&sh, 0, 1, "12.5");
	sheet_set(&sh, 1, 1, "=B1*2");
	sheet_set_colw(&sh, 0, 12);

	dialogs = 0;
	ok("save reports success", do_save_to("/OUT.ZSS"));
	ok("save raised no dialog", dialogs == 0);
	ok("saved file is not modified", !sh.modified);
	ck("save sets the filename", filename, "/OUT.ZSS");

	{
		int i = stub_find("/OUT.ZSS");

		ok("the file exists after saving", i >= 0);

		ck("written bytes", i >= 0 ? stub_data[i] : "<no file>",
			"zsheet 1\n"
			"!w A 12\n"
			"A1 Item\n"
			"B1 12.5\n"
			"B2 =B1*2\n");
	}

	// And straight back in.
	load_path("/OUT.ZSS");
	ck("save then load, label", sheet_src(&sh, 0, 0), "Item");
	ck("save then load, formula", shown(1, 1), "25");

	// -- CSV, chosen by extension ----------------------------------
	//
	// is_csv() runs against the REAL z_ftype_ext() here, so this also
	// covers the case that decides which writer runs at all.

	printf("csv:\n");

	sheet_init(&sh);
	sheet_set(&sh, 0, 0, "name");
	sheet_set(&sh, 0, 1, "n");
	sheet_set(&sh, 1, 0, "widget");
	sheet_set(&sh, 1, 1, "=2*3");

	ok("csv save", do_save_to("/OUT.CSV"));

	{
		int i = stub_find("/OUT.CSV");
		ck("csv written as values", i >= 0 ? stub_data[i] : "<no file>",
			"name,n\nwidget,6\n");
	}

	// Lower case, because FAT gives short names in either and the
	// extension test is the only thing choosing the format.
	ok("csv detected lower case", is_csv("/x.csv"));
	ok("csv detected upper case", is_csv("/X.CSV"));
	ok("zss is not csv", !is_csv("/X.ZSS"));
	ok("no extension is not csv", !is_csv("/README"));

	load_path("/OUT.CSV");
	ck("csv load, text field", sheet_src(&sh, 1, 0), "widget");
	ck("csv load, value field", shown(1, 1), "6");
	ok("csv value arrives as a number",
		sheet_kind(&sh, 1, 1) == SHEET_KIND_NUM);

	// -- a big sheet, many chunks ----------------------------------
	//
	// Enough rows that the reader takes dozens of chunks, written by
	// the app and read back by the app, compared cell for cell.

	printf("round trip:\n");

	sheet_init(&sh);

	for (int r = 0; r < 120; r++) {
		char v[32];
		snprintf(v, sizeof(v), "row %d label", r);
		sheet_set(&sh, r, 0, v);
		snprintf(v, sizeof(v), "%d.%02d", r * 7, r % 100);
		sheet_set(&sh, r, 1, v);
		snprintf(v, sizeof(v), "=B%d*2", r + 1);
		sheet_set(&sh, r, 2, v);
	}

	sheet_set(&sh, 121, 2, "=SUM(C1:C120)");

	char before[64];
	snprintf(before, sizeof(before), "%s", shown(121, 2));

	do_save_to("/BIG.ZSS");
	load_path("/BIG.ZSS");

	int bad = 0;

	for (int r = 0; r < 120; r++) {

		char want[32];

		snprintf(want, sizeof(want), "row %d label", r);
		if (strcmp(sheet_src(&sh, r, 0), want)) bad++;

		snprintf(want, sizeof(want), "=B%d*2", r + 1);
		if (strcmp(sheet_src(&sh, r, 2), want)) bad++;

	}

	checks++;
	if (bad) { printf("  FAIL  %d cells differ after a round trip\n", bad); fails++; }

	ck("total survives the round trip", shown(121, 2), before);

	printf("\n%d checks, %s\n", checks, fails ? "FAILURES" : "all passed");

	return fails ? 1 : 0;

}
