/*
 * Zeitlos chess -- layout geometry. Runs on the HOST:
 *
 *   cd sw/apps/chess && make test
 *
 * This is the unattended half of the pair; tests/render.c is the half
 * a person looks at. sw/common/tests/zrender.h's header explains why
 * both exist, using sw/apps/logic's three shipped-wrong panels as the
 * worked example: a geometry assertion can only check a relationship
 * somebody thought to write down.
 *
 * So this file checks the relationships that ARE known and that the
 * app would be broken without:
 *
 *   - a click lands on the square it looks like it landed on, for all
 *     64 squares, both orientations
 *   - nothing is ever drawn outside the content rectangle
 *   - a piece that moves away is actually erased
 *   - the panel is dropped, rather than overlapping the board, when
 *     the window is too narrow for both
 */

#include <stdio.h>
#include <string.h>

#include "chess_shim.h"

#include "../board_ui.h"
#include "../game.h"
#include "../input.h"

static int checks, fails;

static void okmsg(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}

static void eq_int(long got, long want, const char *what) {
	checks++;
	if (got != want) {
		fails++;
		printf("  FAIL: %s -- got %ld, want %ld\n", what, got, want);
	}
}

/* The content rectangle is deliberately NOT at the origin. A layout
 * bug that confuses content-relative with absolute coordinates is
 * invisible at (0,0) -- which is exactly how sw/apps/logic shipped
 * its panel drawn at the window's screen position three times. */
#define OX 40
#define OY 30
#define CW (WIN_CONTENT_W)
#define CH (WIN_CONTENT_H)

/* What chess.c's 320x240 window actually gives the renderer:
 * z_win_content_rect() insets by 2 on every side and by the titlebar
 * at the top. */
#define WIN_CONTENT_W (320 - 4)
#define WIN_CONTENT_H (240 - 11 - 4)

static chess_game_t game;

static void ink_outside(const cb_layout_t *L, const char *what) {

	int x, y, bad = 0;

	for (y = 0; y < Z_SCREEN_H && !bad; y++)
		for (x = 0; x < Z_SCREEN_W; x++) {
			if (x >= L->clip.x0 && x <= L->clip.x1 &&
				y >= L->clip.y0 && y <= L->clip.y1) continue;
			if (shim_get(x, y)) {
				printf("  FAIL: %s -- ink at (%d,%d), outside the "
					"content rect (%d,%d)-(%d,%d)\n", what, x, y,
					(int)L->clip.x0, (int)L->clip.y0,
					(int)L->clip.x1, (int)L->clip.y1);
				bad = 1;
				break;
			}
		}

	checks++;
	if (bad) fails++;

}

static int ink_in(int x0, int y0, int w, int h) {
	int x, y, n = 0;
	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++)
			if (shim_get(x, y)) n++;
	return n;
}

int main(void) {

	z_win_t win;
	cb_layout_t L;
	int i;

	ce_init();

	if (!z_render_open(&win, 320, 240)) {
		printf("test_layout: cannot map the framebuffer here -- skipping\n");
		return 77;
	}

	cb_init();

	printf("layout: geometry\n");

	cb_layout(&L, OX, OY, CW, CH, false);

	/* The board block and the two text lines must all fit, or the
	 * default window size is wrong -- which is a thing to find here
	 * and not on a screen. */
	okmsg(CW >= CB_MIN_W, "default window is wide enough for the board");
	okmsg(CH >= CB_MIN_H, "default window is tall enough for the board");
	okmsg(L.has_panel, "default window has room for the side panel");

	eq_int(L.board_x, OX + CB_LABEL_W, "board sits right of the rank labels");
	okmsg(L.board_y >= OY, "board starts inside the content rect");
	okmsg(L.file_y == L.board_y + CB_BOARD,
		"file labels sit directly under the board");
	okmsg(L.file_y + CB_LABEL_H <= L.msg_y,
		"file labels clear the message line");
	okmsg(L.msg_y + CB_LINE_H <= L.cmd_y, "message line clears the command line");
	eq_int(L.cmd_y + CB_LINE_H, OY + CH, "command line reaches the bottom");

	/* The panel must start beyond the board block -- an overlap would
	 * put the move list on top of the h-file. */
	okmsg(L.panel_x >= L.board_x + CB_BOARD,
		"panel starts clear of the board");
	okmsg(L.panel_x + L.panel_w <= OX + CW,
		"panel ends inside the content rect");

	printf("layout: squares and clicks\n");

	for (i = 0; i < 2; i++) {

		int file, rank, bad = 0;

		cb_layout(&L, OX, OY, CW, CH, i != 0);

		for (rank = 0; rank < 8 && !bad; rank++)
			for (file = 0; file < 8; file++) {

				uint8_t sq = CE_SQ(file, rank);
				int x, y;

				cb_square_xy(&L, sq, &x, &y);

				if (x < L.clip.x0 || y < L.clip.y0 ||
					x + CB_SQ - 1 > L.clip.x1 ||
					y + CB_SQ - 1 > L.clip.y1) {
					printf("  FAIL: square %c%d falls outside the content "
						"rect\n", 'a' + file, rank + 1);
					bad = 1;
					break;
				}

				/* Every corner and the middle of the square must map
				 * back to it. The middle alone would pass even with a
				 * half-square offset. */
				if (cb_xy_square(&L, x, y) != sq ||
					cb_xy_square(&L, x + CB_SQ - 1, y) != sq ||
					cb_xy_square(&L, x, y + CB_SQ - 1) != sq ||
					cb_xy_square(&L, x + CB_SQ - 1, y + CB_SQ - 1) != sq ||
					cb_xy_square(&L, x + CB_SQ / 2, y + CB_SQ / 2) != sq) {
					printf("  FAIL: click round trip failed for %c%d "
						"(flipped=%d)\n", 'a' + file, rank + 1, i);
					bad = 1;
					break;
				}
			}

		checks++;
		if (bad) fails++;
	}

	/* a1 is bottom left unflipped and top right flipped -- the one
	 * relationship that says the flip is a flip and not a no-op. */
	{
		int x1, y1, x2, y2;
		cb_layout(&L, OX, OY, CW, CH, false);
		cb_square_xy(&L, CE_SQ(0, 0), &x1, &y1);
		cb_layout(&L, OX, OY, CW, CH, true);
		cb_square_xy(&L, CE_SQ(0, 0), &x2, &y2);
		okmsg(x1 < x2 && y1 > y2, "flipping moves a1 to the far corner");
	}

	/* One pixel left of, or above, the board is NOT the board. The
	 * integer divide in cb_xy_square() truncates towards zero, so
	 * without an explicit test a click just outside the a-file reads
	 * as a click on it. */
	cb_layout(&L, OX, OY, CW, CH, false);
	okmsg(cb_xy_square(&L, L.board_x - 1, L.board_y + 5) == CE_SQ_NONE,
		"one pixel left of the board is not a square");
	okmsg(cb_xy_square(&L, L.board_x + 5, L.board_y - 1) == CE_SQ_NONE,
		"one pixel above the board is not a square");
	okmsg(cb_xy_square(&L, L.board_x + CB_BOARD, L.board_y + 5) == CE_SQ_NONE,
		"one pixel right of the board is not a square");
	okmsg(cb_xy_square(&L, L.board_x + 5, L.board_y + CB_BOARD) == CE_SQ_NONE,
		"one pixel below the board is not a square");

	printf("layout: drawing stays inside the content rect\n");

	game.level = 3;
	game.mode = CG_MODE_HUMAN_WHITE;
	game.show_hints = true;
	game_new(&game, NULL);
	game_set_message(&game, "a message that is quite long indeed");
	snprintf(game.cmd, sizeof(game.cmd), "Nf3");
	game.cmd_len = 3;

	cb_layout(&L, OX, OY, CW, CH, false);
	z_render_clear();
	cb_draw_all(&L, &game);
	ink_outside(&L, "full repaint");

	okmsg(ink_in(L.board_x, L.board_y, CB_BOARD, CB_BOARD) > 500,
		"the board actually drew something");
	okmsg(ink_in(L.panel_x, L.panel_y, L.panel_w, 8) > 0,
		"the panel actually drew something");
	okmsg(ink_in(L.ox, L.cmd_y, L.w, CB_LINE_H) > 0,
		"the command line actually drew something");

	/* A selected square with hints on draws markers, which is the
	 * path most likely to run off the edge of a square. */
	game.sel = CE_SQ(4, 1);
	{
		uint32_t all[CE_MAX_MOVES];
		int n = ce_gen_legal(&game.pos, all), k;
		game.sel_n = 0;
		for (k = 0; k < n; k++)
			if (CE_MOVE_FROM(all[k]) == game.sel)
				game.sel_moves[game.sel_n++] = all[k];
	}
	z_render_clear();
	cb_draw_all(&L, &game);
	ink_outside(&L, "repaint with a selection and hints");
	okmsg(game.sel_n == 2, "the e2 pawn has two moves at the start");

	printf("layout: a piece that moves away is erased\n");

	/* The point of this one: the tiles are OPAQUE, and the whole
	 * reason board_ui.c blits a whole tile per square rather than
	 * drawing a transparent sprite is that a transparent sprite
	 * leaves the old piece behind. If the blit ever stops copying and
	 * starts OR-ing, this is what notices. */
	{
		int x, y, before, after;
		uint32_t mv;
		bool amb;

		game_new(&game, NULL);
		cb_layout(&L, OX, OY, CW, CH, false);
		z_render_clear();
		cb_draw_all(&L, &game);

		cb_square_xy(&L, CE_SQ(1, 0), &x, &y);       /* b1, a knight */

		/* The INTERIOR of the square, inset past the edge.
		 * board_ui.c draws a one-pixel frame round the last move's
		 * from and to squares, and that frame is ink -- measuring the
		 * whole square would compare a knight against a frame and
		 * find them comparably dark, which is how the first version
		 * of this test failed on correct code. */
		before = ink_in(x + 3, y + 3, CB_SQ - 6, CB_SQ - 6);
		okmsg(before > 0, "b1 has a knight on it to start with");

		mv = ce_parse_move(&game.pos, "Nc3", &amb);
		okmsg(mv != CE_MOVE_NONE, "Nc3 parses");
		game_play(&game, mv);
		cb_draw_board(&L, &game);

		after = ink_in(x + 3, y + 3, CB_SQ - 6, CB_SQ - 6);
		/* b1 is a light square, so an empty one is genuinely blank
		 * inside the frame. */
		okmsg(after == 0, "the knight was erased from b1");
	}

	printf("layout: a window too narrow for the panel\n");

	/* Just wide enough for the board block and nothing else. The
	 * panel must be DROPPED, not squeezed on top of the board --
	 * a board you can see all of beats a panel you cannot read. */
	cb_layout(&L, OX, OY, CB_LABEL_W + CB_BOARD + 10, CH, false);
	okmsg(!L.has_panel, "panel dropped when there is no room");
	okmsg(L.board_x + CB_BOARD <= L.clip.x1 + 1,
		"board still fits when the panel is dropped");

	z_render_clear();
	cb_draw_all(&L, &game);
	ink_outside(&L, "repaint in a narrow window");

	/* And a window too small for the board at all: it must clip, not
	 * scribble. Somebody WILL drag the corner. */
	cb_layout(&L, OX, OY, 120, 120, false);
	z_render_clear();
	cb_draw_all(&L, &game);
	ink_outside(&L, "repaint in a window smaller than the board");

	printf("layout: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;

}
