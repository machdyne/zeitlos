/*
 * planedemo -- the arcade runtime's machine half as nothing at all, for
 * counting the film's instructions on a RISC-V emulator (run.py): the
 * blitter, the rasterizer and the mixer cost the CPU nothing to ask
 * for, so here they are counted, not done.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../../../../common/games/zarcade_int.h"
#include "../../../../common/games/zsfx.h"

uint32_t rv_ops;

void arh_fill(int x, int y, int w, int h, int bit) { (void)x; (void)y; (void)w; (void)h; (void)bit; rv_ops++; }
void arh_pattern(int x, int y, int w, int h, const uint8_t *p) { (void)x; (void)y; (void)w; (void)h; (void)p; rv_ops++; }
void arh_blit(const void *src, int stride, int sx, int sy, int x, int y, int w, int h, int op) {
	(void)src; (void)stride; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h; (void)op; rv_ops++; }
void arh_sprite(const void *data, const void *mask, int stride, int sx, int sy, int x, int y, int w, int h) {
	(void)data; (void)mask; (void)stride; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h; rv_ops++; }
void arh_vcopy(int sx, int sy, int dx, int dy, int w, int h) { (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h; rv_ops++; }
void arh_line(int x0, int y0, int x1, int y1, int op, int cx0, int cy0, int cx1, int cy1) {
	(void)x0; (void)y0; (void)x1; (void)y1; (void)op; (void)cx0; (void)cy0; (void)cx1; (void)cy1; rv_ops++; }
void arh_targets(void) { }
bool arh_copy_ok(void) { return true; }
static void (*hook)(void);
void ar_vsync_exact(bool on) { (void)on; }
void ar_frame_hook(void (*fn)(void)) { hook = fn; }
uint32_t arh_pad(int p) { (void)p; return 0; }
bool arh_pad_present(int p) { (void)p; return false; }
int arh_read_scores(char *b, int c) { (void)b; (void)c; return 0; }
bool arh_write_scores(const char *b, int l) { (void)b; (void)l; return false; }
int arh_read_settings(char *b, int c) { (void)b; (void)c; return 0; }
bool arh_write_settings(const char *b, int l) { (void)b; (void)l; return false; }
int arh_cfg_skill(void) { return 1; }
void arh_say(const char *t) { (void)t; }
uint32_t arh_seed(void) { return 12345u; }

bool ar_color_available(void) { return true; }
bool ar_set_fullscreen(bool on) {
	ar__s.mode = on ? AR_MODE_FULL_COLOR : AR_MODE_WINDOW;
	ar__choose_planes(8);
	ar__pal_all_dirty();
	ar__invalidate();
	return true;
}
void ar_set_color(bool on) { (void)on; }
int ar_set_colours(int n) {
	ar__s.colours_want = n >= 256 ? 256 : n >= 64 ? 64 : 16;
	ar__choose_planes(8);
	return 1 << ar__s.planes;
}
bool ar_poll(ar_event_t *e) { return ar__dequeue(e); }
uint32_t ar_frame(void) {
	int i, n = 1 << ar__s.planes;
	ar__shake_step();
	/* the palette writes the board makes (z_color_set_palette): a
	 * store each */
	for (i = 0; i < n; i++)
		if (ar__s.pal_dirty[i >> 5] & (1u << (i & 31))) {
			*(volatile uint32_t *)0x3f00000u = ar__s.pal[i];
			ar__s.pal_dirty[i >> 5] &= ~(1u << (i & 31));
		}
	if (hook) hook();       /* the frame's drawing, queued (planedemo.c) */
	ar__render();
	zsfx_tick(1);
	ar__frame_input();
	ar__s.ticks += 1;
	return 1;
}
void ar_dialog_begin(void *ctx) { memset(ctx, 0, sizeof(void *) * 3); }
void ar_dialog_end(void) { }
bool ar_init(const ar_desc_t *d) { ar__core_init(d); ar_seed(arh_seed()); zsfx_init(); ar__invalidate(); return true; }
void ar_shutdown(void) { zsfx_shutdown(); }
bool zsfxh_present(void) { return true; }
bool zsfxh_speech(void) { return false; }
uint32_t zsfxh_out_hz(void) { return 48000; }

void zsfxh_gain(int ch, uint32_t step, uint8_t gl, uint8_t gr) { (void)ch; (void)step; (void)gl; (void)gr; }
void zsfxh_off(int ch) { (void)ch; }
void zsfxh_release(void) { }

int game_main(void);
volatile int rv_result;
int main(void) { rv_result = game_main(); return rv_result; }
void zsfxh_note(int ch, const int8_t *wave, uint32_t len, uint32_t step, uint8_t gl, uint8_t gr) {
	(void)ch; (void)wave; (void)len; (void)step; (void)gl; (void)gr; }
unsigned long long arh_ops;
