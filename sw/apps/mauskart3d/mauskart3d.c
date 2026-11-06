/*
 * mauskart3d -- a kart race, the mouse in a matchbox (docs/mauskart3d.md).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 *   > run mauskart3d
 *
 * Three laps of the Meadow Ring against the cat, the frog and the owl,
 * in game mode on chunky VRAM, drawn by T2. ? boxes give a power-up
 * (mausrun's: shoes, a feather, the bell, a bubble, the magnet), cheese
 * a burst of speed, a spring a jump.
 *
 * -- the frame --
 *
 *   1. the sky: a gradient above the horizon, in picture coordinates;
 *   2. the grass (no depth test: everything is in front of it), the
 *      road, kerbs and posts ahead, display lists (track.c);
 *   3. the ? boxes and cheese near enough, the karts (karts.c);
 *   4. the HUD as lines (../mausrun3d/vfont.c).
 *
 * At 60 frames a second, locked; when frames run long, an even 30
 * (each frame two refreshes) for a while, never 1, 2, 1, 2.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zfont.h"
#include "../../common/games/zarcade.h"
#include "../../common/zgl/zgl.h"
#include "../mausrun/sound.h"
#include "../mausrun3d/world.h"
#include "../mausrun3d/vfont.h"
#include "mauskart3d.h"
#include "karts.h"

uint16_t mk_atan2(int32_t dx, int32_t dz);   /* track.c */

/* -- the view -- */

#define NEAR      14
#define FAR       640                /* world units: the draw distance at most */
#define FAR_MIN   384
#define CAM_DIST  44                 /* behind the kart */
#define CAM_H     18
#define PITCH_DEG 9
#define LAPS      3

/* -- driving (Q8 world units, a tick of 60 a second) -- */

#define TOP       (5 * Q + 64)       /* 5.25 world units a tick: 315 a second */
#define TOP_OFF   (TOP * 45 / 100)   /* on the grass */
#define BOOST_TOP (TOP * 135 / 100)
#define GRAVITY   (Q * 30 / 100)
#define K_RADIUS  (9 * Q)

enum { P_NONE, P_SHOES, P_FEATHER, P_BELL, P_BUBBLE, P_MAGNET, P_N };
static const char *const pname[P_N] = { "", "SHOES", "FEATHER", "BELL", "BUBBLE", "MAGNET" };

typedef struct {
	int32_t x, z, y;                 /* Q8 */
	int32_t v, vy;                   /* speed along yaw, Q8 a tick; up */
	uint16_t yaw;
	int16_t spin;                    /* a stun's spin, turn16 a tick */
	int seg;
	int32_t across;                  /* world units right of the centreline */
	int lap;                         /* laps done: 0 behind the line at the start */
	bool half;                       /* past halfway this lap */
	int32_t prog;                    /* for the places: lap, segment, along */
	int place;                       /* 1..4 */
	uint8_t item;
	int16_t boost, stun, shield, magnet;
	int16_t item_wait;               /* the AI's: ticks before it uses one */
	int16_t lane;                    /* the AI's line, world units across */
	int16_t skill;                   /* the AI's: percent of the track's speed */
	uint32_t finish;                 /* ticks at the line, 0 not yet */
	uint32_t lap_start, best;
	bool ai;
} kart_t;

static kart_t kart[RACERS];
static int32_t cam_x, cam_y, cam_z;   /* world units */
static uint16_t cam_yaw;
static int32_t far_now = FAR;
static uint32_t ticks, race_ticks;
static int cheese_got, bell_flash;
static uint32_t record;              /* the track's best lap, ticks; 0 none */
static bool new_record;

/* the best lap of each track: the scores table, mode the track, the
 * score 1,000,000 less the ticks (higher is better there) */
static void record_load(void) {
	ar_table_t t;
	ar_scores_load(tk_selected(), &t);
	record = t.n ? 1000000u - t.e[0].score : 0;
}

static void record_try(uint32_t lap) {
	ar_table_t t;
	if (!lap) return;
	ar_scores_load(tk_selected(), &t);
	if (ar_scores_insert(&t, 1000000u - lap, "MAU") == 0) {
		ar_scores_save(tk_selected(), &t);
		new_record = true;
		record = lap;
	}
}
static uint32_t seed = 12345;
static bool autopilot;               /* the host tests: the AI drives the mouse */

enum { G_TITLE, G_COUNT, G_RACE, G_DONE };
static int gstate = G_TITLE;
static int gtimer;

static uint32_t rnd(void) {
	seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
	return seed;
}

static bool chunky, sound_on;

static void sound(int which) {
	if (sound_on) snd_play(which, -1);
}

/* -- chunky, or a message -- */

static void paint(void *user, const ar_rect_t *r) {
	(void)user;
	ar_fill(r->x, r->y, r->w, r->h, 0);
	ar_text(40, 100, "MAUSKART NEEDS GAME MODE", 1, &z_font_5x8, 1);
	ar_text(40, 116, "IN COLOUR: PRESS F11 (AND C)", 1, &z_font_5x8, 1);
	ar_text(40, 132, "IT IS MADE FOR T2", 1, &z_font_5x8, 1);
}

static const char help_text[] =
	"Mauskart: three laps of the Meadow Ring in a matchbox, against "
	"the cat, the frog and the owl.\n\n"
	"Up\tAccelerate\n"
	"Down\tBrake, reverse\n"
	"Left Right\tSteer\n"
	"Space\tUse the power-up\n"
	"Enter\tStart; again after the race\n"
	"Left Right\tOn the title: the track\n"
	"M\tMusic on or off\n"
	"S\tScenery on or off (for a frame rate without it)\n"
	"F\tPacing free (no lock to 30), for the frame rate as it is\n"
	"F11\tGame mode, or back to the window\n"
	"Esc\tLeave game mode\n\n"
	"? boxes: SHOES go faster, a FEATHER jumps, the BELL stuns those "
	"ahead, a BUBBLE shields you, the MAGNET pulls you to the kart "
	"ahead. Cheese is a burst of speed, a spring a jump.";

static ar_desc_t desc = {
	.name = "mauskart3d",
	.help_title = "mauskart3d help",
	.help_text = help_text,
	.help_doc = "mauskart3d",
	.palette = { [1] = 0xfff },
	.ink = { [1] = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } } },
	.paint = paint,
};

/* -- the palette: mausrun3d's ramps (its world.h) and a few colours -- */

static void ramp(uint16_t *pal, int at, uint16_t dark, uint16_t light) {
	int i, k;
	for (i = 0; i < 16; i++) {
		uint16_t c = 0;
		for (k = 0; k < 3; k++) {
			int sh = 8 - 4 * k;
			int a = (dark >> sh) & 15, b = (light >> sh) & 15;
			c |= (uint16_t)((a + (b - a) * i / 15) << sh);
		}
		pal[at + i] = c;
	}
}

static void palette(uint16_t pal[256]) {
	memset(pal, 0, 256 * sizeof(pal[0]));
	pal[C_WHITE] = 0xfff;
	pal[C_HUD] = 0xfd2;
	pal[C_SHADOW] = 0x232;
	pal[C_CHEESE] = 0xfd2;
	pal[C_RED] = 0xe33;
	pal[C_BLACK] = 0x111;
	pal[C_DARK] = 0x334;
	ramp(pal, W3_R_SKY, 0x9df, 0x37d);
	ramp(pal, W3_R_GRASS, 0x130, 0x7e4);
	ramp(pal, W3_R_EARTH, 0x210, 0xa63);
	ramp(pal, W3_R_PATH, 0x320, 0xdb7);
	ramp(pal, W3_R_SAND, 0x431, 0xfe9);
	ramp(pal, W3_R_ROCK, 0x111, 0x99a);
	ramp(pal, W3_R_BOX, 0x310, 0xc83);
	ramp(pal, W3_R_QBOX, 0x420, 0xfd3);
	ramp(pal, W3_R_WATER, 0x024, 0x5af);
	ramp(pal, W3_R_MOUSE, 0x333, 0xeef);
	ramp(pal, W3_R_PINK, 0x412, 0xfab);
	ramp(pal, W3_R_CHEESE, 0x420, 0xfe5);
	ramp(pal, W3_R_CAT, 0x310, 0xf93);
	ramp(pal, W3_R_SHELF, 0x321, 0xdb8);
	ramp(pal, W3_R_RED, 0x300, 0xf44);
}

static GLuint l_sky;
static void track_lists(void);

/* -- the karts: driving -- */

static void place_grid(void) {
	int i;
	for (i = 0; i < RACERS; i++) {
		kart_t *k = &kart[i];
		/* two by two behind the line: the mouse at the back left */
		int row = (RACERS - 1 - i) / 2, col = i & 1;
		int sg = (tk_n - 2 - row * 2) % tk_n;
		const seg_t *g = &tk_seg[sg];
		int32_t off = col ? 14 : -14;
		memset(k, 0, sizeof(*k));
		k->x = (g->x + (-(int32_t)g->dz * off) / 16384) * Q;
		k->z = (g->z + ((int32_t)g->dx * off) / 16384) * Q;
		k->yaw = g->yaw;
		k->seg = sg;
		k->ai = i != 0 || autopilot;
		k->lane = (int16_t)(i == 1 ? -24 : i == 2 ? 24 : i == 3 ? 8 : -8);   /* 16 apart: not touching */
		k->skill = (int16_t)(i == 1 ? 98 : i == 2 ? 95 : i == 3 ? 92 : 97);
		k->item_wait = (int16_t)(60 + (int)(rnd() % 180));
	}
	cam_yaw = kart[0].yaw;
}

static void give_item(kart_t *k) {
	/* further back, better: the bell and shoes from the back */
	uint32_t r = rnd() % 100;
	if (k->item) return;
	if (k->place >= 3) k->item = r < 35 ? P_SHOES : r < 60 ? P_BELL : r < 80 ? P_MAGNET : r < 92 ? P_FEATHER : P_BUBBLE;
	else k->item = r < 25 ? P_SHOES : r < 40 ? P_BELL : r < 55 ? P_MAGNET : r < 78 ? P_FEATHER : P_BUBBLE;
	k->item_wait = (int16_t)(40 + (int)(rnd() % 240));
	if (k == &kart[0]) sound(SND_POWER);
}

static void use_item(kart_t *k) {
	int i;
	switch (k->item) {
	case P_SHOES: k->boost = 150; k->v += 40; break;
	case P_FEATHER: if (k->y == 0) k->vy = 5 * Q; break;
	case P_BELL:
		for (i = 0; i < RACERS; i++) {
			kart_t *o = &kart[i];
			if (o == k || o->prog <= k->prog || o->shield) continue;
			if (o->prog - k->prog > (int32_t)tk_n * 256 / 3) continue;
			o->stun = 60;
		}
		if (sound_on) snd_play(SND_STOMP, -1);
		if (k == &kart[0]) bell_flash = 45;
		break;
	case P_BUBBLE: k->shield = 600; break;
	case P_MAGNET: k->magnet = 180; break;
	default: return;
	}
	if (k == &kart[0] && k->item != P_BELL) sound(SND_SELECT);
	k->item = P_NONE;
}

/* the AI's hands: steer to a point ahead on its line, at the track's speed */
static void ai_drive(kart_t *k, int *steer, int *gas) {
	int look = 2 + k->v / 448, i;
	const seg_t *g = &tk_seg[(k->seg + look) % tk_n];
	int32_t lane = k->lane, tx, tz, want;
	int16_t d;
	/* round a kart ahead on the same line */
	for (i = 0; i < RACERS; i++) {
		kart_t *o = &kart[i];
		int32_t dp = o->prog - k->prog;
		if (o == k || dp <= 0 || dp > 3 * 256) continue;
		if (o->across - lane < 16 && lane - o->across < 16) lane = o->across > 0 ? o->across - 22 : o->across + 22;
	}
	if (lane > ROAD_HW - 10) lane = ROAD_HW - 10;
	if (lane < -(ROAD_HW - 10)) lane = -(ROAD_HW - 10);
	tx = g->x + (-(int32_t)g->dz * lane) / 16384;
	tz = g->z + ((int32_t)g->dx * lane) / 16384;
	d = (int16_t)(mk_atan2(tx * Q - k->x, tz * Q - k->z) - k->yaw);
	/* in proportion: full (16) from 640 turn16 off its point */
	*steer = d / 40;
	if (*steer > 16) *steer = 16;
	if (*steer < -16) *steer = -16;
	want = (int32_t)tk_seg[(k->seg + 4) % tk_n].vmax * k->skill / 100;
	/* the rubber band: a little faster far behind the mouse, slower ahead */
	if (!autopilot) {
		int32_t gap = kart[0].prog - k->prog;
		if (gap > (int32_t)tk_n * 256 / 8) want = want * 108 / 100;
		if (gap < -(int32_t)tk_n * 256 / 8) want = want * 94 / 100;
	}
	*gas = k->v < want ? 1 : k->v > want + 96 ? -1 : 0;
	if (k->item && --k->item_wait <= 0) use_item(k);
}

static void step_kart(kart_t *k, int steer, int gas) {
	int32_t top = TOP, rate;
	bool off;
	/* where it is on the track */
	k->seg = tk_nearest(k->x / Q, k->z / Q, k->seg, &k->across);
	off = k->across > ROAD_HW + KERB_W || k->across < -(ROAD_HW + KERB_W);
	if (off) top = TOP_OFF;
	if (k->boost > 0) { top = BOOST_TOP; k->boost--; }
	if (k->magnet > 0) {
		int i;
		k->magnet--;
		for (i = 0; i < RACERS; i++) {
			int32_t dp = kart[i].prog - k->prog;
			if (dp > 0 && dp < 16 * 256) { top = top * 118 / 100; break; }
		}
	}
	if (k->shield > 0) k->shield--;
	if (k->stun > 0) {
		k->stun--;
		k->v -= k->v / 12;
		k->yaw = (uint16_t)(k->yaw + 2400);
		steer = 0; gas = 0;
	}
	/* speed: up towards the top, down off the road or braking */
	if (gas > 0) k->v += (top - k->v) / 40 + 3;
	else if (gas < 0) k->v -= k->v > 0 ? 36 : 8;
	else k->v -= k->v / 96;
	if (k->v > top) k->v -= (k->v - top) / 16 + 4;
	if (k->v < -TOP / 4) k->v = -TOP / 4;
	/* steering: tighter at speed up to a point, none standing */
	rate = 220 + (k->v > 0 ? k->v : -k->v) * 600 / TOP;
	if (rate > 680) rate = 680;
	if (k->v < 64 && k->v > -64) rate = rate * (k->v < 0 ? -k->v : k->v) / 64;
	if (k->v < 0) rate = -rate;
	if (k->y == 0) k->yaw = (uint16_t)(k->yaw + steer * rate / 16);   /* steer -16..16 */
	/* the mouse's assist: on the road, going forward, the kart turns a
	 * little of its own with the road (a sixteenth of the difference a
	 * tick, more with the wheel straight): the bends take less steering
	 * and the straights none */
	if (k == &kart[0] && !k->ai && k->y == 0 && k->v > 128 && !k->stun &&
	    k->across < ROAD_HW + KERB_W && k->across > -(ROAD_HW + KERB_W)) {
		int16_t d = (int16_t)(tk_seg[(k->seg + 2) % tk_n].yaw - k->yaw);
		int32_t a = d / (steer ? 24 : 12);
		if (a > 240) a = 240;
		if (a < -240) a = -240;
		k->yaw = (uint16_t)(k->yaw + a);
	}
	/* move along the heading: (sin, -cos) */
	k->x += (int32_t)(((int64_t)zglSin(k->yaw) * k->v) >> 16);
	k->z -= (int32_t)(((int64_t)zglCos(k->yaw) * k->v) >> 16);
	/* in the air */
	if (k->y > 0 || k->vy > 0) {
		k->y += k->vy;
		k->vy -= GRAVITY;
		if (k->y <= 0) { k->y = 0; k->vy = 0; }
	}
	/* far off the track: back towards it */
	if (k->across > 3 * ROAD_HW || k->across < -3 * ROAD_HW) {
		const seg_t *g = &tk_seg[k->seg];
		k->x += (g->x * Q - k->x) / 32;
		k->z += (g->z * Q - k->z) / 32;
	}
}

/* laps and places */
static void count(kart_t *k, int seg_was) {
	if (seg_was > tk_n * 3 / 4 && k->seg < tk_n / 4) {         /* over the line */
		if (k->half || k->lap == 0) {
			if (k->lap > 0) {
				uint32_t t = race_ticks - k->lap_start;
				if (!k->best || t < k->best) k->best = t;
			}
			k->lap++;
			k->lap_start = race_ticks;
			k->half = false;
			if (k->lap > LAPS && !k->finish) {
				k->finish = race_ticks ? race_ticks : 1;
				if (k == &kart[0]) sound(SND_GOAL);
			} else if (k == &kart[0] && k->lap > 1) sound(SND_CHECK);
		}
	} else if (seg_was < tk_n / 4 && k->seg > tk_n * 3 / 4) {   /* back over it */
		if (k->lap > 0) { k->lap--; k->half = true; }
	}
	if (k->seg > tk_n / 3 && k->seg < tk_n * 2 / 3) k->half = true;
	k->prog = (k->lap * tk_n + (k->seg + tk_n - (k->lap == 0 && k->seg > tk_n / 2 ? tk_n : 0))) * 256;
	if (k->finish) k->prog = (int32_t)(1 << 30) - (int32_t)k->finish;
}

static void places(void) {
	int i, j;
	for (i = 0; i < RACERS; i++) {
		int p = 1;
		for (j = 0; j < RACERS; j++)
			if (j != i && (kart[j].prog > kart[i].prog || (kart[j].prog == kart[i].prog && j < i))) p++;
		kart[i].place = p;
	}
}

/* karts against each other: pushed apart, a bump's worth of speed lost */
static void bumps(void) {
	int i, j;
	for (i = 0; i < RACERS; i++)
		for (j = i + 1; j < RACERS; j++) {
			kart_t *a = &kart[i], *b = &kart[j];
			int32_t dx = (b->x - a->x) / Q, dz = (b->z - a->z) / Q, d2 = dx * dx + dz * dz;
			int32_t r = 2 * K_RADIUS / Q;
			if (d2 >= r * r || a->y > 4 * Q || b->y > 4 * Q) continue;
			{
				int32_t d = 1;
				while (d * d < d2) d++;
				if (d == 0) d = 1;
				/* half the overlap each, along the line between them */
				a->x -= dx * (r - d) * Q / (2 * d); a->z -= dz * (r - d) * Q / (2 * d);
				b->x += dx * (r - d) * Q / (2 * d); b->z += dz * (r - d) * Q / (2 * d);
				if (!a->shield) a->v -= a->v / 48;
				if (!b->shield) b->v -= b->v / 48;
				if ((a == &kart[0] || b == &kart[0]) && (ticks & 15) == 0) sound(SND_BUMP);
			}
		}
}

/* ? boxes, cheese, springs */
static void pickups(void) {
	int i, t;
	for (t = 0; t < tk_nthings; t++) {
		thing_t *th = &tk_thing[t];
		if (th->gone) { th->gone--; continue; }
		for (i = 0; i < RACERS; i++) {
			kart_t *k = &kart[i];
			int32_t dx, dz, ds = th->seg - k->seg;
			if (ds < -1) ds += tk_n;
			if (ds > 1 && ds < tk_n - 1) continue;      /* not this segment or the next */
			dx = k->x / Q - th->x; dz = k->z / Q - th->z;
			if (dx * dx + dz * dz > 13 * 13 || k->y > 6 * Q) continue;
			switch (th->kind) {
			case TH_QBOX: give_item(k); th->gone = 180; break;
			case TH_CHEESE:
				k->boost = k->boost > 40 ? k->boost : 40;
				th->gone = 240;
				if (i == 0) { cheese_got++; sound(SND_COIN); }
				break;
			case TH_SPRING:
				if (k->y == 0) { k->vy = 6 * Q; if (i == 0) sound(SND_SPRING); }
				break;
			default: break;
			}
		}
	}
}

static void step_race(uint32_t held) {
	int i;
	for (i = 0; i < RACERS; i++) {
		kart_t *k = &kart[i];
		int steer = 0, gas = 0, seg_was = k->seg;
		if (gstate == G_RACE && !k->finish) {
			if (k->ai) ai_drive(k, &steer, &gas);
			else {
				/* the wheel (-32..32) turns in over 16 ticks and back
				 * over 8; the steering is its square, so a tap is a
				 * small nudge and a hold comes on smoothly to a full
				 * turn */
				static int wheel;
				if (held & AR_BTN_RIGHT) wheel = wheel < 0 ? wheel + 4 : wheel + 2;
				else if (held & AR_BTN_LEFT) wheel = wheel > 0 ? wheel - 4 : wheel - 2;
				else wheel = wheel > 3 ? wheel - 4 : wheel < -3 ? wheel + 4 : 0;
				if (wheel > 32) wheel = 32;
				if (wheel < -32) wheel = -32;
				steer = wheel * (wheel < 0 ? -wheel : wheel) / 64;   /* -16..16 */
				gas = (held & AR_BTN_UP) ? 1 : (held & AR_BTN_DOWN) ? -1 : 0;
			}
		} else if (k->finish) {                  /* over the line: coast round */
			ai_drive(k, &steer, &gas);
			if (k->v > TOP / 2) gas = -1;
		}
		step_kart(k, steer, gas);
		count(k, seg_was);
	}
	bumps();
	pickups();
	places();
	if (gstate == G_RACE) race_ticks++;
}

/* the engine: a low buzz whose pitch follows the mouse's speed, a short
 * note played again every tenth of a second at the lowest priority (any
 * other effect takes its channel) */
static void engine(void) {
	static zsfx_seg_t seg[2];
	static zsfx_fx_t fx[2];
	static uint32_t last;
	static int which;
	int32_t v = kart[0].v > 0 ? kart[0].v : 0;
	uint16_t f;
	if (!sound_on || (gstate != G_RACE && gstate != G_COUNT) || ticks / 6 == last) return;
	last = ticks / 6;
	f = (uint16_t)(55 + v * 110 / TOP);
	if (kart[0].boost) f = (uint16_t)(f * 6 / 5);
	which ^= 1;                  /* not the one playing: it reads its segment */
	seg[which].wave = ZSFX_PULSE25;
	seg[which].frames = 7;
	seg[which].f0 = f;
	seg[which].f1 = (uint16_t)(f + 3);
	seg[which].v0 = seg[which].v1 = (uint8_t)(gstate == G_COUNT ? 24 : 36);
	fx[which].prio = 0;
	fx[which].n = 1;
	fx[which].seg = &seg[which];
	zsfx_play(&fx[which], 0);
}

/* another track: its segments, its lists (when there are lists yet) */
static void change_track(int t) {
	tk_select(t);
	tk_build();
	if (l_sky) {
		tk_lists();
		track_lists();
	}
	place_grid();
	places();
	sound(SND_MENU);
	record_load();
	printf("mauskart3d: %s, %d segments\n", tk_name(), tk_n);
}

static void step_camera(void) {
	kart_t *k = &kart[0];
	static int32_t rate;                         /* the camera's turn a tick, turn16 */
	int16_t d = (int16_t)(k->yaw - cam_yaw);
	int32_t x, z;
	if (k->stun) d = 0;                          /* the camera does not spin */
	/* it follows the kart's heading, and its own turning comes on and
	 * off gently: no step in how fast the view swings */
	rate += (d / 4 - rate) / 2;
	cam_yaw = (uint16_t)(cam_yaw + rate);
	x = k->x / Q; z = k->z / Q;
	cam_x = x - (int32_t)(((int64_t)zglSin(cam_yaw) * CAM_DIST) >> 16);
	cam_z = z + (int32_t)(((int64_t)zglCos(cam_yaw) * CAM_DIST) >> 16);
	cam_y = CAM_H + k->y / Q / 2;
}

/* -- drawing -- */

#define MAP_TEX 20                    /* the minimap's texture (track.c's are 1-9) */
static bool map_tex;
static GLuint l_qbox, l_cheese, l_spring, l_dot, l_map, l_hud, l_hills, l_bubble, l_stars;
static bool tex_have;
static uint32_t tris_sent;

/* the horizon, pixels from the top, as the frustum and the pitch put it */
static int horizon_y(int near) {
	/* half height 12 at the near plane: 120 pixels; tan(9 degrees) ~ 0.158 */
	return 120 - (120 * near / 12) * 158 / 1000;
}

static void view_2d(void) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthox(0, 320 << 16, 240 << 16, 0, -65536, 65536);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static int near_now = NEAR;

static void view_3d(void) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	/* at speed the view widens a little: the near plane comes in */
	zglFrustumi(-16, 16, -12, 12, near_now, FAR + 256);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatex(PITCH_DEG << 16, 65536, 0, 0);
	glRotatex((GLfixed)(cam_yaw * 360u), 0, 65536, 0);
	zglTranslatei(-cam_x, -cam_y, -cam_z);
}

static void sky_content(void) {
	int hy = horizon_y(NEAR);                /* moved as the view widens (render()) */
	glBegin(GL_TRIANGLE_STRIP);
	glIndexi(W3_R_SKY + 15); glVertex2i(0, -40);
	glIndexi(W3_R_SKY + 15); glVertex2i(320, -40);
	glIndexi(W3_R_SKY); glVertex2i(0, hy);
	glIndexi(W3_R_SKY); glVertex2i(320, hy);
	glEnd();
	/* the ground is the clear colour (render()): T2's raster costs a
	 * triangle's box, a pixel a clock, so the smaller half is drawn */
}

/* a card standing up, facing -z, its foot at y0: w wide, h high */
static void card_list(GLuint l, int ramp, int tex, int w, int y0, int h) {
	glNewList(l, GL_COMPILE);
	mk_material(ramp);
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	glBegin(GL_TRIANGLES);
	glNormal3i(0, 0, -1);
	glTexCoord2i(0, 16); glVertex3i(-w / 2, y0, 0);
	glTexCoord2i(16, 16); glVertex3i(w / 2, y0, 0);
	glTexCoord2i(16, 0); glVertex3i(w / 2, y0 + h, 0);
	glTexCoord2i(0, 16); glVertex3i(-w / 2, y0, 0);
	glTexCoord2i(16, 0); glVertex3i(w / 2, y0 + h, 0);
	glTexCoord2i(0, 0); glVertex3i(-w / 2, y0 + h, 0);
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
	glEndList();
}

static void make_lists(void) {
	l_sky = glGenLists(10);
	l_qbox = l_sky + 1; l_cheese = l_sky + 2; l_spring = l_sky + 3;
	l_dot = l_sky + 4; l_map = l_sky + 5; l_hud = l_sky + 6;
	l_hills = l_sky + 7; l_bubble = l_sky + 8; l_stars = l_sky + 9;
	glNewList(l_stars, GL_COMPILE);                        /* stars over a stunned kart */
	mk_flat(C_HUD);
	glIndexi(C_HUD);
	glBegin(GL_LINES);
	{
		int i;
		for (i = 0; i < 3; i++) {
			int32_t sx = (int32_t)((zglSin((uint32_t)i * 21845u) * 9) >> 16);
			int32_t sz = (int32_t)((zglCos((uint32_t)i * 21845u) * 9) >> 16);
			glVertex3i(sx - 2, 24, sz); glVertex3i(sx + 2, 24, sz);
			glVertex3i(sx, 22, sz); glVertex3i(sx, 26, sz);
		}
	}
	glEnd();
	glEndList();
	glNewList(l_bubble, GL_COMPILE);                       /* the bubble: a ring behind the kart */
	mk_flat(W3_R_WATER + 15);
	glIndexi(W3_R_WATER + 15);
	glBegin(GL_LINE_LOOP);
	{
		static const int8_t ring[8][2] = { { 0, 15 }, { 11, 11 }, { 15, 0 }, { 11, -11 }, { 0, -15 }, { -11, -11 }, { -15, 0 }, { -11, 11 } };
		int i;
		for (i = 0; i < 8; i++) glVertex3i(ring[i][0], 10 + ring[i][1], 13);
	}
	glEnd();
	glEndList();
	card_list(l_qbox, W3_R_QBOX, 4, 14, 4, 14);            /* TX_QBOX */
	glNewList(l_cheese, GL_COMPILE);                       /* a wedge, both sides */
	mk_material(W3_R_CHEESE);
	glBegin(GL_TRIANGLES);
	glNormal3i(0, 0, -1);
	glVertex3i(-6, 2, 0); glVertex3i(6, 2, 0); glVertex3i(6, 10, 0);
	glNormal3i(0, 0, 1);
	glVertex3i(-6, 2, 0); glVertex3i(6, 10, 0); glVertex3i(6, 2, 0);
	glEnd();
	glEndList();
	card_list(l_spring, W3_R_PINK, 0, 16, 0, 5);
	glNewList(l_dot, GL_COMPILE);                          /* a kart on the minimap */
	glBegin(GL_LINES);
	glVertex2i(-2, 0); glVertex2i(2, 0);       /* a dash: a line is dear (as a triangle) */
	glEnd();
	glEndList();
	track_lists();                   /* last: the things' lists call the cards' */
}

/* the lists that depend on the track: the sky's ground, the hills, the map */
static void things_lists(void);

static void track_lists(void) {
	glNewList(l_sky, GL_COMPILE); sky_content(); glEndList();
	things_lists();
	glNewList(l_hills, GL_COMPILE);                        /* hills on the horizon, 640 wide */
	{
		static const int16_t hump[6][3] = {    /* x0, width, height in pixels */
			{ 0, 90, 14 }, { 60, 70, 22 }, { 120, 110, 10 }, { 200, 60, 18 }, { 240, 80, 26 }, { 290, 60, 8 } };
		int hy = horizon_y(NEAR), c, i;
		glBegin(GL_TRIANGLES);
		for (c = 0; c < 2; c++)
			for (i = 0; i < 6; i++) {
				int x0 = c * 320 + hump[i][0], w = hump[i][1];
				glIndexi(tk_ground() + 4 + (i & 1) * 2);
				glVertex2i(x0, hy); glVertex2i(x0 + w / 2, hy - hump[i][2]); glVertex2i(x0 + w, hy);
			}
		glEnd();
	}
	glEndList();
	/* the minimap: with textures, the loop drawn into a 32x32 texture
	 * (each texel its ramp, ZGL_TEX_RAMP; the rest keyed out) on one
	 * quad -- two triangles where the loop as lines was 17; without,
	 * the lines */
	map_tex = false;
	if (tex_have) {
		static uint8_t t[32 * 32];
		int i;
		memset(t, 0, sizeof(t));
		for (i = 0; i < tk_n; i++) {
			int x = tk_map_x[i] / 2, y = tk_map_y[i] / 2;
			if (x > 31) x = 31;
			if (y > 31) y = 31;
			t[y * 32 + x] = (uint8_t)(W3_R_MOUSE | 15);
		}
		map_tex = zglTexImage(MAP_TEX, 5, 5, t, ZGL_TEX_RAMP, true);
	}
	glNewList(l_map, GL_COMPILE);
	if (map_tex) {
		glBindTexture(GL_TEXTURE_2D, MAP_TEX);
		glIndexi(15);                          /* a tone's full light */
		glBegin(GL_TRIANGLES);
		glTexCoord2i(0, 0); glVertex2i(250, 168);
		glTexCoord2i(0, 32); glVertex2i(250, 232);
		glTexCoord2i(32, 32); glVertex2i(314, 232);
		glTexCoord2i(0, 0); glVertex2i(250, 168);
		glTexCoord2i(32, 32); glVertex2i(314, 232);
		glTexCoord2i(32, 0); glVertex2i(314, 168);
		glEnd();
		glBindTexture(GL_TEXTURE_2D, 0);
	} else {
		int i;
		glIndexi(C_WHITE);
		glBegin(GL_LINE_LOOP);
		for (i = 0; i < tk_n; i += 14) glVertex2i(250 + tk_map_x[i], 168 + tk_map_y[i]);
		glEnd();
	}
	glEndList();
}

/* each thing a list of its own, its place and facing (back along the
 * track, to the karts coming) in its vertices: drawing it is a call,
 * no matrix (on the board the matrices were 0.1-0.17 M cycles) */
static GLuint l_things;

static void things_lists(void) {
	int t;
	if (l_things) glDeleteLists(l_things, MAX_THINGS);
	l_things = glGenLists(MAX_THINGS);
	for (t = 0; t < tk_nthings; t++) {
		const thing_t *th = &tk_thing[t];
		const seg_t *g = &tk_seg[th->seg];
		GLuint l = th->kind == TH_QBOX ? l_qbox : th->kind == TH_CHEESE ? l_cheese : l_spring;
		glNewList(l_things + (GLuint)t, GL_COMPILE);
		glPushMatrix();
		zglTranslatei(th->x, 0, th->z);
		glRotatex((GLfixed)((65536u - g->yaw) * 360u), 0, 65536, 0);
		glCallList(l);
		glPopMatrix();
		glEndList();
	}
}

static void draw_things(void) {
	int t, base = kart[0].seg;
	for (t = 0; t < tk_nthings; t++) {
		const thing_t *th = &tk_thing[t];
		int ds = th->seg - base;
		if (ds < -1) ds += tk_n;
		if (th->gone || ds < 0 || ds > 10) continue;    /* a few segments ahead */
		glCallList(l_things + (GLuint)t);
		tris_sent += 2;
	}
}

static void draw_karts(void) {
	int i;
	for (i = RACERS - 1; i >= 0; i--) {
		const kart_t *k = &kart[i];
		int32_t x = k->x / Q, z = k->z / Q;
		int32_t dx = x - cam_x, dz = z - cam_z;
		int32_t along = (int32_t)(((int64_t)dx * zglSin(cam_yaw) - (int64_t)dz * zglCos(cam_yaw)) >> 16);
		bool near;
		if (along < 4 || along > far_now) continue;
		near = along < 90;
		glPushMatrix();
		zglTranslatei(x, k->y / Q, z);
		glRotatex((GLfixed)((65536u - k->yaw) * 360u), 0, 65536, 0);
		glCallList(kt_list + (near ? KT_NEAR(i) : KT_FAR(i)));
		tris_sent += kt_tris[near ? KT_NEAR(i) : KT_FAR(i)];
		if (near && k->shield && (k->shield > 120 || (ticks & 4))) glCallList(l_bubble);
		if (near && k->stun) {                           /* stars, turning */
			glRotatex((GLfixed)((ticks * 6u % 360u) << 16), 0, 65536, 0);
			glCallList(l_stars);
		}
		if (near && along < 100) {                       /* the shadow, under it */
			zglTranslatei(0, -(k->y / Q), 0);
			glCallList(kt_list + KT_SHADOW);
			tris_sent += 2;
		}
		glPopMatrix();
	}
}

static const char *const ordinal[5] = { "", "1ST", "2ND", "3RD", "4TH" };

/* -- the race's HUD in letters of texture: z_font_5x8's glyphs, those
 * it uses, in a 64x32 texture (8x8 cells; ZGL_TEX_ADD: the ink texel 1
 * and the colour one less, so any palette entry; the paper keyed out).
 * A letter is two triangles where it was four or five lines (and a line
 * costs T2 about what a triangle does). Without textures, the lines. */

#define FONT_TEX 21
static const char qt_set[] = "0123456789STNDRHLAPOEFBUMGIZY/!";
static bool qt_ok;

static void qt_make(void) {
	static uint8_t t[64 * 32];
	int i, r, c;
	memset(t, 0, sizeof(t));
	for (i = 0; qt_set[i]; i++) {
		const uint8_t *g = &z_font_5x8.glyphs[z_font_index(&z_font_5x8, (uint8_t)qt_set[i]) * 8];
		int cx = (i & 7) * 8, cy = (i >> 3) * 8;
		for (r = 0; r < 8; r++)
			for (c = 0; c < 5; c++)
				if (g[r] & (0x80 >> c)) t[(cy + r) * 64 + cx + c] = 1;
	}
	qt_ok = zglTexImage(FONT_TEX, 6, 5, t, ZGL_TEX_ADD, true);
}

/* in a list being made (glBindTexture goes in it); x, y the top left */
static void qt_text(int x, int y, int scale, int colour, const char *str) {
	if (!qt_ok) {
		vf_text(x, y, scale, colour, str, false);
		return;
	}
	glBindTexture(GL_TEXTURE_2D, FONT_TEX);
	glIndexi(colour - 1);
	glBegin(GL_TRIANGLES);
	for (; *str; str++, x += 6 * scale) {
		const char *p = strchr(qt_set, *str);
		int i, u, v, w = 6 * scale, h = 8 * scale;
		if (!p || *str == 0) continue;
		i = (int)(p - qt_set);
		u = (i & 7) * 8; v = (i >> 3) * 8;
		glTexCoord2i(u, v); glVertex2i(x, y);
		glTexCoord2i(u, v + 8); glVertex2i(x, y + h);
		glTexCoord2i(u + 6, v + 8); glVertex2i(x + w, y + h);
		glTexCoord2i(u, v); glVertex2i(x, y);
		glTexCoord2i(u + 6, v + 8); glVertex2i(x + w, y + h);
		glTexCoord2i(u + 6, v); glVertex2i(x + w, y);
	}
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void qt_centred(int y, int scale, int colour, const char *str) {
	qt_text((320 - (int)strlen(str) * 6 * scale) / 2, y, scale, colour, str);
}

static void time_text(char *b, size_t n, uint32_t t) {
	snprintf(b, n, "%u:%02u.%u", (unsigned)(t / 3600), (unsigned)(t / 60 % 60), (unsigned)(t % 60 / 6));
}

static void hud_content(void) {
	char b[40];
	const kart_t *me = &kart[0];
	int lap = me->lap < 1 ? 1 : me->lap > LAPS ? LAPS : me->lap;
	if (gstate == G_TITLE) {
		vf_centred(60, 4, W3_R_RED + 15, "MAUSKART", false);
		vf_centred(110, 2, C_WHITE, tk_name(), false);
		/* few letters: the title is over the grid, and lines are dear */
		snprintf(b, sizeof(b), "TRACK %d/%d", tk_selected() + 1, tk_tracks());
		vf_centred(126, 1, C_WHITE, b, false);
		if (record) {
			char t[24];
			time_text(t, sizeof(t), record);
			snprintf(b, sizeof(b), "RECORD LAP %s", t);
			vf_centred(138, 1, C_HUD, b, false);
		}
		if ((ticks / 30) & 1) vf_centred(150, 2, C_HUD, "PRESS ENTER", false);
		return;
	}
	if (gstate == G_DONE) goto results;   /* the table alone: lines are dear */
	qt_text(8, 6, 3, C_HUD, ordinal[me->place]);
	snprintf(b, sizeof(b), "LAP %d/%d", lap, LAPS);
	qt_text(262, 6, 1, C_WHITE, b);
	if (me->item) qt_text(8, 34, 1, C_HUD, pname[me->item]);
	if (me->shield) qt_text(8, 44, 1, W3_R_WATER + 15, "BUBBLE");
	if (gstate == G_RACE && bell_flash) qt_centred(70, 3, C_HUD, "DING!");
	else if (gstate == G_RACE && me->stun) qt_centred(70, 2, C_RED, "DIZZY!");
	if (gstate == G_COUNT) {
		int n = 3 - gtimer / 60;
		qt_centred(80, 5, n > 0 ? C_WHITE : C_HUD, n > 0 ? (n == 3 ? "3" : n == 2 ? "2" : "1") : "GO!");
	}
	if (gstate == G_RACE && race_ticks < 60) qt_centred(80, 5, C_HUD, "GO!");
	return;
results:
	{
		int p, y = 70;
		vf_centred(40, 3, C_HUD, me->place == 1 ? "YOU WIN!" : "FINISHED", false);
		for (p = 1; p <= RACERS; p++) {
			int i;
			for (i = 0; i < RACERS; i++) {
				if (kart[i].place != p) continue;
				if (kart[i].finish) time_text(b, sizeof(b), kart[i].finish);
				else snprintf(b, sizeof(b), "-");
				vf_text(90, y, 1, i == 0 ? C_HUD : C_WHITE, ordinal[p], false);
				vf_text(130, y, 1, i == 0 ? C_HUD : C_WHITE, kt_racer[i].name, false);
				vf_text(190, y, 1, i == 0 ? C_HUD : C_WHITE, b, false);
				y += 12;
			}
		}
		if (me->best) {
			char t[24];
			time_text(t, sizeof(t), me->best);
			snprintf(b, sizeof(b), new_record ? "BEST LAP %s  A RECORD!" : "BEST LAP %s", t);
			vf_centred(130, 1, C_WHITE, b, false);
		}
		if (gtimer > 120) vf_centred(170, 1, C_HUD, "ENTER: RACE AGAIN", false);
	}
}

/* the HUD's list, made again when what it shows changes */
static void draw_hud(void) {
	static int key[14], nkey = -1;
	int k[14], n = 0, i;
	const kart_t *me = &kart[0];
	k[n++] = gstate; k[n++] = me->place; k[n++] = me->lap; k[n++] = 0;
	k[n++] = me->item; k[n++] = me->shield > 0; k[n++] = 0;
	k[n++] = me->boost > 0; k[n++] = gstate == G_COUNT ? gtimer / 60 : gstate == G_RACE && race_ticks < 60;
	k[n++] = gstate == G_TITLE ? (int)((ticks / 30) & 1) : gstate == G_DONE ? gtimer > 120 : 0;
	k[n++] = (bell_flash > 0) | (me->stun > 0) << 1;
	k[n++] = tk_selected() | (int)record << 2;
	view_2d();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	glDisable(GL_TEXTURE_2D);
	if (n != nkey || memcmp(k, key, (size_t)n * sizeof(k[0]))) {
		memcpy(key, k, sizeof(k));
		nkey = n;
		glNewList(l_hud, GL_COMPILE);
		hud_content();
		glEndList();
	}
	if (qt_ok) glEnable(GL_TEXTURE_2D);       /* its letters */
	glCallList(l_hud);
	glDisable(GL_TEXTURE_2D);
	if (gstate == G_RACE || gstate == G_COUNT) {
		/* the speed, a bar: outside the HUD's list, which would
		 * otherwise be made again every frame (on the board that was
		 * 0.2-0.7 M cycles) */
		int w = (me->v > 0 ? me->v : 0) * 60 / BOOST_TOP;
		glIndexi(me->boost ? C_RED : C_WHITE);
		glBegin(GL_LINES);
		glVertex2i(8, 228); glVertex2i(8 + w, 228);
		glEnd();
	}
	if (gstate != G_TITLE && gstate != G_DONE) {
		if (map_tex) glEnable(GL_TEXTURE_2D);
		glCallList(l_map);
		glDisable(GL_TEXTURE_2D);
		glBegin(GL_LINES);                        /* the karts: dashes, no matrices */
		for (i = RACERS - 1; i >= 0; i--) {
			int sg = kart[i].seg, x = 250 + tk_map_x[sg], y = 168 + tk_map_y[sg];
			glIndexi(i == 0 ? C_HUD : kt_racer[i].box + 15);
			glVertex2i(x - 2, y); glVertex2i(x + 2, y);
		}
		glEnd();
	}
}

/* where a frame's CPU cycles go (the report every two seconds) */
static inline uint32_t cycles_now(void) {
#ifdef AR_HOST_TESTS
	return 0;
#else
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
#endif
}
static inline uint32_t instret_now(void) {
#ifdef AR_HOST_TESTS
	return 0;
#else
	uint32_t v;
	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
#endif
}
enum { T_SKY, T_TRACK, T_THINGS, T_KARTS, T_HUD, T_FRAME, T_GAME, T_WAIT, T_N };
static uint32_t tsum[T_N], isum[T_N];

#ifdef AR_HOST_TESTS                  /* tests/render.c: a part left out, to cost it */
#define PROBE(x) (!getenv("MK_NO" x))
#else
#define PROBE(x) 1
#endif

static void render(void) {
	static const GLfixed light[4] = { -29491, 52428, 26214, 0 };
	static const GLfixed white[4] = { 65536, 65536, 65536, 65536 };
	uint32_t t0 = cycles_now(), t1, i0 = instret_now(), i1;
#define LAP(T) do { t1 = cycles_now(); tsum[T] += t1 - t0; t0 = t1; \
	i1 = instret_now(); isum[T] += i1 - i0; i0 = i1; } while (0)
	/* faster, wider: the near plane from 14 to 11 */
	/* on a boost the view widens (the near plane in by 2), a step at a
	 * time, every 12 frames: by speed it flipped between two steps as
	 * the speed wavered at the top, every frame -- a bump (2026-11-06) */
	{
		static int steps;
		int want = kart[0].boost ? NEAR - 2 : NEAR;
		if (near_now != want && ++steps >= 12) {
			near_now += want < near_now ? -1 : 1;
			steps = 0;
		}
	}
	tris_sent = 0;
	glClearIndex((GLint)(tk_ground() + 8));
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	glDisable(GL_TEXTURE_2D);
	view_2d();
	zglTranslatei(0, horizon_y(near_now) - horizon_y(NEAR), 0);
	glShadeModel(GL_SMOOTH);
	glCallList(l_sky);
	glShadeModel(GL_FLAT);
	tris_sent += 4;
	if (tk_scenery) {                     /* the hills, turning with the view */
		zglTranslatei(-(int32_t)((cam_yaw * 1280u >> 16) % 320u), 0, 0);
		glCallList(l_hills);
		tris_sent += 12;
	}
	LAP(T_SKY);
	view_3d();
	glLightxv(GL_LIGHT0, GL_POSITION, light);
	glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
	glEnable(GL_LIGHT0);
	glEnable(GL_LIGHTING);
	glEnable(GL_CULL_FACE);
	if (tex_have) glEnable(GL_TEXTURE_2D);
	/* the results' table is lines enough for a frame: over the sky alone */
	if (gstate != G_DONE && PROBE("TRACK")) tris_sent += tk_draw(cam_x, cam_z, cam_yaw, far_now, kart[0].seg);
	LAP(T_TRACK);
	if (gstate != G_DONE && PROBE("THINGS")) draw_things();
	LAP(T_THINGS);
	if (gstate != G_DONE && PROBE("KARTS")) draw_karts();
	LAP(T_KARTS);
	if (PROBE("HUD")) draw_hud();
	LAP(T_HUD);
	zgl_frame();
	LAP(T_FRAME);
#undef LAP
}

/* -- game mode -- */

static bool open_chunky(void) {
	zgl_desc_t zd = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY };
	uint16_t pal[256];
	if (!zgl_open(&zd)) {
		printf("mauskart3d: no memory for zgl\n");
		return false;
	}
	palette(pal);
	ar_pal_load(pal, 256);
	tex_have = tk_textures();
	if (tex_have) qt_make();
	printf("mauskart3d: game mode, chunky: %s%s\n",
		zgl_info()->backend == ZGL_BACKEND_T2 ? "T2" : "software (slow: T2 wanted)",
		tex_have ? ", textures" : "");
	if (!l_sky) {
		tk_lists();
		kt_make();
		make_lists();
	}
	return true;
}

static void mode_changed(void) {
	bool want = ar_mode() == AR_MODE_FULL_CHUNKY;
	if (want == chunky) return;
	if (want) chunky = open_chunky();
	else {
		zgl_close();
		chunky = false;
		ar_pal_load(desc.palette, 16);
		ar_dirty_all();
	}
}

/* -- for tests/render.c -- */

void mk_test_autopilot(bool on) { autopilot = on; kart[0].ai = on; }
void mk_test_status(int *state, int *lap, int *place, int *seg, int32_t *v) {
	*state = gstate; *lap = kart[0].lap; *place = kart[0].place; *seg = kart[0].seg; *v = kart[0].v;
}
void mk_test_to_seg(int s) {
	const seg_t *g = &tk_seg[s % tk_n];
	kart[0].x = g->x * Q; kart[0].z = g->z * Q; kart[0].yaw = g->yaw; kart[0].seg = s % tk_n;
	cam_yaw = g->yaw;
}
uint32_t mk_test_tris(void) { return tris_sent; }
int32_t mk_test_across(void) { return kart[0].across; }
int mk_test_stun(void) { return kart[0].stun | kart[0].boost << 8; }

/* -- the loop -- */

int main(void) {
	bool quit = false;
	uint32_t dt = 1, perf_t0 = 0, perf_frames = 0, perf_cycles = 0, perf_tris = 0;
	int lock = 1, hold = 0, missed = 0;
	bool free_run = false;
	uint32_t shown = 0;
	bool light = false;

	printf("mauskart3d: Mauskart -- the Meadow Ring\n");
	tk_build();
	printf("mauskart3d: %d segments, %u world units a lap\n", tk_n, (unsigned)tk_len);
	place_grid();
	places();

	ar_chunky(true);
	if (!ar_init(&desc)) {
		printf("mauskart3d: failed to create the window\n");
		return 1;
	}
	sound_on = zsfx_ok();
	ar_set_fullscreen(true);
	record_load();

	while (!quit) {
		ar_event_t e;
		bool go = false, use = false;
		int pick = 0;
		uint32_t k;
		while (ar_poll(&e)) {
			if (e.type == AR_EV_QUIT) quit = true;
			if (e.type == AR_EV_MODE) mode_changed();
			if (e.type == AR_EV_BUTTON && e.pressed && e.player == 0 && e.key == AR_BTN_START) go = true;
			if (e.type == AR_EV_BUTTON && e.pressed && e.player == 0 && e.key == AR_BTN_A) use = true;
			if (e.type == AR_EV_BUTTON && e.pressed && e.player == 0 && e.key == AR_BTN_LEFT) pick = -1;
			if (e.type == AR_EV_BUTTON && e.pressed && e.player == 0 && e.key == AR_BTN_RIGHT) pick = 1;
			if (e.type == AR_EV_KEY && e.pressed && (e.key == 's' || e.key == 'S')) {
				tk_scenery = !tk_scenery;
				printf("mauskart3d: scenery %s\n", tk_scenery ? "on" : "off");
			}
			if (e.type == AR_EV_KEY && e.pressed && (e.key == 'f' || e.key == 'F')) {
				/* free: no lock, for the frame rate as it is */
				free_run = !free_run;
				lock = 1; hold = 0; missed = 0;
				ar_frame_lock(1);
				printf("mauskart3d: pacing %s\n", free_run ? "free (no lock)" : "locked when frames run long");
			}
			if (e.type == AR_EV_KEY && e.pressed && (e.key == 'm' || e.key == 'M') && sound_on) {
				zsfx_music_enable(!zsfx_music_enabled());
			}
		}
		if (quit) break;
		for (k = 0; k < dt; k++) {
			uint32_t held = ar_held(0) | (ar_held(1) & AR_BTN_DIRS);
			uint32_t g0 = cycles_now(), gi = instret_now();
			gtimer++;
			switch (gstate) {
			case G_TITLE:
				if (pick && k == 0) { change_track(tk_selected() + pick); pick = 0; }
				if (go || autopilot) {
					gstate = G_COUNT; gtimer = 0; go = false;
					place_grid();
					race_ticks = 0;
					cheese_got = 0;
					if (sound_on) zsfx_music_play(&snd_song_meadow);
				}
				break;
			case G_COUNT:
				if (gtimer % 60 == 1) sound(gtimer >= 180 ? SND_SELECT : SND_TICK);
				if (gtimer >= 180) { gstate = G_RACE; gtimer = 0; }
				break;
			case G_RACE:
				if (use && k == 0 && !autopilot) use_item(&kart[0]);
				if (kart[0].finish) {
					gstate = G_DONE; gtimer = 0;
					new_record = false;
					record_try(kart[0].best);
				}
				break;
			case G_DONE:
				if (go && gtimer > 60) { gstate = G_TITLE; gtimer = 0; }
				break;
			}
			step_race(held);
			if (bell_flash > 0) bell_flash--;
			engine();
			step_camera();
			ticks++;
			tsum[T_GAME] += cycles_now() - g0;
			isum[T_GAME] += instret_now() - gi;
		}
		if (chunky) {
			uint32_t c0 = cycles_now();
			render();
			perf_cycles += cycles_now() - c0;
			perf_frames++;
			perf_tris += tris_sent;
			if (ar_ticks() - perf_t0 >= 120) {
				/* in lines under 80 columns: k is thousands of cycles */
#define K(T) (unsigned long)((tsum[T] / perf_frames + 500) / 1000)
#define KI(T) (unsigned long)((isum[T] / perf_frames + 500) / 1000)
				printf("mk3d: %lu fps, CPU %luk a frame, %lu tris, view %ld%s%s\n",
					(unsigned long)(perf_frames * 60 / (ar_ticks() - perf_t0 ? ar_ticks() - perf_t0 : 1)),
					(unsigned long)((perf_cycles / perf_frames + 500) / 1000), (unsigned long)(perf_tris / perf_frames),
					(long)far_now, free_run ? ", free" : lock == 2 ? ", lock 30" : lock == 3 ? ", lock 20" : "", tk_scenery ? "" : ", no scenery");
				printf("mk3d cyc: sky %lu trk %lu(%lu) thg %lu krt %lu hud %lu zgl %lu game %lu wait %lu\n",
					K(T_SKY), K(T_TRACK), (unsigned long)tk_calls, K(T_THINGS), K(T_KARTS), K(T_HUD),
					K(T_FRAME), K(T_GAME), K(T_WAIT));
				printf("mk3d ins: sky %lu trk %lu thg %lu krt %lu hud %lu game %lu\n",
					KI(T_SKY), KI(T_TRACK), KI(T_THINGS), KI(T_KARTS), KI(T_HUD), KI(T_GAME));
#undef K
#undef KI
				memset(tsum, 0, sizeof(tsum));
				memset(isum, 0, sizeof(isum));
				perf_t0 = ar_ticks();
				perf_frames = perf_cycles = perf_tris = 0;
			}
		}
		/* the pace: 60; an even 30 for a while after frames ran long, an
		 * even 20 if they still do (ar_frame_lock(), docs/arcade.md) */
		/* no glFinish() when locked: T2 draws this frame while the CPU
		 * sends the next (zgl shows each when it is out); waiting here
		 * put T2's whole raster in the CPU's frame */
		{
			uint32_t w0 = cycles_now();
			dt = ar_frame();
			tsum[T_WAIT] += cycles_now() - w0;
		}
		if (free_run) ;
		else if ((int)dt > lock) {
			if (++missed >= (lock == 1 ? 3 : 24) && lock < 3) {
				lock++;
				hold = 300;
				missed = 0;
				ar_frame_lock((uint32_t)lock);
				/* 20 frames a second (Lakritz?): the scenery off, once; S again */
				if (lock == 3 && tk_scenery && !light) {
					light = true;
					tk_scenery = false;
					printf("mauskart3d: frames run long: the scenery off (S turns it on)\n");
				}
			}
		} else if (missed > 0 && (++shown & 31) == 0) missed--;   /* frames, not ticks: dt is 2 or 3 */
		if (lock > 1 && hold > 0 && --hold == 0) {
			lock--;
			hold = lock > 1 ? 300 : 0;
			missed = 0;
			ar_frame_lock((uint32_t)lock);
		}
		/* and the draw distance: shorter while frames run long */
		if (lock > 1 && far_now > FAR_MIN) far_now -= 8;
		else if (lock == 1 && far_now < FAR && (ticks & 31) == 0) far_now += 8;
	}
	if (chunky) zgl_close();
	ar_shutdown();
	return 0;
}
