/*
 * Zeitlos -- desktop colour: the accent palette. See zaccent.h.
 */

#include "zaccent.h"
#include "zsoc.h"
#include <stddef.h>

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

/* The lit-channel mask of a colour: >= 8 counts as lit on 1-bit VGA. */
static uint8_t lit_of(uint16_t c) {
	return (uint8_t)((((c >> 8) & 15u) >= 8u ? 4u : 0u) |
		(((c >> 4) & 15u) >= 8u ? 2u : 0u) | ((c & 15u) >= 8u ? 1u : 0u));
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

/* -- themes --
 *
 * Accent pairs are { dark tint, light tint } for a dark display; full
 * themes give all four. Every full theme keeps black/a1 and ink/a0 well
 * apart, for the pointer (tests/test_accent.c checks). */
const z_accent_theme_t z_accent_themes[] = {
	{ "phosphor", 0, 0, 0, 0, false },
	{ "slate", 0x123, 0xCA8, 0, 0, false },
	{ "umber", 0x210, 0xEC6, 0, 0, false },
	{ "pine", 0x021, 0x3B5, 0, 0, false },
	{ "navy", 0x013, 0x9CE, 0, 0, false },
	{ "plum", 0x302, 0xE9B, 0, 0, false },
	{ "oxblood", 0x300, 0xEDB, 0, 0, false },
	{ "teal", 0x033, 0xF87, 0, 0, false },
	{ "graphite", 0x222, 0xAAA, 0, 0, false },
	{ "midnight", 0x012, 0xFB4, 0, 0, false },
	{ "aubergine", 0x213, 0xBAE, 0, 0, false },
	{ "ocean", 0x024, 0x7DC, 0, 0, false },
	{ "cocoa", 0x321, 0xFCA, 0, 0, false },
	{ "moss", 0x121, 0xBE5, 0, 0, false },
	{ "solarized-dark", 0x034, 0xBA0, 0x023, 0x899, true },
	{ "solarized-light", 0xEEC, 0x28D, 0xFFE, 0x678, true },
	{ "nord", 0x445, 0x8BC, 0x223, 0xDDE, true },
	{ "gruvbox", 0x332, 0xFB3, 0x222, 0xEDB, true },
	{ "dracula", 0x446, 0xB9F, 0x223, 0xEEE, true },
	{ "workbench", 0x037, 0xF80, 0x05A, 0xFFF, true },
	{ "cga", 0x0AA, 0xF5F, 0x000, 0xFFF, true },
};
const int z_accent_theme_count = (int)(sizeof(z_accent_themes) / sizeof(z_accent_themes[0]));

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int z_accent_theme_find(const char *id) {
	if (!id) return -1;
	for (int i = 0; i < z_accent_theme_count; i++) {
		const char *a = z_accent_themes[i].id, *b = id;
		while (*a && lower((unsigned char)*b) == *a) { a++; b++; }
		if (!*a && !*b) return i;
	}
	return -1;
}

static int hexv(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	c = lower(c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* One colour: optional '#', exactly three hex digits. */
static const char *parse_one(const char *s, uint16_t *out) {
	uint16_t v = 0;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '#') s++;
	for (int k = 0; k < 3; k++) {
		int h = hexv((unsigned char)s[k]);
		if (h < 0) return NULL;
		v = (uint16_t)((v << 4) | (uint16_t)h);
	}
	if (hexv((unsigned char)s[3]) >= 0) return NULL;	/* not four or six digits */
	*out = v;
	return s + 3;
}

bool z_accent_parse_pair(const char *s, uint16_t out[2]) {
	uint16_t a, b;
	if (!s) return false;
	s = parse_one(s, &a);
	if (!s) return false;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == ',') s++;
	s = parse_one(s, &b);
	if (!s) return false;
	while (*s == ' ' || *s == '\t') s++;
	if (*s) return false;
	out[0] = a; out[1] = b;
	return true;
}

bool z_accent_spec_exact(const z_accent_spec_t *sp) {
	if (sp->have_base) return false;
	if (sp->theme > 0 && sp->theme < z_accent_theme_count && z_accent_themes[sp->theme].full)
		return false;
	return true;
}

void z_accent_palette_spec(const z_accent_spec_t *sp, uint32_t mode,
	uint32_t level, bool vga, uint16_t out[4]) {
	const z_accent_theme_t *t;
	uint16_t a0, a1;
	bool full;

	if (level > 15u) level = 7u;
	if (mode >= 4u) mode = 0;
	t = (sp->theme > 0 && sp->theme < z_accent_theme_count) ? &z_accent_themes[sp->theme] : 0;

	/* the phosphor theme, no overrides: exactly as before */
	if (!t && !sp->have_acc && !sp->have_base) {
		z_accent_palette(mode, level, vga, out);
		return;
	}

	full = sp->have_base || (t && t->full);
	if (full) {
		uint16_t base[2], acc[2];
		base[0] = sp->have_base ? sp->base[0] : (t && t->full) ? t->black : 0x000;
		base[1] = sp->have_base ? sp->base[1] : (t && t->full) ? t->ink : 0x888;
		if (sp->have_acc) { acc[0] = sp->acc[0]; acc[1] = sp->acc[1]; }
		else if (t) { acc[0] = t->a0; acc[1] = t->a1; }
		else { acc[0] = accents[0][0]; acc[1] = accents[0][1]; }
		/* a full theme only dims: brighter than its own colours would
		 * wash a light theme's black and ink together */
		if (level > 7u) level = 7u;
		out[Z_ACCENT_BLACK] = scale(base[0], level);
		out[Z_ACCENT_INK]   = scale(base[1], level);
		out[Z_ACCENT_A0]    = scale(acc[0], level);
		out[Z_ACCENT_A1]    = scale(acc[1], level);
	} else {
		/* the phosphor's black and ink; the pair swaps on paper */
		z_accent_palette(mode, level, false, out);
		if (sp->have_acc) { a0 = sp->acc[0]; a1 = sp->acc[1]; }
		else { a0 = t->a0; a1 = t->a1; }
		if (mode == 3u) { uint16_t x = a0; a0 = a1; a1 = x; }
		out[Z_ACCENT_A0] = scale(a0, level);
		out[Z_ACCENT_A1] = scale(a1, level);
	}

	if (vga) {
		uint8_t lb = full ? lit_of(out[Z_ACCENT_BLACK]) : lit[mode][0];
		uint8_t li = full ? lit_of(out[Z_ACCENT_INK]) : lit[mode][1];
		if (!full) {
			out[Z_ACCENT_BLACK] = vga_side(out[Z_ACCENT_BLACK], lb);
			out[Z_ACCENT_INK]   = vga_side(out[Z_ACCENT_INK], li);
		}
		out[Z_ACCENT_A0] = vga_side(out[Z_ACCENT_A0], lb);
		out[Z_ACCENT_A1] = vga_side(out[Z_ACCENT_A1], li);
	}
}
