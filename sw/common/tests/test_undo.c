/*
 * Host tests for sw/common/zundo.h.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/test_undo sw/common/tests/test_undo.c
 *   /tmp/test_undo
 *
 * See docs/text_editor.md, "Undo".
 */

#include <stdio.h>
#include <string.h>

#include "zundo.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

// A tiny editor: one buffer, edits recorded as text.c records them.
static char doc[256];
static int len, cur;
static z_undo_t u;
static z_undo_rec_t recs[8];
static char bytes[32];

static void ins(int at, const char *s, uint16_t g) {
	int n = (int)strlen(s);
	z_undo_record(&u, Z_UNDO_INS, at, s, n, cur, g);
	memmove(&doc[at + n], &doc[at], (size_t)(len - at));
	memcpy(&doc[at], s, (size_t)n);
	len += n;
	cur = at + n;
	doc[len] = 0;
}

static void del(int a, int b, uint16_t g) {
	z_undo_record(&u, Z_UNDO_DEL, a, &doc[a], b - a, cur, g);
	memmove(&doc[a], &doc[b], (size_t)(len - b));
	len -= b - a;
	cur = a;
	doc[len] = 0;
}

static bool undo(void) {
	int from;
	bool ok = z_undo_undo(&u, doc, &len, sizeof(doc) - 1, &cur, &from);
	doc[len] = 0;
	return ok;
}

static bool redo(void) {
	int from;
	bool ok = z_undo_redo(&u, doc, &len, sizeof(doc) - 1, &cur, &from);
	doc[len] = 0;
	return ok;
}

static void fresh(void) {
	z_undo_init(&u, recs, 8, bytes, sizeof(bytes));
	z_undo_reset(&u);
	len = cur = 0;
	doc[0] = 0;
}

int main(void) {

	// Typing merges; one undo takes the group back.
	fresh();
	ins(0, "h", 1); ins(1, "i", 1); ins(2, "!", 1);
	CHECK(!strcmp(doc, "hi!"), "typed");
	CHECK(u.top - u.first == 1, "a typed run is one record");
	CHECK(undo() && !strcmp(doc, "") && cur == 0, "undo the run");
	CHECK(!undo(), "nothing more");
	CHECK(redo() && !strcmp(doc, "hi!") && cur == 3, "redo");
	CHECK(!redo(), "nothing more to redo");

	// Groups: two groups, undone one at a time.
	fresh();
	ins(0, "ab", 1);
	ins(2, "cd", 2);
	CHECK(undo() && !strcmp(doc, "ab"), "second group");
	CHECK(undo() && !strcmp(doc, ""), "first group");

	// Deletes, and a group of a delete and an insert (replace).
	fresh();
	ins(0, "hello world", 1);
	del(6, 11, 2);
	ins(6, "there", 2);
	CHECK(!strcmp(doc, "hello there"), "replaced");
	CHECK(undo() && !strcmp(doc, "hello world") && cur == 11, "replace undone in one step, caret back");
	CHECK(redo() && !strcmp(doc, "hello there"), "and redone");

	// A new edit clears redo.
	fresh();
	ins(0, "abc", 1);
	undo();
	ins(0, "x", 2);
	CHECK(!redo() && !strcmp(doc, "x"), "an edit ends the redo history");

	// The saved point.
	fresh();
	ins(0, "abc", 1);
	z_undo_saved(&u);
	ins(3, "d", 2);
	CHECK(!z_undo_at_saved(&u), "modified after save");
	undo();
	CHECK(z_undo_at_saved(&u) && !strcmp(doc, "abc"), "undo back to the save is unmodified");
	undo();
	CHECK(!z_undo_at_saved(&u), "past the save is modified again");
	redo();
	CHECK(z_undo_at_saved(&u), "redo back to it is unmodified");

	// Typing right after a save is not merged into the saved record.
	fresh();
	ins(0, "ab", 1);
	z_undo_saved(&u);
	ins(2, "c", 1);
	undo();
	CHECK(!strcmp(doc, "ab") && z_undo_at_saved(&u), "undo stops at the save point");

	// Full: the oldest is forgotten, the rest still undo correctly.
	fresh();
	for (int i = 0; i < 12; i++) {
		char s[2] = { (char)('a' + i), 0 };
		ins(len, s, (uint16_t)(i + 1));			// 12 groups, 8 records
	}
	CHECK(!strcmp(doc, "abcdefghijkl"), "twelve edits");
	int undone = 0;
	while (undo()) undone++;
	CHECK(undone == 8, "the last eight can be undone");
	CHECK(!strcmp(doc, "abcd"), "back to where the kept history starts");

	// Bytes full: a big delete pushes out older edits.
	fresh();
	ins(0, "0123456789012345678901234", 1);		// 25 bytes of 32
	del(0, 20, 2);								// 20 more -- first is dropped
	CHECK(undo() && !strcmp(doc, "0123456789012345678901234"), "the big delete undoes");
	CHECK(!undo(), "the insert before it was forgotten");

	// An edit bigger than the whole byte ring clears history.
	fresh();
	ins(0, "abc", 1);
	char big[40];
	memset(big, 'x', 39); big[39] = 0;
	ins(3, big, 2);
	CHECK(!undo(), "an edit too big for the log leaves nothing to undo");
	CHECK(!z_undo_at_saved(&u), "and the document counts as modified");

	printf("%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;

}
