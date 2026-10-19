#ifndef SN_SOUND_H
#define SN_SOUND_H

/* snake -- sounds and music (sn_sound.c). */

#include "../../common/games/zsfx.h"

enum {
	SN_SND_EAT = 0,
	SN_SND_BONUS_EAT,
	SN_SND_BONUS_SHOW,
	SN_SND_BONUS_GONE,
	SN_SND_DIE_CRASH,
	SN_SND_DIE_FALL,
	SN_SND_POP,
	SN_SND_LEVEL,
	SN_SND_MENU,
	SN_SND_SELECT,
	SN_SND_FANFARE,
	SN_SND_GAMEOVER,
	SN_SND_READY,
	SN_SND_LETTER,
	SN_SND_COUNT
};

/* Play a sound, panned to x (0..319); x < 0 for the centre. */
void sn_sound(int which, int x);

extern const zsfx_song_t sn_song_game;
extern const zsfx_song_t sn_song_title;

#endif
