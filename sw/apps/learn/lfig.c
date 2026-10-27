/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- figures: Scheme in a sandbox, a display list, and drawing it.
 * lfig.h; the vocabulary is docs/learn_figures.md.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <setjmp.h>

#include "lfig.h"
#include "lmathdraw.h"
#include "lsx.h"
#include "zapi/ms_api.h"

#include "figlib.h"		// figlib_scm: the figure library, from figlib.scm

lg_fig_t LG;

const char *const lg_role_name[LG_ROLES] = {
	"none", "ink", "paper", "warm", "cool", "good", "bad",
	"accent", "sky", "earth", "plant", "grey",
};

static bool started, failed;
static uint32_t (*clock_ms)(void);
static uint32_t deadline;
static bool timed;

void lg_set_clock(uint32_t (*ms)(void)) { clock_ms = ms; }

// -- Scheme's side --

static void log_hook(int level, const char *m) {
	if (level == 0) return;
	if (!LG.frame_err[0]) snprintf(LG.frame_err, sizeof LG.frame_err, "%s", m);
}

static bool step_hook(void) {
	return timed && clock_ms && (int32_t)(clock_ms() - deadline) > 0;
}

static void mistake(const char *fmt, const char *a) {
	if (LG.frame_err[0]) return;
	snprintf(LG.frame_err, sizeof LG.frame_err, fmt, a ? a : "");
}

// a symbol's name (or a boolean's #t/#f) into buf
static bool sym_name(ms_val *v, char *buf, int cap) {
	if (ms_is_str(v) || ms_is_num(v) || ms_is_pair(v) || ms_is_nil(v)) return false;
	char *s = ms_to_string(v, false);
	if (!s) return false;
	snprintf(buf, (size_t)cap, "%s", s);
	free(s);
	return true;
}

static int role_of(const char *n) {
	for (int i = 1; i < LG_ROLES; i++) if (!strcmp(n, lg_role_name[i])) return i;
	if (!strcmp(n, "gray")) return LG_GREY;
	return -1;
}

// what follows the fixed arguments: a role, numbers, an alignment, a size
typedef struct {
	int		role;			// -1 not given
	double	num[3];
	int		nnum;
	int		align;			// -1 not given
	int		size;			// LG_TINY.., 0 not given
	bool	bold;
	const char *str;		// a string among them (a slider's label)
} opts_t;

static bool options(ms_val *a, opts_t *o, const char *who) {
	memset(o, 0, sizeof *o);
	o->role = -1;
	o->align = -1;
	for (; ms_is_pair(a); a = ms_cdr(a)) {
		ms_val *v = ms_car(a);
		if (ms_is_num(v)) { if (o->nnum < 3) o->num[o->nnum++] = ms_num_val(v); continue; }
		if (ms_is_str(v)) { o->str = ms_str_val(v); continue; }
		char n[32];
		if (!sym_name(v, n, sizeof n)) { mistake("%s: a list where a word or number goes", who); return false; }
		if (!strcmp(n, "#f") || !strcmp(n, "#t")) continue;
		int r = role_of(n);
		if (r >= 0) { o->role = r; continue; }
		if (!strcmp(n, "left")) { o->align = LG_LEFT; continue; }
		if (!strcmp(n, "centre") || !strcmp(n, "center")) { o->align = LG_CENTRE; continue; }
		if (!strcmp(n, "right")) { o->align = LG_RIGHT; continue; }
		if (!strcmp(n, "tiny")) { o->size = LG_TINY; continue; }
		if (!strcmp(n, "small")) { o->size = LG_SMALL; continue; }
		if (!strcmp(n, "body")) { o->size = LG_BODY; continue; }
		if (!strcmp(n, "big")) { o->size = LG_BIG; continue; }
		if (!strcmp(n, "bold")) { o->bold = true; continue; }
		char m[96];
		snprintf(m, sizeof m, "%s: what is '%s? (a colour, left/centre/right, or a size)", who, n);
		mistake("%s", m);
		return false;
	}
	return true;
}

// n numbers from the front of a; returns the rest
static ms_val *numbers(ms_val *a, double *out, int n, const char *who) {
	for (int i = 0; i < n; i++) {
		if (!ms_is_pair(a) || !ms_is_num(ms_car(a))) {
			char m[80];
			snprintf(m, sizeof m, "%s needs %d numbers first", who, n);
			mistake("%s", m);
			return NULL;
		}
		out[i] = ms_num_val(ms_car(a));
		a = ms_cdr(a);
	}
	return a;
}

static void bounds(double x, double y) {
	if (!isfinite(x) || !isfinite(y)) { mistake("a position that is not a number (a division by zero?)", NULL); return; }
	if (x < -0.5 || y < -0.5 || x > LG_W + 0.5 || y > LG_H + 0.5) LG.outside = true;
}

static lg_op_t *add(int kind, int role) {
	if (LG.nops >= LG_MAX_OPS) { LG.full = true; return NULL; }
	lg_op_t *o = &LG.op[LG.nops++];
	memset(o, 0, sizeof *o);
	o->kind = (uint8_t)kind;
	o->role = (uint8_t)role;
	return o;
}

static int add_text(const char *s) {
	int n = (int)strlen(s);
	if (LG.ntext + n + 1 > LG_MAX_TEXT) { LG.full = true; return -1; }
	int off = LG.ntext;
	memcpy(LG.text + off, s, (size_t)n + 1);
	LG.ntext += n + 1;
	return off;
}

static ms_val *T(void) { return ms_mk_bool(true); }
static ms_val *F(void) { return ms_mk_bool(false); }

// (line x0 y0 x1 y1 [role] [width])  and arrow
static ms_val *two_ends(ms_val *a, int kind, const char *who) {
	double v[4];
	opts_t o;
	if (!(a = numbers(a, v, 4, who)) && LG.frame_err[0]) return F();
	if (!options(a, &o, who)) return F();
	lg_op_t *p = add(kind, o.role < 0 ? LG_INK : o.role);
	if (!p) return F();
	for (int i = 0; i < 4; i++) p->a[i] = (float)v[i];
	p->a[4] = (float)(o.nnum ? o.num[0] : 1.5);
	bounds(v[0], v[1]);
	bounds(v[2], v[3]);
	return T();
}
static ms_val *s_line(ms_val *a) { return two_ends(a, LG_OP_LINE, "line"); }
static ms_val *s_arrow(ms_val *a) { return two_ends(a, LG_OP_ARROW, "arrow"); }

// (rect x y w h [role]) filled; (frame x y w h [role] [width])
static ms_val *box(ms_val *a, int kind, const char *who) {
	double v[4];
	opts_t o;
	if (!(a = numbers(a, v, 4, who)) && LG.frame_err[0]) return F();
	if (!options(a, &o, who)) return F();
	lg_op_t *p = add(kind, o.role < 0 ? LG_INK : o.role);
	if (!p) return F();
	for (int i = 0; i < 4; i++) p->a[i] = (float)v[i];
	p->a[4] = (float)(o.nnum ? o.num[0] : 1.5);
	bounds(v[0], v[1]);
	bounds(v[0] + v[2], v[1] + v[3]);
	return T();
}
static ms_val *s_rect(ms_val *a) { return box(a, LG_OP_RECT, "rect"); }
static ms_val *s_frame(ms_val *a) { return box(a, LG_OP_FRAME, "frame"); }

// (circle cx cy r [role]) filled; (ring cx cy r [role] [width]); (dot x y [role])
static ms_val *round_one(ms_val *a, int kind, const char *who, int nfix) {
	double v[3] = { 0, 0, 2.5 };
	opts_t o;
	if (!(a = numbers(a, v, nfix, who)) && LG.frame_err[0]) return F();
	if (!options(a, &o, who)) return F();
	lg_op_t *p = add(kind, o.role < 0 ? LG_INK : o.role);
	if (!p) return F();
	for (int i = 0; i < 3; i++) p->a[i] = (float)v[i];
	p->a[3] = (float)(o.nnum ? o.num[0] : 1.5);
	bounds(v[0] - v[2], v[1] - v[2]);
	bounds(v[0] + v[2], v[1] + v[2]);
	return T();
}
static ms_val *s_circle(ms_val *a) { return round_one(a, LG_OP_DISC, "circle", 3); }
static ms_val *s_ring(ms_val *a) { return round_one(a, LG_OP_RING, "ring", 3); }
static ms_val *s_dot(ms_val *a) { return round_one(a, LG_OP_DISC, "dot", 2); }

// (arc cx cy r from to [role] [width]): degrees, 0 to the right, counter-clockwise
static ms_val *s_arc(ms_val *a) {
	double v[5];
	opts_t o;
	if (!(a = numbers(a, v, 5, "arc")) && LG.frame_err[0]) return F();
	if (!options(a, &o, "arc")) return F();
	lg_op_t *p = add(LG_OP_ARC, o.role < 0 ? LG_INK : o.role);
	if (!p) return F();
	for (int i = 0; i < 5; i++) p->a[i] = (float)v[i];
	p->a[5] = (float)(o.nnum ? o.num[0] : 1.5);
	for (int i = 0; i <= 8; i++) {		// where the arc goes, not its whole circle
		double d = (v[3] + (v[4] - v[3]) * i / 8) * M_PI / 180;
		bounds(v[0] + v[2] * cos(d), v[1] - v[2] * sin(d));
	}
	return T();
}

// (poly '(x0 y0 x1 y1 ...) [role]) filled; (path '(...) [role] [width]) open
static ms_val *points(ms_val *a, int kind, const char *who) {
	if (!ms_is_pair(a) || !(ms_is_pair(ms_car(a)) || ms_is_nil(ms_car(a)))) {
		mistake("%s needs a list of numbers: x y x y ...", who);
		return F();
	}
	ms_val *l = ms_car(a);
	opts_t o;
	if (!options(ms_cdr(a), &o, who)) return F();
	int off = LG.npts, n = 0;
	for (; ms_is_pair(l); l = ms_cdr(l)) {
		ms_val *x = ms_car(l);
		if (!ms_is_num(x)) { mistake("%s: the list holds something that is not a number", who); LG.npts = off; return F(); }
		if (LG.npts >= LG_MAX_PTS) { LG.full = true; LG.npts = off; return F(); }
		LG.pts[LG.npts++] = (float)ms_num_val(x);
		n++;
	}
	if (n & 1) { mistake("%s: an x without its y", who); LG.npts = off; return F(); }
	if (n < 4) { LG.npts = off; return T(); }
	lg_op_t *p = add(kind, o.role < 0 ? LG_INK : o.role);
	if (!p) { LG.npts = off; return F(); }
	p->off = (uint16_t)off;
	p->n = (uint16_t)(n / 2);
	p->a[0] = (float)(o.nnum ? o.num[0] : 1.5);
	for (int i = 0; i < n; i += 2) bounds(LG.pts[off + i], LG.pts[off + i + 1]);
	return T();
}
static ms_val *s_poly(ms_val *a) { return points(a, LG_OP_POLY, "poly"); }
static ms_val *s_path(ms_val *a) { return points(a, LG_OP_PATH, "path"); }

// (text x y "words" [role] [size] [align] [bold]); (math x y "3/4" [size] ...)
static void num_text(double v, char *buf, int cap) {
	if (v == floor(v) && fabs(v) < 1e15) snprintf(buf, (size_t)cap, "%.0f", v);
	else {
		snprintf(buf, (size_t)cap, "%.4f", v);
		char *e = buf + strlen(buf) - 1;
		while (e > buf && *e == '0') *e-- = 0;
		if (*e == '.') *e = 0;
	}
}

static ms_val *words(ms_val *a, int kind, const char *who) {
	double v[2];
	if (!(a = numbers(a, v, 2, who)) && LG.frame_err[0]) return F();
	if (!ms_is_pair(a)) { mistake("%s needs its words after x and y", who); return F(); }
	char nb[40];
	const char *s;
	ms_val *w = ms_car(a);
	if (ms_is_str(w)) s = ms_str_val(w);
	else if (ms_is_num(w)) { num_text(ms_num_val(w), nb, sizeof nb); s = nb; }
	else { mistake("%s: the words go in quotes", who); return F(); }
	opts_t o;
	if (!options(ms_cdr(a), &o, who)) return F();
	lg_op_t *p = add(kind, o.role < 0 ? LG_INK : o.role);
	if (!p) return F();
	int off = add_text(s);
	if (off < 0) { LG.nops--; return F(); }
	p->off = (uint16_t)off;
	p->a[0] = (float)v[0];
	p->a[1] = (float)v[1];
	p->align = (uint8_t)((o.align < 0 ? (kind == LG_OP_MATH ? LG_CENTRE : LG_LEFT) : o.align) | (o.bold ? 4 : 0));
	if (o.nnum) p->size = (uint8_t)(o.num[0] < 6 ? 6 : o.num[0] > 120 ? 120 : o.num[0]);
	else p->size = (uint8_t)(o.size ? o.size : kind == LG_OP_MATH ? 20 : LG_SMALL);
	// where the words reach: the faces are fixed-width (5, 7, 9, 10
	// units); serif about 0.55 of its size a letter. learncheck says so
	// when they go past the figure's edge.
	if (kind == LG_OP_TEXT) {
		int n = 0;
		for (const char *q = s; *q; q++) if (((unsigned char)*q & 0xC0) != 0x80) n++;
		double cw = p->size == LG_TINY ? 5 : p->size == LG_SMALL ? 7 : p->size == LG_BODY ? 9 :
			p->size == LG_BIG ? 10 : p->size * 0.55, w = n * cw;
		double asc = p->size <= LG_BIG ? (p->size == LG_TINY ? 7 : p->size == LG_SMALL ? 10 : p->size == LG_BODY ? 12 : 15) : p->size * 0.75;
		double x0 = (p->align & 3) == LG_CENTRE ? v[0] - w / 2 : (p->align & 3) == LG_RIGHT ? v[0] - w : v[0];
		bool was = LG.outside;
		LG.outside = false;
		bounds(x0, v[1] - asc);
		bounds(x0 + w, v[1]);
		if (LG.outside) LG.cut = true;
		LG.outside |= was;
	} else bounds(v[0], v[1]);
	return T();
}
static ms_val *s_text(ms_val *a) { return words(a, LG_OP_TEXT, "text"); }
static ms_val *s_math(ms_val *a) { return words(a, LG_OP_MATH, "math"); }

// -- controls --

static lg_ctl_t *control(ms_val **a, int kind, const char *who) {
	char n[32];
	if (!ms_is_pair(*a) || !sym_name(ms_car(*a), n, sizeof n)) {
		mistake("%s needs a name first: (slider 'length ...)", who);
		return NULL;
	}
	*a = ms_cdr(*a);
	for (int i = 0; i < LG.nctl; i++)
		if (!strcmp(LG.ctl[i].name, n)) {
			if (LG.ctl[i].kind != kind) { mistake("two controls called %s", n); return NULL; }
			LG.ctl[i].used = true;
			return &LG.ctl[i];
		}
	if (LG.nctl >= LG_MAX_CTL) { mistake("%s: at most 6 controls in a figure", who); return NULL; }
	lg_ctl_t *c = &LG.ctl[LG.nctl++];
	memset(c, 0, sizeof *c);
	c->kind = (uint8_t)kind;
	snprintf(c->name, sizeof c->name, "%.19s", n);
	c->used = true;
	c->value = 1e300;		// not set yet
	return c;
}

// (slider 'name min max [start] ["label"] [step]) -> its value
static ms_val *s_slider(ms_val *a) {
	lg_ctl_t *c = control(&a, LG_SLIDER, "slider");
	if (!c) return ms_mk_num(0);
	double v[2];
	if (!(a = numbers(a, v, 2, "slider (after its name)")) && LG.frame_err[0]) return ms_mk_num(0);
	opts_t o;
	options(a, &o, "slider");
	if (c->value == 1e300) {
		c->min = v[0] < v[1] ? v[0] : v[1];
		c->max = v[0] < v[1] ? v[1] : v[0];
		c->step = o.nnum > 1 ? fabs(o.num[1]) : 0;
		c->value = o.nnum ? o.num[0] : c->min;
		if (c->value < c->min) c->value = c->min;
		if (c->value > c->max) c->value = c->max;
		snprintf(c->label, sizeof c->label, "%s", o.str ? o.str : c->name);
	}
	return ms_mk_num(c->value);
}

// (toggle 'name ["label"] [#t]) -> #t or #f
static ms_val *s_toggle(ms_val *a) {
	lg_ctl_t *c = control(&a, LG_TOGGLE, "toggle");
	if (!c) return F();
	if (c->value == 1e300) {
		const char *label = NULL;
		bool on = false;
		for (; ms_is_pair(a); a = ms_cdr(a)) {
			ms_val *v = ms_car(a);
			char n[8];
			if (ms_is_str(v)) label = ms_str_val(v);
			else if (sym_name(v, n, sizeof n) && !strcmp(n, "#t")) on = true;
		}
		c->value = on;
		snprintf(c->label, sizeof c->label, "%s", label ? label : c->name);
	}
	return ms_mk_bool(c->value != 0);
}

// (choice 'name '("a" "b" "c") ["label"]) -> the index chosen, from 0
static ms_val *s_choice(ms_val *a) {
	lg_ctl_t *c = control(&a, LG_CHOICE, "choice");
	if (!c) return ms_mk_num(0);
	if (c->value == 1e300) {
		if (!ms_is_pair(a) || !ms_is_pair(ms_car(a))) { mistake("choice needs a list of words: '(\"a\" \"b\")", NULL); c->value = 0; return ms_mk_num(0); }
		for (ms_val *l = ms_car(a); ms_is_pair(l) && c->nopt < LG_MAX_OPTS; l = ms_cdr(l))
			if (ms_is_str(ms_car(l))) snprintf(c->opt[c->nopt++], sizeof c->opt[0], "%s", ms_str_val(ms_car(l)));
		a = ms_cdr(a);
		snprintf(c->label, sizeof c->label, "%s", ms_is_pair(a) && ms_is_str(ms_car(a)) ? ms_str_val(ms_car(a)) : c->name);
		c->value = 0;
		c->min = 0;
		c->max = c->nopt ? c->nopt - 1 : 0;
		c->step = 1;
	}
	return ms_mk_num(c->value);
}

// (%drag 'name x y [r]): a point the learner can move; (%ctl-x 'name), (%ctl-y 'name)
static ms_val *s_drag(ms_val *a) {
	lg_ctl_t *c = control(&a, LG_DRAG, "drag");
	if (!c) return F();
	double v[2];
	if (!(a = numbers(a, v, 2, "drag (after its name)")) && LG.frame_err[0]) return F();
	if (c->value == 1e300) {
		c->value = 0;
		c->x = v[0];
		c->y = v[1];
		c->r = ms_is_pair(a) && ms_is_num(ms_car(a)) ? ms_num_val(ms_car(a)) : 6;
		snprintf(c->label, sizeof c->label, "%s", c->name);
	}
	return T();
}

static lg_ctl_t *named(ms_val *a) {
	char n[32];
	if (!ms_is_pair(a) || !sym_name(ms_car(a), n, sizeof n)) return NULL;
	for (int i = 0; i < LG.nctl; i++) if (!strcmp(LG.ctl[i].name, n)) return &LG.ctl[i];
	return NULL;
}
static ms_val *s_ctl_x(ms_val *a) { lg_ctl_t *c = named(a); return ms_mk_num(c ? c->x : 0); }
static ms_val *s_ctl_y(ms_val *a) { lg_ctl_t *c = named(a); return ms_mk_num(c ? c->y : 0); }

// -- time and numbers --

static double frame_t;
// (still forms...), from figlib.scm: (if (%still-begin) (begin forms... (%still-end)))
// The forms run once, and again only when a control changes; what they
// drew stays at the front of the list. Only at the start of a figure:
// anything drawn before them, and they run every frame.
static int still_from;
static ms_val *s_still_begin(ms_val *a) {
	(void)a;
	LG.still_seen = true;
	if (LG.nstill && LG.still_gen == LG.ctl_gen) return F();		// kept: skip the forms
	still_from = LG.nops ? -1 : 0;				// not first: drawn every frame
	return T();
}
static ms_val *s_still_end(ms_val *a) {
	(void)a;
	if (still_from == 0 && !LG.full) {
		LG.nstill = LG.nops;
		LG.nstill_pts = LG.npts;
		LG.nstill_text = LG.ntext;
		LG.still_gen = LG.ctl_gen;
		LG.still_serial++;
	}
	still_from = -1;
	return T();
}

// (seconds): since the figure was shown; asking makes it a moving figure
static ms_val *s_seconds(ms_val *a) { (void)a; LG.animated = true; return ms_mk_num(frame_t); }

// (colour): this figure needs its colours to be read (a map, a chart
// whose colours carry meaning); without it a figure is drawn in black
// and white with a pattern a role, which reads the same
static ms_val *s_colour(ms_val *a) { (void)a; LG.colour = true; return ms_nil_val(); }

// (fmt x digits) -> a string with that many digits after the point
static ms_val *s_fmt(ms_val *a) {
	if (!ms_is_pair(a) || !ms_is_num(ms_car(a))) { mistake("fmt needs a number", NULL); return ms_mk_str(NULL); }
	double v = ms_num_val(ms_car(a));
	int d = ms_is_pair(ms_cdr(a)) && ms_is_num(ms_car(ms_cdr(a))) ? (int)ms_num_val(ms_car(ms_cdr(a))) : 0;
	if (d < 0) d = 0;
	if (d > 8) d = 8;
	char *s = malloc(48);
	if (!s) return ms_mk_str(NULL);
	snprintf(s, 48, "%.*f", d, v);
	if (!strcmp(s, "-0") || (s[0] == '-' && strspn(s + 1, "0.") == strlen(s + 1))) memmove(s, s + 1, strlen(s));
	return ms_mk_str(s);
}

static double arg1(ms_val *a, const char *who) {
	if (!ms_is_pair(a) || !ms_is_num(ms_car(a))) { mistake("%s needs a number", who); return 0; }
	return ms_num_val(ms_car(a));
}
static ms_val *s_sin(ms_val *a) { return ms_mk_num(sin(arg1(a, "sin"))); }
static ms_val *s_cos(ms_val *a) { return ms_mk_num(cos(arg1(a, "cos"))); }
static ms_val *s_tan(ms_val *a) { return ms_mk_num(tan(arg1(a, "tan"))); }
static ms_val *s_asin(ms_val *a) { return ms_mk_num(asin(arg1(a, "asin"))); }
static ms_val *s_acos(ms_val *a) { return ms_mk_num(acos(arg1(a, "acos"))); }
static ms_val *s_exp(ms_val *a) { return ms_mk_num(exp(arg1(a, "exp"))); }
static ms_val *s_log(ms_val *a) { return ms_mk_num(log(arg1(a, "log"))); }
static ms_val *s_atan(ms_val *a) {
	double y = arg1(a, "atan");
	if (ms_is_pair(a) && ms_is_pair(ms_cdr(a)) && ms_is_num(ms_car(ms_cdr(a))))
		return ms_mk_num(atan2(y, ms_num_val(ms_car(ms_cdr(a)))));
	return ms_mk_num(atan(y));
}

// the sandbox: what a figure may not use
static ms_val *s_not_here(ms_val *a) { (void)a; mistake("a figure cannot do that", NULL); return F(); }

// -- starting --

// Scheme is started once and kept, but a figure that runs it out of heap
// or protect stack leaves it unfit for the next one (every later figure
// then failed the same way), and symbols interned by one figure after
// another are never freed. So after such a failure, and every so many
// figures, the next figure starts with a fresh Scheme (ms_init clears
// the static heap); the cost is the library loaded again.
static bool restart_next;
static int loads_since_start;
#define LG_RESTART_EVERY 16

static bool unfit(const char *why) {
	return strstr(why, "heap") || strstr(why, "protect stack") || strstr(why, "memory");
}

bool lg_start(void) {
	if (started && (restart_next || loads_since_start >= LG_RESTART_EVERY)) {
		started = false;
		restart_next = false;
	}
	if (started) return true;
	loads_since_start = 0;
	if (failed) return false;
	ms_log_hook = log_hook;
	LG.frame_err[0] = 0;
	ms_panic_before_try();
	if (setjmp(ms_panic_recovery) == 0) {
		ms_init_lix(true);
		ms_def_builtin("exit", s_not_here);
		ms_def_builtin("read", s_not_here);
		ms_def_builtin("dump", s_not_here);
		ms_def_builtin("line", s_line);
		ms_def_builtin("arrow", s_arrow);
		ms_def_builtin("rect", s_rect);
		ms_def_builtin("frame", s_frame);
		ms_def_builtin("circle", s_circle);
		ms_def_builtin("ring", s_ring);
		ms_def_builtin("dot", s_dot);
		ms_def_builtin("arc", s_arc);
		ms_def_builtin("poly", s_poly);
		ms_def_builtin("path", s_path);
		ms_def_builtin("text", s_text);
		ms_def_builtin("math", s_math);
		ms_def_builtin("slider", s_slider);
		ms_def_builtin("toggle", s_toggle);
		ms_def_builtin("choice", s_choice);
		ms_def_builtin("%drag", s_drag);
		ms_def_builtin("%ctl-x", s_ctl_x);
		ms_def_builtin("%ctl-y", s_ctl_y);
		ms_def_builtin("seconds", s_seconds);
		ms_def_builtin("colour", s_colour);
		ms_def_builtin("color", s_colour);
		ms_def_builtin("fmt", s_fmt);
		ms_def_builtin("%still-begin", s_still_begin);
		ms_def_builtin("%still-end", s_still_end);
		ms_def_builtin("sin", s_sin);
		ms_def_builtin("cos", s_cos);
		ms_def_builtin("tan", s_tan);
		ms_def_builtin("asin", s_asin);
		ms_def_builtin("acos", s_acos);
		ms_def_builtin("atan", s_atan);
		ms_def_builtin("exp", s_exp);
		ms_def_builtin("log", s_log);
		ms_load_string(figlib_scm, ms_global_env);
		ms_load_string("(define %fig #f)", ms_global_env);
		started = true;
	} else {
		ms_panic_after_recover();
		failed = true;
	}
	ms_panic_disarm();
	if (started) ms_step_hook = step_hook;
	if (LG.frame_err[0]) {
		// the library itself is wrong: a mistake in learn, not in a course
		snprintf(LG.err, sizeof LG.err, "figure library: %.140s", LG.frame_err);
	}
	return started;
}

static void reset_list(void) {
	// the still shapes stay, while nothing they depend on has changed
	if (LG.nstill && (LG.still_gen != LG.ctl_gen || !LG.still_seen)) LG.nstill = LG.nstill_pts = LG.nstill_text = 0;
	LG.still_seen = false;
	LG.nops = LG.nstill;
	LG.npts = LG.nstill_pts;
	LG.ntext = LG.nstill_text;
	LG.full = false;
	LG.outside = false;
	LG.cut = false;
}

static bool mv_ok;		// (moved(), below)

void lg_unload(void) {
	mv_ok = false;
	LG.nstill = LG.nstill_pts = LG.nstill_text = 0;
	LG.still_serial++;
	reset_list();
	LG.nctl = 0;
	LG.loaded = false;
	LG.animated = false;
	LG.colour = false;
	LG.err[0] = 0;
	LG.frame_err[0] = 0;
	if (started) {
		ms_panic_before_try();
		if (setjmp(ms_panic_recovery) == 0) ms_load_string("(set! %fig #f)", ms_global_env);
		else ms_panic_after_recover();
		ms_panic_disarm();
	}
}

// -- the course's library, only what a figure uses --
//
// A course's lib/*.scm can hold many procedures; every one defined costs
// cells of the figure's heap (MS_HEAP_SIZE), and a course with several
// authors' libraries ran out of heap in every figure. So a figure gets
// only the library's top-level forms it needs: a (define NAME ...) or
// (define (NAME ...) ...) whose NAME the figure uses, or a form so taken
// uses, and every form that defines nothing (it might do anything).
// The order of the library is kept.

#define LIB_MAX_FORMS 600
#define LIB_NAME 48

static bool sym_char(char c) {
	return c && !strchr(" \t\r\n()'`,\";", c);
}

// the next top-level form of s from *at: its start and end; false at the end
static bool next_form(const char *s, int *at, int *a, int *b) {
	int i = *at;
	for (;;) {
		while (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') i++;
		if (s[i] == ';') { while (s[i] && s[i] != '\n') i++; continue; }
		break;
	}
	if (!s[i]) return false;
	*a = i;
	if (s[i] != '(') { while (sym_char(s[i]) || s[i] == '\'') i++; if (i == *a) i++; *b = i; *at = i; return true; }
	int depth = 0;
	for (; s[i]; i++) {
		char c = s[i];
		if (c == ';') { while (s[i] && s[i] != '\n') i++; if (!s[i]) break; continue; }
		if (c == '"') { for (i++; s[i] && s[i] != '"'; i++) if (s[i] == '\\' && s[i + 1]) i++; if (!s[i]) break; continue; }
		if (c == '(') depth++;
		else if (c == ')' && --depth == 0) { i++; break; }
	}
	*b = i;
	*at = i;
	return true;
}

// the name a form defines, or "" (it defines nothing)
static void form_name(const char *s, int a, int b, char *out) {
	out[0] = 0;
	if (strncmp(s + a, "(define", 7) || sym_char(s[a + 7])) return;
	int i = a + 7;
	while (i < b && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
	if (s[i] == '(') i++;
	int n = 0;
	while (i < b && sym_char(s[i]) && n < LIB_NAME - 1) out[n++] = s[i++];
	out[n] = 0;
}

// does s[a..b) use the symbol name (outside strings and comments)?
static bool uses(const char *s, int a, int b, const char *name) {
	size_t nl = strlen(name);
	for (int i = a; i < b; ) {
		char c = s[i];
		if (c == ';') { while (i < b && s[i] != '\n') i++; continue; }
		if (c == '"') { for (i++; i < b && s[i] != '"'; i++) if (s[i] == '\\') i++; i++; continue; }
		if (sym_char(c)) {
			int j = i;
			while (j < b && sym_char(s[j])) j++;
			if ((size_t)(j - i) == nl && !memcmp(s + i, name, nl)) return true;
			i = j;
			continue;
		}
		i++;
	}
	return false;
}

// lib's forms that src needs, in order, into a new string (malloc'd)
static char *lib_needed(const char *lib, const char *src) {
	static int fa[LIB_MAX_FORMS], fb[LIB_MAX_FORMS];
	static char names[LIB_MAX_FORMS][LIB_NAME];
	static bool take[LIB_MAX_FORMS];
	int n = 0, at = 0, a, b;
	while (n < LIB_MAX_FORMS && next_form(lib, &at, &a, &b)) {
		fa[n] = a; fb[n] = b;
		form_name(lib, a, b, names[n]);
		take[n] = !names[n][0];
		n++;
	}
	if (n >= LIB_MAX_FORMS) return NULL;		// too many to sort out: take it all
	// what the figure uses, then what that uses, until nothing more is taken
	int sl = (int)strlen(src);
	for (int i = 0; i < n; i++) if (!take[i] && uses(src, 0, sl, names[i])) take[i] = true;
	for (bool more = true; more; ) {
		more = false;
		for (int i = 0; i < n; i++) {
			if (take[i]) continue;
			for (int k = 0; k < n; k++)
				if (take[k] && uses(lib, fa[k], fb[k], names[i])) { take[i] = more = true; break; }
		}
	}
	size_t len = 1;
	for (int i = 0; i < n; i++) if (take[i]) len += (size_t)(fb[i] - fa[i]) + 1;
	char *out = malloc(len);
	if (!out) return NULL;
	size_t o = 0;
	for (int i = 0; i < n; i++)
		if (take[i]) { memcpy(out + o, lib + fa[i], (size_t)(fb[i] - fa[i])); o += (size_t)(fb[i] - fa[i]); out[o++] = '\n'; }
	out[o] = 0;
	return out;
}

// brackets and strings that close (ms reads what it can and stops silently)
static bool balanced(const char *s, char *err, int cap) {
	int depth = 0, line = 1;
	bool str = false;
	for (const char *p = s; *p; p++) {
		if (*p == '\n') line++;
		if (str) {
			if (*p == '\\' && p[1]) { p++; continue; }
			if (*p == '"') str = false;
			continue;
		}
		if (*p == ';') { while (p[1] && p[1] != '\n') p++; continue; }
		if (*p == '"') str = true;
		else if (*p == '(') depth++;
		else if (*p == ')' && --depth < 0) { snprintf(err, (size_t)cap, "line %d: a ) too many", line); return false; }
	}
	if (str) { snprintf(err, (size_t)cap, "a string is not closed"); return false; }
	if (depth) { snprintf(err, (size_t)cap, "%d ( not closed", depth); return false; }
	return true;
}

static bool load_once(const char *lib, const char *src);

bool lg_load(const char *lib, const char *src) {
	if (load_once(lib, src)) return true;
	if (!unfit(LG.err)) return false;
	restart_next = true;		// a fresh Scheme, then once more
	return load_once(lib, src);
}

static bool load_once(const char *lib, const char *src) {
	lg_unload();
	if (!lg_start()) { snprintf(LG.err, sizeof LG.err, "Scheme could not start"); return false; }
	loads_since_start++;
	if (lib && !balanced(lib, LG.err, sizeof LG.err)) {
		char m[160];
		snprintf(m, sizeof m, "the course's figure library: %.120s", LG.err);
		snprintf(LG.err, sizeof LG.err, "%s", m);
		return false;
	}
	if (!balanced(src, LG.err, sizeof LG.err)) return false;
	char *need = lib ? lib_needed(lib, src) : NULL;
	if (need) lib = need;
	size_t ll = lib ? strlen(lib) : 0, sl = strlen(src);
	char *all = malloc(ll + sl + 64);
	if (!all) { free(need); snprintf(LG.err, sizeof LG.err, "no memory for the figure"); return false; }
	// a closure: the figure's own definitions stay its own
	snprintf(all, ll + sl + 64, "(set! %%fig (lambda ()\n%s\n%s\n#t))", lib ? lib : "", src);
	free(need);
	LG.frame_err[0] = 0;
	volatile bool ok = true;
	ms_panic_before_try();
	if (setjmp(ms_panic_recovery) == 0) {
		ms_load_string(all, ms_global_env);
	} else {
		ms_panic_after_recover();
		ok = false;
	}
	ms_panic_disarm();
	free(all);
	if (LG.frame_err[0]) { snprintf(LG.err, sizeof LG.err, "%s", LG.frame_err); ok = false; }
	else if (!ok) snprintf(LG.err, sizeof LG.err, "the figure does not read");
	LG.loaded = ok;
	return ok;
}

// What changed since the frame before: the shapes after the still ones
// (and the rings of points to drag), where they were and where they are.
// All of it after a control changed, or the still shapes were drawn again.
static bool mv_ok;

static void box_add(float *b, float x0, float y0, float x1, float y1) {
	if (x0 < b[0]) b[0] = x0;
	if (y0 < b[1]) b[1] = y0;
	if (x1 > b[2]) b[2] = x1;
	if (y1 > b[3]) b[3] = y1;
}

// a shape's box in figure units, generous (its pen, a text's words)
static void op_box(const lg_op_t *o, float *b) {
	const float *a = o->a;
	switch (o->kind) {
	case LG_OP_LINE: case LG_OP_ARROW: {
		float w = a[4] + 8;
		box_add(b, (a[0] < a[2] ? a[0] : a[2]) - w, (a[1] < a[3] ? a[1] : a[3]) - w,
			(a[0] > a[2] ? a[0] : a[2]) + w, (a[1] > a[3] ? a[1] : a[3]) + w);
		break;
	}
	case LG_OP_RECT: case LG_OP_FRAME: box_add(b, a[0] - 2, a[1] - 2, a[0] + a[2] + 2, a[1] + a[3] + 2); break;
	case LG_OP_DISC: case LG_OP_RING: case LG_OP_ARC: {
		float r = a[2] + (o->kind == LG_OP_DISC ? 2 : a[o->kind == LG_OP_RING ? 3 : 5] + 2);
		box_add(b, a[0] - r, a[1] - r, a[0] + r, a[1] + r);
		break;
	}
	case LG_OP_POLY: case LG_OP_PATH: {
		const float *q = LG.pts + o->off;
		float w = (o->kind == LG_OP_PATH ? a[0] : 0) + 3;
		for (int i = 0; i < o->n; i++) box_add(b, q[2 * i] - w, q[2 * i + 1] - w, q[2 * i] + w, q[2 * i + 1] + w);
		break;
	}
	default: {		// words: as wide as they could be
		float px = o->size >= 6 ? o->size : o->size == LG_BIG ? 20 : 16;
		float w = (float)strlen(LG.text + o->off) * px * 0.75f + 8;
		float x0 = a[0], al = (float)(o->align & 3);
		if (al == LG_CENTRE) x0 -= w / 2;
		else if (al == LG_RIGHT) x0 -= w;
		box_add(b, x0 - 4, a[1] - px * 1.5f, x0 + w + 4, a[1] + px * 0.75f);
		break;
	}
	}
}

static uint32_t fnv(uint32_t h, const void *p, size_t n) {
	const uint8_t *b = (const uint8_t *)p;
	while (n--) h = (h ^ *b++) * 16777619u;
	return h;
}

// Each shape of the frame before and of this one, as a hash of all it
// is and its box: a shape that differs from the one in its place before
// is where the picture changed -- where it was, and where it is. Shapes
// that did not change (a still dial, a label, the rest of a triangle's
// figure) cost nothing to draw: the picture is drawn again only inside
// the box of what changed. The rings of points to drag come last.
#define MV_MAX (LG_MAX_OPS + LG_MAX_CTL)
static uint32_t mv_h[2][MV_MAX];
static float mv_b[2][MV_MAX][4];
static int mv_n[2], mv_cur;

static uint32_t op_hash(const lg_op_t *o) {
	uint32_t h = fnv(2166136261u, o, sizeof *o);
	if (o->kind == LG_OP_POLY || o->kind == LG_OP_PATH) h = fnv(h, LG.pts + o->off, (size_t)o->n * 2 * sizeof(float));
	if (o->kind == LG_OP_TEXT || o->kind == LG_OP_MATH) h = fnv(h, LG.text + o->off, strlen(LG.text + o->off));
	return h;
}

static void moved(uint32_t serial0) {
	int c = mv_cur ^ 1, p = mv_cur, n = 0;
	// the still shapes, not run again: as they were (no need to look)
	bool kept = mv_ok && serial0 == LG.still_serial;
	for (int i = 0; i < LG.nops && n < MV_MAX; i++, n++) {
		if (kept && i < LG.nstill && i < mv_n[p]) {
			mv_h[c][n] = mv_h[p][i];
			memcpy(mv_b[c][n], mv_b[p][i], sizeof mv_b[c][n]);
			continue;
		}
		mv_h[c][n] = op_hash(&LG.op[i]);
		float *bx = mv_b[c][n];
		bx[0] = bx[1] = 1e9f;
		bx[2] = bx[3] = -1e9f;
		op_box(&LG.op[i], bx);
	}
	for (int i = 0; i < LG.nctl && n < MV_MAX; i++) {
		const lg_ctl_t *k = &LG.ctl[i];
		if (k->kind != LG_DRAG || !k->used) continue;
		double xy[3] = { k->x, k->y, k->r };
		mv_h[c][n] = fnv(2166136261u, xy, sizeof xy);
		float r = (float)k->r + 6, *bx = mv_b[c][n];
		bx[0] = (float)k->x - r; bx[1] = (float)k->y - r; bx[2] = (float)k->x + r; bx[3] = (float)k->y + r;
		n++;
	}
	mv_n[c] = n;
	float d[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
	int m = n > mv_n[p] ? n : mv_n[p];
	for (int i = 0; mv_ok && i < m; i++) {
		bool in_new = i < n, in_old = i < mv_n[p];
		if (in_new && in_old && mv_h[c][i] == mv_h[p][i]) continue;
		if (in_old) box_add(d, mv_b[p][i][0], mv_b[p][i][1], mv_b[p][i][2], mv_b[p][i][3]);
		if (in_new) box_add(d, mv_b[c][i][0], mv_b[c][i][1], mv_b[c][i][2], mv_b[c][i][3]);
	}
	LG.dirty_all = !mv_ok;
	LG.unchanged = mv_ok && d[2] < d[0];
	memcpy(LG.dirty, d, sizeof d);
	mv_cur = c;
	mv_ok = true;
}

bool lg_dirty_px(int x, int y, int w, int h, int r[4]) {
	if (LG.dirty_all || LG.dirty[2] < LG.dirty[0]) return false;
	float s = (float)w / LG_W;
	r[0] = x + (int)floorf(LG.dirty[0] * s) - 1;
	r[1] = y + (int)floorf(LG.dirty[1] * s) - 1;
	r[2] = x + (int)ceilf(LG.dirty[2] * s) + 2;
	r[3] = y + (int)ceilf(LG.dirty[3] * s) + 2;
	if (r[0] < x) r[0] = x;
	if (r[1] < y) r[1] = y;
	if (r[2] > x + w) r[2] = x + w;
	if (r[3] > y + h) r[3] = y + h;
	return true;
}

bool lg_frame(double t, uint32_t budget_ms) {
	uint32_t serial0 = LG.still_serial;
	reset_list();
	LG.frames++;
	LG.frame_err[0] = 0;
	if (!LG.loaded) return false;
	for (int i = 0; i < LG.nctl; i++) LG.ctl[i].used = false;
	frame_t = t;
	volatile bool ok = true;
	uint32_t t0 = clock_ms ? clock_ms() : 0;
	deadline = clock_ms ? t0 + budget_ms : 0;
	timed = budget_ms > 0;
	ms_panic_before_try();
	if (setjmp(ms_panic_recovery) == 0) {
		const char *call = "(%fig)";
		ms_val *f = ms_read(&call);
		if (f) ms_eval(f, ms_global_env);
	} else {
		ms_panic_after_recover();
		ok = false;
		if (step_hook()) snprintf(LG.frame_err, sizeof LG.frame_err, "the figure took too long to draw");
		else if (!LG.frame_err[0]) snprintf(LG.frame_err, sizeof LG.frame_err, "the figure stopped");
	}
	ms_panic_disarm();
	if (!ok && unfit(LG.frame_err)) restart_next = true;		// the next figure starts afresh
	timed = false;
	LG.ms = clock_ms ? clock_ms() - t0 : 0;
	if (!ok) LG.nstill = LG.nstill_pts = LG.nstill_text = 0;		// (a frame cut short)
	moved(serial0);
	if (LG.full && !LG.frame_err[0])
		snprintf(LG.frame_err, sizeof LG.frame_err, "the figure draws more than a figure holds (%d shapes)", LG_MAX_OPS);
	return ok && !LG.frame_err[0];
}

// -- controls --

// (a control that is set to the value it has is not a change: nothing is drawn again)
void lg_ctl_set(int i, double v) {
	if (i < 0 || i >= LG.nctl) return;
	lg_ctl_t *c = &LG.ctl[i];
	if (c->kind == LG_TOGGLE) v = v != 0;
	else {
		if (v < c->min) v = c->min;
		if (v > c->max) v = c->max;
		if (c->step > 0) v = c->min + floor((v - c->min) / c->step + 0.5) * c->step;
		if (v > c->max) v = c->max;
	}
	if (v == c->value) return;
	c->value = v;
	LG.ctl_gen++;
}

void lg_ctl_nudge(int i, int dir) {
	if (i < 0 || i >= LG.nctl) return;
	lg_ctl_t *c = &LG.ctl[i];
	if (c->kind == LG_TOGGLE) { c->value = !c->value; LG.ctl_gen++; return; }
	if (c->kind == LG_DRAG) return;
	if (c->kind == LG_CHOICE) {
		int n = c->nopt ? c->nopt : 1;
		lg_ctl_set(i, (double)((((int)c->value + dir) % n + n) % n));
		return;
	}
	double s = c->step > 0 ? c->step : (c->max - c->min) / 20;
	lg_ctl_set(i, c->value + dir * s);
}

void lg_ctl_drag_to(int i, double x, double y) {
	if (i < 0 || i >= LG.nctl || LG.ctl[i].kind != LG_DRAG) return;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > LG_W) x = LG_W;
	if (y > LG_H) y = LG_H;
	if (x == LG.ctl[i].x && y == LG.ctl[i].y) return;
	LG.ctl_gen++;
	LG.ctl[i].x = x;
	LG.ctl[i].y = y;
}

int lg_drag_at(double ux, double uy) {
	for (int i = LG.nctl - 1; i >= 0; i--) {
		const lg_ctl_t *c = &LG.ctl[i];
		if (c->kind != LG_DRAG || !c->used) continue;
		double r = c->r + 6, dx = ux - c->x, dy = uy - c->y;
		if (dx * dx + dy * dy <= r * r) return i;
	}
	return -1;
}

// -- drawing --

// patterns for one bit: lit bits (docs/learn_figures.md, "Colours")
static const uint8_t pats[LG_ROLES][8] = {
	[LG_WARM]   = { 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD },	// dense dots
	[LG_COOL]   = { 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 },	// sparse dots
	[LG_GOOD]   = { 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88 },	// rising lines
	[LG_BAD]    = { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 },	// crosses
	[LG_ACCENT] = { 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00 },	// stripes
	[LG_SKY]    = { 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 },	// fine stripes
	[LG_EARTH]  = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },	// upright lines
	[LG_PLANT]  = { 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 },	// falling lines
	[LG_GREY]   = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 },	// half
};

// the colour window: index = the page's bit | the planes' bits << 1
// (8 colours on a board with two planes, 32 with four)
static const uint8_t col32[LG_ROLES] = {
	[LG_NONE] = 0, [LG_PAPER] = 0, [LG_INK] = 1, [LG_WARM] = 2, [LG_BAD] = 3,
	[LG_COOL] = 4, [LG_EARTH] = 5, [LG_GOOD] = 6, [LG_ACCENT] = 7,
	[LG_SKY] = 8, [LG_GREY] = 9, [LG_PLANT] = 10,
};
static const uint32_t rgb[16] = {
	0xFEC,	// 0 paper: cream
	0x223,	// 1 ink
	0xF83,	// 2 warm: orange
	0xC22,	// 3 bad: red
	0x47D,	// 4 cool: blue
	0x852,	// 5 earth: brown
	0x3A4,	// 6 good: green
	0xEB2,	// 7 accent: yellow
	0x9CF,	// 8 sky
	0x999,	// 9 grey
	0x263,	// 10 plant: deep green
	0xFEC, 0xFEC, 0xFEC, 0xFEC, 0xFEC,
};

int lg_colour_index(int role, int planes) {
	int c = role >= 0 && role < LG_ROLES ? col32[role] : 1;
	if (planes < 4 && c > 7) {		// eight colours: the nearest of them
		if (role == LG_SKY) c = 4;
		else if (role == LG_PLANT) c = 6;
		else c = 5;
	}
	return c;
}

uint32_t lg_colour_rgb(int index) { return rgb[index & 15]; }

// how drawing goes: mono patterns, or one colour plane
static int plane_bit = -1, plane_n;
static float S, OX, OY;

#define DX(u) (OX + (float)(u) * S)
#define DY(u) (OY + (float)(u) * S)

static bool lit_in_plane(int role) {
	return (lg_colour_index(role, plane_n) >> plane_bit) & 1;
}

// the ink for a fill (fill) or a line or text
static bool role_ink(int role, bool fill, ld_ink_t *k) {
	if (role == LG_NONE) return false;
	memset(k, 0, sizeof *k);
	if (plane_bit >= 0) k->kind = lit_in_plane(role) ? LD_K_INK : LD_K_DARK;
	else if (role == LG_PAPER) k->kind = LD_K_DARK;
	else if (role == LG_INK || !fill) k->kind = LD_K_INK;
	else { k->kind = LD_K_PAT; memcpy(k->pat, pats[role], 8); }
	return true;
}

static int role_c(int role) {	// a colour for text (ld_*: LD_INK or LD_DARK)
	if (plane_bit >= 0) return lit_in_plane(role) ? LD_INK : LD_DARK;
	return role == LG_PAPER ? LD_DARK : LD_INK;
}

// one bit has no colours for lines: a line in a role is dashed its own way
static const uint8_t dashes[LG_ROLES][4] = {	// on, off, on, off (units); 0: solid
	[LG_COOL] = { 6, 3, 6, 3 }, [LG_GOOD] = { 2, 3, 2, 3 }, [LG_BAD] = { 8, 3, 2, 3 },
	[LG_ACCENT] = { 3, 3, 3, 3 }, [LG_SKY] = { 10, 3, 2, 3 }, [LG_EARTH] = { 12, 4, 12, 4 },
	[LG_PLANT] = { 4, 2, 1, 2 }, [LG_GREY] = { 1, 2, 1, 2 },
};

#define MAX_DPT 512
static float dpt[2 * MAX_DPT];		// a line's points, page pixels
static int ndpt;
static float dash[2 * MAX_DPT];	// one dash's

static void pt(float x, float y) {
	if (ndpt < MAX_DPT) { dpt[2 * ndpt] = x; dpt[2 * ndpt + 1] = y; ndpt++; }
}

static float pen_w(float w) {
	float dw = w * S;
	return dw < 1 ? 1 : dw;
}

static void stroke_pts(ld_page_t *p, int role, float w, bool closed, const ld_ink_t *k) {
	float dw = pen_w(w);
	const uint8_t *d = plane_bit < 0 ? dashes[role] : dashes[0];
	if (!d[0]) { ld_k_polyline(p, dpt, ndpt, dw, closed, k); return; }
	if (closed && ndpt > 1) pt(dpt[0], dpt[1]);
	// along the line, on and off by the pattern (scaled with the figure)
	int di = 0, nd = 0;
	float left = d[0] * S;
	bool on = true;
	for (int i = 1; i < ndpt; i++) {
		float x0 = dpt[2 * i - 2], y0 = dpt[2 * i - 1], x1 = dpt[2 * i], y1 = dpt[2 * i + 1];
		float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)), at = 0;
		if (on && !nd) { dash[0] = x0; dash[1] = y0; nd = 1; }
		while (len - at > left) {
			at += left;
			float x = x0 + (x1 - x0) * at / len, y = y0 + (y1 - y0) * at / len;
			if (on) {
				if (nd < MAX_DPT) { dash[2 * nd] = x; dash[2 * nd + 1] = y; nd++; }
				ld_k_polyline(p, dash, nd, dw, false, k);
				nd = 0;
			} else { dash[0] = x; dash[1] = y; nd = 1; }
			on = !on;
			di = (di + 1) & 3;
			left = d[di] * S;
		}
		left -= len - at;
		if (on && nd < MAX_DPT) { dash[2 * nd] = x1; dash[2 * nd + 1] = y1; nd++; }
	}
	if (on && nd > 1) ld_k_polyline(p, dash, nd, dw, false, k);
}

static uint8_t math_mem[16384];

// page pixels whose centres are in [x0, x1) x [y0, y1)
static void dev_rect(ld_page_t *p, float x0, float y0, float x1, float y1, const ld_ink_t *k) {
	int px0 = (int)floorf(x0 + 0.5f), px1 = (int)floorf(x1 + 0.5f);
	int py0 = (int)floorf(y0 + 0.5f), py1 = (int)floorf(y1 + 0.5f);
	if (px1 > px0 && py1 > py0) ld_paint(p, px0, py0, px1 - px0, py1 - py0, k);
}

static void draw_op(ld_page_t *p, const lg_op_t *o) {
	ld_ink_t k;
	bool fill = o->kind == LG_OP_RECT || o->kind == LG_OP_DISC || o->kind == LG_OP_POLY;
	if (!role_ink(o->role, fill, &k)) return;
	const float *a = o->a;
	switch (o->kind) {
	case LG_OP_LINE:
		if ((a[0] == a[2] || a[1] == a[3]) && (plane_bit >= 0 || !dashes[o->role][0])) {
			// upright or level, solid: a rectangle, its ends square
			float hw = a[4] * S < 1 ? 0.5f : a[4] * S / 2;
			float x0 = DX(a[0] < a[2] ? a[0] : a[2]), x1 = DX(a[0] < a[2] ? a[2] : a[0]);
			float y0 = DY(a[1] < a[3] ? a[1] : a[3]), y1 = DY(a[1] < a[3] ? a[3] : a[1]);
			if (a[0] == a[2]) dev_rect(p, x0 - hw, y0, x0 + hw, y1 + (hw > 0.5f ? hw : 1), &k);
			else dev_rect(p, x0, y0 - hw, x1 + (hw > 0.5f ? hw : 1), y0 + hw, &k);
			break;
		}
		ndpt = 0;
		pt(DX(a[0]), DY(a[1]));
		pt(DX(a[2]), DY(a[3]));
		stroke_pts(p, o->role, a[4], false, &k);
		break;
	case LG_OP_ARROW: {
		float dx = a[2] - a[0], dy = a[3] - a[1], len = sqrtf(dx * dx + dy * dy);
		if (len < 0.01f) break;
		float ux = dx / len, uy = dy / len, hl = 6 + 2 * a[4], hw = 3.5f + a[4];
		if (hl > len) hl = len;
		float bx = a[2] - ux * hl, by = a[3] - uy * hl;
		ld_k_line(p, DX(a[0]), DY(a[1]), DX(bx + ux * 0.5f), DY(by + uy * 0.5f), pen_w(a[4]), &k);
		float head[6] = { DX(a[2]), DY(a[3]), DX(bx - uy * hw), DY(by + ux * hw), DX(bx + uy * hw), DY(by - ux * hw) };
		ld_k_poly(p, head, 3, &k);
		break;
	}
	case LG_OP_RECT:
		dev_rect(p, DX(a[0]), DY(a[1]), DX(a[0] + a[2]), DY(a[1] + a[3]), &k);
		break;
	case LG_OP_FRAME: {
		float w = floorf(pen_w(a[4]) + 0.5f);
		float x0 = DX(a[0]), y0 = DY(a[1]), x1 = DX(a[0] + a[2]), y1 = DY(a[1] + a[3]);
		dev_rect(p, x0, y0, x1, y0 + w, &k);
		dev_rect(p, x0, y1 - w, x1, y1, &k);
		dev_rect(p, x0, y0 + w, x0 + w, y1 - w, &k);
		dev_rect(p, x1 - w, y0 + w, x1, y1 - w, &k);
		break;
	}
	case LG_OP_DISC:
		ld_k_disc(p, DX(a[0]), DY(a[1]), a[2] * S, &k);
		break;
	case LG_OP_RING:
		if (plane_bit >= 0 || !dashes[o->role][0]) ld_k_ring(p, DX(a[0]), DY(a[1]), a[2] * S, pen_w(a[3]), &k);
		else {
			ndpt = 0;
			float c = cosf(3.14159265f / 36), s = sinf(3.14159265f / 36), vx = a[2], vy = 0;
			for (int i = 0; i < 72; i++) {
				pt(DX(a[0] + vx), DY(a[1] - vy));
				float t = vx * c - vy * s;
				vy = vx * s + vy * c;
				vx = t;
			}
			stroke_pts(p, o->role, a[3], true, &k);
		}
		break;
	case LG_OP_POLY:
	case LG_OP_PATH: {
		const float *q = LG.pts + o->off;
		ndpt = 0;
		for (int i = 0; i < o->n; i++) pt(DX(q[2 * i]), DY(q[2 * i + 1]));
		if (o->kind == LG_OP_PATH) stroke_pts(p, o->role, a[0], false, &k);
		else ld_k_poly(p, dpt, ndpt, &k);
		break;
	}
	case LG_OP_ARC: {
		// degrees counter-clockwise on the screen, y down
		float a0 = a[3], a1 = a[4];
		if (a1 < a0) { float s = a0; a0 = a1; a1 = s; }
		int steps = (int)((a1 - a0) / 4) + 2;
		if (steps > MAX_DPT - 2) steps = MAX_DPT - 2;
		float st = (a1 - a0) / steps * 3.14159265f / 180, c = cosf(st), s = sinf(st);
		float vx = a[2] * cosf(a0 * 3.14159265f / 180), vy = a[2] * sinf(a0 * 3.14159265f / 180);
		ndpt = 0;
		for (int i = 0; i <= steps; i++) {
			pt(DX(a[0] + vx), DY(a[1] - vy));
			float t = vx * c - vy * s;
			vy = vx * s + vy * c;
			vx = t;
		}
		stroke_pts(p, o->role, a[5], false, &k);
		break;
	}
	case LG_OP_TEXT: {
		const char *s = LG.text + o->off;
		bool bold = o->align & 4;
		const zc_font_t *f;
		int px = 0;
		switch (o->size) {
		case LG_TINY: f = &LDF.f5; break;
		case LG_SMALL: f = bold ? &LDF.small_b : &LDF.small; break;
		case LG_BODY: f = bold ? &LDF.body_b : &LDF.body; break;
		case LG_BIG: f = &LDF.big; break;
		default: f = bold ? &LDF.serif_b : &LDF.serif; px = (int)lroundf(o->size * S); if (px < 6) px = 6; break;
		}
		ld_style_t st = ld_st(f, px);
		int x = (int)lroundf(DX(a[0])), y = (int)lroundf(DY(a[1])), c = role_c(o->role);
		switch (o->align & 3) {
		case LG_CENTRE: ld_text_c(p, &st, x, y, s, c); break;
		case LG_RIGHT: ld_text_r(p, &st, x, y, s, c); break;
		default: ld_textz(p, &st, x, y, s, c); break;
		}
		break;
	}
	case LG_OP_MATH: {
		const char *s = LG.text + o->off;
		int len = (int)strlen(s), px = (int)lroundf(o->size * S), x = (int)lroundf(DX(a[0]));
		if (px < 8) px = 8;
		lx_arena_t ar;
		lx_arena_init(&ar, math_mem, sizeof math_mem);
		if ((o->align & 3) != LG_LEFT) {
			lm_box_t b = lm_draw_str(p, &ar, s, len, px, 0, 0, LD_INK, true);
			x -= (o->align & 3) == LG_CENTRE ? b.w / 2 : b.w;
			lx_arena_init(&ar, math_mem, sizeof math_mem);
		}
		lm_draw_str(p, &ar, s, len, px, x, (int)lroundf(DY(a[1])), role_c(o->role), false);
		break;
	}
	}
}

// The still shapes, kept: after they are drawn, their pixels are read
// back (ld_save) into a bitmap, and from then on each frame starts with
// one copy of it. One way of drawing (one bit, or a plane of the colour
// picture) a slot. A plane may be drawn in pieces (the colour window's
// fold); the bitmap is good once every pixel of it has been read back
// (cov).
#define STILL_SLOTS 6
#define STILL_WPL (LG_W / 32)
static uint32_t still_bits[STILL_SLOTS][STILL_WPL * LG_H + 2];
static uint32_t still_cov[STILL_SLOTS][STILL_WPL * LG_H + 2];
static uint32_t still_tmp[STILL_WPL * LG_H + 2];
static struct { uint32_t serial; int w, h, planes; bool ok, set; } still_key[STILL_SLOTS];

static int still_slot(void) { return plane_bit < 0 ? 0 : 1 + (plane_bit % (STILL_SLOTS - 1)); }

// the part of the still bitmap just drawn at (rx, ry, rw, rh) on the page, kept
static void still_keep(ld_page_t *p, int k, int x, int y, int w, int h, int rx, int ry, int rw, int rh) {
	if (rw <= 0 || rh <= 0 || !ld_save(p, rx, ry, rw, rh, still_tmp, STILL_WPL)) return;
	ld_page_t b, c;
	ld_page(&b, still_bits[k], LG_W, LG_H);
	ld_blit(&b, still_tmp, STILL_WPL, 0, 0, rx - x, ry - y, rw, rh, LD_ROP_COPY);
	ld_page(&c, still_cov[k], LG_W, LG_H);
	ld_fill(&c, rx - x, ry - y, rw, rh, LD_INK);
	// all of it now?
	uint32_t n = 0;
	for (int r = 0; r < h; r++) {
		const uint32_t *row = still_cov[k] + r * STILL_WPL;
		for (int i = 0; i < (w + 31) / 32; i++) {
			uint32_t v = row[i];
			if (i == (w - 1) / 32 && (w & 31)) v &= (1u << (w & 31)) - 1;
			n += (uint32_t)__builtin_popcount(v);
		}
	}
	still_key[k].ok = n == (uint32_t)(w * h);
}

static void render(ld_page_t *p, int x, int y, int w, int h) {
	S = (float)w / LG_W;
	OX = (float)x;
	OY = (float)y;
	if (!ld_clip_hits(p, x, y, w, h)) return;
	ld_clip_t was = ld_clip_push(p, x, y, x + w, y + h);
	ld_ink_t paper;
	memset(&paper, 0, sizeof paper);
	paper.kind = plane_bit >= 0 && lit_in_plane(LG_PAPER) ? LD_K_INK : LD_K_DARK;
	int from = 0;
	if (LG.nstill && w <= LG_W && h <= LG_H) {
		int k = still_slot();
		if (!still_key[k].set || still_key[k].serial != LG.still_serial || still_key[k].w != w ||
			still_key[k].h != h || still_key[k].planes != plane_n) {
			still_key[k].set = true;
			still_key[k].ok = false;
			still_key[k].serial = LG.still_serial;
			still_key[k].w = w;
			still_key[k].h = h;
			still_key[k].planes = plane_n;
			memset(still_cov[k], 0, sizeof still_cov[k]);
		}
		if (still_key[k].ok) ld_blit(p, still_bits[k], STILL_WPL, 0, 0, x, y, w, h, LD_ROP_COPY);
		else {
			ld_paint(p, x, y, w, h, &paper);
			for (int i = 0; i < LG.nstill; i++) draw_op(p, &LG.op[i]);
			still_keep(p, k, x, y, w, h, p->cx0, p->cy0, p->cx1 - p->cx0, p->cy1 - p->cy0);
		}
		from = LG.nstill;
	} else ld_paint(p, x, y, w, h, &paper);
	for (int i = from; i < LG.nops; i++) draw_op(p, &LG.op[i]);
	// the points the learner can drag: a ring around each
	for (int i = 0; i < LG.nctl; i++) {
		const lg_ctl_t *c = &LG.ctl[i];
		if (c->kind != LG_DRAG || !c->used) continue;
		lg_op_t o;
		memset(&o, 0, sizeof o);
		o.kind = LG_OP_RING;
		o.role = LG_INK;
		o.a[0] = (float)c->x;
		o.a[1] = (float)c->y;
		o.a[2] = (float)(c->r + 3);
		o.a[3] = 1;
		draw_op(p, &o);
	}
	ld_clip_pop(p, was);
}

void lg_render(ld_page_t *p, int x, int y, int w, int h) {
	plane_bit = -1;
	render(p, x, y, w, h);
}

void lg_render_plane(ld_page_t *p, int x, int y, int w, int h, int bit, int planes) {
	plane_bit = bit;
	plane_n = planes;
	render(p, x, y, w, h);
	plane_bit = -1;
}

void lg_forget_still(void) {
	for (int i = 0; i < STILL_SLOTS; i++) still_key[i].set = still_key[i].ok = false;
}
