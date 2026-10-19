/*
 * slide's editor without a window: the mouse, the keyboard, commands,
 * undo, the clipboard -- driven the way slide.c drives them.
 * `make -C sw/apps/slide test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../deck.h"
#include "../deckio.h"
#include "../render.h"
#include "../edit.h"
#include "../../../common/zkbd.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static uint8_t mem[8 << 20];
static uint8_t scratch[96 * 1024];
static editor_t ed;

static uint8_t *load(const char *path, uint32_t *len) {
	char p[512];
	FILE *f;
	long n;
	uint8_t *b;
	if (!strncmp(path, "/sys/fonts/", 11)) {
		const char *e = strrchr(path, '.');
		snprintf(p, sizeof(p), "../../data/font/%s/%s", e ? e + 1 : "", path + 11);
	} else snprintf(p, sizeof(p), "%s", path);
	f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = pool_alloc((size_t)n + 1);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { pool_free(b); b = NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return b;
}

static void drag(int x0, int y0, int x1, int y1, bool shift) {
	edit_press(&ed, x0, y0, shift, false);
	edit_motion(&ed, (x0 + x1) / 2, (y0 + y1) / 2, shift);
	edit_motion(&ed, x1, y1, shift);
	edit_release(&ed, x1, y1);
}

static void click(int x, int y) { edit_press(&ed, x, y, false, false); edit_release(&ed, x, y); }

static bool cmd(const char *s) {
	const char *app;
	return edit_command(&ed, s, &app);
}

static void type(const char *s) {
	while (*s) {
		unsigned char c = (unsigned char)*s;
		if (c == '\n') edit_text_key(&ed, 0x0d, 0);
		else if (c < 0x80) edit_text_key(&ed, c, 0);
		else {
			// UTF-8 as one character
			const char *p = s;
			uint32_t cp = 0;
			int n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2, k;
			cp = c & (0x3F >> (n - 1));
			for (k = 1; k < n; k++) cp = (cp << 6) | (p[k] & 0x3F);
			edit_text_key(&ed, cp, 0);
			s += n - 1;
		}
		s++;
	}
}

int main(void) {
	card_t *c;
	deck_t *d;
	pool_init(mem, sizeof(mem));
	render_init(scratch, sizeof(scratch), load);
	d = g_deck = deck_new();
	edit_init(&ed, d);

	// -- draw a rectangle, move it, resize it --
	ed.tool = TOOL_RECT;
	drag(50, 40, 150, 100, false);
	c = edit_card(&ed);
	ck(c->nobj == 1 && c->obj[0]->kind == OB_RECT, "a dragged rectangle");
	ck(c->obj[0]->x == 50 && c->obj[0]->w == 100 && c->obj[0]->h == 60, "corner to corner");
	ck(ed.nsel == 1 && ed.sel[0] == c->obj[0], "and selected");
	ck(!strcmp(c->obj[0]->name, "rect 1"), "named");
	ed.tool = TOOL_SELECT;
	drag(100, 70, 120, 80, false);
	ck(c->obj[0]->x == 70 && c->obj[0]->y == 50, "moved by the drag");
	drag(170, 110, 200, 130, false);		// the bottom-right handle
	ck(c->obj[0]->w == 130 && c->obj[0]->h == 80, "resized by its corner");
	drag(200, 90, 190, 90, false);			// the right edge, inward
	ck(c->obj[0]->w == 120, "an edge handle changes one side");
	ed.tool = TOOL_OVAL;
	drag(300, 200, 330, 210, true);
	ck(c->obj[1]->w == c->obj[1]->h, "shift draws a circle");

	// -- undo and redo --
	ck(edit_undo(&ed) && c != edit_card(&ed), "undo replaces the card");
	c = edit_card(&ed);
	ck(c->nobj == 1 && c->obj[0]->w == 120, "the circle is gone, the rest is as it was");
	ck(edit_undo(&ed) && edit_card(&ed)->obj[0]->w == 130, "and again");
	ck(edit_redo(&ed) && edit_redo(&ed) && edit_card(&ed)->nobj == 2, "redo brings both back");
	c = edit_card(&ed);

	// -- selection --
	ed.tool = TOOL_SELECT;
	click(400, 300);
	ck(ed.nsel == 0, "a click on nothing deselects");
	drag(10, 10, 500, 330, false);
	ck(ed.nsel == 2, "a band selects what it encloses");
	edit_press(&ed, 100, 80, true, false);	// shift on a selected one
	edit_release(&ed, 100, 80);
	ck(ed.nsel == 1 && ed.sel[0]->kind == OB_OVAL, "shift-click deselects one");

	// -- commands on the selection --
	ck(cmd("/fill shade 6") && ed.sel[0]->fill == FILL_SHADE && ed.sel[0]->fill_v == 6, "fill shade");
	ck(cmd("fill 13") && ed.sel[0]->fill == FILL_PATTERN && ed.sel[0]->fill_v == 13, "fill pattern");
	ck(cmd("line 3 8") && ed.sel[0]->line_w == 3 && ed.sel[0]->line_tone == 8, "line width, tone");
	ck(cmd("name sun") && !strcmp(ed.sel[0]->name, "sun"), "name an object");
	ck(!cmd("name rect 1") && strstr(ed.msg, "Already"), "names stay unique on a card");
	ck(!cmd("frobnicate") && strstr(ed.msg, "Not a command"), "unknown commands say so");
	{
		const char *app;
		ck(!edit_command(&ed, "show", &app) && app && !strcmp(app, "show"), "show is the app's");
		ck(!edit_command(&ed, "new deck", &app) && app && !strcmp(app, "new deck"), "so is new deck");
	}
	cmd("front");
	ck(c->obj[c->nobj - 1] == ed.sel[0], "front");
	cmd("back");
	ck(c->obj[0] == ed.sel[0], "back");

	// -- text: type, move the caret, wrap, delete --
	ed.tool = TOOL_TEXT;
	drag(20, 150, 220, 180, false);
	ck(ed.text_obj && ed.text_obj->kind == OB_TEXT, "a text box is ready to type into");
	type("Hello world");
	ck(!strcmp(ed.text_obj->text, "Hello world"), "typed");
	edit_text_key(&ed, Z_KEY_HOME, 0);
	type(">");
	ck(!strcmp(ed.text_obj->text, ">Hello world"), "Home, then typing");
	edit_text_key(&ed, Z_KEY_END, 0);
	edit_text_key(&ed, 0x7f, 0);
	type("D\xc3\xa4");
	ck(!strcmp(ed.text_obj->text, ">Hello worlD\xc3\xa4"), "Backspace and a two-byte character");
	edit_text_key(&ed, 0x7f, 0);
	ck(!strcmp(ed.text_obj->text, ">Hello worlD"), "Backspace removes a whole character");
	edit_text_key(&ed, Z_KEY_LEFT, 0);
	edit_text_key(&ed, Z_KEY_DELETE, 0);
	ck(!strcmp(ed.text_obj->text, ">Hello worl"), "Delete");
	type("\nsecond line that is long enough to wrap in two hundred units");
	{
		zc_fix x, y, h;
		obj_t *t = ed.text_obj;
		text_layout_t L;
		uint32_t at_end = ed.caret;
		render_text_layout(ed.d, t, &L);
		ck(L.n >= 3, "the text wraps");
		edit_caret(&ed, &x, &y, &h);
		ck(y == L.top + L.line_h * (L.n - 1), "the caret is on the last line");
		edit_text_key(&ed, Z_KEY_UP, 0);
		ck(ed.caret < at_end, "Up moves into the line above");
		edit_text_key(&ed, Z_KEY_DOWN, 0);
		ck(ed.caret == at_end || ed.caret + 3 >= at_end, "and Down comes back to about the same place");
		edit_text_key(&ed, Z_KEY_HOME, Z_KBD_MOD_LCTRL);
		ck(ed.caret == 0, "Ctrl+Home");
	}
	edit_text_end(&ed);
	ck(ed.text_obj == NULL, "editing ends");
	ck(edit_undo(&ed), "undo the typing");
	{
		card_t *k = edit_card(&ed);
		obj_t *t = k->obj[k->nobj - 1];
		ck(t->kind == OB_TEXT && strcmp(t->text, ">Hello worl") != 0, "typing undoes in runs");
	}
	c = edit_card(&ed);

	// -- the clipboard --
	{
		sbuf_t b;
		int n0 = c->nobj, n;
		ed.tool = TOOL_SELECT;
		edit_select_all(&ed);
		sb_init(&b);
		ck(edit_copy(&ed, &b) && b.len > 0, "copy");
		n = edit_paste(&ed, b.buf, b.len);
		c = edit_card(&ed);
		ck(n == n0 && c->nobj == 2 * n0 && ed.nsel == n0, "paste adds and selects copies");
		ck(c->obj[n0]->x == c->obj[0]->x + 10, "pasted onto itself, moved aside");
		ck(strcmp(c->obj[n0]->name, c->obj[0]->name) != 0, "with names of their own");
		sb_free(&b);
		n = edit_paste(&ed, "just words", 10);
		c = edit_card(&ed);
		ck(n == 1 && c->obj[c->nobj - 1]->kind == OB_TEXT && !strcmp(c->obj[c->nobj - 1]->text, "just words"),
			"plain text pastes as a text box");
		edit_delete(&ed);
		ck(c->nobj == 2 * n0, "delete");
	}

	// -- polygons --
	ed.tool = TOOL_POLY;
	click(300, 50);
	click(400, 60);
	click(350, 120);
	click(301, 51);			// back on the first point: closed
	c = edit_card(&ed);
	ck(c->obj[c->nobj - 1]->kind == OB_POLY && c->obj[c->nobj - 1]->npts == 3 &&
		c->obj[c->nobj - 1]->closed, "a closed polygon of three points");
	ck(c->obj[c->nobj - 1]->x == 300 && c->obj[c->nobj - 1]->w == 100, "with its box");

	// -- cards --
	cmd("new");
	ck(ed.d->ncard == 2 && ed.cur == 1, "a new card, current");
	cmd("name intro");
	ck(!strcmp(ed.d->card[1]->name, "intro"), "a card is named when nothing is selected");
	cmd("go 1");
	ck(ed.cur == 0, "go by number");
	cmd("go intro");
	ck(ed.cur == 1, "go by name");
	cmd("dup card");
	ck(ed.d->ncard == 3 && ed.cur == 2, "duplicate a card");
	edit_move_card(&ed, 2, 0);
	ck(ed.cur == 0 && strcmp(ed.d->card[0]->name, "intro") != 0, "move a card, the current one follows");
	cmd("delete card");
	ck(ed.d->ncard == 2, "delete a card");
	ck(edit_undo(&ed) && ed.d->ncard == 3, "undo brings a deleted card back");

	// -- backgrounds --
	cmd("go 2");
	cmd("background plain");
	ck(ed.d->nbg == 1 && ed.d->card[ed.cur]->bg == 0, "a background, made");
	cmd("edit background");
	ck(ed.on_bg && edit_card(&ed) == ed.d->bg[0], "editing it");
	ed.tool = TOOL_RECT;
	drag(0, 0, 512, 30, false);
	ck(ed.d->bg[0]->nobj == 1, "drawing goes onto the background");
	cmd("edit background");
	ck(!ed.on_bg, "and back to the card");
	cmd("new");
	ck(ed.d->card[ed.cur]->bg == 0, "a new card shares the background");

	// -- the page, objects scaled with it --
	{
		obj_t *o;
		cmd("go card 1");
		c = edit_card(&ed);
		ck(c->nobj > 0, "the first card, with its drawing");
		if (!c->nobj) return 1;
		o = c->obj[0];
		{
			int x = o->x;
			ck(cmd("page 16:9") && ed.d->page.gh == 288, "page 16:9");
			o = edit_card(&ed)->obj[0];
			ck(o->x == x, "x keeps (same width)");
		}
		ck(cmd("page letter portrait") && ed.d->page.gw == 340 && ed.d->page.gh == 440, "letter portrait");
		ck(!cmd("page b7") && strstr(ed.msg, "Not a page"), "unknown sizes refused");
	}

	// -- text styles and fonts by command --
	{
		ed.tool = TOOL_TEXT;
		drag(10, 10, 200, 40, false);
		type("Title");
		edit_text_end(&ed);
		ck(cmd("font serif 30 bold"), "font serif 30 bold");
		ck(ed.sel[0]->size == 30 && (ed.sel[0]->style & ST_BOLD) &&
			!strcmp(ed.d->font[ed.sel[0]->font], "serif"), "applied");
		ck(!cmd("font comicsans"), "a font that is not there is refused");
		ck(cmd("align center middle") && ed.sel[0]->align == AL_CENTER && ed.sel[0]->valign == VA_MIDDLE,
			"align");
		ck(cmd("italic") && (ed.sel[0]->style & ST_ITALIC), "italic toggles on");
	}

	// -- the whole deck survives a save and a load --
	{
		sbuf_t b;
		deck_t *e2;
		char err[64];
		sb_init(&b);
		ck(deckio_write(&b, ed.d), "write the edited deck");
		e2 = deckio_read(b.buf, b.len, err, sizeof(err));
		ck(e2 && e2->ncard == ed.d->ncard && e2->nbg == 1, "and read it back");
		deck_free(e2);
		sb_free(&b);
	}

	// -- the history is bounded, and undo still works at its limit --
	{
		int i;
		ed.tool = TOOL_SELECT;
		edit_select_all(&ed);
		for (i = 0; i < 200; i++) edit_nudge(&ed, 1, 0);
		ck(ed.nundo == UNDO_MAX, "the undo history keeps the last 64 steps");
		for (i = 0; i < UNDO_MAX; i++) if (!edit_undo(&ed)) break;
		ck(i == UNDO_MAX && !edit_undo(&ed), "all of them undo, then no more");
	}

	edit_reset(&ed, ed.d);
	deck_free(ed.d);
	ck(pool_check(), "pool intact");
	printf("edit: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}

