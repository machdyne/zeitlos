#ifndef BENCH_DRAW_H
#define BENCH_DRAW_H

/*
 * bench -- drawing the bench into a one-bit canvas: the cards, laid out
 * on a grid that follows the window's width, and the bus log. Portable:
 * bench_app.c copies the canvas to the window, tests/ render it on a
 * host.
 *
 * The canvas is in the framebuffer's own format (zgfx.h): pixel x of a
 * row is bit (x & 31) of word (x >> 5), lowest bit leftmost, so
 * z_fb_hw_blit_mem() copies it as it is. 1 is white (ink), 0 black.
 *
 * The canvas is the VIEW, not the whole document: drawing starts at an
 * origin (the scroll position), so a canvas no bigger than the window
 * shows any part of a bench of any size. (bench keeps it in .bss: an
 * app's malloc heap shares 16KB with its stack, docs/executables.md.)
 *
 * Layout: cells of BD_CELL_W x BD_CELL_H. A one-pin part takes one cell;
 * a part with more pins takes three cells across and as many down as its
 * rows of eight pins need. A part with `place COL ROW` goes there; the
 * rest fill the first free cells, in netlist order, row by row.
 */

#include <stdint.h>
#include <stdbool.h>
#include "core.h"

#define BD_CELL_W   72
#define BD_CELL_H   52
#define BD_MAX_W    640         /* the widest canvas: the screen */

typedef struct {
    uint32_t *px;
    int w, h, wpl;              /* pixels across and down; words per row */
} canvas_t;

/* where each part's card is, after bd_layout() */
typedef struct {
    int16_t x, y, w, h;
} card_t;

extern card_t bd_cards[BN_PARTS];

/* Lays the parts out for a canvas `width` pixels wide: the height the
 * canvas needs. */
int bd_layout(int width);

/* Draws the cards (as laid out) into c, whose top left corner is the
 * document's point (ox, oy). */
void bd_draw(canvas_t *c, int ox, int oy);

/* The bus log, newest at the bottom, as text lines: the document's
 * height when c is 0; otherwise drawn, from the document's line oy. */
int bd_draw_log(canvas_t *c, int oy);

/* Text lines (the modules' consoles): the height when c is 0,
 * otherwise drawn from the document's line oy. */
int bd_draw_text(canvas_t *c, const char *text, int oy);

/* What is at (x, y) in the canvas: the part's index and, for a
 * multi-pin part, the pin's (-1 elsewhere on the card); -1 if nothing. */
int bd_hit(int x, int y, int *pin);

/* One line about a pin's net into buf: its name, level and what is on
 * it ("x1.P00: high -- x1.P00 l0.PIN") */
void bd_net_info(part_t *p, int pin, char *buf, int len);

#endif
