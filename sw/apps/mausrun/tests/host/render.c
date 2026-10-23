/*
 * mausrun on a build host, driven by a script, every screen written out
 * as a picture. make -C sw/apps/mausrun render
 *
 * The game is mausrun.c, unchanged (mr_wrap_game.c); the machine is
 * mr_host.c. The pictures are what the display would show: the 320x240
 * view, in the four colours or in monochrome.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mr_host.h"

int game_main(void);
const char *mrh_progress(void);
extern int mrh_sfx;

#define DOWN(f, k)  { (f), MRH_DOWN, (k), 0, NULL }
#define UP(f, k)    { (f), MRH_UP, (k), 0, NULL }
#define TAP(f, k)   { (f), MRH_TAP, (k), 0, NULL }
#define SHOT(f, n)  { (f), MRH_SHOT, 0, 0, (n) }

static const mrh_step_t script[] = {
	SHOT(60, "01_title"),
	TAP(70, K_DOWN_U),                  /* to the skill (no CONTINUE yet) */
	SHOT(80, "02_title_skill"),
	TAP(90, K_RIGHT_U),                 /* EASY -> NORMAL */
	SHOT(100, "03_title_normal"),
	TAP(110, K_LEFT_U),                 /* back to EASY */
	TAP(120, K_UP_U),                   /* to PLAY */
	TAP(130, K_ENTER_U),
	SHOT(150, "04_intro"),
	TAP(170, K_SPACE_U),                /* go */
	SHOT(200, "05_play_start"),
	/* run right, jumping when the hook says so (it holds Right) */
	SHOT(400, "06_play"),
	TAP(500, K_P_U),
	SHOT(510, "07_pause"),
	TAP(520, K_P_U),
	SHOT(700, "08_play_later"),
	TAP(705, K_C_U),                    /* colour off */
	SHOT(720, "09_mono"),
	TAP(725, K_C_U),                    /* and on */
	TAP(740, 0x3A),                     /* F1: the help, paused */
	SHOT(750, "09b_help"),
	TAP(760, 0x3A),                     /* and on */
	TAP(780, 0x43),                     /* F10: colour off, as C */
	SHOT(790, "09c_f10_mono"),
	TAP(795, 0x43),
	{ 20000, MRH_END, 0, 0, NULL },
};

/* `render_mausrun out garden`: level 3 from CONTINUE, for round 3's
 * things -- the pond, the frogs, the sleeping cat, the owl, the
 * power-ups, the particles; the hook takes the pictures as they come */
static const mrh_step_t script_garden[] = {
	TAP(60, K_DOWN_U),                  /* to CONTINUE: LEVEL 4 */
	SHOT(70, "g00_title_continue4"),
	TAP(80, K_ENTER_U),
	SHOT(110, "g01_intro"),
	TAP(120, K_SPACE_U),
	SHOT(180, "g02_garden"),
	TAP(330, K_C_U),                    /* the pond in monochrome */
	SHOT(345, "g03b_pond_mono"),
	TAP(350, K_C_U),
	{ 20000, MRH_END, 0, 0, NULL },
};
static bool garden;

/* An autopilot for the right arrow: jump when the floor ends or a wall
 * comes, both by the map; the hook reads the game's own state. */
#include "../../level.h"
#include "../../phys.h"
extern mr_actor_t mr_host_player(void);
extern int mr_host_state(void);
extern int mr_host_level(void);
extern int mr_host_lives(void);
extern int mr_host_cat_ahead(int32_t x, int32_t y);
extern bool mr_host_cat_near(int32_t x, int32_t y);
extern int mr_host_cheese(void);
extern int mr_host_hurt(void);
extern int mr_host_power(int i);
extern int mr_host_parts(void);
extern int mr_host_foe_state(int i);
extern void mr_host_give(int pw);
extern bool mr_host_drop_on_cat(void);
extern void mr_host_goto(int n);
extern bool mr_host_drop_on_plank(void);
extern int mr_host_riding(void);
extern int32_t mr_host_plank_x(void);
extern uint32_t mr_host_blits(void);
extern bool mr_host_colour(void);
static void themes_hook(uint32_t frame, mr_actor_t p, int st);
static bool themes;
static int themes_bad;
void mrh_seed_progress(const char *text);
static void garden_hook(uint32_t frame, mr_actor_t p, int st);
void mr_host_key(int usage, int down);

static int phase;
static uint32_t phase_at;

void mrh_hook(uint32_t frame) {
	mr_actor_t p = mr_host_player();
	int st = mr_host_state();
	(void)p;
	/* MRH_TRACE=1: where the mouse is, twice a second */
	if (getenv("MRH_TRACE") && frame % 30 == 0) printf("trace f%u st %d x %d y %d lives %d ground %d\n", (unsigned)frame, st, (int)TOPX(p.x), (int)TOPX(p.y), mr_host_lives(), p.on_ground);
	/* in play: jump at the edges and walls ahead */
	if (st == 2 /* ST_PLAY */) {
		int32_t x = TOPX(p.x), y = TOPX(p.y);
		int32_t ahead = x + MR_PLAYER_W + 1;
		bool floor_gone = !mr_tile_solid(mr_tile_at(ahead, y + MR_PLAYER_H + 2));
		bool wall = mr_tile_solid(mr_tile_at(ahead + 8, y + MR_PLAYER_H - 2)) ||
			mr_tile_at(ahead + 6, y + MR_PLAYER_H - 2) == MR_TILE_THORNS;
		int cat = mr_host_cat_ahead(x, y);
		/* a cat ahead: over it */
		/* over a cat only where the landing is ground */
		bool land_ok = mr_tile_solid(mr_tile_at(x + 64 + MR_PLAYER_W / 2, y + MR_PLAYER_H + 2));
		/* from phase 3 on it forgets pits: hearts lost, GAME OVER */
		if (phase >= 3) floor_gone = false;
		bool jump = p.on_ground && (floor_gone || wall || (cat == 2 && land_ok)) &&
			(frame & 1) == 0;
		/* in the water: a stroke now and then */
		if (p.in_water) jump = (frame & 15) == 0;
		mr_host_key(0x2C, jump);
		/* a cat between us and a pit: back off until there is room to
		 * jump it */
		{
			bool retreat = mr_host_cat_near(x, y) && !land_ok && p.on_ground;
			mr_host_key(0x4F, !retreat);
			mr_host_key(0x50, retreat);
		}
	}
	if (garden) { garden_hook(frame, p, st); return; }
	if (themes) { themes_hook(frame, p, st); return; }
	switch (phase) {
	case 0:
		if (st == 5 /* ST_CLEAR */) {
			phase_at = frame;
			phase = 1;
			mrh_snapshot("10_clear");
		}
		break;
	case 1:
		if (frame == phase_at + 120) { mrh_snapshot("11_clear_counted"); mr_host_key(0x2C, 1); }
		if (frame == phase_at + 122) mr_host_key(0x2C, 0);
		if (st == 1 /* ST_INTRO */) { mrh_snapshot("12_intro_level2"); phase = 2; phase_at = frame; }
		break;
	case 2:
		if (frame == phase_at + 10) mr_host_key(0x2C, 1);
		if (frame == phase_at + 12) mr_host_key(0x2C, 0);
		if (frame == phase_at + 300) mrh_snapshot("13_level2");
		/* let the mouse fall in pits: lose the hearts, see GAME OVER */
		if (frame == phase_at + 400) phase = 3;
		break;
	case 3:
		/* walk left into... just stand still and wait for a cat */
		if (st == 4 /* ST_DYING */ && mr_host_lives() == 1) { mrh_snapshot("14_dying"); phase = 4; }
		if (frame > phase_at + 6000) phase = 5;
		break;
	case 4:
		if (st == 6 /* ST_OVER */) { mrh_snapshot("15_game_over"); phase = 5; phase_at = frame; }
		break;
	case 5:
		if (frame == phase_at + 60) mr_host_key(0x29, 1);     /* Esc: menu */
		if (frame == phase_at + 62) mr_host_key(0x29, 0);
		if (st == 0 && frame > phase_at + 64 && frame < phase_at + 100) {
			mrh_snapshot("16_title_continue");
			phase = 6;
			phase_at = frame;
		}
		break;
	case 6:
		if (frame == phase_at + 20) mr_host_key(0x29, 1);     /* Esc: quit */
		if (frame == phase_at + 22) mr_host_key(0x29, 0);
		break;
	}
}

static void garden_hook(uint32_t frame, mr_actor_t p, int st) {
	static int shot;
	static uint32_t at;
	int32_t x = TOPX(p.x);
	int i;
	if (st != 2 && st != 5 && shot < 7) return;
	switch (shot) {
	case 0: if (x > 440) { mrh_snapshot("g03_pond"); shot++; } break;
	case 1:
		if (x > 700) {
			mr_host_give(0); mr_host_give(1); mr_host_give(4);   /* feather, magnet, bubble */
			at = frame;
			shot++;
		}
		break;
	case 2: if (frame == at + 20) { mrh_snapshot("g04_powers"); shot++; at = 0; } break;
	case 3: if (x > 1180) { mrh_snapshot("g05_sleeper"); shot++; } break;
	case 4: if (x > 1460) { mrh_snapshot("g06_owl"); shot++; } break;
	case 5:
		if (x > 1640 && !at) { at = mr_host_drop_on_cat() ? frame : 0; if (at) printf("garden: dropped at %u\n", (unsigned)frame); }
		if (at && frame < at + 20) mr_host_key(0x4F, 0);   /* straight down */
		if (at && frame < at + 40 && getenv("MRH_TRACE")) { printf("drop f%u y %d vy %d st", (unsigned)frame, (int)TOPX(p.y), (int)p.vy); for (i = 0; i < 6; i++) printf(" %d", mr_host_foe_state(i)); printf("\n"); }
		for (i = 0; i < 24; i++)
			if (mr_host_foe_state(i) == 1 /* FS_DIZZY */) {
				if (!at || frame < at + 30) continue;
				mrh_snapshot("g07_dizzy");
				at = 0;
				shot++;
				break;
			}
		break;
	case 6: if (st == 5) { at = frame; shot++; } break;
	case 7:
		if (frame == at + 30) mrh_snapshot("g08_clear");
		if (frame == at + 40) printf("garden: cheese %d, parts %d\n", mr_host_cheese(), mr_host_parts());
		if (frame == at + 160) mrh_snapshot("g08b_clear_stars");
		/* Space: on to level 5 (the count is done by now) */
		if (frame == at + 170) mr_host_key(0x2C, 1);
		if (frame == at + 172) mr_host_key(0x2C, 0);
		if (frame == at + 200) mrh_snapshot("g09_next_level");
		if (frame == at + 210 || frame == at + 250) mr_host_key(0x29, 1);
		if (frame == at + 212 || frame == at + 252) mr_host_key(0x29, 0);
		break;
	}
	if (getenv("MRH_TRACE") && (frame % 30 == 0 || (getenv("MRH_FROM") && frame >= (uint32_t)atoi(getenv("MRH_FROM")) && frame < (uint32_t)atoi(getenv("MRH_FROM")) + 60)))
		printf("garden f%u x %d y %d vy %d g %d lives %d water %d shot %d\n", (unsigned)frame,
			(int)x, (int)TOPX(p.y), (int)p.vy, p.on_ground, mr_host_lives(), p.in_water, shot);
}

/* `render_mausrun out themes`: every level a picture after a few
 * seconds' running, a ride on a plank (checked), and the results with
 * their stars */
static const mrh_step_t script_themes[] = {
	{ 20000, MRH_END, 0, 0, NULL },
};

static void themes_hook(uint32_t frame, mr_actor_t p, int st) {
	static int lv = 0, step;
	static uint32_t at;
	static int32_t px0, qx0;
	char name[32];
	if (frame < 60) return;
	if (lv > 8) {
		if (frame == at + 80) mrh_snapshot("t9_title_stars");
		if (frame == at + 10 || frame == at + 50 || frame == at + 90) mr_host_key(0x29, 1);
		if (frame == at + 12 || frame == at + 52 || frame == at + 92) mr_host_key(0x29, 0);
		return;
	}
	if (lv == 0 || (step == 3 && frame >= at + 4)) {
		at = frame;
		if (++lv > 8) return;     /* Esc, Esc: the menu, then quit */
		mr_host_goto(lv);
		step = 0;
		return;
	}
	/* the blits a frame, in play (the figure docs/mausrun.md quotes) */
	{
		static uint64_t sum;
		static uint32_t n, mx;
		extern uint32_t mr_prof[8];
		static uint64_t part[5];
		if (step == 0 && st == 2 && frame > at + 20) {
			uint32_t b = mr_host_blits();
			part[0] += mr_prof[1] - mr_prof[0];   /* far layers */
			part[1] += mr_prof[2] - mr_prof[1];   /* earth, waterfalls */
			part[2] += mr_prof[3] - mr_prof[2];   /* tiles */
			part[3] += mr_prof[4] - mr_prof[3];   /* birds, actors, particles */
			part[4] += b - mr_prof[4];            /* status line */
			sum += b;
			n++;
			if (b > mx) mx = b;
		}
		if (step == 0 && frame == at + 200 && n) {
			printf("themes: level %d %s: %u blits a frame on average, %u at most\n", lv,
				mr_host_colour() ? "colour" : "mono", (unsigned)(sum / n), (unsigned)mx);
			printf("themes:   far %u, earth %u, tiles %u, actors %u, status %u\n",
				(unsigned)(part[0] / n), (unsigned)(part[1] / n), (unsigned)(part[2] / n),
				(unsigned)(part[3] / n), (unsigned)(part[4] / n));
			memset(part, 0, sizeof(part));
			sum = 0; n = 0; mx = 0;
		}
	}
	if (getenv("MRH_MONO") && frame == 61) mr_host_key(0x06, 1);
	if (getenv("MRH_MONO") && frame == 63) mr_host_key(0x06, 0);
	switch (step) {
	case 0:   /* run a while with the autopilot */
		if (frame == at + 200) {
			snprintf(name, sizeof name, "t%d_level%d", lv, lv);
			mrh_snapshot(name);
			step = mr_host_drop_on_plank() ? 1 : 3;
			at = frame;
		}
		/* not while it is coming back from a pit: wait for play */
		if (frame == at + 199 && (st != 2 || !p.alive)) at++;
		if (0) {
		}
		break;
	case 1:   /* on the plank? held still, it should carry the mouse */
		mr_host_key(0x4F, 0);
		mr_host_key(0x50, 0);
		mr_host_key(0x2C, 0);
		if (lv == 8 && getenv("MRH_TRACE")) printf("lv8 f%u x %d y %d vy %d g %d ride %d plank %d\n", (unsigned)(frame - at), (int)TOPX(mr_host_player().x), (int)TOPX(mr_host_player().y), (int)mr_host_player().vy, mr_host_player().on_ground, mr_host_riding(), (int)mr_host_plank_x());
		if (frame == at + 40) {
			px0 = TOPX(mr_host_player().x);
			qx0 = mr_host_plank_x();
		}
		if (frame == at + 70) {
			int32_t dp = TOPX(mr_host_player().x) - px0, dq = mr_host_plank_x() - qx0;
			bool ok = mr_host_riding() == 0 && dp == dq && dq != 0;
			printf("themes: level %d (st %d, y %d): riding %d, mouse moved %d, plank %d: %s\n", lv, st, (int)TOPX(mr_host_player().y),
				mr_host_riding(), (int)dp, (int)dq, ok ? "carried" : "NOT CARRIED");
			if (!ok) themes_bad++;
			snprintf(name, sizeof name, "t%d_plank", lv);
			mrh_snapshot(name);
			step = 3;
			at = frame;
		}
		break;
	}
}

int main(int argc, char **argv) {
	int r;
	if (argc > 2 && strcmp(argv[2], "themes") == 0) {
		themes = true;
		mrh_seed_progress("skill easy\nunlocked 8\nstars 1 7\nstars 2 3\n");
		mrh_run(script_themes, argv[1]);
		r = game_main();
		printf("render: %d pictures, %u frames\n", mrh_shots(), (unsigned)mrh_frame());
		return r || themes_bad;
	}
	if (argc > 2 && strcmp(argv[2], "garden") == 0) {
		garden = true;
		mrh_seed_progress("skill easy\nunlocked 4\n");
		mrh_run(script_garden, argv[1]);
	} else
	mrh_run(script, argc > 1 ? argv[1] : "out");
	r = game_main();
	printf("render: %d pictures, %u frames, %d effects; last said \"%s\" (%d lines)\n",
		mrh_shots(), (unsigned)mrh_frame(), mrh_sfx, mrh_said, mrh_says);
	printf("render: progress.txt:\n%s", mrh_progress());
	return r;
}
