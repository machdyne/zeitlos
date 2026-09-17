/*
 * kidgames -- on-screen keyboard tests.
 *
 * The pad is the app's whole answer to "playable with a mouse alone",
 * and every one of its failure modes is quiet: a key that overlaps its
 * neighbour types the wrong letter, a key outside the band is drawn
 * nowhere and cannot be pressed, and a click and a keystroke that land
 * on different indices light the wrong key while typing the right
 * one -- which is worse than either, because it teaches a kid the
 * wrong position for the letter they just found.
 *
 * sw/apps/wm's dock_layout.h makes the same argument about the dock,
 * and for the same reason: a click landing one slot off gets blamed on
 * the mouse for a week.
 */

#include "kg_shim.h"

#include "../kgui.h"
#include "../kgpad.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
		failures++; \
	} \
} while (0)

/* Every key inside the playfield, inside the pad band, and not
 * overlapping any other. */
static void test_geometry(kg_charset_t cs, const char *what)
{
	int i, j;
	int n;

	kg_pad_show(cs);
	n = kg_pad_count();

	CHECK(n > 0, "the pad laid out no keys");

	for (i = 0; i < n; i++) {

		const kg_padkey_t *a = kg_pad_key(i);

		CHECK(a->x >= 0 && a->x + a->w <= KG_W, "a key runs off the side");
		CHECK(a->y >= KG_PAD_Y && a->y + a->h <= KG_H,
			"a key is outside the pad band");

		/* Big enough to hit. A five-year-old with a mouse is not
		 * aiming at a 12-pixel target, and at 1:1 in a window these
		 * are already the smallest thing on screen. */
		CHECK(a->w >= 24 && a->h >= 12, "a key is too small to hit");

		for (j = i + 1; j < n; j++) {

			const kg_padkey_t *b = kg_pad_key(j);
			bool sep = a->x + a->w <= b->x || b->x + b->w <= a->x ||
				a->y + a->h <= b->y || b->y + b->h <= a->y;

			CHECK(sep, "two keys overlap");

		}

	}

	(void)what;
}

/* A click at a key's centre must return that key, and the four corners
 * just outside it must not. */
static void test_hit(kg_charset_t cs)
{
	int i, n;

	kg_pad_show(cs);
	n = kg_pad_count();

	for (i = 0; i < n; i++) {

		const kg_padkey_t *k = kg_pad_key(i);

		CHECK(kg_pad_hit(k->x + k->w / 2, k->y + k->h / 2) == i,
			"a click at a key's centre did not find it");

		/* Exclusive upper bounds: a click one pixel past the right
		 * edge belongs to the next key or to nothing, never to this
		 * one. An inclusive test here makes every key one pixel wider
		 * than it is drawn, and the overlap check above would not
		 * catch it because the RECTANGLES do not overlap. */
		CHECK(kg_pad_hit(k->x + k->w, k->y + k->h / 2) != i,
			"a key's hit box extends past its right edge");
		CHECK(kg_pad_hit(k->x + k->w / 2, k->y + k->h) != i,
			"a key's hit box extends past its bottom edge");

	}

	CHECK(kg_pad_hit(0, 0) == -1, "the play area answers pad clicks");
	CHECK(kg_pad_hit(KG_W / 2, KG_PLAY_Y) == -1,
		"the play area answers pad clicks");
}

/*
 * A click and the matching keystroke must land on the SAME index.
 *
 * This is the one that matters. kg_pad_hit() is what the mouse goes
 * through and kg_pad_find() is what a press on the real keyboard goes
 * through, and they are separate pieces of code over the same table.
 * If they disagree, pressing C on the keyboard lights some other key
 * -- and the pad exists to show a kid where C is.
 */
static void test_click_and_key_agree(kg_charset_t cs)
{
	int i, n;

	kg_pad_show(cs);
	n = kg_pad_count();

	for (i = 0; i < n; i++) {
		const kg_padkey_t *k = kg_pad_key(i);
		CHECK(kg_pad_find(k->key, k->ch) == i,
			"a keystroke and a click land on different keys");
	}
}

/* Every letter present exactly once in the alpha layout, every digit
 * in the digits one, and the three command keys in both. A missing
 * letter is a word that cannot be typed with the mouse at all. */
static void test_coverage(void)
{
	const char *c;
	int i;

	kg_pad_show(KG_CS_ALPHA);

	for (c = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; *c; c++) {
		int found = 0;
		for (i = 0; i < kg_pad_count(); i++)
			if (kg_pad_key(i)->key == KG_KEY_CHAR &&
				kg_pad_key(i)->ch == *c) found++;
		CHECK(found == 1, "a letter is missing or duplicated on the pad");
	}

	CHECK(kg_pad_find(KG_KEY_ENTER, 0) >= 0, "alpha pad has no GO key");
	CHECK(kg_pad_find(KG_KEY_BACKSPACE, 0) >= 0, "alpha pad has no BACK key");
	CHECK(kg_pad_find(KG_KEY_ESC, 0) >= 0, "alpha pad has no MENU key");

	kg_pad_show(KG_CS_DIGITS);

	for (c = "0123456789"; *c; c++) {
		int found = 0;
		for (i = 0; i < kg_pad_count(); i++)
			if (kg_pad_key(i)->key == KG_KEY_CHAR &&
				kg_pad_key(i)->ch == *c) found++;
		CHECK(found == 1, "a digit is missing or duplicated on the pad");
	}

	CHECK(kg_pad_find(KG_KEY_ENTER, 0) >= 0, "digit pad has no GO key");
	CHECK(kg_pad_find(KG_KEY_BACKSPACE, 0) >= 0, "digit pad has no BACK key");
	CHECK(kg_pad_find(KG_KEY_ESC, 0) >= 0, "digit pad has no MENU key");

	/* No letters on a number pad. A field that only accepts digits
	 * showing keys it will silently reject is a kid pressing a key and
	 * having nothing happen, repeatedly. */
	for (i = 0; i < kg_pad_count(); i++) {
		const kg_padkey_t *k = kg_pad_key(i);
		CHECK(k->key != KG_KEY_CHAR || (k->ch >= '0' && k->ch <= '9'),
			"a letter appears on the digits pad");
	}
}

/* The QWERTY order is the whole point -- an alphabetical pad would be
 * easier to scan and useless for finding the key on the machine in
 * front of you. Asserted so a well-meaning tidy-up cannot quietly sort
 * it. */
static void test_qwerty(void)
{
	static const char *const ROW = "QWERTYUIOP";
	int i;
	int prev_x = -1;

	kg_pad_show(KG_CS_ALPHA);

	for (i = 0; ROW[i]; i++) {
		int idx = kg_pad_find(KG_KEY_CHAR, ROW[i]);
		CHECK(idx >= 0, "a top-row letter is missing");
		if (idx < 0) continue;
		CHECK(kg_pad_key(idx)->x > prev_x,
			"the top row is not in QWERTY order");
		prev_x = kg_pad_key(idx)->x;
	}

	/* A is below Q, not next to it: the rows are staggered the way a
	 * keyboard's are. */
	CHECK(kg_pad_key(kg_pad_find(KG_KEY_CHAR, 'A'))->y >
		kg_pad_key(kg_pad_find(KG_KEY_CHAR, 'Q'))->y,
		"the home row is not below the top row");
	CHECK(kg_pad_key(kg_pad_find(KG_KEY_CHAR, 'A'))->x >
		kg_pad_key(kg_pad_find(KG_KEY_CHAR, 'Q'))->x,
		"the home row is not indented");
}

/* The digits layout is bottom-aligned into the band, so kg_pad_top()
 * must report lower than for the alpha layout -- that is what gives
 * the answer field its extra room, and a caller placing the field
 * against KG_PAD_Y instead would put it back where it was. */
static void test_pad_top(void)
{
	int alpha_top, digit_top;

	kg_pad_show(KG_CS_ALPHA);
	alpha_top = kg_pad_top();

	kg_pad_show(KG_CS_DIGITS);
	digit_top = kg_pad_top();

	CHECK(alpha_top >= KG_PAD_Y, "the alpha pad starts above its band");
	CHECK(digit_top > alpha_top,
		"the digits pad is not bottom-aligned into the band");

	kg_pad_hide();
	CHECK(kg_pad_top() == KG_H, "a hidden pad claims to occupy rows");
}

/* Held state, which drives both the inverted key and the pulse. */
static void test_held(void)
{
	int q;

	kg_pad_show(KG_CS_ALPHA);
	q = kg_pad_find(KG_KEY_CHAR, 'Q');

	CHECK(kg_pad_held() == -1, "a fresh pad reports a held key");

	kg_pad_set_held(q, 1000);
	CHECK(kg_pad_held() == q, "the held key was not recorded");

	/* Nothing pulses before the nudge threshold. An ordinary press is
	 * well under it and must not flash at all, or the cue means
	 * nothing when it does fire. */
	CHECK(!kg_pad_animate(1000 + KG_HOLD_NUDGE_TICKS / 2),
		"the pad pulsed before the hold threshold");
	CHECK(kg_pad_animate(1000 + KG_HOLD_NUDGE_TICKS + 1),
		"the pad did not pulse after the hold threshold");

	kg_pad_set_held(-1, 2000);
	CHECK(kg_pad_held() == -1, "the key stayed held after release");
	CHECK(!kg_pad_animate(9999), "a released key still pulses");
}

int main(void)
{
	if (!z_render_open((z_win_t *)kg_window(), KG_WIN_W, KG_WIN_H)) {
		puts("test_pad: skipped (cannot map the VRAM address)");
		return 77;
	}

	test_geometry(KG_CS_ALPHA, "alpha");
	test_geometry(KG_CS_DIGITS, "digits");
	test_hit(KG_CS_ALPHA);
	test_hit(KG_CS_DIGITS);
	test_click_and_key_agree(KG_CS_ALPHA);
	test_click_and_key_agree(KG_CS_DIGITS);
	test_coverage();
	test_qwerty();
	test_pad_top();
	test_held();

	if (failures) {
		printf("test_pad: %d FAILURES\n", failures);
		return 1;
	}

	puts("test_pad: ok");

	return 0;
}
