/*
 * Zeitlos chess -- draw the board on the build machine and write it
 * out as a picture.
 *
 *   cd sw/apps/chess && make render
 *   make render WHAT=midgame|thinking|mate|narrow|game
 *   render DIR scr-<state>   that state on the screen face, in colour
 *
 * See sw/common/tests/zrender.h for why this exists at all and what
 * it can and cannot catch. In short: tests/test_layout.c checks the
 * relationships somebody thought to write down, and this checks the
 * ones nobody did. For a chessboard that is most of them -- whether
 * the pieces are legible at 24 pixels, whether a white piece reads on
 * a white square, whether the dark-square dither fights the outlines,
 * whether the move list is too cramped. None of those is an
 * assertion. All of them are obvious in one look.
 *
 * LOOK AT THE OUTPUT before changing the art in gen_pieces.py.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "chess_shim.h"

#include "../board_ui.h"
#include "../game.h"
#include "../input.h"
#include "../../../common/games/tests/ar_host.h"

static chess_game_t game;

/* -- the screen face: the same board through the arcade runtime, in
 * colour (`render DIR scr-midgame` and the other states with scr-):
 * the window in black and white, its colour window, full screen, every
 * frame checked against a full repaint -- */
static void scr_paint(void *user, const ar_rect_t *r) {
	cb_layout_t t;
	(void)user; (void)r;
	cb_layout(&t, 0, 0, AR_W, AR_H, game.flipped);
	cb_draw_all(&t, &game);
}

#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define WCOLOR(f, on)   { (f), ARH_WCOLOR, (on), 0, NULL, 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static int screen_face(const char *dir, const char *what) {
	static ar_desc_t desc;
	static char n1[48], n2[48], n3[48];
	ar_host_step_t script[8];
	ar_event_t e;
	int k = 0;

	snprintf(n1, sizeof n1, "%s_window", what);
	snprintf(n2, sizeof n2, "%s_colour_window", what);
	snprintf(n3, sizeof n3, "%s_full", what);
	script[k++] = (ar_host_step_t)SHOT(3, n1);
	script[k++] = (ar_host_step_t)WCOLOR(4, 1);
	script[k++] = (ar_host_step_t)SHOT(7, n2);
	script[k++] = (ar_host_step_t)MODE(8, ARH_FULL_COLOR);
	script[k++] = (ar_host_step_t)SHOT(11, n3);
	script[k++] = (ar_host_step_t)END(14);

	desc.name = "chess";
	desc.paint = scr_paint;
	memcpy(desc.palette, cb_palette, sizeof desc.palette);
	cb_mono_inks(desc.ink);
	cb_screen(true);
	ar_window_colour(true);
	ar_host_run(script, dir);
	if (!ar_init(&desc)) return 1;
	for (;;) {
		bool quit = false;
		while (ar_poll(&e)) if (e.type == AR_EV_QUIT) quit = true;
		if (quit) break;
		ar_frame();
	}
	printf("render: %s, %d pictures in %s, %u frames verified, %u differ\n",
		what, ar_host_shots(), dir, ar_host_verified(), ar_host_verify_bad());
	return ar_host_verify_bad() ? 1 : 0;
}

static void play(const char *moves) {

	const char *s = moves;

	while (*s) {
		char tok[12];
		int n = 0;
		uint32_t mv;
		bool amb;
		while (*s == ' ') s++;
		if (!*s) break;
		while (*s && *s != ' ' && n < 11) tok[n++] = *s++;
		tok[n] = 0;
		mv = ce_parse_move(&game.pos, tok, &amb);
		if (mv == CE_MOVE_NONE) {
			fprintf(stderr, "render: '%s' is not legal here\n", tok);
			exit(1);
		}
		game_play(&game, mv);
	}

}

int main(int argc, char **argv) {

	const char *path = argc > 1 ? argv[1] : "/tmp/chess.pbm";
	const char *what = argc > 2 ? argv[2] : "start";
	const char *scr = NULL;
	z_win_t win;
	cb_layout_t L;
	int w = 320, h = 240;
	z_clip_t c;

	ce_init();

	game.level = 4;
	game.mode = CG_MODE_HUMAN_WHITE;
	game.show_hints = true;
	game_new(&game, NULL);
	game_set_message(&game, "type a move, or F1 for help");

	if (!strcmp(what, "narrow")) w = 240;

	/* scr-<state>: that state on the screen face, pictures into the
	 * directory `path` */
	if (!strncmp(what, "scr-", 4)) { scr = what; what += 4; }

	if (!scr && !z_render_open(&win, w, h)) {
		fprintf(stderr, "render: cannot map the framebuffer here "
			"(Linux/x86-64 only) -- skipping\n");
		return 77;
	}

	cb_init();

	if (!strcmp(what, "midgame")) {
		play("e4 e5 Nf3 Nc6 Bb5 a6 Ba4 Nf6 O-O Be7 Re1 b5 Bb3 d6 c3 O-O");
		game_set_message(&game, "d5 24817 nodes");
		snprintf(game.cmd, sizeof(game.cmd), "h3");
		game.cmd_len = 2;
		/* A selected piece with its destinations marked -- the state
		 * somebody is in for most of a game played with a mouse. */
		game.sel = CE_SQ(5, 2);           /* f3, the knight */
		{
			uint32_t all[CE_MAX_MOVES];
			int n = ce_gen_legal(&game.pos, all), k;
			game.sel_n = 0;
			for (k = 0; k < n; k++)
				if (CE_MOVE_FROM(all[k]) == game.sel)
					game.sel_moves[game.sel_n++] = all[k];
		}
	} else if (!strcmp(what, "thinking")) {
		play("e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6");
		game.thinking = true;
		game.think_depth = 6;
		game.think_nodes = 41200;
	} else if (!strcmp(what, "mate")) {
		/* Scholar's mate, so the result line and the check frame are
		 * both on screen. */
		play("e4 e5 Bc4 Nc6 Qh5 Nf6 Qxf7#");
		game_set_message(&game, "mate in 1");
	} else if (!strcmp(what, "game")) {
		/* Game mode is a 320x240 page with no window inset at all --
		 * a different rectangle from the windowed one, which is
		 * exactly what is worth looking at. */
		play("d4 Nf6 c4 g6 Nc3 Bg7 e4 d6");
		game_set_message(&game, "full screen -- Esc to leave");
	}

	if (scr) return screen_face(path, scr);

	if (!strcmp(what, "game")) {
		cb_layout(&L, 0, 0, 320, 240, game.flipped);
	} else {
		z_win_content_rect(&win, &c);
		cb_layout(&L, c.x0, c.y0, c.x1 - c.x0 + 1, c.y1 - c.y0 + 1,
			game.flipped);
	}

	cb_draw_all(&L, &game);

	/* z_render_write() writes the WINDOW's content area. Game mode is
	 * not a window, so its page is written by hand. */
	if (!strcmp(what, "game")) {
		FILE *f = fopen(path, "w");
		int x, y, i, j, scale = 2;
		if (!f) { perror(path); return 1; }
		fprintf(f, "P1\n%d %d\n", 320 * scale, 240 * scale);
		for (y = 0; y < 240; y++)
			for (j = 0; j < scale; j++) {
				for (x = 0; x < 320; x++)
					for (i = 0; i < scale; i++)
						fprintf(f, "%d ", shim_get(x, y));
				fprintf(f, "\n");
			}
		fclose(f);
	} else {
		z_render_write(path, &win, 2);
	}

	printf("render: wrote %s (%s)\n", path, what);

	return 0;

}
