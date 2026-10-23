/*
 * planedemo -- the song: 32 bars of 128 BPM in A minor, about a minute.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Written as the film is cut (docs/planedemo.md, "The story"), a bar at
 * a time; the notes are built here at start-up into one sorted list.
 *
 *   bars  scene              music
 *    1-2  the clock          ticks; the hands fall (a clatter); a bell a letter
 *    3-4  ignition           a pad, a riser, the drums come in
 *    5-8  the storm          everything but the lead; thunder at 5 and 7
 *   9-12  the mouse          plucked arpeggios join
 *  13-16  the donut          the hook
 *  17-20  the tunnel         driving: sixteenth bass, open hats
 *  21-24  vectors            the breakdown, and a build
 *  25-28  sunrise            the hook in C major
 *  29-32  the end            the last phrase, a break, A major to close
 */

#include <string.h>

#include "pd_music.h"

static pd_note_t *out;
static int n, cap;

static void add(int row, int c, int in, int note, int vel, int len, int slide) {
	if (n >= cap || row < 0) return;
	out[n].row = (uint16_t)row;
	out[n].ch = (uint8_t)c;
	out[n].inst = (uint8_t)in;
	out[n].note = (int8_t)note;
	out[n].vel = (uint8_t)vel;
	out[n].len = (uint8_t)len;
	out[n].slide = (int8_t)slide;
	n++;
}

/* "a4", "c#5", "eb3" -> MIDI; -1 if not a note */
static int midi(const char *s) {
	static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* a b c d e f g */
	int v, oct;
	if (*s < 'a' || *s > 'g') return -1;
	v = base[*s - 'a'];
	s++;
	if (*s == '#') { v++; s++; }
	else if (*s == 'b' && s[1] >= '0' && s[1] <= '9') { v--; s++; }
	if (*s < '0' || *s > '9') return -1;
	oct = *s - '0';
	return (oct + 1) * 12 + v;
}

/* sixteen characters: x a hit, X an accent, . nothing */
static void beat(int bar, int c, int in, const char *pat, int vel) {
	int i;
	for (i = 0; i < 16 && pat[i]; i++) {
		if (pat[i] == '.') continue;
		add(bar * 16 + i, c, in, 60, pat[i] == 'X' ? 127 : vel, 0, 0);
	}
}

/* sixteen tokens a bar: a note, - to hold it, . a rest */
static void tune(int bar, int c, int in, const char *s, int vel) {
	int row = 0, start = -1, note = -1;
	char tok[8];
	while (*s && row <= 16) {
		int k = 0;
		while (*s == ' ') s++;
		if (!*s) break;
		while (*s && *s != ' ' && k < 7) tok[k++] = *s++;
		tok[k] = 0;
		if (tok[0] == '-') { row++; continue; }
		if (start >= 0) add(bar * 16 + start, c, in, note, vel, row - start, 0);
		start = -1;
		if (tok[0] != '.') { note = midi(tok); start = row; }
		row++;
	}
	if (start >= 0) add(bar * 16 + start, c, in, note, vel, row - start, 0);
}

/* the chords: root (MIDI, octave 2), and the third */
typedef struct { int root, third; } chord_t;
#define AM { 45, 3 }
#define F_ { 41, 4 }
#define C_ { 48, 4 }
#define G_ { 43, 4 }
#define DM { 38, 3 }
#define AJ { 45, 4 }      /* A major: the last chord */

static const chord_t chords[PD_BARS] = {
	AM, AM, AM, F_,         /* clock, ignition */
	AM, F_, C_, G_,         /* storm */
	AM, F_, C_, G_,         /* mouse */
	AM, F_, C_, G_,         /* donut */
	AM, AM, F_, G_,         /* tunnel */
	F_, F_, G_, G_,         /* vectors */
	C_, G_, AM, F_,         /* sunrise */
	F_, G_, AM, AJ,         /* the end */
};

static int tone(const chord_t *k, int i) {
	/* chord tones upward: root, third, fifth, octave, ... */
	static const int step[4] = { 0, -1, 7, 12 };
	int o = i / 4, t = i % 4;
	return k->root + o * 12 + (t == 1 ? k->third : step[t]);
}

static void pad(int bar, int bars, int vel) {
	const chord_t *k = &chords[bar];
	int len = bars * 16;
	/* the pad is one sample with its own fifth in it: the root and the
	 * third make the chord */
	add(bar * 16, PD_CH_PAD, PD_I_PAD, k->root + 12, vel, len, 0);
	(void)k;
}

static void bass8(int bar, int vel) {
	const chord_t *k = &chords[bar];
	static const char p[] = "r.r.o.r.r.r.o.r.";
	int i;
	for (i = 0; i < 16; i++) {
		if (p[i] == '.') continue;
		add(bar * 16 + i, PD_CH_BASS, PD_I_BASS, k->root - 12 + (p[i] == 'o' ? 12 : 0),
			vel + (i % 4 == 0 ? 10 : 0), 1, 0);
	}
}

static void bass16(int bar, int vel) {
	const chord_t *k = &chords[bar];
	int i;
	for (i = 0; i < 16; i++)
		add(bar * 16 + i, PD_CH_BASS, PD_I_BASS, k->root - 12 + ((i % 4) == 2 ? 12 : 0),
			vel + (i % 4 == 0 ? 12 : 0), 1, 0);
}

static void arp(int bar, int vel, int up) {
	static const int order[16] = { 0, 1, 2, 3, 4, 3, 2, 1, 0, 1, 2, 3, 5, 4, 3, 2 };
	const chord_t *k = &chords[bar];
	int i;
	for (i = 0; i < 16; i++)
		add(bar * 16 + i, PD_CH_ARP, PD_I_PLUCK, tone(k, order[i]) + 24 + up,
			vel + (i % 4 == 0 ? 15 : 0), 1, 0);
}

static void drums(int bar, int kick, int snare, int hats) {
	if (kick) beat(bar, PD_CH_KICK, PD_I_KICK, "X...x...x...x...", 115);
	if (snare == 1) beat(bar, PD_CH_SNARE, PD_I_SNARE, "....x.......x...", 110);
	if (snare == 2) beat(bar, PD_CH_SNARE, PD_I_CLAP, "....x.......x...", 115);
	if (hats == 1) beat(bar, PD_CH_HAT, PD_I_HATC, "..x...x...x...x.", 90);
	if (hats == 2) beat(bar, PD_CH_HAT, PD_I_HATC, "x.xxx.xxx.xxx.xx", 70);
	if (hats == 3) beat(bar, PD_CH_HAT, PD_I_HATO, "..x...x...x...x.", 85);
}

static void roll(int bar, int from) {
	int i;
	for (i = from; i < 16; i++)
		add(bar * 16 + i, PD_CH_SNARE, PD_I_SNARE, 60, 50 + (i - from) * 70 / (16 - from), 0, 0);
}

int pd_song_build(pd_note_t *o, int c) {
	int b, i;
	out = o; cap = c; n = 0;

	/* -- 1-2: the clock. Ticks on the beat; on the first beat of bar 2
	 * the hands fall; then a bell for each letter of zeitlos. -- */
	for (i = 0; i < 4; i++) add(i * 4, PD_CH_HAT, PD_I_TICK, 60, 100, 0, 0);
	pad(0, 2, 55);
	add(16, PD_CH_FX, PD_I_CLATTER, 60, 127, 0, 0);
	add(16, PD_CH_BASS, PD_I_BASS, 33, 120, 6, 0);
	{
		static const char *const word[7] = { "a4", "c5", "d5", "e5", "g5", "a5", "c6" };
		for (i = 0; i < 7; i++) add(20 + i, PD_CH_LEAD, PD_I_BELL, midi(word[i]) + 0, 95, 0, 0);
	}

	/* -- 3-4: ignition. A riser over two bars, the pad, the drums
	 * gathering, a snare roll into the storm. -- */
	add(32, PD_CH_FX, PD_I_NOISE, 57, 90, 32, 24);
	pad(2, 1, 75);
	pad(3, 1, 80);
	beat(3, PD_CH_KICK, PD_I_KICK, "x...x...x...x...", 100);
	beat(3, PD_CH_HAT, PD_I_HATC, "..x...x...x...x.", 70);
	roll(3, 8);

	/* -- 5-8: the storm -- */
	for (b = 4; b < 8; b++) {
		drums(b, 1, 1, 1);
		bass8(b, 100);
		pad(b, 1, 70);
	}
	add(4 * 16, PD_CH_FX, PD_I_THUNDER, 60, 127, 0, 0);
	add(6 * 16, PD_CH_FX, PD_I_THUNDER, 55, 110, 0, 0);

	/* -- 9-12: the mouse -- */
	for (b = 8; b < 12; b++) {
		drums(b, 1, 1, 2);
		bass8(b, 105);
		arp(b, 70, 0);
		pad(b, 1, 60);
	}

	/* -- 13-16: the donut, and the hook -- */
	for (b = 12; b < 16; b++) {
		drums(b, 1, 2, 2);
		bass8(b, 110);
		arp(b, 55, 0);
		pad(b, 1, 55);
	}
	tune(12, PD_CH_LEAD, PD_I_LEAD, "e5 - e5 d5 c5 - a4 - . . c5 d5 e5 - g5 -", 110);
	tune(13, PD_CH_LEAD, PD_I_LEAD, "f5 - e5 - c5 - a4 - . . a4 c5 f5 - e5 -", 110);
	tune(14, PD_CH_LEAD, PD_I_LEAD, "e5 - e5 d5 c5 - g4 - . . c5 d5 e5 - g5 -", 110);
	tune(15, PD_CH_LEAD, PD_I_LEAD, "d5 - - - b4 - g4 - b4 - d5 - g5 - f5 -", 110);

	/* -- 17-20: the tunnel: driving -- */
	for (b = 16; b < 20; b++) {
		drums(b, 1, 1, 3);
		beat(b, PD_CH_SNARE, PD_I_CLAP, "....x.......x..x", 95);
		bass16(b, 95);
		arp(b, 65, 12);
	}
	pad(16, 2, 65);
	pad(18, 1, 65);
	pad(19, 1, 65);

	/* -- 21-24: vectors: the breakdown, then a build -- */
	for (b = 20; b < 23; b++) {
		pad(b, 1, 85);
		arp(b, 50, 0);
		beat(b, PD_CH_HAT, PD_I_HATC, "..x...x...x...x.", 50);
	}
	tune(20, PD_CH_LEAD, PD_I_BELL, "a5 . . . c6 . . . e6 . . . c6 . . .", 70);
	tune(21, PD_CH_LEAD, PD_I_BELL, "a5 . . . c6 . . . f6 . . . e6 . . .", 70);
	tune(22, PD_CH_LEAD, PD_I_BELL, "b5 . . . d6 . . . g6 . . . d6 . . .", 70);
	pad(23, 1, 90);
	beat(23, PD_CH_KICK, PD_I_KICK, "x...x...x.x.xxxx", 105);
	roll(23, 0);
	add(23 * 16, PD_CH_FX, PD_I_NOISE, 57, 95, 16, 24);
	bass8(23, 90);

	/* -- 25-28: sunrise: the hook in C major -- */
	for (b = 24; b < 28; b++) {
		drums(b, 1, 2, 2);
		beat(b, PD_CH_SNARE, PD_I_SNARE, "....x.......x...", 70);
		bass8(b, 110);
		arp(b, 55, 0);
		pad(b, 1, 60);
	}
	add(24 * 16, PD_CH_FX, PD_I_HATO, 60, 127, 0, 0);
	tune(24, PD_CH_LEAD, PD_I_LEAD, "e5 - g5 - c6 - - - b5 - g5 - e5 - d5 -", 115);
	tune(25, PD_CH_LEAD, PD_I_LEAD, "d5 - - - b4 - d5 - g5 - - - f5 - e5 -", 115);
	tune(26, PD_CH_LEAD, PD_I_LEAD, "c5 - e5 - a5 - - - g5 - e5 - c5 - d5 -", 115);
	tune(27, PD_CH_LEAD, PD_I_LEAD, "c5 - - - a4 - c5 - f5 - - - e5 - - -", 115);

	/* -- 29-32: the end -- */
	for (b = 28; b < 30; b++) {
		drums(b, 1, 2, 2);
		bass8(b, 110);
		arp(b, 50, 0);
		pad(b, 1, 60);
	}
	tune(28, PD_CH_LEAD, PD_I_LEAD, "a5 - - - g5 - f5 - e5 - - - c5 - - -", 115);
	tune(29, PD_CH_LEAD, PD_I_LEAD, "d5 - - - - - e5 - g5 - - - - - - -", 115);
	/* a bar of breath: the bass and the pad alone, and a tick */
	add(30 * 16, PD_CH_BASS, PD_I_BASS, 33, 110, 8, 0);
	pad(30, 1, 70);
	for (i = 0; i < 4; i++) add(30 * 16 + i * 4, PD_CH_HAT, PD_I_TICK, 60, 80, 0, 0);
	/* A major, and everything rings */
	add(31 * 16, PD_CH_KICK, PD_I_KICK, 60, 127, 0, 0);
	add(31 * 16, PD_CH_FX, PD_I_HATO, 60, 110, 0, 0);
	add(31 * 16, PD_CH_BASS, PD_I_BASS, 33, 120, 16, 0);
	add(31 * 16, PD_CH_PAD, PD_I_PAD, 57, 95, 16, 0);
	add(31 * 16, PD_CH_LEAD, PD_I_LEAD, midi("c#6"), 110, 16, 0);
	add(31 * 16, PD_CH_ARP, PD_I_BELL, midi("a5"), 100, 0, 0);
	add(31 * 16 + 4, PD_CH_ARP, PD_I_BELL, midi("e6"), 80, 0, 0);
	add(31 * 16 + 8, PD_CH_ARP, PD_I_BELL, midi("a6"), 60, 0, 0);

	/* sorted by row, stable: insertion sort is plenty for this */
	for (b = 1; b < n; b++) {
		pd_note_t t = out[b];
		i = b - 1;
		while (i >= 0 && out[i].row > t.row) { out[i + 1] = out[i]; i--; }
		out[i + 1] = t;
	}
	return n;
}
