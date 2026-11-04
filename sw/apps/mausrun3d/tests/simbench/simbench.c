/*
 * mausrun3d in the simulator (sim/README.md, "Cycle model"): the game
 * itself, the arcade runtime stood in for here -- game mode on chunky
 * from the start, the arrows held, a jump now and then, no sound -- for
 * a number of frames, its report every two seconds on the console. With
 * ZSIM_COST=ddr3 its rdcycle reads the modelled cycles, and ZSIM_GTRI=2
 * makes T2 the simulator's model (never busy: the cycles are the CPU's).
 *
 *   ZSIM_COST=ddr3 ZSIM_GTRI=2 sim/zsim-headless -n 0 simbench.bin
 *   ZSIM_PROF=prof.txt ...; tools/simprof.py simbench.dasm prof.txt
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../../../../common/games/zarcade.h"

int game_main(void);

#ifndef FRAMES
#define FRAMES 480
#endif
static uint32_t frame;
static bool mode_sent;

bool ar_init(const ar_desc_t *d) { (void)d; return true; }
void ar_shutdown(void) { }
void ar_chunky(bool on) { (void)on; }
bool ar_set_fullscreen(bool on) { (void)on; return true; }
ar_mode_t ar_mode(void) { return AR_MODE_FULL_CHUNKY; }
void ar_pal_load(const uint16_t *rgb, int n) { (void)rgb; (void)n; }
void ar_dirty_all(void) { }
void ar_fill(int x, int y, int w, int h, int c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
int ar_text(int x, int y, const char *s, int c, const z_font_t *f, int scale) {
	(void)x; (void)y; (void)s; (void)c; (void)f; (void)scale; return 0;
}
uint32_t ar_ticks(void) { return frame; }
void mr3_memset_report(void);
uint32_t ar_frame(void) {
	frame++;
#ifdef MR3_WRAP_MEMSET
	if (frame == 100 || frame == 200) mr3_memset_report();
#endif
	return 1;
}

bool ar_poll(ar_event_t *e) {
	memset(e, 0, sizeof(*e));
	if (!mode_sent) { mode_sent = true; e->type = AR_EV_MODE; return true; }
	if (frame >= FRAMES) { e->type = AR_EV_QUIT; return true; }
	/* Space: past the level card, then a jump every second */
	if (frame % 60 == 5) {
		static uint32_t last = ~0u;
		if (last == frame) return false;
		last = frame;
		e->type = AR_EV_BUTTON; e->player = 0; e->pressed = 1; e->key = AR_BTN_A;
		return true;
	}
	return false;
}

uint32_t ar_held(int p) {
	/* run, turning now and then */
	if (p != 0) return 0;
	return AR_BTN_UP | ((frame / 90) % 3 == 1 ? AR_BTN_RIGHT : 0);
}

/* mausrun's sounds and the card: none here */
bool zsfx_ok(void) { return false; }
void zsfx_music_play(const void *s) { (void)s; }
void snd_play(int which, int pan_x) { (void)which; (void)pan_x; }
const char snd_song_meadow[1];
int fs_size(char *f) { (void)f; return -1; }
int fs_read_file(char *f, char *b, int n) { (void)f; (void)b; (void)n; return -1; }

void mr3_memset_report(void);
int main(void) {
	int r = game_main();
#ifdef MR3_WRAP_MEMSET
	mr3_memset_report();
#endif
	return r;
}

/* what zgl's other outputs name (not used on chunky) */
bool ar_color(void) { return true; }
int ar_colours(void) { return 256; }
uint16_t ar_pal_get(int i) { (void)i; return 0; }
void ar_pal(int i, uint16_t c) { (void)i; (void)c; }
void ar_plane_line(int k, int x0, int y0, int x1, int y1, int op) { (void)k; (void)x0; (void)y0; (void)x1; (void)y1; (void)op; }
void ar_plane(int k, const void *b, int st, int sx, int sy, int x, int y, int w, int h) {
	(void)k; (void)b; (void)st; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h;
}
void ar_bitmap_opaque(const void *b, int st, int sx, int sy, int x, int y, int w, int h) {
	(void)b; (void)st; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h;
}
const z_font_t z_font_5x8;

#ifdef MR3_WRAP_MEMSET
/* who clears how much: -Wl,--wrap=memset */
#include <stdio.h>
void *__real_memset(void *d, int c, size_t n);
static struct { void *ra; uint32_t calls, bytes; } ms[32];
void *__wrap_memset(void *d, int c, size_t n) {
	void *ra = __builtin_return_address(0);
	int i;
	for (i = 0; i < 32 && ms[i].ra && ms[i].ra != ra; i++) ;
	if (i < 32) { ms[i].ra = ra; ms[i].calls++; ms[i].bytes += n; }
	return __real_memset(d, c, n);
}
void mr3_memset_report(void) {
	int i;
	for (i = 0; i < 32 && ms[i].ra; i++)
		printf("memset from %p: %lu calls, %lu bytes\n", ms[i].ra, (unsigned long)ms[i].calls, (unsigned long)ms[i].bytes);
}
#endif
