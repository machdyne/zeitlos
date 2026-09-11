/*
 * Test suite for the VT100 terminal emulation core (sw/common/zvt100.c)
 *
 * Runs entirely on the host (no target hardware/toolchain needed) --
 * see sw/apps/term/term.c for what actually runs on-device. This is
 * the primary way to validate zvt100.c changes: fast, deterministic,
 * no window/messaging/keyboard involved, matching this project's
 * general "verify one layer before wiring up the next" approach
 * (docs/user_input.md's keyboard work, docs/networking.md's TFTP
 * bring-up).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zvt100.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
	do { \
		printf("Running test: %s\n", name); \
		tests_run++; \
	} while(0)

#define TEST_ASSERT(condition, message) \
	do { \
		if (condition) { \
			printf("  \xe2\x9c\x93 %s\n", message); \
		} else { \
			printf("  \xe2\x9c\x97 %s\n", message); \
			tests_failed++; \
			return 0; \
		} \
	} while(0)

#define TEST_END() \
	do { \
		tests_passed++; \
		printf("  Test passed\n\n"); \
		return 1; \
	} while(0)

static void feed_str(vt_screen_t *vt, const char *s) {
	vt_feed(vt, (const uint8_t *)s, (uint32_t)strlen(s));
}

static int test_init_is_blank(void) {
	TEST_START("init produces a blank screen at (0,0)");
	vt_screen_t vt;
	vt_init(&vt);
	TEST_ASSERT(vt.cursor_x == 0 && vt.cursor_y == 0, "cursor starts at (0,0)");
	TEST_ASSERT(vt.cells[0][0].ch == ' ', "cell (0,0) is blank");
	TEST_ASSERT(vt.cells[VT_ROWS - 1][VT_COLS - 1].ch == ' ', "last cell is blank");
	TEST_ASSERT(!vt.reverse, "reverse starts off");
	TEST_END();
}

static int test_plain_text(void) {
	TEST_START("plain text advances the cursor and writes cells");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "Hi");
	TEST_ASSERT(vt.cells[0][0].ch == 'H', "first cell is 'H'");
	TEST_ASSERT(vt.cells[0][1].ch == 'i', "second cell is 'i'");
	TEST_ASSERT(vt.cursor_x == 2 && vt.cursor_y == 0, "cursor advanced to (2,0)");
	TEST_END();
}

static int test_deferred_wrap(void) {
	TEST_START("deferred wrap: exactly-80-wide content doesn't insert a blank line");
	vt_screen_t vt;
	vt_init(&vt);
	for (int i = 0; i < VT_COLS; i++) feed_str(&vt, "A");
	feed_str(&vt, "B");
	TEST_ASSERT(vt.cells[0][VT_COLS - 1].ch == 'A', "last column of row 0 is 'A'");
	TEST_ASSERT(vt.cells[1][0].ch == 'B', "'B' landed at the START of row 1, not row 2");
	TEST_ASSERT(vt.cursor_y == 1, "cursor is on row 1, not row 2");
	TEST_END();
}

static int test_cr_lf(void) {
	TEST_START("CR resets column, LF moves down without resetting column");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "abc");
	vt_feed_byte(&vt, '\n');
	TEST_ASSERT(vt.cursor_x == 3 && vt.cursor_y == 1, "LF alone keeps column (true VT100 semantics)");
	vt_feed_byte(&vt, '\r');
	TEST_ASSERT(vt.cursor_x == 0 && vt.cursor_y == 1, "CR resets column, doesn't move row");
	TEST_END();
}

static int test_backspace(void) {
	TEST_START("backspace moves the cursor left without erasing");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "XXXX\b\b\bYY");
	TEST_ASSERT(strncmp(&vt.cells[0][0].ch, "X", 1) == 0, "cell 0 still 'X'");
	TEST_ASSERT(vt.cells[0][1].ch == 'Y', "cell 1 overwritten with 'Y'");
	TEST_ASSERT(vt.cells[0][2].ch == 'Y', "cell 2 overwritten with 'Y'");
	TEST_ASSERT(vt.cells[0][3].ch == 'X', "cell 3 still 'X' (not reached by the 2nd Y)");
	TEST_END();
}

static int test_tab(void) {
	TEST_START("tab stops every 8 columns");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "A\tB");
	TEST_ASSERT(vt.cells[0][0].ch == 'A', "'A' at column 0");
	TEST_ASSERT(vt.cells[0][8].ch == 'B', "'B' at column 8 (next tab stop)");
	TEST_END();
}

static int test_cursor_position(void) {
	TEST_START("CSI H (CUP) moves the cursor to a 1-indexed row;col");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "\x1b[10;5HX");
	TEST_ASSERT(vt.cells[9][4].ch == 'X', "'X' landed at 0-indexed (row=9,col=4)");
	TEST_END();
}

static int test_cursor_movement(void) {
	TEST_START("CSI A/B/C/D move the cursor relative to its current position");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "\x1b[5;5H");        // start at (row=4,col=4) 0-indexed
	feed_str(&vt, "\x1b[2A");          // up 2
	TEST_ASSERT(vt.cursor_y == 2, "CUU moved up 2 rows");
	feed_str(&vt, "\x1b[3B");          // down 3
	TEST_ASSERT(vt.cursor_y == 5, "CUD moved down 3 rows net");
	feed_str(&vt, "\x1b[4C");          // right 4
	TEST_ASSERT(vt.cursor_x == 8, "CUF moved right 4 cols net");
	feed_str(&vt, "\x1b[1D");          // left 1
	TEST_ASSERT(vt.cursor_x == 7, "CUB moved left 1 col net");
	TEST_END();
}

static int test_reverse_video(void) {
	TEST_START("SGR 7/0 toggles the reverse attribute on written cells");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "a\x1b[7mb\x1b[0mc");
	TEST_ASSERT(!vt.cells[0][0].reverse, "'a' (before SGR 7) is not reverse");
	TEST_ASSERT(vt.cells[0][1].reverse, "'b' (inside SGR 7) is reverse");
	TEST_ASSERT(!vt.cells[0][2].reverse, "'c' (after SGR 0) is not reverse");
	TEST_END();
}

static int test_erase_in_line(void) {
	TEST_START("CSI K (EL) erases from the cursor to end of line by default");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "ABCDE\x1b[1;3H\x1b[K");   // move to col 3 (0-indexed 2), erase to EOL
	TEST_ASSERT(vt.cells[0][0].ch == 'A', "col 0 untouched");
	TEST_ASSERT(vt.cells[0][1].ch == 'B', "col 1 untouched");
	TEST_ASSERT(vt.cells[0][2].ch == ' ', "col 2 erased");
	TEST_ASSERT(vt.cells[0][4].ch == ' ', "col 4 erased");
	TEST_END();
}

static int test_erase_in_display(void) {
	TEST_START("CSI J (ED) erases from the cursor to end of screen by default");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "\x1b[1;1HROW0");
	feed_str(&vt, "\x1b[2;1HROW1");
	feed_str(&vt, "\x1b[2;1H\x1b[J");   // erase from (row1,col0) to end of screen
	TEST_ASSERT(vt.cells[0][0].ch == 'R', "row 0 (before the cursor's row) untouched");
	TEST_ASSERT(vt.cells[1][0].ch == ' ', "row 1 (cursor's own row) erased");
	TEST_END();
}

static int test_scrolling(void) {
	TEST_START("writing past the bottom row scrolls the screen up");
	vt_screen_t vt;
	vt_init(&vt);
	feed_str(&vt, "TOP\r\n");
	for (int i = 0; i < VT_ROWS + 5; i++) {
		char buf[16];
		snprintf(buf, sizeof(buf), "L%d\r\n", i);
		feed_str(&vt, buf);
	}
	feed_str(&vt, "BOTTOM");
	TEST_ASSERT(vt.cells[0][0].ch != 'T', "row 0 no longer says 'TOP' (scrolled away)");
	TEST_ASSERT(vt.cells[VT_ROWS - 1][0].ch == 'B', "bottom row starts with 'BOTTOM'");
	TEST_ASSERT(vt.cursor_y == VT_ROWS - 1, "cursor stayed pinned to the bottom row");
	TEST_END();
}

static int test_dirty_tracking(void) {
	TEST_START("dirty-row tracking marks only rows that actually changed");
	vt_screen_t vt;
	vt_init(&vt);
	vt_clear_dirty(&vt);
	feed_str(&vt, "\x1b[3;1Hhello");
	TEST_ASSERT(vt_row_dirty(&vt, 2), "row 2 (0-indexed row of the write) is dirty");
	TEST_ASSERT(!vt_row_dirty(&vt, 0), "row 0 (untouched) is not dirty");
	vt_clear_dirty(&vt);
	TEST_ASSERT(!vt_row_dirty(&vt, 2), "row 2 no longer dirty after vt_clear_dirty()");
	TEST_END();
}


/* -- scrollback history -- */

/* Text of document line `doc` with trailing blanks dropped, reading
 * through the public API only. */
static const char *doc_text(const vt_screen_t *vt, int doc) {
	static char buf[VT_COLS + 1];
	int last = -1;
	for (int c = 0; c < VT_COLS; c++) {
		buf[c] = VT_PACK_CH(vt_doc_cell(vt, doc, c));
		if (buf[c] != ' ') last = c;
	}
	buf[last + 1] = 0;
	return buf;
}

static void feed_numbered(vt_screen_t *vt, int from, int to) {
	char line[16];
	for (int i = from; i <= to; i++) {
		snprintf(line, sizeof(line), "L%d\r\n", i);
		feed_str(vt, line);
	}
}

static int test_history_detached(void) {
	TEST_START("no history attached: scrolling works and keeps nothing");
	vt_screen_t vt;
	vt_init(&vt);
	feed_numbered(&vt, 0, 40);
	TEST_ASSERT(vt_history_count(&vt) == 0, "count stays 0");
	TEST_ASSERT(vt_history_pushed(&vt) == 0, "pushed stays 0");
	TEST_ASSERT(strcmp(doc_text(&vt, 0), "L17") == 0, "doc 0 is screen row 0");
	TEST_END();
}

static int test_history_linefeed_pushes(void) {
	TEST_START("a linefeed scroll pushes the departing top row");
	static uint8_t buf[10 * VT_COLS];
	vt_screen_t vt;
	vt_init(&vt);
	vt_history_attach(&vt, buf, 10);
	/* 25 rows hold L0..L23 plus the blank row the last CRLF opened;
	 * L24's CRLF is the first to scroll. */
	feed_numbered(&vt, 0, 23);
	TEST_ASSERT(vt_history_count(&vt) == 0, "nothing pushed while the screen fills");
	feed_numbered(&vt, 24, 26);
	TEST_ASSERT(vt_history_count(&vt) == 3, "three lines pushed");
	TEST_ASSERT(strcmp(doc_text(&vt, 0), "L0") == 0, "oldest is L0");
	TEST_ASSERT(strcmp(doc_text(&vt, 2), "L2") == 0, "newest is L2");
	TEST_ASSERT(strcmp(doc_text(&vt, 3), "L3") == 0, "doc 3 is screen row 0");
	TEST_ASSERT(vt.scrolls == 3, "scroll counter agrees");
	TEST_END();
}

static int test_history_wraps_and_evicts(void) {
	TEST_START("the ring evicts the oldest line once full");
	static uint8_t buf[10 * VT_COLS];
	vt_screen_t vt;
	vt_init(&vt);
	vt_history_attach(&vt, buf, 10);
	feed_numbered(&vt, 0, 23 + 37);		/* 37 pushes into a 10-line ring */
	TEST_ASSERT(vt_history_count(&vt) == 10, "count capped at 10");
	TEST_ASSERT(vt_history_pushed(&vt) == 37, "pushed counts every line");
	TEST_ASSERT(strcmp(doc_text(&vt, 0), "L27") == 0, "oldest retained is L27");
	TEST_ASSERT(strcmp(doc_text(&vt, 9), "L36") == 0, "newest is L36");
	TEST_ASSERT(strcmp(doc_text(&vt, 10), "L37") == 0, "then the screen");
	TEST_END();
}

static int test_history_absolute_ids(void) {
	TEST_START("absolute ids stay attached to their text");
	static uint8_t buf[10 * VT_COLS];
	vt_screen_t vt;
	uint8_t b;
	vt_init(&vt);
	vt_history_attach(&vt, buf, 10);
	feed_numbered(&vt, 0, 23 + 5);
	/* screen row 4 holds L9; its id is pushed + 4 */
	uint32_t id = vt_history_pushed(&vt) + 4;
	TEST_ASSERT(vt_id_cell(&vt, id, 1, &b) && VT_PACK_CH(b) == '9', "row 4 is L9");
	feed_numbered(&vt, 29, 35);			/* L9 scrolls into history */
	TEST_ASSERT(vt_id_cell(&vt, id, 0, &b) && VT_PACK_CH(b) == 'L', "same id, same line (L)");
	TEST_ASSERT(vt_id_cell(&vt, id, 1, &b) && VT_PACK_CH(b) == '9', "same id, same line (9)");
	feed_numbered(&vt, 36, 60);			/* and out of the ring */
	TEST_ASSERT(!vt_id_cell(&vt, id, 0, &b), "evicted id reports false");
	TEST_ASSERT(!vt_id_cell(&vt, vt_history_pushed(&vt) + VT_ROWS, 0, &b),
		"below the screen reports false");
	TEST_END();
}

static int test_history_dl_does_not_push(void) {
	TEST_START("DL at row 0 scrolls but does not save (editor scrolling)");
	static uint8_t buf[10 * VT_COLS];
	vt_screen_t vt;
	vt_init(&vt);
	vt_history_attach(&vt, buf, 10);
	feed_str(&vt, "\x1b[1;1Htop line");
	feed_str(&vt, "\x1b[1;1H\x1b[3M");
	TEST_ASSERT(vt.scrolls == 3, "three scrolls counted for the renderer");
	TEST_ASSERT(vt_history_count(&vt) == 0, "nothing saved");
	TEST_END();
}

static int test_history_erase_display(void) {
	TEST_START("ED 2 keeps history; ED 3 clears it and not the screen");
	static uint8_t buf[10 * VT_COLS];
	vt_screen_t vt;
	vt_init(&vt);
	vt_history_attach(&vt, buf, 10);
	feed_numbered(&vt, 0, 23 + 4);
	feed_str(&vt, "\x1b[2J");
	TEST_ASSERT(vt_history_count(&vt) == 4, "ED 2 left history alone");
	feed_str(&vt, "\x1b[1;1Hkeep");
	feed_str(&vt, "\x1b[3J");
	TEST_ASSERT(vt_history_count(&vt) == 0, "ED 3 emptied history");
	TEST_ASSERT(vt_history_pushed(&vt) == 4, "pushed does not go backwards");
	TEST_ASSERT(vt.cells[0][0].ch == 'k', "ED 3 left the screen alone");
	TEST_END();
}

static int test_history_packs_reverse(void) {
	TEST_START("reverse video survives the trip into history");
	static uint8_t buf[4 * VT_COLS];
	vt_screen_t vt;
	vt_init(&vt);
	vt_history_attach(&vt, buf, 4);
	feed_str(&vt, "\x1b[7mR\x1b[0mn~\r\n");
	for (int i = 0; i < VT_ROWS - 1; i++) feed_str(&vt, "\r\n");
	TEST_ASSERT(vt_history_count(&vt) == 1, "one line pushed");
	uint8_t a = vt_doc_cell(&vt, 0, 0), b = vt_doc_cell(&vt, 0, 1);
	uint8_t c = vt_doc_cell(&vt, 0, 2);
	TEST_ASSERT(VT_PACK_CH(a) == 'R' && VT_PACK_REV(a), "R is reverse");
	TEST_ASSERT(VT_PACK_CH(b) == 'n' && !VT_PACK_REV(b), "n is normal");
	TEST_ASSERT(VT_PACK_CH(c) == '~', "0x7e packs losslessly");
	TEST_END();
}

static void print_test_summary(void) {
	printf("=== Test Summary ===\n");
	printf("Tests run: %d\n", tests_run);
	printf("Tests passed: %d\n", tests_passed);
	printf("Tests failed: %d\n", tests_failed);
}

int main(void) {
	printf("=== zvt100 Test Suite ===\n\n");

	test_init_is_blank();
	test_plain_text();
	test_deferred_wrap();
	test_cr_lf();
	test_backspace();
	test_tab();
	test_cursor_position();
	test_cursor_movement();
	test_reverse_video();
	test_erase_in_line();
	test_erase_in_display();
	test_scrolling();
	test_dirty_tracking();
	test_history_detached();
	test_history_linefeed_pushes();
	test_history_wraps_and_evicts();
	test_history_absolute_ids();
	test_history_dl_does_not_push();
	test_history_erase_display();
	test_history_packs_reverse();

	print_test_summary();
	printf("\n");

	return (tests_failed == 0) ? 0 : 1;
}
