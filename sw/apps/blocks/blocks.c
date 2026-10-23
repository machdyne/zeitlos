/*
 * blocks -- the falling-blocks game: steer the shapes as they fall, fill
 * rows to clear them, and do not let the stack reach the top. See
 * docs/blocks_app.md.
 *
 *   > run blocks
 *
 * Three ways to play (endless, forty lines against the clock, three
 * minutes for points), in a 320x240 window or full screen with F11 -- in
 * sixteen colours where the board has them. Built on the arcade runtime
 * (sw/common/games/zarcade.h) and its sound engine (zsfx.h).
 *
 * This file is the screens, the controls and the loop. The rules are
 * bk_game.c, the picture bk_draw.c, the sounds and tunes bk_sound.c.
 */

#include <stdio.h>
#include <string.h>

#include "../../common/games/zarcade.h"
#include "../../common/games/zsfx.h"

#include "blocks.h"
#include "bk_sound.h"

bk_app_t bk;

static const char help_text[] =
	"Shapes fall into the well. Turn and steer them so they fill rows: "
	"a full row clears. Four at once is worth the most. The game ends "
	"when the stack reaches the top.\n\n"
	"Left/Right\tMove (hold to slide)\n"
	"Down\tFaster\n"
	"Up, X\tTurn clockwise\n"
	"Z\tTurn the other way\n"
	"Space\tDrop at once\n"
	"H\tHold the shape for later\n"
	"P\tPause (Start on a pad)\n"
	"F11\tFull screen, or back to the window\n"
	"C\tFull screen: colour on or off\n"
	"F10\tColour on or off, in the window too\n"
	"M\tMusic on or off\n\n"
	"On a pad: the d-pad moves, Up drops, A and B turn, X holds.\n\n"
	"Skill: EASY falls slowly and forgives a full well three times "
	"(the top half is swept away); NORMAL is the classic game; HARD "
	"is faster. Left and Right on the top line of the menu change it. "
	"LITTLE PIECES is endless with shapes of one, two and three blocks "
	"in a narrower well.\n\n"
	"Endless: faster every ten rows. 40 lines: against the clock. "
	"3 minutes: as many points as you can. A T turned into a tight "
	"slot is a twist, worth more.";

static ar_desc_t desc = {
	"Blocks", "Blocks help", help_text, "blocks_app",
	{ 0 },
	{ { { 0 } } },
	bk_paint, NULL,
	false,
	bk_before_paint
};

static bool quit;
static int menu_hold;

#ifdef BK_AUTOPLAY
static bool autoplay_off;
static bk_plan_t plan;
static bool have_plan;
#endif

/* ===================================================================
 * small things
 * =================================================================== */

void bk_format_time(char *out, uint32_t frames) {
	uint32_t tenths = frames / 6;
	uint32_t m = tenths / 600, s = (tenths / 10) % 60, t = tenths % 10;
	char b[8];
	ar_utoa(out, m > 99 ? 99 : m, 2, '0');
	strcat(out, ":");
	ar_utoa(b, s, 2, '0');
	strcat(out, b);
	strcat(out, ".");
	ar_utoa(b, t, 1, '0');
	strcat(out, b);
}

/* The 40-line race is won by the SHORTEST time, and the table keeps the
 * highest number: a time is stored as 9999999 minus its frames. */
static uint32_t score_key(void) {
	if (bk.mode == BK_MODE_40) return bk.g.done ? 9999999u - bk.g.frames : 0;
	return bk.g.score;
}

bk_mode_t bk_menu_mode(int menu) {
	switch (menu) {
	case MENU_LITTLE: return BK_MODE_LITTLE;
	case MENU_40:     return BK_MODE_40;
	case MENU_3MIN:   return BK_MODE_3MIN;
	default:          return BK_MODE_ENDLESS;
	}
}

static bool menu_is_game(int m) {
	return m >= MENU_ENDLESS && m <= MENU_3MIN;
}

static bool menu_has_level(int m) {
	return m == MENU_ENDLESS || m == MENU_LITTLE;
}

static int top_level(void) {
	return ar_skill() == AR_SKILL_EASY ? 8 : 10;
}

/* Calm first, a step a level (docs/sfx.md, "Calm first"): easy from
 * the calmest (tune and bass, 70%), normal from the kick at 86% -- the
 * whole band by level 3, about the pace the game always had -- and hard
 * from the whole band at 98%. Normal started calm too once, and with
 * its rules unchanged it felt slower than it was. */
void bk_music(void) {
	static const int8_t base[AR_SKILLS] = { 0, 4, 7 };
	int lv = bk.g.level ? bk.g.level : 1;
	int sk = ar_skill();
	zsfx_music_intensity(base[sk >= 0 && sk < AR_SKILLS ? sk : 1] + lv - 1);
}

static void load_table(int mode) {
	ar_scores_load(ar_skill_table(mode), &bk.table);
	bk.best = bk.table.n ? bk.table.e[0].score : 0;
}

static void dirty_panel(void) {
	ar_rect_t r;
	if (bk_panel_rect(&r)) ar_dirty(r.x, r.y, r.w, r.h);
}

static void set_state(bk_state_t st) {
	dirty_panel();
	ar_text_input(st == ST_NAME);
	bk.st = st;
	bk.st_frames = 0;
	dirty_panel();
}

/* ===================================================================
 * screens
 * =================================================================== */

static void title(void) {
	bk_draw_reset();
	bk.mode = BK_MODE_ENDLESS;
	memset(&bk.g, 0, sizeof(bk.g));
	load_table(bk_menu_mode(bk.menu));
	bk.rank = -1;
	set_state(ST_TITLE);
	ar_dirty_all();
	zsfx_music_intensity(3);
	zsfx_music_play(&bk_song_title);
}

static void show_scores(int view, int rank) {
	bk.score_view = view;
	load_table(view);
	bk.rank = rank;
	set_state(ST_SCORES);
	ar_dirty_all();
}

static void start_game(bk_mode_t mode) {
	bk.mode = mode;
	load_table(mode);
	bk_draw_reset();
	bk_new_skill(&bk.g, mode,
		(mode == BK_MODE_ENDLESS || mode == BK_MODE_LITTLE) ? bk.start_level : 1,
		ar_skill(), ar_rand_below);
	bk_theme(bk.g.level);
	bk.rows = 0;
	bk.das_dir = 0;
	bk.das_count = 0;
	bk.shown_score = 0xffffffffu;
	set_state(ST_READY);
	ar_dirty_all();
	bk_music();
	zsfx_music_play(&bk_song_game);
#ifdef BK_AUTOPLAY
	have_plan = false;
#endif
}

static void pause(bool on) {
	if (on && bk.st == ST_PLAY) {
		set_state(ST_PAUSE);
		zsfx_suspend(true);
	} else if (!on && bk.st == ST_PAUSE) {
		set_state(ST_PLAY);
		zsfx_suspend(false);
		bk_draw_well_dirty();
	}
}

static void game_finished(void) {
	uint32_t key = score_key();
	bk.rank = key ? ar_scores_rank(&bk.table, key) : -1;
	set_state(ST_OVER);
	zsfx_music_stop();
	bk_sound(bk.g.done ? BK_SND_DONE : BK_SND_OVER);
}

static void name_letter(int delta) {
	char c = bk.name[bk.name_pos];
	int i = (c >= 'A' && c <= 'Z') ? c - 'A' : 26;
	i = (i + delta + 27) % 27;
	bk.name[bk.name_pos] = (char)(i == 26 ? '-' : 'A' + i);
	bk_sound(BK_SND_LETTER);
	dirty_panel();
}

static void name_done(void) {
	bk.name[3] = 0;
	load_table(bk.mode);
	bk.rank = ar_scores_insert(&bk.table, score_key(), bk.name);
	if (!ar_scores_save(ar_skill_table(bk.mode), &bk.table))
		printf("blocks: could not save the high scores\n");
	bk_sound(BK_SND_SELECT);
	show_scores(bk.mode, bk.rank);
}

/* ===================================================================
 * input
 * =================================================================== */

static void hold(void) {
	if (bk_hold(&bk.g)) bk.queue_dirty = true;
}

static void play_button(const ar_event_t *e) {
	bool pad = (e->source == 1);
	switch (e->key) {
	case AR_BTN_LEFT:
	case AR_BTN_RIGHT: {
		int d = (e->key == AR_BTN_LEFT) ? -1 : 1;
		bk_move(&bk.g, d);
		bk.das_dir = d;
		bk.das_count = 0;
		break;
	}
	case AR_BTN_UP:
		/* Up turns on a keyboard, drops on a pad: what each one's
		 * players expect. */
		if (pad) bk_hard_drop(&bk.g);
		else bk_rotate(&bk.g, 1);
		break;
	case AR_BTN_A:
		if (pad) bk_rotate(&bk.g, 1);
		else bk_hard_drop(&bk.g);
		break;
	case AR_BTN_B:
		if (pad) bk_rotate(&bk.g, -1);
		else hold();
		break;
	case AR_BTN_X:
	case AR_BTN_Y:
		hold();
		break;
	case AR_BTN_START:
		pause(true);
		break;
	default:
		break;
	}
}

static bool is_go(const ar_event_t *e) {
	return e->type == AR_EV_BUTTON && e->pressed &&
		(e->key == AR_BTN_A || e->key == AR_BTN_START);
}

static const char *const menu_say[MENU_ITEMS] = {
	"Skill", "Endless", "Little pieces", "40 lines", "3 minutes", "High scores"
};

static void say_menu(void) {
	char b[48];
	strcpy(b, menu_say[bk.menu]);
	if (bk.menu == MENU_SKILL) {
		strcat(b, ", ");
		strcat(b, ar_skill_spoken(ar_skill()));
	}
	ar_say(b);
}

static void menu_move(int d) {
	bk.menu = (bk.menu + d + MENU_ITEMS) % MENU_ITEMS;
	if (menu_is_game(bk.menu)) load_table(bk_menu_mode(bk.menu));
	bk_sound(BK_SND_MENU);
	dirty_panel();
	say_menu();
}

static void skill_change(int d) {
	ar_skill_set((ar_skill() + d + AR_SKILLS) % AR_SKILLS);
	if (bk.start_level > top_level()) bk.start_level = top_level();
	bk_sound(BK_SND_MENU);
	dirty_panel();
	ar_say(ar_skill_spoken(ar_skill()));
}

static void handle(const ar_event_t *e) {

	if (e->type == AR_EV_QUIT) { quit = true; return; }
	if (e->type == AR_EV_SUSPEND) { pause(true); return; }

	if (e->type == AR_EV_KEY && e->pressed) {
		uint32_t k = e->key;
#ifdef BK_AUTOPLAY
		if (k == 'K') { autoplay_off = true; return; }
		if (k == 'J') { autoplay_off = false; return; }
#endif
		if (bk.st == ST_NAME) {
			if (k >= 'a' && k <= 'z') k -= 'a' - 'A';
			if (k >= 'A' && k <= 'Z') {
				bk.name[bk.name_pos] = (char)k;
				if (bk.name_pos < 2) bk.name_pos++;
				bk_sound(BK_SND_LETTER);
				dirty_panel();
			} else if (k == 8 && bk.name_pos > 0) {
				bk.name_pos--;
				dirty_panel();
			}
			return;
		}
		if (k == 'p' || k == 'P') {
			if (bk.st == ST_PLAY) pause(true);
			else if (bk.st == ST_PAUSE) pause(false);
			return;
		}
		if (k == 0x1b) {                 /* Esc: in the window only */
			if (bk.st == ST_PLAY) pause(true);
			else if (bk.st == ST_PAUSE || bk.st == ST_SCORES ||
			         bk.st == ST_READY) {
				zsfx_suspend(false);
				title();
			}
			return;
		}
		if (bk.st == ST_PLAY) {
			if (k == 'z' || k == 'Z') bk_rotate(&bk.g, -1);
			else if (k == 'x' || k == 'X') bk_rotate(&bk.g, 1);
			else if (k == 'h' || k == 'H') hold();
		}
		return;
	}

	if (e->type != AR_EV_BUTTON || !e->pressed) return;

	switch (bk.st) {

	case ST_TITLE:
		if (e->key == AR_BTN_UP || e->key == AR_BTN_DOWN) {
			menu_move(e->key == AR_BTN_UP ? -1 : 1);
			menu_hold = 0;
		} else if ((e->key == AR_BTN_LEFT || e->key == AR_BTN_RIGHT) &&
		           bk.menu == MENU_SKILL) {
			skill_change(e->key == AR_BTN_RIGHT ? 1 : -1);
		} else if ((e->key == AR_BTN_LEFT || e->key == AR_BTN_RIGHT) &&
		           menu_has_level(bk.menu)) {
			bk.start_level += (e->key == AR_BTN_RIGHT) ? 1 : -1;
			if (bk.start_level < 1) bk.start_level = top_level();
			if (bk.start_level > top_level()) bk.start_level = 1;
			bk_sound(BK_SND_MENU);
			dirty_panel();
		} else if (bk.menu == MENU_SKILL && is_go(e)) {
			skill_change(1);
		} else if (is_go(e)) {
			bk_sound(BK_SND_SELECT);
			if (bk.menu == MENU_SCORES) show_scores(BK_MODE_ENDLESS, -1);
			else start_game(bk_menu_mode(bk.menu));
		}
		break;

	case ST_SCORES:
		if (e->key == AR_BTN_LEFT || e->key == AR_BTN_RIGHT) {
			int v = bk.score_view + (e->key == AR_BTN_RIGHT ? 1 : -1);
			if (v < 0) v = BK_MODES - 1;
			if (v >= BK_MODES) v = 0;
			bk_sound(BK_SND_MENU);
			show_scores(v, -1);
		} else if (is_go(e) || e->key == AR_BTN_B) {
			title();
		}
		break;

	case ST_PLAY:
		play_button(e);
		break;

	case ST_PAUSE:
		if (e->key == AR_BTN_START || e->key == AR_BTN_A) pause(false);
		break;

	case ST_OVER:
		if (bk.st_frames > 40 && is_go(e)) {
			if (bk.rank >= 0) {
				memcpy(bk.name, "AAA", 4);
				bk.name_pos = 0;
				set_state(ST_NAME);
			} else {
				show_scores(bk.mode, -1);
			}
		}
		break;

	case ST_NAME:
		switch (e->key) {
		case AR_BTN_UP:    name_letter(1); break;
		case AR_BTN_DOWN:  name_letter(-1); break;
		case AR_BTN_LEFT:  if (bk.name_pos > 0) bk.name_pos--; break;
		case AR_BTN_RIGHT: if (bk.name_pos < 2) bk.name_pos++; break;
		case AR_BTN_A:
			if (bk.name_pos < 2) bk.name_pos++;
			else name_done();
			break;
		case AR_BTN_START: name_done(); break;
		default: break;
		}
		break;

	default:
		break;
	}
}

/* Held keys, once a frame: sliding sideways after a pause, and the
 * soft drop. The press itself already moved (handle()). */
static void held_controls(void) {
	uint32_t h = ar_any_held();
	uint32_t want = bk.das_dir < 0 ? AR_BTN_LEFT : AR_BTN_RIGHT;

	if (bk.das_dir && (h & want)) {
		bk.das_count++;
		if (bk.das_count >= 10 && ((bk.das_count - 10) & 1) == 0)
			bk_move(&bk.g, bk.das_dir);
	} else if (h & (AR_BTN_LEFT | AR_BTN_RIGHT)) {
		/* the other one is still down: carry on with it */
		bk.das_dir = (h & AR_BTN_LEFT) ? -1 : 1;
		bk.das_count = 10;
	} else {
		bk.das_dir = 0;
	}

	bk_soft(&bk.g, (h & AR_BTN_DOWN) != 0);
}

#ifdef BK_AUTOPLAY
/* The host renders: the autopilot plays one action a frame, so the
 * pictures show a game rather than a stack in the middle. */
static void autoplay(void) {
	bk_game_t *g = &bk.g;
	int i;
	(void)i;
	if (autoplay_off || !g->cur.type) return;
	if (!have_plan) {
		have_plan = bk_ai_plan(g, &plan);
		if (have_plan && plan.use_hold) { hold(); have_plan = false; return; }
		return;
	}
	if (g->cur.rot != plan.rot) { if (!bk_rotate(g, 1)) bk_hard_drop(g); }
	else if (g->cur.x < plan.x) { if (!bk_move(g, 1)) bk_hard_drop(g); }
	else if (g->cur.x > plan.x) { if (!bk_move(g, -1)) bk_hard_drop(g); }
	else { bk_hard_drop(g); have_plan = false; }
}
#endif

/* ===================================================================
 * the frame
 * =================================================================== */

static void after_events(void) {
	int i;
	bk_game_t *g = &bk.g;

	bk_draw_events();

	for (i = 0; i < g->nev; i++) {
		/* A spawn, or a spawn that could not happen (topping out
		 * still takes the next piece off the queue). */
		if (g->ev[i].type == BK_EV_SPAWN || g->ev[i].type == BK_EV_OVER)
			bk.queue_dirty = true;
		if (g->ev[i].type == BK_EV_SPAWN) {
			bk.queue_dirty = true;
#ifdef BK_AUTOPLAY
			have_plan = false;
#endif
		}
		if (g->ev[i].type == BK_EV_CLEAR || g->ev[i].type == BK_EV_HOLD ||
		    g->ev[i].type == BK_EV_LEVEL || g->ev[i].type == BK_EV_RESCUE)
			bk.hud_dirty = true;
	}

	if (bk.st == ST_PLAY) {
		if (bk_clearing(g)) {
			bk.rows = g->clear_rows;
			set_state(ST_FLASH);
		} else if (g->over) {
			set_state(ST_CURTAIN);
			bk.curtain_row = BK_H - 1;
			zsfx_music_stop();
			bk_sound(BK_SND_OVER);
		} else if (g->done) {
			game_finished();
		}
	}

	bk_clear_events(g);
}

/* The status box: only the lines that changed are repainted. */
static void stats_dirty(void) {
	uint32_t t = (bk.mode == BK_MODE_3MIN ? bk_frames_left(&bk.g) : bk.g.frames) / 6;
	if (bk.g.score != bk.shown_score || bk.hud_dirty) {
		ar_dirty(STATS_X, STATS_Y + 10, 88, 8);
		bk.shown_score = bk.g.score;
	}
	if (bk.g.level != bk.shown_level || bk.g.lines != bk.shown_lines ||
	    bk.hud_dirty) {
		ar_dirty(STATS_X, STATS_Y + 26, 88, 20);
		bk.shown_level = bk.g.level;
		bk.shown_lines = bk.g.lines;
	}
	if (t != bk.shown_time || bk.hud_dirty) {
		ar_dirty(STATS_X + 32, STATS_Y + 50, 56, 8);
		bk.shown_time = t;
	}
	if (bk.hud_dirty) ar_dirty(STATS_X, STATS_Y + 64, 88, 8);
	if (bk.queue_dirty) {
		ar_dirty(NEXT_X, NEXT_Y, NEXT_W, QUEUE_Y + QUEUE_H - NEXT_Y);
		ar_dirty(HOLD_X, HOLD_Y, HOLD_W, HOLD_H);
	}
	bk.hud_dirty = bk.queue_dirty = false;
}

static void update(uint32_t dt) {

	uint32_t f;
	bk_game_t *g = &bk.g;

	for (f = 0; f < dt; f++) {

		bk.st_frames++;

		switch (bk.st) {

		case ST_READY:
			/* the count changes every half second (bk_draw.c's panel
			 * shows st_frames / 30): mark it on the frame it does */
			if (bk.st_frames % 30 == 0 || bk.st_frames == 1) {
				bk_sound(bk.st_frames >= 90 ? BK_SND_GO : BK_SND_COUNT);
				dirty_panel();
			}
			if (bk.st_frames >= 120) {
				set_state(ST_PLAY);
				bk_draw_well_dirty();
			}
			break;

		case ST_PLAY:
#ifdef BK_AUTOPLAY
			autoplay();
#endif
			held_controls();
			bk_step(g);
			after_events();
			break;

		case ST_FLASH:
			/* the clock runs through a clear too: bk_step() only
			 * counts time while no piece is in play */
			bk_step(g);
			if (bk.st_frames >= FLASH_FRAMES) {
				set_state(ST_FALL);
				bk_draw_fall_start();
			}
			break;

		case ST_FALL:
			bk_step(g);
			if (bk.st_frames > FALL_FRAMES + 1) {
				/* the last copy is on the glass: now the rules can
				 * close the gap for real -- the picture is the same */
				bk_clear_done(g);
				bk.rows = 0;
				memset(bk.fall_px, 0, sizeof(bk.fall_px));
				ar_shake(2, 6);
				bk_sound(BK_SND_LAND);
				bk.hud_dirty = true;
				set_state(ST_PLAY);
				after_events();
			} else {
				bk_draw_fall_step();
			}
			break;

		case ST_CURTAIN:
			if (bk.curtain_row >= BK_HIDDEN && (bk.st_frames & 1)) {
				int x;
				for (x = 0; x < BK_W; x++)
					g->well[bk.curtain_row][x] = BK_GREY;
				bk_draw_rows_dirty(1u << bk.curtain_row);
				bk.curtain_row--;
			}
			if (bk.curtain_row < BK_HIDDEN && bk.st_frames > 60)
				game_finished();
			break;

		default:
			break;
		}
	}

	/* menu repeat while held */
	if (bk.st == ST_TITLE) {
		uint32_t h = ar_any_held();
		if (h & (AR_BTN_UP | AR_BTN_DOWN)) {
			menu_hold += (int)dt;
			if (menu_hold >= 24) {
				menu_hold -= 7;
				menu_move((h & AR_BTN_UP) ? -1 : 1);
			}
		} else {
			menu_hold = 0;
		}
	}

	if (bk.st != ST_TITLE && bk.st != ST_SCORES) stats_dirty();
}

int main(void) {

	uint32_t dt = 1;

	memcpy(desc.palette, bk_base_palette, sizeof(desc.palette));
	bk_mono_inks(desc.ink);

	printf("blocks: rows of shapes, in a window or full screen (F11)\n");

	/* in colour in its window too, while it is focused and uncovered,
	 * on a board with the colour window (docs/color.md) */
	ar_window_colour(true);
	if (!ar_init(&desc)) return 1;
	if (!bk_draw_init()) printf("blocks: out of sprite memory\n");

	bk.start_level = 1;
	bk.menu = MENU_ENDLESS;
	printf("blocks: skill %s\n", ar_skill_name(ar_skill()));
	title();

	while (!quit) {
		ar_event_t e;
		while (ar_poll(&e)) handle(&e);
		if (quit) break;
		/* whatever the input just did, its effects and sounds */
		if (bk.st == ST_PLAY) after_events();
		else bk_clear_events(&bk.g);

		update(dt);
		bk_draw_frame(dt);
		dt = ar_frame();
	}

	ar_shutdown();
	return 0;
}
