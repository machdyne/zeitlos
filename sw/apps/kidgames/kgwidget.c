/*
 * kidgames -- the three widgets more than one screen needs.
 *
 * Split out of kgui.c only because that file is already the mode
 * switch, the pump and the drawing primitives, and these are none of
 * those. Everything here is built out of kgui.h and knows nothing
 * about windows, game mode or messages.
 */

#include <string.h>

#include "kgui.h"
#include "kgpad.h"
#include "kgfont.h"

/* -- menu ------------------------------------------------------------ */

#define MENU_ROW_H   14
#define MENU_TOP     58
#define MENU_FOOT_H  16

/* The QUIT button in the footer. Present because Escape is the only
 * way out of the menu otherwise, and in game mode there is no titlebar
 * close icon either -- so a machine with a mouse and no keyboard, or a
 * kid who has not met Escape, would have no way to leave at all. */
#define QUIT_W  56
#define QUIT_X  (KG_W - QUIT_W - 3)
#define QUIT_Y  (KG_H - MENU_FOOT_H + 2)
#define QUIT_H  12

typedef struct {
	const char *title;
	const char *const *items;
	int count;
	int sel;
	int top;		/* first visible row */
	int rows;		/* visible rows */
} menu_state_t;

static menu_state_t ms;

static int menu_row_y(int i)
{
	return MENU_TOP + (i - ms.top) * MENU_ROW_H;
}

static void menu_draw(void *user)
{
	int i;

	(void)user;

	kg_clear();
	kg_header(0, 0, 0);

	if (ms.title) {
		/* The title in big letters, sized to whatever it is rather
		 * than to a number written down here -- "KIDGAMES" at scale 6
		 * is 282px wide and just fits, and a longer title would
		 * silently overflow if the scale were fixed. */
		int scale = kg_big_best_scale(ms.title, KG_W - 8, 36);
		kg_big_puts_centered(KG_PLAY_Y + 4, ms.title, scale, KG_BIG_SOLID);
	}

	for (i = ms.top; i < ms.count && i < ms.top + ms.rows; i++) {

		int y = menu_row_y(i);
		bool sel = (i == ms.sel);

		if (sel) {
			/*
			 * SOLID, not a shade.
			 *
			 * This was a shade wash plus inverted text, on the theory
			 * that the wash carried the selection and the inversion
			 * was the no-dither fallback. The render settled it:
			 * colour-0 glyphs on a 6/16 ordered dither are mush --
			 * half the ink the letters need is already lit by the
			 * dither, so the strokes and the background differ by
			 * almost nothing.
			 *
			 * A shade is fine BEHIND text at low levels and fine
			 * UNDER nothing at any level. It is not fine under
			 * inverted text, and that is a general rule for this app,
			 * not a fact about level 6: the two are competing for the
			 * same pixels. KG_SHADE_SEL is now used for washes that
			 * carry no text.
			 */
			kg_fill(4, y - 2, KG_W - 8, MENU_ROW_H, 1);
			kg_text2(12, y + 1, ms.items[i], 0, 1);
		} else {
			kg_text(12, y + 1, ms.items[i], 1);
		}

	}

	/* Footer: the hint on the left, QUIT on the right. */
	kg_fill(0, KG_H - MENU_FOOT_H, KG_W, MENU_FOOT_H, 0);
	kg_fill(0, KG_H - MENU_FOOT_H, KG_W, 1, 1);
	kg_text(4, QUIT_Y + 2, "SPACE MOVES   ENTER PLAYS", 1);

	kg_frame(QUIT_X, QUIT_Y, QUIT_W, QUIT_H, 1);
	kg_text(QUIT_X + (QUIT_W - kg_text_w("QUIT")) / 2, QUIT_Y + 2,
		"QUIT", 1);
}

int kg_menu(const char *title, const char *const *items, int count)
{
	kg_repaint_fn saved_fn;
	void *saved_user;
	int result = -1;

	if (count <= 0) return -1;

	saved_fn = kg_get_repaint(&saved_user);

	ms.title = title;
	ms.items = items;
	ms.count = count;
	ms.sel = 0;
	ms.top = 0;
	ms.rows = (KG_H - MENU_FOOT_H - MENU_TOP) / MENU_ROW_H;
	if (ms.rows < 1) ms.rows = 1;

	/* No on-screen keyboard here: nothing is typed, and the rows
	 * themselves are the click targets. */
	kg_pad_hide();

	kg_set_repaint(menu_draw, 0);
	menu_draw(0);

	for (;;) {

		kg_key_t k = kg_getkey();

		if (k == KG_KEY_QUIT) { result = -1; break; }
		if (k == KG_KEY_ESC) { result = -1; break; }

		if (k == KG_KEY_ENTER) { result = ms.sel; break; }

		if (kg_key_is_next(k)) {
			ms.sel = (ms.sel + 1) % ms.count;
		} else if (kg_key_is_prev(k)) {
			ms.sel = (ms.sel + ms.count - 1) % ms.count;
		} else if (k == KG_KEY_CHAR) {
			/* Type the first letter of a game to jump to it. Cheap,
			 * and the one thing a confident reader will try. */
			int i;
			for (i = 1; i <= ms.count; i++) {
				int j = (ms.sel + i) % ms.count;
				char c = ms.items[j][0];
				if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
				if (c == kg_last_char()) { ms.sel = j; break; }
			}
		} else if (k == KG_KEY_CLICK) {

			int x = kg_click_x(), y = kg_click_y();
			int i;

			if (x >= QUIT_X && x < QUIT_X + QUIT_W &&
				y >= QUIT_Y && y < QUIT_Y + QUIT_H) { result = -1; break; }

			for (i = ms.top; i < ms.count && i < ms.top + ms.rows; i++) {
				int ry = menu_row_y(i) - 2;
				if (y >= ry && y < ry + MENU_ROW_H) {
					/* One click chooses. Not select-then-confirm:
					 * a double click is a fine motor skill and this
					 * audience does not reliably have one. */
					ms.sel = i;
					menu_draw(0);
					result = i;
					break;
				}
			}

			if (result >= 0) break;
			continue;

		} else {
			continue;
		}

		/* Keep the selection on screen. With ten games and fourteen
		 * rows this never fires today; it exists so that adding an
		 * eleventh game is one line in games.c and nothing else. */
		if (ms.sel < ms.top) ms.top = ms.sel;
		if (ms.sel >= ms.top + ms.rows) ms.top = ms.sel - ms.rows + 1;

		menu_draw(0);

	}

	kg_set_repaint(saved_fn, saved_user);

	return result;
}

/* -- message box ------------------------------------------------------ */

#define MSG_W   260
#define MSG_BTN_W 60
#define MSG_BTN_H 14

typedef struct {
	const char *title;
	const char *const *lines;
	int nlines;
	int x, y, w, h;
	int btn_x, btn_y;
} msg_state_t;

static msg_state_t msgs;

static void msg_draw(void *user)
{
	int i;

	(void)user;

	/* Filled, framed, and drawn OVER whatever is beneath -- there is
	 * no compositor inside the playfield, so a dialog here is just
	 * pixels on top. That is also why dismissing one has to repaint
	 * the screen underneath; kg_message() does that on the way out. */
	kg_fill(msgs.x, msgs.y, msgs.w, msgs.h, 0);
	kg_frame(msgs.x, msgs.y, msgs.w, msgs.h, 1);
	kg_frame(msgs.x + 2, msgs.y + 2, msgs.w - 4, msgs.h - 4, 1);

	if (msgs.title) {
		kg_fill(msgs.x + 3, msgs.y + 3, msgs.w - 6, 11, 1);
		kg_text2(msgs.x + (msgs.w - kg_text_w(msgs.title)) / 2,
			msgs.y + 5, msgs.title, 0, 1);
	}

	for (i = 0; i < msgs.nlines; i++)
		kg_text(msgs.x + (msgs.w - kg_text_w(msgs.lines[i])) / 2,
			msgs.y + 20 + i * 11, msgs.lines[i], 1);

	kg_frame(msgs.btn_x, msgs.btn_y, MSG_BTN_W, MSG_BTN_H, 1);
	kg_text(msgs.btn_x + (MSG_BTN_W - kg_text_w("ENTER")) / 2,
		msgs.btn_y + 3, "ENTER", 1);
}

bool kg_message(const char *title, const char *const *lines, int nlines)
{
	kg_repaint_fn saved_fn;
	void *saved_user;
	bool result = false;
	bool pad_was = kg_pad_visible();

	if (nlines > KG_MSG_LINES) nlines = KG_MSG_LINES;

	saved_fn = kg_get_repaint(&saved_user);

	msgs.title = title;
	msgs.lines = lines;
	msgs.nlines = nlines;
	msgs.w = MSG_W;
	msgs.h = 20 + nlines * 11 + 8 + MSG_BTN_H + 6;
	msgs.x = (KG_W - msgs.w) / 2;
	msgs.y = (KG_PLAY_Y + KG_H - msgs.h) / 2;
	if (msgs.y < KG_PLAY_Y) msgs.y = KG_PLAY_Y;
	msgs.btn_x = msgs.x + (msgs.w - MSG_BTN_W) / 2;
	msgs.btn_y = msgs.y + msgs.h - MSG_BTN_H - 6;

	/*
	 * The box is drawn over the caller's screen, so the caller's
	 * repaint must run FIRST on a redraw and the box on top of it.
	 * Replacing the callback outright would leave a hole wherever the
	 * box is not -- which on a wm redraw is most of the window.
	 */
	kg_pad_hide();
	kg_set_repaint(msg_draw, 0);
	if (saved_fn) saved_fn(saved_user);
	msg_draw(0);

	for (;;) {

		kg_key_t k = kg_getkey();

		if (k == KG_KEY_QUIT) { result = false; break; }
		if (k == KG_KEY_ENTER || k == KG_KEY_SPACE) { result = true; break; }
		if (k == KG_KEY_ESC) { result = false; break; }

		if (k == KG_KEY_CLICK) {
			int x = kg_click_x(), y = kg_click_y();
			/* Anywhere inside the box dismisses it, not only the
			 * button. The button says where to aim; a near miss
			 * should not be a non-event for someone still learning to
			 * point. Outside the box does nothing, so a wild click
			 * does not skip a "well done" screen. */
			if (x >= msgs.x && x < msgs.x + msgs.w &&
				y >= msgs.y && y < msgs.y + msgs.h) { result = true; break; }
		}

	}

	kg_set_repaint(saved_fn, saved_user);
	if (pad_was) kg_pad_show(KG_CS_ALPHA);
	if (saved_fn) saved_fn(saved_user);

	return result;
}

/* -- text field -------------------------------------------------------- */

/*
 * Tall enough for a scale-3 glyph (21px) plus a frame and a pixel of
 * air each side.
 *
 * It was 20, which is one pixel SHORT of the scale the sizing code
 * below then picked -- so every typed answer had its bottom row
 * clipped by the frame. Nothing asserted the two agreed, and at 1:1 on
 * a desk it is invisible; pixel-doubled it is a letter with its feet
 * cut off. The height and the scale are now derived from each other
 * rather than chosen separately and hoped to match.
 */
#define FIELD_GLYPH_SCALE 3
#define FIELD_H (KG_BIG_ROWS * FIELD_GLYPH_SCALE + 7)

typedef struct {
	int x, y, w;
	char *buf;
	int maxlen;
	kg_repaint_fn owner_fn;
	void *owner_user;
} field_state_t;

static field_state_t fs;

/* Just the field, for a keystroke -- the caller's whole screen does not
 * need repainting because a letter was typed. */
static void field_draw(void)
{
	int scale = FIELD_GLYPH_SCALE;
	int tw;

	kg_fill(fs.x, fs.y, fs.w, FIELD_H, 0);
	kg_frame(fs.x, fs.y, fs.w, FIELD_H, 1);

	/* The answer as it is typed, in big letters -- the same font the
	 * question is asked in. A 5x8 answer under a 10x-scaled question
	 * reads as a footnote rather than as the thing being worked on.
	 *
	 * Steps DOWN only if a long answer would not otherwise fit. A
	 * word-length answer at level 5 is seven or eight characters,
	 * which at scale 3 is 164px in a 240px field, so this rarely
	 * fires -- but Word Guess can hold a whole revealed word and it
	 * does fire there. */
	tw = kg_big_w(fs.buf, scale);
	while (scale > KG_BIG_SCALE_MIN && tw > fs.w - 12) {
		scale--;
		tw = kg_big_w(fs.buf, scale);
	}

	if (fs.buf[0])
		kg_big_puts(fs.x + (fs.w - tw) / 2,
			fs.y + (FIELD_H - kg_big_h(scale)) / 2,
			fs.buf, scale, KG_BIG_SOLID);

	/* A caret only when the field is empty, as a "type here" cue.
	 * With text in it the text is the cue, and a blinking caret is
	 * one more moving thing on a screen aimed at someone who is
	 * already concentrating hard. */
	if (!fs.buf[0])
		kg_fill(fs.x + fs.w / 2 - 1, fs.y + 5, 2, FIELD_H - 10, 1);
}

static void field_repaint(void *user)
{
	(void)user;

	if (fs.owner_fn) fs.owner_fn(fs.owner_user);
	kg_pad_draw();
	field_draw();
}

bool kg_input_line(int x, int y, char *buf, int maxlen, kg_charset_t charset)
{
	kg_repaint_fn saved_fn;
	void *saved_user;
	bool result = false;
	int n = 0;

	saved_fn = kg_get_repaint(&saved_user);

	kg_pad_show(charset);

	fs.x = x;

	/*
	 * y < 0 means "just above the keyboard", which is where every
	 * caller wants it and none of them can compute: the digits layout
	 * is bottom-aligned into the pad band, so the keys start lower
	 * than KG_PAD_Y and a field placed against that constant would
	 * float in the middle of a gap. kg_pad_top() knows where the keys
	 * actually are, but only after kg_pad_show() -- which is why that
	 * moved above this block.
	 */
	fs.y = y >= 0 ? y : kg_pad_top() - FIELD_H - 8;
	fs.w = KG_W - 2 * x;
	if (fs.w < 40) fs.w = 40;
	fs.buf = buf;
	fs.maxlen = maxlen;
	fs.owner_fn = saved_fn;
	fs.owner_user = saved_user;

	buf[0] = '\0';

	kg_set_repaint(field_repaint, 0);

	kg_pad_draw();
	field_draw();

	for (;;) {

		kg_key_t k = kg_getkey();

		if (k == KG_KEY_QUIT || k == KG_KEY_ESC) { result = false; break; }

		if (k == KG_KEY_ENTER) {
			/* Enter on an empty field is IGNORED, not submitted. Every
			 * caller relies on a true return meaning a non-empty
			 * answer, and a kid pressing GO to see what it does should
			 * not be told they are wrong. */
			if (n > 0) { result = true; break; }
			continue;
		}

		if (k == KG_KEY_BACKSPACE) {
			if (n > 0) { buf[--n] = '\0'; field_draw(); }
			continue;
		}

		if (k == KG_KEY_CHAR) {

			char c = kg_last_char();
			bool ok;

			switch (charset) {
			case KG_CS_DIGITS: ok = (c >= '0' && c <= '9'); break;
			case KG_CS_ALPHA:  ok = (c >= 'A' && c <= 'Z'); break;
			default:           ok = true; break;
			}

			/* A rejected character is silently dropped. The pad only
			 * shows the right charset, so this only fires for the
			 * physical keyboard -- and a letter typed into a number
			 * field should be a non-event, not an error the kid has to
			 * clear. */
			if (!ok) continue;

			if (n < maxlen) {
				buf[n++] = c;
				buf[n] = '\0';
				field_draw();
			}

			continue;

		}

	}

	kg_pad_hide();
	kg_set_repaint(saved_fn, saved_user);

	return result;
}
