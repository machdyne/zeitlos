/*
 * kidgames -- layout tests.
 *
 * Runs unattended and fails loudly. tests/render.c is the other half
 * of this and they are not redundant: a geometry assertion can only
 * check a relationship somebody thought to write down, which is
 * exactly how sw/apps/logic shipped three layout bugs against three
 * successive passing tests (sw/common/tests/zrender.h). Look at a
 * render before shipping; run this every build.
 *
 * The test that earns its keep is containment: NOTHING may be drawn
 * outside the 320x240 playfield. In a window that means painting over
 * another app; in game mode it means painting into the off-screen
 * pages, where the sprite art will live. Both are silent.
 */

#include "kg_shim.h"

#include "../kgui.h"
#include "../kgpad.h"
#include "../kgfont.h"
#include "../game.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
		failures++; \
	} \
} while (0)

/* -- the bands fit the screen they are laid out on ------------------- */

static void test_bands(void)
{
	/* Every band constant in kg.h, checked against every other one.
	 * These are hand-chosen numbers and the whole app is positioned
	 * against them, so an edit that makes two of them disagree should
	 * not have to wait for somebody to notice a gap on a screen. */
	CHECK(KG_RULE_Y < KG_PLAY_Y, "the header rule is inside the play area");
	CHECK(KG_PLAY_Y + KG_PLAY_H <= KG_PAD_Y, "play area overlaps the pad");
	CHECK(KG_PAD_Y + KG_PAD_H == KG_H, "the pad does not reach the bottom");
	CHECK(KG_PLAY_Y + KG_PLAY_H_FULL == KG_H,
		"the full play area does not reach the bottom");

	CHECK(KG_PLAY_MID > KG_PLAY_Y && KG_PLAY_MID < KG_PAD_Y,
		"the play centre is outside the play area");

	/* Four rows of keys at the pad's own stride must fit the band, or
	 * the bottom row is drawn off the screen -- which the clamp in
	 * kg_fill() turns into a row that silently is not there rather
	 * than into anything anyone would see and investigate. */
	CHECK(1 + KG_PAD_ROWS * KG_PAD_ROW_STEP <= KG_PAD_H + 1,
		"four key rows do not fit the pad band");
}

/*
 * The playfield is the arcade runtime's screen: kg.h's 320x240 and the
 * runtime's have no reason to stay in step except this test.
 */
static void test_window_size(void)
{
	CHECK(KG_W == AR_W, "the playfield's width is not the runtime's");
	CHECK(KG_H == AR_H, "the playfield's height is not the runtime's");
}

/* -- the big font ---------------------------------------------------- */

/*
 * kg_big_putc() does not draw a rectangle per lit cell; it merges
 * identical adjacent rows and then emits one fill per run of set bits.
 * That is a real transformation of the bitmap and a subtly wrong one
 * produces letters that are subtly wrong -- a 'B' with a stray fill is
 * read as a bad font, not as a bug in a run splitter.
 *
 * So: draw every glyph at a scale where each cell is one pixel, and
 * compare the framebuffer against the bitmap, cell for cell. Covers
 * both directions -- a lit cell that was not drawn AND a drawn pixel
 * that should not be lit.
 */
static void test_font_runs(void)
{
	static const char *ALL =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	int i;

	for (i = 0; ALL[i]; i++) {

		const uint8_t *rows = kg_glyph(ALL[i]);
		int r, col;
		char msg[64];

		CHECK(rows != 0, "a glyph in the font is missing");
		if (!rows) continue;

		kg_fill(0, 0, 16, 16, 0);
		kg_big_putc(2, 2, ALL[i], 1, KG_BIG_SOLID);

		for (r = 0; r < KG_BIG_ROWS; r++)
			for (col = 0; col < KG_BIG_COLS; col++) {

				int want = (rows[r] >> (KG_BIG_COLS - 1 - col)) & 1;
				int got = kg_host_pixel(2 + col, 2 + r);

				if (want != got) {
					snprintf(msg, sizeof(msg),
						"glyph '%c' cell (%d,%d): want %d got %d",
						ALL[i], col, r, want, got);
					CHECK(0, msg);
				}

			}

	}
}

/* kg_big_w() is what every caller centres with; kg_big_puts() returns
 * what it actually used. Two functions disagreeing by one cell shows
 * up as a word that drifts sideways as it gets longer, which nobody
 * reads as an off-by-one in a trailing gap. */
static void test_font_widths(void)
{
	static const char *const WORDS[] = { "A", "AB", "CAT", "ELEPHANT" };
	int i;

	for (i = 0; i < 4; i++) {
		int used = kg_big_puts(0, KG_H, WORDS[i], 3, KG_BIG_SOLID);
		CHECK(used == kg_big_w(WORDS[i], 3),
			"kg_big_puts and kg_big_w disagree about a width");
	}

	/* The scale chooser must respect BOTH bounds. The height one is
	 * the easy one to leave out -- the original had only a width
	 * bound, because a terminal row is a fixed height and here it is
	 * not -- and leaving it out puts a 126px-tall three-letter word in
	 * a 154px play area that also has to hold a prompt and an answer
	 * field. */
	for (i = 0; i < 4; i++) {
		int s = kg_big_best_scale(WORDS[i], KG_W - 16, 60);
		CHECK(kg_big_w(WORDS[i], s) <= KG_W - 16, "best scale overflows width");
		CHECK(kg_big_h(s) <= 60, "best scale overflows height");
		CHECK(s >= KG_BIG_SCALE_MIN && s <= KG_BIG_SCALE_MAX,
			"best scale is out of range");
	}
}

/* -- containment ----------------------------------------------------- */

/*
 * Draw deliberately oversized things, and require every pixel outside
 * the playfield to be untouched.
 *
 * Oversized on purpose: a test that only draws things that fit proves
 * nothing about the clamp, and the clamp is the entire defence -- a
 * rectangle that runs past our own edge would land on whatever app is
 * next to us. The runtime does the clipping now; this holds it to it.
 */
static void test_containment(void)
{
	int escaped;

	/* Each of these runs off a different edge, including negatively. */
	kg_fill(-50, -50, KG_W + 200, KG_H + 200, 1);
	kg_fill(KG_W - 4, KG_H - 4, 100, 100, 1);
	kg_shade(-20, KG_H - 10, KG_W + 40, 60, KG_SHADE_SEL);
	kg_frame(-10, -10, KG_W + 20, KG_H + 20, 1);
	kg_text(-40, -20, "OFF THE TOP LEFT", 1);
	kg_text(KG_W - 10, KG_H - 4, "OFF THE BOTTOM RIGHT", 1);
	kg_big_puts(KG_W - 20, KG_H - 20, "ZZZZ", 8, KG_BIG_SOLID);
	kg_tries_left(KG_H - 4, 6, 20);

	escaped = kg_host_outside();

	if (escaped) {
		char msg[80];
		snprintf(msg, sizeof(msg),
			"%d pixels drawn outside the playfield", escaped);
		CHECK(0, msg);
	}
}

/* -- the menu's own labels ------------------------------------------- */

/*
 * Every game's label has to fit a menu row, allowing for the row's
 * left inset. Checked against the shipped table rather than against a
 * length constant, so adding an eleventh game with a long name fails
 * here rather than being drawn off the right edge on a board.
 */
static void test_labels(void)
{
	int i;

	for (i = 0; i < GAMES_COUNT; i++) {
		CHECK(12 + kg_text_w(GAMES[i].label) < KG_W - 12,
			"a game label does not fit a menu row");
		CHECK(GAMES[i].id && GAMES[i].id[0],
			"a game has no save-file id");
		CHECK(GAMES[i].run != 0, "a game has no run function");
	}

	/* Save-file ids must be unique or two games share a kid's
	 * progress -- which shows up as a level that jumps around and is
	 * very hard to attribute to anything. */
	for (i = 0; i < GAMES_COUNT; i++) {
		int j;
		for (j = i + 1; j < GAMES_COUNT; j++)
			CHECK(strcmp(GAMES[i].id, GAMES[j].id) != 0,
				"two games share a save-file id");
	}
}

int main(void)
{
	if (!kg_host_open("/tmp")) {
		puts("test_layout: kg_init() failed");
		return 1;
	}

	test_bands();
	test_window_size();
	test_font_runs();
	test_font_widths();
	test_containment();
	test_labels();

	if (failures) {
		printf("test_layout: %d FAILURES\n", failures);
		return 1;
	}

	puts("test_layout: ok");

	return 0;
}
