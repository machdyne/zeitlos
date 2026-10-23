/*
 * Zeitlos chess -- board and panel rendering.
 * See board_ui.h for why there is one renderer and not two.
 */

#include <string.h>
#include <stdio.h>

#include "../../common/zgfx.h"
#include "../../common/zfont.h"

#include "board_ui.h"
#include "pieces.h"
#include "ce_eval.h"
#include "../../common/games/zarcade.h"

#define FONT (&z_font_5x8)
#define FW 5
#define FH 8

/* z_fb_hw_blit_mem() is a bitstream feature, not a given -- zgfx.h
 * says to probe once at startup and keep a software path, the same
 * way sw/apps/draw and sw/apps/view do. Probed here rather than per
 * call because the answer cannot change while the app is running. */
static bool use_hw_blit;

/* -- the screen face (docs/chess_app.md, "In colour") --
 *
 * The same board, drawn through the arcade runtime on chess's 320x240
 * screen: every primitive goes to ar_*() instead of zgfx, and every
 * colour asked for is a ROLE (CB_C_*), drawn in one bit by the desktop
 * and in cb_palette by the screen. A piece on the screen is not the
 * one-bit tile: a white piece is its outline filled with ivory, a black
 * one its silhouette in black with its details picked out -- both
 * from the tile on a light square (the piece's lit pixels), and the
 * inside of its outline found once by a flood fill from the edge. */
static bool scr;
static uint32_t pc_ink[12][CP_TILE_H];     /* the tile's lit pixels */
static uint32_t pc_in[12][CP_TILE_H];      /* inside the outline */

const uint16_t cb_palette[16] = {
	0x223,      /* CB_C_BG     around the board */
	0xfff,      /* CB_C_INK    text */
	0xedb,      /* CB_C_LIGHT  a light square */
	0xa75,      /* CB_C_DARK   a dark square */
	0xffe,      /* CB_C_WPC    a white piece */
	0x111,      /* CB_C_BPC    a black piece */
	0x69e,      /* CB_C_LAST   the last move's squares */
	0xfc2,      /* CB_C_SEL    the piece picked up */
	0x3b4,      /* CB_C_HINT   where it can go */
	0xe33,      /* CB_C_CHECK  a king in check */
	0x778,      /* CB_C_RULE   rules, the caret's line */
	0x777,      /* CB_C_DETAIL a black piece's details */
	0x000, 0x000, 0x000, 0x000
};

static int mono_of(int role) {
	switch (role) {
	case CB_C_INK: case CB_C_LAST: case CB_C_SEL: case CB_C_HINT:
	case CB_C_CHECK: case CB_C_RULE:
		return 1;
	default:
		return 0;
	}
}

void cb_mono_inks(ar_ink_t ink[16]) {
	int i;
	for (i = 0; i < 16; i++) ink[i] = mono_of(i) ? ar_ink_white : ar_ink_black;
}

void cb_screen(bool on) { scr = on; }

/* The inside of each piece's outline. An outline can have gaps a
 * pixel or two wide (the hollow of the king's cross), so the flood
 * from the tile's edge runs between walls grown by a pixel, which
 * closes them; what it reaches then grows back by a pixel, to the
 * outline itself. The inside is what is neither reached nor lit. */
static void make_masks(void) {
	int p, x, y, k, changed;
	uint32_t m = (1u << CP_TILE_W) - 1u;
	for (p = 0; p < 12; p++) {
		uint32_t out[CP_TILE_H], wall[CP_TILE_H];
		const uint32_t *t = cp_tiles[CP_LIGHT][1 + p];
		for (y = 0; y < CP_TILE_H; y++) {
			uint32_t r = t[y] | (t[y] << 1) | (t[y] >> 1);
			pc_ink[p][y] = t[y];
			wall[y] = r;
			out[y] = 0;
		}
		for (y = 0; y < CP_TILE_H; y++) {
			uint32_t r = wall[y];
			if (y > 0) r |= t[y - 1] | (t[y - 1] << 1) | (t[y - 1] >> 1);
			if (y < CP_TILE_H - 1) r |= t[y + 1] | (t[y + 1] << 1) | (t[y + 1] >> 1);
			wall[y] = r & m;
		}
		/* the edge's open pixels are outside */
		for (y = 0; y < CP_TILE_H; y++)
			for (x = 0; x < CP_TILE_W; x++)
				if ((y == 0 || y == CP_TILE_H - 1 || x == 0 || x == CP_TILE_W - 1) &&
				    !((wall[y] >> x) & 1u))
					out[y] |= 1u << x;
		do {
			changed = 0;
			for (y = 0; y < CP_TILE_H; y++) {
				uint32_t g = out[y] | (out[y] << 1) | (out[y] >> 1);
				if (y > 0) g |= out[y - 1];
				if (y < CP_TILE_H - 1) g |= out[y + 1];
				g &= ~wall[y] & m;
				if (g != out[y]) { out[y] = g; changed = 1; }
			}
		} while (changed);
		/* back out to the outline: twice, a pixel at a time, from the
		 * reached and from beyond the tile, through open pixels */
		for (k = 0; k < 2; k++) {
			uint32_t prev = m;   /* the row above the tile: outside */
			for (y = 0; y < CP_TILE_H; y++) {
				uint32_t nx = y < CP_TILE_H - 1 ? out[y + 1] : m;
				uint32_t g = out[y] | prev | nx | 1u | (1u << (CP_TILE_W - 1));
				g |= (g << 1) | (g >> 1);
				prev = out[y];
				wall[y] = (out[y] | g) & ~t[y] & m;
			}
			for (y = 0; y < CP_TILE_H; y++) out[y] = wall[y];
		}
		for (y = 0; y < CP_TILE_H; y++)
			pc_in[p][y] = ~(out[y] | t[y]) & m;
	}
}

void cb_init(void) {
	use_hw_blit = z_fb_hw_blit_mem_available();
	make_masks();
}

/* -- layout --------------------------------------------------------- */

void cb_layout(cb_layout_t *L, int ox, int oy, int w, int h, bool flipped) {

	int block_w = CB_LABEL_W + CB_BOARD;
	int slack, top;

	memset(L, 0, sizeof(*L));

	L->ox = ox; L->oy = oy; L->w = w; L->h = h;
	L->flipped = flipped;

	L->clip.x0 = ox;
	L->clip.y0 = oy;
	L->clip.x1 = ox + w - 1;
	L->clip.y1 = oy + h - 1;

	/* The two text lines are anchored to the BOTTOM and the board to
	 * the top, with whatever is left over shared between them. That
	 * way the command line is in the same place whatever size the
	 * window is, which matters because it is the thing being typed
	 * into. */
	L->cmd_y = oy + h - CB_LINE_H;
	L->msg_y = L->cmd_y - CB_LINE_H;

	slack = h - (CB_BOARD + CB_LABEL_H + 2 * CB_LINE_H);
	if (slack < 0) slack = 0;
	top = slack / 2;
	if (top > 8) top = 8;

	L->board_x = ox + CB_LABEL_W;
	L->board_y = oy + top;
	L->rank_x = ox;
	L->file_y = L->board_y + CB_BOARD;

	L->panel_x = ox + block_w + CB_GAP;
	L->panel_y = L->board_y;
	L->panel_w = w - block_w - CB_GAP;
	L->panel_h = CB_BOARD + CB_LABEL_H;
	L->has_panel = L->panel_w >= CB_PANEL_MIN_W;

}

void cb_square_xy(const cb_layout_t *L, uint8_t sq, int *x, int *y) {

	int file = CE_FILE(sq), rank = CE_RANK(sq);

	/* Unflipped, White is at the bottom: file a is the leftmost
	 * column and rank 8 the topmost row. Flipping mirrors both. */
	if (L->flipped) { file = 7 - file; rank = 7 - rank; }

	*x = L->board_x + file * CB_SQ;
	*y = L->board_y + (7 - rank) * CB_SQ;

}

uint8_t cb_xy_square(const cb_layout_t *L, int x, int y) {

	int col = (x - L->board_x) / CB_SQ;
	int row = (y - L->board_y) / CB_SQ;
	int file, rank;

	/* The divisions above truncate towards zero, so a point one pixel
	 * LEFT of the board gives column 0 rather than -1 and would be
	 * read as a click on the a-file. Test the raw coordinates. */
	if (x < L->board_x || y < L->board_y) return CE_SQ_NONE;
	if (col < 0 || col > 7 || row < 0 || row > 7) return CE_SQ_NONE;

	file = col;
	rank = 7 - row;
	if (L->flipped) { file = 7 - file; rank = 7 - rank; }

	return CE_SQ(file, rank);

}

/* -- primitives ----------------------------------------------------- */

static void fill(const cb_layout_t *L, int x, int y, int w, int h, int color) {

	int x1 = x + w - 1, y1 = y + h - 1;

	if (scr) { ar_fill(x, y, w, h, color); return; }

	if (x < L->clip.x0) x = L->clip.x0;
	if (y < L->clip.y0) y = L->clip.y0;
	if (x1 > L->clip.x1) x1 = L->clip.x1;
	if (y1 > L->clip.y1) y1 = L->clip.y1;
	if (x1 < x || y1 < y) return;

	/* z_fb_hw_fill_rect() clamps to the SCREEN and clips to the
	 * visible region, but knows nothing about this app's content
	 * rectangle -- so the clamp above is not belt and braces, it is
	 * the only thing stopping a fill painting over the window frame
	 * or the desktop. */
	z_fb_hw_fill_rect(x, y, x1 - x + 1, y1 - y + 1, mono_of(color));

}

/* A one-pixel frame, inset by `inset` from the square's edge. */
static void frame(const cb_layout_t *L, int x, int y, int size, int inset,
	int thick, int color) {

	x += inset; y += inset;
	size -= 2 * inset;

	fill(L, x, y, size, thick, color);
	fill(L, x, y + size - thick, size, thick, color);
	fill(L, x, y, thick, size, color);
	fill(L, x + size - thick, y, thick, size, color);

}

static void blit_tile(const cb_layout_t *L, const uint32_t *tile,
	int dx, int dy) {

	int sx = 0, sy = 0, w = CP_TILE_W, h = CP_TILE_H;

	/* Clamp the destination to the content rectangle, moving the
	 * SOURCE origin in step. Clamping only the destination would
	 * slide the wrong part of the tile into view -- a half-visible
	 * square at the edge of a resized window would show the middle of
	 * a piece rather than its left half. */
	if (dx < L->clip.x0) { int d = L->clip.x0 - dx; sx += d; w -= d; dx += d; }
	if (dy < L->clip.y0) { int d = L->clip.y0 - dy; sy += d; h -= d; dy += d; }
	if (dx + w - 1 > L->clip.x1) w = L->clip.x1 - dx + 1;
	if (dy + h - 1 > L->clip.y1) h = L->clip.y1 - dy + 1;
	if (w <= 0 || h <= 0) return;

	if (!use_hw_blit) {
		/* Software fallback, for a bitstream whose blitter has no
		 * memory-source mode. Slow -- 576 read-modify-writes a
		 * square -- and correct, which is the right trade for a path
		 * that only runs on old gateware. */
		int px, py;
		for (py = 0; py < h; py++)
			for (px = 0; px < w; px++)
				z_fb_set_pixel(dx + px, dy + py,
					(int)((tile[sy + py] >> (sx + px)) & 1u), &L->clip);
		return;
	}

	/* -- the paint session --
	 *
	 * z_fb_hw_blit_mem() sets the blitter's CLIP bit but does not
	 * program the scissor, and the scissor is persistent hardware
	 * state -- so a blit issued outside a session inherits whatever
	 * rectangle the last unrelated operation left behind.
	 *
	 * The session walks the window's visible region, programs the
	 * scissor once per rectangle, and resets it at the end. It also
	 * gets the unrestricted case right, which is the part worth not
	 * hand-rolling: a visible count of 0 means UNRESTRICTED and not
	 * invisible (zgfx.h is explicit), and it yields a single
	 * unclipped pass rather than no passes at all.
	 *
	 * This is z_gfx_paint_* rather than zwin.c's z_win_paint_*
	 * because the same code draws a game-mode page, where there is no
	 * window to take a content rectangle from. */
	z_gfx_paint_begin();

	while (z_gfx_paint_next_rect(&L->clip))
		z_fb_hw_blit_mem(tile, CP_TILE_STRIDE, sx, sy, dx, dy, w, h);

	z_gfx_paint_end();

}

static void text(const cb_layout_t *L, int x, int y, const char *s) {
	/* on the screen over a strip already filled, as every caller does */
	if (scr) { ar_text(x, y, s, CB_C_INK, FONT, 1); return; }
	z_fb_draw_text(x, y, s, 1, FONT, &L->clip);
}

/* -- squares -------------------------------------------------------- */

static int tile_index(uint8_t pc) {
	if (pc == CE_EMPTY) return 0;
	return CE_COLOR(pc) == CE_WHITE ? CE_TYPE(pc) : 6 + CE_TYPE(pc);
}

static bool is_destination(const chess_game_t *g, uint8_t sq) {
	int i;
	for (i = 0; i < g->sel_n; i++)
		if (CE_MOVE_TO(g->sel_moves[i]) == sq) return true;
	return false;
}

void cb_draw_square(const cb_layout_t *L, chess_game_t *g, uint8_t sq) {

	int x, y;
	uint8_t pc = g->pos.board[sq];
	int dark = ((CE_FILE(sq) + CE_RANK(sq)) & 1) == 0;   /* a1 is dark */

	cb_square_xy(L, sq, &x, &y);

	if (scr && !ar_color()) {
		/* the screen in black and white: the desktop's own tile */
		ar_bitmap_opaque(cp_tiles[dark ? CP_DARK : CP_LIGHT][tile_index(pc)],
			CP_TILE_STRIDE, 0, 0, x, y, CP_TILE_W, CP_TILE_H);
	} else if (scr) {
		int ti = tile_index(pc);
		ar_fill(x, y, CB_SQ, CB_SQ, dark ? CB_C_DARK : CB_C_LIGHT);
		if (ti) {
			bool white = ti <= 6;
			ar_bitmap(pc_in[ti - 1], CP_TILE_STRIDE, 0, 0, x, y,
				CP_TILE_W, CP_TILE_H, white ? CB_C_WPC : CB_C_DETAIL);
			ar_bitmap(pc_ink[ti - 1], CP_TILE_STRIDE, 0, 0, x, y,
				CP_TILE_W, CP_TILE_H, CB_C_BPC);
		}
	} else {
		blit_tile(L, cp_tiles[dark ? CP_DARK : CP_LIGHT][tile_index(pc)], x, y);
	}

	/* -- highlights, cheapest and least intrusive first --
	 *
	 * Every one of these is a frame or a block rather than a change
	 * of fill, because the dark squares are already a 50% dither and
	 * anything that alters their texture is indistinguishable from
	 * the texture itself. A frame reads on both colours. */

	if (sq == g->last_from || sq == g->last_to)
		frame(L, x, y, CB_SQ, 0, 1, CB_C_LAST);

	if (g->sel != CE_SQ_NONE && g->show_hints && is_destination(g, sq)) {
		if (pc == CE_EMPTY) {
			/* An empty destination gets a block in the middle. */
			fill(L, x + CB_SQ / 2 - 3, y + CB_SQ / 2 - 3, 6, 6, CB_C_HINT);
		} else {
			/* An occupied one gets corner ticks instead, so the
			 * piece being captured stays recognisable -- a block in
			 * the middle would sit on top of it. */
			int t = 5;
			fill(L, x, y, t, 2, CB_C_HINT);
			fill(L, x, y, 2, t, CB_C_HINT);
			fill(L, x + CB_SQ - t, y, t, 2, CB_C_HINT);
			fill(L, x + CB_SQ - 2, y, 2, t, CB_C_HINT);
			fill(L, x, y + CB_SQ - 2, t, 2, CB_C_HINT);
			fill(L, x, y + CB_SQ - t, 2, t, CB_C_HINT);
			fill(L, x + CB_SQ - t, y + CB_SQ - 2, t, 2, CB_C_HINT);
			fill(L, x + CB_SQ - 2, y + CB_SQ - t, 2, t, CB_C_HINT);
		}
	}

	if (sq == g->sel)
		frame(L, x, y, CB_SQ, 0, 2, CB_C_SEL);

	/* A king in check gets a double frame -- distinct from the single
	 * frame a selected square gets, without needing a second colour
	 * this display does not have. */
	if (g->pos.board[sq] == CE_PIECE(g->pos.side, CE_KING) &&
		ce_in_check(&g->pos, g->pos.side)) {
		frame(L, x, y, CB_SQ, 0, 2, CB_C_CHECK);
		frame(L, x, y, CB_SQ, 4, 1, CB_C_CHECK);
	}

}

void cb_draw_board(const cb_layout_t *L, chess_game_t *g) {

	int file, rank, i;
	char lab[2] = { 0, 0 };

	for (rank = 0; rank < 8; rank++)
		for (file = 0; file < 8; file++)
			cb_draw_square(L, g, CE_SQ(file, rank));

	/* Labels. Cleared first: they sit outside every tile, so nothing
	 * else in this function paints over the previous frame's. */
	fill(L, L->rank_x, L->board_y, CB_LABEL_W, CB_BOARD, CB_C_BG);
	fill(L, L->rank_x, L->file_y, CB_LABEL_W + CB_BOARD, CB_LABEL_H, CB_C_BG);

	for (i = 0; i < 8; i++) {
		int r = L->flipped ? i : 7 - i;
		lab[0] = (char)('1' + r);
		text(L, L->rank_x + 2, L->board_y + i * CB_SQ + (CB_SQ - FH) / 2, lab);
	}

	for (i = 0; i < 8; i++) {
		int f = L->flipped ? 7 - i : i;
		lab[0] = (char)('a' + f);
		text(L, L->board_x + i * CB_SQ + (CB_SQ - FW) / 2, L->file_y + 1, lab);
	}

}

/* -- panel ---------------------------------------------------------- */

static void panel_line(const cb_layout_t *L, int row, const char *s) {
	text(L, L->panel_x, L->panel_y + row * FH, s);
}

void cb_draw_panel(const cb_layout_t *L, chess_game_t *g) {

	char buf[40];
	int row = 0, rows, first, i;
	int mat;

	if (!L->has_panel) return;

	fill(L, L->panel_x, L->panel_y, L->panel_w, L->panel_h, CB_C_BG);

	if (g->result != CE_RESULT_NONE) {
		switch (g->result) {
		case CE_RESULT_CHECKMATE:
			snprintf(buf, sizeof(buf), "%s mates",
				g->pos.side == CE_WHITE ? "Black" : "White");
			break;
		case CE_RESULT_STALEMATE:  snprintf(buf, sizeof(buf), "Stalemate"); break;
		case CE_RESULT_FIFTY:      snprintf(buf, sizeof(buf), "Draw: 50 moves"); break;
		case CE_RESULT_MATERIAL:   snprintf(buf, sizeof(buf), "Draw: material"); break;
		case CE_RESULT_REPETITION: snprintf(buf, sizeof(buf), "Draw: repetition"); break;
		default:                   snprintf(buf, sizeof(buf), "Game over"); break;
		}
	} else if (g->thinking) {
		/* The depth and node count are not decoration. A search that
		 * is going to take five seconds has to show that it is
		 * ALIVE, or the app looks hung -- and this is a machine where
		 * five seconds of thinking is an ordinary move. */
		snprintf(buf, sizeof(buf), "Thinking d%d %luk", g->think_depth,
			(unsigned long)(g->think_nodes / 1000));
	} else {
		snprintf(buf, sizeof(buf), "%s to move",
			g->pos.side == CE_WHITE ? "White" : "Black");
	}
	panel_line(L, row++, buf);

	snprintf(buf, sizeof(buf), "L%d %s", g->level, ce_level_name(g->level));
	panel_line(L, row++, buf);

	switch (g->mode) {
	case CG_MODE_HUMAN_WHITE: snprintf(buf, sizeof(buf), "You: White"); break;
	case CG_MODE_HUMAN_BLACK: snprintf(buf, sizeof(buf), "You: Black"); break;
	case CG_MODE_TWO_HUMANS:  snprintf(buf, sizeof(buf), "Two players"); break;
	default:                  snprintf(buf, sizeof(buf), "Engine v engine"); break;
	}
	panel_line(L, row++, buf);

	/* Material balance, in pawns, from White's point of view. The one
	 * number a person actually wants while playing. */
	mat = (ce_material(&g->pos, CE_WHITE) - ce_material(&g->pos, CE_BLACK))
		/ 100;
	if (mat == 0) snprintf(buf, sizeof(buf), "Material even");
	else snprintf(buf, sizeof(buf), "Material %s%d",
		mat > 0 ? "W+" : "B+", mat > 0 ? mat : -mat);
	panel_line(L, row++, buf);

	row++;
	fill(L, L->panel_x, L->panel_y + row * FH - 3, L->panel_w, 1, CB_C_RULE);

	/* -- the move list --
	 *
	 * Shows the END of the game, not the beginning. Somebody looking
	 * at this mid-game wants the last few moves; the opening is
	 * history. */
	rows = (L->panel_h - row * FH) / FH;
	if (rows < 1) return;

	first = (g->nply + 1) / 2 - rows;     /* in full moves */
	if (first < 0) first = 0;

	for (i = 0; i < rows; i++) {

		int mv = first + i;               /* full move index, 0-based */
		int wi = mv * 2, bi = mv * 2 + 1;
		char w[10], b[10];

		if (wi >= g->nply) break;

		snprintf(w, sizeof(w), "%s", g->san[wi]);
		if (bi < g->nply) snprintf(b, sizeof(b), "%s", g->san[bi]);
		else b[0] = 0;

		snprintf(buf, sizeof(buf), "%3d.%-7s%s",
			g->start_fullmove + mv, w, b);
		panel_line(L, row + i, buf);
	}

}

/* -- status --------------------------------------------------------- */

void cb_draw_status(const cb_layout_t *L, chess_game_t *g) {

	char buf[CG_CMD_LEN + 4];
	int cols, from;

	fill(L, L->ox, L->msg_y, L->w, 2 * CB_LINE_H, CB_C_BG);
	fill(L, L->ox, L->msg_y - 2, L->w, 1, CB_C_RULE);

	text(L, L->ox + 1, L->msg_y + 1, g->message);

	/* The command line scrolls rather than running off the end. The
	 * caret is a block after the text, which is also what shows the
	 * field has focus -- there is nowhere else for typing to go in
	 * this app, so it is always drawn. */
	cols = (L->w - 4) / FW - 2;
	if (cols < 4) cols = 4;

	from = g->cmd_len - cols;
	if (from < 0) from = 0;

	snprintf(buf, sizeof(buf), "> %s", g->cmd + from);
	text(L, L->ox + 1, L->cmd_y + 1, buf);

	fill(L, L->ox + 1 + (int)strlen(buf) * FW, L->cmd_y + 1, FW - 1, FH, CB_C_INK);

}

void cb_draw_all(const cb_layout_t *L, chess_game_t *g) {

	/* The gap between the board block and the panel, and any slack
	 * above the board, belong to nobody and so are cleared here --
	 * the board and the panel each paint only their own rectangle. */
	fill(L, L->ox, L->oy, L->w, L->board_y - L->oy, CB_C_BG);
	fill(L, L->ox + CB_LABEL_W + CB_BOARD, L->board_y, CB_GAP, L->panel_h, CB_C_BG);
	fill(L, L->ox, L->file_y + CB_LABEL_H, L->w,
		L->msg_y - 2 - (L->file_y + CB_LABEL_H), CB_C_BG);

	if (!L->has_panel && L->panel_w > 0)
		fill(L, L->panel_x, L->panel_y, L->panel_w, L->panel_h, CB_C_BG);

	cb_draw_board(L, g);
	cb_draw_panel(L, g);
	cb_draw_status(L, g);

}
