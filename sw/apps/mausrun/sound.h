#ifndef MAUSRUN_SOUND_H
#define MAUSRUN_SOUND_H

/*
 * mausrun -- the sounds and the tunes, on the arcade games' sound
 * engine (sw/common/games/zsfx.[ch], docs/sfx.md). mausrun's own
 * music.c, which zsfx was made general from, is gone.
 */

#include "../../common/games/zsfx.h"

enum {
	SND_JUMP = 0,
	SND_LAND,
	SND_COIN,         /* a cheese */
	SND_OUCH,         /* a cat, a frog, an owl or thorns: a heart less */
	SND_FALL,         /* into a pit */
	SND_SPRING,
	SND_STOMP,        /* on a cat's head: it is dizzy */
	SND_POWER,        /* a power-up taken */
	SND_POP,          /* the bubble used up */
	SND_SPLASH,
	SND_CHECK,        /* a mouse hole: the checkpoint */
	SND_GOAL,         /* the cheese wheel */
	SND_HEART,        /* a heart more */
	SND_BUMP,         /* a ? box from below */
	SND_CRUMBLE,      /* a biscuit giving way */
	SND_CROAK,        /* a frog hopping */
	SND_HOOT,         /* an owl starting its swoop */
	SND_WAKE,         /* a sleeping cat waking */
	SND_MENU,
	SND_SELECT,
	SND_TICK,         /* the results counting up */
	SND_COUNT
};

void snd_play(int which, int pan_x);     /* pan_x: view x 0..319, -1 centre */

extern const zsfx_song_t snd_song_meadow;
extern const zsfx_song_t snd_song_garden;     /* round 4 */
extern const zsfx_song_t snd_song_kitchen;
extern const zsfx_song_t snd_song_roofs;

/* the tune for a level's music number (its theme's, MR_THEME_*) */
const zsfx_song_t *snd_song(int music);

#endif
