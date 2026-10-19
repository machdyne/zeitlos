#ifndef BR_SOUND_H
#define BR_SOUND_H

/* bricks -- sounds and music (br_sound.c). */

#include "../../common/games/zsfx.h"

enum {
	BR_SND_PADDLE = 0,
	BR_SND_WALL,
	BR_SND_SILVER,
	BR_SND_GOLD,
	BR_SND_DROP,
	BR_SND_WIDE,
	BR_SND_MULTI,
	BR_SND_SLOW,
	BR_SND_CATCH_POWER,
	BR_SND_LASER_POWER,
	BR_SND_LIFE,
	BR_SND_LAUNCH,
	BR_SND_CATCH,
	BR_SND_FIRE,
	BR_SND_BOLT_END,
	BR_SND_BALL_LOST,
	BR_SND_LOST,
	BR_SND_CLEAR,
	BR_SND_READY,
	BR_SND_ROW,
	BR_SND_MENU,
	BR_SND_SELECT,
	BR_SND_LETTER,
	BR_SND_OVER,
	BR_SND_COUNT
};

/* Play a sound, panned to x (0..319); x < 0 for the centre. */
void br_sound(int which, int x);

/* A brick breaking: each brick in a chain (between two touches of the
 * paddle) a step higher, up a scale. */
void br_sound_break(int chain, int x);

extern const zsfx_song_t br_song_game;
extern const zsfx_song_t br_song_title;

#endif
