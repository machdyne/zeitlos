/*
 * snake -- the classic: steer a snake to the food, grow with every
 * bite, and do not run into anything. See docs/snake_app.md.
 *
 *   > run snake
 *
 * One or two players, four ways to play (classic, open field, mazes,
 * versus), in a 320x240 window or full screen with F11 -- in sixteen
 * colours where the board has them. Built on the arcade runtime
 * (sw/common/games/zarcade.h) and its sound engine (zsfx.h).
 *
 * This file is the screens and the loop. The rules are sn_game.c, the
 * picture sn_draw.c, the sounds and tunes sn_sound.c.
 */

#include <stdio.h>
#include <string.h>

#include "../../common/games/zarcade.h"
#include "../../common/games/zsfx.h"

#include "snake.h"
#include "sn_sound.h"

sn_app_t sn;

const char *const sn_mode_names[] = {
	"CLASSIC", "OPEN FIELD", "MAZES", "TWO PLAYERS", "TITLE"
};

static const char help_text[] =
	"Steer the snake to the food. Every bite makes it longer -- and, in "
	"the classic game, faster. Run into a wall or a snake and it is "
	"over.\n\n"
	"Arrows\tSteer (also W A S D, or a gamepad)\n"
	"P\tPause (Start on a pad)\n"
	"Esc\tPause; again for the menu\n"
	"F11\tFull screen, or back to the window\n"
	"C\tFull screen: colour on or off\n"
	"F10\tColour on or off, in the window too\n"
	"M\tMusic on or off\n\n"
	"Skill: on EASY the snake starts slowly and a crash only costs a "
	"heart -- it stops and waits for you to steer it somewhere free. "
	"NORMAL is the classic game, HARD is faster. Left and Right on the "
	"top line of the menu change it.\n\n"
	"Classic: walls all round. Open field: the edges wrap. Mazes: eat "
	"twelve to clear a level. Two players: the arrows against W A S D "
	"(or two gamepads), first to three rounds.\n\n"
	"A star appears now and then: worth more the sooner you get it.";

static ar_desc_t desc = {
	"Snake", "Snake help", help_text, "snake_app",
	{ 0 },
	{ { { 0 } } },
	sn_paint, NULL,
	false,
	NULL
};

static bool quit;
#ifdef SN_AUTOPLAY
static bool autoplay_off;
#endif
static int menu_hold;     /* frames UP or DOWN has been held */

/* ===================================================================
 * states
 * =================================================================== */

static void dirty_panel(void) {
	ar_rect_t r;
	if (sn_panel_rect(&r)) ar_dirty(r.x, r.y, r.w, r.h);
}

static void set_state(sn_state_t st) {
	dirty_panel();               /* where the old panel was */
	ar_text_input(st == ST_NAME);   /* initials: C and M are letters */
	sn.st = st;
	sn.st_frames = 0;
	dirty_panel();               /* where the new one is */
	ar_dirty(0, 0, AR_W, SN_PLAY_Y);
}

static int score_mode(sn_mode_t m) {
	return ar_skill_table((int)m);
}

/* The music follows the snake: calm and slow at the start, the whole
 * band at full speed only when the snake is fast (zsfx.h,
 * zsfx_music_intensity()). 3.5 cells a second is the calmest, every 0.8
 * a step more. */
static void music_follow(void) {
	int cps = sn_cps(&sn.g.s[0]);
	zsfx_music_intensity((cps - 35) / 8);
}

static const char *const menu_say[MENU_ITEMS] = {
	"Skill", "Classic", "Open field", "Mazes", "Two players", "High scores"
};

static void say_menu(void) {
	char b[48];
	strcpy(b, menu_say[sn.menu]);
	if (sn.menu == MENU_SKILL) {
		strcat(b, ", ");
		strcat(b, ar_skill_spoken(ar_skill()));
	}
	ar_say(b);
}

static void load_table(sn_mode_t m) {
	ar_scores_load(score_mode(m), &sn.table);
	sn.hiscore = sn.table.n ? sn.table.e[0].score : 0;
}

static void title(void) {
	sn_new(&sn.g, SN_MODE_TITLE, 0, ar_rand_below);
	sn_draw_reset();
	load_table(SN_MODE_CLASSIC);
	sn.rank = -1;
	ar_dirty_all();
	set_state(ST_TITLE);
	zsfx_music_intensity(3);
	zsfx_music_play(&sn_song_title);
}

static void show_scores(int view, int rank) {
	sn.score_view = view;
	load_table((sn_mode_t)view);
	sn.rank = rank;
	set_state(ST_SCORES);
	/* the field is drawn without the snakes now: all of it */
	ar_dirty_all();
}

static void begin(sn_mode_t mode, int level, bool keep) {
	uint32_t score = sn.g.s[0].score;
	uint8_t w0 = sn.g.s[0].wins, w1 = sn.g.s[1].wins;
	uint8_t h0 = sn.g.s[0].hearts, h1 = sn.g.s[1].hearts;
	sn_new_skill(&sn.g, mode, level, ar_skill(), ar_rand_below);
	if (keep) {
		sn.g.s[0].score = score;
		sn.g.s[0].wins = w0;
		sn.g.s[1].wins = w1;
		/* a maze level keeps the hearts it had; a round of two
		 * players starts each with a full set */
		if (mode == SN_MODE_MAZE && h0) sn.g.s[0].hearts = h0;
		(void)h1;
	}
	music_follow();
	sn.mode = mode;
	sn_draw_reset();
	sn.dissolving[0] = sn.dissolving[1] = false;
	ar_dirty_all();
	set_state(ST_READY);
	sn_sound(SN_SND_READY, -1);
}

static void start_game(sn_mode_t mode) {
	if (mode != SN_MODE_VERSUS) load_table(mode);
	else sn.hiscore = 0;
	memset(&sn.g, 0, sizeof(sn.g));
	begin(mode, 0, false);
	zsfx_music_play(&sn_song_game);
}

static void menu_move(int d) {
	sn.menu = (sn.menu + d + MENU_ITEMS) % MENU_ITEMS;
	sn_sound(SN_SND_MENU, -1);
	dirty_panel();
	say_menu();
}

static void skill_change(int d) {
	ar_skill_set((ar_skill() + d + AR_SKILLS) % AR_SKILLS);
	sn_sound(SN_SND_MENU, -1);
	load_table(SN_MODE_CLASSIC);
	sn.hud_dirty = true;
	dirty_panel();
	ar_say(ar_skill_spoken(ar_skill()));
}

/* ===================================================================
 * input
 * =================================================================== */

static int button_dir(uint32_t b) {
	switch (b) {
	case AR_BTN_RIGHT: return SN_RIGHT;
	case AR_BTN_DOWN:  return SN_DOWN;
	case AR_BTN_LEFT:  return SN_LEFT;
	case AR_BTN_UP:    return SN_UP;
	default:           return -1;
	}
}

static bool is_go(const ar_event_t *e) {
	return e->type == AR_EV_BUTTON && e->pressed &&
		(e->key == AR_BTN_A || e->key == AR_BTN_START);
}

static void pause(bool on) {
	if (on && sn.st == ST_PLAY) {
		set_state(ST_PAUSE);
		zsfx_suspend(true);
	} else if (!on && sn.st == ST_PAUSE) {
		set_state(ST_PLAY);
		zsfx_suspend(false);
	}
}

static void name_letter(int delta) {
	char c = sn.name[sn.name_pos];
	int i = (c >= 'A' && c <= 'Z') ? c - 'A' : 26;   /* 26 = '-' */
	i = (i + delta + 27) % 27;
	sn.name[sn.name_pos] = (char)(i == 26 ? '-' : 'A' + i);
	sn_sound(SN_SND_LETTER, -1);
	dirty_panel();
}

static void name_done(void) {
	sn.name[3] = 0;
	load_table(sn.mode);
	sn.rank = ar_scores_insert(&sn.table, sn.g.s[0].score, sn.name);
	if (!ar_scores_save(score_mode(sn.mode), &sn.table))
		printf("snake: could not save the high scores\n");
	sn_sound(SN_SND_SELECT, -1);
	show_scores(sn.mode, sn.rank);
}

static void handle(const ar_event_t *e) {

	if (e->type == AR_EV_QUIT) { quit = true; return; }

	if (e->type == AR_EV_SUSPEND) { pause(true); return; }

	if (e->type == AR_EV_KEY && e->pressed) {
		uint32_t k = e->key;
#ifdef SN_AUTOPLAY
		if (k == 'K') { autoplay_off = true; return; }
		if (k == 'J') { autoplay_off = false; return; }
#endif
		if (k == 'p' || k == 'P') {
			if (sn.st == ST_PLAY) pause(true);
			else if (sn.st == ST_PAUSE) pause(false);
			return;
		}
		if (k == 0x1b) {              /* Esc: in the window only */
			if (sn.st == ST_PLAY) pause(true);
			else if (sn.st == ST_PAUSE || sn.st == ST_SCORES ||
			         sn.st == ST_READY || sn.st == ST_ROUND ||
			         sn.st == ST_LEVEL) {
				zsfx_suspend(false);
				title();
			}
			return;
		}
		if (sn.st == ST_NAME) {
			if (k >= 'a' && k <= 'z') k -= 'a' - 'A';
			if (k >= 'A' && k <= 'Z') {
				sn.name[sn.name_pos] = (char)k;
				if (sn.name_pos < 2) sn.name_pos++;
				sn_sound(SN_SND_LETTER, -1);
				dirty_panel();
			} else if (k == 8 && sn.name_pos > 0) {
				sn.name_pos--;
				dirty_panel();
			}
			return;
		}
	}

	if (e->type != AR_EV_BUTTON || !e->pressed) return;

	switch (sn.st) {

	case ST_TITLE:
		/* A press moves at once; holding repeats (update()). */
		if (e->key == AR_BTN_UP || e->key == AR_BTN_DOWN) {
			menu_move(e->key == AR_BTN_UP ? -1 : 1);
			menu_hold = 0;
		} else if (sn.menu == MENU_SKILL &&
		           (e->key == AR_BTN_LEFT || e->key == AR_BTN_RIGHT)) {
			skill_change(e->key == AR_BTN_RIGHT ? 1 : -1);
		} else if (sn.menu == MENU_SKILL && is_go(e)) {
			skill_change(1);
		} else if (is_go(e)) {
			sn_sound(SN_SND_SELECT, -1);
			switch (sn.menu) {
			case MENU_CLASSIC: start_game(SN_MODE_CLASSIC); break;
			case MENU_OPEN:    start_game(SN_MODE_OPEN); break;
			case MENU_MAZE:    start_game(SN_MODE_MAZE); break;
			case MENU_VERSUS:  start_game(SN_MODE_VERSUS); break;
			default:           show_scores(SN_MODE_CLASSIC, -1); break;
			}
		}
		break;

	case ST_SCORES:
		if (e->key == AR_BTN_LEFT || e->key == AR_BTN_RIGHT) {
			int v = sn.score_view + (e->key == AR_BTN_RIGHT ? 1 : -1);
			if (v < 0) v = 2;
			if (v > 2) v = 0;
			sn_sound(SN_SND_MENU, -1);
			show_scores(v, -1);
		} else if (is_go(e) || e->key == AR_BTN_B) {
			title();
		}
		break;

	case ST_READY:
	case ST_PLAY: {
		int d = button_dir(e->key);
		int p = sn.g.players == 2 ? e->player : 0;
		if (d >= 0) sn_turn(&sn.g, p, d);
		else if (e->key == AR_BTN_START && sn.st == ST_PLAY) pause(true);
		break;
	}

	case ST_PAUSE:
		if (e->key == AR_BTN_START || e->key == AR_BTN_A) pause(false);
		break;

	case ST_OVER:
		if (sn.st_frames > 40 && is_go(e)) {
			if (sn.rank >= 0) {
				memcpy(sn.name, "AAA", 4);
				sn.name_pos = 0;
				set_state(ST_NAME);
			} else {
				show_scores(sn.mode, -1);
			}
		}
		break;

	case ST_ROUND:
		if (sn.st_frames > 40 && is_go(e)) sn.st_frames = 100000;
		break;

	case ST_NAME:
		switch (e->key) {
		case AR_BTN_UP:    name_letter(1); break;
		case AR_BTN_DOWN:  name_letter(-1); break;
		case AR_BTN_LEFT:  if (sn.name_pos > 0) sn.name_pos--; break;
		case AR_BTN_RIGHT: if (sn.name_pos < 2) sn.name_pos++; break;
		case AR_BTN_A:
			if (sn.name_pos < 2) sn.name_pos++;
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

/* ===================================================================
 * the frame
 * =================================================================== */

static void after_death(void) {
	const sn_game_t *g = &sn.g;
	if (g->players == 2) {
		bool a0 = g->s[0].alive, a1 = g->s[1].alive;
		sn.round_winner = (a0 && !a1) ? 0 : (a1 && !a0) ? 1 : -1;
		if (sn.round_winner >= 0) sn.g.s[sn.round_winner].wins++;
		set_state(ST_ROUND);
		sn_sound(sn.round_winner >= 0 ? SN_SND_FANFARE : SN_SND_GAMEOVER, -1);
		return;
	}
	sn.rank = (sn.mode == SN_MODE_TITLE) ? -1 :
		ar_scores_rank(&sn.table, g->s[0].score);
	set_state(ST_OVER);
	zsfx_music_stop();
	sn_sound(SN_SND_GAMEOVER, -1);
}

/* One segment of every dead snake at a time, from the head: a long
 * snake goes faster, so no death takes more than two seconds. */
static bool dissolve_step(void) {
	int p;
	bool any = false;
	if (sn.dissolve_wait > 0) { sn.dissolve_wait--; return true; }
	sn.dissolve_wait = 2;
	for (p = 0; p < SN_PLAYERS; p++) {
		sn_snake_t *s = &sn.g.s[p];
		int k, n;
		if (!s->active || s->alive || s->len == 0) continue;
		n = 1 + s->len / 30;
		for (k = 0; k < n && s->len; k++) {
			uint16_t c = sn_pop_head(&sn.g, p);
			if (c != SN_NONE) sn_draw_pop(c, p);
		}
		if (sn.st != ST_TITLE) sn_sound(SN_SND_POP, -1);
		any = true;
	}
	return any;
}

/* The autopilot plans once per move, when one starts: it needs the
 * cell the head is entering, and a plan per frame would be the same
 * plan sixty times. */
static void autopilot_on_moves(void) {
	int i;
	for (i = 0; i < sn.g.nev; i++)
		if (sn.g.ev[i].type == SN_EV_MOVE)
			sn_autopilot(&sn.g, sn.g.ev[i].p);
}

static void game_step(void) {
	int i;
	sn_step(&sn.g);
	sn_draw_events();
#ifdef SN_AUTOPLAY
	/* The host renders: the autopilot plays, so the pictures show a
	 * game in progress rather than a snake driving into a wall -- until
	 * the script presses K, and it does. */
	if (!autoplay_off) autopilot_on_moves();
#endif
	for (i = 0; i < sn.g.nev; i++) {
		const sn_event_t *e = &sn.g.ev[i];
		if (e->type == SN_EV_SPEEDUP) music_follow();
		if (e->type == SN_EV_LEVEL_DONE && sn.st == ST_PLAY) {
			set_state(ST_LEVEL);
			sn_draw_celebrate(120);
			sn_sound(SN_SND_LEVEL, -1);
		}
	}
	if (sn.g.over && sn.st == ST_PLAY) {
		set_state(ST_DYING);
		sn.dissolve_wait = 20;
	}
}

static void update(uint32_t dt) {

	uint32_t f;

	for (f = 0; f < dt; f++) {

		sn.st_frames++;

		switch (sn.st) {

		case ST_TITLE:
			if (sn.g.s[0].alive) {
				sn_step(&sn.g);
				sn_draw_events();
				autopilot_on_moves();
			} else if (!dissolve_step() && ar_parts_live() == 0) {
				/* the demo snake starts over */
				sn_new(&sn.g, SN_MODE_TITLE, 0, ar_rand_below);
				ar_dirty(0, SN_PLAY_Y, AR_W, AR_H - SN_PLAY_Y);
			}
			break;

		case ST_READY:
			if (sn.st_frames >= 90) set_state(ST_PLAY);
			break;

		case ST_PLAY:
			game_step();
			break;

		case ST_DYING:
			if (!dissolve_step() && sn.st_frames > 60) after_death();
			break;

		case ST_LEVEL:
			if (sn.st_frames >= 150) begin(SN_MODE_MAZE, sn.g.level + 1, true);
			break;

		case ST_ROUND:
			dissolve_step();
			if (sn.st_frames >= 240) {
				if (sn.g.s[0].wins >= SN_WINS_NEEDED ||
				    sn.g.s[1].wins >= SN_WINS_NEEDED)
					title();
				else
					begin(SN_MODE_VERSUS, 0, true);
			}
			break;

		default:
			break;
		}
	}

	/* The menu's held-key repeat. The press itself moved the cursor
	 * (handle()); this is only the repeat after a pause. */
	if (sn.st == ST_TITLE) {
		uint32_t h = ar_any_held();
		int d = 0;
		if (h & (AR_BTN_UP | AR_BTN_DOWN)) {
			menu_hold += (int)dt;
			if (menu_hold >= 24) {
				menu_hold -= 7;
				d = (h & AR_BTN_UP) ? -1 : 1;
			}
		} else {
			menu_hold = 0;
		}
		if (d) menu_move(d);
	}
}

int main(void) {

	uint32_t dt = 1;
	extern const uint16_t sn_base_palette[16];
	extern void sn_mono_inks(ar_ink_t *ink);

	memcpy(desc.palette, sn_base_palette, sizeof(desc.palette));
	sn_mono_inks(desc.ink);

	printf("snake: the classic, in a window or full screen (F11)\n");

	/* in colour in its window too, while it is focused and uncovered,
	 * on a board with the colour window (docs/color.md) */
	ar_window_colour(true);
	if (!ar_init(&desc)) return 1;

	if (!sn_draw_init())
		printf("snake: out of sprite memory\n");

	sn.menu = MENU_CLASSIC;
	printf("snake: skill %s\n", ar_skill_name(ar_skill()));
	title();

	while (!quit) {
		ar_event_t e;
		while (ar_poll(&e)) handle(&e);
		if (quit) break;

		update(dt);

		sn_draw_dirty_snakes();
		if (sn.hud_dirty) {
			ar_dirty(0, 0, AR_W, SN_PLAY_Y);
			sn.hud_dirty = false;
		}
		sn_draw_frame(dt);

		dt = ar_frame();
	}

	ar_shutdown();
	return 0;
}
