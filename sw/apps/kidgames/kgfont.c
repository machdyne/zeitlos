/*
 * kidgames -- big block letters. See kgfont.h.
 */

#include <string.h>

#include "kgfont.h"
#include "kgui.h"

/*
 * The 5x7 dot-matrix font, taken unchanged from the original's
 * src/bigfont.c. Rows are MSB-first in the low 5 bits: bit 4 is the
 * leftmost column.
 *
 * Indexed rather than searched: '0'-'9' then 'A'-'Z', so kg_glyph()
 * is two range tests and an array read. The original walked a 36-entry
 * table comparing a char field, which is fine at terminal rates and
 * is not fine inside a per-glyph draw loop.
 */
static const uint8_t FONT_DIGITS[10][KG_BIG_ROWS] = {
	{ 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E },	/* 0 */
	{ 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E },	/* 1 */
	{ 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F },	/* 2 */
	{ 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E },	/* 3 */
	{ 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 },	/* 4 */
	{ 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E },	/* 5 */
	{ 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E },	/* 6 */
	{ 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 },	/* 7 */
	{ 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E },	/* 8 */
	{ 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C },	/* 9 */
};

static const uint8_t FONT_ALPHA[26][KG_BIG_ROWS] = {
	{ 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 },	/* A */
	{ 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E },	/* B */
	{ 0x0F,0x10,0x10,0x10,0x10,0x10,0x0F },	/* C */
	{ 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E },	/* D */
	{ 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F },	/* E */
	{ 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 },	/* F */
	{ 0x0F,0x10,0x10,0x13,0x11,0x11,0x0F },	/* G */
	{ 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 },	/* H */
	{ 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E },	/* I */
	{ 0x07,0x02,0x02,0x02,0x02,0x12,0x0C },	/* J */
	{ 0x11,0x12,0x14,0x18,0x14,0x12,0x11 },	/* K */
	{ 0x10,0x10,0x10,0x10,0x10,0x10,0x1F },	/* L */
	{ 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 },	/* M */
	{ 0x11,0x19,0x15,0x15,0x13,0x11,0x11 },	/* N */
	{ 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E },	/* O */
	{ 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 },	/* P */
	{ 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D },	/* Q */
	{ 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 },	/* R */
	{ 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E },	/* S */
	{ 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 },	/* T */
	{ 0x11,0x11,0x11,0x11,0x11,0x11,0x0E },	/* U */
	{ 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 },	/* V */
	{ 0x11,0x11,0x11,0x15,0x15,0x15,0x0A },	/* W */
	{ 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 },	/* X */
	{ 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 },	/* Y */
	{ 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F },	/* Z */
};

const uint8_t *kg_glyph(char c)
{
	if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

	if (c >= '0' && c <= '9') return FONT_DIGITS[c - '0'];
	if (c >= 'A' && c <= 'Z') return FONT_ALPHA[c - 'A'];

	return 0;
}

int kg_big_advance(int scale)
{
	/* One whole cell of gap. A narrower gap makes 'II' and 'LL' read
	 * as one wide glyph at small scales, which is exactly where a
	 * beginning reader is least able to recover from it. */
	return (KG_BIG_COLS + 1) * scale;
}

int kg_big_h(int scale)
{
	return KG_BIG_ROWS * scale;
}

int kg_big_w(const char *s, int scale)
{
	int n = s ? (int)strlen(s) : 0;

	if (n <= 0) return 0;

	/* n glyphs and n-1 gaps: the trailing gap is not part of the
	 * string's width, and including it would centre every word half a
	 * cell to the left. */
	return n * KG_BIG_COLS * scale + (n - 1) * scale;
}

int kg_big_best_scale(const char *s, int maxw, int maxh)
{
	int scale;

	for (scale = KG_BIG_SCALE_MAX; scale > KG_BIG_SCALE_MIN; scale--) {
		if (kg_big_w(s, scale) <= maxw && kg_big_h(scale) <= maxh)
			return scale;
	}

	/* Deliberately returns the minimum rather than 0 when even that
	 * overflows. A word that is one pixel too wide should be drawn
	 * and clipped, not silently omitted -- kg_fill() clips to the
	 * playfield, so the failure mode is a visibly cut-off word, which
	 * says "this word is too long" to whoever sees it. Returning 0
	 * would say nothing at all. */
	return KG_BIG_SCALE_MIN;
}

/*
 * Paint one 5x7 bitmap at (x,y), `scale` pixels per cell.
 *
 * Two compressions, in this order:
 *
 *  1. VERTICAL: consecutive rows with identical bit patterns are one
 *     group, drawn at group_height * scale. 'L' is two groups, 'T' is
 *     two, 'I' is three.
 *
 *  2. HORIZONTAL: within a group, each run of set bits is one fill.
 *
 * Together these take the whole font from 19.4 lit cells per glyph to
 * 6.4 fills per glyph. Every fill goes through the blitter
 * (z_fb_hw_fill_rect via kg_fill), so the cost is per-OPERATION, not
 * per-pixel -- which is what makes the compression worth doing at all.
 */
static void draw_bitmap(int x, int y, const uint8_t *rows, int scale,
	int shade)
{
	int r = 0;

	while (r < KG_BIG_ROWS) {

		uint8_t bits = rows[r];
		int span = 1;
		int c;

		while (r + span < KG_BIG_ROWS && rows[r + span] == bits)
			span++;

		if (bits) {

			int gy = y + r * scale;
			int gh = span * scale;

			c = 0;
			while (c < KG_BIG_COLS) {

				int run;

				/* bit (COLS-1-c) is column c: MSB is leftmost */
				if (!(bits & (1u << (KG_BIG_COLS - 1 - c)))) { c++; continue; }

				run = 0;
				while (c + run < KG_BIG_COLS &&
					(bits & (1u << (KG_BIG_COLS - 1 - (c + run))))) run++;

				if (shade < 0)
					kg_fill(x + c * scale, gy, run * scale, gh, 1);
				else
					kg_shade(x + c * scale, gy, run * scale, gh, shade);

				c += run;

			}

		}

		r += span;

	}
}

int kg_big_putc(int x, int y, char c, int scale, kg_big_style_t style)
{
	const uint8_t *rows;

	switch (style) {

	case KG_BIG_BLANK:
		/* A solid block the size of a glyph. Shaded rather than
		 * filled, with a hard 1px frame around it, so it reads as "a
		 * letter belongs here" against both a solid letter next to it
		 * and a plain background -- and so it survives a bitstream
		 * with no dither, where the shade collapses to a flat fill
		 * and the frame is the only thing left saying it is a slot. */
		kg_shade(x, y, KG_BIG_COLS * scale, KG_BIG_ROWS * scale,
			KG_SHADE_BACK);
		kg_frame(x, y, KG_BIG_COLS * scale, KG_BIG_ROWS * scale, 1);
		return kg_big_advance(scale);

	case KG_BIG_GHOST:
		/* An outline only: "nothing here yet". Distinct from BLANK by
		 * SHAPE (empty vs filled), not by shade, so the two stay
		 * distinguishable with no dither hardware. */
		kg_frame(x, y, KG_BIG_COLS * scale, KG_BIG_ROWS * scale, 1);
		return kg_big_advance(scale);

	default:
		break;

	}

	rows = kg_glyph(c);
	if (rows)
		draw_bitmap(x, y, rows, scale,
			style == KG_BIG_HINT ? KG_SHADE_HINT : -1);

	/* A space, or an unsupported character, advances and draws
	 * nothing -- same as the original. */
	return kg_big_advance(scale);
}

int kg_big_puts(int x, int y, const char *s, int scale, kg_big_style_t style)
{
	return kg_big_puts_blank(x, y, s, scale, style, -1, KG_BIG_BLANK);
}

int kg_big_puts_blank(int x, int y, const char *s, int scale,
	kg_big_style_t style, int blank_idx, kg_big_style_t blank_style)
{
	int i;
	int x0 = x;

	if (!s) return 0;

	for (i = 0; s[i]; i++)
		x += kg_big_putc(x, y, s[i], scale,
			i == blank_idx ? blank_style : style);

	/* The advance past the LAST glyph includes a trailing gap that is
	 * not part of the string; subtract it so this agrees with
	 * kg_big_w(), which several callers use to centre. Two functions
	 * disagreeing about a string's width by one cell is the kind of
	 * thing that shows up as "the word drifts right as it gets
	 * longer". */
	return i ? (x - x0 - scale) : 0;
}

int kg_big_puts_centered(int y, const char *s, int scale,
	kg_big_style_t style)
{
	int x = (KG_W - kg_big_w(s, scale)) / 2;

	if (x < 0) x = 0;

	kg_big_puts(x, y, s, scale, style);

	return x;
}
