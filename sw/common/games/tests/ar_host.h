#ifndef AR_HOST_H
#define AR_HOST_H

/*
 * Zeitlos -- driving an arcade game on a build host (ar_host.c): a
 * script of key presses and snapshots, run through the game's own loop.
 */

#include <stdint.h>
#include <stdbool.h>

enum {
	ARH_END = 0,
	ARH_KEY_DOWN,      /* a = keysym */
	ARH_KEY_UP,
	ARH_TAP,           /* down and up in the same frame */
	ARH_PAD,           /* a = AR_BTN_* bits held, b = player */
	ARH_WINDOW,
	ARH_FULL_COLOR,
	ARH_FULL_MONO,
	ARH_COVER,         /* x, y, w, h: another window over ours; w 0 = gone */
	ARH_SHOT,          /* name: write <dir>/<name>.ppm after this frame */
	ARH_MOUSE          /* x, y logical, a = buttons: the pointer moved */
};

typedef struct {
	uint32_t frame;
	int kind;
	uint32_t a;
	int b;
	const char *name;
	int x, y, w, h;
} ar_host_step_t;

/* zkbd.h's named keys, for scripts */
#define ARH_K_UP     0x110000u
#define ARH_K_DOWN   0x110001u
#define ARH_K_LEFT   0x110002u
#define ARH_K_RIGHT  0x110003u
#define ARH_K_F1     0x110010u
#define ARH_K_F2     0x110011u

void ar_host_run(const ar_host_step_t *script, const char *dir);
uint32_t ar_host_frame(void);
int ar_host_shots(void);
int ar_host_lit(int x, int y, int w, int h);
void ar_host_cover(int x, int y, int w, int h);
const char *ar_host_scores(void);

extern uint64_t arh_ops;
extern uint32_t zsfxh_notes, zsfxh_updates, zsfxh_offs;
extern uint8_t zsfxh_peak[8];



/* The runtime's own tests (test_zarcade.c): the framebuffer, and one
 * render pass without the rest of a frame. */
#define AR_HOST_FB_BYTES (640 * 480)
void ar_host_fb_copy(uint8_t *out);
int ar_host_fb_diff(const uint8_t *a, const uint8_t *b);
void ar_host_render_only(void);

/* With AR_HOST_VERIFY set (and -DAR_HOST_TESTS): frames checked against
 * a full repaint, and how many differed. */
uint32_t ar_host_verified(void);

/* A picture now (<dir>/<name>.ppm), and the hook a script may define to
 * take pictures when something happens: called after every frame. */
void ar_host_snapshot(const char *name);
void ar_host_hook(uint32_t frame);
uint32_t ar_host_verify_bad(void);
#endif
