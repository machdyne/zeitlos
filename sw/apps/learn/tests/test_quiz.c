/* learn -- judging answers (host). */
#include <stdio.h>
#include <string.h>
#include "../lquiz.h"

static uint8_t mem[1 << 16];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static lq_q_t q;
static void load(const char *src, uint32_t seed) {
	lx_arena_t a; char err[96];
	lx_arena_init(&a, mem, sizeof mem);
	lx_node_t *f = lx_read(&a, src, strlen(src), 1, err, sizeof err);
	if (!f || !lq_load(&q, lx_nth(f, 0), seed)) { printf("FAIL load %s %s\n", src, err); fails++; }
}
static int pos_of(int opt) { for (int i = 0; i < q.nopt; i++) if (q.order[i] == opt) return i; return -1; }

int main(void) {
	load("(choice (ask \"Which is greater?\" (math \"2/3\") \"or\" (math \"2/5\"))"
		" (right \"2/3\") (wrong \"2/5\" \"Fifths are smaller.\") (wrong \"equal\" \"No.\") (hint \"h1\") (hint \"h2\"))", 7);
	CHECK(q.kind == LQ_CHOICE && q.nopt == 3 && q.nhints == 2 && q.nask == 4 && q.ask[1].math, "choice loaded");
	lq_result_t r = lq_judge_choice(&q, 1u << pos_of(0));
	CHECK(r.verdict == LQ_RIGHT, "right choice");
	r = lq_judge_choice(&q, 1u << pos_of(1));
	CHECK(r.verdict == LQ_WRONG && r.feedback && !strcmp(r.feedback, "Fifths are smaller."), "wrong choice feedback");
	CHECK(lq_judge_choice(&q, 0).verdict == LQ_UNREADABLE, "nothing chosen");
	{	// the same seed shuffles the same way; another may not
		int o1[3]; memcpy(o1, q.order, sizeof o1);
		load("(choice (ask \"x\") (right \"a\") (wrong \"b\" \"f\") (wrong \"c\" \"f\"))", 7);
		int o2[3]; memcpy(o2, q.order, sizeof o2);
		load("(choice (ask \"x\") (right \"a\") (wrong \"b\" \"f\") (wrong \"c\" \"f\"))", 7);
		CHECK(!memcmp(o2, q.order, sizeof o2), "deterministic shuffle");
		(void)o1;
		load("(choice (ask \"x\") (right \"a\") (wrong \"b\" \"f\") (keep-order))", 99);
		CHECK(q.order[0] == 0 && q.order[1] == 1, "keep-order");
	}
	load("(choice (ask \"x\") (several) (right \"a\") (right \"b\") (wrong \"c\" \"no\"))", 3);
	CHECK(lq_judge_choice(&q, (1u << pos_of(0)) | (1u << pos_of(1))).verdict == LQ_RIGHT, "several right");
	CHECK(lq_judge_choice(&q, 1u << pos_of(0)).verdict == LQ_WRONG, "several: only one");

	load("(number (ask \"T?\") (answer 1.42) (within 0.01) (unit \"s\") (wrong-if 2.84 \"Two swings.\"))", 1);
	CHECK(lq_judge_typed(&q, "1.42").verdict == LQ_RIGHT, "number");
	CHECK(lq_judge_typed(&q, "1.415").verdict == LQ_RIGHT, "number within");
	CHECK(lq_judge_typed(&q, " 1.42 s ").verdict == LQ_RIGHT, "number with unit");
	CHECK(lq_judge_typed(&q, "1.5").verdict == LQ_WRONG, "number wrong");
	r = lq_judge_typed(&q, "2.84");
	CHECK(r.verdict == LQ_WRONG && r.feedback && strstr(r.feedback, "Two"), "wrong-if number");
	CHECK(lq_judge_typed(&q, "abc").verdict == LQ_UNREADABLE, "not a number");
	load("(number (ask \"n\") (answer 1000))", 1);
	CHECK(lq_judge_typed(&q, "1,000").verdict == LQ_RIGHT, "thousands comma");
	CHECK(lq_judge_typed(&q, "999").verdict == LQ_WRONG, "exact number wrong");

	load("(fraction (ask \"half of 3/2\") (answer 3 4) (wrong-if (4 3) \"Upside down.\"))", 1);
	CHECK(lq_judge_typed(&q, "3/4").verdict == LQ_RIGHT, "fraction");
	CHECK(lq_judge_typed(&q, "6/8").verdict == LQ_NEARLY, "fraction equal value, other form");
	r = lq_judge_typed(&q, "4/3");
	CHECK(r.verdict == LQ_WRONG && r.feedback, "fraction wrong-if");
	load("(fraction (ask \"x\") (answer 3 4) (equivalent))", 1);
	CHECK(lq_judge_typed(&q, "6/8").verdict == LQ_RIGHT, "equivalent accepted");
	load("(fraction (ask \"x\") (answer 3 4) (equivalent) (simplest))", 1);
	CHECK(lq_judge_typed(&q, "6/8").verdict == LQ_NEARLY, "simplest wanted");
	load("(fraction (ask \"x\") (answer 5 2) (equivalent))", 1);
	CHECK(lq_judge_typed(&q, "2 1/2").verdict == LQ_RIGHT, "mixed number equivalent");

	load("(text (ask \"Ich ___ müde\") (accept \"bin\") (wrong-if \"ist\" \"That is for er, sie, es.\"))", 1);
	CHECK(lq_judge_typed(&q, "bin").verdict == LQ_RIGHT, "text");
	CHECK(lq_judge_typed(&q, "  Bin. ").verdict == LQ_RIGHT, "text case, spaces, full stop");
	r = lq_judge_typed(&q, "ist");
	CHECK(r.verdict == LQ_WRONG && r.feedback && strstr(r.feedback, "er, sie"), "text wrong-if");
	load("(text (ask \"x\") (accept \"m\xC3\xBC" "de\") (accents nearly))", 1);
	CHECK(lq_judge_typed(&q, "m\xC3\xBC" "de").verdict == LQ_RIGHT, "umlaut right");
	CHECK(lq_judge_typed(&q, "mude").verdict == LQ_NEARLY, "umlaut missing: nearly");
	CHECK(lq_judge_typed(&q, "M\xC3\x9C" "DE").verdict == LQ_RIGHT, "umlaut, case folded");
	load("(text (ask \"x\") (accept \"m\xC3\xBC" "de\"))", 1);
	CHECK(lq_judge_typed(&q, "mude").verdict == LQ_WRONG, "strict accents: wrong");
	load("(text (ask \"x\") (accept \"Paris\") (case))", 1);
	CHECK(lq_judge_typed(&q, "paris").verdict == LQ_WRONG, "case matters");

	load("(order (ask \"x\") (items \"one\" \"two\" \"three\"))", 5);
	{
		int sorted = 1;
		for (int i = 0; i < 3; i++) if (q.order[i] != i) sorted = 0;
		CHECK(!sorted, "order shuffled");
		int right[3] = { 0, 1, 2 }, wrong[3] = { 1, 0, 2 };
		CHECK(lq_judge_order(&q, right, 3).verdict == LQ_RIGHT, "order right");
		CHECK(lq_judge_order(&q, wrong, 3).verdict == LQ_WRONG, "order wrong");
	}
	{
		char t[64];
		load("(number (ask \"x\") (answer 1.42) (unit \"s\"))", 1);
		lq_answer_text(&q, t, sizeof t);
		CHECK(!strcmp(t, "1.42 s"), "answer text %s", t);
	}
	printf("%s: %d failed\n", fails ? "test_quiz FAILED" : "test_quiz ok", fails);
	return fails != 0;
}
