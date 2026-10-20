/*
 * slide's controls and actions on the build machine: the .zsl forms
 * for controls, the verbs, (wait), (go 'back), and the player's clicks
 * and typing. `make test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../deck.h"
#include "../deckio.h"
#include "../render.h"
#include "../act.h"

static int fails, checks;
#define ck(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, m); } } while (0)

static uint8_t mem[4 << 20];
static uint8_t scratch[96 * 1024];

// -- the host: what the actions did --
static int at, changed, opened;
static char said[200], spoke[200], ran[200], opened_path[200];
static uint32_t clock_now;
static deck_t *D;

static void h_go(void *u, int to) { (void)u; at = to; act_arrived(D, to); }
static void h_changed(void *u) { (void)u; changed++; }
static void h_open(void *u, const char *p) { (void)u; opened++; snprintf(opened_path, sizeof(opened_path), "%s", p); }
static void h_msg(void *u, const char *s) { (void)u; snprintf(said, sizeof(said), "%s", s); }
static void h_speak(void *u, const char *s) { (void)u; snprintf(spoke, sizeof(spoke), "%s", s); }
static void h_run(void *u, const char *s) { (void)u; snprintf(ran, sizeof(ran), "%s", s); }
static uint32_t h_now(void *u) { (void)u; return clock_now; }

static const char *deck_text =
	"(deck (version 1) (page \"classic\" landscape) (title \"Controls\") (loop)\n"
	" (background \"b\" (button \"home\" (box 450 300 50 20) (text \"Home\") (action \"(go 'first)\")))\n"
	" (card \"one\" (background \"b\")\n"
	"  (button \"next\" (box 20 20 90 24) (radius 6) (text \"Next\") (action \"(go 'next)\"))\n"
	"  (check \"agree\" (box 20 60 150 18) (text \"I agree\") (value 0) (action \"(toggle \\\"thanks\\\")\"))\n"
	"  (text \"thanks\" (box 200 60 100 18) (hidden) (text \"Thanks!\"))\n"
	"  (radio \"r1\" (box 20 90 100 18) (text \"Tea\") (value 1) (group \"drink\"))\n"
	"  (radio \"r2\" (box 20 110 100 18) (text \"Coffee\") (value 0) (group \"drink\"))\n"
	"  (radio \"r3\" (box 20 130 100 18) (text \"Other\") (value 1))\n"
	"  (slider \"vol\" (box 20 160 160 18) (value 25) (range 0 100) (action \"(set-text \\\"thanks\\\" \\\"moved\\\")\"))\n"
	"  (list \"pick\" (box 200 100 120 80) (text \"red\\ngreen\\nblue\") (value -1) (action \"(speak-text \\\"answer\\\")\"))\n"
	"  (field \"answer\" (box 20 200 180 24) (text \"\") (action \"(go \\\"three\\\")\"))\n"
	"  (field \"notes\" (box 20 230 180 60) (text \"a\")))\n"
	" (card \"two\" (action \"(speak \\\"two\\\") (wait 500) (go 'next)\")\n"
	"  (button \"back\" (box 20 20 90 24) (text \"Back\") (action \"(go 'back)\")))\n"
	" (card \"three\"\n"
	"  (button \"odd\" (box 20 20 90 24) (text \"?\") (action \"(frobnicate 1)\"))))\n";

int main(void) {
	char err[96];
	act_host_t h;
	player_t p;
	bool took, redraw;
	obj_t *o;

	pool_init(mem, sizeof(mem));
	render_init(scratch, sizeof(scratch), NULL);
	D = deckio_read(deck_text, (uint32_t)strlen(deck_text), err, sizeof(err));
	ck(D != NULL, "the deck of controls reads");
	if (!D) { printf("  %s\n", err); return 1; }

	// -- the file --
	ck(D->loop && D->card[0]->nobj == 10, "loop, and ten objects");
	o = act_find(D, 0, "vol");
	ck(o && o->kind == OB_SLIDER && o->value == 25 && o->vmin == 0 && o->vmax == 100, "a slider: value and range");
	o = act_find(D, 0, "r2");
	ck(o && o->kind == OB_RADIO && !strcmp(o->group, "drink"), "a radio's group");
	ck(act_find(D, 0, "home") && act_find(D, 0, "home")->kind == OB_BUTTON, "a background's button is found from the card");
	ck(D->card[1]->action && strstr(D->card[1]->action, "wait"), "a card's own action");
	{
		// written and read back the same
		sbuf_t b;
		deck_t *e;
		sb_init(&b);
		ck(deckio_write(&b, D), "written");
		e = deckio_read(b.buf, b.len, err, sizeof(err));
		ck(e && e->loop && e->card[0]->nobj == 10, "read back");
		if (e) {
			sbuf_t c;
			sb_init(&c);
			deckio_write(&c, e);
			ck(c.len == b.len && !memcmp(c.buf, b.buf, b.len), "the same text again");
			sb_free(&c);
			deck_free(e);
		}
		sb_free(&b);
	}

	memset(&h, 0, sizeof(h));
	h.go = h_go; h.changed = h_changed; h.open_deck = h_open; h.message = h_msg;
	h.speak = h_speak; h.run = h_run; h.now = h_now; h.tick_hz = 1000;
	act_init(&h);
	act_arrived(D, 0);

	// -- the verbs --
	at = 0;
	act_run(D, 0, "(go 'next)");
	ck(at == 1 && !strcmp(spoke, "two"), "go next; the card's own action speaks");
	ck(act_waiting() && at == 1, "and waits 500 ms before going on");
	clock_now += 499; act_tick();
	ck(act_waiting() && at == 1, "not yet at 499");
	clock_now += 1; act_tick();
	ck(!act_waiting() && at == 2, "at 500 it goes on: card three");
	act_run(D, 2, "(go 'back)");
	ck(at == 1, "back: to card two");
	ck(act_waiting(), "whose action runs again");
	act_run(D, 1, "(go 'last)");
	ck(at == 2 && !act_waiting(), "a new action drops the waiting one");
	act_run(D, 2, "(go 'next)");
	ck(at == 0, "a looping deck: after the last, the first");
	act_run(D, 0, "(go 3) (go \"two\")");
	ck(at == 1, "go by number and by name, in turn");
	act_reset();
	act_arrived(D, 0);
	at = 0;
	changed = 0;
	act_run(D, 0, "(show \"thanks\") (set-text \"thanks\" \"Hello\") ; a comment\n (set-value \"vol\" 500)");
	o = act_find(D, 0, "thanks");
	ck(!(o->flags & OBF_HIDDEN) && !strcmp(o->text, "Hello") && changed == 3, "show, set-text, a comment");
	ck(act_find(D, 0, "vol")->value == 100, "set-value is kept in the slider's range");
	act_run(D, 0, "(toggle \"thanks\") (hide \"nobody\")");
	ck((o->flags & OBF_HIDDEN) && strstr(said, "nobody"), "toggle; an unknown object says so");
	act_run(D, 0, "(run \"clock\") (open \"/data/slide/other.zsl\") (speak \"never\")");
	ck(!strcmp(ran, "clock") && opened == 1 && !strcmp(opened_path, "/data/slide/other.zsl") &&
		strcmp(spoke, "never"), "run, open; nothing after open");
	act_run(D, 0, "(frobnicate 1)");
	ck(strstr(said, "needs scripting") != NULL, "anything else waits for scripting");
	act_run(D, 0, "(go 'next");
	ck(strstr(said, "cannot read") != NULL, "a broken action says so");
	act_run(D, 0, "(set-text \"thanks\" 42)");
	ck(!strcmp(o->text, "42"), "a number as text");

	// -- the player --
	memset(&p, 0, sizeof(p));
	p.d = D;
	p.cur = 0;
	at = 0;
	act_reset();
	act_arrived(D, 0);
	// a button: pressed, dragged off and back, released on it
	redraw = player_press(&p, 30, 30, &took);
	ck(took && redraw && p.pressed && !strcmp(p.pressed->name, "next"), "a press on a button");
	ck(player_motion(&p, 300, 300) && !p.inside, "dragged off: not pressed");
	player_motion(&p, 30, 30);
	ck(p.inside, "and back on");
	player_release(&p, 30, 30);
	ck(at == 1 && !p.pressed, "released on it: its action");
	p.cur = 0;
	at = 0;
	// released off it: nothing
	player_press(&p, 30, 30, &took);
	player_release(&p, 300, 300);
	ck(at == 0, "released off it: nothing");
	// nothing there: not taken
	player_press(&p, 400, 30, &took);
	ck(!took, "a click on no control is the app's");
	// a check: toggles, and its action shows the text
	o = act_find(D, 0, "thanks");
	o->flags |= OBF_HIDDEN;
	player_press(&p, 25, 65, &took);
	player_release(&p, 25, 65);
	ck(act_find(D, 0, "agree")->value == 1 && !(o->flags & OBF_HIDDEN), "a check: on, and its action");
	player_press(&p, 25, 65, &took);
	player_release(&p, 25, 65);
	ck(act_find(D, 0, "agree")->value == 0, "and off");
	// radios: one of a group; another group untouched
	player_press(&p, 25, 115, &took);
	player_release(&p, 25, 115);
	ck(act_find(D, 0, "r2")->value == 1 && act_find(D, 0, "r1")->value == 0 &&
		act_find(D, 0, "r3")->value == 1, "a radio: one of its group, the other group alone");
	// a slider: dragged to the end, its action on release
	player_press(&p, 25, 165, &took);
	ck(took && act_find(D, 0, "vol")->value == 0, "a slider: pressed at its start");
	player_motion(&p, 500, 165);
	ck(act_find(D, 0, "vol")->value == 100, "dragged past its end: the most");
	player_release(&p, 500, 165);
	ck(!strcmp(act_find(D, 0, "thanks")->text, "moved"), "its action when let go");
	// a list: the second row, its action speaks the field
	o = act_find(D, 0, "answer");
	obj_set_text(o, "hi", 2);
	{
		int rh = render_list_row_h(D, act_find(D, 0, "pick"));
		player_press(&p, 210, 100 + 3 + rh + rh / 2, &took);
		ck(act_find(D, 0, "pick")->value == 1 && !strcmp(spoke, "hi"), "a list: the row clicked, and its action");
	}
	// a field: typing, UTF-8, Backspace, Tab to the next, Enter
	obj_set_text(o, "", 0);
	player_press(&p, 30, 210, &took);
	ck(p.focus == o, "a click in a field: typing goes there");
	player_key(&p, 'a', &redraw);
	player_key(&p, 0xE4, &redraw);		// ä
	player_key(&p, 'b', &redraw);
	ck(!strcmp(o->text, "a\xC3\xA4" "b") && redraw, "typed, in UTF-8");
	player_key(&p, 0x08, &redraw);
	player_key(&p, 0x08, &redraw);
	ck(!strcmp(o->text, "a"), "Backspace takes whole characters");
	player_key(&p, '\t', &redraw);
	ck(p.focus && !strcmp(p.focus->name, "notes"), "Tab: the next field");
	player_key(&p, 0x0d, &redraw);
	ck(!strcmp(p.focus->text, "a\n"), "Enter in a tall field: a new line");
	player_key(&p, '\t', &redraw);
	at = 0;
	player_key(&p, 0x0d, &redraw);
	ck(at == 2, "Enter in a one-line field: its action");
	p.cur = 0;
	ck(player_key(&p, 0x1b, &redraw) && !p.focus, "Esc: no field");
	ck(!player_key(&p, 'x', &redraw), "keys with no field are the app's");
	// hidden controls are not there
	act_find(D, 0, "next")->flags |= OBF_HIDDEN;
	player_press(&p, 30, 30, &took);
	ck(!took, "a hidden button takes no click");

	// a card whose action goes on, round a looping deck: stopped, not a
	// stack overflow
	{
		int i;
		for (i = 0; i < D->ncard; i++) obj_set_str(&D->card[i]->action, "(go 'next)");
		said[0] = 0;
		act_arrived(D, 0);
		ck(strstr(said, "circles") != NULL, "actions going round in circles stop");
	}

	deck_free(D);
	act_reset();
	ck(pool_check(), "pool intact");
	printf("act: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
