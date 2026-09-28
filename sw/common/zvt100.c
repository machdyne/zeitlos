/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zvt100.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zvt100.h"
#include "zutf8.h"		// z_cp_width(), z_cp_to_l9()

static void mark_dirty(vt_screen_t *vt, int row) {
	if (row >= 0 && row < VT_ROWS) vt->dirty[row] = true;
}

// -- line drawing (zvt100.h, "line drawing, blocks and shades") --

// The arms of the eleven line glyphs, in code order (single; the
// double ones are the same eleven again).
static const uint8_t box_arms[11] = {
	VT_ARM_LEFT | VT_ARM_RIGHT,                             // H
	VT_ARM_UP | VT_ARM_DOWN,                                // V
	VT_ARM_DOWN | VT_ARM_RIGHT,                             // DR  ┌
	VT_ARM_DOWN | VT_ARM_LEFT,                              // DL  ┐
	VT_ARM_UP | VT_ARM_RIGHT,                               // UR  └
	VT_ARM_UP | VT_ARM_LEFT,                                // UL  ┘
	VT_ARM_UP | VT_ARM_DOWN | VT_ARM_RIGHT,                 // VR  ├
	VT_ARM_UP | VT_ARM_DOWN | VT_ARM_LEFT,                  // VL  ┤
	VT_ARM_DOWN | VT_ARM_LEFT | VT_ARM_RIGHT,               // DH  ┬
	VT_ARM_UP | VT_ARM_LEFT | VT_ARM_RIGHT,                 // UH  ┴
	VT_ARM_UP | VT_ARM_DOWN | VT_ARM_LEFT | VT_ARM_RIGHT,   // VH  ┼
};

// Each code's own character, for vt_glyph_cp(): what a copy of it pastes.
static const uint16_t box_cp[VT_BOX_LAST - VT_BOX_FIRST + 1] = {
	0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
	0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2560, 0x2563, 0x2566, 0x2569, 0x256C,
	0x2588, 0x2580, 0x2584, 0x258C, 0x2590,
	0x2591, 0x2592, 0x2593,
};

static uint8_t box_by_arms(uint8_t arms, bool dbl) {
	// A lone half-line (╴╵╶╷) is drawn as the whole line it is half of.
	if (arms == VT_ARM_LEFT || arms == VT_ARM_RIGHT) arms = VT_ARM_LEFT | VT_ARM_RIGHT;
	if (arms == VT_ARM_UP || arms == VT_ARM_DOWN) arms = VT_ARM_UP | VT_ARM_DOWN;
	for (int i = 0; i < 11; i++)
		if (box_arms[i] == arms) return (uint8_t)((dbl ? VT_BOX_H2 : VT_BOX_H) + i);
	return 0;
}

uint8_t vt_box_from_cp(uint32_t cp) {

	if (cp < 0x2500 || cp > 0x2593) return 0;

	// U+2500-U+254B: light and heavy lines, both drawn light. The
	// block of corners and tees comes in runs of variants of one shape
	// (heavy arms, mixed weights), so a table of run starts is enough.
	if (cp <= 0x254F) {
		static const struct { uint16_t first; uint8_t arms; } runs[] = {
			{ 0x2500, VT_ARM_LEFT | VT_ARM_RIGHT },     // ─ ━
			{ 0x2502, VT_ARM_UP | VT_ARM_DOWN },        // │ ┃
			{ 0x2504, VT_ARM_LEFT | VT_ARM_RIGHT },     // ┄ ┅   dashes
			{ 0x2506, VT_ARM_UP | VT_ARM_DOWN },        // ┆ ┇
			{ 0x2508, VT_ARM_LEFT | VT_ARM_RIGHT },     // ┈ ┉
			{ 0x250A, VT_ARM_UP | VT_ARM_DOWN },        // ┊ ┋
			{ 0x250C, VT_ARM_DOWN | VT_ARM_RIGHT },     // ┌ x4
			{ 0x2510, VT_ARM_DOWN | VT_ARM_LEFT },      // ┐ x4
			{ 0x2514, VT_ARM_UP | VT_ARM_RIGHT },       // └ x4
			{ 0x2518, VT_ARM_UP | VT_ARM_LEFT },        // ┘ x4
			{ 0x251C, VT_ARM_UP | VT_ARM_DOWN | VT_ARM_RIGHT },     // ├ x8
			{ 0x2524, VT_ARM_UP | VT_ARM_DOWN | VT_ARM_LEFT },      // ┤ x8
			{ 0x252C, VT_ARM_DOWN | VT_ARM_LEFT | VT_ARM_RIGHT },   // ┬ x8
			{ 0x2534, VT_ARM_UP | VT_ARM_LEFT | VT_ARM_RIGHT },     // ┴ x8
			{ 0x253C, VT_ARM_UP | VT_ARM_DOWN | VT_ARM_LEFT | VT_ARM_RIGHT },  // ┼ x16
			{ 0x254C, VT_ARM_LEFT | VT_ARM_RIGHT },     // ╌ ╍
			{ 0x254E, VT_ARM_UP | VT_ARM_DOWN },        // ╎ ╏
			{ 0x2550, 0 },
		};
		for (int i = 0; runs[i].arms; i++)
			if (cp >= runs[i].first && cp < runs[i + 1].first)
				return box_by_arms(runs[i].arms, false);
		return 0;
	}

	// U+2550-U+256C: double lines. After ═ and ║ they come in threes
	// -- single/double, double/single, double -- and a mixed one is
	// drawn as the double: the double half is the one a box is built
	// of, in the CP437 art these come from.
	if (cp == 0x2550) return VT_BOX_H2;
	if (cp == 0x2551) return VT_BOX_V2;
	if (cp <= 0x256C) {
		static const uint8_t shapes[9] = {
			VT_ARM_DOWN | VT_ARM_RIGHT, VT_ARM_DOWN | VT_ARM_LEFT,
			VT_ARM_UP | VT_ARM_RIGHT, VT_ARM_UP | VT_ARM_LEFT,
			VT_ARM_UP | VT_ARM_DOWN | VT_ARM_RIGHT, VT_ARM_UP | VT_ARM_DOWN | VT_ARM_LEFT,
			VT_ARM_DOWN | VT_ARM_LEFT | VT_ARM_RIGHT, VT_ARM_UP | VT_ARM_LEFT | VT_ARM_RIGHT,
			VT_ARM_UP | VT_ARM_DOWN | VT_ARM_LEFT | VT_ARM_RIGHT,
		};
		return box_by_arms(shapes[(cp - 0x2552) / 3], true);
	}

	switch (cp) {
	case 0x256D: return VT_BOX_DR;          // ╭ rounded corners, drawn square
	case 0x256E: return VT_BOX_DL;          // ╮
	case 0x256F: return VT_BOX_UL;          // ╯
	case 0x2570: return VT_BOX_UR;          // ╰
	case 0x2574: case 0x2576: case 0x2578: case 0x257A: case 0x257C: case 0x257E:
		return VT_BOX_H;                    // half lines, light and heavy
	case 0x2575: case 0x2577: case 0x2579: case 0x257B: case 0x257D: case 0x257F:
		return VT_BOX_V;
	case 0x2580: return VT_BOX_UPPER;
	case 0x2584: return VT_BOX_LOWER;
	case 0x2588: return VT_BOX_FULL;
	case 0x258C: return VT_BOX_LEFT;
	case 0x2590: return VT_BOX_RIGHT;
	case 0x2591: return VT_BOX_SHADE1;
	case 0x2592: return VT_BOX_SHADE2;
	case 0x2593: return VT_BOX_SHADE3;
	}
	return 0;       // diagonals, partial blocks: the missing-glyph box
}

uint32_t vt_glyph_cp(uint8_t ch) {
	if (vt_is_box(ch)) return box_cp[ch - VT_BOX_FIRST];
	if (ch == (uint8_t)VT_CH_WIDE_RIGHT) return 0;
	if (ch == 0x7f) return 0xFFFD;
	if (ch < 0x20 || (ch >= 0x80 && ch < 0xA0)) return ' ';
	return z_l9_to_cp(ch);
}

// DEC special graphics (ESC ( 0), 0x5F-0x7E: what VT100 programs --
// curses with TERM=vt100 -- draw boxes with. The line pieces map to the
// codes above; the few that Latin-9 has (degree, plus-minus, pound,
// middle dot) to those; the scan lines to the horizontal line, the
// nearest thing; the rest to the missing-glyph box.
static uint8_t dec_graphic(uint8_t c) {
	static const uint8_t map[32] = {
		/* _ */ ' ',
		/* ` */ 0x7f,         /* a */ VT_BOX_SHADE2, /* b */ 0x7f, /* c */ 0x7f,
		/* d */ 0x7f,         /* e */ 0x7f, /* f */ 0xB0, /* g */ 0xB1,
		/* h */ 0x7f,         /* i */ 0x7f, /* j */ VT_BOX_UL, /* k */ VT_BOX_DL,
		/* l */ VT_BOX_DR,    /* m */ VT_BOX_UR, /* n */ VT_BOX_VH, /* o */ VT_BOX_H,
		/* p */ VT_BOX_H,     /* q */ VT_BOX_H, /* r */ VT_BOX_H, /* s */ VT_BOX_H,
		/* t */ VT_BOX_VR,    /* u */ VT_BOX_VL, /* v */ VT_BOX_UH, /* w */ VT_BOX_DH,
		/* x */ VT_BOX_V,     /* y */ 0x7f, /* z */ 0x7f, /* { */ 0x7f,
		/* | */ 0x7f,         /* } */ 0xA3, /* ~ */ 0xB7,
	};
	return (c >= 0x5F && c <= 0x7E) ? map[c - 0x5F] : c;
}

/* The pictures. Lines through the middle of the cell, reaching every
 * edge they leave by, so neighbouring cells join; a double line is two,
 * a pixel either side of that middle.
 *
 * A double glyph is drawn as the OUTLINE of a thick one. Each arm is a
 * band three pixels wide; the union of the bands, less every pixel whose
 * eight neighbours are all in it, leaves exactly two lines per arm, and
 * they meet properly at every corner and tee -- ╔'s inner line turns at
 * the inner corner, ╬ breaks into four corners -- without a table of
 * special cases. A neighbour outside the cell counts as whatever the
 * nearest pixel inside it is, so a band leaving by an edge is not
 * outlined along that edge. */
static bool in_band(int x, int y, int w, int h, int cx, int cy, uint8_t arms) {
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= w) x = w - 1;
	if (y >= h) y = h - 1;
	bool row = y >= cy - 1 && y <= cy + 1, col = x >= cx - 1 && x <= cx + 1;
	if ((arms & VT_ARM_LEFT) && row && x <= cx + 1) return true;
	if ((arms & VT_ARM_RIGHT) && row && x >= cx - 1) return true;
	if ((arms & VT_ARM_UP) && col && y <= cy + 1) return true;
	if ((arms & VT_ARM_DOWN) && col && y >= cy - 1) return true;
	return false;
}

void vt_box_glyph(uint8_t code, int w, int h, int xpar, int ypar, uint8_t *rows) {

	if (w > 8) w = 8;
	for (int y = 0; y < h; y++) rows[y] = 0;
	if (!vt_is_box(code) || w < 1 || h < 1) return;

	int cx = (w - 1) / 2, cy = (h - 1) / 2;
	#define PX(x, y) (rows[(y)] |= (uint8_t)(0x80u >> (x)))

	if (code <= VT_BOX_VH) {
		uint8_t arms = box_arms[code - VT_BOX_H];
		for (int x = 0; x < w; x++) {
			if ((arms & VT_ARM_LEFT) && x <= cx) PX(x, cy);
			if ((arms & VT_ARM_RIGHT) && x >= cx) PX(x, cy);
		}
		for (int y = 0; y < h; y++) {
			if ((arms & VT_ARM_UP) && y <= cy) PX(cx, y);
			if ((arms & VT_ARM_DOWN) && y >= cy) PX(cx, y);
		}
		return;
	}

	if (code <= VT_BOX_VH2) {
		uint8_t arms = box_arms[code - VT_BOX_H2];
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				if (!in_band(x, y, w, h, cx, cy, arms)) continue;
				bool inside = true;
				for (int dy = -1; dy <= 1 && inside; dy++)
					for (int dx = -1; dx <= 1; dx++)
						if (!in_band(x + dx, y + dy, w, h, cx, cy, arms)) { inside = false; break; }
				if (!inside) PX(x, y);
			}
		return;
	}

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int X = x + xpar, Y = y + ypar;     // the dithers follow the screen, not the cell
			bool on = false;
			switch (code) {
			case VT_BOX_FULL:   on = true; break;
			case VT_BOX_UPPER:  on = y < h / 2; break;
			case VT_BOX_LOWER:  on = y >= h / 2; break;
			case VT_BOX_LEFT:   on = x < w / 2; break;
			case VT_BOX_RIGHT:  on = x >= w / 2; break;
			case VT_BOX_SHADE1: on = !(X & 1) && !(Y & 1); break;       // a quarter
			case VT_BOX_SHADE2: on = !((X + Y) & 1); break;              // a half
			case VT_BOX_SHADE3: on = (X & 1) == 0 || (Y & 1) == 0; break; // three quarters
			}
			if (on) PX(x, y);
		}
	#undef PX
}

// shifts every row up by one (row 0 is lost), clears the new bottom
// row to blank using the *current* SGR state (matches real terminal
// behavior -- newly-exposed space takes on whatever attributes are
// active when it's exposed, not whatever was there before).
/* Counts scrolls so a renderer can move pixels instead of redrawing
 * them. See vt_take_scrolls() in zvt100.h.
 *
 * A COUNT rather than a flag, because several lines can scroll between
 * two renders -- a burst of output, a paste -- and the renderer needs
 * the total to know how far to shift. */
/* Copies screen row 0 into the history ring, evicting the oldest line
 * once the ring is full. Called only on the linefeed path -- see
 * scroll_up()'s `save` and "scrollback history" in zvt100.h. */
static void history_push_top(vt_screen_t *vt) {

	if (!vt->hist_buf || !vt->hist_cap) return;

	uint8_t *line = vt->hist_buf + (uint32_t)vt->hist_head * VT_HIST_LINE_BYTES;
	uint8_t *rev = line + VT_COLS;
	uint8_t *wide = rev + (VT_COLS + 7) / 8;
	for (int c = 0; c < (VT_COLS + 7) / 8; c++) rev[c] = wide[c] = 0;
	for (int c = 0; c < VT_COLS; c++) {
		line[c] = (uint8_t)vt->cells[0][c].ch;
		if (vt->cells[0][c].reverse) rev[c / 8] |= (uint8_t)(1u << (c % 8));
	}
	// A wide character with its codepoint keeps it: the pair's two
	// glyph bytes carry it, and the wide bitmap says so.
	for (int c = 0; c + 1 < VT_COLS; c++) {
		uint16_t cp = vt->cells[0][c].cp;
		if (!cp || vt->cells[0][c + 1].ch != VT_CH_WIDE_RIGHT) continue;
		line[c] = (uint8_t)(cp >> 8);
		line[c + 1] = (uint8_t)cp;
		wide[c / 8] |= (uint8_t)(1u << (c % 8));
	}

	vt->hist_head++;
	if (vt->hist_head >= vt->hist_cap) vt->hist_head = 0;
	if (vt->hist_count < vt->hist_cap) vt->hist_count++;
	vt->hist_pushed++;

}

/* `save`: whether the row leaving the top belongs in history. True for
 * a linefeed at the bottom, false for DL at row 0 -- see zvt100.h. */
static void scroll_up(vt_screen_t *vt, bool save) {

	if (vt->scrolls < 0xFFFF) vt->scrolls++;

	if (save) history_push_top(vt);


	for (int r = 0; r < VT_ROWS - 1; r++) {
		for (int c = 0; c < VT_COLS; c++)
			vt->cells[r][c] = vt->cells[r + 1][c];
	}

	for (int c = 0; c < VT_COLS; c++) {
		vt->cells[VT_ROWS - 1][c].ch = ' ';
		vt->cells[VT_ROWS - 1][c].cp = 0;
		vt->cells[VT_ROWS - 1][c].reverse = vt->reverse;
	}

	for (int r = 0; r < VT_ROWS; r++) mark_dirty(vt, r);

}

// moves the cursor down one row, scrolling if it was already on the
// bottom row -- does NOT touch cursor_x (true VT100 line-feed
// semantics; CR is what resets the column, see vt_feed_byte()).
static void newline(vt_screen_t *vt) {
	vt->cursor_y++;
	if (vt->cursor_y >= VT_ROWS) {
		vt->cursor_y = VT_ROWS - 1;
		scroll_up(vt, true);
	}
}

// writes one printable character at the cursor and advances it.
// implements "deferred wrap": if the *previous* call left the cursor
// sitting one column past the end of the line (cursor_x == VT_COLS),
// the wrap to the next line happens here, right before writing this
// character, not immediately after that last write. this is what
// real terminals do, and it matters: wrapping immediately after the
// last column would insert a spurious blank line for content that's
// exactly VT_COLS wide and ends in something other than a newline.
static void put_char(vt_screen_t *vt, char c) {

	if (vt->cursor_x >= VT_COLS) {
		vt->cursor_x = 0;
		newline(vt);
	}

	vt->cells[vt->cursor_y][vt->cursor_x].ch = c;
	vt->cells[vt->cursor_y][vt->cursor_x].cp = 0;
	vt->cells[vt->cursor_y][vt->cursor_x].reverse = vt->reverse;
	mark_dirty(vt, vt->cursor_y);

	vt->cursor_x++;

}

// Writes one decoded character. The cell gets the character's ISO
// 8859-15 byte -- the byte the hardware font draws it with -- or the
// missing-glyph box (0x7f) when Latin-9 has none; see "characters" in
// zvt100.h. Width follows z_cp_width(), which is what the program at
// the other end uses to decide where its own cursor is.
static void put_cp(vt_screen_t *vt, uint32_t cp) {

	int w = z_cp_width(cp);
	if (w == 0) return;					// a combining mark: no cell of its own

	// C1 controls (U+0080-U+009F) are not interpreted, like the C0
	// controls this parser does not handle.
	if (cp >= 0x80 && cp < 0xA0) return;

	// Line drawing, blocks, shades: a code of their own (zvt100.h).
	uint8_t box = vt_box_from_cp(cp);
	if (box) { put_char(vt, (char)box); return; }

	uint8_t b = z_cp_to_l9(cp);
	if (b < 0x20) b = 0x7f;				// none (0), or not a glyph: the box

	if (w == 2) {
		// Both halves on one line: a wide character that would start
		// in the last column wraps first and leaves that column blank,
		// as xterm does.
		if (vt->cursor_x == VT_COLS - 1) put_char(vt, ' ');
		put_char(vt, (char)b);
		// put_char() has just moved past the left cell. Keep which
		// character this is, for drawing it from the Japanese font and
		// for copying it as itself -- see vt_cell_t.cp.
		if (cp <= 0xFFFF)
			vt->cells[vt->cursor_y][vt->cursor_x - 1].cp = (uint16_t)cp;
		put_char(vt, VT_CH_WIDE_RIGHT);
		return;
	}

	put_char(vt, (char)b);

}

// blanks one cell using the *current* SGR state -- same reasoning as
// scroll_up()'s bottom row: erased space isn't "whatever was drawn
// before", it's blank-with-current-attributes.
static void erase_cell(vt_screen_t *vt, int row, int col) {
	vt->cells[row][col].ch = ' ';
	vt->cells[row][col].cp = 0;
	vt->cells[row][col].reverse = vt->reverse;
}

// returns CSI parameter `idx`'s value, or `default_val` if that
// parameter was never given a digit (distinguishing "absent" from an
// explicit "0" -- CSI's own default isn't always 0; CUP's is 1, for
// instance).
static int csi_param(const vt_screen_t *vt, int idx, int default_val) {
	if (idx < 0 || idx >= VT_CSI_MAX_PARAMS) return default_val;
	if (!vt->csi_has_param[idx]) return default_val;
	return vt->csi_params[idx];
}

// -- the rest of VT100 and ANSI that a BBS or a curses program uses --

static void reply(vt_screen_t *vt, const char *s) {
	uint32_t n = 0;
	while (s[n]) n++;
	if (vt->reply_len + n > VT_REPLY_MAX) return;     // nobody is reading: drop, never overflow
	for (uint32_t i = 0; i < n; i++) vt->reply[vt->reply_len++] = (uint8_t)s[i];
}

static void reply_num(char *p, int v) {
	char t[8];
	int n = 0;
	if (v <= 0) { *p++ = '0'; *p = 0; return; }
	while (v && n < 7) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n) *p++ = t[--n];
	*p = 0;
}

static void save_cursor(vt_screen_t *vt) {
	vt->saved.x = vt->cursor_x;
	vt->saved.y = vt->cursor_y;
	vt->saved.reverse = vt->reverse;
	vt->saved.g0 = vt->g0;
	vt->saved.g1 = vt->g1;
	vt->saved.shift_out = vt->shift_out;
}

// Nothing saved restores the defaults, as xterm does: vt_init() leaves
// `saved` as the home position with nothing set.
static void restore_cursor(vt_screen_t *vt) {
	vt->cursor_x = vt->saved.x;
	vt->cursor_y = vt->saved.y;
	vt->reverse = vt->saved.reverse;
	vt->g0 = vt->saved.g0;
	vt->g1 = vt->saved.g1;
	vt->shift_out = vt->saved.shift_out;
	if (vt->cursor_x > VT_COLS) vt->cursor_x = VT_COLS;
	if (vt->cursor_y >= VT_ROWS) vt->cursor_y = VT_ROWS - 1;
}

// Rows from `top` down move down by n; the n rows at `top` go blank.
// IL at the cursor, and RI / SD at the top of the screen.
static void insert_rows(vt_screen_t *vt, int top, int n) {
	if (n > VT_ROWS - top) n = VT_ROWS - top;
	for (int r = VT_ROWS - 1; r >= top + n; r--)
		for (int c = 0; c < VT_COLS; c++)
			vt->cells[r][c] = vt->cells[r - n][c];
	for (int r = top; r < top + n; r++)
		for (int c = 0; c < VT_COLS; c++)
			erase_cell(vt, r, c);
	for (int r = top; r < VT_ROWS; r++) mark_dirty(vt, r);
}

// The C0 controls. Also inside an escape sequence, where VT100 obeys
// them without leaving it -- a CR in the middle of a CSI is a CR.
static void control(vt_screen_t *vt, uint8_t c) {
	switch (c) {
	case '\r': vt->cursor_x = 0; return;
	case '\n': case 0x0B: case 0x0C: newline(vt); return;   // LF, VT, FF
	case '\b': if (vt->cursor_x > 0) vt->cursor_x--; if (vt->cursor_x >= VT_COLS) vt->cursor_x = VT_COLS - 1; return;
	case '\t': {
		int next_tab = ((vt->cursor_x / 8) + 1) * 8;
		if (next_tab >= VT_COLS) next_tab = VT_COLS - 1;
		vt->cursor_x = next_tab;
		return;
	}
	case 0x0E: vt->shift_out = true; return;      // SO: G1
	case 0x0F: vt->shift_out = false; return;     // SI: G0
	default: return;                              // BEL (no speaker) and the rest
	}
}

// ESC c: everything back to the start, except the scrollback, which is
// the user's rather than the program's.
static void full_reset(vt_screen_t *vt) {
	for (int r = 0; r < VT_ROWS; r++)
		for (int c = 0; c < VT_COLS; c++) {
			vt->cells[r][c].ch = ' ';
			vt->cells[r][c].cp = 0;
			vt->cells[r][c].reverse = false;
		}
	vt->cursor_x = vt->cursor_y = 0;
	vt->reverse = false;
	vt->g0 = vt->g1 = VT_CS_ASCII;
	vt->shift_out = false;
	vt->cursor_hidden = false;
	vt->saved.x = vt->saved.y = 0;
	vt->saved.reverse = vt->saved.shift_out = false;
	vt->saved.g0 = vt->saved.g1 = VT_CS_ASCII;
	vt_mark_all_dirty(vt);
}

// dispatches one completed "ESC [ params final" sequence. only the
// subset of CSI final bytes term actually needs is implemented --
// cursor movement, erase, and SGR (reverse only, see zvt100.h's file
// header comment on why that's the one attribute this framebuffer can
// represent). anything else is silently ignored rather than treated
// as an error -- an unimplemented or malformed sequence just has no
// visible effect, it doesn't corrupt parser state or crash.
static void csi_dispatch(vt_screen_t *vt, char final) {

	// "CSI ? ..." -- DEC private modes. Only the cursor's visibility
	// means anything here; every other one is absorbed, and none may
	// fall through to the ANSI meaning of the same final byte (CSI ? 6 n
	// is not CSI 6 n).
	if (vt->csi_private) {
		if (vt->csi_private == '?' && (final == 'h' || final == 'l') && !vt->csi_inter) {
			int n = vt->csi_param_count + 1;
			if (n > VT_CSI_MAX_PARAMS) n = VT_CSI_MAX_PARAMS;
			for (int i = 0; i < n; i++)
				if (csi_param(vt, i, 0) == 25) {
					vt->cursor_hidden = (final == 'l');
					mark_dirty(vt, vt->cursor_y);
				}
		}
		return;
	}
	// An intermediate byte makes it a different command altogether
	// (CSI SP q, cursor style; CSI ! p, soft reset): absorbed.
	if (vt->csi_inter) return;

	switch (final) {

		case '@': { // ICH -- insert blank characters at the cursor
			int n = csi_param(vt, 0, 1), x = vt->cursor_x < VT_COLS ? vt->cursor_x : VT_COLS - 1;
			if (n < 1) n = 1;
			if (n > VT_COLS - x) n = VT_COLS - x;
			for (int c = VT_COLS - 1; c >= x + n; c--) vt->cells[vt->cursor_y][c] = vt->cells[vt->cursor_y][c - n];
			for (int c = x; c < x + n; c++) erase_cell(vt, vt->cursor_y, c);
			mark_dirty(vt, vt->cursor_y);
			break;
		}

		case 'P': { // DCH -- delete characters at the cursor; the line closes up
			int n = csi_param(vt, 0, 1), x = vt->cursor_x < VT_COLS ? vt->cursor_x : VT_COLS - 1;
			if (n < 1) n = 1;
			if (n > VT_COLS - x) n = VT_COLS - x;
			for (int c = x; c < VT_COLS - n; c++) vt->cells[vt->cursor_y][c] = vt->cells[vt->cursor_y][c + n];
			for (int c = VT_COLS - n; c < VT_COLS; c++) erase_cell(vt, vt->cursor_y, c);
			mark_dirty(vt, vt->cursor_y);
			break;
		}

		case 'X': { // ECH -- erase characters from the cursor, nothing moves
			int n = csi_param(vt, 0, 1), x = vt->cursor_x < VT_COLS ? vt->cursor_x : VT_COLS - 1;
			if (n < 1) n = 1;
			for (int c = x; c < x + n && c < VT_COLS; c++) erase_cell(vt, vt->cursor_y, c);
			mark_dirty(vt, vt->cursor_y);
			break;
		}

		case 'G': case '`': { // CHA, HPA -- to a column, 1-indexed
			int col = csi_param(vt, 0, 1);
			if (col < 1) col = 1;
			vt->cursor_x = (col > VT_COLS) ? VT_COLS - 1 : col - 1;
			break;
		}

		case 'd': { // VPA -- to a row, 1-indexed
			int row = csi_param(vt, 0, 1);
			if (row < 1) row = 1;
			vt->cursor_y = (row > VT_ROWS) ? VT_ROWS - 1 : row - 1;
			break;
		}

		case 'E': case 'F': { // CNL, CPL -- down or up n lines, to column 1
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			vt->cursor_y += (final == 'E') ? n : -n;
			if (vt->cursor_y < 0) vt->cursor_y = 0;
			if (vt->cursor_y >= VT_ROWS) vt->cursor_y = VT_ROWS - 1;
			vt->cursor_x = 0;
			break;
		}

		case 'S': { // SU -- scroll up n; like DL at the top, not into history
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			for (int i = 0; i < n && i < VT_ROWS; i++) scroll_up(vt, false);
			break;
		}

		case 'T': { // SD -- scroll down n: blank rows arrive at the top
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			insert_rows(vt, 0, n);
			break;
		}

		case 's': save_cursor(vt); break;       // SCOSC (DECSLRM when margins are on -- they never are here)
		case 'u': restore_cursor(vt); break;    // SCORC

		case 'n': { // DSR -- device status report
			int q = csi_param(vt, 0, 0);
			if (q == 5) reply(vt, "\x1b[0n");       // "terminal OK"
			else if (q == 6) {                      // "cursor at row;col", 1-indexed
				char b[20], num[8];
				int i = 0;
				b[i++] = 0x1b; b[i++] = '[';
				reply_num(num, vt->cursor_y + 1);
				for (char *p = num; *p; p++) b[i++] = *p;
				b[i++] = ';';
				// Past the last column (a deferred wrap) is reported as
				// the last column, as xterm does.
				reply_num(num, (vt->cursor_x >= VT_COLS ? VT_COLS - 1 : vt->cursor_x) + 1);
				for (char *p = num; *p; p++) b[i++] = *p;
				b[i++] = 'R';
				b[i] = 0;
				reply(vt, b);
			}
			break;
		}

		case 'c': // DA -- "what are you": a VT100 with no options
			if (csi_param(vt, 0, 0) == 0) reply(vt, "\x1b[?1;0c");
			break;

		case 'A': { // CUU -- cursor up
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			vt->cursor_y -= n;
			if (vt->cursor_y < 0) vt->cursor_y = 0;
			break;
		}

		case 'B': { // CUD -- cursor down
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			vt->cursor_y += n;
			if (vt->cursor_y >= VT_ROWS) vt->cursor_y = VT_ROWS - 1;
			break;
		}

		case 'C': { // CUF -- cursor forward
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			vt->cursor_x += n;
			if (vt->cursor_x >= VT_COLS) vt->cursor_x = VT_COLS - 1;
			break;
		}

		case 'D': { // CUB -- cursor back
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			vt->cursor_x -= n;
			if (vt->cursor_x < 0) vt->cursor_x = 0;
			break;
		}

		case 'H': case 'f': { // CUP -- cursor position, 1-indexed "row;col", default 1;1
			int row = csi_param(vt, 0, 1);
			int col = csi_param(vt, 1, 1);
			if (row < 1) row = 1;
			if (col < 1) col = 1;
			vt->cursor_y = (row - 1 >= VT_ROWS) ? VT_ROWS - 1 : row - 1;
			vt->cursor_x = (col - 1 >= VT_COLS) ? VT_COLS - 1 : col - 1;
			break;
		}

		case 'J': { // ED -- erase in display
			int mode = csi_param(vt, 0, 0);
			if (mode == 0) {
				for (int c = vt->cursor_x; c < VT_COLS; c++) erase_cell(vt, vt->cursor_y, c);
				for (int r = vt->cursor_y + 1; r < VT_ROWS; r++)
					for (int c = 0; c < VT_COLS; c++) erase_cell(vt, r, c);
			} else if (mode == 1) {
				for (int r = 0; r < vt->cursor_y; r++)
					for (int c = 0; c < VT_COLS; c++) erase_cell(vt, r, c);
				for (int c = 0; c <= vt->cursor_x && c < VT_COLS; c++) erase_cell(vt, vt->cursor_y, c);
			} else if (mode == 3) {
				/* ED 3 -- erase SAVED lines, the screen untouched.
				 * xterm's meaning; it used to fall into the branch
				 * below and clear the screen, but nothing in this
				 * tree sends it and every terminal that knows it
				 * leaves the screen alone. */
				vt_history_clear(vt);
				break;
			} else {
				for (int r = 0; r < VT_ROWS; r++)
					for (int c = 0; c < VT_COLS; c++) erase_cell(vt, r, c);
			}
			for (int r = 0; r < VT_ROWS; r++) mark_dirty(vt, r);
			break;
		}

		case 'L': { // IL -- insert lines at the cursor
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			/* Rows below the cursor move DOWN by n; the n rows at the
			 * cursor become blank; whatever falls off the bottom is
			 * gone. The scrolling region is the whole screen -- DECSTBM
			 * is not implemented, and nothing here sets one. */
			insert_rows(vt, vt->cursor_y, n);
			break;
		}

		case 'M': { // DL -- delete lines at the cursor
			int n = csi_param(vt, 0, 1);
			if (n < 1) n = 1;
			/* The mirror of IL: rows below move UP by n and the bottom
			 * n become blank.
			 *
			 * These two are how a full-screen editor scrolls. nextvi's
			 * term_room() emits ESC[nL and ESC[nM and nothing else --
			 * it never redraws the whole screen to scroll it. Without
			 * them, moving past the last line redrew only that line
			 * and the rest of the display stood still, which is
			 * exactly what a terminal that silently ignores the
			 * sequence looks like. */
			/* At the top of the screen this IS a scroll, so use
			 * scroll_up() -- which does the same shift and, crucially,
			 * increments the counter that lets the renderer move
			 * PIXELS instead of redrawing rows (vt_take_scrolls()).
			 *
			 * That is the case an editor actually hits: moving past
			 * the last line scrolls the whole screen by one. Doing it
			 * the general way below would mark every row dirty and
			 * repaint the display, which on this machine is the
			 * difference between a scroll and a visible redraw. */
			if (vt->cursor_y == 0) {
				/* false: DL is how an editor scrolls, and its
				 * screens are not scrollback -- see zvt100.h. */
				for (int i = 0; i < n && i < VT_ROWS; i++) scroll_up(vt, false);
				break;
			}

			for (int r = vt->cursor_y; r < VT_ROWS - n; r++)
				for (int c = 0; c < VT_COLS; c++)
					vt->cells[r][c] = vt->cells[r + n][c];
			for (int r = VT_ROWS - n > vt->cursor_y ? VT_ROWS - n : vt->cursor_y;
				 r < VT_ROWS; r++)
				for (int c = 0; c < VT_COLS; c++)
					erase_cell(vt, r, c);
			for (int r = vt->cursor_y; r < VT_ROWS; r++) mark_dirty(vt, r);
			break;
		}

		case 'K': { // EL -- erase in line
			int mode = csi_param(vt, 0, 0);
			if (mode == 0) {
				for (int c = vt->cursor_x; c < VT_COLS; c++) erase_cell(vt, vt->cursor_y, c);
			} else if (mode == 1) {
				for (int c = 0; c <= vt->cursor_x && c < VT_COLS; c++) erase_cell(vt, vt->cursor_y, c);
			} else {
				for (int c = 0; c < VT_COLS; c++) erase_cell(vt, vt->cursor_y, c);
			}
			mark_dirty(vt, vt->cursor_y);
			break;
		}

		case 'm': { // SGR
			int n = vt->csi_param_count + 1;
			if (n > VT_CSI_MAX_PARAMS) n = VT_CSI_MAX_PARAMS;
			for (int i = 0; i < n; i++) {
				int p = csi_param(vt, i, 0);	// bare ESC[m (no digits
												// at all) behaves as
												// ESC[0m -- csi_param()
												// returns the 0 default
												// for the untouched slot
				switch (p) {
					case 0:  vt->reverse = false; break;	// reset
					case 7:  vt->reverse = true;  break;	// reverse on
					case 27: vt->reverse = false; break;	// reverse off
					// bold(1)/underline(4)/blink(5)/etc. and their
					// resets are accepted (not treated as unknown/
					// erroneous) but currently no-ops -- see zvt100.h.
					default: break;
				}
			}
			break;
		}

		default:
			break;	// unrecognized final byte -- silently ignored

	}

}

void vt_feed_byte(vt_screen_t *vt, uint8_t c) {

	// -- UTF-8 --
	//
	// Decoded here, a byte at a time, because bytes arrive one at a
	// time and a character can be split across two reads. Only outside
	// escape sequences: those are ASCII. A malformed sequence becomes
	// one missing-glyph box, and the byte that ended it is then read
	// as itself -- so an ESC arriving mid-character still starts its
	// escape sequence.
	if (vt->pstate == VT_PSTATE_NORMAL) {

		if (vt->u8_need) {
			if ((c & 0xC0) == 0x80) {
				vt->u8_cp = (vt->u8_cp << 6) | (c & 0x3Fu);
				if (--vt->u8_need) return;
				uint32_t cp = vt->u8_cp;
				if (cp < vt->u8_min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
					cp = 0xFFFD;
				put_cp(vt, cp);
				return;
			}
			vt->u8_need = 0;
			put_cp(vt, 0xFFFD);
		}

		if (c >= 0x80) {
			if (c >= 0xC2 && c <= 0xDF)      { vt->u8_cp = c & 0x1Fu; vt->u8_need = 1; vt->u8_min = 0x80; }
			else if (c >= 0xE0 && c <= 0xEF) { vt->u8_cp = c & 0x0Fu; vt->u8_need = 2; vt->u8_min = 0x800; }
			else if (c >= 0xF0 && c <= 0xF4) { vt->u8_cp = c & 0x07u; vt->u8_need = 3; vt->u8_min = 0x10000; }
			else put_cp(vt, 0xFFFD);
			return;
		}

	}

	switch (vt->pstate) {

		case VT_PSTATE_NORMAL:

			if (c == 0x1b) {
				vt->pstate = VT_PSTATE_ESC;
				return;
			}
			if (c < 0x20) { control(vt, c); return; }

			if (c < 0x7f) {
				// The DEC special graphics set, when it is the one in
				// use (ESC ( 0, or ESC ) 0 and SO): line drawing.
				uint8_t set = vt->shift_out ? vt->g1 : vt->g0;
				put_char(vt, (char)(set == VT_CS_DEC ? dec_graphic(c) : c));
			}
			return;

		case VT_PSTATE_ESC:

			switch (c) {
			case '[':
				vt->pstate = VT_PSTATE_CSI;
				vt->csi_param_count = 0;
				vt->csi_private = 0;
				vt->csi_inter = 0;
				for (int i = 0; i < VT_CSI_MAX_PARAMS; i++) {
					vt->csi_params[i] = 0;
					vt->csi_has_param[i] = false;
				}
				return;
			case '(': case ')': case '#':
				vt->esc_cmd = (char)c;
				vt->pstate = VT_PSTATE_ESC_ARG;
				return;
			case 0x1b:
				return;                                 // ESC ESC: still in an escape
			case '7': save_cursor(vt); break;           // DECSC
			case '8': restore_cursor(vt); break;        // DECRC
			case 'c': full_reset(vt); break;            // RIS
			case 'D': newline(vt); break;               // IND: down, scrolling at the bottom
			case 'E': vt->cursor_x = 0; newline(vt); break;   // NEL
			case 'M':                                   // RI: up, scrolling DOWN at the top
				if (vt->cursor_y > 0) vt->cursor_y--;
				else insert_rows(vt, 0, 1);
				break;
			default:
				// A C0 control after ESC is obeyed and the escape
				// continues; anything else is an escape this parser
				// does not know (ESC = keypad, ESC H tab set ...).
				if (c < 0x20) { control(vt, c); return; }
				break;
			}
			vt->pstate = VT_PSTATE_NORMAL;
			return;

		case VT_PSTATE_ESC_ARG:
			if (c < 0x20 && c != 0x1b) { control(vt, c); return; }
			if (vt->esc_cmd == '(') vt->g0 = (c == '0') ? VT_CS_DEC : VT_CS_ASCII;
			else if (vt->esc_cmd == ')') vt->g1 = (c == '0') ? VT_CS_DEC : VT_CS_ASCII;
			// ESC # n (line sizes, the alignment test): absorbed
			vt->pstate = (c == 0x1b) ? VT_PSTATE_ESC : VT_PSTATE_NORMAL;
			return;

		case VT_PSTATE_CSI:

			if (c >= '0' && c <= '9') {
				if (vt->csi_param_count < VT_CSI_MAX_PARAMS) {
					vt->csi_params[vt->csi_param_count] =
						vt->csi_params[vt->csi_param_count] * 10 + (c - '0');
					vt->csi_has_param[vt->csi_param_count] = true;
				}
				return;
			}

			if (c == ';' || c == ':') {
				if (vt->csi_param_count < VT_CSI_MAX_PARAMS - 1)
					vt->csi_param_count++;
				return;
			}

			// '?', '>', '<', '=': a private sequence (CSI ? 25 l). It
			// used to END the sequence, as a final byte, and the rest
			// -- "25l" -- was printed: every program that hides its
			// cursor left that on the screen.
			if (c >= 0x3C && c <= 0x3F) {
				if (!vt->csi_private && !vt->csi_param_count && !vt->csi_has_param[0])
					vt->csi_private = (char)c;
				return;
			}

			if (c >= 0x20 && c <= 0x2F) { vt->csi_inter = (char)c; return; }

			if (c == 0x1b) { vt->pstate = VT_PSTATE_ESC; return; }   // a new escape: this one is abandoned
			if (c == 0x18 || c == 0x1a) { vt->pstate = VT_PSTATE_NORMAL; return; }  // CAN, SUB: cancelled
			if (c < 0x20) { control(vt, c); return; }  // obeyed, and the sequence goes on

			// the final byte (0x40-0x7E; anything else ends it too, harmlessly)
			vt->pstate = VT_PSTATE_NORMAL;
			csi_dispatch(vt, (char)c);
			return;

	}

}

uint32_t vt_take_reply(vt_screen_t *vt, uint8_t *out, uint32_t cap) {
	uint32_t n = vt->reply_len < cap ? vt->reply_len : cap;
	for (uint32_t i = 0; i < n; i++) out[i] = vt->reply[i];
	for (uint32_t i = n; i < vt->reply_len; i++) vt->reply[i - n] = vt->reply[i];
	vt->reply_len = (uint8_t)(vt->reply_len - n);
	return n;
}

void vt_feed(vt_screen_t *vt, const uint8_t *data, uint32_t len) {
	for (uint32_t i = 0; i < len; i++) vt_feed_byte(vt, data[i]);
}

void vt_init(vt_screen_t *vt) {

	vt->scrolls = 0;

	vt->u8_cp = 0;
	vt->u8_need = 0;
	vt->u8_min = 0;

	vt->hist_buf = 0;
	vt->hist_cap = 0;
	vt->hist_head = 0;
	vt->hist_count = 0;
	vt->hist_pushed = 0;

	for (int r = 0; r < VT_ROWS; r++) {
		for (int c = 0; c < VT_COLS; c++) {
			vt->cells[r][c].ch = ' ';
			vt->cells[r][c].cp = 0;
			vt->cells[r][c].reverse = false;
		}
		vt->dirty[r] = true;	// everything starts dirty, so a fresh
								// renderer draws the whole (blank)
								// screen once
	}

	vt->cursor_x = 0;
	vt->cursor_y = 0;
	vt->reverse = false;
	vt->pstate = VT_PSTATE_NORMAL;
	vt->csi_param_count = 0;
	vt->csi_private = 0;
	vt->csi_inter = 0;
	vt->esc_cmd = 0;
	vt->g0 = vt->g1 = VT_CS_ASCII;
	vt->shift_out = false;
	vt->cursor_hidden = false;
	vt->saved.x = vt->saved.y = 0;
	vt->saved.reverse = vt->saved.shift_out = false;
	vt->saved.g0 = vt->saved.g1 = VT_CS_ASCII;
	vt->reply_len = 0;

	for (int i = 0; i < VT_CSI_MAX_PARAMS; i++) {
		vt->csi_params[i] = 0;
		vt->csi_has_param[i] = false;
	}

}

bool vt_row_dirty(const vt_screen_t *vt, int row) {
	if (row < 0 || row >= VT_ROWS) return false;
	return vt->dirty[row];
}

void vt_clear_dirty(vt_screen_t *vt) {
	for (int r = 0; r < VT_ROWS; r++) vt->dirty[r] = false;
}

void vt_mark_all_dirty(vt_screen_t *vt) {
	for (int r = 0; r < VT_ROWS; r++) vt->dirty[r] = true;
}

void vt_history_attach(vt_screen_t *vt, uint8_t *buf, uint16_t cap_lines) {

	vt->hist_buf = (buf && cap_lines) ? buf : 0;
	vt->hist_cap = (buf && cap_lines) ? cap_lines : 0;
	vt->hist_head = 0;
	vt->hist_count = 0;

}

void vt_history_clear(vt_screen_t *vt) {

	/* The buffer itself is not wiped: nothing reads past hist_count,
	 * and on an 8KB-stack app a memset of 16KB of .bss per clear buys
	 * nothing. hist_pushed keeps counting -- see zvt100.h. */
	vt->hist_head = 0;
	vt->hist_count = 0;

}

/* Oldest retained line is at head - count, modulo the ring. */
static const uint8_t *history_line(const vt_screen_t *vt, int idx) {

	int slot = (int)vt->hist_head - (int)vt->hist_count + idx;
	while (slot < 0) slot += vt->hist_cap;
	while (slot >= vt->hist_cap) slot -= vt->hist_cap;

	return vt->hist_buf + (uint32_t)slot * VT_HIST_LINE_BYTES;

}

vt_packed_t vt_doc_cell(const vt_screen_t *vt, int doc, int col) {

	if (col < 0 || col >= VT_COLS || doc < 0) return VT_PACK(' ', false);

	if (doc < (int)vt->hist_count) {
		const uint8_t *line = history_line(vt, doc);
		const uint8_t *wide = line + VT_COLS + (VT_COLS + 7) / 8;
		bool rev = (line[VT_COLS + col / 8] >> (col % 8)) & 1;
		if ((wide[col / 8] >> (col % 8)) & 1)
			return VT_PACK_CP(0x7f, rev, ((uint16_t)line[col] << 8) | line[col + 1]);
		if (col > 0 && ((wide[(col - 1) / 8] >> ((col - 1) % 8)) & 1))
			return VT_PACK(VT_CH_WIDE_RIGHT, rev);
		return VT_PACK(line[col], rev);
	}

	int row = doc - (int)vt->hist_count;
	if (row >= VT_ROWS) return VT_PACK(' ', false);

	// The codepoint only while the pair is whole: a program that wrote
	// over the right half has left a lone box, not a wide character --
	// the same test history_push_top() applies.
	const vt_cell_t *cl = &vt->cells[row][col];
	uint16_t cp = (col + 1 < VT_COLS && vt->cells[row][col + 1].ch == VT_CH_WIDE_RIGHT) ?
		cl->cp : 0;
	return VT_PACK_CP(cl->ch, cl->reverse, cp);

}

bool vt_id_cell(const vt_screen_t *vt, uint32_t id, int col, vt_packed_t *out) {

	uint32_t first = vt->hist_pushed - vt->hist_count;

	*out = VT_PACK(' ', false);

	if (id < first) return false;
	if (id - first >= (uint32_t)vt->hist_count + VT_ROWS) return false;

	*out = vt_doc_cell(vt, (int)(id - first), col);
	return true;

}
