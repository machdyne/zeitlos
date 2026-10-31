/*
 * Zeitlos mahjong -- the music and the sounds. See mj_music.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Every row is three cells: the tune, the harmony, the bass (zsfx's
 * format, docs/sfx.md). Four rows a second; the order plays A B A C
 * A B A D, about half a minute before it comes round.
 */

#include "mj_music.h"

static const char *const rows[] = {
	/* 0: A -- up the scale and back */
	"G-4 B-3 G-2",
	"--- --- ---",
	"A-4 --- ---",
	"--- --- ---",
	"B-4 D-4 ---",
	"--- --- ---",
	"D-5 --- D-3",
	"--- --- ---",
	"E-5 G-4 ---",
	"--- --- ---",
	"D-5 --- E-2",
	"--- --- ---",
	"B-4 E-4 ---",
	"--- --- ---",
	"A-4 --- ---",
	"--- --- ---",

	/* 1: B -- higher, to the fifth */
	"D-5 B-4 C-3",
	"--- --- ---",
	"E-5 --- ---",
	"--- --- ---",
	"G-5 E-4 ---",
	"--- --- ---",
	"E-5 --- G-2",
	"--- --- ---",
	"D-5 B-4 D-3",
	"--- --- ---",
	"B-4 --- ---",
	"--- --- ---",
	"A-4 F#4 D-2",
	"--- --- ---",
	"--- --- ---",
	"=== --- ---",

	/* 2: C -- the relative minor, E */
	"E-5 G-4 E-2",
	"--- --- ---",
	"D-5 --- ---",
	"B-4 --- ---",
	"D-5 B-4 B-2",
	"--- --- ---",
	"E-5 --- ---",
	"--- --- ---",
	"G-5 E-4 C-3",
	"--- --- ---",
	"E-5 --- ---",
	"D-5 --- ---",
	"E-5 G-4 A-2",
	"--- --- ---",
	"--- --- ---",
	"--- --- ---",

	/* 3: D -- home */
	"D-5 B-4 D-3",
	"--- --- ---",
	"B-4 --- ---",
	"A-4 --- ---",
	"G-4 D-4 B-2",
	"--- --- ---",
	"A-4 --- ---",
	"B-4 --- ---",
	"A-4 C-4 D-3",
	"--- --- ---",
	"--- --- ---",
	"E-4 --- ---",
	"G-4 B-3 G-2",
	"--- --- ---",
	"--- --- ---",
	"=== === ---",
};

const zsfx_song_t mj_song = {
	240, 3,                                 /* four rows a second, three channels */
	{
		{ ZSFX_SINE, 26, 250, 3 },      /* the tune: soft, a breath of vibrato */
		{ ZSFX_TRI,  12, 247, 0 },      /* the harmony, under it */
		{ ZSFX_TRI,  22, 249, 0 },      /* the bass */
		{ 0, 0, 0, 0 },
	},
	rows, 4, "01020103"
};

/* -- the sounds -- */

static const zsfx_seg_t s_pick[] = {
	{ ZSFX_TRI, 2, 1500, 1300, 110, 0 },            /* a tap of ivory */
};
static const zsfx_seg_t s_take[] = {
	{ ZSFX_NOISE, 2, 4000, 2500, 150, 60 },         /* the clack of two tiles */
	{ ZSFX_TRI,   5,  900,  640, 130, 0 },
};
static const zsfx_seg_t s_nope[] = {
	{ ZSFX_SQUARE, 5, 196, 165, 70, 0 },
};
static const zsfx_seg_t s_win[] = {
	{ ZSFX_TRI, 6,  392,  392, 150, 130 },          /* G B D G, the tune's key */
	{ ZSFX_TRI, 6,  494,  494, 150, 130 },
	{ ZSFX_TRI, 6,  587,  587, 150, 130 },
	{ ZSFX_TRI, 18, 784,  784, 160, 0 },
};
static const zsfx_seg_t s_shuffle[] = {
	{ ZSFX_NOISE, 4, 1500, 3000, 60, 90 },
	{ ZSFX_NOISE, 6, 3000, 1500, 90, 0 },
};

const zsfx_fx_t mj_fx[] = {
	{ 0, 0, 0 },
	{ 1, 1, s_pick },
	{ 2, 2, s_take },
	{ 1, 1, s_nope },
	{ 3, 4, s_win },
	{ 2, 2, s_shuffle },
};
const int mj_nfx = (int)(sizeof(mj_fx) / sizeof(mj_fx[0]));
