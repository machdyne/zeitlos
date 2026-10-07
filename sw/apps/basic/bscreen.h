#ifndef BSCREEN_H
#define BSCREEN_H

/*
 * basic -- the BASIC computer's screen: 320x240 pixels, one bit each,
 * shared by text (40x30 characters, 8x8 font) and graphics, kept in the
 * app's own memory. basic_app.c copies it to the window or, full screen,
 * to a game-mode page; nothing here knows about either, so it is tested
 * on a host (tests/).
 *
 * The bitmap is in the framebuffer's own format (zgfx.h): pixel x of a
 * row is bit (x & 31) of word (x >> 5), least significant bit leftmost,
 * so z_fb_hw_blit_mem() copies it as it is.
 *
 * Text is always drawn white on black and replaces the cell's pixels.
 * Drawing uses the operation COLOR chose: 0 clears, 1 sets, 2 inverts.
 */

#include <stdint.h>
#include <stdbool.h>

#define BS_W        320
#define BS_H        240
#define BS_WORDS    (BS_W / 32)         /* per row */
#define BS_STRIDE   (BS_WORDS * 4)      /* bytes per row */
#define BS_COLS     40
#define BS_ROWS     30

#define BS_CLEAR    0
#define BS_SET      1
#define BS_INVERT   2

typedef struct {
    uint32_t px[BS_H * BS_WORDS];
    int row, col;           /* text cursor */
    int op;                 /* drawing operation (COLOR) */
    bool cursor;            /* the block cursor is shown */
    int dirty_y0, dirty_y1; /* rows changed since bs_clean(); y0 >= y1: none */
} bscreen_t;

void bs_init(bscreen_t *s);
void bs_cls(bscreen_t *s);                  /* clear, cursor home */

/* Text: a Latin-9 byte at the cursor, or CR (column 0), LF (next row,
 * scrolling the whole screen up a row at the bottom), BS (back one and
 * erase, onto the previous row from column 0). A full row continues on
 * the next. */
void bs_putc(bscreen_t *s, uint8_t c);
bool bs_locate(bscreen_t *s, int row, int col);     /* false: off screen */
void bs_cursor(bscreen_t *s, bool on);              /* the block cursor */

/* Drawing, with the current operation. Points off screen are skipped. */
void bs_plot(bscreen_t *s, int x, int y);
void bs_line(bscreen_t *s, int x0, int y0, int x1, int y1);
void bs_box(bscreen_t *s, int x0, int y0, int x1, int y1, bool fill);
void bs_circle(bscreen_t *s, int cx, int cy, int r);
int bs_point(const bscreen_t *s, int x, int y);     /* 1 set, 0 clear or off screen */

/* The rows changed since the last bs_clean(), for copying out. */
bool bs_dirty(const bscreen_t *s, int *y0, int *y1);
void bs_clean(bscreen_t *s);

#endif
