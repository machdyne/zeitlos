/*
 * kidgames -- Counting and Simple Math.
 *
 * Together for the same reason the three word games are: one screen
 * shape (a question drawn big, a number typed underneath), one scoring
 * rule, one difficulty table per game. The questions differ; nothing
 * else does.
 */

#include <string.h>

#include "game.h"
#include "kground.h"
#include "kgui.h"
#include "kgfont.h"
#include "kgrand.h"

#define NUM_MAX_LEVEL 5

static kg_round_t *q_round;
static const char *q_prompt;

/* -- Counting --------------------------------------------------------- */

/* How high the count goes, per level. The original's table. */
static const int COUNT_MAX[NUM_MAX_LEVEL] = { 5, 8, 12, 16, 20 };

#define STAR_COLS   5
#define STAR_SIZE   22
#define STAR_STEP_X 56
#define STAR_STEP_Y 30

static int count_target;

/*
 * One countable object: a diamond.
 *
 * Not the five-pointed star the name suggests and not the original's
 * '*' character. A five-pointed star at 22 pixels in one bit is a
 * blob with texture, and the entire task here is to look at a group of
 * shapes and know how many there are -- which needs each one to have a
 * hard edge and a gap around it, and needs nothing else at all. A
 * diamond is eleven fills and unmistakable.
 */
static void star(int x, int y, int size)
{
	int i;
	int half = size / 2;

	for (i = 0; i <= half; i++) {
		int w = 2 * i + 1;
		kg_fill(x + half - i, y + i, w, 1, 1);
		kg_fill(x + half - i, y + size - 1 - i, w, 1, 1);
	}
}

static void counting_draw(void *user)
{
	int i;
	int rows = (count_target + STAR_COLS - 1) / STAR_COLS;
	int grid_h = rows * STAR_STEP_Y;
	int y0 = KG_PLAY_Y + 18 + ((KG_PLAY_H - 22 - grid_h) / 2);

	(void)user;

	kg_clear();
	kg_round_header(q_round, q_prompt);

	if (y0 < KG_PLAY_Y + 16) y0 = KG_PLAY_Y + 16;

	for (i = 0; i < count_target; i++) {

		int row = i / STAR_COLS;
		int col = i % STAR_COLS;

		/* Each row centred on its OWN width, not on a full row's.
		 * A last row of two stars left-aligned under a row of five
		 * reads as a group of seven with a gap in it, and a kid
		 * counting by shape rather than by number gets it wrong. */
		int in_row = count_target - row * STAR_COLS;
		int n = in_row < STAR_COLS ? in_row : STAR_COLS;
		int row_w = (n - 1) * STAR_STEP_X + STAR_SIZE;
		int x0 = (KG_W - row_w) / 2;

		star(x0 + col * STAR_STEP_X, y0 + row * STAR_STEP_Y, STAR_SIZE);

	}
}

void game_counting_run(void)
{
	kg_round_t r;
	char answer[6];
	char shown[8];
	int last = -1;

	kg_round_begin(&r, "counting", "COUNTING", NUM_MAX_LEVEL);

	q_round = &r;
	q_prompt = "HOW MANY?";

	for (;;) {

		int max = COUNT_MAX[r.level - 1];
		int tries = 0;

		/* Bounded, not a do/while on inequality: at level 1 the range
		 * is five, so a run of repeats is entirely possible and a
		 * loop that insisted would eventually be the game hanging
		 * rather than the game repeating. */
		do {
			count_target = 1 + (int)kg_rand((uint32_t)max);
		} while (count_target == last && max > 1 && ++tries < 8);

		last = count_target;

		kg_set_repaint(counting_draw, 0);
		counting_draw(0);

		if (!kg_input_line(60, -1, answer, 2, KG_CS_DIGITS)) break;

		kg_utoa(shown, sizeof(shown), (unsigned long)count_target);

		kg_round_finish(&r, strcmp(answer, shown) == 0,
			"GREAT COUNTING!", "NICE TRY!", shown);

	}

	kg_round_end(&r);
}

/* -- Simple Math ------------------------------------------------------- */

typedef enum { OP_ADD, OP_SUB, OP_MUL } op_t;

typedef struct {
	op_t op;
	int lo, hi;
} math_level_t;

/* The original's ladder: addition, then subtraction that never goes
 * negative, then tables. */
static const math_level_t MATH_LEVELS[NUM_MAX_LEVEL] = {
	{ OP_ADD, 1, 5  },	/* sums to 10 */
	{ OP_ADD, 1, 10 },	/* sums to 20 */
	{ OP_SUB, 5, 20 },	/* never negative */
	{ OP_MUL, 1, 5  },	/* tables to 5 */
	{ OP_MUL, 1, 10 },	/* tables to 10 */
};

static char math_a[6], math_b[6];
static char math_op[2];
static int math_answer;

static void math_draw(void *user)
{
	/*
	 * Start at the tallest that fits the play area and come DOWN, the
	 * way kg_big_best_scale() does, rather than starting at a number
	 * somebody picked.
	 *
	 * It was a flat 7, which is 49 pixels of a 154-pixel play area --
	 * a one-digit sum floating in a screen two thirds empty. The
	 * height bound is what actually binds here: two three-digit
	 * operands are still only 224 pixels wide at scale 10, so width
	 * almost never decides.
	 */
	int scale = 10;
	int wa, wb, wop, total, x, y;

	(void)user;

	kg_clear();
	kg_round_header(q_round, q_prompt);

	/* Both operands at the SAME scale, chosen for the wider of the
	 * two. Sizing each to itself would draw "3 x 10" with a small 3
	 * and a large 10, which reads as the two numbers meaning
	 * different kinds of thing. */
	while (scale > KG_BIG_SCALE_MIN) {
		wa = kg_big_w(math_a, scale);
		wb = kg_big_w(math_b, scale);
		if (wa + wb + 6 * scale <= KG_W - 16 &&
			kg_big_h(scale) <= KG_PAD_Y - (KG_PLAY_Y + 22) - 40) break;
		scale--;
	}

	wa = kg_big_w(math_a, scale);
	wb = kg_big_w(math_b, scale);
	wop = 4 * scale;

	total = wa + wop + wb;
	x = (KG_W - total) / 2;
	y = KG_PLAY_Y + 22;

	kg_big_puts(x, y, math_a, scale, KG_BIG_SOLID);
	kg_big_puts(x + wa + wop, y, math_b, scale, KG_BIG_SOLID);

	/* The operator, drawn rather than set in the big font -- which has
	 * no '+', '-' or 'x' and would render all three as blank. Two
	 * fills each, vertically centred on the numbers. */
	{
		int ox = x + wa + wop / 2;
		int oy = y + kg_big_h(scale) / 2;
		int len = scale * 2;
		int th = scale / 2 < 2 ? 2 : scale / 2;

		kg_fill(ox - len / 2, oy - th / 2, len, th, 1);

		if (math_op[0] == '+')
			kg_fill(ox - th / 2, oy - len / 2, th, len, 1);

		if (math_op[0] == 'X') {
			/* A diagonal cross, as two stepped runs. 'x' in the 5x8
			 * font next to a 49-pixel numeral is invisible, and the
			 * multiplication sign a grown-up would reach for is not
			 * in any font here. */
			int i;
			kg_fill(ox - len / 2, oy - th / 2, len, th, 0);
			for (i = 0; i < len; i++) {
				kg_fill(ox - len / 2 + i, oy - len / 2 + i, th, th, 1);
				kg_fill(ox - len / 2 + i, oy + len / 2 - i - th, th, th, 1);
			}
		}
	}
}

void game_simple_math_run(void)
{
	kg_round_t r;
	char answer[8];
	char shown[8];

	kg_round_begin(&r, "simplemath", "SIMPLE MATH", NUM_MAX_LEVEL);

	q_round = &r;
	q_prompt = "WHAT IS THE ANSWER?";

	for (;;) {

		const math_level_t *lv = &MATH_LEVELS[r.level - 1];
		uint32_t span = (uint32_t)(lv->hi - lv->lo + 1);
		int a = lv->lo + (int)kg_rand(span);
		int b;

		switch (lv->op) {

		case OP_SUB:
			/* b drawn from [0, a] rather than independently, so the
			 * answer is never negative -- a five-year-old has not met
			 * negative numbers and the field takes no minus sign. */
			b = (int)kg_rand((uint32_t)(a + 1));
			math_answer = a - b;
			math_op[0] = '-';
			break;

		case OP_MUL:
			b = lv->lo + (int)kg_rand(span);
			math_answer = a * b;
			math_op[0] = 'X';
			break;

		default:
			b = lv->lo + (int)kg_rand(span);
			math_answer = a + b;
			math_op[0] = '+';
			break;

		}

		math_op[1] = '\0';
		kg_utoa(math_a, sizeof(math_a), (unsigned long)a);
		kg_utoa(math_b, sizeof(math_b), (unsigned long)b);

		kg_set_repaint(math_draw, 0);
		math_draw(0);

		if (!kg_input_line(60, -1, answer, 3, KG_CS_DIGITS)) break;

		kg_utoa(shown, sizeof(shown), (unsigned long)math_answer);

		kg_round_finish(&r, strcmp(answer, shown) == 0,
			"THAT IS RIGHT!", "NICE TRY!", shown);

	}

	kg_round_end(&r);
}
