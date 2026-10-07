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
 * Text is drawn in the ink colour on the paper colour (white on black
 * until a program says otherwise) and replaces the cell's pixels.
 * Drawing uses the operation COLOR chose: 0 draws black, 1 draws the
 * ink colour, 2 inverts.
 *
 * -- colour (docs/basic_app.md, docs/color.md) --
 *
 * Sixteen colours, as four bitplanes: px is plane 0 and pl[] planes
 * 1..3, all in the same format, so a pixel's colour is bit k of plane
 * k. That is the game-mode colour hardware's own layout, so full screen
 * copies each plane to its quadrant as it is.
 *
 * Planes 1..3 are not touched until a program uses colour: `colour`
 * goes true (and stays true) the first time INK, PAPER or PALETTE asks
 * for anything but white on black. Until then every operation is the
 * monochrome one, pixel for pixel, and px alone is the picture -- so a
 * program written before colour existed draws exactly what it drew.
 *
 * COLOR 2 inverts plane 0 only: drawing a shape twice still erases it,
 * and on a black-and-white picture it is the inversion it always was.
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

#define BS_PLANE_WORDS (BS_H * BS_WORDS)

typedef struct {
    uint32_t px[BS_PLANE_WORDS];        /* plane 0 */
    uint32_t pl[3][BS_PLANE_WORDS];     /* planes 1..3: colour only */
    int row, col;           /* text cursor */
    int op;                 /* drawing operation (COLOR) */
    int ink, paper;         /* colours 0-15: INK, PAPER */
    bool colour;            /* a program has used colour; see above */
    uint16_t pal[16];       /* RGB444, PALETTE; the C64's to start */
    bool pal_dirty;         /* pal changed since bs_pal_clean() */
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
int bs_point(const bscreen_t *s, int x, int y);     /* colour 0-15; 0 off screen */

/* Colour. Each returns false for a value outside 0-15. */
bool bs_ink(bscreen_t *s, int c);           /* drawing and text colour */
bool bs_paper(bscreen_t *s, int c);         /* text background; CLS */
bool bs_palette(bscreen_t *s, int i, int r, int g, int b);
void bs_pal_clean(bscreen_t *s);

/* The colour picture as one plane, for a monochrome display: the
 * PAPER colour is black and every other colour white, so text and
 * drawings stay readable against their background. Rows y0..y1-1, into
 * dst in px's format (whole rows; dst is BS_PLANE_WORDS long). Without
 * colour this is a copy of px. */
void bs_mono(const bscreen_t *s, int y0, int y1, uint32_t *dst);

/* The rows changed since the last bs_clean(), for copying out. */
bool bs_dirty(const bscreen_t *s, int *y0, int *y1);
void bs_clean(bscreen_t *s);

#endif
