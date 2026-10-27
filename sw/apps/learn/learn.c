/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- a school on the machine (docs/learn.md).
 *
 *   > run learn
 *   > run learn /opt/ark/edu/zeit-learn      opens that course
 *
 * This file is the board's side of learn: the window, its messages, and
 * putting what the app draws on the screen; the machine's services
 * (files, the clock, speech, boxes, running an app) as the app asks for
 * them through lplat.h. Everything else -- the screens, the lessons,
 * the questions, the records -- is lapp.c and the files it uses, which
 * the tests run on the build machine as they are.
 *
 * The page: what the app draws (ldraw.h) goes straight into the window,
 * each rectangle, line, copy and scroll an operation of the blitter or
 * the line rasterizer (the GPU device below), confined by them to the
 * window's visible region. Nothing is drawn twice and nothing is copied
 * after. On a bitstream whose blitter cannot read memory the page is a
 * 1bpp buffer instead, drawn in software and copied by the CPU.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zfs.h"
#include "../../common/zrtc.h"
#include "../../common/zspeak.h"
#include "../../common/zpaths.h"
#include "../../common/zco.h"

#include "lapp.h"
#include "lplat.h"
#include "ldraw.h"
#include "lfig.h"

#define VRAM        ((volatile uint32_t *)0x20000000)
#define VRAM_WPL    (Z_SCREEN_W / 32)

static z_win_t win;
static z_dialog_ctx_t dlg;
static bool hw_blit;

// the page: the window's content, at most the screen
#define PAGE_WPL (Z_SCREEN_W / 32)
static uint32_t pagebits[PAGE_WPL * Z_SCREEN_H + 2];
static ld_page_t page;
static int page_w, page_h;

static uint8_t scratch[96 << 10];

// -- memory: fonts first, the app after them --

static uint8_t *arena;
static uint32_t arena_size, arena_used;

static void *arena_take(uint32_t n) {
	uint32_t at = (arena_used + 7u) & ~7u;
	if (at + n > arena_size) return NULL;
	arena_used = at + n;
	return arena + at;
}

static bool mem_setup(void) {
	static const uint32_t sizes[] = { 4u << 20, 3u << 20, 2u << 20, 3u << 19 };
	for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		void *base = NULL;
		uint32_t got = 0;
		if (z_arena_set(sizes[i], &base, &got) == Z_ARENA_OK && base && got >= sizes[i]) {
			arena = (uint8_t *)base;
			arena_size = got;
			return true;
		}
	}
	return false;
}

static const void *load_font(const char *file, uint32_t *len) {
	char path[96];
	if (!strcmp(file, "jp12.zfn")) snprintf(path, sizeof path, "%s", Z_PATH_JFONT_FONT);
	else snprintf(path, sizeof path, "%s/%s", Z_DIR_SYS_FONTS, file);
	int n = fs_size(path);
	if (n <= 0) return NULL;
	uint8_t *b = (uint8_t *)arena_take((uint32_t)n + 1);
	if (!b) return NULL;
	if (fs_read_file(path, (char *)b, n) != n) return NULL;
	*len = (uint32_t)n;
	return b;
}

// -- slow work in a coroutine (docs/responsive.md) --
//
// Opening a course is a few dozen requests to fs (a listing a folder, a
// title a unit), and fs's reply comes as a message. Made in one call,
// the main loop read nothing meanwhile: wm's messages filled the
// mailbox (32), fs's reply found no room and was dropped, and each
// request waited out its 15 s timeout -- a course took minutes, and the
// window could not be closed. So la_work() runs in a coroutine that
// hands back after every request to fs (fs_yield(), below): the main
// loop reads its messages between them, and Esc stops an opening. It
// holds nothing between requests (every file is opened and closed
// within one), so abandoning it is safe. Its stack is in main()'s frame,
// above the heap: Scheme's strings are malloc'd, and the figure library
// is loaded in it once.
static zco_t work_co;
static bool work_on, in_work;
static uint8_t *work_stack;
static size_t work_stack_size;

static void work_fn(void *arg) { (void)arg; la_work(15); }

static void fs_yield(void) { if (in_work) zco_yield(); }

static void work_step(void) {
	if (!work_on) {
		zco_init(&work_co, work_fn, NULL, work_stack, work_stack_size);
		work_on = true;
	}
	in_work = true;
	bool more = zco_resume(&work_co);
	in_work = false;
	if (!more) {
		work_on = false;
		if (work_co.overflow) printf("learn: the work's stack overflowed (%u bytes)\n", (unsigned)work_stack_size);
	}
}

static void work_stop(void) {
	if (work_on) zco_abandon(&work_co);
	work_on = false;
}

// -- the platform (lplat.h) over the fs service and the rest --

static bool p_list(void *ctx, const char *dir, void (*fn)(void *, const char *, bool), void *arg) {
	(void)ctx;
	static char buf[8192];
	static uint8_t types[256];
	uint32_t count = 0, trunc = 0;
	if (!fs_list_into(dir, buf, sizeof buf, types, 256, &count, &trunc)) return false;
	const char *p = buf;
	for (uint32_t i = 0; i < count; i++) {
		const char *base = strrchr(p, '/');
		base = base ? base + 1 : p;
		if (base[0]) fn(arg, base, types[i] == Z_FS_TYPE_DIR);
		p += strlen(p) + 1;
	}
	fs_yield();
	return true;
}

static long p_read_at(const char *path, long off, char *buf, long cap) {
	int h = fs_open_read(path);
	if (h < 0) return -1;
	if (off && !fs_seek(h, (uint32_t)off)) { fs_close_handle(h); return -1; }
	long got = 0;
	while (got < cap) {
		int want = cap - got > 4096 ? 4096 : (int)(cap - got);
		int n = fs_read_chunk(h, buf + got, want);
		if (n <= 0) break;
		got += n;
		if (n < want) break;
	}
	fs_close_handle(h);
	fs_yield();
	return got;
}

static long p_read(void *ctx, const char *path, char *buf, long cap) {
	(void)ctx;
	if (!buf || cap <= 0) return fs_size((char *)path) > 0 ? 0 : -1;
	return p_read_at(path, 0, buf, cap);
}

static bool write_all(int h, const char *data, long len) {
	while (len > 0) {
		int n = len > 4096 ? 4096 : (int)len;
		if (fs_write_chunk(h, data, n) != n) return false;
		data += n;
		len -= n;
	}
	return true;
}

static bool p_write_whole(const char *path, const char *data, long len) {
	// the whole file or nothing: NAME.part, then renamed (docs/philosophy.md, "Trusted")
	char part[128];
	snprintf(part, sizeof part, "%s.part", path);
	int h = fs_open_write(part);
	if (h < 0) return false;
	bool ok = write_all(h, data, len);
	ok = fs_close_handle(h) && ok;
	if (!ok) { fs_unlink(part); return false; }
	int err = 0;
	if (fs_rename(part, path, &err)) return true;
	if (err == Z_FS_ERR_EXIST) {
		fs_unlink((char *)path);
		if (fs_rename(part, path, &err)) return true;
	}
	return false;
}

static bool p_append(const char *path, const char *data, long len) {
	int h = fs_open_rw(path);
	if (h < 0) {
		h = fs_open_write(path);
		if (h < 0) return false;
	} else {
		z_fs_info_t fi;
		if (fs_stat(path, &fi)) fs_seek(h, fi.size);
	}
	bool ok = write_all(h, data, len);
	ok = fs_sync(h) && ok;
	ok = fs_close_handle(h) && ok;
	return ok;
}

static bool p_exists(const char *path) {
	z_fs_info_t fi;
	return fs_stat(path, &fi) != 0;
}

static bool p_mkdir(const char *path) {
	if (p_exists(path)) return true;
	fs_mkdir(path);
	return p_exists(path);
}

static uint32_t p_now(void) { return z_rtc_valid() ? z_rtc_seconds() : 0; }
static uint32_t p_ms(void) { return (uint32_t)((uint64_t)z_uptime_ticks() * 1000u / Z_TICK_HZ); }

static char spoken[1024];
static void p_speak(const char *t) {
	snprintf(spoken, sizeof spoken, "%s", t);
	z_speak_static(spoken, Z_TTS_F_INTERRUPT);
}
static void p_speak_stop(void) { z_speak_stop(); }

static bool p_prompt(const char *title, const char *msg, const char *initial, char *out, int cap) {
	return z_dialog_prompt(&dlg, title, msg, initial, out, cap);
}

static bool p_confirm(const char *title, const char *msg) {
	return z_dialog_confirm(&dlg, title, msg, Z_DIALOG_YES_NO) == Z_DIALOG_YES;
}

static bool p_run(const char *app, const char *arg) {
	if (arg && arg[0]) z_launch_arg_set(arg);
	return z_proc_run(app) != 0;
}

static const lplat_t board = {
	{ p_list, p_read, NULL },
	p_read_at, p_write_whole, p_append, p_mkdir, p_exists,
	p_now, p_ms, p_speak, p_speak_stop, p_prompt, p_confirm, p_run,
};

// -- the screen --

// from a 1bpp buffer (wpl words a row) to the framebuffer, rows below
// `rows` only (the screen's 480, or the colour window's sheet)
static void blit_buf(const uint32_t *src, int wpl, int sx, int sy, int dx, int dy, int w, int h, int rows) {
	for (int j = 0; j < h; j++) {
		int y = dy + j;
		if (y < 0 || y >= rows) continue;
		volatile uint32_t *drow = &VRAM[y * VRAM_WPL];
		const uint32_t *srow = src + (size_t)(sy + j) * wpl;
		int w0 = dx >> 5, w1 = (dx + w - 1) >> 5;
		for (int wi = w0; wi <= w1; wi++) {
			int first = wi * 32, s0 = sx + (first - dx);
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

static void blit_sw(int sx, int sy, int dx, int dy, int w, int h) {
	blit_buf(pagebits, PAGE_WPL, sx, sy, dx, dy, w, h, Z_SCREEN_H);
}

// the page's (px, py, w, h) on the screen: all of it, or a moving figure
static void show_rect(int px, int py, int w, int h) {
	if (z_win_frozen(&win)) return;
	z_clip_t cc;
	z_win_content_rect(&win, &cc);
	int cw = cc.x1 - cc.x0 + 1, ch = cc.y1 - cc.y0 + 1;
	if (cw > page_w) cw = page_w;
	if (ch > page_h) ch = page_h;
	if (px + w > cw) w = cw - px;
	if (py + h > ch) h = ch - py;
	if (w <= 0 || h <= 0 || px < 0 || py < 0) return;
	if (hw_blit) {
		int n = z_gfx_visible_count();
		if (n < 1) n = 1;
		for (int i = 0; i < n; i++) {
			if (!z_gfx_blit_scissor(i, &cc)) continue;
			z_fb_hw_blit_mem(pagebits, PAGE_WPL * 4, px, py, cc.x0 + px, cc.y0 + py, w, h);
		}
		z_gfx_blit_scissor_reset();
		return;
	}
	// the CPU: each visible rectangle of the content, by hand
	for (int i = 0; i < win.clip_n; i++) {
		z_clip_t r = win.clip[i];
		if (r.x0 < cc.x0 + px) r.x0 = cc.x0 + px;
		if (r.y0 < cc.y0 + py) r.y0 = cc.y0 + py;
		if (r.x1 > cc.x0 + px + w - 1) r.x1 = cc.x0 + px + w - 1;
		if (r.y1 > cc.y0 + py + h - 1) r.y1 = cc.y0 + py + h - 1;
		if (r.x1 < r.x0 || r.y1 < r.y0) continue;
		blit_sw(r.x0 - cc.x0, r.y0 - cc.y0, r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1);
	}
}

// -- the GPU device: ldraw's operations as the hardware's --
//
// Two engines, each in order with itself but not with the other: a fill
// (the blitter) and a line (the rasterizer) given one after the other
// could land the other way round. So when the next operation is the
// other engine's, the first is let finish (z_fb_hw_sync()).

typedef struct {
	int		ox, oy;		// the page's (0, 0) on the screen (or in VRAM)
	bool	sheet;		// the colour window's planes: VRAM past the screen, no region
} gpu_t;

static gpu_t gwin;		// the window
static bool gpu;		// drawing through it (else the page is pagebits)
enum { ENG_NONE, ENG_BLIT, ENG_LINE };
static int engine;

static void use(int e) {
	if (engine != e && engine != ENG_NONE) z_fb_hw_sync();
	engine = e;
}

static void g_fill(ld_page_t *p, int x, int y, int w, int h, const ld_ink_t *k) {
	const gpu_t *g = (const gpu_t *)p->ctx;
	int sx = g->ox + x, sy = g->oy + y;
	use(ENG_BLIT);
	switch (k->kind) {
	case LD_K_DARK: z_fb_hw_fill_rect_async(sx, sy, w, h, 0); break;
	case LD_K_INK: z_fb_hw_fill_rect_async(sx, sy, w, h, 1); break;
	case LD_K_XOR: z_fb_hw_fill_rect_rop(sx, sy, w, h, 1, Z_ROP_XOR); break;
	case LD_K_SHADE: z_fb_hw_fill_shade_async(sx, sy, w, h, k->level); break;
	default: z_fb_hw_fill_pattern(sx, sy, w, h, k->pat); break;
	}
}

static void g_line(ld_page_t *p, int x0, int y0, int x1, int y1, int c, const ld_clip_t *cl) {
	const gpu_t *g = (const gpu_t *)p->ctx;
	z_clip_t sc = { g->ox + cl->x0, g->oy + cl->y0, g->ox + cl->x1 - 1, g->oy + cl->y1 - 1 };
	use(ENG_LINE);
	z_fb_hw_line(g->ox + x0, g->oy + y0, g->ox + x1, g->oy + y1,
		c == LD_INK ? Z_RASTER_SET : c == LD_XOR ? Z_RASTER_XOR : Z_RASTER_CLEAR, &sc);
}

static void g_blit(ld_page_t *p, const uint32_t *bits, int wpl, int sx, int sy, int x, int y, int w, int h, int rop) {
	const gpu_t *g = (const gpu_t *)p->ctx;
	use(ENG_BLIT);
	z_fb_hw_blit_mem_async(bits, wpl * 4, sx, sy, g->ox + x, g->oy + y, w, h, rop);
}

static bool g_scroll(ld_page_t *p, int x, int y, int w, int h, int dy) {
	const gpu_t *g = (const gpu_t *)p->ctx;
	if (g->sheet || !z_fb_hw_scroll_allowed(g->ox + x, g->oy + y, w, h)) return false;
	use(ENG_BLIT);
	z_fb_hw_scroll(g->ox + x, g->oy + y, w, h, dy);
	return true;
}

// read back: only pixels that are this window's (nothing over them)
static bool g_save(ld_page_t *p, int x, int y, int w, int h, uint32_t *bits, int wpl) {
	const gpu_t *g = (const gpu_t *)p->ctx;
	int sx = g->ox + x, sy = g->oy + y;
	if (!g->sheet) {
		int n = z_gfx_visible_count();
		bool whole = n == 0;
		for (int i = 0; i < n && !whole; i++) {
			z_clip_t r;
			if (!z_gfx_visible_clip(i, NULL, &r)) continue;
			if (r.x0 <= sx && r.y0 <= sy && r.x1 >= sx + w - 1 && r.y1 >= sy + h - 1) whole = true;
		}
		if (!whole) return false;
	}
	z_fb_hw_sync();
	engine = ENG_NONE;
	for (int j = 0; j < h; j++) {
		volatile uint32_t *row = &VRAM[(sy + j) * VRAM_WPL];
		uint32_t *out = bits + (size_t)j * (size_t)wpl;
		for (int i = 0; i < (w + 31) / 32; i++) {
			int at = sx + i * 32, wi = at >> 5, sh = at & 31;
			uint32_t v = row[wi] >> sh;
			if (sh && wi + 1 < VRAM_WPL) v |= row[wi + 1] << (32 - sh);
			int left = w - i * 32;
			if (left < 32) v &= (1u << left) - 1;
			out[i] = v;
		}
	}
	return true;
}

static const ld_dev_t gpu_dev = { g_fill, g_line, g_blit, g_scroll, g_save };

// Before drawing into the window: where it is, and the visible region
// loaded (z_win_content_rect() does that). False while it is frozen
// (being moved): nothing may be drawn, and all of it is drawn after.
static bool missed;

static bool win_begin(void) {
	if (z_win_frozen(&win)) { missed = true; return false; }
	z_clip_t cc;
	z_win_content_rect(&win, &cc);
	gwin.ox = cc.x0;
	gwin.oy = cc.y0;
	gwin.sheet = false;
	return true;
}

// what was drawn, on the screen: already there, unless the page is pagebits
static void shown(int x, int y, int w, int h) {
	if (!gpu) show_rect(x, y, w, h);
}

// -- the figure in colour: the colour window over it (docs/color.md) --
//
// learn's window is a dual app's (Z_WIN_FLAG_COLOR | _COLOUR_LATER): it
// wants the colour window only while a lesson's figure shows whole, and
// says so (Z_WM_COLOR_WANT). When wm hands it over (Z_WM_COLOR 1), and
// nothing covers the figure, COLWIN is set over it: the page holds the
// colours' lowest bit there, and the window's planes (folded into the
// second VRAM sheet) the rest. Desktop colour keeps the sheet otherwise.

static bool cw_avail;		// the bitstream has the colour window
static bool cw_wanted;		// told wm we want it
static bool cw_focus;		// wm said it is ours
static bool cw_on;			// COLWIN set over the figure
static bool cw_user_off;	// F10: black and white
static int cw_x, cw_y, cw_w, cw_h;		// where (the screen)
static int cw_planes;
static uint32_t cw_frame;	// the figure's frame the planes hold
static bool cw_planes_ok;	// they hold all of it (not stale from before)
#define CW_WPL (320 / 32)
static uint32_t cw_plane[4][CW_WPL * 240 + 2];	// each plane, drawn in memory and kept

static void colour_want(bool on) {
	uint32_t wm;
	if (!cw_avail || on == cw_wanted || win.id < 0 || !z_pid_lookup("wm0", &wm)) return;
	cw_wanted = on;
	z_msg_new_send(wm, Z_WM_COLOR_WANT, (uint32_t)win.id, z_obj_uint32(on ? 1u : 0u));
}

static void colour_drop(void) {
	if (cw_on) {
		if (cw_focus) z_color_window_off();
		z_fb_set_rows(480);
		cw_on = false;
	}
}

// is the page's (x, y, w, h) on the screen with nothing over it?
static bool whole_on_screen(int x, int y, int w, int h) {
	z_clip_t cc;
	z_win_content_rect(&win, &cc);
	int sx = cc.x0 + x, sy = cc.y0 + y;
	int n = z_gfx_visible_count();
	bool whole = false;
	if (n == 0) whole = true;
	for (int i = 0; i < n && !whole; i++) {
		z_clip_t r;
		if (!z_gfx_visible_clip(i, &cc, &r)) continue;
		if (r.x0 <= sx && r.y0 <= sy && r.x1 >= sx + w - 1 && r.y1 >= sy + h - 1) whole = true;
	}
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	return whole;
}

// window plane k's piece of (sx, sy, w, h): where the fold puts it
static void plane_out(int k, int sx, int sy, int w, int h) {
	int rows1 = (int)z_color_rows() - 480;
	if (rows1 <= 0) rows1 = 240;
	int X = (sx + ((k & 1) ? 320 : 0)) % 640, Y = ((sy + ((k & 2) ? 240 : 0)) % 480) % rows1;
	int w1 = w < 640 - X ? w : 640 - X, h1 = h < rows1 - Y ? h : rows1 - Y;
	struct { int x, y, w, h, ox, oy; } piece[4] = {
		{ X, 480 + Y, w1, h1, 0, 0 }, { 0, 480 + Y, w - w1, h1, w1, 0 },
		{ X, 480, w1, h - h1, 0, h1 }, { 0, 480, w - w1, h - h1, w1, h1 },
	};
	int rows = (int)z_color_rows();
	for (int i = 0; i < 4; i++) {
		if (piece[i].w <= 0 || piece[i].h <= 0) continue;
		if (hw_blit) z_fb_hw_blit_mem(cw_plane[k], CW_WPL * 4, piece[i].ox, piece[i].oy, piece[i].x, piece[i].y, piece[i].w, piece[i].h);
		else blit_buf(cw_plane[k], CW_WPL, piece[i].ox, piece[i].oy, piece[i].x, piece[i].y, piece[i].w, piece[i].h, rows);
	}
}

// The planes of the figure; only what its last frame changed when the
// planes hold the frame before it (cw_frame), all of it otherwise.
static void colour_planes(void) {
	int d[4] = { 0, 0, cw_w, cw_h };
	if (cw_frame + 1 == LG.frames && cw_planes_ok) lg_dirty_px(0, 0, cw_w, cw_h, d);
	z_clip_t cc;
	z_win_content_rect(&win, &cc);		// (loads the window's region as the scissor)
	// the planes are in the second VRAM sheet, outside every region:
	// the drawing must not be confined to the window's rectangles
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	int sx = cc.x0 + cw_x, sy = cc.y0 + cw_y;
	for (int k = 0; k < cw_planes && k < 4; k++) {
		// the part that changed, in memory; the plane copied where the fold puts it
		ld_page_t pg;
		ld_page(&pg, cw_plane[k], CW_WPL * 32, cw_h);
		ld_clip_push(&pg, d[0], d[1], d[2], d[3]);
		la_figure_plane(&pg, k + 1);
		plane_out(k, sx, sy, cw_w, cw_h);
	}
	z_fb_hw_sync();
	engine = ENG_NONE;
	cw_frame = LG.frames;
	cw_planes_ok = true;
}

// after the page was drawn: colour on, moved or off as the figure and wm allow
static void colour_update(void) {
	if (!cw_avail) return;
	int x, y, w, h;
	// wanted for as long as the step has a figure, shown whole or not:
	// scrolling must not hand the sheet back and forth with desktop colour
	colour_want(!cw_user_off && la_figure_present());
	bool fig = !cw_user_off && la_figure_present() && la_figure_rect(&x, &y, &w, &h) && w <= 320 && h <= 240;
	bool can = fig && cw_focus && !z_win_frozen(&win) && !z_game_enabled() && whole_on_screen(x, y, w, h);
	if (!can) {
		if (cw_on || cw_planes) {
			colour_drop();
			cw_planes = 0;
			// the figure in black and white again: it alone
			if (win_begin()) { la_figure_recolour(&page, 0); shown(0, 0, page_w, page_h); }
		}
		return;
	}
	if (cw_on && x == cw_x && y == cw_y && w == cw_w && h == cw_h && z_color_window_on()) {
		if (cw_frame != LG.frames) { colour_planes(); cw_frame = LG.frames; }	// a control moved it
		return;
	}
	cw_x = x; cw_y = y; cw_w = w; cw_h = h;
	cw_planes = (int)z_color_window_planes();
	cw_planes_ok = false;
	z_fb_set_rows((int)z_color_rows());
	for (int c = 0; c < (2 << cw_planes); c++) z_color_set_palette(z_color_window_entry((uint32_t)c), lg_colour_rgb(c));
	// the page's part: the lowest bit of each colour
	// (moved, by a scroll: the page's part is there already, the planes are drawn where it is now)
	if (win_begin()) { la_figure_recolour(&page, cw_planes); shown(0, 0, page_w, page_h); }
	colour_planes();
	cw_frame = LG.frames;
	z_clip_t cc;
	z_win_content_rect(&win, &cc);
	z_gfx_clear_visible();
	z_color_window_set(cc.x0 + x, cc.y0 + y, cc.x0 + x + w, cc.y0 + y + h);
	cw_on = true;
}

static void layout(void) {
	int w = (int)z_win_content_w(&win), h = (int)z_win_content_h(&win);
	if (w > Z_SCREEN_W) w = Z_SCREEN_W;
	if (h > Z_SCREEN_H) h = Z_SCREEN_H;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	page_w = w;
	page_h = h;
	if (gpu) ld_page_dev(&page, &gpu_dev, &gwin, w, h);
	else {
		ld_page(&page, pagebits, PAGE_WPL * 32, h);
		page.w = w;
		ld_clip_reset(&page);
	}
	lg_forget_still();
	la_resize(w, h);
}

static void colour_update(void);

static bool flashed;
static int fl_x, fl_y, fl_w, fl_h;

static void flash_xor(void) {
	if (!win_begin()) return;
	ld_clip_reset(&page);
	ld_fill(&page, fl_x, fl_y, fl_w, fl_h, LD_XOR);
	shown(fl_x, fl_y, fl_w, fl_h);
}

static void pressed(const la_ev_t *e) {
	if (flashed || !la_pressed(e, &fl_x, &fl_y, &fl_w, &fl_h)) return;
	flash_xor();
	flashed = true;
}

// the press shown, undone: by the page drawn again over it, or by hand
static void unflash(int x, int y, int w, int h) {
	if (!flashed) return;
	bool covered = x <= fl_x && y <= fl_y && x + w >= fl_x + fl_w && y + h >= fl_y + fl_h;
	if (!covered) flash_xor();
	flashed = false;
}

static char shown_title[96];

// A repaint slower than this is said on the console (printf: the UART),
// with what it was, so a slow screen can be found on the board.
#define SLOW_MS 40

static void repaint(void) {
	int x, y, w, h;
	uint32_t t0 = p_ms();
	if (!win_begin()) return;
	if (!missed && la_partial(&x, &y, &w, &h)) {
		// typing, a figure's control: that part alone, drawn and shown at once
		la_draw_part(&page, x, y, w, h);
		uint32_t t1 = p_ms();
		shown(x, y, w, h);
		unflash(x, y, w, h);
		colour_update();
		uint32_t t2 = p_ms();
		if (t2 - t0 > SLOW_MS)
			printf("learn: part %dx%d at %d,%d: drawn %lu ms, then %lu ms\n", w, h, x, y,
				(unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
		return;
	}
	missed = false;
	if (flashed) { flash_xor(); flashed = false; }	// (a menu is copied only where it changed)
	la_draw(&page);
	uint32_t t1 = p_ms();
	// the title only when it changes: each one is a message to wm, and a titlebar drawn
	const char *t = la_title();
	if (strcmp(t, shown_title)) {
		snprintf(shown_title, sizeof shown_title, "%s", t);
		z_win_set_title(&win, t);
	}
	shown(0, 0, page_w, page_h);
	colour_update();
	uint32_t t2 = p_ms();
	if (t2 - t0 > SLOW_MS)
		printf("learn: page: drawn %lu ms, then %lu ms\n", (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
}

// -- messages --

static void forward_msg(z_msg_t *m, void *user);

static void on_wm(z_msg_t *m) {
	switch (m->subject) {
	case Z_WM_SET_CLIP: z_win_apply_clip(&win, &m->obj); break;
	case Z_WM_REDRAW: {
		uint32_t v = m->obj.val.uint32;
		if (z_win_redraw_id(v) != win.id) break;
		z_win_apply_redraw(&win, v);
		// z_win_apply_redraw() takes any RESIZED waiting behind the REDRAW
		// and applies it without telling us: at start, the maximize's
		// came that way, and the page kept its first size (the footer
		// where the dock had been)
		{
			int cw = (int)z_win_content_w(&win), ch = (int)z_win_content_h(&win);
			if (cw > Z_SCREEN_W) cw = Z_SCREEN_W;
			if (ch > Z_SCREEN_H) ch = Z_SCREEN_H;
			if (cw != page_w || ch != page_h) layout();
		}
		if (win_begin()) {
			missed = false;
			la_forget_page();		// wm's: the window's pixels are not ours to trust
			la_draw(&page);
			shown(0, 0, page_w, page_h);
		}
		flashed = false;
		colour_update();
		z_win_redraw_done(&win);
		break;
	}
	case Z_WM_WINDOW_MOVED: z_win_parse_rect(&win, &m->obj); cw_on = false; break;
	case Z_WM_COLOR:
		// wm cleared COLWIN when it said no
		cw_focus = m->obj.type == Z_UINT32 && m->obj.val.uint32 != 0;
		if (!cw_focus) cw_on = false;
		break;
	case Z_WM_WINDOW_RESIZED: z_win_apply_resized(&win, &m->obj); layout(); break;
	default: break;
	}
}

static void forward_msg(z_msg_t *m, void *user) { (void)user; on_wm(m); }

static void help(void) {
	z_dialog_help(&dlg, "learn help", la_help(), "learn");
}

// A press shows at once: the target turns over (inverted) and is put on
// the screen before the work it starts; the page drawn after puts it back.
static bool handle(z_msg_t *m) {
	la_ev_t e;
	memset(&e, 0, sizeof e);
	switch (m->subject) {
	case Z_WM_KEY: {
		if (m->obj.type != Z_UINT32) return false;
		uint32_t v = m->obj.val.uint32;
		if (!Z_WM_UNPACK_KEY_PRESSED(v)) return false;
		uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(v);
		if (k == Z_KEY_HELP) { help(); return true; }
		if (k == 0x1b && la_opening()) { work_stop(); la_stop_opening(); return true; }
		if (k == Z_KEY_COLOUR && cw_avail) { cw_user_off = !cw_user_off; return true; }
		e.type = LA_KEY;
		e.key = k;
		e.mods = (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(v);
		pressed(&e);
		return la_event(&e);
	}
	case Z_WM_MOUSE: {
		if (m->obj.type != Z_UINT32) return false;
		static uint32_t last_buttons;
		uint32_t v = m->obj.val.uint32;
		uint32_t b = Z_WM_UNPACK_MOUSE_BUTTONS(v);
		int cx, cy;
		bool inside = z_win_mouse_content_xy(&win, v, &cx, &cy);
		bool held = (b & Z_MOUSE_BTN_LEFT) != 0, was = (last_buttons & Z_MOUSE_BTN_LEFT) != 0;
		last_buttons = b;
		e.x = cx;
		e.y = cy;
		if (held && !was) {			// a press: only inside
			if (!inside) return false;
			e.type = LA_DOWN;
		} else if (held) e.type = LA_MOVE;		// a drag (a figure's slider or point)
		else if (was) e.type = LA_UP;
		else return false;
		if (e.type == LA_DOWN) pressed(&e);
		return la_event(&e);
	}
	case Z_WM_WHEEL: {
		if (m->obj.type != Z_UINT32) return false;
		e.type = LA_WHEEL;
		e.notches = Z_WM_WHEEL_NOTCHES(m->obj.val.uint32);
		return la_event(&e);
	}
	case Z_WM_CLOSE:
		la_close();
		return true;
	case Z_WM_COLOR:
		// the colour window is ours now (or not): draw the figure again
		on_wm(m);
		return true;
	default:
		on_wm(m);
		return false;
	}
}

int main(void) {
	uint8_t stack[48 * 1024];		// the work's (above the heap: see "slow work")
	work_stack = stack;
	work_stack_size = sizeof stack;
	char arg[160];
	bool have_arg = z_launch_path_take(arg, sizeof arg);
	if (!mem_setup()) {
		dlg.parent = NULL;
		dlg.on_msg = NULL;
		z_dialog_confirm(&dlg, "learn", "learn needs a board with more\nmemory than this one has.", Z_DIALOG_OK_CANCEL);
		return 1;
	}
	LP = &board;
	la_defer_figures = true;
	ld_fonts_load(load_font);
	void *cache = arena_take(128u << 10);
	ld_init(scratch, sizeof scratch, cache, cache ? 128u << 10 : 0);
	hw_blit = z_fb_hw_blit_mem_available();
	// text is copied with OR and AND-NOT: a blitter without raster ops
	// draws the page in memory instead
	gpu = hw_blit && z_fb_hw_rop_available();

	// opens maximized: the whole screen, the dock hidden (docs/learn.md, "Windows and the dock")
	cw_avail = z_color_window_available();
	if (z_win_create_flags(&win, "learn", 600, 420, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		(cw_avail ? Z_WIN_FLAG_COLOR | Z_WIN_FLAG_COLOUR_LATER : 0)) != Z_OK)
		return 1;
	dlg.parent = &win;
	dlg.on_msg = forward_msg;
	dlg.user = NULL;
	z_win_maximize(&win);

	la_init(arena + arena_used, arena_size - arena_used);
	if (have_arg) la_open_arg(arg);
	layout();

	for (;;) {
		z_msg_t m;
		bool redraw = false;
		while (z_msg_read(&m) == Z_OK) if (handle(&m)) redraw = true;
		if (la_quit()) break;
		if (la_busy() || work_on) {
			// finding courses, opening one, a unit's titles: a request to fs
			// at a time, the window answering between them
			work_step();
			if (la_wants_draw()) redraw = true;		// (only when it changed something)
		}
		if (redraw) repaint();
		else if (flashed) unflash(0, 0, 0, 0);		// a press that changed nothing
		else if (la_figure_due()) {
			// the step's words are up: now its figure
			la_figure_now();
			repaint();
		}
		else if (la_animating()) {
			// a moving figure: its frame alone, about a dozen a second
			// a dozen frames a second at most, and never more than a third
			// of the time: the window answers between them
			static uint32_t last, gap = 80;
			uint32_t t = p_ms();
			int x, y, w, h;
			if (t - last >= gap && win_begin() && la_draw_figure(&page, &x, &y, &w, &h)) {
				shown(x, y, w, h);
				if (cw_on && LG.unchanged && cw_frame + 1 == LG.frames) cw_frame = LG.frames;	// (nothing moved)
				else if (cw_on) { colour_planes(); cw_frame = LG.frames; }
				uint32_t took = p_ms() - t;
				gap = took * 2 > 80 ? took * 2 : 80;
				last = p_ms();
			}
		}
		if (!la_busy() && !work_on) z_proc_wait(la_animating() ? Z_TICK_HZ / 50 : Z_TICK_HZ / 20);
	}
	colour_drop();
	colour_want(false);
	z_win_destroy(&win);
	return 0;
}
