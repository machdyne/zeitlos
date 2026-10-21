/*
 * bricks -- the sounds and the music (sw/common/games/zsfx.h).
 *
 * Every effect is a few segments of one waveform; every tune is ASCII,
 * read top to bottom, four channels left to right: lead, harmony, bass,
 * drums. Both tunes are original.
 */

#include "../../common/games/zsfx.h"

#include "br_sound.h"

/* -- effects -- */

static const zsfx_seg_t s_paddle[] = {
	{ ZSFX_SQUARE, 2, 392, 392, 200, 150 },
	{ ZSFX_SQUARE, 4, 523, 523, 150, 0 },
};
static const zsfx_seg_t s_wall[] = {
	{ ZSFX_TRI, 3, 330, 300, 140, 0 },
};
static const zsfx_seg_t s_silver[] = {
	{ ZSFX_SQUARE, 2, 2093, 2093, 170, 140 },
	{ ZSFX_PULSE12, 10, 2637, 2600, 140, 0 },
};
static const zsfx_seg_t s_gold[] = {
	{ ZSFX_SINE, 2, 3136, 3136, 190, 170 },
	{ ZSFX_SINE, 14, 3951, 3951, 170, 0 },
};
static const zsfx_seg_t s_drop[] = {
	{ ZSFX_SINE, 4, 1568, 1760, 110, 90 },
	{ ZSFX_SINE, 4, 1760, 1568, 90, 0 },
};
static const zsfx_seg_t s_wide[] = {
	{ ZSFX_SAW, 18, 220, 660, 170, 0 },
};
static const zsfx_seg_t s_multi[] = {
	{ ZSFX_SQUARE, 3, 1047, 1047, 180, 160 },
	{ ZSFX_SQUARE, 3, 1319, 1319, 180, 160 },
	{ ZSFX_SQUARE, 3, 1568, 1568, 180, 160 },
	{ ZSFX_SQUARE, 3, 1047, 1047, 160, 140 },
	{ ZSFX_SQUARE, 3, 1319, 1319, 160, 140 },
	{ ZSFX_SQUARE, 8, 1568, 1568, 140, 0 },
};
static const zsfx_seg_t s_slow[] = {
	{ ZSFX_TRI, 24, 880, 220, 200, 0 },
};
static const zsfx_seg_t s_catch_power[] = {
	{ ZSFX_PULSE25, 4, 523, 523, 170, 150 },
	{ ZSFX_PULSE25, 4, 784, 784, 170, 150 },
	{ ZSFX_PULSE25, 10, 1047, 1047, 160, 0 },
};
static const zsfx_seg_t s_laser_power[] = {
	{ ZSFX_SAW, 6, 1600, 400, 160, 140 },
	{ ZSFX_SAW, 6, 1600, 400, 140, 120 },
	{ ZSFX_SAW, 10, 1600, 400, 120, 0 },
};
static const zsfx_seg_t s_life[] = {
	{ ZSFX_SQUARE, 4, 784, 784, 200, 180 },
	{ ZSFX_SQUARE, 4, 988, 988, 200, 180 },
	{ ZSFX_SQUARE, 4, 1175, 1175, 200, 180 },
	{ ZSFX_SQUARE, 4, 1568, 1568, 200, 180 },
	{ ZSFX_SQUARE, 4, 1175, 1175, 190, 170 },
	{ ZSFX_SQUARE, 20, 1568, 1568, 190, 0 },
};
static const zsfx_seg_t s_launch[] = {
	{ ZSFX_PULSE25, 6, 440, 880, 150, 0 },
};
static const zsfx_seg_t s_catch[] = {
	{ ZSFX_TRI, 3, 196, 196, 220, 150 },
	{ ZSFX_NOISE, 3, 900, 600, 90, 0 },
};
static const zsfx_seg_t s_fire[] = {
	{ ZSFX_PULSE12, 8, 2400, 700, 150, 0 },
};
static const zsfx_seg_t s_bolt_end[] = {
	{ ZSFX_NOISE, 3, 4000, 3000, 70, 0 },
};
static const zsfx_seg_t s_ball_lost[] = {
	{ ZSFX_TRI, 14, 660, 165, 170, 0 },
};
static const zsfx_seg_t s_lost[] = {
	{ ZSFX_NOISE, 5, 1800, 1400, 255, 230 },
	{ ZSFX_NOISE, 40, 1400, 90, 230, 0 },
};
static const zsfx_seg_t s_clear[] = {
	{ ZSFX_SQUARE, 4, 523, 523, 200, 180 },
	{ ZSFX_SQUARE, 4, 659, 659, 200, 180 },
	{ ZSFX_SQUARE, 4, 784, 784, 200, 180 },
	{ ZSFX_SQUARE, 4, 1047, 1047, 200, 180 },
	{ ZSFX_SQUARE, 4, 784, 784, 190, 170 },
	{ ZSFX_SQUARE, 4, 1047, 1047, 190, 170 },
	{ ZSFX_SQUARE, 30, 1319, 1319, 190, 0 },
};
static const zsfx_seg_t s_ready[] = {
	{ ZSFX_PULSE25, 6, 523, 523, 160, 120 },
	{ ZSFX_PULSE25, 6, 659, 659, 160, 120 },
	{ ZSFX_PULSE25, 6, 784, 784, 160, 120 },
	{ ZSFX_PULSE25, 18, 1047, 1047, 170, 0 },
};
static const zsfx_seg_t s_row[] = {
	{ ZSFX_PULSE12, 2, 1800, 1700, 80, 0 },
};
static const zsfx_seg_t s_menu[] = {
	{ ZSFX_PULSE12, 3, 1200, 1250, 120, 0 },
};
static const zsfx_seg_t s_select[] = {
	{ ZSFX_SQUARE, 3, 880, 880, 180, 160 },
	{ ZSFX_SQUARE, 8, 1319, 1319, 170, 0 },
};
static const zsfx_seg_t s_letter[] = {
	{ ZSFX_PULSE25, 2, 1568, 1568, 110, 0 },
};
static const zsfx_seg_t s_over[] = {
	{ ZSFX_TRI, 10, 440, 440, 220, 190 },
	{ ZSFX_TRI, 10, 349, 349, 210, 180 },
	{ ZSFX_TRI, 10, 294, 294, 200, 170 },
	{ ZSFX_TRI, 30, 220, 208, 190, 0 },
};

/* a springy "boing" up, then the glass of the floor going */
static const zsfx_seg_t s_floor[] = {
	{ ZSFX_TRI, 6, 220, 660, 230, 210 },
	{ ZSFX_TRI, 8, 660, 880, 210, 120 },
	{ ZSFX_NOISE, 14, 4000, 2000, 90, 0 },
};

static const zsfx_fx_t fx[BR_SND_COUNT] = {
	[BR_SND_PADDLE]      = { 2, 2, s_paddle },
	[BR_SND_WALL]        = { 0, 1, s_wall },
	[BR_SND_SILVER]      = { 2, 2, s_silver },
	[BR_SND_GOLD]        = { 2, 2, s_gold },
	[BR_SND_DROP]        = { 1, 2, s_drop },
	[BR_SND_WIDE]        = { 3, 1, s_wide },
	[BR_SND_MULTI]       = { 3, 6, s_multi },
	[BR_SND_SLOW]        = { 3, 1, s_slow },
	[BR_SND_CATCH_POWER] = { 3, 3, s_catch_power },
	[BR_SND_LASER_POWER] = { 3, 3, s_laser_power },
	[BR_SND_LIFE]        = { 4, 6, s_life },
	[BR_SND_LAUNCH]      = { 2, 1, s_launch },
	[BR_SND_CATCH]       = { 2, 2, s_catch },
	[BR_SND_FIRE]        = { 1, 1, s_fire },
	[BR_SND_BOLT_END]    = { 0, 1, s_bolt_end },
	[BR_SND_BALL_LOST]   = { 3, 1, s_ball_lost },
	[BR_SND_LOST]        = { 5, 2, s_lost },
	[BR_SND_CLEAR]       = { 5, 7, s_clear },
	[BR_SND_READY]       = { 4, 4, s_ready },
	[BR_SND_ROW]         = { 0, 1, s_row },
	[BR_SND_MENU]        = { 1, 1, s_menu },
	[BR_SND_SELECT]      = { 2, 2, s_select },
	[BR_SND_LETTER]      = { 1, 1, s_letter },
	[BR_SND_OVER]        = { 5, 4, s_over },
	[BR_SND_FLOOR]       = { 5, 3, s_floor },
};

void br_sound(int which, int x) {
	if (which < 0 || which >= BR_SND_COUNT) return;
	zsfx_play(&fx[which], x < 0 ? 0 : zsfx_pan_x(x));
}

/* Two octaves of a major pentatonic scale: a chain of breaks climbs it
 * and stays at the top. */
void br_sound_break(int chain, int x) {
	static const uint16_t scale[10] = {
		523, 587, 659, 784, 880, 1047, 1175, 1319, 1568, 1760
	};
	static zsfx_seg_t seg[10][2];
	static zsfx_fx_t cfx[10];
	int k = chain - 1;
	if (k < 0) k = 0;
	if (k > 9) k = 9;
	seg[k][0].wave = ZSFX_SQUARE;
	seg[k][0].frames = 2;
	seg[k][0].f0 = seg[k][0].f1 = scale[k];
	seg[k][0].v0 = 190;
	seg[k][0].v1 = 170;
	seg[k][1].wave = ZSFX_PULSE25;
	seg[k][1].frames = 9;
	seg[k][1].f0 = (uint16_t)(scale[k] * 2);
	seg[k][1].f1 = (uint16_t)(scale[k] * 2 - scale[k] / 8);
	seg[k][1].v0 = 160;
	seg[k][1].v1 = 0;
	cfx[k].prio = 1;
	cfx[k].n = 2;
	cfx[k].seg = seg[k];
	zsfx_play(&cfx[k], x < 0 ? 0 : zsfx_pan_x(x));
}

/* -- "Rebound", the game tune: A minor, driving, 8.4 rows a second -- */

static const char *const rebound_rows[] = {
	/* 0: A minor, the hook */
	"A-4 E-4 A-2 K--",
	"--- --- --- H--",
	"C-5 --- A-3 H--",
	"E-5 --- --- K--",
	"A-5 C-5 A-2 S--",
	"--- --- --- H--",
	"G-5 --- A-3 H--",
	"E-5 --- --- H--",
	"D-5 B-4 G-2 K--",
	"--- --- --- H--",
	"E-5 --- G-3 K--",
	"--- --- --- H--",
	"G-5 D-5 G-2 S--",
	"--- --- --- H--",
	"E-5 --- G-3 H--",
	"D-5 --- --- O--",
	/* 1: F, E */
	"C-5 A-4 F-2 K--",
	"--- --- --- H--",
	"A-4 --- F-3 H--",
	"C-5 --- --- K--",
	"F-5 C-5 F-2 S--",
	"--- --- --- H--",
	"E-5 --- F-3 H--",
	"C-5 --- --- H--",
	"B-4 G#4 E-2 K--",
	"--- --- --- H--",
	"D-5 --- E-3 K--",
	"--- --- --- H--",
	"E-5 B-4 E-2 S--",
	"G#5 --- --- S--",
	"B-5 --- E-3 S--",
	"--- --- --- S--",
	/* 2: up high */
	"A-5 E-5 A-2 K--",
	"--- --- --- H--",
	"E-5 --- A-3 H--",
	"A-5 --- --- K--",
	"B-5 F-5 D-2 S--",
	"--- --- --- H--",
	"A-5 --- D-3 H--",
	"F-5 --- --- H--",
	"G-5 E-5 C-3 K--",
	"--- --- --- H--",
	"E-5 --- C-2 K--",
	"C-5 --- --- H--",
	"D-5 B-4 G-2 S--",
	"E-5 --- --- H--",
	"G-5 --- G-3 H--",
	"--- --- --- O--",
	/* 3: down, and round again */
	"F-5 C-5 F-2 K--",
	"E-5 --- --- H--",
	"D-5 --- F-3 H--",
	"C-5 --- --- K--",
	"D-5 A-4 D-2 S--",
	"C-5 --- --- H--",
	"B-4 --- D-3 H--",
	"A-4 --- --- H--",
	"G#4 E-4 E-2 K--",
	"--- --- --- H--",
	"B-4 --- E-3 K--",
	"--- --- --- H--",
	"E-5 G#4 E-2 S--",
	"--- --- --- S--",
	"=== --- E-3 S--",
	"--- --- --- S--",
};

const zsfx_song_t br_song_game = {
	504, 4,
	{
		{ ZSFX_SQUARE,  20, 236, 3 },
		{ ZSFX_PULSE25,  9, 222, 0 },
		{ ZSFX_TRI,     30, 248, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	rebound_rows, 4, "01012323"
};

/* -- "Mortar", the title tune: D major, easy going -- */

static const char *const mortar_rows[] = {
	"D-5 F#4 D-3 K--",
	"--- A-4 --- ---",
	"F#5 D-5 --- H--",
	"--- A-4 --- ---",
	"A-5 F#4 A-2 S--",
	"--- A-4 --- ---",
	"F#5 D-5 --- H--",
	"E-5 A-4 --- ---",
	"D-5 G-4 G-2 K--",
	"--- B-4 --- ---",
	"B-4 D-5 --- H--",
	"--- B-4 --- ---",
	"D-5 G-4 B-2 S--",
	"--- B-4 --- ---",
	"E-5 D-5 --- H--",
	"--- B-4 --- ---",

	"F#5 A-4 A-2 K--",
	"--- C#5 --- ---",
	"E-5 E-5 --- H--",
	"--- C#5 --- ---",
	"C#5 A-4 E-3 S--",
	"--- C#5 --- ---",
	"A-4 E-5 --- H--",
	"--- C#5 --- ---",
	"D-5 F#4 D-3 K--",
	"--- A-4 --- ---",
	"F#5 D-5 --- H--",
	"--- A-4 --- ---",
	"D-5 F#4 A-2 S--",
	"--- A-4 --- ---",
	"=== D-5 --- H--",
	"--- A-4 --- ---",
};

const zsfx_song_t br_song_title = {
	420, 4,
	{
		{ ZSFX_TRI,     34, 244, 5 },
		{ ZSFX_PULSE12,  8, 215, 0 },
		{ ZSFX_TRI,     26, 250, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	mortar_rows, 2, "0101"
};
