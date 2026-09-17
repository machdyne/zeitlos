/*
 * kidgames -- Memory Match (concentration).
 *
 * A grid of face-down cards, two of each letter. Flip two that match
 * and they stay up.
 *
 * Its own file because it is the one game with genuine 2D navigation,
 * and that changes several things the other nine share:
 *
 *   - The arrow keys move LITERALLY here -- up and down change row,
 *     left and right change column -- rather than going through
 *     kg_key_is_next()/kg_key_is_prev() the way the menu and Letter
 *     Hunt do. Clamped at the edges, NOT wrapped: wrapping in one
 *     dimension is a convenience and wrapping in two is a way to lose
 *     the cursor.
 *
 *   - Space still means "move", as it does everywhere else in this
 *     app -- it advances in reading order and wraps at the end. Enter
 *     alone flips. The original makes this point and it is worth
 *     keeping: a key that means "move" in nine games and "flip" in the
 *     tenth is the kind of inconsistency a five-year-old absorbs as
 *     "sometimes it does the wrong thing".
 *
 *   - A round is the whole grid, not one question, so levelling is by
 *     EFFICIENCY (clearing with few wrong guesses) rather than by a
 *     streak of right answers, and the threshold is two rather than
 *     five. Five grids between rewards is a long time.
 *
 * It is also the most mouse-native of the ten: a card is a thing you
 * point at. Clicking one flips it, with no cursor movement involved.
 */

#include <string.h>

#include "game.h"
#include "kground.h"
#include "kgui.h"
#include "kgpad.h"
#include "kgfont.h"
#include "levelup.h"
#include "kgrand.h"

#define MM_MAX_LEVEL 5
#define MM_MAX_PAIRS 8
#define MM_MAX_CARDS (MM_MAX_PAIRS * 2)

/* Efficient rounds in a row. See the file comment. */
#define MM_STREAK 2

typedef struct { int rows, cols; } shape_t;

/* The original's ladder: 3 pairs up to a full 4x4. */
static const shape_t MM_LEVELS[MM_MAX_LEVEL] = {
	{ 2, 3 },	/* 3 pairs */
	{ 2, 4 },	/* 4 pairs */
	{ 2, 5 },	/* 5 pairs */
	{ 3, 4 },	/* 6 pairs */
	{ 4, 4 },	/* 8 pairs */
};

/*
 * Card geometry. Sized so the widest grid (5 columns) and the tallest
 * (4 rows) both fit the playfield with the keyboard hidden, which is
 * the only constraint -- every other shape is narrower or shorter and
 * gets centred.
 *
 *   5 * 62 - 6 = 304 wide, in 320
 *   4 * 46 - 6 = 178 tall, in the 205 below the prompt
 */
#define CARD_W    56
#define CARD_H    40
#define CARD_GAP  6
#define STRIDE_X  (CARD_W + CARD_GAP)
#define STRIDE_Y  (CARD_H + CARD_GAP)

#define GRID_TOP  (KG_PLAY_Y + 20)

/*
 * The card back: diagonal hatching.
 *
 * Diagonal specifically. A grid or a checker aligns with the card's own
 * edges and with the 8-pixel anchor the hardware imposes, so a field of
 * them reads as one continuous texture and the individual cards stop
 * being visible as objects -- which is fatal when the thing being asked
 * of the player is "remember which card was where". Diagonals cut
 * across the card edges and leave the frames the most obvious line on
 * the screen.
 */
static const uint8_t CARD_BACK[8] = {
	0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88
};

static char mm_sym[MM_MAX_CARDS];
static bool mm_matched[MM_MAX_CARDS];
static int mm_rows, mm_cols;
static int mm_cursor;		/* index, not row/col -- one thing to keep right */
static int mm_reveal_a, mm_reveal_b;
static kg_round_t *q_round;

static int grid_x0(void) { return (KG_W - (mm_cols * STRIDE_X - CARD_GAP)) / 2; }

static int grid_y0(void)
{
	int h = mm_rows * STRIDE_Y - CARD_GAP;
	int avail = KG_H - 12 - GRID_TOP;

	return GRID_TOP + (avail - h) / 2;
}

static void card_rect(int idx, int *x, int *y)
{
	*x = grid_x0() + (idx % mm_cols) * STRIDE_X;
	*y = grid_y0() + (idx / mm_cols) * STRIDE_Y;
}

static void draw_card(int idx)
{
	int x, y;
	bool face = mm_matched[idx] || idx == mm_reveal_a || idx == mm_reveal_b;

	card_rect(idx, &x, &y);

	if (face) {

		int scale = kg_big_best_scale("W", CARD_W - 14, CARD_H - 14);
		char s[2];

		kg_fill(x, y, CARD_W, CARD_H, 0);
		kg_frame(x, y, CARD_W, CARD_H, 1);

		s[0] = mm_sym[idx];
		s[1] = '\0';

		kg_big_puts(x + (CARD_W - kg_big_w(s, scale)) / 2,
			y + (CARD_H - kg_big_h(scale)) / 2 - 2, s, scale, KG_BIG_SOLID);

		/*
		 * A matched card gets a solid bar across its foot.
		 *
		 * It has to be distinguishable from a card that is merely
		 * flipped up this instant, and the two cannot differ by shade:
		 * both carry a letter, and kg.h's second rule forbids putting
		 * text on a dither. So the difference is a SHAPE added below
		 * the letter, where it cannot interfere with reading it --
		 * the same reasoning as Letter Hunt's cursor box.
		 */
		if (mm_matched[idx])
			kg_fill(x + 6, y + CARD_H - 6, CARD_W - 12, 3, 1);

	} else {
		kg_pattern(x, y, CARD_W, CARD_H, CARD_BACK);
		kg_frame(x, y, CARD_W, CARD_H, 1);
	}

	/* The cursor sits OUTSIDE the card, in the gap. CARD_GAP is 6, so
	 * two rings of 2 leave a clear pixel between the cursor and the
	 * neighbouring card's own frame. */
	if (idx == mm_cursor) {
		kg_frame(x - 2, y - 2, CARD_W + 4, CARD_H + 4, 1);
		kg_frame(x - 3, y - 3, CARD_W + 6, CARD_H + 6, 1);
	}
}

static void memory_draw(void *user)
{
	int i;

	(void)user;

	kg_clear();
	kg_round_header(q_round, "FIND THE PAIRS!");

	for (i = 0; i < mm_rows * mm_cols; i++) draw_card(i);

	/* grid_y0() reserves 12 pixels at the foot for this, which is why
	 * it centres in KG_H - 12 rather than in KG_H. The cursor ring
	 * sticks 3 pixels past the bottom card, so the reservation is what
	 * keeps the two apart. */
	kg_status("ARROWS MOVE   ENTER FLIPS");
}

/* The card at a click, or -1. Uses card_rect() rather than repeating
 * the arithmetic, so a layout change cannot move the cards without
 * moving the hit boxes with them. */
static int card_at(int px, int py)
{
	int i;

	for (i = 0; i < mm_rows * mm_cols; i++) {
		int x, y;
		card_rect(i, &x, &y);
		if (px >= x && px < x + CARD_W && py >= y && py < y + CARD_H)
			return i;
	}

	return -1;
}

static void deal(int pairs)
{
	int n = pairs * 2;
	int i;

	for (i = 0; i < pairs; i++) {
		mm_sym[i * 2] = mm_sym[i * 2 + 1] = (char)('A' + i);
		mm_matched[i * 2] = mm_matched[i * 2 + 1] = false;
	}

	/* Fisher-Yates. No retry-if-unchanged here, unlike Unscramble: an
	 * identity shuffle of a memory grid is not a free answer, it is
	 * just one particular arrangement, and every arrangement is
	 * equally unknown to the player. */
	for (i = n - 1; i > 0; i--) {
		int j = (int)kg_rand((uint32_t)(i + 1));
		char t = mm_sym[i];
		mm_sym[i] = mm_sym[j];
		mm_sym[j] = t;
	}
}

void game_memorymatch_run(void)
{
	kg_round_t r;

	kg_round_begin(&r, "memorymatch", "MEMORY MATCH", MM_MAX_LEVEL);
	q_round = &r;

	for (;;) {

		const shape_t *sh = &MM_LEVELS[r.level - 1];
		int n, pairs, found = 0, attempts = 0;
		int first = -1;
		bool escaped = false;

		mm_rows = sh->rows;
		mm_cols = sh->cols;
		n = mm_rows * mm_cols;
		pairs = n / 2;

		deal(pairs);

		mm_cursor = 0;
		mm_reveal_a = mm_reveal_b = -1;

		/* No keyboard: nothing is typed and the grid is the board. */
		kg_pad_hide();
		kg_set_repaint(memory_draw, 0);

		while (found < pairs) {

			kg_key_t k;
			int pick = -1;

			memory_draw(0);
			k = kg_getkey();

			if (k == KG_KEY_QUIT || k == KG_KEY_ESC) { escaped = true; break; }

			/* Literal 2D movement, clamped. */
			if (k == KG_KEY_UP) {
				if (mm_cursor >= mm_cols) mm_cursor -= mm_cols;
				continue;
			}
			if (k == KG_KEY_DOWN) {
				if (mm_cursor + mm_cols < n) mm_cursor += mm_cols;
				continue;
			}
			if (k == KG_KEY_LEFT) {
				if (mm_cursor % mm_cols) mm_cursor--;
				continue;
			}
			if (k == KG_KEY_RIGHT) {
				if ((mm_cursor + 1) % mm_cols) mm_cursor++;
				continue;
			}

			if (k == KG_KEY_SPACE) {
				/* Reading order, wrapping at the end -- so Space is
				 * "move" here exactly as it is everywhere else, and
				 * a player who never finds the arrow keys can still
				 * reach every card. */
				mm_cursor = (mm_cursor + 1) % n;
				continue;
			}

			if (k == KG_KEY_ENTER) pick = mm_cursor;

			if (k == KG_KEY_CLICK) {
				pick = card_at(kg_click_x(), kg_click_y());
				/* A click also moves the cursor, so the keyboard and
				 * the mouse never disagree about where "here" is --
				 * someone switching between them mid-round would
				 * otherwise find Enter flipping a card they stopped
				 * looking at several moves ago. */
				if (pick >= 0) mm_cursor = pick;
			}

			if (pick < 0) continue;
			if (mm_matched[pick] || pick == first) continue;

			if (first < 0) {
				first = pick;
				mm_reveal_a = pick;
				continue;
			}

			attempts++;
			mm_reveal_b = pick;

			if (mm_sym[pick] == mm_sym[first]) {

				mm_matched[pick] = mm_matched[first] = true;
				found++;
				first = -1;
				mm_reveal_a = mm_reveal_b = -1;

				/* A beat with the pair face up before the bars appear,
				 * so a match is something the player SEES happen
				 * rather than something that has already happened by
				 * the time they look. */
				memory_draw(0);
				kg_pause_ms(350);

			} else {

				/* Both shown, then hidden. The whole game is
				 * remembering what was where, so this pause is the
				 * mechanic, not politeness -- too short and there is
				 * nothing to remember. */
				memory_draw(0);
				kg_pause_ms(900);

				first = -1;
				mm_reveal_a = mm_reveal_b = -1;

			}

		}

		if (escaped) break;

		/*
		 * Paid per pair and scaled by level, then levelled on
		 * EFFICIENCY: clearing a grid with at most two wasted guesses
		 * counts. A perfect round is `pairs` attempts, so this allows
		 * a couple of genuine misses without demanding luck.
		 */
		r.score += pairs * r.level;

		if (attempts <= pairs + 2) {
			r.streak++;
			if (r.streak >= MM_STREAK && r.level < MM_MAX_LEVEL) {
				r.level++;
				r.streak = 0;
				kg_round_end(&r);
				kg_round_show(&r, true, "ALL FOUND!", 0);
				levelup_celebrate(r.level);
				continue;
			}
		} else {
			r.streak = 0;
		}

		kg_round_end(&r);
		kg_round_show(&r, true, "ALL FOUND!", 0);

	}

	kg_round_end(&r);
}
