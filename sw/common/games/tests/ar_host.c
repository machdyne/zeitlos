/*
 * Zeitlos -- the arcade runtime's machine half, on a build host.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zarcade_sys.c talks to wm and the blitter; this file pretends to be
 * both, so a game's real loop runs on a PC and its frames can be looked
 * at. It is the same contract (zarcade_int.h):
 *
 *   - a 640x480 1bpp framebuffer, written by a software blitter with the
 *     hardware's semantics (fills, screen-aligned 8x8 patterns, OR/ANDN
 *     copies from memory, masked sprites, VRAM copies)
 *   - a palette, a viewport offset (the shake) and the 16-colour
 *     scanout of docs/color.md: plane k at (k&1)*320, (k>>1)*240
 *   - a window at an arbitrary place on that framebuffer, optionally
 *     with another "window" covering part of it, so the clipping is
 *     exercised the way wm's overlapping windows exercise it
 *   - a SCRIPT instead of wm: key presses at given frames, mode
 *     changes, and snapshots written as PPM files
 *
 * The game links this instead of zarcade_sys.c; nothing in the game
 * changes. Its main() is renamed (-Dmain=game_main) and called from the
 * script runner here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zarcade_int.h"
#include "../zsfx.h"
#include "ar_host.h"

/* ===================================================================
 * the framebuffer and a software blitter
 * =================================================================== */

#define FB_W 640
#define FB_H 480

static uint8_t fb[FB_H][FB_W];       /* one byte a pixel: simple */

uint64_t arh_ops;                    /* blitter operations issued */

static void put(int x, int y, int v) {
	if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) {
		fprintf(stderr, "ar_host: draw outside the framebuffer at %d,%d\n",
			x, y);
		abort();
	}
	fb[y][x] = (uint8_t)v;
}

static int src_bit(const void *src, int stride, int x, int y) {
	const uint32_t *w = (const uint32_t *)((const uint8_t *)src + y * stride);
	return (w[x >> 5] >> (x & 31)) & 1;
}

void arh_fill(int x, int y, int w, int h, int bit) {
	int i, j;
	arh_ops++;
	for (j = 0; j < h; j++) for (i = 0; i < w; i++) put(x + i, y + j, bit);
}

void arh_pattern(int x, int y, int w, int h, const uint8_t *pat8) {
	int i, j;
	arh_ops++;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int px = x + i, py = y + j;
			put(px, py, (pat8[py & 7] >> (7 - (px & 7))) & 1);
		}
}

void arh_blit(const void *src, int stride, int sx, int sy,
	int x, int y, int w, int h, int rop) {
	int i, j;
	arh_ops++;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int b = src_bit(src, stride, sx + i, sy + j);
			if (!b) { put(x + i, y + j, fb[y + j][x + i]); continue; }
			put(x + i, y + j, rop == ARH_OR ? 1 : 0);
		}
}

void arh_sprite(const void *data, const void *mask, int stride,
	int sx, int sy, int x, int y, int w, int h) {
	int i, j;
	arh_ops++;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int m = src_bit(mask, stride, sx + i, sy + j);
			int d = src_bit(data, stride, sx + i, sy + j);
			if (d && !m) {
				fprintf(stderr, "ar_host: sprite data outside its mask\n");
				abort();
			}
			if (m) put(x + i, y + j, d);
			else put(x + i, y + j, fb[y + j][x + i]);
		}
}

void arh_vcopy(int sx, int sy, int dx, int dy, int w, int h) {
	static uint8_t tmp[FB_H][FB_W];
	int i, j;
	arh_ops++;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			if (sx + i >= FB_W || sy + j >= FB_H) abort();
			tmp[j][i] = fb[sy + j][sx + i];
		}
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) put(dx + i, dy + j, tmp[j][i]);
}

/* ===================================================================
 * pads, files, seed
 * =================================================================== */

static uint32_t pads[AR_PLAYERS];
static bool pad_in[AR_PLAYERS];

uint32_t arh_pad(int p) { return (p >= 0 && p < AR_PLAYERS) ? pads[p] : 0; }
bool arh_pad_present(int p) { return p >= 0 && p < AR_PLAYERS && pad_in[p]; }

static char scores[4096];
static int scores_len;

int arh_read_scores(char *buf, int cap) {
	int n = scores_len < cap ? scores_len : cap;
	memcpy(buf, scores, (size_t)n);
	return n;
}

bool arh_write_scores(const char *buf, int len) {
	if (len >= (int)sizeof(scores)) return false;
	memcpy(scores, buf, (size_t)len);
	scores_len = len;
	return true;
}

const char *ar_host_scores(void) {
	scores[scores_len] = 0;
	return scores;
}

static char settings[1024];
static int settings_len;
int ar_host_cfg_skill = -1;
char ar_host_said[64];
int ar_host_says;

int arh_read_settings(char *buf, int cap) {
	int n = settings_len < cap ? settings_len : cap;
	memcpy(buf, settings, (size_t)n);
	return n;
}

bool arh_write_settings(const char *buf, int len) {
	if (len >= (int)sizeof(settings)) return false;
	memcpy(settings, buf, (size_t)len);
	settings_len = len;
	return true;
}

const char *ar_host_settings(void) {
	settings[settings_len] = 0;
	return settings;
}

void ar_host_set_settings(const char *text) {
	settings_len = (int)strlen(text);
	if (settings_len >= (int)sizeof(settings)) settings_len = (int)sizeof(settings) - 1;
	memcpy(settings, text, (size_t)settings_len);
}

int arh_cfg_skill(void) { return ar_host_cfg_skill; }

void arh_say(const char *text) {
	size_t n = strlen(text);
	if (n >= sizeof ar_host_said) n = sizeof ar_host_said - 1;
	memcpy(ar_host_said, text, n);
	ar_host_said[n] = 0;
	ar_host_says++;
}

uint32_t arh_seed(void) { return 12345u; }

/* ===================================================================
 * modes and targets
 * =================================================================== */

#define WIN_X 40          /* where the window's content sits */
#define WIN_Y 60

static ar_rect_t cover;   /* a window in front of ours, or w = 0 */

static void clear_fb(void) {
	memset(fb, 0, sizeof(fb));
}

bool ar_color_available(void) { return true; }

bool ar_set_fullscreen(bool on) {
	if (on == (ar__s.mode != AR_MODE_WINDOW)) return true;
	clear_fb();
	if (on) ar__s.mode = ar__s.want_color ? AR_MODE_FULL_COLOR : AR_MODE_FULL_MONO;
	else ar__s.mode = AR_MODE_WINDOW;
	ar__s.pal_dirty = 0xffff;
	ar__invalidate();
	ar__s.targets_stale = true;
	return true;
}

void ar_set_color(bool on) {
	ar__s.want_color = on;
	if (ar__s.mode == AR_MODE_WINDOW) return;
	clear_fb();
	ar__s.mode = on ? AR_MODE_FULL_COLOR : AR_MODE_FULL_MONO;
	ar__invalidate();
	ar__s.targets_stale = true;
}

static void add_vis(int x0, int y0, int x1, int y1) {
	ar_rect_t *r;
	if (x1 <= x0 || y1 <= y0 || ar__s.nvis >= AR_MAX_VIS) return;
	r = &ar__s.vis[ar__s.nvis++];
	r->x = (int16_t)x0; r->y = (int16_t)y0;
	r->w = (int16_t)(x1 - x0); r->h = (int16_t)(y1 - y0);
}

void arh_targets(void) {
	ar__s.targets_stale = false;
	ar__s.can_draw = true;
	ar__s.nvis = 0;
	if (ar__s.mode != AR_MODE_WINDOW) {
		ar__s.ox = ar__s.oy = 0;
		add_vis(0, 0, AR_W, AR_H);
		ar__s.vis_whole = true;
		return;
	}
	ar__s.ox = WIN_X;
	ar__s.oy = WIN_Y;
	if (cover.w == 0) {
		add_vis(WIN_X, WIN_Y, WIN_X + AR_W, WIN_Y + AR_H);
		ar__s.vis_whole = true;
		return;
	}
	/* The content minus the cover, as wm would send it: the bands
	 * above and below, and the pieces left and right of it. */
	{
		int x0 = WIN_X, y0 = WIN_Y, x1 = WIN_X + AR_W, y1 = WIN_Y + AR_H;
		int cx0 = cover.x, cy0 = cover.y;
		int cx1 = cover.x + cover.w, cy1 = cover.y + cover.h;
		add_vis(x0, y0, x1, cy0 < y0 ? y0 : cy0);
		add_vis(x0, cy1 > y1 ? y1 : cy1, x1, y1);
		{
			int my0 = cy0 > y0 ? cy0 : y0, my1 = cy1 < y1 ? cy1 : y1;
			add_vis(x0, my0, cx0 < x0 ? x0 : (cx0 > x1 ? x1 : cx0), my1);
			add_vis(cx1 > x1 ? x1 : (cx1 < x0 ? x0 : cx1), my0, x1, my1);
		}
		ar__s.vis_whole = false;
	}
}

void ar_host_cover(int x, int y, int w, int h) {
	cover.x = (int16_t)(WIN_X + x); cover.y = (int16_t)(WIN_Y + y);
	cover.w = (int16_t)w; cover.h = (int16_t)h;
	ar__s.targets_stale = true;
	if (w == 0) ar__invalidate();    /* uncovered: wm would send REDRAW */
}

/* ===================================================================
 * the script
 * =================================================================== */

static const ar_host_step_t *script;
static int script_pos;
static uint32_t frame_no;
static const char *out_dir = ".";
static int shots;

void ar_host_run(const ar_host_step_t *s, const char *dir) {
	script = s;
	script_pos = 0;
	out_dir = dir;
}

void ar_host_snapshot(const char *name);
static void snapshot(const char *name) { ar_host_snapshot(name); }

/* A script may also take pictures when something happens rather than
 * at a frame: it defines ar_host_hook(), called after every frame. */
void ar_host_hook(uint32_t frame) __attribute__((weak));

void ar_host_snapshot(const char *name) {

	char path[512];
	FILE *f;
	int x, y;

	snprintf(path, sizeof path, "%s/%s.ppm", out_dir, name);
	f = fopen(path, "wb");
	if (!f) { perror(path); exit(1); }
	fprintf(f, "P6\n%d %d\n255\n", AR_W, AR_H);

	for (y = 0; y < AR_H; y++)
		for (x = 0; x < AR_W; x++) {
			int r, g, b;
			if (ar__s.mode == AR_MODE_FULL_COLOR) {
				int vx = (x + ar__s.view_dx + 640) % 640;
				int vy = (y + ar__s.view_dy + 480) % 480;
				int k, idx = 0;
				uint16_t c;
				for (k = 0; k < 4; k++) {
					int px = (vx + ((k & 1) ? 320 : 0)) % 640;
					int py = (vy + ((k & 2) ? 240 : 0)) % 480;
					idx |= fb[py][px] << k;
				}
				c = ar__s.pal[idx];
				r = ((c >> 8) & 15) * 17;
				g = ((c >> 4) & 15) * 17;
				b = (c & 15) * 17;
			} else if (ar__s.mode == AR_MODE_FULL_MONO) {
				int vx = (x + ar__s.view_dx + 640) % 640;
				int vy = (y + ar__s.view_dy + 480) % 480;
				r = g = b = fb[vy][vx] ? 255 : 0;
			} else {
				int px = WIN_X + x, py = WIN_Y + y;
				bool covered = cover.w && px >= cover.x &&
					px < cover.x + cover.w && py >= cover.y &&
					py < cover.y + cover.h;
				if (covered) { r = 90; g = 90; b = 160; }
				else r = g = b = fb[py][px] ? 255 : 0;
			}
			fputc(r, f); fputc(g, f); fputc(b, f);
		}

	fclose(f);
	shots++;
#ifdef AR_HOST_DEBUG
	{ extern void ar__test_dump(void); ar__test_dump(); }
#endif
	printf("ar_host: frame %u -> %s (%llu blitter ops so far)\n",
		frame_no, path, (unsigned long long)arh_ops);
}

static bool quit_sent;

bool ar_poll(ar_event_t *e) {

	for (;;) {
		const ar_host_step_t *s;

		if (ar__dequeue(e)) return true;
		if (!script) return false;
		s = &script[script_pos];
		if (s->kind == ARH_END && s->frame <= frame_no) {
			if (!quit_sent) {
				memset(e, 0, sizeof(*e));
				e->type = AR_EV_QUIT;
				quit_sent = true;
				return true;
			}
			return false;
		}
		if (s->frame > frame_no || s->kind == ARH_END) return false;
		script_pos++;

		switch (s->kind) {
		case ARH_KEY_DOWN: ar__key(s->a, true, 0); break;
		case ARH_KEY_UP:   ar__key(s->a, false, 0); break;
		case ARH_TAP:      ar__key(s->a, true, 0); ar__key(s->a, false, 0); break;
		case ARH_PAD:      pads[s->b] = s->a; pad_in[s->b] = true; break;
		case ARH_WINDOW:   ar_set_fullscreen(false); break;
		case ARH_FULL_COLOR: ar__s.want_color = true; ar_set_fullscreen(true);
			ar_set_color(true); break;
		case ARH_FULL_MONO: ar__s.want_color = false; ar_set_fullscreen(true);
			ar_set_color(false); break;
		case ARH_COVER:    ar_host_cover(s->x, s->y, s->w, s->h); break;
		case ARH_SHOT:     break;   /* handled in ar_frame */
		case ARH_MOUSE: {
			ar_event_t m;
			memset(&m, 0, sizeof(m));
			m.type = AR_EV_MOUSE;
			m.x = (int16_t)s->x;
			m.y = (int16_t)s->y;
			m.buttons = (uint8_t)s->a;
			m.inside = 1;
			ar__queue(&m);
			break;
		}
		default: break;
		}
		if (s->kind == ARH_SHOT) {
			/* take it after this frame has been drawn */
			script_pos--;
			return false;
		}
	}
}

static int verify = -1;
uint32_t ar_host_verify_frames, verify_frames, verify_bad;

uint32_t ar_host_verify_bad(void) { return verify_bad; }
uint32_t ar_host_verified(void) { return verify_frames; }

uint32_t ar_frame(void) {

	frame_no++;
	ar__shake_step();
	ar__s.pal_dirty = 0;
	arh_targets();
	ar__render();
#ifdef AR_HOST_TESTS
	/* AR_HOST_VERIFY=1: after every frame, repaint everything from
	 * nothing and compare. The incremental picture -- dirty tiles,
	 * overlays, the game's own copies -- must be exactly the picture a
	 * full repaint gives. */
	if (verify < 0) verify = getenv("AR_HOST_VERIFY") != NULL;
	if (verify) {
		static uint8_t inc[FB_H][FB_W];
		extern void ar__test_resubmit(void);
		int d;
		memcpy(inc, fb, sizeof(fb));
		ar__s.verifying = true;
		ar__invalidate();
		ar__test_resubmit();
		ar__render();
		ar__s.verifying = false;
		d = ar_host_fb_diff(&inc[0][0], &fb[0][0]);
		if (d) {
			verify_bad++;
			if (verify_bad <= 5) {
				char path[512];
				FILE *f;
				int x, y, x0 = 9999, y0 = 9999, x1 = -1, y1 = -1;
				for (y = 0; y < FB_H; y++)
					for (x = 0; x < FB_W; x++)
						if (inc[y][x] != fb[y][x]) {
							if (x < x0) x0 = x;
							if (y < y0) y0 = y;
							if (x > x1) x1 = x;
							if (y > y1) y1 = y;
						}
				printf("ar_host: frame %u: %d pixels differ from a full repaint, "
					"framebuffer %d,%d .. %d,%d\n", frame_no, d, x0, y0, x1, y1);
				/* the whole framebuffer: grey where both agree on lit,
				 * red where only the incremental frame has a pixel,
				 * green where only the repaint has */
				snprintf(path, sizeof path, "%s/verify_%u.ppm", out_dir, frame_no);
				f = fopen(path, "wb");
				if (f) {
					fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
					for (y = 0; y < FB_H; y++)
						for (x = 0; x < FB_W; x++) {
							int a = inc[y][x], b = fb[y][x];
							int r = a && b ? 120 : a ? 255 : 0;
							int g = a && b ? 120 : b ? 255 : 0;
							int bl = a && b ? 120 : 0;
							fputc(r, f); fputc(g, f); fputc(bl, f);
						}
					fclose(f);
				}
			}
		}
		verify_frames++;
	}
#endif
	zsfx_tick(1);
	ar__frame_input();

	if (ar_host_hook) ar_host_hook(frame_no);

	while (script && script[script_pos].kind == ARH_SHOT &&
	       script[script_pos].frame <= frame_no) {
		snapshot(script[script_pos].name);
		script_pos++;
	}

	ar__s.ticks += 1;
	return 1;
}

uint32_t ar_host_frame(void) { return frame_no; }
int ar_host_shots(void) { return shots; }

bool ar_init(const ar_desc_t *d) {
	ar__core_init(d);
	ar_seed(arh_seed());
	clear_fb();
	zsfx_init();
	ar__invalidate();
	return true;
}

void ar_shutdown(void) {
	zsfx_shutdown();
}

/* Count lit pixels of the current picture in a logical rectangle, in
 * plane 0 -- for tests that want to check something was drawn. */
int ar_host_lit(int x, int y, int w, int h) {
	int i, j, n = 0;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int px = ar__s.ox + x + i, py = ar__s.oy + y + j;
			if (px >= 0 && py >= 0 && px < FB_W && py < FB_H) n += fb[py][px];
		}
	return n;
}

/* ===================================================================
 * zsfx's hardware, recorded
 * =================================================================== */

uint32_t zsfxh_notes, zsfxh_updates, zsfxh_offs;
uint8_t zsfxh_peak[8];

bool zsfxh_present(void) { return true; }
bool zsfxh_speech(void) { return false; }
uint32_t zsfxh_out_hz(void) { return 44117; }
void zsfxh_note(int ch, const int8_t *wave, uint32_t len, uint32_t step,
	uint8_t gl, uint8_t gr) {
	(void)wave; (void)len; (void)step;
	zsfxh_notes++;
	if (ch >= 0 && ch < 8) {
		if (gl > zsfxh_peak[ch]) zsfxh_peak[ch] = gl;
		if (gr > zsfxh_peak[ch]) zsfxh_peak[ch] = gr;
	}
}
void zsfxh_gain(int ch, uint32_t step, uint8_t gl, uint8_t gr) {
	(void)ch; (void)step; (void)gl; (void)gr;
	zsfxh_updates++;
}
void zsfxh_off(int ch) { (void)ch; zsfxh_offs++; }
void zsfxh_release(void) { }

/* -- for the runtime's own tests -- */

void ar_host_fb_copy(uint8_t *out) {
	memcpy(out, fb, sizeof(fb));
}

int ar_host_fb_diff(const uint8_t *a, const uint8_t *b) {
	int i, n = 0;
	for (i = 0; i < FB_W * FB_H; i++) n += a[i] != b[i];
	return n;
}

void ar_host_render_only(void) {
	arh_targets();
	ar__render();
}
