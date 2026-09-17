/*
 * Render kidgames' screens to an image, on the build machine.
 *
 *   make render WHAT=menu|pad|digits|field|msg|font
 *
 * LOOK AT THE OUTPUT before changing any layout. sw/apps/logic's panel
 * shipped wrong three times against a passing arithmetic test, and the
 * third bug -- absolute screen coordinates used where content-relative
 * ones were needed -- was found in one look at a render. This app's
 * fixed 320x240 playfield exists so that what comes out of here is
 * exactly what a board puts on the glass.
 *
 * kgwidget.c is #included rather than linked, so this can call its
 * static draw functions directly and see a menu or a message box
 * WITHOUT running its input loop -- which would block on a message
 * queue that does not exist here.
 */

#include "kg_shim.h"

#include "../kgui.h"
#include "../kgpad.h"
#include "../kgfont.h"
#include "../game.h"

/* The widgets, for their statics. kgwidget.c is deliberately absent
 * from the Makefile's PANELSRC for this reason. */
#include "../kgwidget.c"

/* The games, for their static draw functions -- same reason as
 * kgwidget.c above: a scene has to be drawn WITHOUT running the input
 * loop around it. These are absent from the Makefile's PANELSRC and
 * present in TESTSRC, so only this target sees them twice. */
#include "../kground.c"
#include "../games_word.c"
#include "../games_number.c"
#include "../games_guess.c"
#include "../games_memory.c"
#include "../games_animal.c"

static const char *const MENU_ITEMS[] = {
	"Spelling Practice", "Number Guessing", "Word Unscramble",
	"Missing Letter", "Name That Animal", "Letter Hunt",
	"Counting", "Simple Math", "Memory Match", "Word Guess",
};
#define MENU_N ((int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0])))

static void scene_menu(void)
{
	ms.title = "KIDGAMES";
	ms.items = MENU_ITEMS;
	ms.count = MENU_N;
	ms.sel = 3;
	ms.top = 0;
	ms.rows = (KG_H - MENU_FOOT_H - MENU_TOP) / MENU_ROW_H;

	menu_draw(0);
}

/* A question screen: header, prompt, the word in big letters, and the
 * keyboard. This is the shape six of the ten games have, so it is the
 * one worth looking at hardest. */
static void scene_question(kg_charset_t cs, const char *word,
	const char *typed)
{
	int scale;

	kg_clear();
	kg_header("SPELLING", 3, 120);

	kg_center_text(KG_PLAY_Y + 4, "SPELL THIS WORD", 1);

	scale = kg_big_best_scale(word, KG_W - 16, 60);
	kg_big_puts_centered(KG_PLAY_Y + 20, word, scale, KG_BIG_SOLID);

	kg_pad_show(cs);
	kg_pad_draw();

	/* The field, positioned where kg_input_line() puts it. */
	fs.x = 40;
	fs.y = kg_pad_top() - FIELD_H - 8;
	fs.w = KG_W - 80;
	fs.buf = (char *)typed;
	fs.maxlen = 12;
	field_draw();
}

static void scene_msg(void)
{
	static const char *const lines[] = {
		"THAT IS RIGHT!",
		"",
		"5 in a row -- level up!"
	};

	scene_question(KG_CS_ALPHA, "APPLE", "APPLE");

	msgs.title = "WELL DONE";
	msgs.lines = lines;
	msgs.nlines = 3;
	msgs.w = MSG_W;
	msgs.h = 20 + 3 * 11 + 8 + MSG_BTN_H + 6;
	msgs.x = (KG_W - msgs.w) / 2;
	msgs.y = (KG_PLAY_Y + KG_H - msgs.h) / 2;
	msgs.btn_x = msgs.x + (msgs.w - MSG_BTN_W) / 2;
	msgs.btn_y = msgs.y + msgs.h - MSG_BTN_H - 6;

	msg_draw(0);
}

/* Every glyph at a legible scale, plus the four styles side by side.
 * The styles matter as much as the letters: SOLID/HINT differ only in
 * fill, and BLANK/GHOST have to stay apart by SHAPE so they survive a
 * bitstream with no dither (kg.h). */
static void scene_font(void)
{
	kg_clear();
	kg_header("FONT", 0, 0);

	kg_big_puts(4, 16, "ABCDEFGHI", 5, KG_BIG_SOLID);
	kg_big_puts(4, 16 + 40, "JKLMNOPQR", 5, KG_BIG_SOLID);
	kg_big_puts(4, 16 + 80, "STUVWXYZ", 5, KG_BIG_SOLID);
	kg_big_puts(4, 16 + 120, "0123456789", 4, KG_BIG_SOLID);

	kg_text(4, 184, "SOLID    HINT     BLANK    GHOST", 1);
	kg_big_putc(10, 196, 'W', 5, KG_BIG_SOLID);
	kg_big_putc(55, 196, 'W', 5, KG_BIG_HINT);
	kg_big_putc(100, 196, 'W', 5, KG_BIG_BLANK);
	kg_big_putc(145, 196, 'W', 5, KG_BIG_GHOST);

	/* Tries-left, the one shared "how many goes are left" visual, as
	 * used by Number Guessing and Word Guess. */
	kg_tries_left(224, 3, 6);
}

/* -- the game screens -------------------------------------------------
 *
 * Each sets the statics its draw function reads and calls it. No input
 * loop, no message queue, no save file. */

static kg_round_t demo = { "demo", "SPELLING", 5, 120, 3, 0, 120 };

static void scene_result(void)
{
	res_round = &demo;
	res_correct = false;
	res_headline = "NICE TRY!";
	res_answer = "ELEPHANT";
	result_draw(0);
}

static void scene_missing(void)
{
	demo.title = "MISSING LETTER";
	q_round = &demo;
	q_prompt = "TYPE THE MISSING LETTER";
	strcpy(shown, "RABBIT");
	blank_idx = 2;
	question_draw(0);
	kg_pad_show(KG_CS_ALPHA);
	kg_pad_draw();
}

static void scene_counting(void)
{
	demo.title = "COUNTING";
	count_target = 13;
	q_round = &demo;
	q_prompt = "HOW MANY?";
	counting_draw(0);
	kg_pad_show(KG_CS_DIGITS);
	kg_pad_draw();
}

static void scene_math(void)
{
	demo.title = "SIMPLE MATH";
	q_round = &demo;
	q_prompt = "WHAT IS THE ANSWER?";
	strcpy(math_a, "7");
	strcpy(math_b, "8");
	strcpy(math_op, "X");
	math_draw(0);
	kg_pad_show(KG_CS_DIGITS);
	kg_pad_draw();
}

static void scene_numguess(void)
{
	demo.title = "NUMBER GUESSING";
	q_round = &demo;
	ng_min = 1; ng_max = 100;
	ng_tries_left = 6; ng_tries_total = 10;
	ng_hint = 1;
	numguess_draw(0);
}

static void scene_wordguess(void)
{
	int i;
	demo.title = "WORD GUESS";
	q_round = &demo;
	strcpy(wg_word, "MONKEY");
	for (i = 0; i < WORD_MAX; i++) wg_revealed[i] = false;
	wg_revealed[1] = wg_revealed[4] = true;
	for (i = 0; i < 26; i++) wg_guessed[i] = false;
	wg_guessed['O' - 'A'] = wg_guessed['E' - 'A'] = true;
	wg_guessed['S' - 'A'] = wg_guessed['T' - 'A'] = true;
	wg_wrong = 2;
	wordguess_draw(0);
	kg_pad_show(KG_CS_ALPHA);
	kg_pad_draw();
}

static void scene_letterhunt(void)
{
	demo.title = "LETTER HUNT";
	q_round = &demo;
	strcpy(lh_word, "GARDEN");
	lh_target[0] = 'D'; lh_target[1] = 0;
	lh_cursor = 3;
	kg_pad_hide();
	letterhunt_draw(0);
}

/* A grid mid-round: some pairs matched, one card flipped, the cursor
 * on another. Every card state on one screen, which is the point --
 * matched, face-up, face-down and cursor all have to be told apart at
 * a glance. */
static void scene_memory(int rows, int cols)
{
	int i;

	demo.title = "MEMORY MATCH";
	q_round = &demo;
	mm_rows = rows;
	mm_cols = cols;

	for (i = 0; i < rows * cols; i++) {
		mm_sym[i] = (char)('A' + i / 2);
		mm_matched[i] = false;
	}
	mm_matched[0] = mm_matched[1] = true;
	if (rows * cols > 7) mm_matched[6] = mm_matched[7] = true;

	mm_reveal_a = 3;
	mm_reveal_b = -1;
	mm_cursor = rows * cols - 2;

	kg_pad_hide();
	memory_draw(0);
}

static void scene_animal(void)
{
	const animal_t *a = animals_at(3, 0);	/* crocodile -- the longest name */

	demo.title = "NAME THAT ANIMAL";
	q_round = &demo;
	q_animal = a;
	q_have_sprite = true;
	double_art(a->art);
	animal_draw(0);
	kg_pad_show(KG_CS_ALPHA);
	kg_pad_draw();
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "/tmp/kidgames.pbm";
	const char *what = argc > 2 ? argv[2] : "menu";

	if (!z_render_open((z_win_t *)kg_window(), KG_WIN_W, KG_WIN_H)) {
		puts("render: skipped (cannot map the VRAM address)");
		return 77;
	}

	if (!strcmp(what, "menu")) scene_menu();
	else if (!strcmp(what, "pad")) scene_question(KG_CS_ALPHA, "ELEPHANT", "ELE");
	else if (!strcmp(what, "digits")) scene_question(KG_CS_DIGITS, "7", "4");
	else if (!strcmp(what, "field")) scene_question(KG_CS_ALPHA, "APPLE", "APP");
	else if (!strcmp(what, "msg")) scene_msg();
	else if (!strcmp(what, "font")) scene_font();
	else if (!strcmp(what, "result")) scene_result();
	else if (!strcmp(what, "counting")) scene_counting();
	else if (!strcmp(what, "math")) scene_math();
	else if (!strcmp(what, "numguess")) scene_numguess();
	else if (!strcmp(what, "wordguess")) scene_wordguess();
	else if (!strcmp(what, "letterhunt")) scene_letterhunt();
	else if (!strcmp(what, "missing")) scene_missing();
	else if (!strcmp(what, "memory")) scene_memory(4, 4);
	else if (!strcmp(what, "memory1")) scene_memory(2, 3);
	else if (!strcmp(what, "animal")) scene_animal();
	else { puts("render: unknown scene"); return 2; }

	/* 2x, which is also what game mode does on the way to the glass --
	 * so this image is, pixel for pixel, the full-screen picture. */
	z_render_write(out, (z_win_t *)kg_window(), 2);

	return 0;
}
