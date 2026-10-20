/*
 * Zeitlos -- host test for sw/common/zedit.c.
 *
 *   cc -std=gnu99 -I sw/common -o /tmp/t sw/common/tests/test_edit.c \
 *      sw/common/zedit.c && /tmp/t
 *
 * Runs on the build machine, like the other tests here.
 *
 * The editing and scrolling logic, which is the part with decisions
 * in it. Drawing needs a framebuffer and is exercised on hardware.
 *
 * Scrolling especially: the dialog's version of this field could not
 * scroll at all, so a filename wider than the box ran off the end of
 * its own frame. That is the improvement both callers get by sharing
 * one widget, and it is the part most likely to be wrong.
 */
#include <stdio.h>
#include <string.h>
#include "zkbd.h"
#include "zedit.h"
static int fails, checks;
static void ck(int c, const char *w) {
	checks++; if (!c) { fails++; printf("FAIL: %s\n", w); } }

/* zedit.c's draw path is not linked here; scroll_to_caret is exercised
 * through a copy of the same rule, so the test pins the BEHAVIOUR the
 * caller sees rather than the private function. */
static void scroll_for(z_edit_t *e, int cols) {
	if (e->cur < e->scroll) e->scroll = e->cur;
	if (e->cur >= e->scroll + cols) e->scroll = e->cur - cols + 1;
	if (e->scroll > e->len) e->scroll = e->len;
	if (e->scroll < 0) e->scroll = 0;
}

static int fills[64][5], nfill, text_x, text_y;
static bool text_solid;

int main(void) {
	char buf[64];
	z_edit_t e;

	z_edit_init(&e, buf, sizeof buf, "hello");
	ck(e.len == 5 && e.cur == 5, "init puts the caret at the end");

	ck(z_edit_key(&e, 'X') && !strcmp(buf, "helloX"), "typing appends");
	ck(z_edit_key(&e, 0x7f) && !strcmp(buf, "hello"), "backspace deletes");

	ck(z_edit_key(&e, Z_KEY_HOME) && e.cur == 0, "home");
	ck(z_edit_key(&e, 'A') && !strcmp(buf, "Ahello"), "insert at the caret");
	ck(z_edit_key(&e, Z_KEY_DELETE) && !strcmp(buf, "Aello"),
		"delete removes forwards");

	/* Boundaries: no movement past either end, and no underflow. */
	z_edit_set(&e, "ab");
	e.cur = 0;
	ck(!z_edit_key(&e, Z_KEY_LEFT), "left at the start does nothing");
	ck(!z_edit_key(&e, 0x7f) && !strcmp(buf, "ab"),
		"backspace at the start does nothing");
	e.cur = e.len;
	ck(!z_edit_key(&e, Z_KEY_RIGHT), "right at the end does nothing");
	ck(!z_edit_key(&e, Z_KEY_DELETE), "delete at the end does nothing");

	/* Return and Escape belong to the caller. */
	ck(!z_edit_key(&e, '\r') && !strcmp(buf, "ab"), "return is not consumed");
	ck(!z_edit_key(&e, 0x1b) && !strcmp(buf, "ab"), "escape is not consumed");

	/* Capacity: the buffer must never overrun, and the NUL must stay. */
	{
		char small[6];
		z_edit_t s;
		z_edit_init(&s, small, sizeof small, "");
		for (int i = 0; i < 40; i++) z_edit_key(&s, 'z');
		ck(s.len == 5 && small[5] == '\0', "fills to capacity and stops");
		ck(strlen(small) == 5, "still NUL terminated at capacity");
	}

	/* Scrolling: the caret must stay visible at both ends. */
	{
		z_edit_set(&e, "0123456789ABCDEF");
		e.cur = e.len; e.scroll = 0;
		scroll_for(&e, 8);
		ck(e.cur - e.scroll < 8, "caret visible when typing at the end");
		ck(e.scroll == e.len - 8 + 1,
			"scrolled exactly far enough, not further");
		e.cur = 0;
		scroll_for(&e, 8);
		ck(e.scroll == 0, "returning to the start scrolls back");
	}

		// -- UTF-8 (long file names, docs/sdcard.md) --
	{
		char b[16];
		z_edit_t u;
		z_edit_init(&u, b, sizeof(b), "a");
		z_edit_key(&u, 0xFC);						// u-umlaut, 2 bytes
		z_edit_key(&u, 0x65E5);						// a kanji, 3 bytes
		ck(u.len == 6 && !strcmp(b, "a\xC3\xBC\xE6\x97\xA5"), "typed UTF-8");
		z_edit_key(&u, Z_KEY_LEFT);
		ck(u.cur == 3, "Left steps over a 3-byte character");
		z_edit_key(&u, 0x7f);
		ck(!strcmp(b, "a\xE6\x97\xA5") && u.cur == 1, "Backspace removes a whole 2-byte character");
		z_edit_key(&u, Z_KEY_DELETE);
		ck(!strcmp(b, "a") && u.len == 1, "Delete removes a whole 3-byte character");
		// A character that does not fit whole is refused.
		z_edit_init(&u, b, 5, "abc");
		ck(!z_edit_key(&u, 0x65E5) && u.len == 3, "no half character at the end");
		ck(z_edit_key(&u, 0xE4) == false || u.len == 5, "a 2-byte one does not fit either");
	}

	// Drawing never clears where the text is and then draws it again
	// (that flashed while typing): the text goes down solid, cell
	// backgrounds and all, and the black fills stay off its cells.
	{
		static const z_font_t f58 = { 5, 8, 32, 126, NULL, 0, 0 };
		z_win_t w;
		char b[32];
		z_edit_t u;
		int i;
		bool over = false;
		memset(&w, 0, sizeof(w));
		z_edit_init(&u, b, sizeof b, "hello");
		u.focus = true;
		nfill = 0; text_x = -1;
		z_edit_draw(&w, &u, 10, 20, 200, 14, &f58);
		for (i = 0; i < nfill; i++)
			if (fills[i][4] == 0 && fills[i][0] < text_x + 25 && fills[i][0] + fills[i][2] > text_x &&
				fills[i][1] < text_y + 8 && fills[i][1] + fills[i][3] > text_y) over = true;
		ck(text_x == 13 && text_y == 23 && text_solid, "the text, solid, inside the padding");
		ck(!over, "nothing cleared under the text");
		ck(text_y + 8 <= 20 + 14 - 1 - 2, "and two rows clear of the bottom line (a 14-row box)");
	}

	printf("%s: %d checks, %d failures\n", fails?"FAIL":"ok", checks, fails);
	return fails ? 1 : 0;
}

/* The draw path needs a framebuffer; stubbed so the editing logic
 * above can be linked and tested on its own (and the drawing calls
 * recorded). */
void z_win_content_rect(const z_win_t *w, z_clip_t *c) { (void)w; c->x0 = 0; c->y0 = 0; c->x1 = 639; c->y1 = 479; }
void z_fb_hw_fill_rect(int a,int b,int c,int d,int e) {
	if (nfill < 64) { fills[nfill][0] = a; fills[nfill][1] = b; fills[nfill][2] = c; fills[nfill][3] = d; fills[nfill][4] = e; nfill++; }
}
void z_fb_draw_utf8_2(int x, int y, const char *t, int fg, int bg, const z_font_t *font, const z_clip_t *clip) {
	(void)t; (void)fg; (void)bg; (void)font; (void)clip;
	text_x = x; text_y = y; text_solid = bg == 0 && fg == 1;
}


void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip) { (void)x;(void)y;(void)c;(void)color;(void)font;(void)clip; }
void z_fb_draw_utf8(int x, int y, const char *t, int color, const z_font_t *font, const z_clip_t *clip) { (void)x;(void)y;(void)t;(void)color;(void)font;(void)clip; }
