/*
 * blocks -- the sounds and the music (sw/common/games/zsfx.h).
 *
 * Effects are a few segments of one waveform each; the tunes are ASCII,
 * four channels left to right: lead, harmony, bass, drums. Both tunes
 * are original.
 */

#include "../../common/games/zsfx.h"

#include "bk_sound.h"

static const zsfx_seg_t s_move[]   = { { ZSFX_PULSE12, 2, 700, 700, 70, 0 } };
static const zsfx_seg_t s_rotate[] = { { ZSFX_TRI, 3, 520, 780, 140, 0 } };
static const zsfx_seg_t s_kick[]   = {
	{ ZSFX_TRI, 2, 520, 600, 140, 120 },
	{ ZSFX_TRI, 3, 900, 1100, 120, 0 },
};
static const zsfx_seg_t s_lock[]   = { { ZSFX_NOISE, 3, 900, 500, 120, 0 } };
static const zsfx_seg_t s_land[]   = { { ZSFX_TRI, 2, 200, 160, 90, 0 } };
static const zsfx_seg_t s_hard[]   = {
	{ ZSFX_NOISE, 2, 1400, 900, 220, 160 },
	{ ZSFX_TRI, 8, 140, 50, 220, 0 },
};
static const zsfx_seg_t s_hold[]   = { { ZSFX_NOISE, 6, 600, 2400, 60, 120 },
	{ ZSFX_NOISE, 4, 2400, 3000, 120, 0 } };
static const zsfx_seg_t s_clear1[] = {
	{ ZSFX_SQUARE, 3, 784, 784, 170, 150 },
	{ ZSFX_SQUARE, 8, 1047, 1047, 160, 0 },
};
static const zsfx_seg_t s_clear2[] = {
	{ ZSFX_SQUARE, 3, 784, 784, 180, 160 },
	{ ZSFX_SQUARE, 3, 988, 988, 170, 150 },
	{ ZSFX_SQUARE, 8, 1175, 1175, 160, 0 },
};
static const zsfx_seg_t s_clear3[] = {
	{ ZSFX_SQUARE, 3, 784, 784, 190, 170 },
	{ ZSFX_SQUARE, 3, 988, 988, 180, 160 },
	{ ZSFX_SQUARE, 3, 1175, 1175, 170, 150 },
	{ ZSFX_SQUARE, 10, 1568, 1568, 170, 0 },
};
static const zsfx_seg_t s_four[] = {
	{ ZSFX_SAW, 4, 392, 392, 220, 200 },
	{ ZSFX_SAW, 4, 523, 523, 220, 200 },
	{ ZSFX_SAW, 4, 659, 659, 220, 200 },
	{ ZSFX_SAW, 4, 784, 784, 220, 200 },
	{ ZSFX_SAW, 4, 1047, 1047, 220, 200 },
	{ ZSFX_SAW, 24, 1319, 1290, 220, 0 },
};
static const zsfx_seg_t s_twist[] = {
	{ ZSFX_SINE, 3, 1319, 1568, 200, 190 },
	{ ZSFX_SINE, 3, 1568, 1976, 190, 180 },
	{ ZSFX_SINE, 3, 1976, 2349, 180, 170 },
	{ ZSFX_SINE, 12, 2637, 2637, 170, 0 },
};
static const zsfx_seg_t s_perfect[] = {
	{ ZSFX_SQUARE, 4, 523, 523, 220, 200 },
	{ ZSFX_SQUARE, 4, 659, 659, 220, 200 },
	{ ZSFX_SQUARE, 4, 784, 784, 220, 200 },
	{ ZSFX_SQUARE, 4, 1047, 1047, 220, 200 },
	{ ZSFX_SQUARE, 4, 1319, 1319, 220, 200 },
	{ ZSFX_SQUARE, 4, 1568, 1568, 220, 200 },
	{ ZSFX_SQUARE, 4, 2093, 2093, 220, 200 },
	{ ZSFX_SQUARE, 30, 2637, 2600, 220, 0 },
};
static const zsfx_seg_t s_level[] = {
	{ ZSFX_PULSE25, 4, 587, 587, 200, 180 },
	{ ZSFX_PULSE25, 4, 784, 784, 200, 180 },
	{ ZSFX_PULSE25, 4, 988, 988, 200, 180 },
	{ ZSFX_PULSE25, 16, 1175, 1175, 200, 0 },
};
static const zsfx_seg_t s_over[] = {
	{ ZSFX_TRI, 12, 392, 392, 220, 190 },
	{ ZSFX_TRI, 12, 370, 370, 210, 180 },
	{ ZSFX_TRI, 12, 349, 349, 200, 170 },
	{ ZSFX_TRI, 40, 330, 300, 190, 0 },
};
static const zsfx_seg_t s_done[] = {
	{ ZSFX_SQUARE, 6, 784, 784, 210, 190 },
	{ ZSFX_SQUARE, 6, 988, 988, 210, 190 },
	{ ZSFX_SQUARE, 6, 1175, 1175, 210, 190 },
	{ ZSFX_SQUARE, 6, 1568, 1568, 210, 190 },
	{ ZSFX_SQUARE, 30, 1568, 1568, 200, 0 },
};
static const zsfx_seg_t s_count[] = { { ZSFX_PULSE25, 8, 660, 660, 160, 0 } };
static const zsfx_seg_t s_go[]    = { { ZSFX_PULSE25, 18, 1320, 1320, 180, 0 } };
static const zsfx_seg_t s_menu[]  = { { ZSFX_PULSE12, 3, 1200, 1250, 120, 0 } };
static const zsfx_seg_t s_select[] = {
	{ ZSFX_SQUARE, 3, 880, 880, 180, 160 },
	{ ZSFX_SQUARE, 8, 1319, 1319, 170, 0 },
};
static const zsfx_seg_t s_letter[] = { { ZSFX_PULSE25, 2, 1568, 1568, 110, 0 } };

/* a rising whoosh, then a little chime: saved */
static const zsfx_seg_t s_rescue[] = {
	{ ZSFX_NOISE, 18, 300, 3200, 40, 160 },
	{ ZSFX_NOISE, 10, 3200, 4000, 160, 0 },
	{ ZSFX_SINE, 6, 1047, 1047, 190, 170 },
	{ ZSFX_SINE, 6, 1319, 1319, 190, 170 },
	{ ZSFX_SINE, 16, 1568, 1568, 180, 0 },
};

static const zsfx_fx_t fx[BK_SND_N] = {
	[BK_SND_MOVE]    = { 0, 1, s_move },
	[BK_SND_ROTATE]  = { 0, 1, s_rotate },
	[BK_SND_KICK]    = { 0, 2, s_kick },
	[BK_SND_LOCK]    = { 1, 1, s_lock },
	[BK_SND_LAND]    = { 0, 1, s_land },
	[BK_SND_RESCUE]  = { 5, 5, s_rescue },
	[BK_SND_HARD]    = { 2, 2, s_hard },
	[BK_SND_HOLD]    = { 1, 2, s_hold },
	[BK_SND_CLEAR1]  = { 3, 2, s_clear1 },
	[BK_SND_CLEAR2]  = { 3, 3, s_clear2 },
	[BK_SND_CLEAR3]  = { 3, 4, s_clear3 },
	[BK_SND_FOUR]    = { 4, 6, s_four },
	[BK_SND_TWIST]   = { 4, 4, s_twist },
	[BK_SND_PERFECT] = { 5, 8, s_perfect },
	[BK_SND_LEVEL]   = { 4, 4, s_level },
	[BK_SND_OVER]    = { 5, 4, s_over },
	[BK_SND_DONE]    = { 5, 5, s_done },
	[BK_SND_COUNT]   = { 2, 1, s_count },
	[BK_SND_GO]      = { 2, 1, s_go },
	[BK_SND_MENU]    = { 1, 1, s_menu },
	[BK_SND_SELECT]  = { 2, 2, s_select },
	[BK_SND_LETTER]  = { 1, 1, s_letter },
};

void bk_sound(int which) {
	if (which < 0 || which >= BK_SND_N) return;
	zsfx_play(&fx[which], 0);
}

/* A combo rises a step each time: eight steps of a major scale. */
void bk_sound_combo(int combo) {
	static const uint16_t scale[8] = { 784, 880, 988, 1047, 1175, 1319, 1480, 1568 };
	static zsfx_seg_t seg[8][2];
	static zsfx_fx_t cfx[8];
	int k = combo - 1;
	if (k < 0) return;
	if (k > 7) k = 7;
	seg[k][0].wave = ZSFX_SINE;
	seg[k][0].frames = 3;
	seg[k][0].f0 = seg[k][0].f1 = scale[k];
	seg[k][0].v0 = 160; seg[k][0].v1 = 150;
	seg[k][1].wave = ZSFX_SINE;
	seg[k][1].frames = 10;
	seg[k][1].f0 = seg[k][1].f1 = (uint16_t)(scale[k] * 2);
	seg[k][1].v0 = 150; seg[k][1].v1 = 0;
	cfx[k].prio = 2;
	cfx[k].n = 2;
	cfx[k].seg = seg[k];
	zsfx_play(&cfx[k], 0);
}

/* -- "Lattice", the game tune: G minor, quick -- */

static const char *const lattice_rows[] = {
	/* 0: G minor, E flat */
	"G-5 D-5 G-2 K--",
	"--- --- --- H--",
	"D-5 A#4 G-3 H--",
	"--- --- --- H--",
	"G-5 D-5 G-2 S--",
	"A#5 --- --- H--",
	"A-5 A#4 G-3 H--",
	"--- --- --- O--",
	"G-5 D#5 D#2 K--",
	"--- --- --- H--",
	"D#5 A#4 D#3 K--",
	"--- --- --- H--",
	"G-5 G-4 D#2 S--",
	"--- --- --- H--",
	"F-5 A#4 D#3 H--",
	"D#5 --- --- O--",
	/* 1: B flat, F */
	"D-5 A#4 A#1 K--",
	"--- --- --- H--",
	"F-5 D-5 A#2 H--",
	"--- --- --- H--",
	"A#5 F-5 A#1 S--",
	"--- --- --- H--",
	"A-5 D-5 A#2 H--",
	"F-5 --- --- O--",
	"C-6 A-4 F-2 K--",
	"--- --- --- H--",
	"A-5 F-5 F-3 K--",
	"--- --- --- H--",
	"F-5 C-5 F-2 S--",
	"G-5 --- --- H--",
	"A-5 A-4 F-3 S--",
	"=== --- --- S--",
	/* 2: G minor high, C minor */
	"A#5 G-5 G-2 K--",
	"--- --- --- H--",
	"A-5 D-5 G-3 H--",
	"G-5 --- --- H--",
	"D-6 A#4 G-2 S--",
	"--- --- --- H--",
	"C-6 D-5 G-3 H--",
	"A#5 --- --- O--",
	"C-6 G-5 C-3 K--",
	"--- --- --- H--",
	"A#5 D#5 C-3 K--",
	"--- --- --- H--",
	"A-5 C-5 C-2 S--",
	"G-5 --- --- H--",
	"A-5 D#5 C-3 H--",
	"--- --- --- O--",
	/* 3: D, to turn around */
	"F#5 D-5 D-2 K--",
	"--- --- --- H--",
	"A-5 C-5 D-3 H--",
	"--- --- --- H--",
	"D-6 F#5 D-2 S--",
	"--- --- --- H--",
	"C-6 A-4 D-3 H--",
	"A-5 --- --- O--",
	"F#5 D-5 D-2 K--",
	"--- --- --- S--",
	"D-5 C-5 D-3 K--",
	"--- --- --- S--",
	"E-5 A-4 F#2 S--",
	"F#5 --- --- S--",
	"A-5 C-5 A-2 S--",
	"--- --- --- S--",
};

const zsfx_song_t bk_song_game = {
	552, 4,
	{
		{ ZSFX_SQUARE,  20, 236, 3 },
		{ ZSFX_PULSE12, 10, 215, 0 },
		{ ZSFX_TRI,     30, 246, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	lattice_rows, 4, "01012303"
};

/* -- "Drift", the title tune: C major, unhurried -- */

static const char *const drift_rows[] = {
	"E-5 C-5 C-3 K--",
	"--- G-4 --- ---",
	"G-5 E-5 --- H--",
	"--- G-4 --- ---",
	"C-6 C-5 G-2 ---",
	"--- G-4 --- ---",
	"B-5 E-5 --- H--",
	"--- G-4 --- ---",
	"A-5 A-4 F-2 K--",
	"--- C-5 --- ---",
	"G-5 F-5 --- H--",
	"--- C-5 --- ---",
	"F-5 A-4 C-3 ---",
	"--- C-5 --- ---",
	"E-5 F-5 --- H--",
	"--- C-5 --- ---",

	"D-5 B-4 G-2 K--",
	"--- D-5 --- ---",
	"G-5 G-4 --- H--",
	"--- D-5 --- ---",
	"B-5 B-4 D-3 ---",
	"--- D-5 --- ---",
	"A-5 G-4 --- H--",
	"--- D-5 --- ---",
	"G-5 E-5 C-3 K--",
	"--- G-4 --- ---",
	"E-5 C-5 --- H--",
	"--- G-4 --- ---",
	"D-5 E-5 G-2 ---",
	"--- G-4 --- ---",
	"C-5 C-5 --- H--",
	"=== G-4 --- ---",
};

const zsfx_song_t bk_song_title = {
	384, 4,
	{
		{ ZSFX_SINE,    36, 244, 5 },
		{ ZSFX_PULSE12,  8, 215, 0 },
		{ ZSFX_TRI,     26, 250, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	drift_rows, 2, "0101"
};
