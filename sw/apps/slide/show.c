/*
 * slide -- the show. See show.h.
 *
 * The screen is taken with z_screen_grab(): wm stops managing windows
 * and sends every key and click here, and this draws the framebuffer
 * itself, 640x480, without game mode's smaller view. Each card is
 * rendered at the largest scale that fits the screen (or, with S, at
 * its own size), into one of two buffers; a transition moves from one
 * buffer to the other with the blitter.
 */

#include "show.h"
#include "render.h"
#include "act.h"
#include "script.h"
#include "../../common/zpool.h"

#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zgfx.h"
#include "../../common/zwm.h"
#include "../../common/zkbd.h"
#include "../../common/zsoc.h"
#include "../../common/zspeak.h"

#define SW Z_SCREEN_W
#define SH Z_SCREEN_H
#define SWPL (SW / 32)

#define VRAM ((volatile uint32_t *)0x20000000)

static uint32_t *buf[2];		// what is on the screen, what comes next
static bool hw_blit;

bool show_alloc(void) {
	int i;
	hw_blit = z_fb_hw_blit_mem_available();
	for (i = 0; i < 2; i++) {
		if (!buf[i]) {
			// a spare word: the blitter may read one past the end
			buf[i] = pool_alloc((size_t)SWPL * SH * 4 + 8);
			if (!buf[i]) return false;
		}
	}
	return true;
}

static void blit_sw(const uint32_t *src, int wpl, int sx, int sy, int dx, int dy,
	int w, int h) {
	int j;
	for (j = 0; j < h; j++) {
		volatile uint32_t *drow = &VRAM[(dy + j) * SWPL];
		const uint32_t *srow = src + (size_t)(sy + j) * (size_t)wpl;
		int w0 = dx >> 5, w1 = (dx + w - 1) >> 5, wi;
		for (wi = w0; wi <= w1; wi++) {
			int first = wi * 32;
			int s0 = sx + (first - dx);
			int sw = s0 >> 5, s = s0 & 31;
			uint32_t lo = (sw >= 0 && sw < wpl) ? srow[sw] : 0;
			uint32_t hi = (sw + 1 >= 0 && sw + 1 < wpl) ? srow[sw + 1] : 0;
			uint32_t val = s ? ((lo >> s) | (hi << (32 - s))) : lo;
			uint32_t mask = 0xFFFFFFFFu;
			if (dx > first) mask &= 0xFFFFFFFFu << (dx - first);
			if (dx + w < first + 32) mask &= 0xFFFFFFFFu >> (32 - (dx + w - first));
			drow[wi] = (drow[wi] & ~mask) | (val & mask);
		}
	}
}

void blit_to_screen(const uint32_t *src, int wpl, int sx, int sy, int dx, int dy,
	int w, int h) {
	if (w <= 0 || h <= 0) return;
	if (hw_blit && z_fb_hw_blit_mem(src, wpl * 4, sx, sy, dx, dy, w, h)) return;
	blit_sw(src, wpl, sx, sy, dx, dy, w, h);
}

// -- drawing a card for the screen ---------------------------------------------

static bool actual_size;

static void card_xf(const deck_t *d, zc_xf_t *xf, int *x, int *y, int *w, int *h) {
	int32_t s;
	if (actual_size) {
		s = ZC_XF_ONE;
	} else {
		int32_t sx = (int32_t)(((int64_t)SW << 16) / d->page.gw);
		int32_t sy = (int32_t)(((int64_t)SH << 16) / d->page.gh);
		s = sx < sy ? sx : sy;
	}
	*w = (int)(((int64_t)d->page.gw * s) >> 16);
	*h = (int)(((int64_t)d->page.gh * s) >> 16);
	if (*w > SW) *w = SW;
	if (*h > SH) *h = SH;
	*x = (SW - *w) / 2;
	*y = (SH - *h) / 2;
	*xf = zc_xf_scale(s, ZC_I(*x), ZC_I(*y));
}

static void draw_card(uint32_t *b, const deck_t *d, int i, int blank) {
	zc_target_t t;
	zc_xf_t xf;
	int x, y, w, h;
	zc_paint_t black = zc_black(), white = zc_white();
	zc_target_bits(&t, b, SWPL, SW, SH);
	zc_clear(&t, blank == 2 ? &white : &black);
	if (blank) return;
	card_xf(d, &xf, &x, &y, &w, &h);
	zc_clip(&t, x, y, x + w, y + h);
	render_card(&t, &xf, d, d->card[i], RENDER_SHOW);
}

// -- transitions -------------------------------------------------------------------

static bool skip_wanted(void) {
	// a key or a click during a transition finishes it at once
	z_msg_t m;
	if (z_msg_read(&m) == Z_OK) {
		if (m.subject == Z_WM_KEY || m.subject == Z_WM_MOUSE || m.subject == Z_WM_GAME_REVOKED) {
			z_msg_unread(&m);
			return m.subject != Z_WM_MOUSE ||
				Z_WM_UNPACK_MOUSE_BUTTONS(m.obj.val.uint32) != 0;
		}
		z_msg_unread(&m);
	}
	return false;
}

static void transition(int kind, int ms, uint32_t *from, uint32_t *to) {
	uint32_t t0 = z_uptime_ticks();
	uint32_t total = (uint32_t)ms * Z_TICK_HZ / 1000;
	int done = 0;
	z_gfx_clear_visible();
	if (kind == TR_CUT || total == 0) {
		blit_to_screen(to, SWPL, 0, 0, 0, 0, SW, SH);
		return;
	}
	for (;;) {
		uint32_t el = z_uptime_ticks() - t0;
		int p = el >= total ? SW : (int)((int64_t)SW * el / total);
		if (skip_wanted()) p = SW;
		if (kind == TR_WIPE) {
			// what is newly uncovered, left to right
			blit_to_screen(to, SWPL, done, 0, done, 0, p - done, SH);
			done = p;
		} else if (kind == TR_IRIS) {
			// a circle opening from the middle: each row's span of it
			int R = 400, r = (int)((int64_t)R * p / SW), y;
			for (y = 0; y < SH; y++) {
				int dy = y - SH / 2, dx2 = r * r - dy * dy, dx = 0;
				if (dx2 <= 0) continue;
				while ((dx + 1) * (dx + 1) <= dx2) dx++;	// sqrt, small numbers
				{
					int x0 = SW / 2 - dx, x1 = SW / 2 + dx;
					if (x0 < 0) x0 = 0;
					if (x1 > SW) x1 = SW;
					blit_to_screen(to, SWPL, x0, y, x0, y, x1 - x0, 1);
				}
			}
		} else if (kind == TR_DISSOLVE) {
			// 16x16 blocks in a scattered order: block k is the
			// (k * 2963) mod n'th, which visits every one (2963 is prime
			// to n = 1200)
			const int bw = 16, cols = SW / 16, n = (SW / 16) * (SH / 16);
			int upto = (int)((int64_t)n * p / SW), k;
			for (k = done; k < upto; k++) {
				int b = (int)(((long)k * 2963) % n);
				int bx = (b % cols) * bw, by = (b / cols) * bw;
				blit_to_screen(to, SWPL, bx, by, bx, by, bw, bw);
			}
			done = upto;
		} else {
			// the new card pushes the old one off to the left
			if (p < SW) blit_to_screen(from, SWPL, p, 0, 0, 0, SW - p, SH);
			blit_to_screen(to, SWPL, 0, 0, SW - p, 0, p, SH);
		}
		if (p >= SW) break;
		z_proc_wait(1);
	}
}

// -- the show ---------------------------------------------------------------------

static void answer_window(z_msg_t *m, z_win_t *win, bool *repaint) {
	switch (m->subject) {
	case Z_WM_SET_CLIP:
		z_win_apply_clip(win, &m->obj);
		break;
	case Z_WM_REDRAW:
		if (m->obj.type == Z_UINT32 && z_win_redraw_id(m->obj.val.uint32) == win->id) {
			z_win_apply_redraw(win, m->obj.val.uint32);
			z_win_redraw_done(win);
			*repaint = true;
		}
		break;
	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(win, &m->obj);
		break;
	case Z_WM_WINDOW_RESIZED:
		z_win_apply_resized(win, &m->obj);
		break;
	default:
		break;
	}
}

// The card on the screen's pictures first, then the next card's, so
// that moving on finds them loaded.
static void ask_pictures(deck_t *d, int cur) {
	if (cur + 1 < d->ncard) render_card_want(d, d->card[cur + 1], RENDER_SHOW, false);
	render_card_want(d, d->card[cur], RENDER_SHOW, true);
}

// -- the show's state, shared with the actions' host below ----------------------

static deck_t *sd;
static int s_cur, s_cb, s_blank;
static uint32_t s_shown_at;
static bool s_speak_notes;
static player_t s_pl;
static char s_open[200];			// a deck an action asked for: the editor opens it
static bool s_running;

static void say_notes(void) {
	const char *n = sd->card[s_cur]->notes;
	if (s_speak_notes && n && *n) z_speak(n, 0);
}

static void redraw_now(void) {
	draw_card(buf[s_cb], sd, s_cur, s_blank);
	blit_to_screen(buf[s_cb], SWPL, 0, 0, 0, 0, SW, SH);
}

// To card `to`, with its transition (going back is a cut: a transition
// belongs to arriving); its own action runs.
static void go_card(int to) {
	const card_t *nc;
	if (to < 0) to = 0;
	if (to >= sd->ncard) to = sd->ncard - 1;
	if (to == s_cur) return;
	nc = sd->card[to];
	player_reset(&s_pl);
	s_pl.cur = to;
	draw_card(buf[1 - s_cb], sd, to, s_blank);
	transition(to > s_cur && !s_blank ? nc->trans : TR_CUT, nc->trans_ms, buf[s_cb], buf[1 - s_cb]);
	s_cb = 1 - s_cb;
	s_cur = to;
	s_shown_at = z_uptime_ticks();
	ask_pictures(sd, s_cur);
	say_notes();
	act_arrived(sd, s_cur);
	script_card(sd, s_cur);
}

// The next card: round to the first in a looping deck.
static int next_of(int cur) {
	if (cur + 1 < sd->ncard) return cur + 1;
	return sd->loop ? 0 : cur;
}

static void h_go(void *u, int to) { (void)u; go_card(to); }
static void h_changed(void *u) { (void)u; redraw_now(); }
static void h_open(void *u, const char *path) {
	int i;
	(void)u;
	for (i = 0; path[i] && i < (int)sizeof(s_open) - 1; i++) s_open[i] = path[i];
	s_open[i] = 0;
	s_running = false;
}
static void h_msg(void *u, const char *m) { (void)u; (void)m; }
static void h_speak(void *u, const char *t) { (void)u; z_speak(t, 0); }
static void h_run(void *u, const char *cmd) { (void)u; show_launch(cmd); }
static uint32_t h_now(void *u) { (void)u; return z_uptime_ticks(); }
static void h_eval(void *u, deck_t *d, int cur, const char *f) { (void)u; script_eval(d, cur, f, NULL, 0); }
static void h_clicked(void *u, deck_t *d, int cur, obj_t *o) { (void)u; script_clicked(d, cur, o); }
static int h_current(void *u) { (void)u; return s_cur; }

static z_win_t *s_win;
static void answer_window(z_msg_t *m, z_win_t *win, bool *repaint);

// A script running in the show: Esc stops it (and not the show); the
// window's messages are answered, keys and clicks dropped.
static bool show_stop_check(void) {
	z_msg_t m;
	int n;
	bool rp = false;
	for (n = 0; n < 8 && z_msg_read(&m) == Z_OK; n++) {
		if (m.subject == Z_WM_KEY) {
			if (m.obj.type == Z_UINT32 && Z_WM_UNPACK_KEY_PRESSED(m.obj.val.uint32) &&
				Z_WM_UNPACK_KEY_KEYSYM(m.obj.val.uint32) == 0x1b) return true;
			continue;
		}
		if (m.subject == Z_WM_MOUSE || m.subject == Z_WM_WHEEL) continue;
		if (m.subject == Z_WM_GAME_REVOKED) { z_msg_unread(&m); return true; }
		if (script_window_msg(&m)) continue;
		answer_window(&m, s_win, &rp);
	}
	return false;
}

void show_launch(const char *cmd) {
	char app[32];
	int i = 0;
	while (*cmd == ' ') cmd++;
	while (cmd[i] && cmd[i] != ' ' && i < (int)sizeof(app) - 1) { app[i] = cmd[i]; i++; }
	app[i] = 0;
	cmd += i;
	while (*cmd == ' ') cmd++;
	if (*cmd) z_launch_arg_set(cmd);
	z_proc_run(app);
}

const char *show_open_request(void) { return s_open[0] ? s_open : NULL; }

// Screen to card units, for the controls.
static void to_units(int px, int py, int *ux, int *uy) {
	zc_xf_t xf;
	int x, y, w, h;
	card_xf(sd, &xf, &x, &y, &w, &h);
	*ux = w ? (int)((int64_t)(px - x) * sd->page.gw / w) : 0;
	*uy = h ? (int)((int64_t)(py - y) * sd->page.gh / h) : 0;
}

int show_run(deck_t *d, int start, z_win_t *win) {
	int typed = 0;
	bool repaint = false;
	uint32_t bar_at = 0, last_btn = 0;
	act_host_t host;

	if (!show_alloc() || d->ncard == 0) return start;
	if (start < 0 || start >= d->ncard) start = 0;
	if (!z_screen_grab(true)) return start;

	sd = d;
	s_cur = start;
	s_cb = 0;
	s_blank = 0;
	s_open[0] = 0;
	s_running = true;
	memset(&s_pl, 0, sizeof(s_pl));
	s_pl.d = d;
	s_pl.cur = s_cur;
	render_ui_state(NULL, NULL);
	memset(&host, 0, sizeof(host));
	host.go = h_go; host.changed = h_changed; host.open_deck = h_open; host.message = h_msg;
	host.speak = h_speak; host.run = h_run; host.now = h_now; host.tick_hz = Z_TICK_HZ;
	host.eval = h_eval; host.clicked = h_clicked; host.current = h_current;
	act_init(&host);
	act_reset();
	s_win = win;
	script_stop_check(show_stop_check);
	script_deck(d, s_cur);

	z_gfx_clear_visible();
	draw_card(buf[s_cb], d, s_cur, 0);
	blit_to_screen(buf[s_cb], SWPL, 0, 0, 0, 0, SW, SH);
	s_shown_at = z_uptime_ticks();
	ask_pictures(d, s_cur);
	say_notes();
	act_arrived(d, s_cur);
	script_card(d, s_cur);

	while (s_running) {
		z_msg_t m;
		int go = -1;			// a card to go to
		bool redraw = false;

		while (s_running && go < 0 && z_msg_read(&m) == Z_OK) {
			if (script_window_msg(&m)) continue;
			switch (m.subject) {
			case Z_WM_KEY: {
				uint32_t k;
				bool r;
				if (m.obj.type != Z_UINT32 || !Z_WM_UNPACK_KEY_PRESSED(m.obj.val.uint32)) break;
				k = Z_WM_UNPACK_KEY_KEYSYM(m.obj.val.uint32);
				// typing into a field comes first (Esc leaves it)
				if (player_key(&s_pl, k, &r)) { if (r) redraw = true; break; }
				if (k == 0x1b || k == 'q' || k == 'Q') { s_running = false; break; }
				if (k >= '0' && k <= '9') { typed = typed * 10 + (int)(k - '0'); if (typed > 99999) typed = 0; break; }
				if (k == 0x0d && typed) { go = typed - 1; typed = 0; break; }
				typed = 0;
				if (k == Z_KEY_RIGHT || k == Z_KEY_DOWN || k == Z_KEY_PAGEDOWN || k == ' ' ||
					k == 0x0d || k == 'n' || k == 'N') go = next_of(s_cur);
				else if (k == Z_KEY_LEFT || k == Z_KEY_UP || k == Z_KEY_PAGEUP || k == 0x7f ||
					k == 'p' || k == 'P') go = s_cur > 0 ? s_cur - 1 : d->loop ? d->ncard - 1 : 0;
				else if (k == Z_KEY_HOME) go = 0;
				else if (k == Z_KEY_END) go = d->ncard - 1;
				else if (k == 'b' || k == 'B' || k == '.') { s_blank = s_blank == 1 ? 0 : 1; redraw = true; }
				else if (k == 'w' || k == 'W' || k == ',') { s_blank = s_blank == 2 ? 0 : 2; redraw = true; }
				else if (k == 's' || k == 'S') { actual_size = !actual_size; redraw = true; }
				else if (k == 't' || k == 'T') {
					s_speak_notes = !s_speak_notes;
					if (s_speak_notes) say_notes(); else z_speak_stop();
				}
				break;
			}
			case Z_WM_MOUSE: {
				uint32_t v, b;
				int ux, uy;
				if (m.obj.type != Z_UINT32) break;
				v = m.obj.val.uint32;
				b = Z_WM_UNPACK_MOUSE_BUTTONS(v);
				to_units((int)Z_WM_UNPACK_MOUSE_X(v), (int)Z_WM_UNPACK_MOUSE_Y(v), &ux, &uy);
				if ((b & Z_MOUSE_BTN_LEFT) && !(last_btn & Z_MOUSE_BTN_LEFT)) {
					// a control takes the click; elsewhere it is the next card
					bool took;
					if (player_press(&s_pl, ux, uy, &took)) redraw = true;
					if (!took && !s_blank) go = next_of(s_cur);
				} else if ((b & Z_MOUSE_BTN_LEFT) && (last_btn & Z_MOUSE_BTN_LEFT)) {
					if (player_motion(&s_pl, ux, uy)) redraw = true;
				} else if (!(b & Z_MOUSE_BTN_LEFT) && (last_btn & Z_MOUSE_BTN_LEFT)) {
					if (player_release(&s_pl, ux, uy)) redraw = true;
				}
				if ((b & Z_MOUSE_BTN_RIGHT) && !(last_btn & Z_MOUSE_BTN_RIGHT)) go = s_cur - 1;
				last_btn = b;
				break;
			}
			case Z_WM_WHEEL:
				if (m.obj.type == Z_UINT32)
					go = Z_WM_WHEEL_NOTCHES(m.obj.val.uint32) > 0 ? s_cur - 1 : next_of(s_cur);
				break;
			case Z_WM_GAME_REVOKED:
				// Super+Esc: wm has the screen back already; draw nothing more
				s_pl.focus = NULL;
				render_ui_state(NULL, NULL);
				return s_cur;
			case Z_WM_CLOSE:
				s_running = false;
				break;
			default:
				answer_window(&m, win, &repaint);
				break;
			}
		}
		if (!s_running) break;

		// a card's own time is up
		if (go < 0 && !s_blank && d->card[s_cur]->advance_ms) {
			uint32_t need = d->card[s_cur]->advance_ms * (Z_TICK_HZ / 4) / 250;
			if (z_uptime_ticks() - s_shown_at >= need) {
				int n = next_of(s_cur);
				if (n != s_cur) go = n;
			}
		}

		if (go >= 0 && go != s_cur) {
			go_card(go);
			repaint = false;
		} else if (redraw || repaint) {
			draw_card(buf[s_cb], d, s_cur, s_blank);
			if (repaint) z_gfx_clear_visible();
			blit_to_screen(buf[s_cb], SWPL, 0, 0, 0, 0, SW, SH);
			if (repaint) s_shown_at = z_uptime_ticks();
			repaint = false;
		}

		// an action's (wait) come due, a script's timers
		if (act_waiting()) act_tick();
		if (script_timers()) script_tick();

		// nothing to answer: load a picture a slice at a time, never
		// longer than a key can wait (docs/responsive.md)
		if (render_loading()) {
			bool fin;
			obj_t *o = render_load_step(Z_TICK_HZ / 50, &fin);
			if (!s_blank && render_card_has(d, d->card[s_cur], o) &&
				(fin || z_uptime_ticks() - bar_at >= Z_TICK_HZ / 2)) {
				// its picture, or how far it has got
				redraw_now();
				bar_at = z_uptime_ticks();
			}
		} else {
			z_proc_wait(act_waiting() || script_timers() || s_pl.pressed ? Z_TICK_HZ / 50 : Z_TICK_HZ / 20);
		}
	}

	player_reset(&s_pl);
	act_reset();
	script_stop_timers();
	if (s_speak_notes) z_speak_stop();
	z_screen_grab(false);
	return s_cur;
}
