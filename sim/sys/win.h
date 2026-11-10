#ifndef ZSYS_WIN_H
#define ZSYS_WIN_H
/*
 * A window for zsys (win_sdl.c): the screen, the keyboard, the mouse and
 * the sound, built when SDL2 is there (ZSYS_WINDOW). The browser build
 * has its own (web/).
 */
#include <stdbool.h>
#include "zsys.h"

bool win_open(int scale, const char *title, bool sound);	// false: no display, or no SDL
bool win_poll(zsys_t *s);			// input to the machine; false once closed
void win_present(zsys_t *s);			// the dirty rows, then a frame
void win_close(void);
void win_fullscreen(bool on);			// the host's full screen (Ctrl+Alt+Enter toggles it)

#endif
