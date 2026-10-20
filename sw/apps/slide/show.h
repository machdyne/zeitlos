#ifndef SLIDE_SHOW_H
#define SLIDE_SHOW_H

/*
 * slide -- the show: the whole screen, card after card
 * (docs/slide_app.md, "Showing a deck").
 */

#include <stdbool.h>
#include "../../common/zwin.h"
#include "deck.h"

// The screen-sized buffers a show draws in; NULL if the pool has no room.
bool show_alloc(void);

// Runs a show of d from card `start`, until Escape (or Super+Esc, or the
// last card's end). `win` is the editor's window, whose messages are
// answered meanwhile. Returns the card it ended on.
int show_run(deck_t *d, int start, z_win_t *win);

// What a show did that the editor finishes: a deck an action opened
// ("(open ...)" ends the show), else NULL. Good until the next show.
const char *show_open_request(void);

// Starts an app for an action: "clock", "view /media/images/x.jpg".
void show_launch(const char *cmd);

// Copies a 1bpp bitmap rectangle to the screen at (dx, dy), through the
// blitter where it can. Shared with the editor.
void blit_to_screen(const uint32_t *src, int wpl, int sx, int sy, int dx, int dy,
	int w, int h);

#endif
