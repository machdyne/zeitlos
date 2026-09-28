/*
 * Tests for zetta (sw/common/zetta.c). docs/zetta.md.
 *
 *   make -f tests/Makefile.zetta     (from sw/common)
 *
 * zetta's output goes into the real terminal emulator (zvt100.c), and the
 * checks are on what a terminal SHOWS: its cells and its cursor. At the
 * heart, a reference wrapper written apart from zetta's -- on arrays of
 * characters, not byte offsets -- says what the text area must show;
 * after thousands of random keys the screen must match it row for row,
 * the cursor must be where it says, and the saved text must keep every
 * line within the width.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../zetta.h"
#include "../zvt100.h"
#include "../zutf8.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static vt_screen_t vt;
static uint32_t written, now_ms = 1000;

static void wr(void *ctx, const char *b, uint32_t n) {
	(void)ctx;
	vt_feed(&vt, (const uint8_t *)b, n);
	written += n;
}

// A screen row as text (Latin-9 bytes; trailing blanks cut).
static const char *row(int r) {
	static char b[VT_COLS + 1];
	for (int c = 0; c < VT_COLS; c++) b[c] = vt.cells[r][c].ch ? vt.cells[r][c].ch : ' ';
	int n = VT_COLS;
	while (n > 0 && b[n - 1] == ' ') n--;
	b[n] = 0;
	return b;
}

static bool on_screen(const char *s) {
	for (int r = 0; r < VT_ROWS; r++) if (strstr(row(r), s)) return true;
	return false;
}

static int type(zetta_t *ed, const char *s) {
	now_ms += 20;
	return zetta_key(ed, (const uint8_t *)s, (uint32_t)strlen(s), now_ms);
}

static int esc_alone(zetta_t *ed) {
	type(ed, "\x1b");
	now_ms += 300;
	return zetta_tick(ed, now_ms);
}

// -- the reference wrapper: characters, not bytes --

#define REF_MAX 20000
static uint32_t cp[REF_MAX];			// the text as code points
static uint32_t cpoff[REF_MAX + 1];		// each one's byte offset
static int ncp;

static void to_cps(const char *b, uint32_t len) {
	ncp = 0;
	for (uint32_t i = 0; i < len; ) {
		uint8_t c = (uint8_t)b[i];
		int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
		uint32_t v = n == 1 ? c : c & (0xFF >> (n + 1));
		for (int k = 1; k < n; k++) v = (v << 6) | ((uint8_t)b[i + k] & 0x3F);
		cpoff[ncp] = i;
		cp[ncp++] = v;
		i += (uint32_t)n;
	}
	cpoff[ncp] = len;
}

// Rows of the whole text: each [start, shown end) in code points.
static int rs[REF_MAX + 2], re[REF_MAX + 2], nrows;
static void ref_rows(int w) {
	nrows = 0;
	int p = 0;
	for (;;) {
		int pe = p;
		while (pe < ncp && cp[pe] != '\n') pe++;
		int pos = p;
		for (;;) {
			if (pe - pos <= w) { rs[nrows] = pos; re[nrows++] = pe; break; }
			if (cp[pos + w] == ' ') { rs[nrows] = pos; re[nrows++] = pos + w; pos = pos + w + 1; continue; }
			int k = pos + w - 1;
			while (k >= pos && cp[k] != ' ') k--;
			if (k >= pos) { rs[nrows] = pos; re[nrows++] = k + 1; pos = k + 1; }
			else { rs[nrows] = pos; re[nrows++] = pos + w; pos = pos + w; }
		}
		if (pe >= ncp) break;
		p = pe + 1;
		if (p == ncp) { rs[nrows] = p; re[nrows++] = p; break; }	// after a final Enter: an empty row
	}
}

// Code point `v` as the terminal shows it: its own conversion to Latin-9.
static char shown_as(uint32_t v) {
	uint8_t c = z_cp_to_l9(v);
	return c ? (char)c : 0x7f;
}

// The text area and the cursor, against the reference.
static bool screen_agrees(const zetta_t *ed, char *why, int cap) {
	int w = ed->cfg.cols - 1, top_row = 1 + ed->cfg.nfields, h = ed->cfg.rows - 3 - top_row;
	to_cps(ed->buf, ed->len);
	ref_rows(w);
	int first = -1;
	for (int i = 0; i < nrows; i++) if ((int)cpoff[rs[i]] == (int)ed->top) { first = i; break; }
	if (first < 0) { snprintf(why, (size_t)cap, "top %u is not a row's start", (unsigned)ed->top); return false; }
	for (int r = 0; r < h; r++) {
		char want[VT_COLS + 1];
		int n = 0, i = first + r;
		if (i < nrows) for (int k = rs[i]; k < re[i] && n < w; k++) want[n++] = shown_as(cp[k]);
		while (n > 0 && want[n - 1] == ' ') n--;
		want[n] = 0;
		if (strcmp(row(top_row + r), want)) {
			snprintf(why, (size_t)cap, "row %d: '%s', not '%s'", r, row(top_row + r), want);
			return false;
		}
	}
	if (ed->focus < 0 && ed->mode == 0) {
		// the cursor's row: the last whose start is at or before it, except
		// that a row's end joined to the next (no space) belongs to the next
		int ci = 0;
		while (ci < ncp && (int)cpoff[ci] < (int)ed->cur) ci++;
		int ri = 0;
		for (int i = 0; i < nrows; i++) if (rs[i] <= ci) ri = i;
		int want_r = top_row + ri - first, want_c = ci - rs[ri];
		if (want_c > ed->cfg.cols - 1) want_c = ed->cfg.cols - 1;
		if (vt.cursor_y != want_r || vt.cursor_x != want_c) {
			snprintf(why, (size_t)cap, "cursor at %d,%d, not %d,%d", vt.cursor_y, vt.cursor_x, want_r, want_c);
			return false;
		}
	}
	return true;
}

// -- an app --

static char to_buf[64], subj_buf[128], text[8192], clip[2048];
static const char *check_to(const char *v, void *ctx) {
	(void)ctx;
	return strcmp(v, "nobody") ? NULL : "There is no user called nobody.";
}

enum { A_POST = 1, A_QUOTE = 2 };

static zetta_cfg_t app_cfg(void) {
	zetta_cfg_t c;
	memset(&c, 0, sizeof(c));
	c.rows = 25; c.cols = 80;
	c.title = "Test BBS - new message";
	c.fields[0].label = "To"; c.fields[0].buf = to_buf; c.fields[0].cap = sizeof(to_buf); c.fields[0].check = check_to;
	c.fields[1].label = "Subject"; c.fields[1].buf = subj_buf; c.fields[1].cap = sizeof(subj_buf);
	c.nfields = 2;
	c.actions[0].key = ZK_CTRL('S'); c.actions[0].label = "Post"; c.actions[0].id = A_POST;
	c.actions[1].key = ZK_CTRL('Q'); c.actions[1].label = "Quote"; c.actions[1].id = A_QUOTE;
	c.nactions = 2;
	c.main_action = A_POST;
	c.clip = clip; c.clip_cap = sizeof(clip);
	c.write = wr;
	return c;
}

static const char *orig[] = { "The first line of the original.", "A second one.", "And a third.", NULL };
static const char *orig_line(void *ctx, int i) { (void)ctx; return i < 3 ? orig[i] : NULL; }

int main(void) {
	static zetta_t ed, ed2;
	char why[200];
	zetta_cfg_t cfg = app_cfg();

	// -- 1. the screen, and the fields --
	vt_init(&vt);
	strcpy(to_buf, "phil");
	subj_buf[0] = 0;
	text[0] = 0;
	zetta_init(&ed, &cfg, text, sizeof(text));
	CK(strstr(row(0), "Test BBS - new message") != NULL, "the title (%s)", row(0));
	CK(!strcmp(row(1), "To: phil") && !strcmp(row(2), "Subject:"), "the fields (%s | %s)", row(1), row(2));
	CK(strstr(row(23), "^S Post") && strstr(row(23), "^Q Quote"), "the app's keys in the help bar (%s)", row(23));
	CK(strstr(row(24), "^X Exit") && strstr(row(24), "Esc Menu"), "and zetta's (%s)", row(24));
	CK(ed.focus == 1 && vt.cursor_y == 2 && vt.cursor_x == 9, "an empty Subject: the cursor starts there (%d,%d)", vt.cursor_y, vt.cursor_x);
	type(&ed, "Hello there\r");
	CK(!strcmp(subj_buf, "Hello there") && ed.focus == -1, "Enter leaves the field for the text");
	CK(vt.cursor_y == 3 && vt.cursor_x == 0, "the cursor at the text's start (%d,%d)", vt.cursor_y, vt.cursor_x);

	// -- 2. wrapping at words, and reflow --
	const char *para = "The quick brown fox jumps over the lazy dog, and then it keeps on running across "
		"the wide green meadow until the sun goes down behind the hills far away.";
	type(&ed, para);
	CK(screen_agrees(&ed, why, sizeof(why)), "a long paragraph wraps at words: %s", why);
	CK(!strchr(row(3), '\0') || strlen(row(3)) <= 79, "no row wider than 79");
	CK(!strncmp(row(4), "across the wide", 15) && row(3)[strlen(row(3)) - 1] == 'g', "the second row starts with a whole word (%s)", row(4));
	type(&ed, "\x1b[H");								// Home: the start of the row
	type(&ed, "\x1bOA");								// Up (ESC O form): the first row
	type(&ed, "\x1b[H");
	type(&ed, "Suddenly, ");
	CK(screen_agrees(&ed, why, sizeof(why)), "a word put in at the start reflows the rest: %s", why);
	CK(!strncmp(row(3), "Suddenly, The quick", 19), "(%s)", row(3));

	// -- 3. saved as lines of at most 79, and as typed --
	{
		static char o[8192];
		uint32_t n = zetta_text(&ed, o, sizeof(o), 79);
		bool ok = true;
		for (char *l = o; *l; ) { char *e = strchr(l, '\n'); int ll = e ? (int)(e - l) : (int)strlen(l); if (ll > 79) ok = false; if (!e) break; l = e + 1; }
		CK(n > 0 && ok && strstr(o, "Suddenly, The quick"), "zetta_text(79): every line within 79 columns");
		CK(o[n - 1] != '\n' && o[n - 1] != ' ', "and nothing added after the last word");
		n = zetta_text(&ed, o, sizeof(o), 0);
		CK(n == ed.len && !strchr(o, '\n'), "zetta_text(0): the paragraph whole, as typed");
	}

	// -- 4. Enter, Backspace, Delete, movement --
	type(&ed, "\x1b[F\x1b[B\x1b[F\r\rA new paragraph.");
	CK(screen_agrees(&ed, why, sizeof(why)), "Enter makes paragraphs: %s", why);
	CK(on_screen("A new paragraph."), "shown");
	type(&ed, "\x1b[H\x7f\x7f");								// two Backspaces: joined back
	CK(screen_agrees(&ed, why, sizeof(why)), "Backspace joins paragraphs: %s", why);
	type(&ed, "\x1b[3~");										// Delete
	CK(screen_agrees(&ed, why, sizeof(why)), "Delete: %s", why);

	// -- 5. cut and paste --
	{
		uint32_t before = ed.len;
		type(&ed, "\x0b");										// Ctrl-K
		CK(ed.len < before && clip[0], "Ctrl-K cuts the paragraph (%u -> %u)", (unsigned)before, (unsigned)ed.len);
		type(&ed, "\x15");										// Ctrl-U
		CK(ed.len == before, "Ctrl-U puts it back (%u)", (unsigned)ed.len);
		CK(screen_agrees(&ed, why, sizeof(why)), "%s", why);
	}

	// -- 6. search --
	type(&ed, "\x17" "MEADOW\r");									// Ctrl-W, case ignored
	CK(!strncmp(ed.buf + ed.cur, "meadow", 6), "Ctrl-W finds the word, whatever its case");
	type(&ed, "\x17" "zebra\r");
	CK(on_screen("Not found."), "and says when it cannot");

	// -- 7. the app's actions; Ctrl-Z; the menu; bytes after an event kept --
	CK(type(&ed, "\x13") == ZE_ACTION && ed.action == A_POST, "Ctrl-S: the app's Post");
	CK(type(&ed, "\x1a") == ZE_ACTION && ed.action == A_POST, "Ctrl-Z: its main action");
	{
		uint32_t before = ed.len;
		int r = type(&ed, "\x11xyz");								// Ctrl-Q, then typing at once
		CK(r == ZE_ACTION && ed.action == A_QUOTE && ed.len == before, "Ctrl-Q: Quote, returned before the rest is typed");
		now_ms += 10;
		zetta_tick(&ed, now_ms);
		CK(ed.len == before + 3, "... and the rest is typed next time, not lost (%u)", (unsigned)(ed.len - before));
		type(&ed, "\x7f\x7f\x7f");
	}
	CK(type(&ed, "\x1b[A") == ZE_NONE && ed.mode == 0, "an arrow's Esc is not the menu");
	esc_alone(&ed);
	CK(ed.mode != 0 && on_screen("Post") && on_screen("Quote") && on_screen("Search") && on_screen("Exit"),
		"a lone Esc, after a pause: the menu, the app's actions and zetta's");
	CK(type(&ed, "\x1b[B\r") == ZE_ACTION && ed.action == A_QUOTE, "Down, Enter: the second, Quote");
	CK(type(&ed, "\x18") == ZE_EXIT, "Ctrl-X: Exit, for the app to decide");

	// -- 7b. what the app says while handling an event shows at once --
	type(&ed, "\x13");
	zetta_error(&ed, "A subject, please.");
	CK(on_screen("A subject, please."), "an error set after the event is drawn now, not at the next key");
	zetta_status(&ed, "Saved.");
	CK(on_screen("Saved.") && !on_screen("A subject, please."), "and a status replaces it at once");

	// -- 8. the app's dialogs --
	zetta_confirm(&ed, "Discard this message?", 7);
	CK(on_screen("Discard this message? (y/n)"), "a question on the status line");
	CK(type(&ed, "q") == ZE_NONE, "anything but y, n or Esc: still asking");
	CK(type(&ed, "n") == ZE_ANSWER && ed.answer == 'n' && ed.answer_tag == 7, "n: answered, with its tag");
	{
		uint32_t before = ed.len;
		zetta_pick(&ed, "Quote", orig_line, NULL, "PH> ", 9);
		CK(on_screen("[ ] The first line of the original."), "the quote window lists the original");
		int r = type(&ed, " \x1b[B\x1b[B \r");					// the first and the third
		CK(r == ZE_ANSWER && ed.answer == 'y' && ed.answer_tag == 9, "Enter: answered");
		CK(ed.len > before && strstr(ed.buf, "PH> The first line of the original.\nPH> And a third.\n") &&
			!strstr(ed.buf, "PH> A second one."), "the chosen lines put in, each after the prefix");
		CK(screen_agrees(&ed, why, sizeof(why)), "%s", why);
	}

	// -- 9. a field's check --
	type(&ed, "\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A");
	type(&ed, "\x1b[A");										// from the Subject to To
	CK(ed.focus == 0, "Up from the first row: the fields (%d)", ed.focus);
	type(&ed, "\x7f\x7f\x7f\x7fnobody\r");
	CK(ed.focus == 0 && on_screen("There is no user called nobody."), "the app's check refuses; the field keeps the cursor");
	type(&ed, "\x7f\x7f\x7f\x7f\x7f\x7f" "anna\r");
	CK(ed.focus == 1 && !strcmp(to_buf, "anna"), "a good one: on to the next field");

	// -- 10. UTF-8: characters, not bytes --
	{
		static char t2[4096];
		zetta_cfg_t c2 = app_cfg();
		c2.nfields = 0;
		t2[0] = 0;
		vt_init(&vt);
		zetta_init(&ed2, &c2, t2, sizeof(t2));
		for (int i = 0; i < 100; i++) type(&ed2, "\xc3\xa9");			// e acute, 2 bytes each
		CK(ed2.len == 200 && screen_agrees(&ed2, why, sizeof(why)), "100 e-acutes: wrapped at 79 characters, not bytes: %s", why);
		type(&ed2, " Gr\xc3\xbc\xc3\x9f" "e");
		CK(on_screen("Gr\xfc\xdf" "e"), "Latin characters shown as themselves");
	}

	// -- 11. economy: a keystroke costs a row, not a screen --
	{
		vt_init(&vt);
		zetta_redraw(&ed);
		written = 0;
		type(&ed, "x");
		CK(written < 250, "one character typed: %u bytes sent", (unsigned)written);
	}

	// -- 12. two editors at once (a BBS: one a caller) keep their input apart --
	{
		static char ta[256], tb[256];
		static zetta_t ea, eb;
		zetta_cfg_t c = app_cfg();
		c.nfields = 0;
		ta[0] = tb[0] = 0;
		zetta_init(&ea, &c, ta, sizeof(ta));
		zetta_init(&eb, &c, tb, sizeof(tb));
		type(&ea, "\x13" "aaa");					// an action, then text held
		type(&eb, "\x13" "bbb");
		zetta_tick(&ea, now_ms += 10);
		zetta_tick(&eb, now_ms += 10);
		CK(!strcmp(ta, "aaa") && !strcmp(tb, "bbb"), "each editor's held input is its own (%s, %s)", ta, tb);
	}

	// -- 13. thousands of random keys: the screen always agrees -- five
	//    seeds, at 80 columns and at 40 (more words on the edges) --
	for (int run = 0; run < 10; run++) {
		static char t3[3000];
		static zetta_t e3;
		zetta_cfg_t c = app_cfg();
		int wcols = run < 5 ? 80 : 40;
		c.cols = wcols;
		c.nfields = 1;
		strcpy(to_buf, "x");
		c.nactions = 0;
		t3[0] = 0;
		vt_init(&vt);
		zetta_init(&e3, &c, t3, sizeof(t3));
		srand(12345u + (unsigned)run);
		static const char *const keys[] = {
			"a", "b", " ", " ", " ", "word ", "longerword", "\r", "\x7f", "\x1b[3~", "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D",
			"\x1b[H", "\x1b[F", "\x1b[5~", "\x1b[6~", "\x0b", "\x15", "\xc3\xa9", "\xe2\x82\xac", "supercalifragilisticexpialidocious",
			"\x01", "\x05", "\t",
		};
		int bad = 0, first_bad = -1;
		char first_why[200] = "";
		for (int i = 0; i < 6000; i++) {
			const char *k = keys[rand() % (int)(sizeof(keys) / sizeof(keys[0]))];
			type(&e3, k);
			if (e3.focus >= 0) type(&e3, "\r");				// back to the text
			if (!screen_agrees(&e3, why, sizeof(why))) {
				if (!bad) { first_bad = i; snprintf(first_why, sizeof(first_why), "%s", why); }
				bad++;
			}
			if (e3.len > 2500) { type(&e3, "\x1b[5~\x1b[5~\x1b[5~\x1b[5~\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b"); }
		}
		CK(bad == 0, "%d columns, seed %d: 6000 random keys, screen and cursor agree after every one (%d wrong, first at %d: %s)",
			wcols, run % 5, bad, first_bad, first_why);
		static char o[4000];
		zetta_text(&e3, o, sizeof(o), wcols - 1);
		bool ok = true;
		to_cps(o, (uint32_t)strlen(o));
		int col = 0;
		for (int i = 0; i < ncp; i++) { if (cp[i] == '\n') col = 0; else if (++col > wcols - 1) ok = false; }
		CK(ok, "and its text, saved at %d: no line longer", wcols - 1);
	}

	printf("zetta: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
