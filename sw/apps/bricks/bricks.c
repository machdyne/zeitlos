/*
 * bricks -- the brick-breaking game: keep the ball in play with the
 * paddle and break every brick that can be broken. See
 * docs/bricks_app.md.
 *
 *   > run bricks
 *
 * Twelve rounds that come round again faster, capsules that change the
 * paddle (longer, catching, firing) or the balls (three of them, or
 * slower), in a 320x240 window or full screen with F11 -- in sixteen
 * colours where the board has them. The mouse, the arrow keys or a
 * gamepad move the paddle. Built on the arcade runtime
 * (sw/common/games/zarcade.h) and its sound engine (zsfx.h).
 *
 * This file is the screens, the controls and the loop. The rules are
 * br_game.c, the picture br_draw.c, the sounds and tunes br_sound.c.
 */

#include <stdio.h>
#include <string.h>

#include "../../common/games/zarcade.h"
#include "../../common/games/zsfx.h"

#include "bricks.h"
#include "br_sound.h"

br_app_t br;

static const char help_text[] =
	"Keep the ball in play with the paddle and break the bricks. Where "
	"the ball lands on the paddle decides where it goes: the middle "
	"sends it up, the ends out wide. Silver bricks take more than one "
	"hit; gold ones never break. A round is clear when every other "
	"brick is gone.\n\n"
	"Mouse\tMove the paddle; click to launch or fire\n"
	"Left/Right\tMove the paddle\n"
	"Space, Up\tLaunch the ball, fire the laser\n"
	"P\tPause (Start on a pad)\n"
	"F11\tFull screen, or back to the window\n"
	"C\tFull screen: colour on or off\n"
	"M\tMusic on or off\n\n"
	"Skill: on EASY the ball is slow, the paddle long, there are five "
	"paddles and a glowing floor under the paddle that sends the ball "
	"back once each time it is served. NORMAL is the classic game, HARD "
	"is faster. Left and Right on the top line of the menu change it.\n\n"
	"Catch the capsules that fall from broken bricks: E a longer "
	"paddle, M three balls, S slower balls, C a paddle that catches, "
	"L a paddle that fires, P another paddle. A new paddle every "
	"50,000 points (the first at 20,000).";

static ar_desc_t desc = {
	"Bricks", "Bricks help", help_text, "bricks_app",
	{ 0 },
	{ { { 0 } } },
	br_paint, NULL,
	false,
	NULL
};

static bool quit;
static int menu_hold;

#ifdef BR_AUTOPLAY
static bool autoplay_off;
#endif

/* ===================================================================
 * small things
 * =================================================================== */

/* The music follows the rounds: calm at the start, the whole band only
 * later -- on easy at half the pace (zsfx_music_intensity()). */
static void music_follow(void) {
	int lv = br.g.level ? br.g.level : 1, i = lv - 1;
	if (ar_skill() == AR_SKILL_EASY) i = (lv - 1) / 2;
	else if (ar_skill() == AR_SKILL_HARD) i = lv + 1;
	zsfx_music_intensity(i);
}

static void load_table(void) {
	ar_scores_load(ar_skill_table(0), &br.table);
	br.best = br.table.n ? br.table.e[0].score : 0;
}

static void dirty_panel(void) {
	ar_rect_t r;
	if (br_panel_rect(&r)) ar_dirty(r.x, r.y, r.w, r.h);
}

static void set_state(br_state_t st) {
	dirty_panel();
	ar_text_input(st == ST_NAME);
	br.st = st;
	br.st_frames = 0;
	dirty_panel();
}

/* ===================================================================
 * screens
 * =================================================================== */

static void title(void) {
	br_draw_reset();
	load_table();
	br.rank = -1;
	set_state(ST_TITLE);
	ar_dirty_all();
	zsfx_music_intensity(3);
	zsfx_music_play(&br_song_title);
}

static void show_scores(int rank) {
	load_table();
	br.rank = rank;
	set_state(ST_SCORES);
	ar_dirty_all();
}

/* A round begins: the bricks come down a row at a time. */
static void ready(bool quick) {
	br.ready_quick = quick;
	br.ready_rows = quick ? BR_ROWS : 0;
	set_state(ST_READY);
	if (!quick) {
		br_draw_level(br.g.level);
		ar_dirty_all();
	}
	br_sound(BR_SND_READY, -1);
}

static void start_game(void) {
	load_table();
	br_draw_reset();
	br_new_skill(&br.g, br.start_level, ar_skill(), ar_rand_below);
	br.key_speed = 0;
	br.shown_score = 0xffffffffu;
	ready(false);
	music_follow();
	zsfx_music_play(&br_song_game);
}

static void pause(bool on) {
	if (on && br.st == ST_PLAY) {
		set_state(ST_PAUSE);
		zsfx_suspend(true);
	} else if (!on && br.st == ST_PAUSE) {
		set_state(ST_PLAY);
		zsfx_suspend(false);
	}
}

static void game_over(void) {
	br.rank = br.g.score ? ar_scores_rank(&br.table, br.g.score) : -1;
	set_state(ST_OVER);
	zsfx_music_stop();
	br_sound(BR_SND_OVER, -1);
}

static void name_letter(int delta) {
	char c = br.name[br.name_pos];
	int i = (c >= 'A' && c <= 'Z') ? c - 'A' : 26;
	i = (i + delta + 27) % 27;
	br.name[br.name_pos] = (char)(i == 26 ? '-' : 'A' + i);
	br_sound(BR_SND_LETTER, -1);
	dirty_panel();
}

static void name_done(void) {
	br.name[3] = 0;
	load_table();
	br.rank = ar_scores_insert(&br.table, br.g.score, br.name);
	if (!ar_scores_save(ar_skill_table(0), &br.table))
		printf("bricks: could not save the high scores\n");
	br_sound(BR_SND_SELECT, -1);
	show_scores(br.rank);
}

static void after_over(void) {
	if (br.rank >= 0) {
		memcpy(br.name, "AAA", 4);
		br.name_pos = 0;
		set_state(ST_NAME);
	} else {
		show_scores(-1);
	}
}

static const char *const menu_say[MENU_ITEMS] = { "Skill", "Play", "High scores" };

static void say_menu(void) {
	char b[48];
	strcpy(b, menu_say[br.menu]);
	if (br.menu == MENU_SKILL) {
		strcat(b, ", ");
		strcat(b, ar_skill_spoken(ar_skill()));
	}
	ar_say(b);
}

static void menu_move(int d) {
	br.menu = (br.menu + d + MENU_ITEMS) % MENU_ITEMS;
	br_sound(BR_SND_MENU, -1);
	dirty_panel();
	say_menu();
}

static void skill_change(int d) {
	ar_skill_set((ar_skill() + d + AR_SKILLS) % AR_SKILLS);
	load_table();
	br_sound(BR_SND_MENU, -1);
	dirty_panel();
	br_draw_hud_dirty();
	ar_say(ar_skill_spoken(ar_skill()));
}

static void menu_go(void) {
	if (br.menu == MENU_SKILL) { skill_change(1); return; }
	br_sound(BR_SND_SELECT, -1);
	if (br.menu == MENU_SCORES) show_scores(-1);
	else start_game();
}

/* ===================================================================
 * input
 * =================================================================== */

/* The button in play: launch, fire -- or, while the bricks are still
 * arriving, get on with it. */
static void fire(void) {
	if (br.st == ST_READY && br.ready_rows >= BR_ROWS) {
		set_state(ST_PLAY);
		return;
	}
	if (br.st == ST_PLAY) br_fire(&br.g);
}

static bool is_go(const ar_event_t *e) {
	return e->type == AR_EV_BUTTON && e->pressed &&
		(e->key == AR_BTN_A || e->key == AR_BTN_START);
}

static void mouse(const ar_event_t *e) {
	bool left = (e->buttons & 1) != 0;
	bool click = left && !br.mouse_left;
	br.mouse_left = left;
	if (!e->inside) return;

	switch (br.st) {
	case ST_READY:
	case ST_PLAY:
		br_paddle_centre(&br.g, e->x);
		if (click) fire();
		break;
	case ST_TITLE:
		if (click) menu_go();
		break;
	case ST_SCORES:
		if (click) title();
		break;
	case ST_PAUSE:
		if (click) pause(false);
		break;
	case ST_OVER:
		if (click && br.st_frames > 40) after_over();
		break;
	default:
		break;
	}
}

static void handle(const ar_event_t *e) {

	if (e->type == AR_EV_QUIT) { quit = true; return; }
	if (e->type == AR_EV_SUSPEND) { pause(true); return; }
	if (e->type == AR_EV_MOUSE) { mouse(e); return; }

	if (e->type == AR_EV_KEY && e->pressed) {
		uint32_t k = e->key;
#ifdef BR_AUTOPLAY
		if (k == 'K') { autoplay_off = true; return; }
		if (k == 'J') { autoplay_off = false; return; }
#endif
		if (br.st == ST_NAME) {
			if (k >= 'a' && k <= 'z') k -= 'a' - 'A';
			if (k >= 'A' && k <= 'Z') {
				br.name[br.name_pos] = (char)k;
				if (br.name_pos < 2) br.name_pos++;
				br_sound(BR_SND_LETTER, -1);
				dirty_panel();
			} else if (k == 8 && br.name_pos > 0) {
				br.name_pos--;
				dirty_panel();
			}
			return;
		}
		if (k == 'p' || k == 'P') {
			if (br.st == ST_PLAY) pause(true);
			else if (br.st == ST_PAUSE) pause(false);
			return;
		}
		if (k == 0x1b) {                 /* Esc: in the window only */
			if (br.st == ST_PLAY) pause(true);
			else if (br.st == ST_PAUSE || br.st == ST_SCORES ||
			         br.st == ST_READY) {
				zsfx_suspend(false);
				zsfx_music_stop();
				title();
			}
			return;
		}
		return;
	}

	if (e->type != AR_EV_BUTTON || !e->pressed) return;

	switch (br.st) {

	case ST_TITLE:
		if (e->key == AR_BTN_UP || e->key == AR_BTN_DOWN) {
			menu_move(e->key == AR_BTN_UP ? -1 : 1);
			menu_hold = 0;
		} else if ((e->key == AR_BTN_LEFT || e->key == AR_BTN_RIGHT) &&
		           br.menu == MENU_SKILL) {
			skill_change(e->key == AR_BTN_RIGHT ? 1 : -1);
		} else if ((e->key == AR_BTN_LEFT || e->key == AR_BTN_RIGHT) &&
		           br.menu == MENU_PLAY) {
			br.start_level += (e->key == AR_BTN_RIGHT) ? 1 : -1;
			if (br.start_level < 1) br.start_level = br_levels();
			if (br.start_level > br_levels()) br.start_level = 1;
			br_sound(BR_SND_MENU, -1);
			dirty_panel();
		} else if (is_go(e)) {
			menu_go();
		}
		break;

	case ST_SCORES:
		if (is_go(e) || e->key == AR_BTN_B) title();
		break;

	case ST_READY:
	case ST_PLAY:
		if (e->key == AR_BTN_A || e->key == AR_BTN_B || e->key == AR_BTN_UP ||
		    e->key == AR_BTN_X || e->key == AR_BTN_Y)
			fire();
		else if (e->key == AR_BTN_START && br.st == ST_PLAY)
			pause(true);
		break;

	case ST_PAUSE:
		if (e->key == AR_BTN_START || e->key == AR_BTN_A) pause(false);
		break;

	case ST_OVER:
		if (br.st_frames > 40 && is_go(e)) after_over();
		break;

	case ST_NAME:
		switch (e->key) {
		case AR_BTN_UP:    name_letter(1); break;
		case AR_BTN_DOWN:  name_letter(-1); break;
		case AR_BTN_LEFT:  if (br.name_pos > 0) { br.name_pos--; dirty_panel(); } break;
		case AR_BTN_RIGHT: if (br.name_pos < 2) { br.name_pos++; dirty_panel(); } break;
		case AR_BTN_A:
			if (br.name_pos < 2) { br.name_pos++; dirty_panel(); }
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

/* The paddle under the arrow keys or a d-pad: it picks up speed while
 * held, from 3 to 7 pixels a frame. */
static void held_controls(void) {
	uint32_t h = ar_any_held();
	int d = 0;
	if (h & AR_BTN_LEFT) d--;
	if (h & AR_BTN_RIGHT) d++;
	if (d == 0) { br.key_speed = 0; return; }
	if (br.key_speed < 12) br.key_speed++;
	br_paddle_move(&br.g, d * (3 + br.key_speed / 3));
}

#ifdef BR_AUTOPLAY
/* The host renders: the autopilot plays, at a keyboard's speed. */
static void autoplay(void) {
	bool f;
	int t, d;
	if (autoplay_off) return;
	t = br_ai_target(&br.g, &f);
	d = t - (br.g.px + br.g.pw / 2);
	if (d > 6) d = 6;
	if (d < -6) d = -6;
	br_paddle_move(&br.g, d);
	if (f) fire();
}
#endif

/* ===================================================================
 * the frame
 * =================================================================== */

static void after_events(void) {
	br_game_t *g = &br.g;
	int i;

	br_draw_events();

	if (br.st == ST_PLAY) {
		for (i = 0; i < g->nev; i++) {
			if (g->ev[i].type == BR_EV_LOST) {
				set_state(ST_LOST);
				br_sound(BR_SND_LOST, g->ev[i].x);
				br_draw_lost();
			} else if (g->ev[i].type == BR_EV_CLEAR) {
				set_state(ST_CLEAR);
				zsfx_music_stop();
				br_sound(BR_SND_CLEAR, -1);
			}
		}
	}
	br_clear_events(g);
}

static void update(uint32_t dt) {

	uint32_t f;

	for (f = 0; f < dt; f++) {

		br.st_frames++;

		switch (br.st) {

		case ST_READY:
			held_controls();
			if (br.ready_rows < BR_ROWS && br.st_frames % READY_ROW_FRAMES == 0) {
				int c, any = 0;
				for (c = 0; c < BR_COLS; c++) {
					if (!br.g.brick[br.ready_rows][c]) continue;
					any = 1;
					/* a glint as each brick lands */
					if ((c & 3) == (br.ready_rows & 3))
						ar_parts_burst(BR_BRICK_X(c) + 8, BR_BRICK_Y(br.ready_rows) + 4,
							1, BR_C_WHITE, 1, 120, 8, 0);
				}
				ar_dirty(BR_FX0, BR_BRICK_Y(br.ready_rows), BR_FX1 - BR_FX0, BR_BH);
				if (any) br_sound(BR_SND_ROW, -1);
				br.ready_rows++;
				if (br.ready_rows == BR_ROWS) dirty_panel();
			}
			if (br.st_frames >= (uint32_t)(br.ready_quick ? READY_AGAIN : READY_FRAMES))
				set_state(ST_PLAY);
			break;

		case ST_PLAY:
#ifdef BR_AUTOPLAY
			autoplay();
#endif
			held_controls();
			br_step(&br.g);
			after_events();
			break;

		case ST_LOST:
			if (br.st_frames >= LOST_FRAMES) {
				if (br.g.over) {
					game_over();
				} else {
					br_serve(&br.g);
					ready(true);
				}
			}
			break;

		case ST_CLEAR:
			if (br.st_frames >= CLEAR_FRAMES) {
				br_draw_reset();
				br_next_level(&br.g);
				ready(false);
				music_follow();
				zsfx_music_play(&br_song_game);
			}
			break;

		default:
			break;
		}
	}

	/* menu repeat while held */
	if (br.st == ST_TITLE) {
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

	br_draw_hud_dirty();
}

int main(void) {

	uint32_t dt = 1;

	memcpy(desc.palette, br_base_palette, sizeof(desc.palette));
	br_mono_inks(desc.ink);

	printf("bricks: break them all, in a window or full screen (F11)\n");

	if (!ar_init(&desc)) return 1;
	if (!br_draw_init()) printf("bricks: out of sprite memory\n");

	br.start_level = 1;
	br.menu = MENU_PLAY;
	printf("bricks: skill %s\n", ar_skill_name(ar_skill()));
	title();

	while (!quit) {
		ar_event_t e;
		while (ar_poll(&e)) handle(&e);
		if (quit) break;
		/* whatever the input just did (a launch, a bolt), its effects */
		if (br.st == ST_PLAY) after_events();
		else br_clear_events(&br.g);

		update(dt);
		br_draw_frame(dt);
		dt = ar_frame();
	}

	ar_shutdown();
	return 0;
}
