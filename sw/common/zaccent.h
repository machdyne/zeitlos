#ifndef ZACCENT_H
#define ZACCENT_H

/*
 * Zeitlos -- desktop colour: the accent palette (docs/color.md,
 * "Desktop colour").
 *
 * On a board with the colour window and eight planes, wm can show the
 * whole desktop in four colours: COLWIN set over the entire screen, and
 * every palette entry made to depend on two bits only -- the desktop's
 * own (bit 0) and the ACCENT bit, the second VRAM sheet's plane at the
 * pixel's own position (bit 4). The window's three other planes (bits
 * 5..7) read the second sheet 320 columns and 240 rows on, i.e. other
 * pixels' accent bits, and the palette ignores them. No new hardware.
 *
 * An accent bit TINTS a pixel rather than adding a colour: black shows
 * as A0, ink as A1. So a tinted strip of text is still text.
 *
 *   entry  bit 4  bit 0
 *     0      0      0     black (the phosphor's background)
 *     1      0      1     ink   (the phosphor)
 *    16      1      0     A0, tinted black -- dark
 *    17      1      1     A1, tinted ink   -- light
 *
 * The palettes keep two rules:
 *
 *  - A0 is dark and A1 light, so the pointer's XOR (which inverts bit
 *    0 and the accent bit together) always lands on a contrasting
 *    colour: black <-> A1, ink <-> A0.
 *  - Each accent channel sits on the same side of 8 as the colour it
 *    tints, so on a 1-bit-per-channel VGA output (only the top bit of
 *    each channel reaches the connector) the accents fold onto black
 *    and the ink: the desktop looks exactly as it does without colour.
 *
 * Pure arithmetic, no MMIO except z_accent_write_palette() (zsoc.h):
 * sw/common/tests/test_accent.c runs it on the build host.
 */

#include <stdint.h>
#include <stdbool.h>

#define Z_ACCENT_BLACK  0
#define Z_ACCENT_INK    1
#define Z_ACCENT_A0     2
#define Z_ACCENT_A1     3

/* The four colours (RGB444) for a phosphor mode (Z_VIDEO_MODE_*) at a
 * brightness level (0..15, 7 the default): out[Z_ACCENT_*]. The black
 * and the ink follow the monochrome path's own formulas (rtl/gpu/
 * gpu_video.v) rounded to four bits; the accents scale with the level
 * the same way. `vga`: the board has a 1-bit VGA output too (the
 * palette feeds both), so every channel is kept on its side of 8 at
 * any level -- below the default the desktop then dims on DDMI only as
 * far as VGA still shows it. */
void z_accent_palette(uint32_t mode, uint32_t level, bool vga, uint16_t out[4]);

/* The palette entry an index (0..255) shows: z_accent's colour for its
 * bits 0 and 4. */
static inline int z_accent_entry_colour(uint32_t index) {
	return (int)((index & 1u) | (((index >> 4) & 1u) << 1));
}

/* All 256 entries from the four colours (zsoc.h's palette register). */
void z_accent_write_palette(const uint16_t c[4]);

/* -- themes (docs/color.md, "Themes") --
 *
 * A theme is a pair of accents, or a whole four-colour scheme:
 *
 *  - ACCENTS ONLY ("rides on the phosphor"): { a0, a1 } = { dark tint,
 *    light tint }. Black and the ink stay the phosphor's own -- exact,
 *    through the ink path on gateware with COLWIN's DESK -- and follow
 *    the display colour and the brightness. On paper the pair swaps,
 *    so the paper takes the light tint and the ink the dark one.
 *  - FULL: black, ink, a0, a1 all the theme's own (`full`). The
 *    phosphor is not consulted. Black and the ink then have to go
 *    through the palette, so wm leaves DESK off: the pointer's XOR
 *    tints (black <-> a1, ink <-> a0), and a full theme keeps those two
 *    pairs apart in brightness.
 *
 * Theme 0 is "phosphor", the default: each display colour's own pair
 * (z_accent_palette()). All colours are RGB444 at brightness 7 and
 * scale with the level as the ink does -- a full theme dims below 7
 * but does not go brighter than its own colours. */
typedef struct {
	const char *id;		/* zeitlos.cfg's system.video.desktop_theme */
	uint16_t a0, a1;
	uint16_t black, ink;	/* full themes only */
	bool full;
} z_accent_theme_t;

extern const z_accent_theme_t z_accent_themes[];
extern const int z_accent_theme_count;

/* What settings shows for each theme, in the same order. In
 * zaccent_names.c, so wm (which needs only the ids) does not carry
 * them. */
extern const char *const z_accent_theme_names[];

/* The theme with this id (case-insensitive), or -1. */
int z_accent_theme_find(const char *id);

/* "123,CA8" -- two RGB444 colours in hex, '#' allowed, separated by a
 * comma or spaces -- into out[0], out[1]. False if it is not that. */
bool z_accent_parse_pair(const char *s, uint16_t out[2]);

/* What wm shows: a theme, optionally overridden by custom accents
 * (system.video.desktop_accents: dark,light) and/or custom black and
 * ink (system.video.desktop_base: black,ink -- which makes it full). */
typedef struct {
	int theme;			/* index into z_accent_themes */
	bool have_acc, have_base;
	uint16_t acc[2], base[2];
} z_accent_spec_t;

/* The four colours for a spec, display colour and level (as
 * z_accent_palette(), which is the phosphor theme's). */
void z_accent_palette_spec(const z_accent_spec_t *sp, uint32_t mode,
	uint32_t level, bool vga, uint16_t out[4]);

/* True if the spec keeps the phosphor's black and ink, so the exact
 * ink path (COLWIN's DESK) can show them; false for a full theme or a
 * custom base. */
bool z_accent_spec_exact(const z_accent_spec_t *sp);

#endif
