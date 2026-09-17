#ifndef KG_LEVELUP_H
#define KG_LEVELUP_H

/*
 * kidgames -- the shared "LEVEL UP!" screen.
 *
 * One screen, called by every game when the player's level increases,
 * so the reward looks the same everywhere. A kid should recognise it
 * instantly without reading it, which is why the number is the biggest
 * thing on the screen and the words are secondary.
 *
 * Timed rather than waiting for a key: the original's reasoning, and
 * it holds. A celebration that requires pressing something to dismiss
 * is a celebration that can be missed by a kid who is already
 * reaching for the next answer, and it needs an instruction to be
 * read.
 */

#include "kg.h"

void levelup_celebrate(int new_level);

#endif
