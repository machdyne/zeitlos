#ifndef MAUSRUN_ART_H
#define MAUSRUN_ART_H

/*
 * mausrun -- the art from the card (docs/mausrun.md, "Art").
 *
 * The art is compiled in (sprites.c, made by gen_sprites.py from
 * sw/data/mausrun/tiles.zsp and sprites.zsp). At start, a tiles.zsp
 * or sprites.zsp in /data/mausrun replaces the tiles and sprites of the
 * same name, so art drawn in pix shows the next time mausrun starts.
 * A sheet that does not parse is reported and the built-in art kept.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdbool.h>
#include "../../common/zsprite.h"

/* Before anything copies the art (strips_init(), far_init()). */
void art_load(void);

/* Sheet `s`'s tiles (or sprites) over the art of the same name: how
 * many replaced. A name the game has no art for, or a size other than
 * 16x16, is skipped, and the first is said in `why`. Pure: the host
 * test calls it. */
int art_apply(const zsp_sheet_t *s, bool tiles, char *why, int whylen);

#endif
