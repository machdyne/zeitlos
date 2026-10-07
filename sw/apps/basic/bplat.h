#ifndef BPLAT_H
#define BPLAT_H

/*
 * basic -- what the graphics extensions (bext.c) need from the app:
 * basic_app.c on Zeitlos, tests/host.c on a host.
 */

#include <stdint.h>
#include <stdbool.h>
#include "bscreen.h"

bscreen_t *bp_screen(void);

/* SCREEN: full screen (game mode) or a window. false if full screen is
 * not available here. */
bool bp_full_screen(bool on);

/* KEY: the next key pressed while the program runs, or 0: a Latin-9
 * byte, or one of the codes below for keys without a character */
uint8_t bp_key(void);
#define BP_KEY_UP       128
#define BP_KEY_DOWN     129
#define BP_KEY_LEFT     130
#define BP_KEY_RIGHT    131

/* SYNC: show the screen now, then wait for the next video frame */
void bp_sync(void);

#endif
