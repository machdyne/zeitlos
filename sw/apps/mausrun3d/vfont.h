/*
 * mausrun3d -- letters of lines, for a chunky picture the runtime draws
 * no text on: A-Z, 0-9 and ! . , : / - ? ', in picture coordinates
 * (view_2d), a letter 4 x 6 grid units, `scale` pixels a unit.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */
#ifndef MAUSRUN3D_VFONT_H
#define MAUSRUN3D_VFONT_H

#include <stdbool.h>

int vf_width(const char *s, int scale);
void vf_text(int x, int y, int scale, int colour, const char *s, bool bold);
void vf_centred(int y, int scale, int colour, const char *s, bool bold);

#endif
