/*
 * mausrun3d -- the models: the mouse and everyone and everything else
 * in the world, low-poly, after mausrun's 16x16 drawings and in their
 * colours, each a display list in world units (a tile is 16), its feet
 * at y 0 and its front to -z (docs/mausrun3d.md, "Models").
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */
#ifndef MAUSRUN3D_MODELS_H
#define MAUSRUN3D_MODELS_H

#include <stdint.h>

enum {
	MD_MOUSE = 0, MD_MOUSE_STEP,    /* standing, and a stride (two frames of running) */
	MD_SHADOW,
	MD_CHEESE,
	MD_CAT, MD_CAT_STEP, MD_CAT_NAP,
	MD_FROG, MD_FROG_HOP,
	MD_OWL,
	MD_HOLE, MD_HOLE_LIT,           /* a mouse hole; with a cheese in it (passed) */
	MD_WHEEL,                       /* the cheese wheel: the end */
	MD_STAR,
	MD_HEART,
	MD_FEATHER, MD_BUBBLE, MD_MAGNET, MD_BELL, MD_SHOES,
	MD_DIZZY,                       /* stars round a head */
	MD_COUNT
};

/* the lists, made once (glGenLists): name of model m is md_list + m */
extern uint32_t md_list;
/* triangles each, for the frame's count */
extern uint16_t md_tris[MD_COUNT];

void md_make(void);

#endif
