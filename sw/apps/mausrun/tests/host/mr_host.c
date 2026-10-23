/*
 * mausrun on a build host -- the machine half. See mr_host.h.
 *
 * The game reads and writes the SOC's registers directly (the USB
 * keyboard report, the game-mode view and frame counter, the colour
 * control), so those two pages are mapped at their real addresses and
 * the host plays the hardware's part: a frame counter that advances at
 * each frame boundary, read-only bits that stay set. The blitter, the
 * filesystem, speech and sound are functions, and are written here: a
 * 640x480 1bpp framebuffer with the blitter's fills, patterns, shades,
 * copies and raster ops, and files read from the tree.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../../../../common/zeitlos.h"
#include "../../../../common/zsoc.h"
#include "../../../../common/zgfx.h"
#include "../../../../common/zcolor.h"
#include "../../../../common/zobj.h"

#include "mr_host.h"

#define FB_W 640
#define FB_H 480

static uint8_t fb[FB_H][FB_W];
static uint16_t palette[16] = { 0x6be, 0x320, 0x4a3, 0xfe8 };
int mrh_blits;
char mrh_said[128];
int mrh_says;
int mrh_quit_frame;

/* ===================================================================
 * the registers
 * =================================================================== */

static void map_page(uintptr_t at) {
	void *p = mmap((void *)at, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p != (void *)at) {
		printf("mr_host: cannot map the registers at %#lx\n", (unsigned long)at);
		exit(2);
	}
}

/* what the hardware holds and software cannot change */
static void hold_bits(void) {
	reg_socctl_magic = Z_SOCCTL_MAGIC;
	reg_socctl_game = (reg_socctl_game & 0xffffu) | Z_GAME_AVAIL | (Z_GAME_SIG << 16);
	reg_socctl_color = (reg_socctl_color & 0x7fffu) | Z_COLOR_AVAIL |
		(Z_COLOR_SIG << 16);
}

__attribute__((constructor)) static void regs_init(void) {
	map_page(0x70000000u);
	map_page(0xc0000000u);
	reg_csr_features = 0;
	hold_bits();
	/* a keyboard on port 0: type 1 in bits 25:24 */
	reg_usb0_info = 1u << 24;
	reg_usb0_keys = 0;
	reg_usb1_info = 0;
}

void host_set_palette(uint32_t i, uint32_t rgb) {
	if (i < 16) palette[i] = (uint16_t)(rgb & 0xfff);
}

/* ===================================================================
 * the blitter
 * =================================================================== */

static bool clip(int *x, int *y, int *w, int *h, int *sx, int *sy) {
	if (*x < 0) { *sx -= *x; *w += *x; *x = 0; }
	if (*y < 0) { *sy -= *y; *h += *y; *y = 0; }
	if (*x + *w > FB_W) *w = FB_W - *x;
	if (*y + *h > FB_H) *h = FB_H - *y;
	return *w > 0 && *h > 0;
}

static void rop_px(int x, int y, int v, int rop) {
	uint8_t *d = &fb[y][x];
	switch (rop) {
	case Z_ROP_OR:   *d |= (uint8_t)v; break;
	case Z_ROP_XOR:  *d ^= (uint8_t)v; break;
	case Z_ROP_ANDN: *d &= (uint8_t)!v; break;
	default:         *d = (uint8_t)v; break;
	}
}

void z_fb_hw_fill_rect(int x, int y, int w, int h, int color) {
	int sx = 0, sy = 0, i, j;
	hold_bits();
	mrh_blits++;
	if (!clip(&x, &y, &w, &h, &sx, &sy)) return;
	for (j = 0; j < h; j++) memset(&fb[y + j][x], color ? 1 : 0, (size_t)w);
	(void)i;
}

void z_fb_hw_fill_rect_async(int x, int y, int w, int h, int color) {
	z_fb_hw_fill_rect(x, y, w, h, color);
}

void z_fb_hw_fill_pattern(int x, int y, int w, int h, const uint8_t *pat) {
	int sx = 0, sy = 0, i, j;
	hold_bits();
	mrh_blits++;
	if (!clip(&x, &y, &w, &h, &sx, &sy)) return;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int X = x + i, Y = y + j;
			fb[Y][X] = (uint8_t)((pat[Y & 7] >> (7 - (X & 7))) & 1);
		}
}

static const uint8_t bayer[4][4] = {
	{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

void z_fb_hw_fill_shade(int x, int y, int w, int h, int level) {
	int sx = 0, sy = 0, i, j;
	hold_bits();
	mrh_blits++;
	if (!clip(&x, &y, &w, &h, &sx, &sy)) return;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int X = x + i, Y = y + j;
			fb[Y][X] = (uint8_t)(bayer[Y & 3][X & 3] < level);
		}
}

void z_fb_hw_fill_shade_async(int x, int y, int w, int h, int level) {
	z_fb_hw_fill_shade(x, y, w, h, level);
}

static int src_bit(const void *src, int stride, int x, int y) {
	const uint8_t *row = (const uint8_t *)src + (size_t)y * (size_t)stride;
	uint32_t word;
	memcpy(&word, row + (x >> 5) * 4, 4);
	return (int)((word >> (x & 31)) & 1u);
}

bool z_fb_hw_blit_mem_async(const void *src, int stride, int sx, int sy,
	int dx, int dy, int w, int h, int rop) {
	int i, j;
	hold_bits();
	mrh_blits++;
	if (!clip(&dx, &dy, &w, &h, &sx, &sy)) return true;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			rop_px(dx + i, dy + j, src_bit(src, stride, sx + i, sy + j), rop);
	return true;
}

bool z_fb_hw_blit_mem(const void *src, int stride, int sx, int sy,
	int dx, int dy, int w, int h) {
	return z_fb_hw_blit_mem_async(src, stride, sx, sy, dx, dy, w, h, Z_ROP_COPY);
}

bool z_fb_hw_blit_mem_rop(const void *src, int stride, int sx, int sy,
	int dx, int dy, int w, int h, int rop) {
	return z_fb_hw_blit_mem_async(src, stride, sx, sy, dx, dy, w, h, rop);
}

bool z_fb_hw_blit_sprite_async(const void *data, const void *mask, int stride,
	int sx, int sy, int dx, int dy, int w, int h) {
	int i, j;
	hold_bits();
	mrh_blits++;
	if (!clip(&dx, &dy, &w, &h, &sx, &sy)) return true;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			if (!src_bit(mask, stride, sx + i, sy + j)) continue;
			fb[dy + j][dx + i] = (uint8_t)src_bit(data, stride, sx + i, sy + j);
		}
	return true;
}

bool z_fb_hw_blit_sprite(const void *data, const void *mask, int stride,
	int sx, int sy, int dx, int dy, int w, int h) {
	return z_fb_hw_blit_sprite_async(data, mask, stride, sx, sy, dx, dy, w, h);
}

bool z_fb_hw_blit_mem_available(void) { return true; }
bool z_fb_hw_rop_available(void) { return true; }
bool z_fb_hw_dither_available(void) { return true; }
void z_fb_hw_sync(void) { hold_bits(); }
/* zcolor.c lets a layout of more than four planes reach rows 480 up;
 * mausrun uses four, inside the 640x480 this host keeps */
void z_fb_set_rows(int rows) { (void)rows; }

/* ===================================================================
 * the system: files, speech, sound, wm
 * =================================================================== */

static char progress[2048];
static int progress_len = -1;

static const char *tree_path(const char *p) {
	static char b[256];
	if (strncmp(p, "/data/mausrun/levels/", 21) == 0) {
		snprintf(b, sizeof b, "../../../data/mausrun/levels/%s", p + 21);
		return b;
	}
	return NULL;
}

int fs_size(char *filename) {
	const char *t = tree_path(filename);
	FILE *f;
	long n;
	if (strstr(filename, "progress.txt")) return progress_len;
	if (!t || !(f = fopen(t, "rb"))) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (int)n;
}

int fs_read_file(char *filename, char *buf, int maxlen) {
	const char *t = tree_path(filename);
	FILE *f;
	int n;
	if (strstr(filename, "progress.txt")) {
		if (progress_len < 0) return 0;
		if (progress_len > maxlen) return -1;
		memcpy(buf, progress, (size_t)progress_len);
		return progress_len;
	}
	if (!t || !(f = fopen(t, "rb"))) return 0;
	n = (int)fread(buf, 1, (size_t)maxlen, f);
	fclose(f);
	return n;
}

int fs_write_file(char *filename, char *buf, int len) {
	if (strstr(filename, "progress.txt") && len < (int)sizeof(progress)) {
		memcpy(progress, buf, (size_t)len);
		progress_len = len;
		return len;
	}
	return 0;
}

int fs_mkdir(char *path) { (void)path; return 1; }

/* a card that has been played before */
void mrh_seed_progress(const char *text) {
	progress_len = (int)strlen(text);
	memcpy(progress, text, (size_t)progress_len);
}

const char *mrh_progress(void) {
	static char b[2049];
	int n = progress_len < 0 ? 0 : progress_len;
	memcpy(b, progress, (size_t)n);
	b[n] = 0;
	return b;
}

bool z_cfg_get(const char *key, char *out, size_t outlen) {
	(void)key; (void)out; (void)outlen;
	return false;
}

bool z_speak(const char *text, uint32_t flags) {
	(void)flags;
	snprintf(mrh_said, sizeof mrh_said, "%s", text);
	mrh_says++;
	return true;
}

bool z_pid_lookup(const char *name, uint32_t *pid) { (void)name; (void)pid; return false; }
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	(void)to; (void)subject; (void)tag; (void)obj;
	return 0;
}
bool z_game_set_enabled(bool on, bool wrap) {
	reg_socctl_game = (on ? Z_GAME_ENABLE : 0u) | (wrap ? Z_GAME_WRAP : 0u);
	hold_bits();
	return true;
}

/* sound: counted, not heard */
int mrh_sfx;
/* zsfx, silent: the effects counted (sound.c is the real one) */
#include "../../../../common/games/zsfx.h"
static bool zh_music = true;
static uint8_t zh_master = 255;
bool zsfx_init(void) { return false; }
bool zsfx_ok(void) { return false; }
void zsfx_tick(uint32_t f) { (void)f; }
void zsfx_play(const zsfx_fx_t *fx, int pan) { (void)fx; (void)pan; mrh_sfx++; }
void zsfx_stop_all(void) { }
int zsfx_pan_x(int x) { return x; }
void zsfx_music_play(const zsfx_song_t *s) { (void)s; }
void zsfx_music_stop(void) { }
void zsfx_music_enable(bool on) { zh_music = on; }
bool zsfx_music_enabled(void) { return zh_music; }
int mrh_intensity = -1;
void zsfx_music_intensity(int level) { mrh_intensity = level; }
void zsfx_master_volume(uint8_t v) { zh_master = v; }
uint8_t zsfx_master_get(void) { return zh_master; }
void zsfx_suspend(bool on) { (void)on; }
void zsfx_shutdown(void) { }

/* ===================================================================
 * the frame boundary, the script, the pictures
 * =================================================================== */

static const mrh_step_t *script;
static const char *out_dir;
static uint32_t frame_no;
static int shots;
static uint8_t held[8];
static int nheld;
static struct { uint8_t u; uint32_t until; } taps[8];

uint32_t mrh_frame(void) { return frame_no; }
int mrh_shots(void) { return shots; }

static void key_down(uint8_t u) {
	int i;
	for (i = 0; i < nheld; i++) if (held[i] == u) return;
	if (nheld < 8) held[nheld++] = u;
}

static void key_up(uint8_t u) {
	int i;
	for (i = 0; i < nheld; i++)
		if (held[i] == u) { held[i] = held[--nheld]; return; }
}

static void keys_to_report(void) {
	uint32_t k = 0;
	int i;
	for (i = 0; i < nheld && i < 4; i++) k |= (uint32_t)held[i] << (8 * i);
	reg_usb0_keys = k;
}

void mrh_snapshot(const char *name) {
	char path[512];
	FILE *f;
	int x, y;
	bool colour = (reg_socctl_color & Z_COLOR_ENABLE) != 0;
	int vx = (int)(reg_socctl_view & 0x3ff), vy = (int)((reg_socctl_view >> 16) & 0x3ff);
	snprintf(path, sizeof path, "%s/%s.ppm", out_dir, name);
	f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n320 240\n255\n");
	for (y = 0; y < 240; y++)
		for (x = 0; x < 320; x++) {
			int X = (vx + x) % FB_W, Y = (vy + y) % FB_H;
			uint8_t rgb[3];
			if (colour) {
				int c = fb[Y][X] | (fb[(Y + 240) % FB_H][X] << 1);
				uint16_t p = palette[c];
				rgb[0] = (uint8_t)(((p >> 8) & 15) * 17);
				rgb[1] = (uint8_t)(((p >> 4) & 15) * 17);
				rgb[2] = (uint8_t)((p & 15) * 17);
			} else {
				rgb[0] = rgb[1] = rgb[2] = fb[Y][X] ? 255 : 0;
			}
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	shots++;
	printf("mr_host: frame %u -> %s\n", (unsigned)frame_no, path);
}

/* The frame boundary: everything the game drew this frame is in, the
 * view points at it. Pictures, then the script's next keys. */
void host_wait_frame(void) {
	int i;
	hold_bits();
	frame_no++;
	reg_socctl_frame = (reg_socctl_frame + 1) & 0xffffu;
	mrh_hook(frame_no);
	for (i = 0; i < 8; i++)
		if (taps[i].u && frame_no >= taps[i].until) { key_up(taps[i].u); taps[i].u = 0; }
	while (script && script->kind != MRH_END && script->frame <= frame_no) {
		switch (script->kind) {
		case MRH_DOWN: key_down((uint8_t)script->a); break;
		case MRH_UP:   key_up((uint8_t)script->a); break;
		case MRH_TAP:
			key_down((uint8_t)script->a);
			for (i = 0; i < 8; i++)
				if (!taps[i].u) { taps[i].u = (uint8_t)script->a; taps[i].until = frame_no + 2; break; }
			break;
		case MRH_SHOT: mrh_snapshot(script->name); break;
		case MRH_PAD:  reg_usb1_info = 0; break;
		default: break;
		}
		script++;
	}
	if (script && script->kind == MRH_END && frame_no >= script->frame + 600) {
		printf("mr_host: the game did not quit by frame %u\n", (unsigned)frame_no);
		exit(3);
	}
	keys_to_report();
}

void mr_host_key(int usage, int down) {
	if (down) key_down((uint8_t)usage);
	else key_up((uint8_t)usage);
}

void mrh_run(const mrh_step_t *s, const char *dir) {
	script = s;
	out_dir = dir;
}

z_obj_t z_obj_uint32(uint32_t v) {
	z_obj_t o;
	memset(&o, 0, sizeof o);
	(void)v;
	return o;
}
