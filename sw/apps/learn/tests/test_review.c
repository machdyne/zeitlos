/* learn -- review: Leitner boxes, days, interleaving (host). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../lstore.h"
#include "../lreview.h"
#include "hostplat.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static unsigned char mem[256 << 10], rmem[192 << 10];

#define DAY 86400u

static bool due(const char *id) {
	int idx[64], n = lr_due(idx, 64, NULL);
	for (int i = 0; i < n; i++) if (!strcmp(lr_name(lr_get(idx[i])->id), id)) return true;
	return false;
}

int main(void) {
	LP = &host_plat;
	snprintf(hp_root, sizeof hp_root, "/tmp/learn_review_card");
	hp_reset_card();
	ls_who_t mia;
	CHECK(ls_create("Mia", LS_ON_MACHINE, &mia), "create Mia");
	CHECK(ls_open(&mia, mem, sizeof mem), "open");
	hp_now = 20000u * DAY + 3600;		// day 20000

	CHECK(lr_open(rmem, sizeof rmem), "review opens");
	CHECK(lr_today() == 20000, "today: %ld", (long)lr_today());
	CHECK(lr_count() == 0 && lr_count_due(NULL) == 0, "nothing yet");

	// a key from what a question asks; its own id if it has one
	{
		lq_q_t q;
		memset(&q, 0, sizeof q);
		q.kind = LQ_NUMBER;
		q.ask[0].text = "How many hints?";
		q.nask = 1;
		char k1[40], k2[40];
		lr_key(&q, k1, sizeof k1);
		q.ask[0].text = "How many hints ?";
		lr_key(&q, k2, sizeof k2);
		CHECK(k1[0] == 'q' && strlen(k1) == 9 && strcmp(k1, k2), "keys differ with the words: %s %s", k1, k2);
		q.id = "hints-max";
		lr_key(&q, k1, sizeof k1);
		CHECK(!strcmp(k1, "hints-max"), "its own id");
	}

	// in a lesson: right first time -> box 2 (back in 3 days); missed -> box 1 (tomorrow)
	CHECK(lr_answered("math-3/fr/cmp/same", "q1", 1, "01-fr/01-cmp/02-same.md", true, false), "answered");
	CHECK(lr_answered("math-3/fr/cmp/same", "q2", 2, "01-fr/01-cmp/02-same.md", false, false), "missed");
	CHECK(lr_answered("sci-3/plants/roots", "q1", 0, "01-plants/01-roots.md", false, false), "another course");
	CHECK(lr_answered("sci-3/plants/roots", "q2", 0, "01-plants/01-roots.md", false, false), "and again");
	CHECK(lr_box_of("math-3/fr/cmp/same", "q1") == 2 && lr_box_of("math-3/fr/cmp/same", "q2") == 1, "boxes 2 and 1");
	CHECK(lr_count() == 4 && lr_count_due(NULL) == 0, "four, none due today");

	// tomorrow: the missed ones; interleaved, a course at a time
	hp_now += DAY;
	CHECK(lr_open(rmem, sizeof rmem), "reopen tomorrow");
	CHECK(lr_count() == 4, "all four read back: %d", lr_count());
	CHECK(lr_count_due(NULL) == 3, "three due: %d", lr_count_due(NULL));
	{
		int idx[8], n = lr_due(idx, 8, NULL);
		char c0[48], c1[48];
		lr_course_of(lr_name(lr_get(idx[0])->id), c0, sizeof c0);
		lr_course_of(lr_name(lr_get(idx[1])->id), c1, sizeof c1);
		CHECK(n == 3 && strcmp(c0, c1), "courses in turn: %s then %s", c0, c1);
		CHECK(!strcmp(lr_name(lr_get(idx[0])->file), "01-fr/01-cmp/02-same.md") || !strcmp(lr_name(lr_get(idx[0])->file), "01-plants/01-roots.md"), "its file kept");
	}
	CHECK(lr_count_due("math-3") == 1 && lr_count_due("sci-3") == 2, "by course");
	CHECK(!due("math-3/fr/cmp/same#q1"), "the known one is not due yet");

	// reviewed: right -> up a box (box 2, 3 days); wrong -> box 1 again
	lr_answered("math-3/fr/cmp/same", "q2", 2, "01-fr/01-cmp/02-same.md", true, true);
	lr_answered("sci-3/plants/roots", "q1", 0, "01-plants/01-roots.md", false, true);
	CHECK(lr_box_of("math-3/fr/cmp/same", "q2") == 2 && lr_box_of("sci-3/plants/roots", "q1") == 1, "up and back");
	CHECK(lr_count_due(NULL) == 1, "one left today: %d", lr_count_due(NULL));

	// three days on: q1 and q2 of math (box 2), roots q1 (box 1), roots q2 (box 1)
	hp_now += 3 * DAY;
	lr_open(rmem, sizeof rmem);
	CHECK(due("math-3/fr/cmp/same#q1") && due("math-3/fr/cmp/same#q2") && due("sci-3/plants/roots#q1"), "due after three days");
	// up through the boxes: 7, 16, 35 days
	lr_answered("math-3/fr/cmp/same", "q1", 1, NULL, true, true);
	CHECK(lr_box_of("math-3/fr/cmp/same", "q1") == 3, "box 3");
	hp_now += 7 * DAY;
	lr_open(rmem, sizeof rmem);
	CHECK(due("math-3/fr/cmp/same#q1"), "after a week");
	lr_answered("math-3/fr/cmp/same", "q1", 1, NULL, true, true);
	hp_now += 15 * DAY;
	lr_open(rmem, sizeof rmem);
	CHECK(!due("math-3/fr/cmp/same#q1"), "not after 15 days (box 4: 16)");
	hp_now += DAY;
	lr_open(rmem, sizeof rmem);
	CHECK(due("math-3/fr/cmp/same#q1"), "after 16");
	lr_answered("math-3/fr/cmp/same", "q1", 1, NULL, true, true);
	CHECK(lr_box_of("math-3/fr/cmp/same", "q1") == 5, "box 5");
	lr_answered("math-3/fr/cmp/same", "q1", 1, NULL, true, true);
	CHECK(lr_box_of("math-3/fr/cmp/same", "q1") == 5, "box 5 is the last");
	// the file it was in is kept when a line has none
	{
		int idx[64], n = lr_due(idx, 64, "math-3");
		(void)n;
		bool ok = false;
		for (int i = 0; i < lr_count(); i++)
			if (!strcmp(lr_name(lr_get(i)->id), "math-3/fr/cmp/same#q1")) ok = !strcmp(lr_name(lr_get(i)->file), "01-fr/01-cmp/02-same.md");
		CHECK(ok, "file kept");
	}

	// a torn line (power cut) is not an answer
	{
		char p[700];
		snprintf(p, sizeof p, "%s/data/learn/learners/Mia/review.txt", hp_root);
		FILE *f = fopen(p, "ab");
		fputs("20030 r math-3/fr/cmp/same#q9 1 01-f", f);
		fclose(f);
	}
	int before = lr_count();
	lr_open(rmem, sizeof rmem);
	CHECK(lr_count() == before, "torn line ignored (%d, %d)", lr_count(), before);

	// no clock: each opening is a day
	hp_reset_card();
	CHECK(ls_create("Jonas", LS_ON_MACHINE, &mia) && ls_open(&mia, mem, sizeof mem), "Jonas");
	hp_now = 0;
	lr_open(rmem, sizeof rmem);
	int32_t d0 = lr_today();
	lr_answered("zeit-learn/start/questions", "q1", 1, "01-start/02-questions.md", false, false);
	CHECK(lr_count_due(NULL) == 0, "not today");
	lr_open(rmem, sizeof rmem);
	CHECK(lr_today() == d0 + 1 && lr_count_due(NULL) == 1, "the next opening: a day later, due (%ld %ld)", (long)d0, (long)lr_today());

	printf("test_review: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
