#ifndef MR_HOST_H
#define MR_HOST_H

/*
 * mausrun on a build host: the game's own mausrun.c, unchanged, on a
 * software framebuffer and blitter, with scripted keys and pictures.
 * make -C sw/apps/mausrun render   (tests/host/render.c is the script)
 */

#include <stdint.h>
#include <stdbool.h>

/* USB HID usages, as kbd_held() reads them */
#define K_LEFT_U   0x50
#define K_RIGHT_U  0x4F
#define K_UP_U     0x52
#define K_DOWN_U   0x51
#define K_SPACE_U  0x2C
#define K_ENTER_U  0x28
#define K_ESC_U    0x29
#define K_P_U      0x13
#define K_M_U      0x10
#define K_C_U      0x06

typedef struct {
	uint32_t frame;
	int kind;              /* MRH_* */
	int a, b;
	const char *name;
} mrh_step_t;

enum {
	MRH_END = 0,
	MRH_DOWN,      /* a = usage: held from this frame */
	MRH_UP,        /* a = usage: released */
	MRH_TAP,       /* a = usage: held for two frames */
	MRH_SHOT,      /* name: out/<name>.ppm */
	MRH_PAD        /* a = Z_PAD_* bits held from now (0 releases) */
};

void mrh_run(const mrh_step_t *script, const char *dir);
uint32_t mrh_frame(void);
int mrh_shots(void);
void mrh_snapshot(const char *name);
extern int mrh_blits;
extern char mrh_said[128];
extern int mrh_says;
extern int mrh_quit_frame;

/* called by the host once a frame, before the snapshot step: a script
 * can watch the game's state through the hook (render.c) */
void mrh_hook(uint32_t frame);

#endif
