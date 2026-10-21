/*
 * mausrun -- the sounds and the tunes. See sound.h.
 *
 * An effect is up to eight linear segments (waveform, frames, pitch
 * from and to, level from and to: docs/sfx.md). The tunes, one a theme:
 * Meadow, the one gamedemo always had (its drums now zsfx's kick, snare
 * and hat); Garden, Kitchen and Roofs (round 4). All four are laid out
 * as the arcade games' are -- tune, harmony, bass, drums -- so they can
 * be played calm first (zsfx_music_intensity(), mausrun.c).
 */

#include "sound.h"

static const zsfx_seg_t s_jump[] = {
	{ ZSFX_SQUARE, 6, 330, 660, 150, 110 },
};
static const zsfx_seg_t s_land[] = {
	{ ZSFX_TRI, 3, 160, 110, 120, 0 },
};
static const zsfx_seg_t s_coin[] = {
	{ ZSFX_SQUARE, 3, 988, 988, 170, 150 },
	{ ZSFX_SQUARE, 8, 1319, 1319, 160, 0 },
};
static const zsfx_seg_t s_ouch[] = {
	{ ZSFX_SQUARE, 4, 523, 392, 190, 170 },
	{ ZSFX_SQUARE, 10, 392, 262, 170, 0 },
};
static const zsfx_seg_t s_fall[] = {
	{ ZSFX_TRI, 30, 880, 110, 200, 40 },
	{ ZSFX_NOISE, 6, 600, 300, 120, 0 },
};
static const zsfx_seg_t s_spring[] = {
	{ ZSFX_TRI, 4, 196, 392, 220, 210 },
	{ ZSFX_TRI, 6, 392, 784, 210, 190 },
	{ ZSFX_TRI, 10, 784, 1175, 190, 0 },
};
static const zsfx_seg_t s_stomp[] = {
	{ ZSFX_SQUARE, 3, 262, 523, 200, 180 },
	{ ZSFX_SINE, 10, 1047, 784, 160, 0 },
};
static const zsfx_seg_t s_power[] = {
	{ ZSFX_SQUARE, 4, 523, 523, 170, 160 },
	{ ZSFX_SQUARE, 4, 659, 659, 170, 160 },
	{ ZSFX_SQUARE, 4, 784, 784, 170, 160 },
	{ ZSFX_SQUARE, 4, 1047, 1047, 170, 160 },
	{ ZSFX_SQUARE, 12, 1319, 1319, 160, 0 },
};
static const zsfx_seg_t s_pop[] = {
	{ ZSFX_NOISE, 3, 3000, 2000, 200, 0 },
	{ ZSFX_SINE, 8, 1568, 784, 150, 0 },
};
static const zsfx_seg_t s_splash[] = {
	{ ZSFX_NOISE, 4, 1800, 1200, 160, 140 },
	{ ZSFX_NOISE, 16, 1200, 400, 140, 0 },
};
static const zsfx_seg_t s_check[] = {
	{ ZSFX_SINE, 6, 784, 784, 190, 170 },
	{ ZSFX_SINE, 6, 988, 988, 190, 170 },
	{ ZSFX_SINE, 16, 1175, 1175, 180, 0 },
};
static const zsfx_seg_t s_goal[] = {
	{ ZSFX_SQUARE, 6, 523, 523, 180, 170 },
	{ ZSFX_SQUARE, 6, 659, 659, 180, 170 },
	{ ZSFX_SQUARE, 6, 784, 784, 180, 170 },
	{ ZSFX_SQUARE, 6, 1047, 1047, 180, 170 },
	{ ZSFX_SQUARE, 6, 784, 784, 180, 170 },
	{ ZSFX_SQUARE, 30, 1047, 1047, 180, 0 },
};
static const zsfx_seg_t s_heart[] = {
	{ ZSFX_SINE, 5, 784, 784, 200, 180 },
	{ ZSFX_SINE, 5, 1047, 1047, 200, 180 },
	{ ZSFX_SINE, 5, 1319, 1319, 200, 180 },
	{ ZSFX_SINE, 16, 1568, 1568, 190, 0 },
};
static const zsfx_seg_t s_bump[] = {
	{ ZSFX_TRI, 4, 220, 180, 220, 120 },
	{ ZSFX_SQUARE, 8, 880, 1319, 140, 0 },
};
static const zsfx_seg_t s_crumble[] = {
	{ ZSFX_NOISE, 12, 900, 300, 150, 0 },
};
static const zsfx_seg_t s_croak[] = {
	{ ZSFX_PULSE12, 4, 150, 120, 140, 120 },
	{ ZSFX_PULSE12, 6, 130, 100, 120, 0 },
};
static const zsfx_seg_t s_hoot[] = {
	{ ZSFX_SINE, 10, 392, 370, 140, 120 },
	{ ZSFX_SINE, 4, 370, 370, 0, 0 },
	{ ZSFX_SINE, 14, 392, 330, 140, 0 },
};
static const zsfx_seg_t s_wake[] = {
	{ ZSFX_TRI, 8, 660, 880, 120, 140 },
	{ ZSFX_TRI, 12, 880, 520, 140, 0 },
};
static const zsfx_seg_t s_menu[] = {
	{ ZSFX_PULSE25, 3, 784, 784, 120, 0 },
};
static const zsfx_seg_t s_select[] = {
	{ ZSFX_SQUARE, 3, 784, 784, 160, 150 },
	{ ZSFX_SQUARE, 8, 1175, 1175, 150, 0 },
};
static const zsfx_seg_t s_tick[] = {
	{ ZSFX_SQUARE, 2, 1568, 1568, 110, 0 },
};

#define FX(s, prio) { (prio), (uint8_t)(sizeof(s) / sizeof(s[0])), (s) }

static const zsfx_fx_t fx[SND_COUNT] = {
	[SND_JUMP]    = FX(s_jump, 1),
	[SND_LAND]    = FX(s_land, 0),
	[SND_COIN]    = FX(s_coin, 2),
	[SND_OUCH]    = FX(s_ouch, 5),
	[SND_FALL]    = FX(s_fall, 5),
	[SND_SPRING]  = FX(s_spring, 3),
	[SND_STOMP]   = FX(s_stomp, 4),
	[SND_POWER]   = FX(s_power, 4),
	[SND_POP]     = FX(s_pop, 4),
	[SND_SPLASH]  = FX(s_splash, 2),
	[SND_CHECK]   = FX(s_check, 3),
	[SND_GOAL]    = FX(s_goal, 5),
	[SND_HEART]   = FX(s_heart, 4),
	[SND_BUMP]    = FX(s_bump, 3),
	[SND_CRUMBLE] = FX(s_crumble, 2),
	[SND_CROAK]   = FX(s_croak, 1),
	[SND_HOOT]    = FX(s_hoot, 1),
	[SND_WAKE]    = FX(s_wake, 2),
	[SND_MENU]    = FX(s_menu, 1),
	[SND_SELECT]  = FX(s_select, 2),
	[SND_TICK]    = FX(s_tick, 1),
};

void snd_play(int which, int pan_x) {
	if (which < 0 || which >= SND_COUNT) return;
	zsfx_play(&fx[which], pan_x < 0 ? 0 : zsfx_pan_x(pan_x));
}

/* -- "Meadow": the tune the game has always had, A minor -- */

static const char *const meadow_rows[] = {
	"A-4 A-3 A-2 K--",
	"--- --- --- ---",
	"C-5 C-4 --- H--",
	"--- --- --- ---",
	"E-5 E-4 A-2 S--",
	"--- --- --- ---",
	"C-5 C-4 --- H--",
	"--- --- --- ---",
	"A-4 A-3 A-2 K--",
	"--- --- --- ---",
	"B-4 D-4 --- H--",
	"--- --- --- ---",
	"C-5 E-4 A-2 S--",
	"--- --- --- ---",
	"=== === --- H--",
	"--- --- --- ---",
	"F-4 A-3 F-2 K--",
	"--- --- --- ---",
	"A-4 C-4 --- H--",
	"--- --- --- ---",
	"C-5 F-4 F-2 S--",
	"--- --- --- ---",
	"A-4 C-4 --- H--",
	"--- --- --- ---",
	"F-4 A-3 F-2 K--",
	"--- --- --- ---",
	"G-4 B-3 --- H--",
	"--- --- --- ---",
	"A-4 C-4 F-2 S--",
	"--- --- --- ---",
	"=== === --- H--",
	"--- --- --- ---",
	"C-5 E-4 C-3 K--",
	"--- --- --- ---",
	"E-5 G-4 --- H--",
	"--- --- --- ---",
	"G-5 C-5 C-3 S--",
	"--- --- --- ---",
	"E-5 G-4 --- H--",
	"--- --- --- ---",
	"C-5 E-4 C-3 K--",
	"--- --- --- ---",
	"B-4 D-4 --- H--",
	"--- --- --- ---",
	"A-4 C-4 C-3 S--",
	"--- --- --- ---",
	"=== === --- H--",
	"--- --- --- ---",
	"G-4 B-3 G-2 K--",
	"--- --- --- ---",
	"B-4 D-4 --- H--",
	"--- --- --- ---",
	"D-5 G-4 G-2 S--",
	"--- --- --- ---",
	"B-4 D-4 --- H--",
	"--- --- --- ---",
	"E-5 G-4 G-2 K--",
	"--- --- --- ---",
	"D-5 B-3 --- H--",
	"--- --- --- ---",
	"C-5 E-4 G-2 S--",
	"--- --- --- ---",
	"=== === --- H--",
	"--- --- --- ---",
};

const zsfx_song_t snd_song_meadow = {
	480, 4,
	{
		{ ZSFX_SQUARE,  24, 244, 3 },
		{ ZSFX_PULSE25, 15, 240, 0 },
		{ ZSFX_TRI,     28, 250, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	meadow_rows, 4, "0123"
};

/* Round 4: a tune for each theme, generated once from melody, chords
 * and beats laid out by hand (the score below is the source now). */

/* -- "Garden": G major, bouncy -- */

static const char *const garden_rows[] = {
	"G-4 B-3 G-2 K--",
	"--- --- --- ---",
	"B-4 --- --- H--",
	"--- --- --- ---",
	"D-5 D-4 D-3 S--",
	"--- --- --- ---",
	"B-4 --- --- H--",
	"--- --- --- ---",
	"C-5 B-3 G-2 K--",
	"--- --- --- ---",
	"A-4 --- --- H--",
	"--- --- --- ---",
	"G-4 D-4 D-3 S--",
	"--- --- --- ---",
	"--- --- --- H--",
	"--- --- --- ---",
	"E-5 E-4 C-3 K--",
	"--- --- --- ---",
	"D-5 --- --- H--",
	"--- --- --- ---",
	"C-5 G-4 G-2 S--",
	"--- --- --- ---",
	"E-5 --- --- H--",
	"--- --- --- ---",
	"D-5 E-4 C-3 K--",
	"--- --- --- ---",
	"B-4 --- --- H--",
	"--- --- --- ---",
	"A-4 G-4 G-2 S--",
	"--- --- --- ---",
	"--- --- --- H--",
	"--- --- --- ---",
	"F#5 F#4 D-3 K--",
	"--- --- --- ---",
	"E-5 --- --- H--",
	"--- --- --- ---",
	"D-5 A-4 A-2 S--",
	"--- --- --- ---",
	"A-4 --- --- H--",
	"--- --- --- ---",
	"B-4 F#4 D-3 K--",
	"--- --- --- ---",
	"C-5 --- --- H--",
	"--- --- --- ---",
	"D-5 A-4 A-2 S--",
	"--- --- --- ---",
	"--- --- --- H--",
	"--- --- --- ---",
	"B-4 D-4 G-2 K--",
	"--- --- --- ---",
	"D-5 --- --- H--",
	"--- --- --- ---",
	"G-5 B-3 D-3 S--",
	"--- --- --- ---",
	"D-5 --- --- H--",
	"--- --- --- ---",
	"B-4 D-4 G-2 K--",
	"--- --- --- ---",
	"A-4 --- --- H--",
	"--- --- --- ---",
	"G-4 B-3 D-3 S--",
	"--- --- --- ---",
	"=== --- --- O--",
	"--- --- --- ---",
};

const zsfx_song_t snd_song_garden = {
	480, 4,
	{
		{ ZSFX_SQUARE,  22, 236, 3 },
		{ ZSFX_PULSE25, 14, 228, 0 },
		{ ZSFX_TRI,     28, 246, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	garden_rows, 4, "0123"
};

/* -- "Kitchen": C major, plucked: everything short -- */

static const char *const kitchen_rows[] = {
	"C-5 E-4 C-3 K--",
	"=== === --- ---",
	"E-5 --- === H--",
	"=== --- --- ---",
	"G-5 G-4 G-2 S--",
	"=== === --- ---",
	"E-5 --- === H--",
	"=== --- --- ---",
	"F-5 E-4 C-3 K--",
	"=== === --- ---",
	"D-5 --- === H--",
	"=== --- --- ---",
	"B-4 G-4 G-2 S--",
	"=== === --- ---",
	"G-4 --- === H--",
	"=== --- --- ---",
	"A-4 F-4 F-2 K--",
	"=== === --- ---",
	"C-5 --- === H--",
	"=== --- --- ---",
	"F-5 A-4 C-3 S--",
	"=== === --- ---",
	"C-5 --- === H--",
	"=== --- --- ---",
	"A-4 F-4 F-2 K--",
	"=== === --- ---",
	"G-4 --- === H--",
	"=== --- --- ---",
	"F-4 A-4 C-3 S--",
	"--- === --- ---",
	"--- --- === H--",
	"=== --- --- ---",
	"B-4 D-4 G-2 K--",
	"=== === --- ---",
	"D-5 --- === H--",
	"=== --- --- ---",
	"G-5 G-4 D-3 S--",
	"=== === --- ---",
	"F-5 --- === H--",
	"=== --- --- ---",
	"E-5 D-4 G-2 K--",
	"=== === --- ---",
	"D-5 --- === H--",
	"=== --- --- ---",
	"C-5 G-4 D-3 S--",
	"=== === --- ---",
	"B-4 --- === H--",
	"=== --- --- ---",
	"C-5 E-4 C-3 K--",
	"=== === --- ---",
	"G-4 --- === H--",
	"=== --- --- ---",
	"E-4 G-4 G-2 S--",
	"=== === --- ---",
	"G-4 --- === H--",
	"=== --- --- ---",
	"C-5 E-4 C-3 K--",
	"--- === --- ---",
	"--- --- === H--",
	"--- --- --- ---",
	"=== G-4 G-2 S--",
	"--- === --- ---",
	"--- --- === H--",
	"--- --- --- ---",
};

const zsfx_song_t snd_song_kitchen = {
	456, 4,
	{
		{ ZSFX_PULSE12, 22, 210, 0 },
		{ ZSFX_PULSE25, 12, 200, 0 },
		{ ZSFX_TRI,     28, 230, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	kitchen_rows, 4, "0123"
};

/* -- "Roofs": D minor, a lullaby over the chimneys -- */

static const char *const roofs_rows[] = {
	"D-5 A-4 D-3 K--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"F-5 --- --- H--",
	"--- --- --- ---",
	"E-5 --- --- ---",
	"--- --- --- ---",
	"D-5 F-4 A-2 S--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"A-4 --- --- H--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"F-5 D-4 A#2 K--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"D-5 --- --- H--",
	"--- --- --- ---",
	"C-5 --- --- ---",
	"--- --- --- ---",
	"A#4 F-4 F-2 S--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"D-5 --- --- H--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"E-5 E-4 C-3 K--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"G-5 --- --- H--",
	"--- --- --- ---",
	"F-5 --- --- ---",
	"--- --- --- ---",
	"E-5 G-4 G-2 S--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"C-5 --- --- H--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"C#5 E-4 A-2 K--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"E-5 --- --- H--",
	"--- --- --- ---",
	"D-5 --- --- ---",
	"--- --- --- ---",
	"C#5 A-4 E-2 S--",
	"--- --- --- ---",
	"--- --- --- ---",
	"--- --- --- ---",
	"A-4 --- --- H--",
	"--- --- --- ---",
	"=== --- --- ---",
	"--- --- --- ---",
};

const zsfx_song_t snd_song_roofs = {
	420, 4,
	{
		{ ZSFX_SINE,    26, 250, 5 },
		{ ZSFX_TRI,     14, 252, 0 },
		{ ZSFX_TRI,     26, 252, 0 },
		{ ZSFX_DRUMS,  255,   0, 0 },
	},
	roofs_rows, 4, "0123"
};

const zsfx_song_t *snd_song(int music) {
	switch (music) {
	case 1: return &snd_song_garden;
	case 2: return &snd_song_kitchen;
	case 3: return &snd_song_roofs;
	default: return &snd_song_meadow;
	}
}
