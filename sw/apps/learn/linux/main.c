/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn on Linux (docs/learn_linux.md): the same app as on the board --
 * lapp.c and everything it uses, unchanged -- in an SDL2 window.
 *
 *   make -C sw/apps/learn linux
 *   ./sw/apps/learn/learn-linux ~/ark/edu
 *
 * This file is the window: its events become learn's (la_event), what
 * learn draws (a page of one bit a pixel, ldraw.h) is put on the screen
 * scaled by a whole number, in a dark or a light theme, and a lesson's
 * figure in colour over it, as the board's colour window shows it. The
 * boxes learn asks for (a name, a yes or no, F1's help) are drawn here,
 * in learn's own look, over the page. lplat_linux.c is the rest of the
 * machine: files, the clock, speech.
 *
 * The main loop never waits on learn's slow work: la_work() runs about
 * 15 ms at a time between events (docs/responsive.md), and a step's
 * words show before its figure is run (la_defer_figures), as on the board.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>

#include <SDL.h>

#include "../lapp.h"
#include "../lplat.h"
#include "../ldraw.h"
#include "../lfig.h"
#include "lplat_linux.h"
#include "zkbd.h"
#include "zpaths.h"

const void *ll_font(const char *file, uint32_t *len);		// fonts.c (made by mkfonts.py)

#define BASE_W 640			// learn's page on the board; a scale of 1 fits it
#define BASE_H 480
#define MAX_PAGE_W 2048
#define MAX_PAGE_H 1536

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static uint32_t *pix;			// the page in colour, for the texture

static uint32_t *bits;			// the page: one bit a pixel
static int wpl;
static ld_page_t page;
static int page_w, page_h, scale = 1, user_scale;	// user_scale: Ctrl+=/-, 0 automatic
static int off_x, off_y;		// the page's top left in the window's pixels

static bool light;				// the theme
static uint32_t ink_rgb, paper_rgb, edge_rgb;
static bool colour_on = true;	// F10
static int planes_now;			// the figure in colour (4) or not (0)
static uint32_t cplane[4][(320 / 32) * 240 + 2];
static bool quit_now;
static bool full_after;			// a box was over the page: all of it drawn again

static uint8_t mem[4u << 20];
static uint8_t scratch[96 << 10], cache[128 << 10];

static void set_theme(bool l) {
	light = l;
	ink_rgb = light ? 0xFF1E1E1Eu : 0xFFE6E6E6u;
	paper_rgb = light ? 0xFFF4F1EAu : 0xFF000000u;
	edge_rgb = light ? 0xFFDCD8CFu : 0xFF181818u;
}

static uint32_t rgb12(uint32_t c) {
	uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
	return 0xFF000000u | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
}

// -- the page and its size --

static void layout(void) {
	int ow, oh;
	SDL_GetRendererOutputSize(ren, &ow, &oh);
	int s = user_scale;
	if (!s) {
		s = ow / BASE_W < oh / BASE_H ? ow / BASE_W : oh / BASE_H;
		if (s < 1) s = 1;
	}
	scale = s;
	int w = ow / s, h = oh / s;
	if (w > MAX_PAGE_W) w = MAX_PAGE_W;
	if (h > MAX_PAGE_H) h = MAX_PAGE_H;
	if (w < 200) w = 200;
	if (h < 160) h = 160;
	off_x = (ow - w * s) / 2;
	off_y = (oh - h * s) / 2;
	if (off_x < 0) off_x = 0;
	if (off_y < 0) off_y = 0;
	if (w != page_w || h != page_h) {
		page_w = w;
		page_h = h;
		wpl = (w + 31) / 32;
		free(bits);
		bits = calloc((size_t)wpl * (size_t)h + 2, 4);
		free(pix);
		pix = malloc((size_t)w * (size_t)h * 4);
		if (tex) SDL_DestroyTexture(tex);
		tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		tex_w = w;
		tex_h = h;
		if (!bits || !pix || !tex) { fprintf(stderr, "learn: out of memory for a %dx%d page\n", w, h); exit(1); }
		ld_page(&page, bits, wpl * 32, h);
		page.w = w;
		ld_clip_reset(&page);
		lg_forget_still();
		la_resize(w, h);
	}
	la_forget_page();
}

// the figure in colour when it shows whole and asks for it (the board's
// colour_update(), learn.c): the page holds the colours' lowest bit there
static int fig_x, fig_y, fig_w, fig_h;

static void colour_sync(void) {
	int x, y, w, h;
	bool want = colour_on && la_figure_present() && la_figure_rect(&x, &y, &w, &h) && w <= 320 && h <= 240;
	int planes = want ? 4 : 0;
	if (planes != planes_now) {
		la_figure_recolour(&page, planes);
		planes_now = planes;
	}
	if (!planes_now) return;
	fig_x = x; fig_y = y; fig_w = w; fig_h = h;
	for (int k = 0; k < 4; k++) {
		ld_page_t pg;
		ld_page(&pg, cplane[k], 320, 240);
		ld_clear(&pg);
		la_figure_plane(&pg, k + 1);
	}
}

static inline int bit_at(const uint32_t *b, int w32, int x, int y) { return (b[y * w32 + (x >> 5)] >> (x & 31)) & 1; }

static void present(void) {
	for (int y = 0; y < page_h; y++) {
		const uint32_t *row = bits + (size_t)y * (size_t)wpl;
		uint32_t *out = pix + (size_t)y * (size_t)page_w;
		for (int x = 0; x < page_w; x++) out[x] = (row[x >> 5] >> (x & 31)) & 1 ? ink_rgb : paper_rgb;
	}
	if (planes_now) {
		for (int y = 0; y < fig_h && fig_y + y < page_h; y++)
			for (int x = 0; x < fig_w && fig_x + x < page_w; x++) {
				int c = bit_at(bits, wpl, fig_x + x, fig_y + y);
				for (int k = 0; k < 4; k++) c |= bit_at(cplane[k], 320 / 32, x, y) << (k + 1);
				pix[(size_t)(fig_y + y) * (size_t)page_w + (size_t)(fig_x + x)] = rgb12(lg_colour_rgb(c));
			}
	}
	SDL_UpdateTexture(tex, NULL, pix, page_w * 4);
	SDL_SetRenderDrawColor(ren, (edge_rgb >> 16) & 255, (edge_rgb >> 8) & 255, edge_rgb & 255, 255);
	SDL_RenderClear(ren);
	SDL_Rect dst = { off_x, off_y, page_w * scale, page_h * scale };
	SDL_RenderCopy(ren, tex, NULL, &dst);
	SDL_RenderPresent(ren);
}

static char shown_title[128];

static void draw_all(void) {
	la_draw(&page);
	colour_sync();
	const char *t = la_title();
	if (strcmp(t, shown_title)) {
		snprintf(shown_title, sizeof shown_title, "%s", t);
		SDL_SetWindowTitle(win, t);
	}
	present();
}

// what changed: a part (typing, a control, a scroll), or the page
static void repaint(void) {
	int x, y, w, h;
	if (!full_after && la_partial(&x, &y, &w, &h)) {
		la_draw_part(&page, x, y, w, h);
		colour_sync();
		present();
		return;
	}
	full_after = false;
	draw_all();
}

// -- boxes over the page, in learn's look (the board's are zdialog's) --

static uint32_t *saved;

static void box_begin(void) {
	size_t n = (size_t)wpl * (size_t)page_h + 2;
	saved = malloc(n * 4);
	if (saved) memcpy(saved, bits, n * 4);
}

static void box_end(void) {
	if (saved) { memcpy(bits, saved, ((size_t)wpl * (size_t)page_h + 2) * 4); free(saved); saved = NULL; }
	full_after = true;
	la_forget_page();
	present();
}

static void box_restore(void) { if (saved) memcpy(bits, saved, ((size_t)wpl * (size_t)page_h + 2) * 4); }

// a box of width bw and height bh in the middle, its title; returns its top left
static void box_frame(int bw, int bh, const char *title, int *bx, int *by) {
	ld_clip_reset(&page);
	if (bw > page_w - 16) bw = page_w - 16;
	if (bh > page_h - 16) bh = page_h - 16;
	*bx = (page_w - bw) / 2;
	*by = (page_h - bh) / 2;
	ld_fill(&page, *bx - 3, *by - 3, bw + 6, bh + 6, LD_DARK);
	ld_frame(&page, *bx, *by, bw, bh, 2, LD_INK);
	ld_fill(&page, *bx, *by, bw, 24, LD_INK);
	ld_style_t tb = ld_st(&LDF.body_b, 0);
	ld_fitz(&page, &tb, *bx + 10, *by + 17, bw - 20, title, LD_DARK);
}

static void put_utf8(char *s, int cap, uint32_t cp) {
	int len = (int)strlen(s), n = 0;
	char u[4];
	if (cp < 0x80) u[n++] = (char)cp;
	else if (cp < 0x800) { u[n++] = (char)(0xC0 | (cp >> 6)); u[n++] = (char)(0x80 | (cp & 0x3F)); }
	else if (cp < 0x10000) { u[n++] = (char)(0xE0 | (cp >> 12)); u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[n++] = (char)(0x80 | (cp & 0x3F)); }
	else { u[n++] = (char)(0xF0 | (cp >> 18)); u[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[n++] = (char)(0x80 | (cp & 0x3F)); }
	if (len + n >= cap) return;
	memcpy(s + len, u, (size_t)n);
	s[len + n] = 0;
}

static void back_utf8(char *s) {
	int n = (int)strlen(s);
	while (n > 0 && ((unsigned char)s[n - 1] & 0xC0) == 0x80) n--;
	if (n > 0) n--;
	s[n] = 0;
}

static void resized(void);

// Waits for the box's answer. kind 0: text (out), 1: yes or no, 2: OK.
// Returns true for OK / yes.
static bool box_run(int kind, const char *title, const char *msg, char *out, int cap) {
	box_begin();
	if (kind == 0) SDL_StartTextInput();
	bool result = false, done = false;
	while (!done && !quit_now) {
		box_restore();
		ld_style_t sm = ld_st(&LDF.small, 0), big = ld_st(&LDF.big, 0);
		int bw = 440, lines = ld_wrap_lines(&sm, bw - 24, msg);
		int bh = 24 + 14 + lines * 16 + (kind == 0 ? 44 : 0) + 40;
		int bx, by;
		box_frame(bw, bh, title, &bx, &by);
		int y = ld_wrap(&page, &sm, bx + 12, by + 24 + 18, bw - 24, 16, msg, LD_INK);
		if (kind == 0) {
			int fy = y + 4;
			ld_frame(&page, bx + 12, fy, bw - 24, 30, 2, LD_INK);
			int tx = ld_fitz(&page, &big, bx + 20, fy + 21, bw - 44, out, LD_INK);
			ld_fill(&page, tx + 1, fy + 5, 2, 20, LD_INK);
		}
		ld_keys(&page, bx + 12, by + bh - 12, kind == 0 ? "Enter|OK;Esc|cancel" : kind == 1 ? "Y|yes;N|no;Esc|cancel" : "Enter|OK", LD_INK);
		present();
		SDL_Event ev;
		if (!SDL_WaitEvent(&ev)) break;
		do {
			if (ev.type == SDL_QUIT) { quit_now = true; done = true; }
			else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				// the page's size changes: the box is drawn over the new one
				free(saved); saved = NULL;
				resized();
				la_draw(&page);
				box_begin();
			} else if (ev.type == SDL_TEXTINPUT && kind == 0) {
				const unsigned char *t = (const unsigned char *)ev.text.text;
				while (*t) {
					uint32_t cp = *t++;
					int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
					if (more) cp &= 0x3Fu >> more;
					while (more-- && (*t & 0xC0) == 0x80) cp = cp << 6 | (*t++ & 0x3Fu);
					if (cp >= 0x20) put_utf8(out, cap, cp);
				}
			} else if (ev.type == SDL_KEYDOWN) {
				SDL_Keycode k = ev.key.keysym.sym;
				if (k == SDLK_ESCAPE) { result = false; done = true; }
				else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { result = true; done = true; }
				else if (kind == 0 && k == SDLK_BACKSPACE) back_utf8(out);
				else if (kind == 1 && k == SDLK_y) { result = true; done = true; }
				else if (kind == 1 && k == SDLK_n) { result = false; done = true; }
			}
		} while (!done && SDL_PollEvent(&ev));
	}
	if (kind == 0) SDL_StopTextInput();
	box_end();
	if (kind == 0) SDL_StartTextInput();
	return result && !quit_now;
}

static bool b_prompt(const char *title, const char *msg, const char *initial, char *out, int cap) {
	char t[256];
	snprintf(t, sizeof t, "%s", initial ? initial : "");
	bool ok = box_run(0, title, msg ? msg : "", t, sizeof t);
	if (ok) snprintf(out, (size_t)cap, "%s", t);
	return ok;
}

static bool b_confirm(const char *title, const char *msg) { return box_run(1, title, msg ? msg : "", NULL, 0); }

// F1: la_help()'s lines, "key<TAB>what" in two columns
static void help_box(void) {
	box_begin();
	bool done = false;
	while (!done && !quit_now) {
		box_restore();
		ld_style_t s6 = ld_st(&LDF.f6, 0), s6b = ld_st(&LDF.small_b, 0);
		const char *h = la_help();
		int n = 1;
		for (const char *c = h; *c; c++) n += *c == '\n';
		int bw = 470, bh = 24 + 12 + n * 13 + 36 + 26;
		int bx, by;
		box_frame(bw, bh, "learn help", &bx, &by);
		int y = by + 24 + 18;
		const char *c = h;
		while (*c) {
			const char *e = strchr(c, '\n');
			int len = e ? (int)(e - c) : (int)strlen(c);
			const char *tab = memchr(c, '\t', (size_t)len);
			char line[160];
			if (tab) {
				snprintf(line, sizeof line, "%.*s", (int)(tab - c), c);
				ld_textz(&page, &s6b, bx + 14, y, line, LD_INK);
				snprintf(line, sizeof line, "%.*s", (int)(len - (tab - c) - 1), tab + 1);
				ld_textz(&page, &s6, bx + 140, y, line, LD_INK);
			} else {
				snprintf(line, sizeof line, "%.*s", len, c);
				ld_textz(&page, &s6, bx + 14, y, line, LD_INK);
			}
			y += 13;
			c = e ? e + 1 : c + len;
		}
		y += 8;
		ld_textz(&page, &s6, bx + 14, y, "On Linux: F11 full screen, F10 colour, Ctrl+= and Ctrl+-", LD_INK);
		ld_textz(&page, &s6, bx + 14, y + 13, "the size, Ctrl+0 as it fits, Ctrl+T light or dark.", LD_INK);
		ld_keys(&page, bx + 14, by + bh - 10, "Esc|close", LD_INK);
		present();
		SDL_Event ev;
		if (!SDL_WaitEvent(&ev)) break;
		do {
			if (ev.type == SDL_QUIT) quit_now = true;
			else if (ev.type == SDL_KEYDOWN) {
				SDL_Keycode k = ev.key.keysym.sym;
				if (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_F1 || k == SDLK_KP_ENTER) done = true;
			} else if (ev.type == SDL_MOUSEBUTTONDOWN) done = true;
			else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				free(saved); saved = NULL;
				resized();
				la_draw(&page);
				box_begin();
			}
		} while (!done && SDL_PollEvent(&ev));
	}
	box_end();
}

// -- another app: a `try` step, a link (lstep.c) --

static bool spawn(const char *prog, const char *arg) {
	pid_t pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		setsid();
		execlp(prog, prog, arg, (char *)NULL);
		_exit(127);
	}
	return true;
}

static bool b_run(const char *app, const char *arg) {
	// a web page opens in this computer's browser
	if (!strcmp(app, "web") && arg && (!strncmp(arg, "http://", 7) || !strncmp(arg, "https://", 8)))
		return spawn("xdg-open", arg);
	char msg[400];
	if (!strcmp(app, "read") && arg && !strncmp(arg, Z_DIR_DOCS, strlen(Z_DIR_DOCS)))
		snprintf(msg, sizeof msg, "This link opens a page of Zeitlos's documentation, %s, in read on Zeitlos. "
			"In the Zeitlos source it is docs%s.", arg + strlen(Z_DIR_DOCS), arg + strlen(Z_DIR_DOCS));
	else
		snprintf(msg, sizeof msg, "This step opens %s on Zeitlos%s%s. On Linux, learn cannot open it: "
			"do the step on a Zeitlos computer, or go on.", app, arg && arg[0] ? " with " : "", arg && arg[0] ? arg : "");
	box_run(2, "On Zeitlos", msg, NULL, 0);
	return true;
}

// -- keys and the mouse --

static uint8_t mods_of(Uint16 m) {
	uint8_t z = 0;
	if (m & KMOD_LCTRL) z |= Z_KBD_MOD_LCTRL;
	if (m & KMOD_RCTRL) z |= Z_KBD_MOD_RCTRL;
	if (m & KMOD_LSHIFT) z |= Z_KBD_MOD_LSHIFT;
	if (m & KMOD_RSHIFT) z |= Z_KBD_MOD_RSHIFT;
	if (m & KMOD_LALT) z |= Z_KBD_MOD_LALT;
	if (m & KMOD_RALT) z |= Z_KBD_MOD_RALT;
	if (m & KMOD_LGUI) z |= Z_KBD_MOD_LGUI;
	if (m & KMOD_RGUI) z |= Z_KBD_MOD_RGUI;
	return z;
}

// a key that is not text, as a Zeitlos keysym (zkbd.h); 0 for none.
// Text -- letters, digits, Space, ä, ß -- comes as SDL_TEXTINPUT.
static uint32_t keysym(SDL_Keycode k) {
	switch (k) {
	case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0d;
	case SDLK_ESCAPE: return 0x1b;
	case SDLK_BACKSPACE: return 0x7f;
	case SDLK_TAB: return 0x09;
	case SDLK_UP: return Z_KEY_UP;
	case SDLK_DOWN: return Z_KEY_DOWN;
	case SDLK_LEFT: return Z_KEY_LEFT;
	case SDLK_RIGHT: return Z_KEY_RIGHT;
	case SDLK_HOME: return Z_KEY_HOME;
	case SDLK_END: return Z_KEY_END;
	case SDLK_PAGEUP: return Z_KEY_PAGEUP;
	case SDLK_PAGEDOWN: return Z_KEY_PAGEDOWN;
	case SDLK_INSERT: return Z_KEY_INSERT;
	case SDLK_DELETE: return Z_KEY_DELETE;
	case SDLK_F2: return Z_KEY_F2;
	case SDLK_F3: return Z_KEY_F3;
	case SDLK_F4: return Z_KEY_F4;
	case SDLK_F5: return Z_KEY_F5;
	case SDLK_F6: return Z_KEY_F6;
	case SDLK_F7: return Z_KEY_F7;
	case SDLK_F8: return Z_KEY_F8;
	case SDLK_F12: return Z_KEY_F12;
	default: return 0;
	}
}

static bool send_key(uint32_t k, uint8_t mods) {
	la_ev_t e;
	memset(&e, 0, sizeof e);
	e.type = LA_KEY;
	e.key = k;
	e.mods = mods;
	return la_event(&e);
}

static bool to_page(int wx, int wy, int *px, int *py) {
	int ww, wh, ow, oh;
	SDL_GetWindowSize(win, &ww, &wh);
	SDL_GetRendererOutputSize(ren, &ow, &oh);
	int x = ww ? wx * ow / ww : wx, y = wh ? wy * oh / wh : wy;
	*px = (x - off_x) / scale;
	*py = (y - off_y) / scale;
	return *px >= 0 && *py >= 0 && *px < page_w && *py < page_h;
}

static void resized(void) {
	layout();
}

static void set_scale(int s) {
	user_scale = s;
	layout();
	full_after = true;
}

// -- the program --

static void usage(FILE *f) {
	fprintf(f,
		"usage: learn-linux [options] [FOLDER...]\n"
		"\n"
		"A FOLDER is one course (a folder with course.cfg), a pack (a folder of\n"
		"courses, like an Ark checkout's edu/), or a folder of packs. With none,\n"
		"the packs in $XDG_DATA_HOME/learn/packs (~/.local/share/learn/packs).\n"
		"One course opens at once.\n"
		"\n"
		"  --light, --dark     the theme (dark is the board's look)\n"
		"  --scale N           pixels a page pixel (1..6); without it, as the window fits\n"
		"  --no-colour         figures in black and white (F10 switches)\n"
		"  --quiet             no speech (otherwise espeak-ng, if it is installed)\n"
		"  --data DIR          learners and progress in DIR (default $XDG_DATA_HOME/learn)\n"
		"  --help\n"
		"\n"
		"docs/learn_linux.md in the Zeitlos source says more.\n");
}

int main(int argc, char **argv) {
	char data[PATH_MAX] = "", open_arg[PATH_MAX + 64] = "";
	bool any_folder = false;
	int want_scale = 0;
	set_theme(false);
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
		else if (!strcmp(a, "--light")) set_theme(true);
		else if (!strcmp(a, "--dark")) set_theme(false);
		else if (!strcmp(a, "--no-colour") || !strcmp(a, "--no-color")) colour_on = false;
		else if (!strcmp(a, "--quiet")) ll_speech(false);
		else if (!strcmp(a, "--scale") && i + 1 < argc) want_scale = atoi(argv[++i]);
		else if (!strcmp(a, "--data") && i + 1 < argc) snprintf(data, sizeof data, "%s", argv[++i]);
		else if (a[0] == '-') { fprintf(stderr, "learn: what is %s?\n\n", a); usage(stderr); return 2; }
		else {
			char one[PATH_MAX + 64];
			if (!ll_add_folder(a, one, sizeof one)) fprintf(stderr, "learn: %s holds no courses\n", a);
			if (one[0] && !open_arg[0]) snprintf(open_arg, sizeof open_arg, "%s", one);
			any_folder = true;
		}
	}
	if (want_scale < 0 || want_scale > 6) { fprintf(stderr, "learn: --scale is 1 to 6\n"); return 2; }
	const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	char base[PATH_MAX];
	if (xdg && xdg[0] == '/') snprintf(base, sizeof base, "%s/learn", xdg);
	else snprintf(base, sizeof base, "%s/.local/share/learn", home ? home : ".");
	if (!data[0]) snprintf(data, sizeof data, "%s", base);
	if (!ll_set_data(data)) { fprintf(stderr, "learn: cannot make %s for the learners' progress\n", data); return 1; }
	if (!any_folder) {
		char packs[PATH_MAX + 8];
		snprintf(packs, sizeof packs, "%s/packs", base);
		ll_add_folder(packs, NULL, 0);
	}
	if (!ll_npacks())
		fprintf(stderr, "learn: no courses found. Name a folder of courses, e.g.\n"
			"  learn-linux ~/ark/edu\n(an Ark checkout: git clone https://github.com/machdyne/ark)\n");
	signal(SIGCHLD, SIG_IGN);		// browsers started for links: nobody waits for them

	linux_plat.prompt = b_prompt;
	linux_plat.confirm = b_confirm;
	linux_plat.run = b_run;
	LP = &linux_plat;

	if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "learn: SDL: %s\n", SDL_GetError()); return 1; }
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	// a window twice the board's page if the screen has room for it
	int ww = BASE_W, wh = BASE_H;
	SDL_DisplayMode dm;
	if (!SDL_GetDesktopDisplayMode(0, &dm) && dm.w >= BASE_W * 2 + 100 && dm.h >= BASE_H * 2 + 120) { ww *= 2; wh *= 2; }
	if (want_scale) { ww = BASE_W * want_scale; wh = BASE_H * want_scale; }
	win = SDL_CreateWindow("learn", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!win) { fprintf(stderr, "learn: SDL: %s\n", SDL_GetError()); return 1; }
	SDL_SetWindowMinimumSize(win, 320, 240);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (!ren) { fprintf(stderr, "learn: SDL: %s\n", SDL_GetError()); return 1; }
	user_scale = want_scale;

	ld_fonts_load(ll_font);
	ld_init(scratch, sizeof scratch, cache, sizeof cache);
	la_defer_figures = true;
	la_init(mem, sizeof mem);
	if (open_arg[0]) la_open_arg(open_arg);
	layout();
	SDL_StartTextInput();
	draw_all();

	bool held = false;			// the left button, for drags
	uint32_t last_frame = 0;
	while (!quit_now) {
		bool redraw = false;
		int wait = la_busy() ? 0 : la_animating() ? 15 : la_figure_due() ? 0 : 100;
		SDL_Event ev;
		bool got = wait ? SDL_WaitEventTimeout(&ev, wait) : SDL_PollEvent(&ev);
		while (got) {
			switch (ev.type) {
			case SDL_QUIT: la_close(); quit_now = true; break;
			case SDL_WINDOWEVENT:
				if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { resized(); full_after = redraw = true; }
				else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) { present(); }
				break;
			case SDL_KEYDOWN: {
				SDL_Keycode k = ev.key.keysym.sym;
				Uint16 m = ev.key.keysym.mod;
				bool ctrl = (m & KMOD_CTRL) != 0;
				if (k == SDLK_F1) { help_box(); redraw = true; break; }
				if (k == SDLK_F10) { colour_on = !colour_on; full_after = redraw = true; break; }
				if (k == SDLK_F11) {
					bool fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
					SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
					break;
				}
				if (ctrl && (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS)) { set_scale((user_scale ? user_scale : scale) + 1 > 6 ? 6 : (user_scale ? user_scale : scale) + 1); redraw = true; break; }
				if (ctrl && (k == SDLK_MINUS || k == SDLK_KP_MINUS)) { int s = (user_scale ? user_scale : scale) - 1; set_scale(s < 1 ? 1 : s); redraw = true; break; }
				if (ctrl && (k == SDLK_0 || k == SDLK_KP_0)) { set_scale(0); redraw = true; break; }
				if (ctrl && k == SDLK_t) { set_theme(!light); full_after = redraw = true; break; }
				if (ctrl && k == SDLK_q) { la_close(); quit_now = true; break; }
				if (k == SDLK_ESCAPE && la_opening()) { la_stop_opening(); redraw = true; break; }
				uint32_t z = keysym(k);
				if (z && send_key(z, mods_of(m))) redraw = true;
				break;
			}
			case SDL_TEXTINPUT: {
				// a key a code point: ä and ß as learn's answer boxes take them
				const unsigned char *t = (const unsigned char *)ev.text.text;
				while (*t) {
					uint32_t cp = *t++;
					int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
					if (more) cp &= 0x3Fu >> more;
					while (more-- && (*t & 0xC0) == 0x80) cp = cp << 6 | (*t++ & 0x3Fu);
					if (cp >= 0x20 && send_key(cp, 0)) redraw = true;
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
			case SDL_MOUSEMOTION: {
				la_ev_t e;
				memset(&e, 0, sizeof e);
				int mx = ev.type == SDL_MOUSEMOTION ? ev.motion.x : ev.button.x;
				int my = ev.type == SDL_MOUSEMOTION ? ev.motion.y : ev.button.y;
				bool in = to_page(mx, my, &e.x, &e.y);
				if (ev.type == SDL_MOUSEBUTTONDOWN) {
					if (ev.button.button != SDL_BUTTON_LEFT || !in) break;
					held = true;
					e.type = LA_DOWN;
				} else if (ev.type == SDL_MOUSEBUTTONUP) {
					if (ev.button.button != SDL_BUTTON_LEFT || !held) break;
					held = false;
					e.type = LA_UP;
				} else {
					if (!held) break;
					e.type = LA_MOVE;
				}
				if (la_event(&e)) redraw = true;
				break;
			}
			case SDL_MOUSEWHEEL: {
				la_ev_t e;
				memset(&e, 0, sizeof e);
				e.type = LA_WHEEL;
				e.notches = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y;
				if (e.notches && la_event(&e)) redraw = true;
				break;
			}
			default: break;
			}
			got = SDL_PollEvent(&ev);
		}
		if (la_quit()) break;
		if (la_busy()) {
			// finding courses, opening one: a slice, then the window again
			la_work(15);
			if (la_wants_draw()) redraw = true;
		}
		if (redraw || la_wants_draw()) repaint();
		else if (la_figure_due()) {
			// the step's words are up: now its figure
			la_figure_now();
			repaint();
		} else if (la_animating()) {
			// a moving figure: its frame alone, a dozen a second
			uint32_t t = SDL_GetTicks();
			int x, y, w, h;
			if (t - last_frame >= 80 && la_draw_figure(&page, &x, &y, &w, &h)) {
				colour_sync();
				present();
				last_frame = t;
			}
		}
	}
	if (linux_plat.speak_stop) linux_plat.speak_stop();
	SDL_StopTextInput();
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
