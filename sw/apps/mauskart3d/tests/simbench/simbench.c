/*
 * mauskart3d in the simulator (sim/README.md, "Cycle model"): the game
 * itself on autopilot, the arcade runtime stood in for here -- game mode
 * on chunky from the start, no sound -- for FRAMES frames, its report
 * every two seconds on the console. With ZSIM_COST its rdcycle reads the
 * modelled cycles; ZSIM_GTRI=2 makes T2 the simulator's model (never
 * busy: the cycles are the CPU's).
 *
 * dt is always 2 (the game's ticks for a frame at 30): the steps a frame
 * costs at 30 fps. It exits after FRAMES frames (a quit event would leave
 * the game waiting on ticks that never come).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../../../../common/games/zarcade.h"
#include "../../../../common/games/zsfx.h"

int game_main(void);
void mk_test_autopilot(bool on);

#ifndef FRAMES
#define FRAMES 600
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
uint32_t ar_ticks(void) { return frame * 2; }
void ar_frame_lock(uint32_t n) { (void)n; }
uint32_t ar_frame_lock_get(void) { return 2; }
#include <stdlib.h>
extern char _end;
extern void *_sbrk(int);
uint32_t ar_frame(void) {
	if (frame == 0 || frame == FRAMES) printf("simbench: heap %lu KB\n", (unsigned long)(((char *)_sbrk(0) - &_end) >> 10));
	if (++frame > FRAMES) exit(0);
	return 2;
}

bool ar_poll(ar_event_t *e) {
	memset(e, 0, sizeof(*e));
	if (!mode_sent) { mode_sent = true; e->type = AR_EV_MODE; 
#ifndef TITLE
		mk_test_autopilot(true);
#endif
	 return true; }
	return false;
}
uint32_t ar_held(int p) { (void)p; return 0; }

/* no sound here */
bool zsfx_ok(void) { return false; }
void zsfx_play(const zsfx_fx_t *fx, int pan) { (void)fx; (void)pan; }
void zsfx_music_play(const zsfx_song_t *s) { (void)s; }
void zsfx_music_enable(bool on) { (void)on; }
bool zsfx_music_enabled(void) { return false; }
void snd_play(int which, int pan_x) { (void)which; (void)pan_x; }
const zsfx_song_t snd_song_meadow;

/* the scores: none kept */
void ar_scores_load(int mode, ar_table_t *t) { (void)mode; memset(t, 0, sizeof(*t)); }
int ar_scores_insert(ar_table_t *t, uint32_t score, const char *who) { (void)t; (void)score; (void)who; return -1; }
bool ar_scores_save(int mode, const ar_table_t *t) { (void)mode; (void)t; return false; }

int main(void) { return game_main(); }

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
