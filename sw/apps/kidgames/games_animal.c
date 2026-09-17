/*
 * kidgames -- Name That Animal.
 *
 * A picture, and the player types what it is. The only game in the app
 * that draws art rather than letters, which is the whole reason it is
 * last: it needed the pipeline in gen_art.py and the sprite path in
 * kgui.c before it could exist at all.
 *
 * Otherwise it is the plainest of the ten -- one picture, one field,
 * one answer -- so it uses the shared round in kground.h unchanged.
 */

#include <string.h>

#include "game.h"
#include "kground.h"
#include "kgui.h"
#include "kgfont.h"
#include "kgart.h"
#include "animals.h"

static const animal_t *q_animal;
static char q_name[16];
static kg_round_t *q_round;
static bool q_have_sprite;

/* Doubled, so a 64x48 piece lands as 128x96 on the playfield.
 *
 * Not because the art is too small at 1:1 -- it is, but the fix for
 * that would be to draw it larger. It is doubled because the pieces
 * have to be authored at a size somebody can actually draw in one bit,
 * and 64x48 is about the point where a shape stays a shape. Doubling
 * at draw time keeps the source honest and the screen legible.
 *
 * Done as four blits of the same source rather than a scaling blit,
 * which the hardware does not have -- see draw_doubled(). */
#define ART_SCALE 2
#define ART_W (KG_ART_W * ART_SCALE)
#define ART_H (KG_ART_H * ART_SCALE)

/*
 * Pixel-double a 1bpp bitmap into a static buffer, once per question.
 *
 * The blitter copies; it does not scale. The alternative to this is
 * KG_ART_W * KG_ART_H fills -- three thousand blitter operations for
 * one picture, which is not a sensible way to draw a picture.
 *
 * Doubling in software into a buffer the blitter can then move in ONE
 * operation costs 3072 bit tests once per question, on a screen that
 * then sits still while a kid types. Static, not stack, for the reason
 * kgsave.c gives.
 */
#define DBL_STRIDE (ART_W / 8)
static uint8_t doubled[DBL_STRIDE * ART_H + 4];

static void double_art(const uint8_t *src)
{
	int y, x;

	for (y = 0; y < DBL_STRIDE * ART_H + 4; y++) doubled[y] = 0;

	for (y = 0; y < KG_ART_H; y++)
		for (x = 0; x < KG_ART_W; x++) {

			int dx, dy;

			/* Same bit order as the source: pixel x at bit (x & 7) of
			 * byte (x >> 3), least significant bit leftmost. Getting
			 * this backwards mirrors every piece horizontally, which
			 * on a symmetrical placeholder box looks like nothing at
			 * all and would have been found only when real art
			 * arrived. */
			if (!(src[y * KG_ART_STRIDE + (x >> 3)] & (1 << (x & 7))))
				continue;

			for (dy = 0; dy < ART_SCALE; dy++)
				for (dx = 0; dx < ART_SCALE; dx++) {
					int px = x * ART_SCALE + dx;
					int py = y * ART_SCALE + dy;
					doubled[py * DBL_STRIDE + (px >> 3)] |= 1 << (px & 7);
				}

		}
}

static void animal_draw(void *user)
{
	int x = (KG_W - ART_W) / 2;
	int y = KG_PLAY_Y + 14;

	(void)user;

	kg_clear();
	kg_round_header(q_round, "WHAT ANIMAL IS THIS?");

	if (q_have_sprite) {
		kg_sprite(x, y, doubled, DBL_STRIDE, ART_W, ART_H);
		return;
	}

	/*
	 * No memory-source blit on this bitstream.
	 *
	 * Rather than a blank rectangle, say what happened and keep the
	 * game playable: the name is the answer, so it cannot be shown --
	 * but a framed box tells the player the picture is missing rather
	 * than that they have missed it. The round still scores, and the
	 * result screen still reveals the name.
	 *
	 * This is not a hypothetical branch. z_fb_hw_blit_mem() is a
	 * blitter mode that some bitstreams predate, and zgfx.h is
	 * explicit that a binary built here may be run against one of
	 * them -- which is why kg_sprite_available() is probed once at
	 * startup and not per draw.
	 */
	kg_frame(x, y, ART_W, ART_H, 1);
	kg_center_text(y + ART_H / 2 - 12, "NO PICTURE", 1);
	kg_center_text(y + ART_H / 2 - 2, "ON THIS BITSTREAM", 1);
	kg_center_text(y + ART_H / 2 + 12, "GUESS ANYWAY!", 1);
}

void game_nameanimal_run(void)
{
	kg_round_t r;
	char answer[16];
	const animal_t *last = 0;
	bool can_sprite = kg_sprite_available();

	kg_round_begin(&r, "nameanimal", "NAME THAT ANIMAL", ANIMALS_MAX_LEVEL);
	q_round = &r;

	for (;;) {

		const animal_t *a = animals_pick_excluding(r.level, last);

		last = a;
		q_animal = a;
		q_have_sprite = can_sprite;

		kg_upper(q_name, sizeof(q_name), a->name);

		if (can_sprite) double_art(a->art);

		kg_set_repaint(animal_draw, 0);
		animal_draw(0);

		/* Room for the longest name plus slack, so a kid who types an
		 * extra letter can see it and backspace. "giraffe" is seven. */
		if (!kg_input_line(30, -1, answer, 12, KG_CS_ALPHA)) break;

		kg_round_finish(&r, strcmp(answer, q_name) == 0,
			"THAT IS RIGHT!", "NICE TRY!", q_name);

	}

	kg_round_end(&r);
}
