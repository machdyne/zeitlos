#ifndef ZSPRITE_H
#define ZSPRITE_H

/*
 * Zeitlos -- sprite sheets (.zsp): read, write, and reduce to the
 * colours a board shows. See docs/zsp.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A sheet is text: named sprites of any size, each pixel a colour index
 * or transparent, and a palette of 2, 4, 16, 64 or 256 colours ordered
 * so that each smaller palette is the start of the larger one. Every
 * entry also says what stands in for it with fewer colours:
 *
 *   to64   the entry 0..63 at 64 colours   (256-colour sheets)
 *   to16   the entry 0..15 at 16 colours   (64- and 256-colour sheets)
 *   slot   one of 4 slots, at 4 colours
 *   ink    one of the 17 dither greys at 2: 0 black .. 16 white
 *
 * so a sheet is shown the same way everywhere with fewer colours: pix's
 * preview, a game on a 16-colour board and one on a 64-colour board all
 * use these tables, and nothing is worked out from RGB when the art is
 * shown. Defaults are computed from RGB once (zsp_defaults()), when a
 * palette is made or a colour changes, and after that the artist's.
 *
 * Pure C, no hardware and no file calls: pix, the games and the host
 * tests share it.
 */

#include <stdint.h>
#include <stdbool.h>

#define ZSP_T          0x100     /* a transparent pixel */
#define ZSP_NAME       24        /* a sprite's name, NUL included */
#define ZSP_DIFFER_MAX 32        /* "must differ" pairs */
#define ZSP_MAX_DIM    1024

/* zsp_colour_t.given: which fields the artist set (written with a '!'
 * after them); the others follow zsp_defaults(). */
#define ZSP_GIVEN_TO64 0x01u
#define ZSP_GIVEN_TO16 0x02u
#define ZSP_GIVEN_SLOT 0x04u
#define ZSP_GIVEN_INK  0x08u
#define ZSP_GIVEN_ALL  0x0fu

typedef struct {
	uint16_t rgb;                 /* RGB444 */
	uint8_t  to64, to16, slot, ink;
	uint8_t  given;               /* ZSP_GIVEN_* */
} zsp_colour_t;

typedef struct {
	int          colours;         /* 2, 4, 16, 64 or 256 */
	zsp_colour_t pal[256];
	uint16_t     slot_rgb[4];     /* the 4-colour form's own colours */
	int          ndiffer;
	uint8_t      differ[ZSP_DIFFER_MAX][2];
} zsp_palette_t;

typedef struct {
	char      name[ZSP_NAME];
	uint16_t  w, h;
	uint16_t *px;                 /* w*h: an index, or ZSP_T */
} zsp_sprite_t;

typedef struct {
	zsp_palette_t p;
	int           n, cap;
	zsp_sprite_t *spr;
	char         *extra;          /* other lines (anim, comments), kept */
	uint32_t      extra_len;
} zsp_sheet_t;

/* -- the palette -- */

/* A palette of `colours`: the Commodore 64's sixteen first (0 black,
 * 1 white), then shades of each, every field from zsp_defaults(). */
void zsp_palette_init(zsp_palette_t *p, int colours);

/* Fields of entry i not given by the artist, from its RGB and the
 * entries before it: to64 and to16 the nearest smaller-palette entry,
 * slot from the brightness quartile, ink from the brightness. */
void zsp_defaults(zsp_palette_t *p, int i);
void zsp_defaults_all(zsp_palette_t *p);

/* Brightness of an RGB444 colour, 0..15. */
int zsp_lum(uint16_t rgb);

/* Index `idx` (or ZSP_T) as shown with `target` colours (2, 4, 16, 64,
 * 256): through to64, to16, the slot, or ink >= 8. Raising changes
 * nothing; ZSP_T stays transparent. */
int zsp_reduce(const zsp_palette_t *p, int idx, int target);

/* What index `idx` looks like with `target` colours, as RGB444: the
 * entry it reduces to, its slot's colour, or its ink's grey. */
uint16_t zsp_shown_rgb(const zsp_palette_t *p, int idx, int target);

/* Index idx's ink, 0 (black) .. 16 (white): its 2-colour form, and
 * the grey a monochrome screen shows it in. 0 for ZSP_T. */
int zsp_ink(const zsp_palette_t *p, int idx);

/* The first "must differ" pair that reaches the same entry with fewer
 * colours (*a, *b, and *tier the count), checking 64, 16, 4 and 2 in
 * that order (only those below the sheet's own count). False if none
 * collide. */
bool zsp_collision(const zsp_palette_t *p, int *a, int *b, int *tier);

/* -- the palette as bytes: a colour picture's (zbm.h) -- */

/* The palette block a colour ZBM carries (zbm.h, "colour pictures"):
 * its size, z_bm_pal_size(colours, ndiffer), and the bytes. */
uint32_t zsp_palette_bin_size(const zsp_palette_t *p);
void zsp_palette_to_bin(const zsp_palette_t *p, uint8_t *out);

/* And back: false if it is not one (a count that is not 2, 4, 16, 64
 * or 256, an entry past it, a block longer than len). *used is its
 * size. The entries a block does not have are the default palette's. */
bool zsp_palette_from_bin(zsp_palette_t *p, const uint8_t *in, uint32_t len, uint32_t *used);

/* -- the sheet -- */

void zsp_init(zsp_sheet_t *s, int colours);
void zsp_free(zsp_sheet_t *s);

/* A new sprite, every pixel transparent; NULL if out of memory, too big
 * or the name is empty or taken. The pointer is good until the next
 * zsp_add(). */
zsp_sprite_t *zsp_add(zsp_sheet_t *s, const char *name, int w, int h);
int zsp_find(const zsp_sheet_t *s, const char *name);

/* Text in, a sheet out. False with a reason ("line 7: ...") in err. A
 * sheet without a `colours` line is 2 colours, and there '#', 'X' and
 * any other mark is ink: what the pointer's file and hand-drawn art
 * use. */
bool zsp_parse(zsp_sheet_t *s, const char *text, uint32_t len, char *err, int errlen);

/* The sheet as text (malloc'd, NUL-terminated), its length in *len. */
char *zsp_write(const zsp_sheet_t *s, uint32_t *len);

/* Sprite i reduced to `target` colours: w*h bytes of index into `px`
 * and, if `opaque` is not NULL, 1 where a pixel is drawn. Transparent
 * pixels are 0xff in px (with opaque, 0). For zarcade's
 * ar_sprite_from_indices(). */
void zsp_pixels(const zsp_sheet_t *s, int i, int target, uint8_t *px, uint8_t *opaque);

#endif
