/*
 * slide -- Scheme in a deck. See script.h and docs/slide_scripting.md.
 */

#include "script.h"
#include "render.h"
#include "../../common/zpool.h"
#include "../../common/zapi/ms_api.h"
#include "../../common/zapi/zapi.h"
#include "../../common/zeitlos.h"
#include "../../common/zwm.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -- slide's own Scheme: the short forms a deck's author writes ----------------
//
// (after MS forms...) and (every MS forms...) run forms later; (cancel
// id) stops one. (when-clicked "name" forms...) runs forms when that
// control is clicked, on any card. No lambda for the author to write:
// define-macro (sw/ext/ms) wraps the forms in one.

static const char slide_scm[] =
	"(define %slide-timers '())\n"
	"(define (%slide-del k l) (cond ((null? l) '()) ((equal? (car (car l)) k) (cdr l))"
	"  (else (cons (car l) (%slide-del k (cdr l))))))\n"
	"(define (%slide-timer-add id thunk) (set! %slide-timers (cons (cons id thunk) %slide-timers)) id)\n"
	"(define (%slide-timer-fire id) (let ((t (assv id %slide-timers))) (if t ((cdr t)) #f)))\n"
	"(define (%slide-timer-forget id) (set! %slide-timers (%slide-del id %slide-timers)) #t)\n"
	"(define-macro (after ms . body) `(%slide-timer-add (%slide-after ,ms #f) (lambda () ,@body)))\n"
	"(define-macro (every ms . body) `(%slide-timer-add (%slide-after ,ms #t) (lambda () ,@body)))\n"
	"(define (cancel id) (%slide-cancel id) (%slide-timer-forget id))\n"
	"(define %slide-clicks '())\n"
	"(define (%slide-on-click name thunk)"
	"  (set! %slide-clicks (cons (cons name thunk) (%slide-del name %slide-clicks))) name)\n"
	"(define-macro (when-clicked name . body) `(%slide-on-click ,name (lambda () ,@body)))\n"
	"(define (%slide-click name) (let ((h (assoc name %slide-clicks))) (if h (begin ((cdr h)) #t) #f)))\n"
	"(define (%slide-reset) (set! %slide-timers '()) (set! %slide-clicks '()) #t)\n";

// -- state -------------------------------------------------------------------------

static bool ready, failed;
static bool (*stop_fn)(void);
static deck_t *ctx_deck;
static int ctx_cur;
static int depth;				// evaluations in progress (a builtin's go -> ...)
static char err_msg[160];		// the first error of this evaluation

// Captured (display ...) output: its last line goes to the status line.
static char cap[160];
static int cap_len;

static void cap_hook(const char *d, uint32_t n) {
	uint32_t i;
	for (i = 0; i < n; i++) {
		char c = d[i];
		if (c == '\r') continue;
		if (c == '\n') { if (cap_len) cap[cap_len] = 0; continue; }
		// a new line after a newline starts over
		if (cap_len && cap[cap_len] == 0) cap_len = 0;
		if (cap_len < (int)sizeof(cap) - 1) { cap[cap_len++] = c; cap[cap_len] = 0; }
	}
}

static void log_hook(int level, const char *m) {
	if (level == 0) return;			// info
	if (!err_msg[0]) snprintf(err_msg, sizeof(err_msg), "%s", m);
}

static bool step_hook(void) { return stop_fn ? stop_fn() : false; }

void script_stop_check(bool (*fn)(void)) { stop_fn = fn; }

bool script_ready(void) { return ready; }

static const act_host_t *H(void) { return act_host(); }

static int cur_card(void) {
	const act_host_t *h = H();
	if (h->current) return h->current(h->u);
	return ctx_cur;
}

static void say(const char *a, const char *b) {
	const act_host_t *h = H();
	char m[200];
	snprintf(m, sizeof(m), "%s%s", a, b ? b : "");
	if (h->message) h->message(h->u, m);
}

// -- timers ----------------------------------------------------------------------------

#define TIMERS 24
static struct {
	bool		used;
	int			id;
	uint32_t	due, period;	// ticks; period 0: once
	int			card;			// -1: the deck's
} timers[TIMERS];
static int next_id = 1;
static int timer_scope = -1;	// the card a new timer belongs to

static uint32_t now(void) {
	const act_host_t *h = H();
	return h->now ? h->now(h->u) : 0;
}

static uint32_t ms_to_ticks(double ms) {
	const act_host_t *h = H();
	uint32_t hz = h->tick_hz ? h->tick_hz : 1000;
	uint32_t t;
	if (ms < 1) ms = 1;
	t = (uint32_t)(ms * hz / 1000.0 + 0.5);
	return t ? t : 1;
}

bool script_timers(void) {
	int i;
	for (i = 0; i < TIMERS; i++) if (timers[i].used) return true;
	return false;
}

// -- the builtins ------------------------------------------------------------------------

static const char *arg_str(ms_val *args, int n) {
	ms_val *v = args;
	while (n-- > 0 && ms_is_pair(v)) v = ms_cdr(v);
	if (!ms_is_pair(v) || !ms_is_str(ms_car(v))) return NULL;
	return ms_str_val(ms_car(v));
}

static ms_val *mk_strc(const char *s) {
	size_t n = strlen(s);
	char *c = malloc(n + 1);
	if (!c) return ms_mk_bool(false);
	memcpy(c, s, n + 1);
	return ms_mk_str(c);
}

// A Scheme value as one argument of a verb's form: a string quoted, a
// symbol quoted with ', a number as it is.
static bool form_arg(ms_val *v, char *out, int cap) {
	char *s = ms_to_string(v, true);
	if (!s) return false;
	if (ms_is_str(v) || ms_is_num(v)) snprintf(out, (size_t)cap, " %s", s);
	else snprintf(out, (size_t)cap, " '%s", s);
	free(s);
	return true;
}

// (verb arg...) through act.h's verbs, on the card shown now.
static ms_val *verb(const char *v, ms_val *args, int nargs) {
	char f[400], a[200];
	int k, i;
	k = snprintf(f, sizeof(f), "(%s", v);
	for (i = 0; i < nargs && ms_is_pair(args); i++, args = ms_cdr(args)) {
		if (!form_arg(ms_car(args), a, sizeof(a))) return ms_mk_bool(false);
		k += snprintf(f + k, sizeof(f) - (size_t)k, "%s", a);
	}
	snprintf(f + k, sizeof(f) - (size_t)k, ")");
	if (!ctx_deck) return ms_mk_bool(false);
	return ms_mk_bool(act_do(ctx_deck, cur_card(), f));
}

//: (go 'next) / 'prev 'first 'last 'back, (go 3), (go "name")
static ms_val *s_go(ms_val *a) { return verb("go", a, 1); }
static ms_val *s_show(ms_val *a) { return verb("show", a, 1); }
static ms_val *s_hide(ms_val *a) { return verb("hide", a, 1); }
static ms_val *s_toggle(ms_val *a) { return verb("toggle", a, 1); }
static ms_val *s_set_text(ms_val *a) {
	// any value as its text: (set-text "sum" (+ 1 2))
	const char *n = arg_str(a, 0);
	obj_t *o;
	char *s;
	if (!n || !ms_is_pair(ms_cdr(a))) return ms_mk_bool(false);
	o = act_find(ctx_deck, cur_card(), n);
	if (!o) { say("no object called ", n); return ms_mk_bool(false); }
	s = ms_is_str(ms_car(ms_cdr(a))) ? NULL : ms_to_string(ms_car(ms_cdr(a)), false);
	{
		const char *t = s ? s : ms_str_val(ms_car(ms_cdr(a)));
		obj_set_text(o, t, (uint32_t)strlen(t));
	}
	free(s);
	if (H()->changed) H()->changed(H()->u);
	return ms_mk_bool(true);
}
static ms_val *s_set_value(ms_val *a) { return verb("set-value", a, 2); }
static ms_val *s_speak(ms_val *a) {
	char *s;
	if (!ms_is_pair(a)) return ms_mk_bool(false);
	s = ms_is_str(ms_car(a)) ? NULL : ms_to_string(ms_car(a), false);
	if (H()->speak) H()->speak(H()->u, s ? s : ms_str_val(ms_car(a)));
	free(s);
	return ms_mk_bool(true);
}
static ms_val *s_speak_text(ms_val *a) { return verb("speak-text", a, 1); }
static ms_val *s_open(ms_val *a) { return verb("open", a, 1); }
static ms_val *s_beep(ms_val *a) { (void)a; return verb("beep", a, 0); }

static ms_val *s_get_text(ms_val *a) {
	const char *n = arg_str(a, 0);
	obj_t *o = n ? act_find(ctx_deck, cur_card(), n) : NULL;
	if (!o) { if (n) say("no object called ", n); return ms_mk_bool(false); }
	return mk_strc(o->text ? o->text : "");
}
static ms_val *s_get_value(ms_val *a) {
	const char *n = arg_str(a, 0);
	obj_t *o = n ? act_find(ctx_deck, cur_card(), n) : NULL;
	if (!o) { if (n) say("no object called ", n); return ms_mk_bool(false); }
	if (o->kind == OB_CHECK || o->kind == OB_RADIO) return ms_mk_bool(o->value != 0);
	if (o->kind == OB_LIST) {
		const char *s;
		uint32_t len;
		char b[160];
		if (!render_list_item(o, o->value, &s, &len)) return ms_mk_bool(false);
		if (len >= sizeof(b)) len = sizeof(b) - 1;
		memcpy(b, s, len);
		b[len] = 0;
		return mk_strc(b);
	}
	return ms_mk_num(o->value);
}
static ms_val *s_shown(ms_val *a) {
	const char *n = arg_str(a, 0);
	obj_t *o = n ? act_find(ctx_deck, cur_card(), n) : NULL;
	return ms_mk_bool(o && !(o->flags & OBF_HIDDEN));
}
static ms_val *s_card_name(ms_val *a) {
	int c = cur_card();
	(void)a;
	if (!ctx_deck || c < 0 || c >= ctx_deck->ncard) return ms_mk_bool(false);
	return mk_strc(ctx_deck->card[c]->name);
}
static ms_val *s_card_number(ms_val *a) { (void)a; return ms_mk_num(cur_card() + 1); }
static ms_val *s_card_count(ms_val *a) { (void)a; return ms_mk_num(ctx_deck ? ctx_deck->ncard : 0); }
static ms_val *s_deck_title(ms_val *a) { (void)a; return mk_strc(ctx_deck ? ctx_deck->title : ""); }

static ms_val *s_after(ms_val *a) {
	int i;
	double ms;
	bool rep;
	if (!ms_is_pair(a) || !ms_is_num(ms_car(a))) { say("after/every: a number of milliseconds", NULL); return ms_mk_bool(false); }
	ms = ms_num_val(ms_car(a));
	rep = ms_is_pair(ms_cdr(a)) && !ms_is_nil(ms_car(ms_cdr(a))) && ms_car(ms_cdr(a)) != ms_mk_bool(false);
	for (i = 0; i < TIMERS; i++) if (!timers[i].used) break;
	if (i == TIMERS) { say("too many timers (24)", NULL); return ms_mk_bool(false); }
	timers[i].used = true;
	timers[i].id = next_id++;
	timers[i].period = rep ? ms_to_ticks(ms) : 0;
	timers[i].due = now() + ms_to_ticks(ms);
	timers[i].card = timer_scope;
	return ms_mk_num(timers[i].id);
}
static ms_val *s_cancel(ms_val *a) {
	int i;
	if (!ms_is_pair(a) || !ms_is_num(ms_car(a))) return ms_mk_bool(false);
	for (i = 0; i < TIMERS; i++)
		if (timers[i].used && timers[i].id == (int)ms_num_val(ms_car(a))) { timers[i].used = false; return ms_mk_bool(true); }
	return ms_mk_bool(false);
}

// -- starting --------------------------------------------------------------------------

// Defined in ms.c's -DLIX build: the stdlib compiled in.
void ms_init_lix(bool stdlib);

bool script_start(void) {
	if (ready) return true;
	if (failed) return false;
	ms_log_hook = log_hook;
	ms_panic_before_try();
	if (setjmp(ms_panic_recovery) == 0) {
		ms_init_lix(true);
		zapi_register();
		ms_def_builtin("go", s_go);
		ms_def_builtin("show", s_show);
		ms_def_builtin("hide", s_hide);
		ms_def_builtin("toggle", s_toggle);
		ms_def_builtin("set-text", s_set_text);
		ms_def_builtin("set-value", s_set_value);
		ms_def_builtin("get-text", s_get_text);
		ms_def_builtin("get-value", s_get_value);
		ms_def_builtin("shown?", s_shown);
		ms_def_builtin("speak", s_speak);
		ms_def_builtin("speak-text", s_speak_text);
		ms_def_builtin("open", s_open);
		ms_def_builtin("beep", s_beep);
		ms_def_builtin("card-name", s_card_name);
		ms_def_builtin("card-number", s_card_number);
		ms_def_builtin("card-count", s_card_count);
		ms_def_builtin("deck-title", s_deck_title);
		ms_def_builtin("%slide-after", s_after);
		ms_def_builtin("%slide-cancel", s_cancel);
		ms_load_string(slide_scm, ms_global_env);
		ready = true;
	} else {
		ms_panic_after_recover();
		failed = true;
	}
	ms_panic_disarm();
	if (ready) ms_step_hook = step_hook;
	return ready;
}

// -- evaluating ----------------------------------------------------------------------------

static const char *skip_blank(const char *p) {
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
		if (*p == ';') { while (*p && *p != '\n') p++; continue; }
		return p;
	}
}

// Work asked for while a script runs (a card's action after its (go
// ...)): done when the outer evaluation has finished, never inside it
// -- ms has one panic-recovery point.
#define LATER_MAX 8
static struct { deck_t *d; int cur; char *src; int kind; } later[LATER_MAX];
static int nlater;
enum { L_EVAL = 0, L_CARD };

static void defer(deck_t *d, int cur, const char *src, int kind) {
	if (nlater == LATER_MAX) { say("too much happening at once: some left undone", NULL); return; }
	later[nlater].d = d;
	later[nlater].cur = cur;
	later[nlater].src = src ? pool_strdup(src) : NULL;
	later[nlater].kind = kind;
	nlater++;
}

static void run_later(void);

static bool eval_now(deck_t *d, int cur, const char *src, char *out, int cap_out) {
	z_stdout_hook_t saved_hook = z_stdout_hook;
	bool ok = true;
	char *printed = NULL;
	ctx_deck = d;
	ctx_cur = cur;
	err_msg[0] = 0;
	cap_len = 0;
	cap[0] = 0;
	z_stdout_hook = cap_hook;
	depth++;
	ms_panic_before_try();
	if (setjmp(ms_panic_recovery) == 0) {
		const char *p = skip_blank(src);
		ms_val *last = NULL;
		while (*p) {
			ms_val *form = ms_read(&p);
			if (!form) break;
			last = ms_eval(form, ms_global_env);
			p = skip_blank(p);
		}
		if (out && last) printed = ms_to_string(last, true);
	} else {
		ms_panic_after_recover();
		ok = false;
	}
	ms_panic_disarm();
	depth--;
	fflush(stdout);
	z_stdout_hook = saved_hook;
	if (err_msg[0]) ok = false;
	if (out && cap_out > 0) {
		if (!ok) snprintf(out, (size_t)cap_out, "%s", err_msg[0] ? err_msg : "stopped");
		else if (cap[0]) snprintf(out, (size_t)cap_out, "%s", cap);
		else snprintf(out, (size_t)cap_out, "%s", printed ? printed : "");
	}
	free(printed);
	return ok;
}

bool script_eval(deck_t *d, int cur, const char *src, char *out, int cap_out) {
	bool ok;
	if (!src || !*src) return true;
	if (depth) {
		// inside a script: afterwards
		defer(d, cur, src, L_EVAL);
		if (out && cap_out) out[0] = 0;
		return true;
	}
	if (!script_start()) {
		if (out && cap_out) snprintf(out, (size_t)cap_out, "Scheme could not start (no memory)");
		return false;
	}
	ok = eval_now(d, cur, src, out, cap_out);
	if (!ok && !out) say("Script: ", err_msg[0] ? err_msg : "stopped");
	run_later();
	return ok;
}

static void card_script(deck_t *d, int cur);

static void run_later(void) {
	while (nlater) {
		deck_t *d = later[0].d;
		int cur = later[0].cur, kind = later[0].kind;
		char *src = later[0].src;
		memmove(later, later + 1, sizeof(later[0]) * (size_t)(nlater - 1));
		nlater--;
		if (kind == L_CARD) card_script(d, cur);
		else if (src && !eval_now(d, cur, src, NULL, 0)) say("Script: ", err_msg[0] ? err_msg : "stopped");
		pool_free(src);
	}
}

// -- the deck, cards, clicks, timers -----------------------------------------------------

void script_stop_timers(void) {
	int i;
	for (i = 0; i < TIMERS; i++) timers[i].used = false;
	if (ready && !depth) eval_now(ctx_deck, ctx_cur, "(set! %slide-timers '())", NULL, 0);
}

void script_deck(deck_t *d, int cur) {
	int i;
	for (i = 0; i < TIMERS; i++) timers[i].used = false;
	nlater = 0;
	ctx_deck = d;
	ctx_cur = cur;
	if (!d->script || !*d->script) {
		if (ready && !depth) eval_now(d, cur, "(%slide-reset)", NULL, 0);
		return;
	}
	if (!script_start()) return;
	eval_now(d, cur, "(%slide-reset)", NULL, 0);
	timer_scope = -1;
	if (!eval_now(d, cur, d->script, NULL, 0)) say("The deck's script: ", err_msg[0] ? err_msg : "stopped");
	run_later();
}

static void card_script(deck_t *d, int cur) {
	card_t *c;
	int i;
	if (!d || cur < 0 || cur >= d->ncard) return;
	// the card left takes its timers with it
	for (i = 0; i < TIMERS; i++) if (timers[i].used && timers[i].card >= 0 && timers[i].card != cur) timers[i].used = false;
	c = d->card[cur];
	if (!c->script || !*c->script) return;
	if (!script_start()) return;
	timer_scope = cur;
	if (!eval_now(d, cur, c->script, NULL, 0)) {
		char m[64];
		snprintf(m, sizeof(m), "Card %s's script: ", c->name);
		say(m, err_msg[0] ? err_msg : "stopped");
	}
	timer_scope = cur;
}

void script_card(deck_t *d, int cur) {
	if (depth) { defer(d, cur, NULL, L_CARD); return; }
	card_script(d, cur);
	run_later();
}

void script_clicked(deck_t *d, int cur, obj_t *o) {
	char f[80], q[40];
	int i, k = 0;
	if (!ready || !o->name[0]) return;
	// the name as a Scheme string
	for (i = 0; o->name[i] && k < (int)sizeof(q) - 2; i++) {
		if (o->name[i] == '"' || o->name[i] == '\\') q[k++] = '\\';
		q[k++] = o->name[i];
	}
	q[k] = 0;
	snprintf(f, sizeof(f), "(%%slide-click \"%s\")", q);
	timer_scope = cur;
	script_eval(d, cur, f, NULL, 0);
}

void script_tick(void) {
	int i;
	uint32_t t = now();
	if (!ready || depth) return;
	for (i = 0; i < TIMERS; i++) {
		char f[48];
		int id;
		if (!timers[i].used || (int32_t)(t - timers[i].due) < 0) continue;
		id = timers[i].id;
		if (timers[i].period) {
			timers[i].due += timers[i].period;
			if ((int32_t)(t - timers[i].due) > 0) timers[i].due = t + timers[i].period;	// fell behind: no catching up
		} else {
			timers[i].used = false;
		}
		timer_scope = timers[i].card;
		snprintf(f, sizeof(f), "(%%slide-timer-fire %d)", id);
		if (!eval_now(ctx_deck, cur_card(), f, NULL, 0)) {
			say("A timer's script: ", err_msg[0] ? err_msg : "stopped");
			timers[i].used = false;			// a failing (every ...) stops
		}
		if (!timers[i].used) {
			snprintf(f, sizeof(f), "(%%slide-timer-forget %d)", id);
			eval_now(ctx_deck, cur_card(), f, NULL, 0);
		}
		run_later();
	}
}

bool script_check(const char *src, char *err, int cap_out) {
	// balanced parentheses, closed strings: enough to know it reads
	int depth_p = 0, line = 1;
	bool in_str = false;
	const char *p;
	for (p = src; *p; p++) {
		if (*p == '\n') line++;
		if (in_str) {
			if (*p == '\\' && p[1]) { p++; continue; }
			if (*p == '"') in_str = false;
			continue;
		}
		if (*p == ';') { while (*p && *p != '\n') p++; if (!*p) break; line++; continue; }
		if (*p == '"') in_str = true;
		else if (*p == '(') depth_p++;
		else if (*p == ')' && --depth_p < 0) {
			snprintf(err, (size_t)cap_out, "line %d: a ) too many", line);
			return false;
		}
	}
	if (in_str) { snprintf(err, (size_t)cap_out, "a string is not closed"); return false; }
	if (depth_p > 0) { snprintf(err, (size_t)cap_out, "%d ( not closed", depth_p); return false; }
	return true;
}

bool script_window_msg(void *mp) {
	z_msg_t *m = mp;
	char expr[96];
	int conn, id = -1;
	if (!ready || m->obj.type != Z_UINT32) return false;
	switch (m->subject) {
	case Z_WM_SET_CLIP:
	case Z_WM_WINDOW_MOVED:
		return zapi_win_msg(m);
	case Z_WM_REDRAW: id = (int)Z_WM_UNPACK_ID(m->obj.val.uint32); break;
	case Z_WM_CLOSE: id = (int)m->obj.val.uint32; break;
	case Z_WM_KEY: case Z_WM_MOUSE: id = (int)m->tag; break;
	default: return false;
	}
	if (!zapi_win_owns(id)) return false;
	if (zapi_win_event(m, expr, sizeof(expr), &conn)) {
		script_eval(ctx_deck, cur_card(), expr, NULL, 0);
		zapi_win_event_done();
	}
	return true;
}
