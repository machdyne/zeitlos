/*
 * snake -- the sounds and the music (sw/common/games/zsfx.h).
 *
 * Every effect is a few segments of one waveform; every tune is ASCII,
 * read top to bottom, four channels left to right: lead, harmony, bass,
 * drums. Both tunes are original.
 */

#include "../../common/games/zsfx.h"

#include "sn_sound.h"

/* -- effects -- */

static const zsfx_seg_t s_eat[] = {
	{ ZSFX_SQUARE, 3,  659,  659, 200, 170 },
	{ ZSFX_SQUARE, 3,  784,  784, 180, 150 },
	{ ZSFX_SQUARE, 6, 1047, 1047, 160,   0 },
};
static const zsfx_seg_t s_bonus_eat[] = {
	{ ZSFX_SINE, 2, 1047, 1047, 220, 200 },
	{ ZSFX_SINE, 2, 1319, 1319, 210, 190 },
	{ ZSFX_SINE, 2, 1568, 1568, 200, 180 },
	{ ZSFX_SINE, 2, 2093, 2093, 200, 180 },
	{ ZSFX_SINE, 10, 2093, 2637, 180,   0 },
};
static const zsfx_seg_t s_bonus_show[] = {
	{ ZSFX_SINE, 3, 1568, 1568, 140, 140 },
	{ ZSFX_SINE, 8, 2093, 2093, 140,   0 },
};
static const zsfx_seg_t s_bonus_gone[] = {
	{ ZSFX_TRI, 12, 784, 392, 160, 0 },
};
static const zsfx_seg_t s_die_crash[] = {
	{ ZSFX_NOISE, 4, 1600, 1200, 255, 230 },
	{ ZSFX_NOISE, 24, 1200, 150, 230, 0 },
};
static const zsfx_seg_t s_die_fall[] = {
	{ ZSFX_SAW, 36, 440, 55, 200, 0 },
};
static const zsfx_seg_t s_pop[] = {
	{ ZSFX_NOISE, 3, 3200, 1800, 110, 0 },
};
static const zsfx_seg_t s_level[] = {
	{ ZSFX_SQUARE, 3,  523,  523, 200, 180 },
	{ ZSFX_SQUARE, 3,  659,  659, 200, 180 },
	{ ZSFX_SQUARE, 3,  784,  784, 200, 180 },
	{ ZSFX_SQUARE, 3, 1047, 1047, 200, 180 },
	{ ZSFX_SQUARE, 3, 1319, 1319, 200, 180 },
	{ ZSFX_SQUARE, 3, 1568, 1568, 200, 180 },
	{ ZSFX_SQUARE, 16, 2093, 2093, 200, 0 },
};
static const zsfx_seg_t s_menu[] = {
	{ ZSFX_PULSE12, 3, 1200, 1250, 120, 0 },
};
static const zsfx_seg_t s_select[] = {
	{ ZSFX_SQUARE, 3, 880, 880, 180, 160 },
	{ ZSFX_SQUARE, 8, 1319, 1319, 170, 0 },
};
static const zsfx_seg_t s_fanfare[] = {
	{ ZSFX_SQUARE, 5,  784,  784, 200, 180 },
	{ ZSFX_SQUARE, 5, 1047, 1047, 200, 180 },
	{ ZSFX_SQUARE, 5, 1319, 1319, 200, 180 },
	{ ZSFX_SQUARE, 20, 1568, 1568, 200, 0 },
};
static const zsfx_seg_t s_gameover[] = {
	{ ZSFX_TRI, 10, 392, 392, 220, 190 },
	{ ZSFX_TRI, 10, 330, 330, 210, 180 },
	{ ZSFX_TRI, 10, 262, 262, 200, 170 },
	{ ZSFX_TRI, 30, 196, 185, 190, 0 },
};
static const zsfx_seg_t s_ready[] = {
	{ ZSFX_PULSE25, 6, 659, 659, 160, 0 },
	{ ZSFX_PULSE25, 6, 0, 0, 0, 0 },
	{ ZSFX_PULSE25, 10, 1319, 1319, 170, 0 },
};
static const zsfx_seg_t s_letter[] = {
	{ ZSFX_PULSE25, 2, 1568, 1568, 110, 0 },
};

static const zsfx_fx_t fx[SN_SND_COUNT] = {
	[SN_SND_EAT]        = { 2, 3, s_eat },
	[SN_SND_BONUS_EAT]  = { 3, 5, s_bonus_eat },
	[SN_SND_BONUS_SHOW] = { 1, 2, s_bonus_show },
	[SN_SND_BONUS_GONE] = { 1, 1, s_bonus_gone },
	[SN_SND_DIE_CRASH]  = { 5, 2, s_die_crash },
	[SN_SND_DIE_FALL]   = { 5, 1, s_die_fall },
	[SN_SND_POP]        = { 0, 1, s_pop },
	[SN_SND_LEVEL]      = { 4, 7, s_level },
	[SN_SND_MENU]       = { 1, 1, s_menu },
	[SN_SND_SELECT]     = { 2, 2, s_select },
	[SN_SND_FANFARE]    = { 4, 4, s_fanfare },
	[SN_SND_GAMEOVER]   = { 4, 4, s_gameover },
	[SN_SND_READY]      = { 3, 3, s_ready },
	[SN_SND_LETTER]     = { 1, 1, s_letter },
};

void sn_sound(int which, int x) {
	if (which < 0 || which >= SN_SND_COUNT) return;
	zsfx_play(&fx[which], x < 0 ? 0 : zsfx_pan_x(x));
}

/* -- "Slither", the game tune: E minor, 8.8 rows a second -- */

static const char *const slither_rows[] = {
	/* 0: E minor */
	"E-5 B-4 E-2 K--",
	"--- --- --- H--",
	"G-5 E-4 E-3 H--",
	"--- --- --- H--",
	"B-5 G-4 E-2 S--",
	"--- --- --- H--",
	"A-5 E-4 E-3 H--",
	"G-5 --- --- H--",
	"F#5 B-4 E-2 K--",
	"--- --- --- H--",
	"E-5 G-4 E-3 K--",
	"--- --- --- H--",
	"D-5 E-4 E-2 S--",
	"E-5 --- --- H--",
	"=== B-4 E-3 H--",
	"--- --- --- O--",
	/* 1: C, D */
	"C-5 G-4 C-2 K--",
	"--- --- --- H--",
	"E-5 C-5 C-3 H--",
	"--- --- --- H--",
	"G-5 E-4 C-2 S--",
	"--- --- --- H--",
	"E-5 C-5 C-3 H--",
	"--- --- --- H--",
	"D-5 A-4 D-2 K--",
	"--- --- --- H--",
	"F#5 D-5 D-3 K--",
	"--- --- --- H--",
	"A-5 F#4 D-2 S--",
	"G-5 --- --- H--",
	"F#5 D-5 D-3 H--",
	"D-5 --- --- O--",
	/* 2: E minor, higher */
	"B-5 G-4 E-2 K--",
	"--- --- --- H--",
	"A-5 E-4 E-3 H--",
	"G-5 --- --- H--",
	"F#5 B-4 E-2 S--",
	"--- --- --- H--",
	"G-5 E-4 E-3 H--",
	"A-5 --- --- H--",
	"B-5 G-4 G-2 K--",
	"--- --- --- H--",
	"D-6 B-4 G-3 K--",
	"--- --- --- H--",
	"B-5 G-4 G-2 S--",
	"A-5 --- --- H--",
	"G-5 D-5 G-3 H--",
	"--- --- --- O--",
	/* 3: C, then B to turn around */
	"E-5 C-5 C-2 K--",
	"--- --- --- H--",
	"G-5 E-4 C-3 H--",
	"--- --- --- H--",
	"E-5 C-5 C-2 S--",
	"D-5 --- --- H--",
	"C-5 G-4 C-3 H--",
	"--- --- --- H--",
	"B-4 D#5 B-1 K--",
	"--- --- --- H--",
	"D#5 F#4 B-2 K--",
	"--- --- --- H--",
	"F#5 A-4 B-1 S--",
	"--- --- --- S--",
	"A-5 D#5 B-2 S--",
	"B-5 --- --- S--",
};

const zsfx_song_t sn_song_game = {
	528, 4,
	{
		{ ZSFX_PULSE25, 22, 238, 4 },
		{ ZSFX_SQUARE,  10, 225, 0 },
		{ ZSFX_TRI,     30, 248, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	slither_rows, 4, "01230123"
};

/* -- "Garden", the title tune: C major, unhurried -- */

static const char *const garden_rows[] = {
	"C-5 E-4 C-3 K--",
	"--- G-4 --- ---",
	"E-5 C-5 --- H--",
	"--- G-4 --- ---",
	"G-5 E-4 G-2 ---",
	"--- G-4 --- ---",
	"E-5 C-5 --- H--",
	"--- G-4 --- ---",
	"A-4 E-4 A-2 K--",
	"--- A-4 --- ---",
	"C-5 C-5 --- H--",
	"--- A-4 --- ---",
	"E-5 E-4 E-3 ---",
	"--- A-4 --- ---",
	"D-5 C-5 --- H--",
	"--- A-4 --- ---",

	"F-4 F-4 F-2 K--",
	"--- A-4 --- ---",
	"A-4 C-5 --- H--",
	"--- A-4 --- ---",
	"C-5 F-4 C-3 ---",
	"--- A-4 --- ---",
	"A-4 C-5 --- H--",
	"--- A-4 --- ---",
	"G-4 G-4 G-2 K--",
	"--- B-4 --- ---",
	"B-4 D-5 --- H--",
	"--- B-4 --- ---",
	"D-5 G-4 D-3 ---",
	"--- B-4 --- ---",
	"B-4 D-5 --- H--",
	"=== B-4 --- ---",
};

const zsfx_song_t sn_song_title = {
	400, 4,
	{
		{ ZSFX_TRI,     34, 244, 5 },
		{ ZSFX_PULSE12,  8, 215, 0 },
		{ ZSFX_TRI,     26, 250, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	garden_rows, 2, "0101"
};
