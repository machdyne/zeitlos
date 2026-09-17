/*
 * Zeitlos chess.
 *
 * A chessboard that runs in a window or full screen, plays against
 * you at eight difficulty levels, and can be driven entirely from the
 * keyboard or entirely with a mouse.
 *
 * -- the one thing that shapes this whole file --
 *
 * The engine can think for seconds at a time, and an app that stops
 * reading its message queue for that long is drawing against a
 * VISIBLE REGION THAT MAY HAVE STOPPED BEING TRUE.
 *
 * This used to be a system-wide problem: wm blocked waiting for a
 * redraw acknowledgement until REDRAW_ACK_TIMEOUT, so a four-second
 * search froze every window on screen. wm does not wait any more
 * (docs/window_manager.md, "Content z-order"), which does not make the
 * pump optional -- it moves the whole cost here. Z_WM_SET_CLIP arrives
 * as a message like everything else, so four seconds of not reading
 * the queue is four seconds of painting over any window dropped in
 * front of the board, and of not servicing the redraw that would put
 * it back. The failure went from loud and system-wide to quiet and
 * local, which is harder to diagnose, not better.
 *
 * So ce_search_go() is given a poll callback -- search_poll() below --
 * which runs the message pump every few hundred nodes. Two
 * consequences run through the rest of the file:
 *
 *   1. The search runs on a COPY of the position. It makes and
 *      unmakes moves as it goes, so the board it is searching is a
 *      different board from the one on screen for almost all of that
 *      time. Repainting from it mid-search would draw a position
 *      thirty plies into a variation nobody chose.
 *
 *   2. While a search is running, input is restricted to the things
 *      that are safe: window events, and Escape to stop thinking.
 *      Running a command mid-search would mutate the game under the
 *      search's feet.
 *
 * -- windowed and full screen --
 *
 * Both, from one renderer. board_ui.c draws against an origin and a
 * clip rectangle; windowed passes the window's content rectangle and
 * game mode passes a 320x240 page. See board_ui.h.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zsoc.h"
#include "../../common/zkbd.h"
#include "../../common/zrng.h"

#include "ce_core.h"
#include "ce_eval.h"
#include "ce_search.h"
#include "ce_book.h"
#include "game.h"
#include "board_ui.h"
#include "input.h"

/* Content needs CB_LABEL_W + CB_BOARD + CB_GAP for the board block
 * and a panel wide enough for a move list; the height is the board,
 * the file labels and the two text lines. */
#define WIN_W 320
#define WIN_H 240

static z_win_t win;
static chess_game_t game;
static cb_layout_t layout_rect;

static bool running = true;
static bool game_mode;
static int  game_front;          /* the page currently being scanned out */
static bool exit_armed;
static bool in_search;

static void repaint(void);
static void pump(bool allow_input);

/*
 * The engine's own generator is a plain xorshift32 living in
 * ce_search.c, and it stays there: ce_*.c has no MMIO in it, which is
 * what lets the host tests build the shipped sources and get the same
 * answer twice. This is the seam where the real entropy gets in.
 *
 * z_rng_u32() is sw/common/zrng.c's ChaCha20 stream, seeded from
 * rtl/trng.v where the board has one and from cycle-counter jitter
 * where it does not. It never fails and never blocks, so there is
 * nothing to handle.
 *
 * NOT gated on z_rng_secure(). zrng.h is explicit about which of its
 * two questions to ask, and picking between two near-equal chess moves
 * wants unpredictable-to-a-person, not unpredictable-to-an-adversary.
 * Refusing to play on a board with no TRNG would be treating a board
 * game like a key exchange -- the same call sw/apps/poker and
 * sw/apps/slots make, for the same reason.
 *
 * Once per game, not once per move. Reseeding a generator every move
 * from another generator is churn that buys nothing; seeding it well
 * and letting the stream run is what it is for.
 */
static void seed_engine(void) {
	ce_search_seed(z_rng_u32());
}

/* -- time ----------------------------------------------------------- */

/*
 * z_uptime_ticks() runs at Z_TICK_HZ -- the KTIMER interrupt rate,
 * which is Z_SYSCLK_HZ / 65536 and therefore not a round number and
 * NOT the same on every board. About 732Hz at 48MHz.
 *
 * So the conversion factor is derived from Z_TICK_HZ at compile time
 * rather than written out. A hardcoded 48MHz constant would build
 * fine on a board clocked differently and simply run the engine's
 * clock fast or slow -- which would show up as the difficulty levels
 * thinking for the wrong length of time, with nothing to point at.
 *
 * Fixed point with 10 fractional bits, so the multiply is a multiply
 * and a shift instead of a __divsi3 call on an rv32i build. The
 * 64-bit intermediate is what keeps it monotonic: the uint32_t result
 * wraps every 49 days, which every caller handles because they all
 * compare unsigned DIFFERENCES.
 */
#define MS_PER_TICK_Q10 ((1024u * 1000u + Z_TICK_HZ / 2u) / Z_TICK_HZ)

static uint32_t now_ms(void) {
	return (uint32_t)(((uint64_t)z_uptime_ticks() * MS_PER_TICK_Q10) >> 10);
}

/* -- drawing -------------------------------------------------------- */

static void relayout(void) {

	if (game_mode) {
		int px = (game_front ^ 1) * Z_GAME_VIEW_W;    /* the back page */
		cb_layout(&layout_rect, px, 0, Z_GAME_VIEW_W, Z_GAME_VIEW_H,
			game.flipped);
	} else {
		z_clip_t c;
		/* Loads this window's visible region as a side effect, which
		 * is what confines every subsequent fill and blit to the part
		 * of the window not covered by something in front. See
		 * z_win_content_rect() in zwin.c. */
		z_win_content_rect(&win, &c);
		cb_layout(&layout_rect, c.x0, c.y0, c.x1 - c.x0 + 1,
			c.y1 - c.y0 + 1, game.flipped);
	}

}

static void repaint(void) {

	if (game_mode) {

		/* Nothing is in front of a game-mode page, so the window
		 * manager's clip region has to be OUT of the way -- left in
		 * place it would clip drawing to wherever this app's window
		 * happens to sit on the desktop behind. */
		z_gfx_clear_visible();
		z_gfx_blit_scissor_reset();

		relayout();
		cb_draw_all(&layout_rect, &game);

		z_game_set_view((uint32_t)((game_front ^ 1) * Z_GAME_VIEW_W), 0);
		z_game_wait_frame();
		game_front ^= 1;

		return;
	}

	relayout();
	cb_draw_all(&layout_rect, &game);

}

/* The panel alone, drawn into the page that is ALREADY visible.
 *
 * Used only by the thinking indicator. A full repaint would flip
 * pages, and flipping sixty times during one search to update a node
 * count is a lot of blitting for a number. The panel is text on a
 * cleared rectangle, so drawing it straight into the visible page
 * cannot tear in any way a person would see. */
static void repaint_thinking(void) {

	cb_layout_t t;

	if (game_mode) {
		z_gfx_clear_visible();
		z_gfx_blit_scissor_reset();
		cb_layout(&t, game_front * Z_GAME_VIEW_W, 0,
			Z_GAME_VIEW_W, Z_GAME_VIEW_H, game.flipped);
	} else {
		z_clip_t c;
		z_win_content_rect(&win, &c);
		cb_layout(&t, c.x0, c.y0, c.x1 - c.x0 + 1, c.y1 - c.y0 + 1,
			game.flipped);
	}

	cb_draw_panel(&t, &game);

}

/* -- game mode ------------------------------------------------------ */

static void enter_game_mode(void) {

	if (!z_game_available()) {
		game_set_message(&game, "this bitstream has no game mode");
		printf("chess: no game mode -- rebuild the gateware with "
			"`GAME in rtl/boards.vh\n");
		return;
	}

	game_mode = true;
	game_front = 0;
	exit_armed = false;

	/* Both pages, before the mode change, so the first frame scanned
	 * out is already this app's background rather than whatever the
	 * desktop left behind. */
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	z_fb_hw_fill_rect(0, 0, 640, 480, 0);

	z_game_set_enabled(true, false);

	repaint();
	repaint();       /* both pages, so a flip never shows a blank one */

}

static void leave_game_mode(void) {

	uint32_t wm_pid;

	z_game_set_enabled(false, false);
	game_mode = false;

	/* Every window is still alive and still where it was, and this
	 * app just drew over all of their pixels without any of them
	 * knowing. wm repairs damage it caused itself; this came from
	 * outside it, so it has to be told. See Z_WM_REPAINT in zwm.h. */
	if (z_pid_lookup("wm0", &wm_pid))
		z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));

	repaint();

}

/* -- the engine's turn ---------------------------------------------- */

static uint32_t poll_count;

static bool search_poll(void *user) {

	(void)user;

	/* allow_input false: window events and Escape only. A command run
	 * from here would change the game while the search is holding a
	 * copy of it. */
	pump(false);

	game.think_nodes = ce_search_nodes();

	/* Roughly every eight poll intervals, which is every couple of
	 * thousand nodes -- often enough that the number visibly moves,
	 * rare enough that redrawing it is not what the time is going
	 * on. */
	if ((++poll_count & 7) == 0) repaint_thinking();

	return game.cancel_search || !running;

}

static void engine_move(void) {

	ce_pos_t search_pos;
	ce_limits_t lim;
	ce_info_t info;

	if (!game_engine_to_move(&game)) return;

	/* THE COPY. See this file's header: the search mutates the
	 * position it is given, and the poll callback repaints from
	 * game.pos while it does. */
	search_pos = game.pos;

	game.thinking = true;
	game.cancel_search = false;
	game.think_depth = 0;
	game.think_nodes = 0;
	poll_count = 0;
	in_search = true;

	repaint();

	ce_level_limits(game.level, &lim);
	ce_search_set_history(game.key, game.nply + 1);
	ce_search_go(&search_pos, &lim, &info);

	in_search = false;
	game.thinking = false;
	game.think_depth = info.depth;

	if (!running) return;

	if (info.best == CE_MOVE_NONE) {
		game_set_message(&game, "the engine has no move");
		repaint();
		return;
	}

	if (!game_play(&game, info.best)) {
		game_set_message(&game, "the engine returned an illegal move");
		repaint();
		return;
	}

	if (game.cancel_search)
		game_set_message(&game, "stopped early -- played its best so far");
	else if (info.mate_in > 0)
		snprintf(game.message, sizeof(game.message), "mate in %d",
			info.mate_in);
	else if (info.from_book)
		game_set_message(&game, "book move");
	else
		snprintf(game.message, sizeof(game.message), "d%d %lu nodes",
			info.depth, (unsigned long)info.nodes);

	repaint();

}

/* -- bench ---------------------------------------------------------- */

/*
 * What this board actually manages, measured rather than assumed.
 *
 * Worth having as a command and not just a comment: the difficulty
 * levels are bounded by WALL CLOCK (see ce_level_limits()), so how
 * strong a given level plays depends entirely on how many nodes this
 * particular board gets through in that time -- and that varies by an
 * order of magnitude between a board running from SDRAM with an
 * instruction cache and one running from QQSPI PSRAM without
 * (docs/icache.md, docs/boot.md). This prints the number.
 */
static void do_bench(void) {

	ce_pos_t p;
	ce_limits_t lim;
	ce_info_t info;
	uint32_t t0, dt;
	uint64_t n;
	unsigned long gen_nps = 0, search_nps = 0;

	game_set_message(&game, "benchmarking...");
	repaint();

	ce_set_fen(&p, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/"
		"R3K2R w KQkq - 0 1");

	t0 = now_ms();
	n = ce_perft(&p, 3);
	dt = now_ms() - t0;
	if (dt) gen_nps = (unsigned long)(n * 1000u / dt);

	ce_search_new_game();
	ce_search_set_history(NULL, 0);
	ce_level_limits(6, &lim);
	lim.use_book = false;
	lim.max_ms = 4000;
	lim.max_depth = 6;

	t0 = now_ms();
	ce_search_go(&p, &lim, &info);
	dt = now_ms() - t0;
	if (dt) search_nps = (unsigned long)info.nodes * 1000ul / dt;

	printf("chess: movegen %lu nodes/s, search %lu nodes/s, "
		"depth %d in %lu ms\n", gen_nps, search_nps, info.depth,
		(unsigned long)dt);

	snprintf(game.message, sizeof(game.message),
		"gen %luk/s search %luk/s d%d", gen_nps / 1000,
		search_nps / 1000, info.depth);

	/* The benchmark left its own entries in the table; the game in
	 * progress should not inherit them. */
	ce_search_new_game();

	repaint();

}

/* -- actions -------------------------------------------------------- */

static void do_action(ci_action_t a) {

	switch (a) {

	case CI_QUIT:
		running = false;
		break;

	case CI_GAME_MODE:
		if (game_mode) leave_game_mode();
		else enter_game_mode();
		break;

	case CI_BENCH:
		do_bench();
		break;

	case CI_NEWGAME:
		seed_engine();
		/* fall through */
	case CI_MOVED:
		repaint();
		/* The engine may now owe a reply, and in demo mode it owes
		 * every reply. The loop is here rather than in main() so a
		 * move made by a click and a move typed as text take exactly
		 * the same path. */
		while (running && game_engine_to_move(&game)) {
			engine_move();
			if (game.cancel_search) break;
			if (game.mode != CG_MODE_TWO_ENGINES) break;
		}
		break;

	case CI_REDRAW:
		repaint();
		break;

	default:
		break;
	}

}

/* -- messages ------------------------------------------------------- */

static void handle_key(uint32_t packed, bool allow_input) {

	uint32_t sym = Z_WM_UNPACK_KEY_KEYSYM(packed);
	bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

	if (game_mode) {

		/* Escape leaves full screen -- but only once it has been
		 * RELEASED. Entering game mode with the `game` command and
		 * then pressing Escape is fine, but entering it by any route
		 * that leaves a key still held would otherwise see the key-up
		 * for a key that was already down and leave immediately.
		 * Same technique, and the same reason, as sw/apps/chip8. */
		if (sym == 0x1b) {
			if (!pressed) exit_armed = true;
			else if (exit_armed) leave_game_mode();
			return;
		}
	}

	if (!pressed) return;

	if (in_search) {
		/* The only key that means anything during a search. */
		if (sym == 0x1b) {
			game.cancel_search = true;
			game_set_message(&game, "stopping...");
		}
		return;
	}

	if (!allow_input) return;

	if (sym == Z_KEY_F2) { do_action(CI_GAME_MODE); return; }

	if (sym == Z_KEY_F1) {
		/* A rolling tour of the help text on the message line. The
		 * panel is too narrow for all of it at once and a modal help
		 * window would be a second window to service. */
		static int hl;
		const char *s = input_help_line(hl++);
		if (!s) { hl = 0; s = input_help_line(hl++); }
		game_set_message(&game, s);
		repaint();
		return;
	}

	do_action(input_key(&game, sym));

}

static void handle_mouse(uint32_t packed) {

	static bool was_down;
	bool down = (Z_WM_UNPACK_MOUSE_BUTTONS(packed) & Z_MOUSE_BTN_LEFT) != 0;
	int x = (int)Z_WM_UNPACK_MOUSE_X(packed);
	int y = (int)Z_WM_UNPACK_MOUSE_Y(packed);

	/* On the press edge only. Mouse messages are coalesced and
	 * repeat while a button is held, so acting on the level would
	 * pick the piece up and put it down again several times per
	 * click. */
	if (down && !was_down) {
		relayout();
		do_action(input_click(&game, &layout_rect, x, y));
	}

	was_down = down;

}

static void pump(bool allow_input) {

	z_msg_t msg;

	while (z_msg_read(&msg) == Z_OK) {

		switch (msg.subject) {

		case Z_WM_KEY:
			if (msg.obj.type == Z_UINT32)
				handle_key(msg.obj.val.uint32, allow_input);
			break;

		case Z_WM_MOUSE:
			/* Ignored entirely during a search: a click changes the
			 * selection and can play a move, and the search is
			 * holding a copy of the position it started from. */
			if (allow_input && !game_mode && msg.obj.type == Z_UINT32)
				handle_mouse(msg.obj.val.uint32);
			break;

		/* The part of this window not covered by the windows in front
		 * of it. The ack this sends is not optional -- wm waits for
		 * it when a region narrows. */
		case Z_WM_SET_CLIP:
			if (!z_win_apply_clip(&win, &msg.obj))
				printf("chess: bad clip region message\n");
			break;

		case Z_WM_REDRAW:
			if (msg.obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			/* Answered even in game mode, and answered even
			 * mid-search. The window is still a window as far as wm
			 * is concerned, and an unanswered redraw is what stalls
			 * the desktop. */
			if (!game_mode) repaint();
			z_win_redraw_done(&win);
			break;

		case Z_WM_WINDOW_MOVED:
			z_win_parse_rect(&win, &msg.obj);
			relayout();
			break;

		case Z_WM_WINDOW_RESIZED:
			if (z_win_apply_resized(&win, &msg.obj) && !game_mode) {
				relayout();
				repaint();
			}
			break;

		case Z_WM_CLOSE:
			running = false;
			break;

		default:
			break;
		}
	}

}

/* -- startup -------------------------------------------------------- */

static void apply_launch_arg(void) {

	char arg[Z_WM_ARG_MAX];

	/* Claimed whether or not it is used: leaving one pending would
	 * hand it to whatever the person opens next. */
	if (!z_launch_arg_take(arg, sizeof(arg))) return;
	if (!arg[0]) return;

	/* A bare number is a difficulty; anything else is tried as a
	 * FEN, so `run chess 6` and `run chess "<fen>"` both do the
	 * obvious thing. */
	if (arg[0] >= '1' && arg[0] <= '8' && !arg[1]) {
		game.level = arg[0] - '0';
		return;
	}

	{
		ce_pos_t test;
		if (ce_set_fen(&test, arg)) game_new(&game, arg);
		else printf("chess: ignoring argument '%s'\n", arg);
	}

}

int main(void) {

	printf("chess: starting\n");

	ce_init();
	ce_search_set_clock(now_ms);

	/* Installed once and left installed, rather than around each
	 * search.
	 *
	 * engine_move() is not the only thing that searches -- the `hint`
	 * command does too, for up to a second and a half. A search with
	 * no poll callback services no messages, and a second and a half
	 * of not answering a redraw is a second and a half of frozen
	 * desktop. Setting it here means every search is covered,
	 * including the next one somebody adds. */
	ce_search_set_poll(search_poll, NULL);

	ce_search_new_game();
	seed_engine();

	/* Built once, at startup, from coordinate notation -- cheap, and
	 * the reason ce_book.c does not store SAN. */
	ce_book_init();

	game.level = 3;
	game.mode = CG_MODE_HUMAN_WHITE;
	game.show_hints = true;
	game_new(&game, NULL);
	game_set_message(&game, "type a move, or F1 for help");

	if (z_win_create_flags(&win, "chess", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
		Z_WIN_FLAG_RESIZABLE) != Z_OK) {
		printf("chess: failed to create window -- is wm running?\n");
		return 1;
	}

	apply_launch_arg();

	cb_init();
	relayout();
	repaint();

	/* Black to move at the start means the engine opens. */
	if (game_engine_to_move(&game)) engine_move();

	while (running) {

		pump(true);

		if (!running) break;

		/* Demo mode keeps going without any input at all. */
		if (game_engine_to_move(&game)) {
			engine_move();
			continue;
		}

		/* Nothing to do until something arrives. The timeout exists
		 * only so a stuck message never wedges the app entirely; it
		 * is not a polling interval, because there is nothing here
		 * that changes on its own. */
		z_proc_wait(Z_TICK_HZ);

	}

	if (game_mode) leave_game_mode();

	z_win_destroy(&win);
	printf("chess: bye\n");

	return 0;

}
