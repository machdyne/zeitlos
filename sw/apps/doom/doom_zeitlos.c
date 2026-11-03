/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * doom -- doomgeneric (sw/ext/doomgeneric, fetched by `make fetch`) on
 * Zeitlos: its platform layer (the DG_* functions), main(), the WAD
 * reader and the three wrapped engine functions. See docs/doom_app.md.
 *
 * The screen: chunky game mode (docs/color.md, "Chunky"), 320x240 at a
 * byte a pixel, so the engine's own 320x200 picture goes into VRAM as
 * it is, 20 rows down, at the frame boundary (tear-free: VRAM holds one
 * picture, and a copy started at the boundary stays ahead of the beam).
 * Its palette goes into the 256-entry palette there too. On a bitstream
 * without chunky mode, and in the simulator, it runs headless: nothing
 * is shown, and with -timedemo the result is printed.
 *
 * Keys come from wm while the game holds the screen, both presses and
 * releases; with Z_WM_GAME_GRAB_MODIFIERS wm also sends Ctrl, Shift and
 * Alt on their own, which Doom uses as buttons (fire, run, strafe).
 * The pad works too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "w_file.h"

#include "zeitlos.h"
#include "zsoc.h"
#include "zwm.h"
#include "zkbd.h"
#include "zpad.h"
#include "zfsapp.h"
#include "zdata.h"
#include "zwin.h"

/* i_video.c's palette, with CMAP256 (it has no header for them) */
extern boolean palette_changed;
extern struct color colors[256];

/* doomgeneric.c's, which is not linked. i_video.c's I_FinishUpdate()
 * would copy the engine's screen here and then call DG_DrawFrame();
 * it is wrapped (below) to go to VRAM straight from I_VideoBuffer, a
 * 64,000-byte copy through main memory less a frame, so this is only
 * the symbol. */
pixel_t *DG_ScreenBuffer = NULL;
extern byte *I_VideoBuffer;

void M_FindResponseFile(void);
void D_DoomMain(void);
void doomgeneric_Tick(void);

#define DZ_TOP ((Z_CHUNKY_H - SCREENHEIGHT) / 2)   /* 20 rows above, 20 below */

static bool dz_shown;          /* chunky game mode is ours */
static bool dz_tearing;        /* -notvsync: copy at once, do not wait */
static uint32_t dz_frames;
static uint32_t dz_late;
static uint32_t dz_last_frame;

static bool dz_fps_on;         /* -fps: frames a second, above the picture */
static uint32_t dz_fps_t, dz_fps_n;
static uint32_t dz_i0, dz_c0, dz_t0;  /* at the first frame: instret, cycle, ticks */

static char dz_wad[Z_FS_PATH_MAX];
static char dz_dir[Z_FS_PATH_MAX];

/* -- the screen --------------------------------------------------------- */

static void dz_restore(void) {
	uint32_t wm_pid;
	if (!dz_shown) return;
	dz_shown = false;
	z_color_set_chunky(false);
	z_game_set_enabled(false, false);
	if (z_pid_lookup("wm0", &wm_pid))
		z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));
}

/* `n` rows of `src` (320 bytes each, word aligned) to screen rows y0
 * up, top to bottom: z_chunky_put_rows() with the source's own first
 * row, not the screen's */
static void dz_put_rows(const uint8_t *src, uint32_t y0, uint32_t n) {
	uint32_t y, s, i;
	const uint32_t *w = (const uint32_t *)src;
	for (y = y0; y < y0 + n; y++) {
		for (s = 0; s < 4; s++) {
			volatile uint32_t *d = z_chunky_seg(y, s);
			for (i = 0; i < Z_CHUNKY_SEG_WORDS; i += 4) {
				d[i] = w[0];
				d[i + 1] = w[1];
				d[i + 2] = w[2];
				d[i + 3] = w[3];
				w += 4;
			}
		}
	}
}

static void dz_palette(void) {
	int i;
	for (i = 0; i < 256; i++)
		z_color_set_palette((uint32_t)i,
			((uint32_t)(colors[i].r >> 4) << 8) |
			((uint32_t)(colors[i].g >> 4) << 4) |
			((uint32_t)(colors[i].b >> 4)));
	palette_changed = false;
}

void DG_Init(void) {

	static uint32_t black[Z_CHUNKY_W / 4];
	uint32_t y;

	if (M_CheckParm("-headless") || !z_color_chunky_available()) {
		printf("doom: no chunky game mode here: running without a picture\n");
		return;
	}
	dz_tearing = M_CheckParm("-notvsync") != 0;
	dz_fps_on = M_CheckParm("-fps") != 0;
	dz_fps_t = z_uptime_ticks();

	for (y = 0; y < Z_CHUNKY_H; y++) dz_put_rows((const uint8_t *)black, y, 1);

	z_game_set_modifiers(true);
	z_game_set_enabled(true, false);
	z_game_set_view(0, 0);
	z_color_set_chunky(true);
	dz_shown = true;
	I_AtExit(dz_restore, true);
	z_game_wait_frame();
	dz_last_frame = z_game_frame();
}

static inline uint32_t dz_instret(void) {
	uint32_t v;
	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
}

static inline uint32_t dz_cycle(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

/* With -stats (and always without a picture): what the frames cost, at
 * the end. Instructions and cycles a frame, frames a second (by the
 * tick, which in the simulator is the host's clock). */
static void dz_stats(void) {
	uint32_t di, dc, dt;
	if (dz_frames < 2) return;
	di = dz_instret() - dz_i0;
	dc = dz_cycle() - dz_c0;
	dt = z_uptime_ticks() - dz_t0;
	printf("doom: %lu frames, %lu instructions and %lu cycles a frame, "
		"%lu.%02lu fps, %lu late\n",
		(unsigned long)dz_frames, (unsigned long)(di / (dz_frames - 1)),
		(unsigned long)(dc / (dz_frames - 1)),
		(unsigned long)(dt ? (uint64_t)(dz_frames - 1) * Z_TICK_HZ / dt : 0),
		(unsigned long)(dt ? ((uint64_t)(dz_frames - 1) * Z_TICK_HZ * 100 / dt) % 100 : 0),
		(unsigned long)dz_late);
}

/* -fps: the frame rate in the black band above the picture (rows 0-19,
 * which the engine never draws), redrawn once a second. Digits 3x5,
 * doubled; palette index 4, white in Doom's palette. */
static void dz_fps_draw(uint32_t x10) {
	static const uint16_t glyph[11] = {   /* 0-9 and '.', 3x5, row-major */
		0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249,
		0x7bef, 0x7bcf, 0x0002 };
	static uint8_t band[DZ_TOP * SCREENWIDTH] __attribute__((aligned(4)));
	char t[8];
	int n = 0, i, gx, gy;
	uint32_t v = x10 > 9999 ? 9999 : x10;
	if (v >= 1000) t[n++] = (char)('0' + v / 1000);
	if (v >= 100) t[n++] = (char)('0' + (v / 100) % 10);
	t[n++] = (char)('0' + (v / 10) % 10);
	t[n++] = '.';
	t[n++] = (char)('0' + v % 10);
	memset(band, 0, sizeof(band));
	for (i = 0; i < n; i++) {
		uint16_t g = glyph[t[i] == '.' ? 10 : t[i] - '0'];
		for (gy = 0; gy < 5; gy++)
			for (gx = 0; gx < 3; gx++)
				if (g & (1u << (14 - (gy * 3 + gx)))) {
					int x = 4 + i * 8 + gx * 2, y = 5 + gy * 2;
					band[y * SCREENWIDTH + x] = 4;
					band[y * SCREENWIDTH + x + 1] = 4;
					band[(y + 1) * SCREENWIDTH + x] = 4;
					band[(y + 1) * SCREENWIDTH + x + 1] = 4;
				}
	}
	dz_put_rows(band, 0, DZ_TOP);
}

void DG_DrawFrame(void) {

	if (dz_frames == 0) {
		dz_i0 = dz_instret();
		dz_c0 = dz_cycle();
		dz_t0 = z_uptime_ticks();
		if (!dz_shown || M_CheckParm("-stats")) I_AtExit(dz_stats, true);
	}
	dz_frames++;
	if (!dz_shown) return;

	if (!dz_tearing) {
		z_game_wait_frame();
		if ((uint16_t)(z_game_frame() - dz_last_frame) > 1) dz_late++;
		dz_last_frame = z_game_frame();
	}
	if (palette_changed) dz_palette();
	dz_put_rows(I_VideoBuffer, DZ_TOP, SCREENHEIGHT);

	if (dz_fps_on) {
		uint32_t now = z_uptime_ticks();
		dz_fps_n++;
		if (now - dz_fps_t >= Z_TICK_HZ) {
			dz_fps_draw((uint32_t)((uint64_t)dz_fps_n * Z_TICK_HZ * 10 / (now - dz_fps_t)));
			dz_fps_t = now;
			dz_fps_n = 0;
		}
	}
}

void __wrap_I_FinishUpdate(void) {
	DG_DrawFrame();
}

void DG_SetWindowTitle(const char *title) {
	(void)title;
}

/* -- time ----------------------------------------------------------------- */

uint32_t DG_GetTicksMs(void) {
	return (uint32_t)(((uint64_t)z_uptime_ticks() * 1000u) / Z_TICK_HZ);
}

void DG_SleepMs(uint32_t ms) {
	uint32_t t = (uint32_t)(((uint64_t)ms * Z_TICK_HZ + 999u) / 1000u);
	z_proc_wait(t ? t : 1);
}

/* -- keys and the pad ------------------------------------------------------- */

#define DZ_QLEN 64
static uint16_t dz_q[DZ_QLEN];         /* pressed << 8 | Doom key */
static unsigned dz_qh, dz_qt;
static uint8_t dz_mods;
static uint32_t dz_pad;

static void dz_post(int pressed, unsigned char key) {
	unsigned n = (dz_qt + 1) % DZ_QLEN;
	if (!key || n == dz_qh) return;
	dz_q[dz_qt] = (uint16_t)((pressed ? 0x100u : 0u) | key);
	dz_qt = n;
}

/* Shift on a US keyboard, undone: Doom wants the key, not the character */
static unsigned char dz_unshift(uint32_t c) {
	static const char from[] = "!@#$%^&*()_+<>?:\"{}|~";
	static const char to[]   = "1234567890-=,./;'[]\\`";
	const char *p;
	if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
	p = strchr(from, (int)c);
	if (c && p) return (unsigned char)to[p - from];
	return (unsigned char)c;
}

static unsigned char dz_key(uint32_t k) {
	if (k == Z_KEY_UP) return KEY_UPARROW;
	if (k == Z_KEY_DOWN) return KEY_DOWNARROW;
	if (k == Z_KEY_LEFT) return KEY_LEFTARROW;
	if (k == Z_KEY_RIGHT) return KEY_RIGHTARROW;
	/* F1-F8 are Doom's own (help, save, load, sound, detail, quick
	 * save, end, messages); F9-F12 are Zeitlos's */
	if (k >= Z_KEY_HELP && k < Z_KEY_HELP + 8)
		return (unsigned char)(KEY_F1 + (k - Z_KEY_HELP));
	switch (k) {
	case 0x0d: return KEY_ENTER;
	case 0x1b: return KEY_ESCAPE;
	case 0x09: return KEY_TAB;
	case 0x7f: case 0x08: return KEY_BACKSPACE;
	case ' ': return KEY_USE;
	default: break;
	}
	if (k >= 0x20 && k < 0x7f) {
		unsigned char c = dz_unshift(k);
		if (c == ',') return KEY_STRAFE_L;
		if (c == '.') return KEY_STRAFE_R;
		return c;
	}
	return 0;
}

/* Ctrl, Shift and Alt, from the modifiers every key message carries */
static void dz_mods_now(uint8_t m) {
	uint8_t was = dz_mods;
	if (!(was & Z_KBD_MOD_CTRL) != !(m & Z_KBD_MOD_CTRL))
		dz_post((m & Z_KBD_MOD_CTRL) != 0, KEY_FIRE);
	if (!(was & Z_KBD_MOD_SHIFT) != !(m & Z_KBD_MOD_SHIFT))
		dz_post((m & Z_KBD_MOD_SHIFT) != 0, KEY_RSHIFT);
	if (!(was & Z_KBD_MOD_ALT) != !(m & Z_KBD_MOD_ALT))
		dz_post((m & Z_KBD_MOD_ALT) != 0, KEY_RALT);
	dz_mods = m;
}

static void dz_pad_now(void) {
	static const struct { uint32_t bit; unsigned char key; } map[] = {
		{ Z_PAD_UP, KEY_UPARROW }, { Z_PAD_DOWN, KEY_DOWNARROW },
		{ Z_PAD_LEFT, KEY_LEFTARROW }, { Z_PAD_RIGHT, KEY_RIGHTARROW },
		{ Z_PAD_A, KEY_FIRE }, { Z_PAD_B, KEY_USE },
		{ Z_PAD_X, KEY_RSHIFT }, { Z_PAD_Y, KEY_RALT },
		{ Z_PAD_START, KEY_ESCAPE }, { Z_PAD_SELECT, KEY_TAB },
	};
	uint32_t p, ch;
	unsigned i;
	if (!z_pad_present(0)) return;
	p = z_pad_read(0);
	ch = p ^ dz_pad;
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (ch & map[i].bit) dz_post((p & map[i].bit) != 0, map[i].key);
	dz_pad = p;
}

static void dz_poll(void) {
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) {
		if (m.subject == Z_WM_KEY && m.obj.type == Z_UINT32) {
			uint32_t v = m.obj.val.uint32;
			uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(v);
			bool down = Z_WM_UNPACK_KEY_PRESSED(v) != 0;
			dz_mods_now((uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(v));
			if (k != Z_KEY_NONE) dz_post(down, dz_key(k));
		} else if (m.subject == Z_WM_GAME_REVOKED) {
			/* Super+Esc: wm has the screen back. Doom has no window
			 * to go back to, so it ends (saving its settings). */
			dz_shown = false;
			I_Quit();
		}
	}
	dz_pad_now();
}

int DG_GetKey(int *pressed, unsigned char *key) {
	if (dz_qh == dz_qt) dz_poll();
	if (dz_qh == dz_qt) return 0;
	*pressed = (dz_q[dz_qh] >> 8) != 0;
	*key = (unsigned char)dz_q[dz_qh];
	dz_qh = (dz_qh + 1) % DZ_QLEN;
	return 1;
}

/* -- the WAD, over the fs syscalls ------------------------------------------
 *
 * In place of the engine's stdio reader (w_file_stdc.c, not linked): the
 * fs syscalls work the same on hardware and in the simulator, and one
 * request reads a whole lump. The engine reads lumps whole, by offset. */

typedef struct {
	wad_file_t wad;
	int handle;
	uint32_t pos;
} dz_wad_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *dz_wad_open(char *path) {
	dz_wad_t *w;
	int size = fs_size(path);
	int h;
	if (size <= 0) return NULL;
	h = fs_open_read(path);
	if (h < 0) return NULL;
	w = malloc(sizeof(*w));
	if (!w) { fs_close_handle(h); return NULL; }
	w->wad.file_class = &stdc_wad_file;
	w->wad.mapped = NULL;
	w->wad.length = (unsigned int)size;
	w->handle = h;
	w->pos = 0;
	return &w->wad;
}

static void dz_wad_close(wad_file_t *wad) {
	dz_wad_t *w = (dz_wad_t *)wad;
	fs_close_handle(w->handle);
	free(w);
}

static size_t dz_wad_read(wad_file_t *wad, unsigned int offset,
	void *buffer, size_t len) {
	dz_wad_t *w = (dz_wad_t *)wad;
	uint8_t *p = buffer;
	size_t done = 0;
	if (w->pos != offset) {
		if (fs_seek(w->handle, offset) < 0) return 0;
		w->pos = offset;
	}
	while (done < len) {
		int n = fs_read_chunk(w->handle, p + done, (int)(len - done));
		if (n <= 0) break;
		done += (size_t)n;
		w->pos += (uint32_t)n;
	}
	return done;
}

wad_file_class_t stdc_wad_file = {
	dz_wad_open,
	dz_wad_close,
	dz_wad_read,
};

/* -- wrapped engine functions (-Wl,--wrap in the Makefile) -----------------
 *
 * M_FileExists: through fs_size() first, which the simulator has too
 * (its stdio has no fs process to talk to).
 * M_SetConfigDir: default.cfg and the saves in /data/doom/, not in
 * whatever directory doom was started from. */

boolean __real_M_FileExists(char *filename);
boolean __wrap_M_FileExists(char *filename) {
	if (fs_size(filename) > 0) return true;
	return __real_M_FileExists(filename);
}

void __real_M_SetConfigDir(char *dir);
void __wrap_M_SetConfigDir(char *dir) {
	if (dir == NULL && dz_dir[0]) dir = dz_dir;
	__real_M_SetConfigDir(dir);
}

/* -- -membench: what memory costs, before the engine starts ---------------------
 *
 * Cycles an operation over buffers far larger than the 4 KB data cache:
 * the patterns the renderer makes. Best of three, each a fresh pass. */

static void dz_mb_line(const char *what, uint32_t cycles, uint32_t n) {
	printf("  %-34s %3lu.%02lu cycles each\n", what,
		(unsigned long)(cycles / n), (unsigned long)((cycles % n) * 100 / n));
}

static void dz_membench(void) {
	enum { BIG = 1u << 20, SCR = SCREENWIDTH * SCREENHEIGHT };
	uint8_t *big = malloc(BIG), *scr = malloc(SCR), *scr2 = malloc(SCR);
	volatile uint32_t *w;
	volatile uint8_t *b;
	uint32_t best[8], i, r, t, x, sink = 0;
	void *(*volatile cp)(void *, const void *, size_t) = memcpy;   /* the call, not a builtin */
	if (!big || !scr || !scr2) { printf("membench: no memory\n"); return; }
	memset(big, 1, BIG);
	memset(scr, 1, SCR);
	for (i = 0; i < 8; i++) best[i] = ~0u;
	for (r = 0; r < 3; r++) {
		w = (volatile uint32_t *)big;
		t = dz_cycle();
		for (i = 0; i < BIG / 4; i += 4) sink += w[i];          /* a line each */
		t = dz_cycle() - t; if (t < best[0]) best[0] = t;
		t = dz_cycle();
		for (i = 0, x = 1; i < 16384; i++) { sink += w[x & (BIG / 4 - 1)]; x = x * 1103515245u + 12345u; }
		t = dz_cycle() - t; if (t < best[1]) best[1] = t;
		w = (volatile uint32_t *)scr;
		t = dz_cycle();
		for (i = 0; i < SCR / 4; i++) w[i] = i;
		t = dz_cycle() - t; if (t < best[2]) best[2] = t;
		b = scr;
		t = dz_cycle();
		for (i = 0; i < SCR; i++) b[i] = (uint8_t)i;
		t = dz_cycle() - t; if (t < best[3]) best[3] = t;
		t = dz_cycle();
		for (x = 0; x < SCREENWIDTH; x++)                      /* a column at a time */
			for (i = x; i < SCR; i += SCREENWIDTH) b[i] = (uint8_t)i;
		t = dz_cycle() - t; if (t < best[4]) best[4] = t;
		w = (volatile uint32_t *)big;
		t = dz_cycle();
		for (i = 0, x = 7; i < 16384; i++) { w[x & (BIG / 4 - 1)] = i; x = x * 1103515245u + 12345u; }
		t = dz_cycle() - t; if (t < best[5]) best[5] = t;
		t = dz_cycle();
		cp(scr2, scr, SCR);
		t = dz_cycle() - t; if (t < best[6]) best[6] = t;
		sink += scr2[r * 999];
		t = dz_cycle();
		for (i = 0; i < 16384; i++) sink += dz_instret();     /* the loop alone */
		t = dz_cycle() - t; if (t < best[7]) best[7] = t;
	}
	printf("membench (cycles, best of 3; 4 KB D-cache):\n");
	dz_mb_line("load, a new line each (1 MB)", best[0], BIG / 16);
	dz_mb_line("load, scattered (1 MB)", best[1], 16384);
	dz_mb_line("store word, in order (64 KB)", best[2], SCR / 4);
	dz_mb_line("store byte, in order (64 KB)", best[3], SCR);
	dz_mb_line("store byte, by columns (320 apart)", best[4], SCR);
	dz_mb_line("store word, scattered (1 MB)", best[5], 16384);
	dz_mb_line("memcpy 64,000 bytes, a byte", best[6], SCR);
	dz_mb_line("(an empty loop pass)", best[7], 16384);
	(void)sink;
	free(scr2); free(scr); free(big);
}

/* -- main --------------------------------------------------------------------- */

int main(int argc, char **argv) {

	static char *args[64];
	int n = 0, i;
	bool have_iwad = false;
	size_t l;

	for (i = 0; i < argc && n < 60; i++) {
		if (!strcmp(argv[i], "-iwad")) have_iwad = true;
		args[n++] = argv[i];
	}
	if (n == 0) args[n++] = "doom";

	/* `run doom -fps` passes its words as the launch argument (wm's
	 * Z_WM_GET_ARG), not in argv; with none, /data/doom/args if there
	 * is one. Words separated by spaces or new lines. */
	if (n == 1) {
		static char line[256];
		char path[Z_FS_PATH_MAX];
		char *p;
		int got = 0;
		if (z_launch_arg_take(line, (int)sizeof(line)))
			got = (int)strlen(line);
		else if (z_data_file(path, sizeof(path), "args"))
			got = fs_read_file(path, line, (int)sizeof(line) - 1);
		if (got > 0) {
			line[got] = 0;
			for (p = strtok(line, " \t\r\n"); p && n < 60; p = strtok(NULL, " \t\r\n")) {
				if (!strcmp(p, "-iwad")) have_iwad = true;
				args[n++] = p;
			}
		}
	}

	/* /data/doom/: the WAD, default.cfg, the saves */
	if (z_data_file(dz_wad, sizeof(dz_wad), "doom1.wad")) {
		strncpy(dz_dir, dz_wad, sizeof(dz_dir) - 1);
		l = strlen(dz_dir);
		while (l && dz_dir[l - 1] != '/') dz_dir[--l] = 0;
	}
	if (!have_iwad && dz_wad[0]) {
		args[n++] = "-iwad";
		args[n++] = dz_wad;
	}
	args[n] = NULL;

	myargc = n;
	myargv = args;
	M_FindResponseFile();

	if (M_CheckParm("-membench")) { dz_membench(); return 0; }

	DG_Init();
	D_DoomMain();
	for (;;) doomgeneric_Tick();

	return 0;
}
