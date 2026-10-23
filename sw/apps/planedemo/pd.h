#ifndef PLANEDEMO_PD_H
#define PLANEDEMO_PD_H

/*
 * planedemo -- what the scenes share. See docs/planedemo.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The film is one 320x240 picture in 256 colours (the arcade runtime,
 * eight planes), on one clock: the frame counter, 60 a second, which
 * the music keeps too. Each scene owns a span of bars; it is told the
 * film's frame and its own, and draws what changed.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/games/zarcade.h"
#include "pd_music.h"

#define PD_W 320
#define PD_H 240

typedef struct {
	const char *name;
	int bar;                        /* the bar it starts on, 0-based */
	void (*begin)(void);            /* the picture it starts from */
	void (*frame)(uint32_t t, uint32_t lt);   /* film frame, scene frame */
} pd_scene_t;

/* -- shared helpers (planedemo.c) -- */

/* Palette entries 0..255 as RGB444, written at the next frame. */
void pd_pal(int i, uint16_t rgb);
uint16_t pd_rgb(int r, int g, int b);          /* 0..15 each, clamped */
/* a toward b by t (0..256): exactly ar_rgb_mix()'s rounding, in four
 * multiplies -- red and blue side by side in one word -- not a loop of
 * three channels. The palettes are most of a frame's work, and this is
 * most of the palettes. */
static inline uint16_t pd_mix(uint16_t a, uint16_t b, int t) {
	uint32_t u, x, y, rb, g;
	if (t <= 0) return (uint16_t)(a & 0xfff);
	if (t >= 256) return (uint16_t)(b & 0xfff);
	u = 256u - (uint32_t)t;
	x = (a & 0x00fu) | ((uint32_t)(a & 0xf00u) << 8);
	y = (b & 0x00fu) | ((uint32_t)(b & 0xf00u) << 8);
	rb = (x * u + y * (uint32_t)t + 0x00800080u) >> 8;
	g = (((a >> 4) & 15u) * u + ((b >> 4) & 15u) * (uint32_t)t + 128u) >> 8;
	return (uint16_t)((rb & 0xfu) | ((rb >> 8) & 0xf00u) | (g << 4));
}

/* The whole picture in one index. */
void pd_clear(int c);

/* Text, centred on x, in index c. */
void pd_text_c(int x, int y, const char *s, int c, int scale);

/* Where in the bar we are: 0..255 across a beat, for pulses. */
int pd_beat_phase(uint32_t t);

/* Frames lately dropped: non-zero for a second after one was. A scene
 * that can do less (fewer stars) does while it is. */
extern int pd_late;

/* A cheap random number. */
uint32_t pd_rand(void);

/* -- the scenes (pd_scenes.c) -- */

extern const pd_scene_t *const pd_scenes[];
extern const int pd_nscenes;

#endif
