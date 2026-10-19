#ifndef BK_SOUND_H
#define BK_SOUND_H

/* blocks -- sounds and music (bk_sound.c). */

#include "../../common/games/zsfx.h"

enum {
	BK_SND_MOVE = 0,
	BK_SND_ROTATE,
	BK_SND_KICK,
	BK_SND_LOCK,
	BK_SND_HARD,
	BK_SND_HOLD,
	BK_SND_CLEAR1,
	BK_SND_CLEAR2,
	BK_SND_CLEAR3,
	BK_SND_FOUR,
	BK_SND_TWIST,
	BK_SND_PERFECT,
	BK_SND_LEVEL,
	BK_SND_OVER,
	BK_SND_DONE,
	BK_SND_COUNT,
	BK_SND_GO,
	BK_SND_MENU,
	BK_SND_SELECT,
	BK_SND_LETTER,
	BK_SND_LAND,
	BK_SND_N
};

void bk_sound(int which);
void bk_sound_combo(int combo);     /* rises with the combo */

extern const zsfx_song_t bk_song_game;
extern const zsfx_song_t bk_song_title;

#endif
