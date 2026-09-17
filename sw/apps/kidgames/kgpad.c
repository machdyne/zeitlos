/*
 * kidgames -- the on-screen keyboard. See kgpad.h.
 */

#include "kgpad.h"
#include "kgui.h"

static kg_padkey_t keys[KG_PAD_MAX_KEYS];
static int nkeys;
static bool visible;
static kg_charset_t layout_cs = KG_CS_ALPHA;

static int held = -1;
static uint32_t held_since;
static bool pulse_on;
static uint32_t pulse_next;

/* Row baselines. Row 3 ends at KG_PAD_Y + 52 + 16 = 239, which is the
 * last pixel of the playfield -- the pad is sized to the space, not
 * placed in it and hoped for. */
#define ROW_Y(r)  (KG_PAD_Y + 1 + (r) * KG_PAD_ROW_STEP)

static void add(kg_key_t key, char ch, int x, int y, int w,
	const char *label)
{
	kg_padkey_t *k;

	if (nkeys >= KG_PAD_MAX_KEYS) return;

	k = &keys[nkeys++];
	k->key = key;
	k->ch = ch;
	k->x = (int16_t)x;
	k->y = (int16_t)y;
	k->w = (int16_t)w;
	k->h = KG_PAD_ROW_H;
	k->label = label;
}

/* One row of single-character keys on the standard stride, starting at
 * `x0`. `s` is the row as it appears on a keyboard. */
static void add_row(const char *s, int x0, int row)
{
	int i;

	for (i = 0; s[i]; i++)
		add(KG_KEY_CHAR, s[i], x0 + i * KG_PAD_KEY_STEP, ROW_Y(row),
			KG_PAD_KEY_W, 0);
}

/*
 * Labels for the wide keys.
 *
 * Words, not symbols. An arrow for Backspace and a bent arrow for
 * Enter are the conventions, and they are conventions a reader picks
 * up from having used a keyboard -- which is precisely what the
 * audience has not done yet. Both labels here are short enough to
 * read in the 5x8 font the game is already being read in.
 *
 * -- WHY "ENTER" AND NOT "GO" --
 *
 * "GO" was the first choice: it says what pressing the key DOES, in a
 * word a five-year-old can sound out, and ENTER is jargon.
 *
 * It was wrong, and wrong against this file's own argument. The
 * layout is QWERTY rather than alphabetical precisely because the pad
 * has to rehearse the keyboard in front of the kid -- "the screen says
 * K, now find K on the thing in front of me". A key drawn as GO is
 * a key that is not on that keyboard. Worse, the menu's footer had to
 * name the same key in words and said "GO PLAYS", so one key had two
 * names depending on which screen you were looking at.
 *
 * Ease of reading loses to fidelity here for the same reason it loses
 * on the letter layout. BACK stays as it is: it sits on the key the
 * keyboard calls Backspace, and is a truncation of that word rather
 * than a different one.
 */
static const char LBL_BACK[] = "BACK";
static const char LBL_ENTER[] = "ENTER";
static const char LBL_MENU[] = "MENU";
static const char LBL_SPACE[] = "SPACE";

static void build_alpha(void)
{
	nkeys = 0;

	/* QWERTY, at the physical layout's own indents. Ten keys across
	 * the top sets the stride; the next two rows are centred on it the
	 * way a real keyboard staggers them. */
	add_row("QWERTYUIOP", 1, 0);
	add_row("ASDFGHJKL", 17, 1);
	add_row("ZXCVBNM", 33, 2);

	add(KG_KEY_BACKSPACE, 0, 259, ROW_Y(2), 60, LBL_BACK);

	/* MENU, not ESC. The odd one out, deliberately: this key is an
	 * APP ACTION -- leave the game -- not a letter the kid is being
	 * helped to find, and "Esc" is meaningless to a five-year-old in a
	 * way that "Enter" at least is not once somebody has said it out
	 * loud. Escape on the real keyboard does the same thing for
	 * whoever already knows it. */
	add(KG_KEY_ESC,   0,   1, ROW_Y(3),  60, LBL_MENU);
	add(KG_KEY_SPACE, ' ', 65, ROW_Y(3), 160, LBL_SPACE);
	add(KG_KEY_ENTER, 0, 229, ROW_Y(3),  90, LBL_ENTER);
}

static void build_digits(void)
{
	nkeys = 0;

	/*
	 * The number ROW, in keyboard order, not a calculator keypad.
	 * A keypad is easier to aim at and teaches the wrong map: the
	 * digits on the machine in front of the kid are in a row across
	 * the top, ending in 0, and that is what this has to rehearse.
	 *
	 * BOTTOM-ALIGNED into the band -- rows 2 and 3, not 0 and 1.
	 *
	 * This layout needs two of the four rows, and top-aligning it left
	 * 34 pixels of blank band sitting between the answer field and the
	 * keys, which a render showed reading as a gap in the middle of
	 * the keyboard rather than as extra room. Pushed to the bottom,
	 * the spare rows join the play area above and simply are not
	 * there. The band's constants (KG_PAD_Y, KG_PAD_H) do not move, so
	 * nothing else in the layout has to know.
	 */
	add_row("1234567890", 1, 2);

	add(KG_KEY_ESC,       0,  34, ROW_Y(3), 60, LBL_MENU);
	add(KG_KEY_BACKSPACE, 0,  98, ROW_Y(3), 92, LBL_BACK);
	add(KG_KEY_ENTER,     0, 194, ROW_Y(3), 92, LBL_ENTER);
}

void kg_pad_show(kg_charset_t cs)
{
	/* KG_CS_ALNUM gets the alpha layout. Five rows do not fit in
	 * KG_PAD_H and no game asks for it -- every field in the original
	 * is ALPHA or DIGITS. Kept mapped rather than rejected so a future
	 * game that wants it gets letters and a usable pad rather than an
	 * assertion. */
	layout_cs = cs;

	if (cs == KG_CS_DIGITS) build_digits();
	else build_alpha();

	visible = true;
	held = -1;
	pulse_on = false;
}

void kg_pad_hide(void)
{
	visible = false;
	held = -1;
	nkeys = 0;
}

bool kg_pad_visible(void) { return visible; }
int kg_pad_count(void) { return nkeys; }

const kg_padkey_t *kg_pad_key(int idx)
{
	if (idx < 0 || idx >= nkeys) return 0;
	return &keys[idx];
}

int kg_pad_hit(int x, int y)
{
	int i;

	if (!visible) return -1;

	for (i = 0; i < nkeys; i++) {
		const kg_padkey_t *k = &keys[i];
		if (x >= k->x && x < k->x + k->w &&
			y >= k->y && y < k->y + k->h) return i;
	}

	return -1;
}

int kg_pad_find(kg_key_t key, char ch)
{
	int i;

	if (!visible) return -1;

	for (i = 0; i < nkeys; i++) {
		if (keys[i].key != key) continue;
		if (key == KG_KEY_CHAR && keys[i].ch != ch) continue;
		return i;
	}

	return -1;
}

/* One key. `down` inverts it; `nudge` is the pulse frame for a key
 * that has been held too long. */
static void draw_key(int idx, bool down, bool nudge)
{
	const kg_padkey_t *k;
	char lbl[2];
	const char *text;
	int tw, tx, ty;
	int ink = down ? 0 : 1;
	int paper = down ? 1 : 0;

	k = kg_pad_key(idx);
	if (!k) return;

	if (down) kg_fill(k->x, k->y, k->w, k->h, 1);
	else {
		kg_fill(k->x, k->y, k->w, k->h, 0);
		kg_frame(k->x, k->y, k->w, k->h, 1);
	}

	/*
	 * The pulse. A second frame INSIDE the key, appearing and
	 * disappearing -- not a change of shade, and not the key blinking
	 * out entirely.
	 *
	 * Not a shade because a bitstream without dither draws every
	 * shade as a flat fill and the nudge would vanish (kg.h's rule).
	 * Not a blink-out because a key that disappears while a finger is
	 * on it reads as "broken", and the message is "let go", not
	 * "something is wrong".
	 */
	if (nudge)
		kg_frame(k->x + 2, k->y + 2, k->w - 4, k->h - 4, ink);

	if (k->label) {
		text = k->label;
	} else {
		lbl[0] = k->ch;
		lbl[1] = '\0';
		text = lbl;
	}

	tw = kg_text_w(text);
	tx = k->x + (k->w - tw) / 2;
	ty = k->y + (k->h - 8) / 2;

	/* kg_text2, always: the key body is filled, and kg_text() would
	 * paint a solid cell of 0 around each glyph and punch a hole in
	 * it. That is the trap z_win_draw_text2() exists for. */
	kg_text2(tx, ty, text, ink, paper);
}

void kg_pad_draw_key(int idx)
{
	if (!visible) return;
	draw_key(idx, idx == held, idx == held && pulse_on);
}

void kg_pad_draw(void)
{
	int i;
	int top;

	if (!visible) return;

	/* Clear the whole band first, then the keys. A per-key clear
	 * leaves the gaps between them holding whatever the last screen
	 * put there. */
	kg_fill(0, KG_PAD_Y, KG_W, KG_PAD_H, 0);

	/*
	 * The rule goes above the TOPMOST KEY, not at the top of the band.
	 *
	 * These are the same line for the alpha layout and are not for the
	 * digits one, which is bottom-aligned into the band (build_digits).
	 * A rule at the band's top edge there draws a line across the
	 * screen with nothing under it -- which reads as the bottom of the
	 * play area rather than the top of the keyboard, and puts the
	 * answer field in a box of its own by accident.
	 */
	top = kg_pad_top();
	if (top >= 2) kg_fill(0, top - 3, KG_W, 1, 1);

	for (i = 0; i < nkeys; i++)
		draw_key(i, i == held, i == held && pulse_on);
}

int kg_pad_top(void)
{
	int i, top = KG_H;

	if (!visible || !nkeys) return KG_H;

	for (i = 0; i < nkeys; i++)
		if (keys[i].y < top) top = keys[i].y;

	return top;
}

void kg_pad_set_held(int idx, uint32_t now_ticks)
{
	int was = held;

	if (idx == held) return;

	held = idx;
	held_since = now_ticks;
	pulse_on = false;
	pulse_next = now_ticks + KG_HOLD_NUDGE_TICKS;

	if (!visible) return;

	if (was >= 0) draw_key(was, false, false);
	if (held >= 0) draw_key(held, true, false);
}

int kg_pad_held(void) { return held; }

bool kg_pad_animate(uint32_t now_ticks)
{
	if (!visible || held < 0) return false;

	/* Nothing until the key has been down past the nudge threshold.
	 * An ordinary press is well under it and never pulses at all,
	 * which is what keeps this from being a nag. */
	if ((int32_t)(now_ticks - pulse_next) < 0) return false;

	pulse_on = !pulse_on;
	pulse_next = now_ticks + Z_TICK_HZ / 5;

	draw_key(held, true, pulse_on);

	(void)held_since;
	return true;
}
