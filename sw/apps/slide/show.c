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
#include "../../common/zpool.h"

#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zgfx.h"
#include "../../common/zwm.h"
#include "../../common/zkbd.h"
#include "../../common/zsoc.h"

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

int show_run(deck_t *d, int start, z_win_t *win) {
	int cur = start, cb = 0;
	int blank = 0;
	int typed = 0;
	bool repaint = false, running = true;
	uint32_t shown_at, bar_at = 0;

	if (!show_alloc() || d->ncard == 0) return start;
	if (cur < 0 || cur >= d->ncard) cur = 0;
	if (!z_screen_grab(true)) return start;

	z_gfx_clear_visible();
	draw_card(buf[cb], d, cur, 0);
	blit_to_screen(buf[cb], SWPL, 0, 0, 0, 0, SW, SH);
	shown_at = z_uptime_ticks();
	ask_pictures(d, cur);

	while (running) {
		z_msg_t m;
		int go = -1;			// a card to go to
		bool redraw_now = false;

		while (z_msg_read(&m) == Z_OK && go < 0 && running) {
			switch (m.subject) {
			case Z_WM_KEY: {
				uint32_t k;
				if (m.obj.type != Z_UINT32 || !Z_WM_UNPACK_KEY_PRESSED(m.obj.val.uint32)) break;
				k = Z_WM_UNPACK_KEY_KEYSYM(m.obj.val.uint32);
				if (k == 0x1b || k == 'q' || k == 'Q') { running = false; break; }
				if (k >= '0' && k <= '9') { typed = typed * 10 + (int)(k - '0'); if (typed > 99999) typed = 0; break; }
				if (k == 0x0d && typed) { go = typed - 1; typed = 0; break; }
				typed = 0;
				if (k == Z_KEY_RIGHT || k == Z_KEY_DOWN || k == Z_KEY_PAGEDOWN || k == ' ' ||
					k == 0x0d || k == 'n' || k == 'N') go = cur + 1;
				else if (k == Z_KEY_LEFT || k == Z_KEY_UP || k == Z_KEY_PAGEUP || k == 0x7f ||
					k == 'p' || k == 'P') go = cur - 1;
				else if (k == Z_KEY_HOME) go = 0;
				else if (k == Z_KEY_END) go = d->ncard - 1;
				else if (k == 'b' || k == 'B' || k == '.') { blank = blank == 1 ? 0 : 1; redraw_now = true; }
				else if (k == 'w' || k == 'W' || k == ',') { blank = blank == 2 ? 0 : 2; redraw_now = true; }
				else if (k == 's' || k == 'S') { actual_size = !actual_size; redraw_now = true; }
				break;
			}
			case Z_WM_MOUSE: {
				static uint32_t last_btn;
				uint32_t b;
				if (m.obj.type != Z_UINT32) break;
				b = Z_WM_UNPACK_MOUSE_BUTTONS(m.obj.val.uint32);
				if ((b & Z_MOUSE_BTN_LEFT) && !(last_btn & Z_MOUSE_BTN_LEFT)) go = cur + 1;
				if ((b & Z_MOUSE_BTN_RIGHT) && !(last_btn & Z_MOUSE_BTN_RIGHT)) go = cur - 1;
				last_btn = b;
				break;
			}
			case Z_WM_WHEEL:
				if (m.obj.type == Z_UINT32)
					go = cur - (Z_WM_WHEEL_NOTCHES(m.obj.val.uint32) > 0 ? 1 : -1);
				break;
			case Z_WM_GAME_REVOKED:
				// Super+Esc: wm has the screen back already; draw nothing more
				return cur;
			case Z_WM_CLOSE:
				running = false;
				break;
			default:
				answer_window(&m, win, &repaint);
				break;
			}
		}
		if (!running) break;

		// a card's own time is up
		if (go < 0 && !blank && d->card[cur]->advance_ms) {
			uint32_t need = d->card[cur]->advance_ms * (Z_TICK_HZ / 4) / 250;
			if (z_uptime_ticks() - shown_at >= need) {
				if (cur + 1 < d->ncard) go = cur + 1;
			}
		}

		if (go >= d->ncard) go = d->ncard - 1;
		if (go >= 0 && go != cur) {
			const card_t *nc = d->card[go];
			draw_card(buf[1 - cb], d, go, blank);
			// going back is a cut: a transition belongs to arriving
			transition(go > cur && !blank ? nc->trans : TR_CUT, nc->trans_ms, buf[cb], buf[1 - cb]);
			cb = 1 - cb;
			cur = go;
			shown_at = z_uptime_ticks();
			repaint = false;
			ask_pictures(d, cur);
		} else if (redraw_now || repaint) {
			draw_card(buf[cb], d, cur, blank);
			z_gfx_clear_visible();
			blit_to_screen(buf[cb], SWPL, 0, 0, 0, 0, SW, SH);
			repaint = false;
			shown_at = z_uptime_ticks();
		}

		// nothing to answer: load a picture a slice at a time, never
		// longer than a key can wait (docs/responsive.md)
		if (render_loading()) {
			bool fin;
			obj_t *o = render_load_step(Z_TICK_HZ / 50, &fin);
			if (!blank && render_card_has(d, d->card[cur], o) &&
				(fin || z_uptime_ticks() - bar_at >= Z_TICK_HZ / 2)) {
				// its picture, or how far it has got
				draw_card(buf[cb], d, cur, blank);
				blit_to_screen(buf[cb], SWPL, 0, 0, 0, 0, SW, SH);
				bar_at = z_uptime_ticks();
			}
		} else {
			z_proc_wait(Z_TICK_HZ / 20);
		}
	}

	z_screen_grab(false);
	return cur;
}
