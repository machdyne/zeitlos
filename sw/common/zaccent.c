/*
 * Zeitlos -- desktop colour: the accent palette. See zaccent.h.
 */

#include "zaccent.h"
#include "zsoc.h"

/* The accents at the default level (7), per phosphor: { A0, A1 }.
 * Neo-retro, subdued: white -- slate and sand; amber -- umber and
 * gold; green -- pine and mint; paper -- blue-grey and navy. */
static const uint16_t accents[4][2] = {
	{ 0x123, 0xCA8 },   /* white */
	{ 0x210, 0xEC6 },   /* amber */
	{ 0x021, 0x3B5 },   /* green */
	{ 0x9AB, 0x235 },   /* paper: A0 tints the paper, A1 the ink */
};

/* Which channels the plain colour has lit (R 4, G 2, B 1), per
 * phosphor: { black, ink }. Paper's "black" is the paper. */
static const uint8_t lit[4][2] = {
	{ 0, 7 }, { 0, 6 }, { 0, 2 }, { 7, 0 },
};

static uint32_t nib(uint32_t v8) {
	return (v8 + 8u) / 17u;
}

static uint16_t rgb(uint32_t r8, uint32_t g8, uint32_t b8) {
	return (uint16_t)((nib(r8) << 8) | (nib(g8) << 4) | nib(b8));
}

/* A colour defined at level 7, scaled to `level`. */
static uint16_t scale(uint16_t c, uint32_t level) {
	uint16_t out = 0;
	for (int sh = 8; sh >= 0; sh -= 4) {
		uint32_t v = (((c >> sh) & 15u) * (level + 1u) + 4u) / 8u;
		if (v > 15u) v = 15u;
		out |= (uint16_t)(v << sh);
	}
	return out;
}

/* Each channel on the side of 8 its lit mask says: >= 8 lit, <= 7 not. */
static uint16_t vga_side(uint16_t c, uint8_t mask) {
	uint16_t out = 0;
	for (int ch = 0; ch < 3; ch++) {
		int sh = 8 - 4 * ch;
		uint32_t v = (c >> sh) & 15u;
		if (mask & (4u >> ch)) { if (v < 8u) v = 8u; }
		else if (v > 7u) v = 7u;
		out |= (uint16_t)(v << sh);
	}
	return out;
}

void z_accent_palette(uint32_t mode, uint32_t level, bool vga, uint16_t out[4]) {
	uint32_t w, ar, ag;

	if (mode >= 4u) mode = 0;
	if (level > 15u) level = 7u;

	/* the monochrome path's inks, as gpu_video.v works them out */
	w = level >= 15u ? 255u : 16u * (level + 1u);
	ar = w + w / 2u + w / 8u + w / 64u;
	ag = w + w / 8u + w / 16u + w / 32u + w / 128u;
	if (ar > 255u) ar = 255u;
	if (ag > 255u) ag = 255u;

	switch (mode) {
	case 1:  out[Z_ACCENT_BLACK] = 0; out[Z_ACCENT_INK] = rgb(ar, ag, 0); break;
	case 2:  out[Z_ACCENT_BLACK] = 0; out[Z_ACCENT_INK] = rgb(0, w, 0); break;
	case 3:  out[Z_ACCENT_BLACK] = rgb(w, w, w); out[Z_ACCENT_INK] = 0; break;
	default: out[Z_ACCENT_BLACK] = 0; out[Z_ACCENT_INK] = rgb(w, w, w); break;
	}
	out[Z_ACCENT_A0] = scale(accents[mode][0], level);
	out[Z_ACCENT_A1] = scale(accents[mode][1], level);

	if (vga) {
		out[Z_ACCENT_BLACK] = vga_side(out[Z_ACCENT_BLACK], lit[mode][0]);
		out[Z_ACCENT_INK]   = vga_side(out[Z_ACCENT_INK], lit[mode][1]);
		out[Z_ACCENT_A0]    = vga_side(out[Z_ACCENT_A0], lit[mode][0]);
		out[Z_ACCENT_A1]    = vga_side(out[Z_ACCENT_A1], lit[mode][1]);
	}
}

void z_accent_write_palette(const uint16_t c[4]) {
	for (uint32_t i = 0; i < 256u; i++)
		z_color_set_palette(i, c[z_accent_entry_colour(i)]);
}
