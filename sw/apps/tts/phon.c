/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * From phonemes to synthesiser frames. See phon.h.
 *
 * -- Where the numbers come from --
 *
 * The phoneme targets below are this project's own, set from the
 * standard published ranges of acoustic phonetics -- vowel formant
 * averages for adult male speakers, consonant loci, the spectral
 * shape of each fricative and burst -- and then adjusted by ear and
 * by the recogniser check in tests/ (docs/tts.md, "Tuning"). No
 * existing synthesiser's tables were used.
 *
 * -- How an utterance is built --
 *
 * 1. Each phoneme becomes one or more SEGMENTS, each with target
 *    parameters and a length in frames (synth.h: ~5ms each). A stop is
 *    a closure plus a burst, and puts aspiration at the start of the
 *    vowel that follows it -- voice onset time, which is most of the
 *    difference between "pat" and "bat". A diphthong is two targets
 *    with a glide between them.
 *
 * 2. At each boundary the formants meet at a BOUNDARY VALUE. Between
 *    a consonant and a vowel it is the consonant's locus moved part
 *    of the way toward the vowel (its `k`), which is what makes /d/
 *    before "ee" and before "oo" sound like the same /d/. Formants
 *    glide from the boundary to the target over the first part of a
 *    segment, and toward the next boundary over the last part.
 *
 * 3. Amplitudes ramp from the previous segment's over a few frames,
 *    short enough to keep bursts crisp.
 *
 * 4. Pitch declines gently over the utterance, rises on stressed
 *    vowels, falls at the end of a sentence and rises at the end of a
 *    question.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "phon.h"

enum { K_PAUSE, K_VOWEL, K_SEMI, K_NASAL, K_FRIC, K_STOP, K_AFFR, K_ASP };

typedef struct {
	char		nm[3];
	uint8_t		kind;
	uint8_t		voiced;
	uint8_t		dur;			// ms: stressed vowel, or consonant (a stop's closure)
	uint16_t	f1, f2, f3;		// target, or locus
	uint16_t	e1, e2, e3;		// diphthong end target, or 0
	uint16_t	b1, b2, b3;
	uint8_t		av, af;
	uint8_t		a2, a3, a4, a5, a6, ab;	// frication spectrum, or a stop's burst
	uint8_t		k;				// boundary: % of the way from locus to vowel
} ph_t;

#define V(n, d, f1, f2, f3, b1, b2, b3) \
	{ n, K_VOWEL, 1, d, f1, f2, f3, 0, 0, 0, b1, b2, b3, 60, 0, 0,0,0,0,0,0, 50 }
#define D(n, d, f1, f2, f3, e1, e2, e3) \
	{ n, K_VOWEL, 1, d, f1, f2, f3, e1, e2, e3, 80, 100, 150, 60, 0, 0,0,0,0,0,0, 50 }

static const ph_t phonemes[] = {

	// -- vowels --
	V("IY", 150, 280, 2250, 2900,  60,  90, 150),
	V("IH",  95, 400, 1920, 2560,  70, 100, 150),
	V("EH", 110, 550, 1770, 2490,  70, 100, 150),
	V("AE", 165, 690, 1660, 2490,  90, 100, 150),
	V("AA", 165, 720, 1100, 2540,  90, 100, 150),
	V("AO", 165, 590,  880, 2540,  90,  90, 150),
	V("UH",  95, 450, 1100, 2350,  70, 100, 150),
	V("UW", 150, 310,  900, 2300,  60,  90, 150),
	V("AH", 100, 600, 1170, 2390,  80, 100, 150),
	V("ER", 165, 470, 1270, 1540,  70, 100, 110),
	V("AX",  60, 500, 1400, 2400,  70, 100, 150),
	V("IX",  60, 420, 1800, 2550,  70, 100, 150),

	// -- diphthongs: start target, then end target --
	D("EY", 180, 480, 2000, 2600,  330, 2200, 2800),
	D("OW", 175, 520,  950, 2400,  390,  780, 2300),
	D("AY", 210, 700, 1200, 2500,  420, 1950, 2560),
	D("AW", 215, 700, 1200, 2500,  450,  850, 2300),
	D("OY", 225, 560,  860, 2500,  420, 1850, 2550),

	// -- semivowels: vowel-like, slow transitions --
	{ "W",  K_SEMI, 1, 70, 290,  650, 2200, 0,0,0,  50,  80, 150, 56, 0, 0,0,0,0,0,0, 50 },
	{ "Y",  K_SEMI, 1, 70, 270, 2100, 3000, 0,0,0,  50,  90, 180, 56, 0, 0,0,0,0,0,0, 50 },
	{ "R",  K_SEMI, 1, 70, 330, 1100, 1450, 0,0,0,  70, 100, 120, 56, 0, 0,0,0,0,0,0, 50 },
	{ "L",  K_SEMI, 1, 70, 360, 1050, 2800, 0,0,0,  60, 100, 200, 55, 0, 0,0,0,0,0,0, 50 },

	// -- nasals: low damped F1, heavily damped upper formants --
	{ "M",  K_NASAL, 1, 75, 280, 1000, 2200, 0,0,0, 60, 300, 500, 52, 0, 0,0,0,0,0,0, 35 },
	{ "N",  K_NASAL, 1, 70, 280, 1600, 2600, 0,0,0, 60, 300, 500, 52, 0, 0,0,0,0,0,0, 35 },
	{ "NG", K_NASAL, 1, 75, 280, 2000, 2700, 0,0,0, 60, 300, 500, 52, 0, 0,0,0,0,0,0, 55 },

	// -- fricatives: spectrum from the parallel branch --
	//                                                     av  af  a2 a3 a4 a5 a6 ab
	{ "S",  K_FRIC, 0, 110, 250, 1700, 2700, 0,0,0, 200, 150, 250,  0, 60,  0, 0, 42, 48, 58, 0, 50 },
	{ "Z",  K_FRIC, 1,  90, 250, 1700, 2700, 0,0,0, 200, 150, 250, 48, 55,  0, 0, 42, 48, 58, 0, 50 },
	{ "SH", K_FRIC, 0, 110, 250, 1900, 2500, 0,0,0, 200, 150, 250,  0, 60,  0, 58, 52, 45, 40, 0, 50 },
	{ "ZH", K_FRIC, 1,  90, 250, 1900, 2500, 0,0,0, 200, 150, 250, 48, 55,  0, 58, 52, 45, 40, 0, 50 },
	{ "F",  K_FRIC, 0, 100, 300, 1100, 2100, 0,0,0, 200, 150, 250,  0, 55,  0, 0, 0, 30, 35, 50, 50 },
	{ "V",  K_FRIC, 1,  80, 300, 1100, 2100, 0,0,0, 200, 150, 250, 50, 50,  0, 0, 0, 30, 35, 48, 50 },
	{ "TH", K_FRIC, 0, 100, 300, 1400, 2700, 0,0,0, 200, 150, 250,  0, 52,  0, 0, 0, 35, 42, 48, 50 },
	{ "DH", K_FRIC, 1,  70, 300, 1400, 2700, 0,0,0, 200, 150, 250, 50, 45,  0, 0, 0, 35, 40, 45, 50 },

	// -- /h/: aspiration shaped by the following vowel --
	{ "HH", K_ASP,  0,  65, 500, 1500, 2500, 0,0,0, 300, 200, 250,  0,  0,  0,0,0,0,0,0, 50 },

	// -- stops: dur is the closure; a2..ab is the burst --
	{ "P",  K_STOP, 0,  70, 200,  900, 2100, 0,0,0,  80, 100, 150,  0, 60, 42, 0,  0,  0,  0, 58, 40 },
	{ "B",  K_STOP, 1,  60, 200,  900, 2100, 0,0,0,  80, 100, 150,  0, 58, 42, 0,  0,  0,  0, 55, 40 },
	{ "T",  K_STOP, 0,  60, 200, 1700, 2700, 0,0,0,  80, 100, 150,  0, 60,  0, 0, 45, 52, 58,  0, 35 },
	{ "D",  K_STOP, 1,  50, 200, 1700, 2700, 0,0,0,  80, 100, 150,  0, 58,  0, 0, 45, 52, 56,  0, 35 },
	{ "K",  K_STOP, 0,  70, 250, 1900, 2400, 0,0,0,  80, 100, 150,  0, 60, 50, 58, 45,  0,  0,  0, 60 },
	{ "G",  K_STOP, 1,  60, 250, 1900, 2400, 0,0,0,  80, 100, 150,  0, 58, 50, 56, 45,  0,  0,  0, 60 },

	// -- affricates: a stop's closure, then a short fricative --
	{ "CH", K_AFFR, 0,  60, 250, 1900, 2500, 0,0,0, 200, 150, 250,  0, 60,  0, 58, 52, 45, 40, 0, 50 },
	{ "JH", K_AFFR, 1,  50, 250, 1900, 2500, 0,0,0, 200, 150, 250, 48, 55,  0, 58, 52, 45, 40, 0, 50 },

};

#define NPHON (int)(sizeof(phonemes) / sizeof(phonemes[0]))

// -- prosody parameters --
//
// Every number that shapes timing and pitch, in one table. The
// defaults are the hand-set values the rules were written with; a
// speech pack's PROSODY section replaces them with values fitted to
// a real speaker (tools/speech/lib/prosody.py, the speech-data pipeline docs, published with it).
// Out-of-range values from a pack are clamped here, never trusted.
static int16_t pro[PHON_PRO_N];

static const struct { int16_t def, lo, hi; } pro_limits[PHON_PRO_N] = {
	[PHON_PRO_UNSTRESSED]	= {  55,  30,  90 },	// % of a stressed vowel
	[PHON_PRO_FINAL_COMMA]	= { 135, 100, 220 },	// % before a comma
	[PHON_PRO_FINAL_STOP]	= { 155, 100, 250 },	// % before a full stop
	[PHON_PRO_PAUSE_COMMA]	= { 190,  60, 500 },	// ms
	[PHON_PRO_PAUSE_STOP]	= { 300, 120, 900 },
	[PHON_PRO_PAUSE_PARA]	= { 420, 200, 1500 },
	[PHON_PRO_TOP]		= { 112,  95, 140 },	// % of base pitch
	[PHON_PRO_DECL]		= {  17,   0,  40 },	// % over a phrase
	[PHON_PRO_STEP]		= {   4,   0,  12 },	// % lower per phrase
	[PHON_PRO_STEP_MAX]	= {  16,   0,  40 },
	[PHON_PRO_STRESS1]	= {  17,   0,  40 },	// % lift, primary
	[PHON_PRO_STRESS2]	= {   8,   0,  30 },	// % lift, secondary
	[PHON_PRO_COMMA_RISE]	= {  13, -10,  40 },	// % over a phrase's end
	[PHON_PRO_STOP_FALL]	= {  10,   0,  40 },
	[PHON_PRO_OPEN]		= {  50,  35,  80 },	// % of each period
	[PHON_PRO_TILT]		= {   0,   0,  24 },	// dB at 3kHz
};

// Per-phoneme durations from a pack; 0 means the table's own.
static uint16_t dur_fit[64];

// Per-phoneme formants from a pack: F1-F3, then a diphthong's end
// targets E1-E3. 0 means the table's own. Measured from a real speaker
// and normalised to this voice's tract (tools/speech/lib/acoustics.py).
static uint16_t fmt_fit[64][6];

void phon_prosody_reset(void) {
	for (int i = 0; i < PHON_PRO_N; i++) pro[i] = pro_limits[i].def;
	for (int i = 0; i < 64; i++) {
		dur_fit[i] = 0;
		for (int k = 0; k < 6; k++) fmt_fit[i][k] = 0;
	}
}

void phon_prosody_set(int field, int value) {
	if (field < 0 || field >= PHON_PRO_N) return;
	if (value < pro_limits[field].lo) value = pro_limits[field].lo;
	if (value > pro_limits[field].hi) value = pro_limits[field].hi;
	pro[field] = (int16_t)value;
}

int phon_prosody_get(int field) {
	if (field < 0 || field >= PHON_PRO_N) return 0;
	if (!pro[PHON_PRO_TOP]) phon_prosody_reset();
	return pro[field];
}

// -- segments --

enum { S_SIL, S_V, S_C };

typedef struct {
	synth_frame_t	t;			// targets; f0 unused
	uint16_t		nf;			// frames
	uint8_t			kind;
	uint8_t			k;
	uint8_t			tf;			// formant transition, frames
	uint8_t			ta;			// amplitude ramp at the start, frames
	uint8_t			stress;
	uint8_t			vot;		// frames of aspiration replacing voicing at the start
	uint8_t			phrase;		// which phrase this belongs to
	uint8_t			phrase_end;	// 0 no, 1 comma (more to come), 2 full stop
	uint16_t		tok;		// which token (phoneme) this came from
} seg_t;

#define MAX_SEGS	512

static seg_t segs[MAX_SEGS];
static int nsegs;

// boundary formants: bnd[i] is where seg i-1 and seg i meet
static uint16_t bnd_f1[MAX_SEGS + 1], bnd_f2[MAX_SEGS + 1], bnd_f3[MAX_SEGS + 1];

static int cur_seg;

// Experiments: see phon.h. Off unless the rendering harness sets them.
static uint32_t exp_bits;
#define EXP(b)	(exp_bits & (b))

void phon_set_experiments(uint32_t bits) {
	exp_bits = bits;
}

uint32_t phon_experiment(const char *name) {
	static const struct { const char *n; uint32_t b; } tab[] = {
		{ "vowel-voicing", PHON_EXP_VOWEL_VOICING },
		{ "vot", PHON_EXP_VOT },
		{ "velar", PHON_EXP_VELAR },
		{ "nasal", PHON_EXP_NASAL },
	};
	for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (strcmp(tab[i].n, name) == 0) return tab[i].b;
	return 0;
}

static bool is_front(const ph_t *v) {
	static const char *const front[] = { "IY", "IH", "EY", "EH", "AE", "Y" };
	for (unsigned i = 0; i < sizeof(front) / sizeof(front[0]); i++)
		if (v->nm[0] == front[i][0] && v->nm[1] == front[i][1]) return true;
	return false;
}
static uint32_t phrase_frames[256];		// frames in each phrase
static uint16_t seg_into_phrase[MAX_SEGS];		// frames of its phrase before each segment
static uint32_t formant_pct = 100, expression_pct = 100;
static uint32_t gain_db;		// voicing turned down for a higher voice
static uint32_t tract_db;		// every source turned down for a smaller tract

// 20*log10(num/den) in whole dB, without floating point: count the
// 1dB steps (x1.122) between the two. Once per utterance.
static uint32_t db_above(uint32_t num, uint32_t den) {
	uint32_t db = 0;
	uint32_t r = num * 1000u / (den ? den : 1);
	while (r >= 1122u && db < 40) {
		r = r * 1000u / 1122u;
		db++;
	}
	return db;
}
static int last_tok = -1;		// the token behind the frame phon_next() last returned
static int cur_j;
static uint32_t frame_no, frames_total;
static uint32_t base_f0;			// Hz * 16
static bool question, continues;
static synth_frame_t prev_amp;		// the previous segment's amplitudes

// -- parsing --

static const ph_t *lookup(const char *tok, int n) {
	for (int i = 0; i < NPHON; i++) {
		const char *nm = phonemes[i].nm;
		int l = (int)strlen(nm);
		if (l != n) continue;
		int j;
		for (j = 0; j < n; j++) {
			char c = tok[j];
			if (c >= 'a' && c <= 'z') c -= 32;
			if (c != nm[j]) break;
		}
		if (j == n) return &phonemes[i];
	}
	return 0;
}

// Parsed phonemes, before expansion: the table entry (NULL for a
// pause), stress, and a pause length.
typedef struct {
	const ph_t *p;
	uint8_t stress;
	// uint16_t, not uint8_t: this was a byte until the pauses were
	// measured, and a sentence's 280ms had been wrapping to 24ms --
	// which is most of why sentences ran into each other. Pauses of a
	// paragraph (420ms) need the room too.
	uint16_t pause_ms;
	uint8_t asp;		// frames of aspiration a voiceless stop hands to the next vowel
} tok_t;

#define MAX_TOKS	384
static tok_t toks[MAX_TOKS];
static int ntoks;

static void parse(const char *s) {

	ntoks = 0;
	question = false;

	while (*s && ntoks < MAX_TOKS) {

		while (*s == ' ' || *s == '\t' || *s == '\n') s++;
		if (!*s) break;

		char c = *s;
		if (c == '_' || c == ',' || c == '.' || c == '?' || c == '!' ||
		    c == ';' || c == ':' || c == '|') {
			tok_t *t = &toks[ntoks++];
			t->p = 0;
			t->stress = 0;
			t->asp = 0;
			// A sentence gets a longer pause than a comma, and a
			// paragraph longer still: over a page of prose these are
			// what let a listener keep their place.
			t->pause_ms = (c == '_') ? 60
				: (c == ',' || c == ';' || c == ':') ? (uint16_t)pro[PHON_PRO_PAUSE_COMMA]
				: (c == '|') ? (uint16_t)pro[PHON_PRO_PAUSE_PARA]
				: (uint16_t)pro[PHON_PRO_PAUSE_STOP];
			if (c == '?') question = true;
			else if (c == '.' || c == '!') question = false;
			s++;
			continue;
		}

		const char *st = s;
		while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
		int n = (int)(s - st);
		uint8_t stress = 1;
		if (*s >= '0' && *s <= '2') stress = (uint8_t)(*s++ - '0');
		if (n == 0) { s++; continue; }		// junk character

		const ph_t *p = lookup(st, n);
		if (!p) continue;

		tok_t *t = &toks[ntoks++];
		t->p = p;
		t->stress = stress;
		t->pause_ms = 0;
		t->asp = 0;

	}

}

// -- expansion --

static uint32_t rate_q8;		// duration scale, Q8: 256 = normal

static uint16_t frames_for(uint32_t ms, bool consonant) {
	// Consonants compress less than vowels as speech speeds up -- a
	// burst or a closure has a floor below which it stops being heard.
	uint32_t scale = consonant ? (128 + rate_q8 / 2) : rate_q8;
	uint32_t nf = (ms * scale / 256 + 2) / 5;
	return (uint16_t)(nf ? nf : 1);
}

static seg_t *new_seg(void) {
	if (nsegs >= MAX_SEGS) return 0;
	seg_t *s = &segs[nsegs++];
	memset(s, 0, sizeof(*s));
	return s;
}

static void set_formants(seg_t *s, uint16_t f1, uint16_t f2, uint16_t f3,
	uint16_t b1, uint16_t b2, uint16_t b3) {
	s->t.f1 = f1; s->t.f2 = f2; s->t.f3 = f3;
	s->t.b1 = b1; s->t.b2 = b2; s->t.b3 = b3;
}

static void set_par(seg_t *s, const ph_t *p) {
	s->t.a2 = p->a2; s->t.a3 = p->a3; s->t.a4 = p->a4;
	s->t.a5 = p->a5; s->t.a6 = p->a6; s->t.ab = p->ab;
}

static bool is_vocalic(const tok_t *t) {
	return t && t->p && (t->p->kind == K_VOWEL || t->p->kind == K_SEMI);
}

static bool is_voiced_sound(const tok_t *t) {
	return t && t->p && t->p->voiced &&
		(t->p->kind == K_VOWEL || t->p->kind == K_SEMI || t->p->kind == K_NASAL);
}

// A phoneme's formant `k` (0-2: F1-F3, 3-5: a diphthong's end),
// fitted if a pack gave one, the table's otherwise.
static uint16_t ph_f(const ph_t *p, int k) {
	int i = (int)(p - phonemes);
	if (i >= 0 && i < 64 && fmt_fit[i][k]) return fmt_fit[i][k];
	switch (k) {
	case 0: return p->f1;
	case 1: return p->f2;
	case 2: return p->f3;
	case 3: return p->e1;
	case 4: return p->e2;
	default: return p->e3;
	}
}

// Fitted formants for phoneme `name`. Each value is checked against the
// table's: a pack is data from elsewhere, and a vowel moved more than
// 40% from where this voice has it is a measurement gone wrong, not a
// speaker. 0 keeps the table's value.
bool phon_prosody_formants(const char *name, const uint16_t f[6]) {
	for (int i = 0; i < NPHON && i < 64; i++) {
		const ph_t *p = &phonemes[i];
		if (p->nm[0] != name[0] || p->nm[1] != (name[1] ? name[1] : 0)) continue;
		const uint16_t tab[6] = { p->f1, p->f2, p->f3, p->e1, p->e2, p->e3 };
		for (int k = 0; k < 6; k++) {
			fmt_fit[i][k] = 0;
			if (!f[k] || !tab[k]) continue;
			uint32_t lo = tab[k] * 60u / 100u, hi = tab[k] * 140u / 100u;
			if (f[k] >= lo && f[k] <= hi) fmt_fit[i][k] = f[k];
		}
		return true;
	}
	return false;
}

static uint32_t ph_dur(const ph_t *p) {
	int i = (int)(p - phonemes);
	if (i >= 0 && i < 64 && dur_fit[i]) return dur_fit[i];
	return p->dur;
}

// A fitted duration for phoneme `name`, in ms. False if there is no
// such phoneme or the value is not believable.
bool phon_prosody_dur(const char *name, uint32_t ms) {
	if (ms < 15 || ms > 400) return false;
	for (int i = 0; i < NPHON && i < 64; i++) {
		if (phonemes[i].nm[0] == name[0] &&
		    phonemes[i].nm[1] == (name[1] ? name[1] : 0)) {
			dur_fit[i] = (uint16_t)ms;
			return true;
		}
	}
	return false;
}

static void expand(void) {

	nsegs = 0;
	uint8_t phrase = 0;

	for (int i = 0; i < ntoks; i++) {

		tok_t *tk = &toks[i];
		const tok_t *prev = i > 0 ? &toks[i - 1] : 0;
		const tok_t *next = i + 1 < ntoks ? &toks[i + 1] : 0;
		const ph_t *p = tk->p;
		seg_t *s;
		int first = nsegs;		// the first segment this phoneme creates

		int seg0 = nsegs;

		if (!p) {
			// A pause ends a phrase. Which kind matters: a comma is
			// "there is more", a full stop is "that was all", and
			// they are said with opposite pitch.
			if (!(s = new_seg())) return;
			s->kind = S_SIL;
			s->nf = frames_for(tk->pause_ms, false);
			s->ta = 2;
			s->phrase = phrase;
			s->tok = (uint16_t)i;
			// Mark the WHOLE phrase, not just the segment next to the
			// pause: the contour is applied over the last part of the
			// phrase, and the segment that happens to sit there may be
			// a stop closure a few milliseconds long.
			uint8_t kind = (tk->pause_ms >= 250) ? 2 : 1;
			for (int j = nsegs - 2; j >= 0 && segs[j].phrase == phrase; j--)
				segs[j].phrase_end = kind;
			if (phrase < 255) phrase++;
			continue;
		}

		switch (p->kind) {

		case K_VOWEL: {
			uint32_t ms = ph_dur(p);
			if (tk->stress == 0) ms = ms * (uint32_t)pro[PHON_PRO_UNSTRESSED] / 100;
			else if (tk->stress == 2) ms = ms * 80 / 100;

			// EXPERIMENT vowel-voicing: English marks a final
			// consonant's voicing mostly on the VOWEL before it --
			// "hat" is short, "had" long. The obstruent is a coda when
			// no vowel follows it.
			if (EXP(PHON_EXP_VOWEL_VOICING) && next && next->p &&
			    (next->p->kind == K_STOP || next->p->kind == K_FRIC ||
			     next->p->kind == K_AFFR)) {
				const tok_t *n2 = (i + 2 < ntoks) ? &toks[i + 2] : 0;
				if (!n2 || !n2->p || n2->p->kind != K_VOWEL)
					ms = ms * (next->p->voiced ? 115u : 70u) / 100u;
			}
			// The last vowel before a pause is drawn out, as people do
			// at the end of a phrase -- and more at the end of a
			// sentence than at a comma. This is most of what makes a
			// paragraph sound like phrases rather than a list of
			// words.
			//
			// The LAST VOWEL of the phrase, whatever consonants follow
			// it: "analysis." lengthens its final I though an S comes
			// after. This used to test only whether the very next
			// sound was the pause, which lengthened nothing but words
			// ending in a vowel -- found when the fitted value came
			// back as "no lengthening at all".
			{
				int k = i + 1;
				while (k < ntoks && toks[k].p && toks[k].p->kind != K_VOWEL) k++;
				bool final = (k >= ntoks) || !toks[k].p;
				if (final) {
					bool comma = k < ntoks && toks[k].pause_ms &&
						toks[k].pause_ms < 250;
					ms = ms * (uint32_t)(comma ? pro[PHON_PRO_FINAL_COMMA]
						: pro[PHON_PRO_FINAL_STOP]) / 100;
				}
			}
			uint16_t nf = frames_for(ms, false);

			if (!(s = new_seg())) return;
			s->kind = S_V;
			s->stress = tk->stress;
			s->t.av = p->av;
			s->ta = 3;
			s->tf = 8;
			set_formants(s, ph_f(p, 0), ph_f(p, 1), ph_f(p, 2), p->b1, p->b2, p->b3);

			if (ph_f(p, 3)) {
				// Diphthong: the second half glides all the way.
				uint16_t n1 = (uint16_t)(nf * 45 / 100);
				if (n1 < 1) n1 = 1;
				s->nf = n1;
				seg_t *s2 = new_seg();
				if (!s2) return;
				*s2 = *s;
				s2->nf = (uint16_t)(nf > n1 ? nf - n1 : 1);
				s2->ta = 0;
				s2->tf = (uint8_t)(s2->nf < 255 ? s2->nf : 255);
				s2->t.f1 = ph_f(p, 3); s2->t.f2 = ph_f(p, 4); s2->t.f3 = ph_f(p, 5);
			} else {
				s->nf = nf;
			}
			break;
		}

		case K_SEMI:
		case K_NASAL:
			if (!(s = new_seg())) return;
			s->kind = (p->kind == K_SEMI) ? S_V : S_C;
			s->k = p->k;
			s->nf = frames_for(ph_dur(p), true);
			s->t.av = p->av;
			s->ta = (p->kind == K_SEMI) ? 4 : 2;
			s->tf = (p->kind == K_SEMI) ? 12 : 6;
			// EXPERIMENT nasal: a nasal's release into a vowel is
			// abrupt -- the velum opens, and the sound changes at once
			// -- where L and W glide. Transitions as slow as a glide's
			// made N heard as L.
			if (EXP(PHON_EXP_NASAL) && p->kind == K_NASAL) {
				s->tf = 2;
				s->k = 70;
			}
			set_formants(s, ph_f(p, 0), ph_f(p, 1), ph_f(p, 2), p->b1, p->b2, p->b3);
			break;

		case K_FRIC:
			if (!(s = new_seg())) return;
			s->kind = S_C;
			s->k = p->k;
			s->nf = frames_for(ph_dur(p), true);
			s->t.av = p->av;
			s->t.af = p->af;
			s->ta = 3;
			s->tf = 6;
			set_formants(s, ph_f(p, 0), ph_f(p, 1), ph_f(p, 2), p->b1, p->b2, p->b3);
			set_par(s, p);
			break;

		case K_ASP: {
			// /h/ has no place of its own: it is the next vowel,
			// whispered.
			if (!(s = new_seg())) return;
			s->kind = S_V;
			s->nf = frames_for(ph_dur(p), true);
			s->t.ah = 57;
			s->ta = 3;
			s->tf = 2;
			const ph_t *v = (next && next->p) ? next->p : p;
			set_formants(s, v->f1, v->f2, v->f3, 300, 200, 250);
			break;
		}

		case K_STOP:
		case K_AFFR: {
			// Closure: silence, or a low voice bar for a voiced stop
			// between voiced sounds.
			if (!(s = new_seg())) return;
			s->kind = S_C;
			s->k = p->k;
			uint32_t cl = ph_dur(p);
			if (!prev || !prev->p) cl = cl * 60 / 100;	// utterance- or phrase-initial
			s->nf = frames_for(cl, true);
			s->t.av = (p->voiced && is_voiced_sound(prev)) ? 40 : 0;
			s->ta = 1;
			s->tf = 6;
			set_formants(s, ph_f(p, 0), ph_f(p, 1), ph_f(p, 2), p->b1, p->b2, p->b3);

			// EXPERIMENT velar: a velar's place moves with the vowel --
			// forward before "key", back before "coo" -- so its locus
			// and burst do too. One fixed locus made K before a back
			// vowel sound like T.
			if (EXP(PHON_EXP_VELAR) && p->k >= 60 && next && next->p &&
			    next->p->kind == K_VOWEL) {
				uint16_t vf2 = ph_f(next->p, 1);
				if (is_front(next->p))
					set_formants(s, ph_f(p, 0), 2300, 3000, p->b1, p->b2, p->b3);
				else
					// Well above the vowel's own F2: near it is where
					// P's locus is, and session 1 heard K as P (x7).
					set_formants(s, ph_f(p, 0), (uint16_t)(vf2 + 450 < 1500 ? 1500 : vf2 + 450),
						2400, p->b1, p->b2, p->b3);
			}

			if (p->kind == K_AFFR) {
				seg_t *f = new_seg();
				if (!f) return;
				*f = *s;
				f->nf = frames_for(p->voiced ? 50 : 70, true);
				f->t.av = p->av;
				f->t.af = p->af;
				f->ta = 0;
				set_par(f, p);
				break;
			}

			// Burst: a brief, loud puff shaped by the place of
			// articulation. Velars' is longest.
			seg_t *b = new_seg();
			if (!b) return;
			*b = *s;
			b->nf = (p->k >= 60) ? 3 : 2;
			b->t.av = p->voiced ? 40 : 0;
			b->t.af = p->af;
			b->ta = 0;
			set_par(b, p);

			if (!p->voiced) {
				uint8_t asp = (p->f2 < 1000) ? 9 : (p->k >= 60) ? 12 : 10;	// P, T, K
				// EXPERIMENT vot: English aspiration runs 55-80ms;
				// these were 45-60. (The vowel is lengthened by the
				// same extra frames where the VOT is applied, so the
				// voiced part of it is not shortened: taking the extra
				// time out of the vowel made it heard as "h" -- AE->HH,
				// K->HH in session 1.)
				if (EXP(PHON_EXP_VOT)) asp = (uint8_t)(asp * 3 / 2);
				if (is_vocalic(next)) {
					// Aspiration becomes the start of the next vowel,
					// during its formant transition -- set on the
					// segment that vowel is about to create.
					tk->asp = asp;
				} else {
					seg_t *a = new_seg();
					if (!a) return;
					*a = *s;
					a->nf = (uint16_t)(asp / 2 + 2);
					a->t.ah = 56;
					a->t.av = 0;
					a->ta = 0;
				}
			}
			break;
		}

		}

		for (int j = seg0; j < nsegs; j++) {
			segs[j].phrase = phrase;
			segs[j].tok = (uint16_t)i;
		}

		// Voice onset time for this vowel, left by a voiceless stop.
		if (prev && prev->asp && first < nsegs &&
		    (p->kind == K_VOWEL || p->kind == K_SEMI)) {
			segs[first].vot = prev->asp;
			// EXPERIMENT vot: the extra third of aspiration is added
			// to the segment, so the voiced part keeps its length.
			if (EXP(PHON_EXP_VOT))
				segs[first].nf = (uint16_t)(segs[first].nf + prev->asp / 3);
		}

	}

}

// Where two segments' formants meet.
static uint16_t meet(uint16_t a, uint16_t b, const seg_t *sa, const seg_t *sb) {
	if (sa->kind == S_SIL) return b;
	if (sb->kind == S_SIL) return a;
	if (sa->kind == S_C && sb->kind == S_V)
		return (uint16_t)(a + ((int32_t)b - a) * sa->k / 100);
	if (sa->kind == S_V && sb->kind == S_C)
		return (uint16_t)(b + ((int32_t)a - b) * sb->k / 100);
	return (uint16_t)(((uint32_t)a + b) / 2);
}

static void boundaries(void) {

	// A silence has no formants of its own. Give it the nearest real
	// segment's -- looking back first, then forward, skipping other
	// silences -- so nothing glides in from 0 Hz and no resonator is
	// ever asked for 0 Hz at 0 Hz bandwidth, which is a filter that
	// never decays. (Two silences in a row -- a comma, then the
	// trailing silence -- once did exactly that: the aspiration of a
	// final /k/ fading out into a resonator with its poles on the unit
	// circle, and the output ran away.) Done BEFORE the boundaries are
	// computed from these values.
	for (int i = 0; i < nsegs; i++) {
		if (segs[i].kind != S_SIL) continue;
		const seg_t *n = 0;
		for (int j = i - 1; j >= 0 && !n; j--) if (segs[j].kind != S_SIL) n = &segs[j];
		for (int j = i + 1; j < nsegs && !n; j++) if (segs[j].kind != S_SIL) n = &segs[j];
		if (n) set_formants(&segs[i], n->t.f1, n->t.f2, n->t.f3, n->t.b1, n->t.b2, n->t.b3);
		else set_formants(&segs[i], 500, 1500, 2500, 80, 100, 150);
	}

	for (int i = 0; i <= nsegs; i++) {
		if (i == 0 || i == nsegs) {
			const seg_t *s = &segs[i == 0 ? 0 : nsegs - 1];
			bnd_f1[i] = s->t.f1; bnd_f2[i] = s->t.f2; bnd_f3[i] = s->t.f3;
			continue;
		}
		const seg_t *a = &segs[i - 1], *b = &segs[i];
		bnd_f1[i] = meet(a->t.f1, b->t.f1, a, b);
		bnd_f2[i] = meet(a->t.f2, b->t.f2, a, b);
		bnd_f3[i] = meet(a->t.f3, b->t.f3, a, b);
	}

}

// -- generation --

uint32_t phon_begin(const char *ph, const phon_opts_t *o) {

	// The parameters start at their hand-set defaults; a pack may
	// have replaced them since (pack.c).
	if (!pro[PHON_PRO_TOP]) phon_prosody_reset();

	uint32_t wpm = o && o->rate_wpm ? o->rate_wpm : 180;
	if (wpm < 80) wpm = 80;
	if (wpm > 450) wpm = 450;
	rate_q8 = 180u * 256u / wpm;

	uint32_t hz = o && o->pitch_hz ? o->pitch_hz : 110;
	base_f0 = hz * 16;

	formant_pct = o && o->formant_pct ? o->formant_pct : 100;
	if (formant_pct < 85) formant_pct = 85;
	if (formant_pct > 120) formant_pct = 120;
	expression_pct = o && o->expression_pct ? o->expression_pct - 1 : 100;
	if (expression_pct > 200) expression_pct = 200;

	// A higher voice is a LOUDER voice as the synthesiser sees it:
	// twice the pitch is twice the glottal pulses a second, and higher
	// formants resonate a little harder. Turn the voicing down by as
	// much, so a female voice is as loud as the default one rather
	// than clipping. (Found by the overflow check in `make wav`.)
	// Measured across the range (docs/tts.md, "Voice"): pitch changes
	// loudness by at most a couple of dB, and the full log ratio
	// overcorrects, so two thirds of it. A smaller tract gets louder
	// faster than linearly -- +1.3dB at 117%, +5.9dB at 130% -- and in
	// the consonants as much as the vowels, so every source is turned
	// down by a correction shaped to those measurements -- plus 2dB of
	// margin, because a pack's fitted vowels move the cascade's gain
	// too (a test accent clipped nine samples without it).
	gain_db = db_above(hz, 110) * 2 / 3;
	tract_db = formant_pct > 100
		? db_above(formant_pct, 100) * (formant_pct - 100) / 10 + (formant_pct > 110 ? 3 : 0)
		: 0;

	// F4 and F5 move with the tract too (synth.c explains why), and the
	// source's quality comes from the prosody parameters.
	synth_set_tract(formant_pct);
	synth_set_source((uint32_t)pro[PHON_PRO_OPEN], (uint32_t)pro[PHON_PRO_TILT]);
	continues = o && o->continues;

	parse(ph ? ph : "");
	expand();

	// Trailing silence, so the last sound decays rather than stops.
	seg_t *s = new_seg();
	if (s) {
		s->kind = S_SIL;
		s->nf = continues ? 1 : 6;
		s->ta = 2;
		// The tail belongs to the last token, not to whatever an
		// earlier utterance left in this slot: segs[] is reused, and a
		// stale index here showed up in the marks as a phantom phone
		// after the final pause.
		s->tok = (uint16_t)(ntoks ? ntoks - 1 : 0);
		s->phrase = nsegs > 1 ? segs[nsegs - 2].phrase : 0;
		s->phrase_end = 0;
	}

	boundaries();

	frames_total = 0;
	for (int i = 0; i < nsegs; i++) frames_total += segs[i].nf;

	// Each phrase's length, and how far into its phrase each segment
	// starts, worked out ONCE: the pitch contour needs both on every
	// frame, and summing them there cost a pass over every segment of
	// the utterance per frame -- a few hundred iterations, 200 times a
	// second, for numbers that never change.
	for (int i = 0; i < 256; i++) phrase_frames[i] = 0;
	for (int i = 0; i < nsegs; i++) {
		seg_into_phrase[i] = phrase_frames[segs[i].phrase];
		phrase_frames[segs[i].phrase] += segs[i].nf;
	}

	cur_seg = 0;
	cur_j = 0;
	frame_no = 0;
	memset(&prev_amp, 0, sizeof(prev_amp));

	// Nothing but silence is nothing.
	bool any = false;
	for (int i = 0; i < nsegs; i++) if (segs[i].kind != S_SIL) any = true;
	if (!any) { nsegs = 0; frames_total = 0; }

	return frames_total;

}

static uint16_t lerp16(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
	return (uint16_t)((int32_t)a + ((int32_t)b - (int32_t)a) * (int32_t)num / (int32_t)den);
}

static uint8_t lerp8(uint8_t a, uint8_t b, uint32_t num, uint32_t den) {
	return (uint8_t)lerp16(a, b, num, den);
}

static uint16_t formant(uint16_t target, int i, uint32_t j, uint32_t nf,
	uint32_t tin, uint32_t tout, const uint16_t *bnd) {
	if (tin && j < tin) return lerp16(bnd[i], target, 2 * j + 1, 2 * tin);
	if (tout && j >= nf - tout) return lerp16(target, bnd[i + 1], 2 * (j - (nf - tout)) + 1, 2 * tout);
	return target;
}

bool phon_next(synth_frame_t *f) {

	while (cur_seg < nsegs && cur_j >= segs[cur_seg].nf) {
		prev_amp = segs[cur_seg].t;
		cur_seg++;
		cur_j = 0;
	}
	if (cur_seg >= nsegs) return false;

	const seg_t *s = &segs[cur_seg];
	uint32_t j = (uint32_t)cur_j, nf = s->nf;
	last_tok = s->tok;

	// Transition lengths at each end: the slower of the two segments
	// meeting there, at most half of this one.
	uint32_t tin = s->tf, tout = s->tf;
	if (cur_seg > 0 && segs[cur_seg - 1].tf > tin) tin = segs[cur_seg - 1].tf;
	if (cur_seg + 1 < nsegs && segs[cur_seg + 1].tf > tout) tout = segs[cur_seg + 1].tf;
	if (tin > nf / 2) tin = nf / 2;
	if (tout > nf / 2) tout = nf / 2;
	if (cur_seg == 0) tin = 0;

	*f = s->t;
	f->f1 = formant(s->t.f1, cur_seg, j, nf, tin, tout, bnd_f1);
	f->f2 = formant(s->t.f2, cur_seg, j, nf, tin, tout, bnd_f2);
	f->f3 = formant(s->t.f3, cur_seg, j, nf, tin, tout, bnd_f3);

	if (j < s->ta) {
		f->av = lerp8(prev_amp.av, s->t.av, j + 1, s->ta + 1u);
		f->ah = lerp8(prev_amp.ah, s->t.ah, j + 1, s->ta + 1u);
		f->af = lerp8(prev_amp.af, s->t.af, j + 1, s->ta + 1u);
	}

	if (j < s->vot) {
		// Aspiration while the formants move: voicing starts late.
		f->av = 0;
		f->ah = 55;
		f->b1 = (uint16_t)(f->b1 + 200);
	}

	// -- pitch --
	//
	// Pitch is what tells a listener where one phrase ends and the
	// next begins, and a single falling line over a whole paragraph
	// tells them nothing. So:
	//
	//   - each phrase starts near the top of the range and declines
	//     across itself, RESET at every pause: the "fresh breath" a
	//     reader takes;
	//   - successive phrases start a little lower, so a long sentence
	//     still descends overall;
	//   - a phrase ending in a comma RISES at the end -- "there is
	//     more" -- while one ending in a full stop falls away;
	//   - a stressed vowel is lifted over its neighbours.
	//
	// Learned contours (phase 7) replace the numbers here, not the
	// shape.
	uint32_t n = frames_total ? frames_total : 1;

	// Where we are inside this phrase, and how long it is.
	uint32_t ph_len = phrase_frames[s->phrase];
	uint32_t ph_pos = seg_into_phrase[cur_seg] + j;
	if (!ph_len) ph_len = 1;

	uint32_t top = (uint32_t)pro[PHON_PRO_TOP];
	if (s->phrase) {
		// Each phrase from a little lower, but never so low that a
		// long paragraph ends up growling.
		uint32_t drop = (uint32_t)pro[PHON_PRO_STEP] * s->phrase;
		uint32_t cap = (uint32_t)pro[PHON_PRO_STEP_MAX];
		top -= (drop > cap) ? cap : drop;
	}

	uint32_t pct = top - (uint32_t)pro[PHON_PRO_DECL] * ph_pos / ph_len;

	if (s->kind == S_V && s->t.av && s->stress) {
		uint32_t mid = nf / 2 ? nf / 2 : 1;
		uint32_t d = j < mid ? j : (nf - j);
		pct += (uint32_t)(s->stress == 1 ? pro[PHON_PRO_STRESS1] : pro[PHON_PRO_STRESS2]) * d / mid;
	}

	// The end of a phrase, and the end of the utterance.
	uint32_t ph_left = ph_len - ph_pos;
	if (s->phrase_end == 1 && ph_left < 20)
		pct = (uint32_t)((int32_t)pct + pro[PHON_PRO_COMMA_RISE] * (int32_t)(20 - ph_left) / 20);	// comma: keep going
	else if (s->phrase_end == 2 && ph_left < 20)
		pct -= (uint32_t)pro[PHON_PRO_STOP_FALL] * (20 - ph_left) / 20;		// full stop: done

	uint32_t left = n - frame_no;
	if (question && left < 40) pct += (40 - left);
	else if (!continues && left < 30) pct -= (30 - left) / 3;

	// Expression scales every movement of pitch around the base, all
	// at once: declination, stress, the rise at a comma, the fall at
	// the end.
	if (expression_pct != 100) {
		int32_t dev = (int32_t)pct - 100;
		pct = (uint32_t)(100 + dev * (int32_t)expression_pct / 100);
	}

	if (pct < 60) pct = 60;
	f->f0 = (uint16_t)(base_f0 * pct / 100);

	if (gain_db && f->av) f->av = (f->av > gain_db + 1) ? (uint8_t)(f->av - gain_db) : 1;
	if (tract_db) {
		uint8_t *amps[] = { &f->av, &f->ah, &f->af, &f->a2, &f->a3, &f->a4, &f->a5, &f->a6, &f->ab };
		for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++)
			if (*amps[i]) *amps[i] = (*amps[i] > tract_db + 1) ? (uint8_t)(*amps[i] - tract_db) : 1;
	}

	// A shorter vocal tract raises every formant by the same
	// proportion. The fourth and fifth stay put: they are the same
	// for most speakers and moving them only thins the sound.
	//
	// Only where the VOICE shapes the sound, and never near the top of
	// the band. A fricative's resonances already sit high -- an S is
	// most of the way to the 5.5kHz limit -- and pushing them up
	// another 17% put them close enough to Nyquist that the resonator
	// gain ran away: the first female voice clipped by 15dB, in the
	// esses, not the vowels.
	if (formant_pct != 100 && f->av >= f->af) {
		uint32_t a = f->f1 * formant_pct / 100;
		uint32_t b = f->f2 * formant_pct / 100;
		uint32_t c = f->f3 * formant_pct / 100;
		f->f1 = (uint16_t)(a > 1300 ? 1300 : a);
		f->f2 = (uint16_t)(b > 3300 ? 3300 : b);
		f->f3 = (uint16_t)(c > 4300 ? 4300 : c);
	}

	cur_j++;
	frame_no++;
	return true;

}

// Which phoneme the frame phon_next() just returned belongs to. For
// tools that need phone boundaries (the aligner in tools/speech);
// nothing on the device calls it.
int phon_current(const char **name) {
	if (last_tok < 0 || last_tok >= ntoks) {
		if (name) *name = "";
		return -1;
	}
	// Vowels carry their stress ("AA1"), and a pause says what kind
	// it is ("_c" comma, "_s" sentence, "_p" paragraph, "_" a short
	// break): the prosody fitting in tools/speech needs both, and
	// nothing else is written into the marks.
	static char buf[8];
	const tok_t *t = &toks[last_tok];
	const ph_t *p = t->p;
	if (!p) {
		const char *k = t->pause_ms >= 400 ? "_p" : t->pause_ms >= 250 ? "_s"
			: t->pause_ms >= 150 ? "_c" : "_";
		if (name) *name = k;
		return last_tok;
	}
	if (p->kind == K_VOWEL) {
		buf[0] = p->nm[0];
		buf[1] = p->nm[1] ? p->nm[1] : 0;
		buf[p->nm[1] ? 2 : 1] = (char)('0' + (t->stress > 2 ? 2 : t->stress));
		buf[p->nm[1] ? 3 : 2] = 0;
		if (name) *name = buf;
	} else {
		if (name) *name = p->nm;
	}
	return last_tok;
}
