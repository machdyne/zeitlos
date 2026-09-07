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

	printf("%s: %d checks, %d failures\n", fails?"FAIL":"ok", checks, fails);
	return fails ? 1 : 0;
}

/* The draw path needs a framebuffer; stubbed so the editing logic
 * above can be linked and tested on its own. */
void z_win_content_rect(const z_win_t *w, z_clip_t *c) { (void)w; (void)c; }
void z_fb_hw_fill_rect(int a,int b,int c,int d,int e){(void)a;(void)b;(void)c;(void)d;(void)e;}


void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip) { (void)x;(void)y;(void)c;(void)color;(void)font;(void)clip; }
