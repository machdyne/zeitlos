/*
 * kidgames -- the shared round. See kground.h.
 */

#include <string.h>

#include "kground.h"
#include "kgui.h"
#include "kgpad.h"
#include "kgfont.h"
#include "kgsave.h"
#include "levelup.h"
#include "kgsound.h"

/* The result screen's own layout. The keyboard is hidden while it is
 * up, so the whole playfield below the header is available. */
#define FACE_SIZE   64
#define FACE_Y      (KG_PLAY_Y + 14)
#define HEADLINE_Y  (FACE_Y + FACE_SIZE + 10)
#define ANSWER_Y    (HEADLINE_Y + 16)

void kg_upper(char *dst, int dstlen, const char *src)
{
	int i = 0;

	if (dstlen <= 0) return;

	while (src && src[i] && i < dstlen - 1) {
		char c = src[i];
		dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
		i++;
	}

	dst[i] = '\0';
}

void kg_round_begin(kg_round_t *r, const char *id, const char *title,
	int max_level)
{
	kg_save_t rec = kg_save_load(id, 0);

	r->id = id;
	r->title = title;
	r->max_level = max_level;
	r->score = rec.best_score;
	r->best = rec.best_score;
	r->level = rec.level;
	r->streak = 0;

	/* Clamped at BOTH ends. kgsave.c already floors a level of 0, but
	 * a file carrying level 5 for a game whose ladder is three long --
	 * which happens if a game's max_level is ever reduced -- would
	 * index its level table off the end. Each game knows its own
	 * ceiling and this is where it is applied. */
	if (r->level < 1) r->level = 1;
	if (r->level > max_level) r->level = max_level;
}

static void store(kg_round_t *r)
{
	kg_save_t rec;

	if (r->score > r->best) r->best = r->score;

	rec.best_score = r->best;
	rec.level = r->level;

	/* Return value ignored on purpose: a machine with no sdcard has
	 * nowhere to write and the game carries on with an in-memory
	 * score. See kgsave.h. */
	kg_save_store(r->id, rec);
}

void kg_round_end(kg_round_t *r)
{
	store(r);
}

void kg_round_header(const kg_round_t *r, const char *prompt)
{
	kg_header(r->title, r->level, r->score);

	if (prompt) kg_center_text(KG_PLAY_Y + 3, prompt, 1);
}

void kg_face(int x, int y, int size, bool happy)
{
	int eye_w = size / 7;
	int eye_h = size / 5;
	int eye_y = y + size / 3;
	int mouth_w = size / 2 + size / 8;
	int mouth_x = x + (size - mouth_w) / 2;
	int lift = size / 5;
	int thick = size / 12;

	if (thick < 2) thick = 2;

	/* The head: a square with its corners knocked off. Four corner
	 * fills of background over a frame is two operations cheaper than
	 * any curve and, at this size, indistinguishable from one --
	 * a 64px circle in one bit is a square with chamfers whichever way
	 * it is arrived at. */
	kg_frame(x, y, size, size, 1);
	kg_frame(x + 1, y + 1, size - 2, size - 2, 1);

	{
		int c = size / 6;
		kg_fill(x, y, c, c, 0);
		kg_fill(x + size - c, y, c, c, 0);
		kg_fill(x, y + size - c, c, c, 0);
		kg_fill(x + size - c, y + size - c, c, c, 0);
		/* and the diagonals back in */
		int i;
		for (i = 0; i < c; i++) {
			kg_fill(x + c - i - 1, y + i, 2, 1, 1);
			kg_fill(x + size - c + i - 1, y + i, 2, 1, 1);
			kg_fill(x + c - i - 1, y + size - i - 1, 2, 1, 1);
			kg_fill(x + size - c + i - 1, y + size - i - 1, 2, 1, 1);
		}
	}

	kg_fill(x + size / 4 - eye_w / 2, eye_y, eye_w, eye_h, 1);
	kg_fill(x + 3 * size / 4 - eye_w / 2, eye_y, eye_w, eye_h, 1);

	/*
	 * The mouth: a real curve, as one short vertical fill per column.
	 *
	 * The first version was a bar with its ends turned up or down --
	 * three fills, and on paper unmistakable. It was not. Rendered at
	 * 64 pixels both variants came out as a BRACKET, a horizontal rule
	 * with two stubs, and happy and sad differed only by which side of
	 * the rule the stubs were on. Neither read as a mouth at all, and
	 * a viewer has to tell them apart at a glance without reading the
	 * word underneath -- this face is the whole replacement for the
	 * green and red the original used for right and wrong.
	 *
	 * A parabola fixes it and costs mouth_w fills, which is about
	 * thirty. dy is the drop from the curve's extreme, integer-only:
	 *
	 *     dy = t^2 * lift / half^2
	 *
	 * A smile hangs DOWN in the middle (y largest at t = 0), a frown
	 * peaks up in the middle. Same code, one sign.
	 */
	{
		int half = mouth_w / 2;
		int my = y + 2 * size / 3;
		int col;

		for (col = 0; col < mouth_w; col++) {

			int t = col - half;
			int dy = half ? (t * t * lift) / (half * half) : 0;
			int cy = happy ? my + lift - dy : my + dy;

			kg_fill(mouth_x + col, cy, 1, thick, 1);

		}
	}
}

/* Result screen state, so the repaint callback can redraw it. */
static const kg_round_t *res_round;
static bool res_correct;
static const char *res_headline;
static const char *res_answer;

static void result_draw(void *user)
{
	(void)user;

	kg_clear();
	kg_header(res_round->title, res_round->level, res_round->score);

	kg_face((KG_W - FACE_SIZE) / 2, FACE_Y, FACE_SIZE, res_correct);

	kg_center_text(HEADLINE_Y, res_headline, 1);

	if (!res_correct && res_answer && res_answer[0]) {

		/* The answer, in the same big font the question was asked in
		 * but dithered -- so it reads as "this is what it was" rather
		 * than as a new question. Shape alone would not separate
		 * those two; here the position (below a sad face and a
		 * headline) does the work the dither cannot be trusted to do
		 * on a bitstream without it. */
		int scale = kg_big_best_scale(res_answer, KG_W - 20,
			KG_H - ANSWER_Y - 6);

		kg_big_puts_centered(ANSWER_Y, res_answer, scale, KG_BIG_HINT);

	}
}

void kg_round_show(kg_round_t *r, bool correct, const char *headline,
	const char *answer_shown)
{
	kg_repaint_fn saved_fn;
	void *saved_user;

	saved_fn = kg_get_repaint(&saved_user);

	res_round = r;
	res_correct = correct;
	res_headline = headline;
	res_answer = answer_shown;

	kg_pad_hide();
	kg_set_repaint(result_draw, 0);
	result_draw(0);

	/* After the screen is drawn, not before: the sound and the face
	 * should arrive together, and result_draw() is several hundred
	 * blitter operations. */
	kg_sound_play(correct ? KG_SND_RIGHT : KG_SND_WRONG);

	/*
	 * Longer on a miss.
	 *
	 * The original's numbers and its reasoning: the correct answer is
	 * only on screen during this pause, and a kid who got it wrong
	 * needs longer to read it than a kid who got it right needs to
	 * enjoy being told so. Keys are discarded throughout, which
	 * matters most here -- the ENTER that submitted the answer must
	 * not carry into the next question.
	 */
	kg_pause_ms(correct ? 1600 : 2400);

	kg_set_repaint(saved_fn, saved_user);
}

void kg_round_finish(kg_round_t *r, bool correct, const char *praise,
	const char *miss, const char *answer_shown)
{
	bool leveled_up = false;

	if (correct) {
		/* Score by level, so a harder question is worth more -- the
		 * original's rule, and the thing that makes levelling up feel
		 * like a reward rather than just a change of difficulty. */
		r->score += r->level;
		r->streak++;
		if (r->streak >= KG_STREAK_TO_LEVEL_UP && r->level < r->max_level) {
			r->level++;
			r->streak = 0;
			leveled_up = true;
		}
	} else {
		r->streak = 0;
	}

	/* Saved BEFORE the result screen, which blocks for over two
	 * seconds. A kid who closes the lid during the "nice try" should
	 * still have the answer counted. */
	store(r);

	kg_round_show(r, correct, correct ? praise : miss, answer_shown);

	if (leveled_up) levelup_celebrate(r->level);
}
