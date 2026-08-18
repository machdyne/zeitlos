/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zvt100.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zvt100.h"

static void mark_dirty(vt_screen_t *vt, int row) {
	if (row >= 0 && row < VT_ROWS) vt->dirty[row] = true;
}

// shifts every row up by one (row 0 is lost), clears the new bottom
// row to blank using the *current* SGR state (matches real terminal
// behavior -- newly-exposed space takes on whatever attributes are
// active when it's exposed, not whatever was there before).
static void scroll_up(vt_screen_t *vt) {

	for (int r = 0; r < VT_ROWS - 1; r++) {
		for (int c = 0; c < VT_COLS; c++)
			vt->cells[r][c] = vt->cells[r + 1][c];
	}

	for (int c = 0; c < VT_COLS; c++) {
		vt->cells[VT_ROWS - 1][c].ch = ' ';
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
		scroll_up(vt);
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
	vt->cells[vt->cursor_y][vt->cursor_x].reverse = vt->reverse;
	mark_dirty(vt, vt->cursor_y);

	vt->cursor_x++;

}

// blanks one cell using the *current* SGR state -- same reasoning as
// scroll_up()'s bottom row: erased space isn't "whatever was drawn
// before", it's blank-with-current-attributes.
static void erase_cell(vt_screen_t *vt, int row, int col) {
	vt->cells[row][col].ch = ' ';
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

// dispatches one completed "ESC [ params final" sequence. only the
// subset of CSI final bytes term actually needs is implemented --
// cursor movement, erase, and SGR (reverse only, see zvt100.h's file
// header comment on why that's the one attribute this framebuffer can
// represent). anything else is silently ignored rather than treated
// as an error -- an unimplemented or malformed sequence just has no
// visible effect, it doesn't corrupt parser state or crash.
static void csi_dispatch(vt_screen_t *vt, char final) {

	switch (final) {

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
			} else {
				for (int r = 0; r < VT_ROWS; r++)
					for (int c = 0; c < VT_COLS; c++) erase_cell(vt, r, c);
			}
			for (int r = 0; r < VT_ROWS; r++) mark_dirty(vt, r);
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

	switch (vt->pstate) {

		case VT_PSTATE_NORMAL:

			if (c == 0x1b) {
				vt->pstate = VT_PSTATE_ESC;
				return;
			}

			switch (c) {
				case '\r': vt->cursor_x = 0; return;
				case '\n': newline(vt); return;
				case '\b': if (vt->cursor_x > 0) vt->cursor_x--; return;
				case '\t': {
					int next_tab = ((vt->cursor_x / 8) + 1) * 8;
					if (next_tab >= VT_COLS) next_tab = VT_COLS - 1;
					vt->cursor_x = next_tab;
					return;
				}
				case 0x07: return;	// BEL -- no speaker, ignore
				default: break;
			}

			if (c >= 0x20 && c < 0x7f) put_char(vt, (char)c);
			// other C0 controls: silently ignored

			return;

		case VT_PSTATE_ESC:

			if (c == '[') {
				vt->pstate = VT_PSTATE_CSI;
				vt->csi_param_count = 0;
				for (int i = 0; i < VT_CSI_MAX_PARAMS; i++) {
					vt->csi_params[i] = 0;
					vt->csi_has_param[i] = false;
				}
				return;
			}

			// unsupported escape (not CSI) -- silently absorbed. real
			// VT100 has more of these (ESC c full reset, ESC 7/8
			// cursor save/restore) -- not implemented yet, worth
			// adding if term needs them.
			vt->pstate = VT_PSTATE_NORMAL;
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

			if (c == ';') {
				if (vt->csi_param_count < VT_CSI_MAX_PARAMS - 1)
					vt->csi_param_count++;
				return;
			}

			// any other byte terminates the sequence -- looser than
			// the spec (real CSI final bytes are 0x40-0x7e), but
			// harmless: csi_dispatch()'s default case silently ignores
			// anything it doesn't recognize, so a malformed sequence
			// just has no effect instead of corrupting parser state.
			csi_dispatch(vt, (char)c);
			vt->pstate = VT_PSTATE_NORMAL;
			return;

	}

}

void vt_feed(vt_screen_t *vt, const uint8_t *data, uint32_t len) {
	for (uint32_t i = 0; i < len; i++) vt_feed_byte(vt, data[i]);
}

void vt_init(vt_screen_t *vt) {

	for (int r = 0; r < VT_ROWS; r++) {
		for (int c = 0; c < VT_COLS; c++) {
			vt->cells[r][c].ch = ' ';
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
