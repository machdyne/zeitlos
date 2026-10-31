#ifndef MJ_FACE_H
#define MJ_FACE_H

/*
 * Zeitlos mahjong -- putting the canvas on a face.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The part of the app between the session (mj_play) and the screen:
 * the canvas's memory, setting it up for a face, laying the board out
 * for it, recomposing what a change touched and handing those
 * rectangles to the face, and the screen face's paint() for the arcade
 * runtime. Split out of mahjong.c so that tests/screen.c runs exactly
 * this against the runtime's host build (ar_host.c), with every frame
 * compared to a full repaint.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/games/zarcade.h"
#include "mj_play.h"

/* 320x240 in six planes (64 colours) is the most either face needs:
 * 14,400 words; the desktop's 640x480 in one plane is 9,600. One word
 * spare: the blitter may read a word past the last it uses (zgfx.h,
 * z_fb_hw_blit_mem()). */
#define MJ_CV_WORDS (14400 + 1)

extern mj_canvas_t mj_cv;
extern uint32_t    mj_cvmem[MJ_CV_WORDS];

/* Whether the canvas holds the screen face's picture. False from when
 * the app lends the window to the runtime until mj_face_screen() has
 * composed it: meanwhile the canvas is still the desktop's, a different
 * size, and paint() shows the felt instead of reading it. */
extern bool mj_face_ready;

/* -- the screen face -- */

/* The planes the runtime shows now: 0 in black and white, else 3, 4 or
 * 6 for 8, 16 or 64 colours. */
int  mj_face_planes(void);
/* The canvas for the screen as it is now; the board laid out on it and
 * composed whole, and all of it marked for the runtime to copy. */
void mj_face_screen(mj_play_t *p);
/* Composing once per mode. mj_face_begin() when the window goes to
 * the runtime (`full`: straight to full screen), mj_face_end() when it
 * comes back; mj_face_check() once a frame, before the frame: it
 * composes if the canvas is not what the screen shows -- once, however
 * many AR_EV_MODEs and plane changes a switch takes. Into the window,
 * where the board has the colour window, it waits up to
 * MJ_FACE_WAIT_COLOUR frames for colour rather than composing in black
 * and white for a few frames first. True when it composed. */
#define MJ_FACE_WAIT_COLOUR 6
void mj_face_begin(bool full);
void mj_face_end(void);
bool mj_face_check(mj_play_t *p);
/* How many times the table has been composed whole for the screen
 * face, for the tests: one per change of mode, not three. */
extern int mj_face_composes;

/* ar_desc_t.paint: the canvas, plane by plane. */
void mj_face_paint(void *user, const ar_rect_t *r);
/* The hint's palette entry, breathing; `frame` counts frames. */
void mj_face_pulse(const mj_play_t *p, uint32_t frame);

/* -- either face -- */

/* Lays the board out for the face: the screen's 320x240 with a zoom,
 * or a desktop canvas w x h with the largest tiles that fit. */
void mj_face_layout(mj_play_t *p, bool screen, int w, int h);

/* Recomposes what the last input changed and shows it with `show`
 * (ar_dirty() on the screen face, a blit on the desktop). Clears the
 * picture's part of the session's out-fields: rect, all, relayout,
 * bar. The rest -- what to say, the sound, sparks, saving -- is left
 * for the caller. */
typedef void (*mj_show_fn)(int x, int y, int w, int h);
void mj_face_flush(mj_play_t *p, bool screen, mj_show_fn show);

#endif
