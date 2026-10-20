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

	// -- controls and actions (Phase 3) --
	{
		card_t *k;
		obj_t *o;
		edit_goto(&ed, 0);
		k = edit_card(&ed);
		ck(cmd("button Next card") && ed.nsel == 1 && ed.sel[0]->kind == OB_BUTTON &&
			!strcmp(ed.sel[0]->text, "Next card"), "button LABEL: placed and selected");
		o = ed.sel[0];
		ck(cmd("action go next") && o->action && !strcmp(o->action, "(go 'next)"), "action in plain words");
		ck(cmd("action speak Hello there; wait 500; go \"the end\"") &&
			!strcmp(o->action, "(speak \"Hello there\") (wait 500) (go \"\\\"the end\\\"\")"),
			"several steps; quotes are kept as text");
		ck(cmd("action (go 3)") && !strcmp(o->action, "(go 3)"), "forms as they are");
		ck(!cmd("action frob x") && strstr(ed.msg, "Not an action"), "an unknown verb is refused");
		ck(cmd("action none") && !o->action, "action none");
		ck(edit_undo(&ed), "undo the action's removal");
		k = edit_card(&ed);				// undo re-makes the card
		o = card_find_obj(k, "button 1");
		ck(o && o->action && !strcmp(o->action, "(go 3)"), "the action is back");
		edit_select_none(&ed);
		ck(cmd("action go first") && k->action && !strcmp(k->action, "(go 'first)"), "nothing selected: the card's");
		ck(cmd("list red, green, blue") && !strcmp(ed.sel[0]->text, "red\ngreen\nblue") &&
			ed.sel[0]->value == -1, "list A, B, C: a line each, none chosen");
		ck(cmd("text one, two") && !strcmp(ed.sel[0]->text, "one\ntwo"), "text: a list's items");
		ck(cmd("slider 1 10") && ed.sel[0]->vmin == 1 && ed.sel[0]->vmax == 10 && ed.sel[0]->value == 1,
			"slider MIN MAX");
		ck(cmd("value 50") && ed.sel[0]->value == 10, "value: within the range");
		ck(cmd("range 0 100") && ed.sel[0]->vmax == 100 && ed.sel[0]->value == 10, "range");
		ck(!cmd("range 5 5"), "an empty range is refused");
		ck(cmd("radio Tea") && cmd("group drink") && !strcmp(ed.sel[0]->group, "drink"), "radio, group");
		ck(cmd("check") && !strcmp(ed.sel[0]->text, "Check"), "a check with its default label");
		ck(cmd("field") && ed.sel[0]->kind == OB_FIELD, "a field");
		ck(cmd("app on") && ed.d->app && cmd("loop") && ed.d->loop, "app, loop");
		ck(cmd("app off") && !ed.d->app, "app off");
		{
			sbuf_t b;
			deck_t *e2;
			char err[64];
			sb_init(&b);
			deckio_write(&b, ed.d);
			e2 = deckio_read(b.buf, b.len, err, sizeof(err));
			ck(e2 && e2->loop && e2->card[0]->action && !strcmp(e2->card[0]->action, "(go 'first)") &&
				card_find_obj(e2->card[0], "button 1") &&
				card_find_obj(e2->card[0], "button 1")->kind == OB_BUTTON, "the controls are written and read");
			deck_free(e2);
			sb_free(&b);
		}
		ck(cmd("transition iris 300") && edit_card(&ed)->trans == TR_IRIS, "transition iris");
	}

	// -- diagrams (Phase 5) --
	{
		card_t *k;
		obj_t *a, *b, *ln, *el, *p;
		int i;
		ck(cmd("new"), "a new card for a diagram");
		k = edit_card(&ed);
		ck(cmd("diamond Is it?") && ed.sel[0]->kind == OB_DIAMOND && !strcmp(ed.sel[0]->text, "Is it?") &&
			ed.sel[0]->align == AL_CENTER, "diamond TEXT: a flowchart shape, its text in the middle");
		a = ed.sel[0];
		obj_set_box(a, 40, 40, 100, 60);
		ck(cmd("cylinder Store") && ed.sel[0]->kind == OB_DRUM, "cylinder");
		b = ed.sel[0];
		obj_set_box(b, 300, 200, 100, 60);
		// connect them: the arrow starts on the diamond's edge, ends on the box's
		edit_select(&ed, a, false);
		edit_select(&ed, b, true);
		ck(cmd("connect") && ed.nsel == 1 && obj_is_connector(ed.sel[0]), "connect: an arrow from the first to the second");
		ln = ed.sel[0];
		ck(!strcmp(ln->from, a->name) && !strcmp(ln->to, b->name) && ln->arrows == AR_END, "from, to, an arrowhead");
		edit_route(&ed);
		ck(ln->x > 90 && ln->x < 140 && ln->y + ln->h <= 205 && ln->y + ln->h >= 195, "it runs edge to edge");
		// the box moves: the arrow follows
		obj_move(b, 0, 60);
		edit_route(&ed);
		ck(ln->y + ln->h >= 255 && ln->y + ln->h <= 265, "move a shape: the connector follows");
		// an elbow
		edit_select(&ed, ln, false);
		ck(cmd("route elbow") && ln->kind == OB_POLY && ln->npts == 4, "route elbow: four points");
		el = ln;
		edit_route(&ed);
		ck(el->pts[1] == el->pts[3] && el->pts[2] == el->pts[4] && el->pts[5] == el->pts[7],
			"across, along, across");
		ck(cmd("route straight") && el->kind == OB_LINE, "route straight");
		// a line drawn from one shape to another connects them
		ed.tool = TOOL_LINE;
		drag(90, 70, 350, 290, false);
		ck(obj_is_connector(k->obj[k->nobj - 1]) && !strcmp(k->obj[k->nobj - 1]->to, b->name),
			"a line drawn from a shape to a shape is a connector");
		ed.tool = TOOL_SELECT;
		// written and read back, routed on reading
		{
			sbuf_t sb;
			deck_t *e2;
			char err[64];
			sb_init(&sb);
			deckio_write(&sb, ed.d);
			e2 = deckio_read(sb.buf, sb.len, err, sizeof(err));
			ck(e2 && card_find_obj(e2->card[ed.cur], ln->name) &&
				!strcmp(card_find_obj(e2->card[ed.cur], ln->name)->to, b->name) &&
				card_find_obj(e2->card[ed.cur], a->name)->kind == OB_DIAMOND, "connectors and shapes in the file");
			deck_free(e2);
			sb_free(&sb);
		}
		// groups: one click selects them all; a copy is a new group, its
		// connector joining the copies
		edit_select(&ed, a, false);
		edit_select(&ed, b, true);
		edit_select(&ed, ln, true);
		ck(cmd("group") && a->gid && a->gid == b->gid && b->gid == ln->gid, "group");
		edit_select(&ed, b, false);
		ck(ed.nsel == 3, "a click on one selects its group");
		edit_duplicate(&ed);
		ck(ed.nsel == 3 && ed.sel[0]->gid && ed.sel[0]->gid != a->gid, "a duplicate is a group of its own");
		for (i = 0; i < ed.nsel; i++)
			if (obj_is_connector(ed.sel[i])) {
				ck(strcmp(ed.sel[i]->from, a->name) && card_find_obj(k, ed.sel[i]->from) &&
					card_find_obj(k, ed.sel[i]->from)->gid == ed.sel[i]->gid, "its connector joins the copies");
			}
		ck(cmd("ungroup") && !ed.sel[0]->gid, "ungroup");
		// flip and rotate
		edit_select(&ed, a, false);
		ck(cmd("ungroup"), "a alone");
		edit_select(&ed, a, false);
		{
			int x0 = a->x, w0 = a->w, h0 = a->h;
			ck(cmd("flip across") && (a->flags & OBF_FLIPX) && a->x == x0, "flip a shape alone: in place, mirrored");
			ck(cmd("rotate") && a->w == h0 && a->h == w0, "rotate: a quarter turn swaps its sides");
		}
		// a polygon's corners: dragged, added, taken away
		ed.tool = TOOL_POLY;
		click(200, 30); click(260, 30); click(260, 90);
		edit_finish_poly(&ed);
		p = k->obj[k->nobj - 1];
		ck(p->kind == OB_POLY && p->npts == 3, "a polygon of three corners");
		ed.tool = TOOL_SELECT;
		edit_select(&ed, p, false);
		{
			int hx[EDIT_HANDLES_MAX], hy[EDIT_HANDLES_MAX];
			ck(edit_handles(p, hx, hy) == 11 && hx[9] == 260 && hy[9] == 30, "a handle on each corner");
		}
		drag(260, 90, 280, 120, false);
		ck(p->pts[4] == 280 && p->pts[5] == 120 && p->h == 90, "a corner dragged; the box follows");
		edit_press(&ed, 230, 30, false, true);		// a double-click on a side
		edit_release(&ed, 230, 30);
		ck(p->npts == 4 && p->pts[2] == 230, "a double-click on a side: a corner there");
		edit_press(&ed, 230, 30, false, true);		// on that corner
		edit_release(&ed, 230, 30);
		ck(p->npts == 3, "a double-click on a corner: gone");
		ck(cmd("smooth") && p->smooth, "smooth");
		// freehand: a wobbly line made of fewer points, smooth
		ed.tool = TOOL_PEN;
		edit_press(&ed, 20, 300, false, false);
		for (i = 1; i <= 60; i++) edit_motion(&ed, 20 + i * 4, 300 + (i % 2), false);
		edit_release(&ed, 260, 300);
		p = k->obj[k->nobj - 1];
		ck(p->kind == OB_POLY && p->smooth && !p->closed && p->npts >= 2 && p->npts <= 4,
			"freehand: jitter dropped, a smooth line");
		ed.tool = TOOL_SELECT;
	}

	// -- /import: an SVG drawing's outlines as polygons --
	{
		static const char doc[] =
			"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
			"<path d='M10 10 L90 10 L90 90 L10 90 Z M30 30 L70 30 L70 70 L30 70 Z' fill='black' fill-rule='evenodd'/>"
			"<circle cx='50' cy='50' r='8' fill='#808080' stroke='black'/>"
			"<polyline points='0,100 50,95 100,100' fill='none' stroke='black'/>"
			"</svg>";
		card_t *k;
		int n0, made, i, u;
		ck(cmd("new"), "a card to import onto");
		k = edit_card(&ed);
		n0 = k->nobj;
		u = ed.nundo;
		made = edit_import_svg(&ed, doc, sizeof(doc) - 1);
		k = edit_card(&ed);
		ck(made == 4 && k->nobj == n0 + 4, "four outlines, four polygons");
		ck(ed.nundo == u + 1 || (u == UNDO_MAX && ed.nundo == u), "one undo step");
		ck(ed.nsel == 4, "all of them selected");
		for (i = n0; i < k->nobj; i++) ck(k->obj[i]->gid && k->obj[i]->gid == k->obj[n0]->gid, "one group");
		ck(k->obj[n0]->closed && k->obj[n0]->fill == FILL_SHADE && k->obj[n0]->fill_v == 16 &&
			k->obj[n0]->npts == 4 && !k->obj[n0]->line_w, "a black square, its corners only");
		ck(k->obj[n0 + 1]->fill == FILL_SHADE && k->obj[n0 + 1]->fill_v == 0, "its hole, white");
		ck(k->obj[n0 + 2]->fill_v > 0 && k->obj[n0 + 2]->fill_v < 16 && k->obj[n0 + 2]->line_w == 1 &&
			k->obj[n0 + 2]->npts >= 8, "a grey circle with an outline");
		ck(!k->obj[n0 + 3]->closed && k->obj[n0 + 3]->fill == FILL_NONE && k->obj[n0 + 3]->npts == 3, "an open line");
		{
			int x0 = 32767, x1 = -32768, gw = ed.d->page.gw;
			for (i = n0; i < k->nobj; i++) {
				if (k->obj[i]->x < x0) x0 = k->obj[i]->x;
				if (k->obj[i]->x + k->obj[i]->w > x1) x1 = k->obj[i]->x + k->obj[i]->w;
			}
			ck(x0 > gw / 10 - 2 && x1 < gw - gw / 10 + 2 && (x0 + x1) / 2 > gw / 2 - 4 && (x0 + x1) / 2 < gw / 2 + 4,
				"fitted in the middle of the card");
		}
		ck(edit_undo(&ed) && edit_card(&ed)->nobj == n0, "undo takes it all back");
		u = ed.nundo;
		ck(edit_import_svg(&ed, "hello", 5) == -1 && edit_card(&ed)->nobj == n0, "not SVG: nothing");
		ck(edit_import_svg(&ed, "<svg viewBox='0 0 10 10'></svg>", 31) == 0 && edit_card(&ed)->nobj == n0 &&
			ed.nundo == u, "an empty drawing: nothing, and no undo step");
		{
			// what it made is saved and read back as it was
			sbuf_t b;
			deck_t *d2;
			char err[64];
			made = edit_import_svg(&ed, doc, sizeof(doc) - 1);
			sb_init(&b);
			ck(made == 4 && deckio_write(&b, ed.d), "written");
			d2 = deckio_read(b.buf, b.len, err, sizeof(err));
			ck(d2 && d2->card[ed.cur]->nobj == edit_card(&ed)->nobj &&
				d2->card[ed.cur]->obj[n0 + 2]->npts == edit_card(&ed)->obj[n0 + 2]->npts, "imported shapes read back");
			if (d2) deck_free(d2);
			sb_free(&b);
		}
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

