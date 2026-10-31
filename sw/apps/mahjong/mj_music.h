#ifndef MJ_MUSIC_H
#define MJ_MUSIC_H

/*
 * Zeitlos mahjong -- the music and the sounds (sw/common/games/zsfx.h).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Heard on the screen face, whose frames step the mixer; the desktop
 * face has no frame clock to step it with, and is silent. M turns the
 * music off and on (the arcade runtime's key, docs/arcade.md).
 */

#include "../../common/games/zsfx.h"

/* "Tea Garden": a slow tune in the G major pentatonic -- G A B D E --
 * on three channels: the tune on a soft sine, a harmony under it, a
 * bass. No drums: this is a game to think in. */
extern const zsfx_song_t mj_song;

/* The sounds, by mj_play.h's MJ_SND_*. */
extern const zsfx_fx_t mj_fx[];
extern const int mj_nfx;

#endif
