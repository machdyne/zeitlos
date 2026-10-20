/*
 * slide's Scheme (Phase 4) on the build machine: ms with define-macro,
 * slide's procedures and short forms, actions that are more than the
 * verbs, card and deck scripts, timers, clicks, errors, and stopping a
 * runaway script. `make test`.
 *
 * zapi (the Zeitlos API) needs the board; its few entry points script.c
 * calls are stubs here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../../../common/zmsg.h"
#include "../deck.h"
#include "../deckio.h"
#include "../render.h"
#include "../act.h"
#include "../script.h"

static int fails, checks;
#define ck(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, m); } } while (0)

// -- what script.c reaches beyond ms: tests/zapi_stubs.c --
// zeitlos.h's stdout hook: the host's printf does not call it, so
// (display ...) is checked through what it returns instead
void (*z_stdout_hook)(const char *, uint32_t);

static uint8_t mem[4 << 20];
static uint8_t scratch[96 * 1024];

static deck_t *D;
static int cur, changed, opened;
static char said[200], spoke[200];
static uint32_t clock_now;
static int steps_left = -1;		// the stop check says stop after this many asks

static void h_go(void *u, int to) { (void)u; cur = to; act_arrived(D, to); script_card(D, to); }
static void h_changed(void *u) { (void)u; changed++; }
static void h_open(void *u, const char *p) { (void)u; (void)p; opened++; }
static void h_msg(void *u, const char *s) { (void)u; snprintf(said, sizeof(said), "%s", s); }
static void h_speak(void *u, const char *s) { (void)u; snprintf(spoke, sizeof(spoke), "%s", s); }
static uint32_t h_now(void *u) { (void)u; return clock_now; }
static void h_eval(void *u, deck_t *d, int c, const char *f) { (void)u; script_eval(d, c, f, NULL, 0); }
static void h_clicked(void *u, deck_t *d, int c, obj_t *o) { (void)u; script_clicked(d, c, o); }
static int h_current(void *u) { (void)u; return cur; }
static bool stop_check(void) { if (steps_left < 0) return false; return steps_left-- == 0; }

static const char *deck_text =
	"(deck (version 1) (page \"classic\" landscape) (title \"Scripts\")\n"
	" (script \"(define score 0) (define (bump n) (set! score (+ score n)) (set-text \\\"score\\\" score))"
	" (when-clicked \\\"plus\\\" (bump 10))\")\n"
	" (card \"one\"\n"
	"  (text \"score\" (box 20 20 100 20) (text \"0\"))\n"
	"  (button \"plus\" (box 20 50 90 24) (text \"+10\"))\n"
	"  (button \"go\" (box 20 80 90 24) (text \"Go\") (action \"(if (> score 5) (go 'next) (speak \\\"not yet\\\"))\"))\n"
	"  (field \"name\" (box 20 110 120 24) (text \"Ada\"))\n"
	"  (check \"c\" (box 20 140 120 18) (text \"c\") (value 1))\n"
	"  (list \"l\" (box 200 20 100 60) (text \"red\\ngreen\") (value 1)))\n"
	" (card \"two\" (script \"(every 100 (set-text \\\"ticks\\\" (+ 1 (string->number (get-text \\\"ticks\\\")))))\")\n"
	"  (text \"ticks\" (box 20 20 100 20) (text \"0\")))\n"
	" (card \"three\" (action \"(go 'next)\")))\n";

static int saves;
static bool h_save(void *u) { (void)u; saves++; return true; }

int main(void) {
	char err[96], out[160];
	act_host_t h;
	bool took;
	player_t p;

	pool_init(mem, sizeof(mem));
	render_init(scratch, sizeof(scratch), NULL);
	D = deckio_read(deck_text, (uint32_t)strlen(deck_text), err, sizeof(err));
	ck(D != NULL, "the deck reads");
	if (!D) { printf("  %s\n", err); return 1; }
	ck(D->script && D->card[1]->script, "deck and card scripts are kept");

	memset(&h, 0, sizeof(h));
	h.go = h_go; h.changed = h_changed; h.open_deck = h_open; h.message = h_msg;
	h.speak = h_speak; h.now = h_now; h.tick_hz = 1000;
	h.eval = h_eval; h.clicked = h_clicked; h.current = h_current; h.save = h_save;
	act_init(&h);
	script_stop_check(stop_check);

	// -- starting, the message box --
	ck(!script_ready(), "Scheme waits until a deck needs it");
	ck(script_eval(D, 0, "(+ 1 2)", out, sizeof(out)) && !strcmp(out, "3"), "the message box: (+ 1 2)");
	ck(script_ready(), "and has started");
	ck(script_eval(D, 0, "(define-macro (twice x) `(begin ,x ,x)) (macroexpand-1 '(twice y))", out, sizeof(out)) &&
		!strcmp(out, "(begin y y)"), "define-macro is there");
	ck(script_eval(D, 0, "(card-name)", out, sizeof(out)) && !strcmp(out, "\"one\""), "card-name");
	ck(script_eval(D, 0, "(list (card-number) (card-count) (deck-title))", out, sizeof(out)) &&
		!strcmp(out, "(1 3 \"Scripts\")"), "card-number, card-count, deck-title");
	ck(script_eval(D, 0, "(list (get-text \"name\") (get-value \"c\") (get-value \"l\"))", out, sizeof(out)) &&
		!strcmp(out, "(\"Ada\" #t \"green\")"), "get-text, get-value of a check and a list");
	ck(!script_eval(D, 0, "(car 5)", out, sizeof(out)) && out[0], "an error says so");
	ck(script_eval(D, 0, "(+ 2 2)", out, sizeof(out)) && !strcmp(out, "4"), "and Scheme goes on");
	ck(!script_eval(D, 0, "(nosuch 1)", out, sizeof(out)) && strstr(out, "nosuch"), "an unknown name is named");

	// -- the deck's script: definitions and a click handler --
	cur = 0;
	script_deck(D, 0);
	act_arrived(D, 0);
	memset(&p, 0, sizeof(p));
	p.d = D;
	p.cur = 0;
	player_press(&p, 30, 60, &took);
	player_release(&p, 30, 60);
	ck(!strcmp(act_find(D, 0, "score")->text, "10"), "(when-clicked \"plus\" ...) from the deck's script");
	// an action in Scheme: (if ...)
	spoke[0] = 0;
	ck(!act_simple(act_find(D, 0, "go")->action), "an (if ...) action is Scheme's");
	cur = 0;
	act_run(D, 0, "(set! score 0)");
	player_press(&p, 30, 90, &took);
	player_release(&p, 30, 90);
	ck(cur == 0 && !strcmp(spoke, "not yet"), "a Scheme action: not yet");
	player_press(&p, 30, 60, &took);
	player_release(&p, 30, 60);
	player_press(&p, 30, 90, &took);
	player_release(&p, 30, 90);
	ck(cur == 1, "and after +10, go next");

	// -- a card's script and its timer --
	ck(script_timers(), "card two's (every 100 ...) is running");
	clock_now += 99; script_tick();
	ck(!strcmp(act_find(D, 1, "ticks")->text, "0"), "not before 100 ms");
	clock_now += 1; script_tick();
	clock_now += 100; script_tick();
	clock_now += 100; script_tick();
	ck(!strcmp(act_find(D, 1, "ticks")->text, "3"), "three ticks in 300 ms");
	// leaving the card stops its timer
	h_go(NULL, 0);
	ck(!script_timers(), "leaving the card stops its timers");
	// (after ...) and cancel, from the message box
	ck(script_eval(D, 0, "(define t (after 50 (set-text \"name\" \"later\")))", out, sizeof(out)), "after");
	clock_now += 60; script_tick();
	ck(!strcmp(act_find(D, 0, "name")->text, "later"), "ran after 50 ms");
	ck(script_eval(D, 0, "(define t2 (every 10 (set-text \"name\" \"x\"))) (cancel t2)", out, sizeof(out)) &&
		!script_timers(), "cancel");
	// an error in a timer stops it, and says so
	said[0] = 0;
	script_eval(D, 0, "(every 10 (car 5))", out, sizeof(out));
	clock_now += 20; script_tick();
	ck(!script_timers() && strstr(said, "timer"), "a failing timer stops, and says so");

	// -- a script that never ends: stopped from outside --
	steps_left = 3;
	ck(!script_eval(D, 0, "(define (spin) (spin)) (spin)", out, sizeof(out)) && strstr(out, "stopped"),
		"a runaway script is stopped");
	steps_left = -1;
	ck(script_eval(D, 0, "(+ 40 2)", out, sizeof(out)) && !strcmp(out, "42"), "and Scheme still works");

	// -- a card whose action goes on through Scheme: (go 'next) from a
	// card's own action is the verbs; through Scheme, deferred until the
	// outer script finishes -- never a script inside a script --
	cur = 0;
	ck(script_eval(D, 0, "(go 3) (card-name)", out, sizeof(out)), "go from Scheme");
	ck(cur == 0 || cur == 2, "it went");

	// -- the check the script editor makes --
	ck(script_check("(define (f x) (+ x 1)) ; a comment (\n(f 1)", err, sizeof(err)), "a script that reads");
	ck(!script_check("(define (f x) (+ x 1)", err, sizeof(err)) && strstr(err, "not closed"), "a ( not closed");
	// records: add-item, remove-item, save-deck
	cur = 0;
	ck(script_eval(D, 0, "(add-item \"l\" \"blue\" \" \" 3)", out, sizeof(out)) &&
		!strcmp(act_find(D, 0, "l")->text, "red\ngreen\nblue 3"), "add-item: a line of values at the end");
	ck(script_eval(D, 0, "(remove-item \"l\")", out, sizeof(out)) && !strcmp(out, "#t") &&
		!strcmp(act_find(D, 0, "l")->text, "red\nblue 3") && act_find(D, 0, "l")->value == -1,
		"remove-item: the chosen one (green)");
	ck(script_eval(D, 0, "(remove-item \"l\")", out, sizeof(out)) && !strcmp(out, "#f"), "remove-item: none chosen");
	act_find(D, 0, "l")->value = 1;
	ck(script_eval(D, 0, "(remove-item \"l\")", out, sizeof(out)) && !strcmp(act_find(D, 0, "l")->text, "red"),
		"remove-item: the last one");
	act_find(D, 0, "l")->value = 0;
	ck(script_eval(D, 0, "(remove-item \"l\")", out, sizeof(out)) && !strcmp(act_find(D, 0, "l")->text, ""),
		"remove-item: the only one");
	ck(script_eval(D, 0, "(add-item \"l\" \"a\\nb\")", out, sizeof(out)) && !strcmp(act_find(D, 0, "l")->text, "a b"),
		"add-item: into an empty list; a newline in a value is a space");
	obj_set_text(act_find(D, 0, "name"), "Ada", 3);
	ck(script_eval(D, 0, "(take-text \"name\")", out, sizeof(out)) && !strcmp(out, "\"Ada\"") &&
		!act_find(D, 0, "name")->text[0], "take-text: the text, and the field emptied");
	ck(script_eval(D, 0, "(save-deck)", out, sizeof(out)) && !strcmp(out, "#t") && saves == 1, "save-deck: the host saves");

	ck(!script_check("(f))", err, sizeof(err)) && strstr(err, "too many"), "a ) too many");
	ck(!script_check("(display \"hi)", err, sizeof(err)) && strstr(err, "string"), "a string not closed");

	deck_free(D);
	act_reset();

	// -- the example decks (sw/data/slide/examples), driven as a person
	// would: clicks on their buttons --
	{
		static const char *const names[] = { "quiz", "calculator", "todo", "clock", "phonebook" };
		int e;
		for (e = 0; e < 5; e++) {
			char path[256], *text;
			FILE *f;
			long n;
			snprintf(path, sizeof(path), "../../data/slide/examples/%s.zsl", names[e]);
			f = fopen(path, "rb");
			ck(f != NULL, path);
			if (!f) continue;
			fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
			text = malloc((size_t)n + 1);
			if (fread(text, 1, (size_t)n, f) != (size_t)n) n = 0;
			fclose(f);
			D = deckio_read(text, (uint32_t)n, err, sizeof(err));
			free(text);
			ck(D && D->app, "an example deck reads, made to run as an app");
			if (!D) { printf("  %s: %s\n", path, err); continue; }
			cur = 0;
			said[0] = 0;
			act_reset();
			script_deck(D, 0);
			ck(!said[0], "its script runs without an error");
			if (said[0]) printf("  %s: %s\n", names[e], said);
			memset(&p, 0, sizeof(p));
			p.d = D;
#define CLICK(nm) do { obj_t *o_ = act_find(D, cur, nm); p.cur = cur; \
	if (o_) { player_press(&p, o_->x + 2, o_->y + 2, &took); player_release(&p, o_->x + 2, o_->y + 2); } \
	else { ck(0, "a control by that name"); printf("  no %s\n", nm); } } while (0)
			if (e == 0) {
				int qn;
				static const char *const right[] = { "a2", "a1", "a3" };
				CLICK("go");
				ck(cur == 1, "quiz: Start");
				for (qn = 0; qn < 3; qn++) {
					CLICK(right[qn]);
					CLICK("check");
					ck(!strcmp(act_find(D, cur, "said")->text, "Right!"), "quiz: a right answer");
					clock_now += 1300;
					script_tick();
				}
				ck(cur == 4 && strstr(act_find(D, 4, "score")->text, "3 of 3"), "quiz: three of three");
				CLICK("again");
				ck(cur == 0, "quiz: again");
			} else if (e == 1) {
				// keys k<row><col>: 7 8 9 / | 4 5 6 * | 1 2 3 - | C 0 = +
				CLICK("k20"); CLICK("k21"); CLICK("k33"); CLICK("k21"); CLICK("k32");
				ck(!strcmp(act_find(D, cur, "d")->text, "14"), "calculator: 12 + 2 = 14");
				CLICK("k13"); CLICK("k12"); CLICK("k32");
				ck(!strcmp(act_find(D, cur, "d")->text, "84"), "calculator: then * 6 = 84");
				CLICK("k30");
				ck(!strcmp(act_find(D, cur, "d")->text, "0"), "calculator: C");
			} else if (e == 2) {
				obj_t *nw = act_find(D, 0, "new");
				obj_set_text(nw, "Milk", 4);
				CLICK("add");
				ck(strstr(act_find(D, 0, "items")->text, "\nMilk") && !nw->text[0], "todo: Add");
				act_find(D, 0, "items")->value = 0;
				CLICK("done");
				ck(!strstr(act_find(D, 0, "items")->text, "Try slide") && act_find(D, 0, "items")->value == -1,
					"todo: Done takes the chosen one");
			} else if (e == 4) {
				obj_t *bk = act_find(D, 0, "book");
				int n0 = saves;
				obj_set_text(act_find(D, 0, "name"), "Grace Hopper", 12);
				obj_set_text(act_find(D, 0, "phone"), "1906", 4);
				CLICK("add");
				ck(strstr(bk->text, "\nGrace Hopper  1906") && !act_find(D, 0, "name")->text[0] &&
					saves == n0 + 1, "phone book: Add, cleared, saved");
				bk->value = 0;
				CLICK("delete");
				ck(!strstr(bk->text, "Ada") && strstr(bk->text, "Grace") && saves == n0 + 2,
					"phone book: Delete the chosen one, saved");
			} else {
				script_card(D, 0);
				ck(script_timers(), "clock: the card ticks");
				h_go(NULL, 1);
				CLICK("start");
				clock_now += 1000; script_tick();
				clock_now += 1000; script_tick();
				CLICK("stop");
				ck(strncmp(act_find(D, 1, "watch")->text, "00:00.0", 7) != 0, "clock: the stopwatch ran");
			}
			deck_free(D);
			act_reset();
		}
	}

	printf("script: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
