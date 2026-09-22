/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The recorded voice: diphones joined by TD-PSOLA. See dsyn.h, and
 * tools/speech/lib/diphone.py -- this is its synthesise(), in integers,
 * and tests/dsyn_test.c checks the two agree.
 *
 * TD-PSOLA (Moulines and Charpentier, 1990): output pitch marks are laid
 * down one target pitch period apart, in ONE lattice for the whole
 * utterance. For each, the unit covering that moment is found, the
 * moment is mapped into the unit (each half of the unit stretched onto
 * its half-phone), and the nearest source pitch mark's grain -- two of
 * its periods under a Hann window -- is added in, centred on the output
 * mark. Overlapping grains are normalised by their summed windows.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "dsyn.h"
#include "phon.h"
#include "synth.h"
#include "pack.h"

#define MAX_PH		48		// phone names in the section, "_" included
#define MAX_UNITS	2048
#define MAX_LEN		4096		// samples in one unit
#define MAX_MARKS	160
#define MAX_TOKS	512		// phones in one utterance
#define MAX_FRAMES	4096		// 5ms frames in one utterance: 20s
#define MAXP		256		// widest grain half, in samples
#define ACC		1024		// overlap-add accumulator, power of 2
#define STEP_UV		55		// mark spacing where unvoiced: 5ms
#define PAUSE_KEEP	441		// most of a pause half ever played: 40ms
#define XFADE		132		// grains of both units blended either side of a join: 12ms

// mu-law codes to samples: tools/speech/lib/diphone.py, mulaw_decode_table()
static const int16_t mulaw[256] = {
	-32767, -31367, -30027, -28743, -27514, -26338, -25212, -24133,
	-23101, -22112, -21165, -20259, -19391, -18561, -17765, -17004,
	-16275, -15576, -14908, -14268, -13655, -13069, -12507, -11969,
	-11454, -10962, -10490, -10038,  -9605,  -9191,  -8794,  -8414,
	 -8051,  -7703,  -7369,  -7050,  -6745,  -6452,  -6172,  -5904,
	 -5647,  -5401,  -5166,  -4941,  -4725,  -4518,  -4321,  -4131,
	 -3950,  -3776,  -3610,  -3451,  -3299,  -3153,  -3013,  -2880,
	 -2752,  -2629,  -2512,  -2399,  -2292,  -2189,  -2090,  -1996,
	 -1905,  -1819,  -1736,  -1656,  -1581,  -1508,  -1438,  -1371,
	 -1308,  -1246,  -1188,  -1132,  -1078,  -1027,   -978,   -931,
	  -886,   -842,   -801,   -762,   -724,   -687,   -653,   -619,
	  -588,   -557,   -528,   -500,   -473,   -448,   -423,   -400,
	  -377,   -356,   -335,   -315,   -296,   -278,   -261,   -244,
	  -229,   -213,   -199,   -185,   -172,   -159,   -147,   -135,
	  -124,   -113,   -103,    -93,    -83,    -74,    -66,    -57,
	   -50,    -42,    -35,    -28,    -21,    -15,     -9,     -3,
	     3,      9,     15,     21,     28,     35,     42,     50,
	    57,     66,     74,     83,     93,    103,    113,    124,
	   135,    147,    159,    172,    185,    199,    213,    229,
	   244,    261,    278,    296,    315,    335,    356,    377,
	   400,    423,    448,    473,    500,    528,    557,    588,
	   619,    653,    687,    724,    762,    801,    842,    886,
	   931,    978,   1027,   1078,   1132,   1188,   1246,   1308,
	  1371,   1438,   1508,   1581,   1656,   1736,   1819,   1905,
	  1996,   2090,   2189,   2292,   2399,   2512,   2629,   2752,
	  2880,   3013,   3153,   3299,   3451,   3610,   3776,   3950,
	  4131,   4321,   4518,   4725,   4941,   5166,   5401,   5647,
	  5904,   6172,   6452,   6745,   7050,   7369,   7703,   8051,
	  8414,   8794,   9191,   9605,  10038,  10490,  10962,  11454,
	 11969,  12507,  13069,  13655,  14268,  14908,  15576,  16275,
	 17004,  17765,  18561,  19391,  20259,  21165,  22112,  23101,
	 24133,  25212,  26338,  27514,  28743,  30027,  31367,  32767,
};

// Hann window, sin^2, 257 points over one window, Q15
static const uint16_t hann[257] = {
	    0,     5,    20,    44,    79,   123,   177,   241,   315,   398,
	  491,   593,   705,   827,   958,  1098,  1247,  1406,  1573,  1749,
	 1935,  2128,  2331,  2542,  2761,  2989,  3224,  3468,  3719,  3978,
	 4244,  4518,  4799,  5086,  5381,  5682,  5990,  6304,  6624,  6950,
	 7281,  7618,  7961,  8308,  8660,  9017,  9379,  9744, 10114, 10487,
	10864, 11244, 11628, 12014, 12403, 12794, 13187, 13583, 13980, 14378,
	14778, 15178, 15580, 15981, 16383, 16786, 17187, 17589, 17989, 18389,
	18787, 19184, 19580, 19973, 20364, 20753, 21139, 21523, 21903, 22280,
	22653, 23023, 23388, 23750, 24107, 24459, 24806, 25149, 25486, 25817,
	26143, 26463, 26777, 27085, 27386, 27681, 27968, 28249, 28523, 28789,
	29048, 29299, 29543, 29778, 30006, 30225, 30436, 30639, 30832, 31018,
	31194, 31361, 31520, 31669, 31809, 31940, 32062, 32174, 32276, 32369,
	32452, 32526, 32590, 32644, 32688, 32723, 32747, 32762, 32767, 32762,
	32747, 32723, 32688, 32644, 32590, 32526, 32452, 32369, 32276, 32174,
	32062, 31940, 31809, 31669, 31520, 31361, 31194, 31018, 30832, 30639,
	30436, 30225, 30006, 29778, 29543, 29299, 29048, 28789, 28523, 28249,
	27968, 27681, 27386, 27085, 26777, 26463, 26143, 25817, 25486, 25149,
	24806, 24459, 24107, 23750, 23388, 23023, 22653, 22280, 21903, 21523,
	21139, 20753, 20364, 19973, 19580, 19184, 18787, 18389, 17989, 17589,
	17187, 16786, 16384, 15981, 15580, 15178, 14778, 14378, 13980, 13583,
	13187, 12794, 12403, 12014, 11628, 11244, 10864, 10487, 10114,  9744,
	 9379,  9017,  8660,  8308,  7961,  7618,  7281,  6950,  6624,  6304,
	 5990,  5682,  5381,  5086,  4799,  4518,  4244,  3978,  3719,  3468,
	 3224,  2989,  2761,  2542,  2331,  2128,  1935,  1749,  1573,  1406,
	 1247,  1098,   958,   827,   705,   593,   491,   398,   315,   241,
	  177,   123,    79,    44,    20,     5,     0,
};

typedef struct {
	uint32_t data;			// first sample
	uint32_t mark;			// first mark
	uint16_t len, cut, nmarks, flags;	// flags bit 0: the pause half is no pause
} unit_t;

static bool ready;
static uint32_t sec_off, marks_off, data_off;
static uint16_t nph, nunits;
static char names[MAX_PH][3];
static uint16_t pair[MAX_PH * MAX_PH];
static uint16_t first_of[MAX_PH], last_of[MAX_PH];
static unit_t units[MAX_UNITS];
static uint32_t volume = 200;
static uint32_t missing;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void dsyn_set_volume(uint32_t v) { volume = v > 255 ? 255 : v; }
uint32_t dsyn_missing(void) { return missing; }
bool dsyn_ready(void) { return ready; }
void dsyn_close(void) { ready = false; }

bool dsyn_open(uint32_t off, uint32_t len) {

	uint8_t h[16];
	ready = false;
	if (len < 16 || !pack_read_at(off, h, 16)) return false;
	if (rd16(h) != 1 || rd16(h + 2) != SYNTH_FS) return false;
	nph = rd16(h + 4);
	nunits = rd16(h + 6);
	marks_off = rd32(h + 8);
	data_off = rd32(h + 12);
	if (nph == 0 || nph > MAX_PH || nunits == 0 || nunits > MAX_UNITS) return false;
	sec_off = off;

	uint32_t p = off + 16;
	uint8_t nm[2 * MAX_PH];
	if (!pack_read_at(p, nm, 2u * nph)) return false;
	for (int i = 0; i < nph; i++) {
		names[i][0] = (char)nm[2 * i];
		names[i][1] = (char)nm[2 * i + 1];
		names[i][2] = 0;
	}
	p += 2u * nph;
	// The pair table and the stand-ins: read as bytes, little-endian.
	static uint8_t tmp[2 * MAX_PH * MAX_PH];
	if (!pack_read_at(p, tmp, 2u * nph * nph)) return false;
	for (uint32_t i = 0; i < (uint32_t)nph * nph; i++) pair[i] = rd16(tmp + 2 * i);
	p += 2u * nph * nph;
	if (!pack_read_at(p, tmp, 4u * nph)) return false;
	for (int i = 0; i < nph; i++) {
		first_of[i] = rd16(tmp + 2 * i);
		last_of[i] = rd16(tmp + 2 * nph + 2 * i);
	}
	p += 4u * nph;
	// The unit records in 1KB blocks, front to back, rather than one read
	// each: 1,404 reads at startup became 22.
	static uint8_t blk[1024];
	for (uint32_t i = 0; i < nunits; i++) {
		if (i % 64 == 0) {
			uint32_t n = nunits - i < 64 ? nunits - i : 64;
			if (!pack_read_at(p + 16 * i, blk, 16 * n)) return false;
		}
		const uint8_t *r = blk + 16 * (i % 64);
		units[i].data = rd32(r);
		units[i].mark = rd32(r + 4);
		units[i].len = rd16(r + 8);
		units[i].cut = rd16(r + 10);
		units[i].nmarks = rd16(r + 12);
		units[i].flags = rd16(r + 14);
		// A unit too big for the buffers is left out; its pair falls
		// back to halves of others.
		if (units[i].len > MAX_LEN || units[i].nmarks > MAX_MARKS || units[i].cut > units[i].len)
			units[i].len = 0;
	}
	ready = true;
	return true;

}

// -- a chunk's units, read ahead --
//
// When a chunk begins, every unit it will need is known, so all of them
// are read then, in file order, into this arena (mu-law bytes, decoded
// per sample as grains are cut). Reading them as the voice reached each
// one -- through a two-unit cache that the cross-fade, which touches
// three units at a join, kept emptying -- made a long passage take
// nearly five times as long to render as to speak.
#define ARENA		65536
#define ARENA_MARKS	8192

static uint8_t arena[ARENA];
static uint16_t arena_m[ARENA_MARKS];
static struct { uint16_t id; uint32_t s, m; } pre[MAX_TOKS + 4];
static int npre;

static int prefetched(int id) {
	for (int i = 0; i < npre; i++)
		if (pre[i].id == id) return i;
	return -1;
}

// -- the fallback: units that did not fit the arena, read when reached --

typedef struct {
	int id;
	int16_t s[MAX_LEN];
	uint16_t m[MAX_MARKS];
} slot_t;

static slot_t slot[3];
static int slot_next;

static const slot_t *load(int id) {
	for (int i = 0; i < 3; i++)
		if (slot[i].id == id) return &slot[i];
	slot_t *sl = &slot[slot_next];
	slot_next = (slot_next + 1) % 3;
	sl->id = -1;
	const unit_t *u = &units[id];
	static uint8_t raw[MAX_LEN];
	uint8_t mb[2 * MAX_MARKS];
	if (!pack_read_at(sec_off + marks_off + 2u * u->mark, mb, 2u * u->nmarks)) return 0;
	if (!pack_read_at(sec_off + data_off + u->data, raw, u->len)) return 0;
	for (uint32_t i = 0; i < u->nmarks; i++) sl->m[i] = rd16(mb + 2 * i);
	for (uint32_t i = 0; i < u->len; i++) sl->s[i] = mulaw[raw[i]];
	sl->id = id;
	return sl;
}

// -- the plan for one utterance --

enum { BOTH, LEFT, RIGHT };

typedef struct {
	int32_t o0, oc, o1;		// output samples: mid(A), end(A), mid(B)
	int32_t lo, hi;			// the part of the output this covers
	uint16_t unit;
	uint8_t part;
	uint8_t pa, pb;			// A, B is a pause
} seg_t;

static struct { uint8_t ph; int32_t s, e; } toks[MAX_TOKS + 2];
static int ntoks;
static uint16_t f0s[MAX_FRAMES];	// Hz x 16, 0 where unvoiced
static int nframes;
static seg_t segs[MAX_TOKS + 2];
static int nsegs, seg_j;
static int32_t t, pos, total, last_hi;
static bool lattice_done;
static int32_t acc[ACC], wac[ACC];

static int ph_index(const char *nm) {
	// "AA1" -> "AA"; every pause kind ("_c", "_s", "_") -> "_"
	char b[3] = { nm[0], 0, 0 };
	if (nm[0] != '_' && nm[1] && (nm[1] < '0' || nm[1] > '9')) b[1] = nm[1];
	for (int i = 0; i < nph; i++)
		if (names[i][0] == b[0] && names[i][1] == b[1]) return i;
	return -1;
}

bool dsyn_begin(void) {

	int pause = ph_index("_");
	synth_frame_t fr;
	int last_tok = -2;
	ntoks = 0;
	nframes = 0;

	// The phoneme layer's frames: which phone, what pitch.
	while (phon_next(&fr)) {
		const char *nm;
		int tok = phon_current(&nm);
		if (nframes < MAX_FRAMES) f0s[nframes] = fr.av ? fr.f0 : 0;
		if (tok != last_tok && ntoks < MAX_TOKS) {
			int id = ph_index(nm);
			if (ntoks) toks[ntoks].s = toks[ntoks - 1].e = nframes * SYNTH_FRAME;
			else toks[0].s = nframes * SYNTH_FRAME;
			toks[ntoks].ph = (uint8_t)(id < 0 ? pause : id);
			ntoks++;
			last_tok = tok;
		}
		nframes++;
	}
	if (!ntoks || pause < 0) return false;
	if (nframes > MAX_FRAMES) nframes = MAX_FRAMES;
	toks[ntoks - 1].e = nframes * SYNTH_FRAME;
	total = toks[ntoks - 1].e;

	// Pauses at both ends, as the units were cut (diphone.py, _padded).
	if (toks[0].ph != pause) {
		memmove(&toks[1], &toks[0], sizeof(toks[0]) * (size_t)ntoks);
		toks[0].ph = (uint8_t)pause;
		toks[0].e = toks[1].s;
		toks[0].s = toks[1].s - (80 * SYNTH_FS / 1000) < 0 ? 0 : toks[1].s - (80 * SYNTH_FS / 1000);
		ntoks++;
	}
	if (toks[ntoks - 1].ph != pause) {
		toks[ntoks].ph = (uint8_t)pause;
		toks[ntoks].s = toks[ntoks - 1].e;
		toks[ntoks].e = toks[ntoks].s + 80 * SYNTH_FS / 1000;
		ntoks++;
	}

	// One unit per pair, or halves of two others where the pair is missing.
	nsegs = 0;
	for (int i = 0; i + 1 < ntoks; i++) {
		int a = toks[i].ph, b = toks[i + 1].ph;
		int32_t o0 = (toks[i].s + toks[i].e) / 2, oc = toks[i].e;
		int32_t o1 = (toks[i + 1].s + toks[i + 1].e) / 2;
		uint8_t pa = a == pause, pb = b == pause;
		uint16_t u = pair[a * nph + b];
		if (u != 0xFFFF && units[u].len) {
			segs[nsegs++] = (seg_t){ o0, oc, o1, o0, o1, u, BOTH, pa, pb };
			continue;
		}
		missing++;
		uint16_t ua = first_of[a], ub = last_of[b];
		if (ua != 0xFFFF && units[ua].len) segs[nsegs++] = (seg_t){ o0, oc, oc, o0, oc, ua, LEFT, pa, 0 };
		if (ub != 0xFFFF && units[ub].len) segs[nsegs++] = (seg_t){ oc, oc, o1, oc, o1, ub, RIGHT, 0, pb };
	}
	if (!nsegs) return false;

	// Read the chunk's units now, in file order: forward seeks only.
	npre = 0;
	static uint16_t ids[MAX_TOKS + 4];
	int nids = 0;
	for (int i = 0; i < nsegs; i++) {
		int seen = 0;
		for (int k = 0; k < nids; k++) if (ids[k] == segs[i].unit) { seen = 1; break; }
		if (!seen) ids[nids++] = segs[i].unit;
	}
	for (int i = 1; i < nids; i++) {		// insertion sort by position in the file
		uint16_t v = ids[i];
		int k = i - 1;
		while (k >= 0 && units[ids[k]].data > units[v].data) { ids[k + 1] = ids[k]; k--; }
		ids[k + 1] = v;
	}
	// Two passes, each front to back: every unit's marks, then every
	// unit's samples. The marks and the samples are megabytes apart in
	// the file, and reading each unit's marks then its samples made every
	// unit a BACKWARD seek -- which, without FatFs's fast seek, walks the
	// file's cluster chain from its start. A long passage rendered at
	// five times real time and the ring ran dry.
	uint32_t sa = 0, ma = 0;
	for (int i = 0; i < nids; i++) {
		const unit_t *u = &units[ids[i]];
		if (sa + u->len > ARENA || ma + u->nmarks > ARENA_MARKS) continue;
		pre[npre].id = ids[i];
		pre[npre].s = sa;
		pre[npre].m = ma;
		npre++;
		sa += u->len;
		ma += u->nmarks;
	}
	for (int i = 0; i < npre; i++) {
		const unit_t *u = &units[pre[i].id];
		uint8_t mb[2 * MAX_MARKS];
		if (!pack_read_at(sec_off + marks_off + 2u * u->mark, mb, 2u * u->nmarks))
			memset(mb, 0, sizeof(mb));
		for (uint32_t k = 0; k < u->nmarks; k++) arena_m[pre[i].m + k] = rd16(mb + 2 * k);
	}
	for (int i = 0; i < npre; i++) {
		const unit_t *u = &units[pre[i].id];
		if (!pack_read_at(sec_off + data_off + u->data, arena + pre[i].s, u->len))
			memset(arena + pre[i].s, 0x7F, u->len);		// mu-law silence
	}

	seg_j = 0;
	t = segs[0].lo;
	last_hi = segs[nsegs - 1].hi;
	pos = 0;
	lattice_done = false;
	memset(acc, 0, sizeof(acc));
	memset(wac, 0, sizeof(wac));
	return true;

}

// The source period at mark k: to the next voiced mark, or the last.
static int32_t period_at(const uint16_t *m, int nm, int k) {
	if (k + 1 < nm && (m[k + 1] & 0x8000))
		return (int32_t)(m[k + 1] & 0x7FFF) - (int32_t)(m[k] & 0x7FFF);
	if (k > 0 && (m[k - 1] & 0x8000))
		return (int32_t)(m[k] & 0x7FFF) - (int32_t)(m[k - 1] & 0x7FFF);
	return SYNTH_FS / 200;
}

// The grain segment `sg` gives for output time t, added in with weight
// `wt` (Q15). Returns false where the unit is silent there -- a pause
// half beyond what it keeps. *voiced and *period report the source mark.
static bool add_grain(const seg_t *sg, int32_t t, int32_t wt, bool *voiced, int32_t *period) {

	const unit_t *u = &units[sg->unit];
	if (!u->nmarks) return false;
	const uint8_t *mu = 0;
	const int16_t *pcm = 0;
	const uint16_t *mk;
	int pi = prefetched(sg->unit);
	if (pi >= 0) {
		mu = arena + pre[pi].s;
		mk = arena_m + pre[pi].m;
	} else {
		const slot_t *sl = load(sg->unit);
		if (!sl) return false;
		pcm = sl->s;
		mk = sl->m;
	}

	// A PAUSE half is never stretched, and at most PAUSE_KEEP of it is
	// played, next to the join -- none, if it holds no pause at all (the
	// alignment mistook part of a word for one: flags bit 0). A third of
	// them did, and stretching or playing them put a fragment of another
	// word into every gap: the echo in the pauses.
	int32_t keep = (u->flags & 1) ? 0 : PAUSE_KEEP;
	bool first = sg->part == LEFT || (sg->part == BOTH && t < sg->oc);
	int32_t s_t;
	if (first && sg->pa) {
		s_t = (int32_t)u->cut - (sg->oc - t);
		if (s_t < (int32_t)u->cut - keep || s_t < 0) return false;
	} else if (!first && sg->pb) {
		s_t = (int32_t)u->cut + (t - sg->oc);
		if (s_t >= (int32_t)u->cut + keep || s_t >= (int32_t)u->len) return false;
	} else if (first) {
		int32_t d = sg->oc - sg->o0;
		s_t = (t - sg->o0) * (int32_t)u->cut / (d > 0 ? d : 1);
	} else {
		int32_t d = sg->o1 - sg->oc;
		s_t = (int32_t)u->cut + (t - sg->oc) * ((int32_t)u->len - u->cut) / (d > 0 ? d : 1);
	}

	// The nearest source mark.
	int k = 0;
	int32_t best = 0x7FFFFFFF;
	for (int i = 0; i < u->nmarks; i++) {
		int32_t dd = (int32_t)(mk[i] & 0x7FFF) - s_t;
		if (dd < 0) dd = -dd;
		if (dd < best) { best = dd; k = i; }
		else if (dd > best) break;		// marks are in order
	}

	bool mv = (mk[k] & 0x8000) != 0;
	int32_t p = mv ? period_at(mk, u->nmarks, k) : STEP_UV;
	if (p < 20) p = 20;
	if (p > MAXP) p = MAXP;
	*voiced = mv;
	*period = p;

	int32_t c = mk[k] & 0x7FFF;
	int32_t lo = c - p < 0 ? 0 : c - p;
	int32_t hi = c + p > u->len ? u->len : c + p;
	int32_t L = hi - lo;
	int32_t start = t - (c - lo);
	if (L <= 2) return false;
	// The window position steps by 256/L per sample, in 16.16 fixed
	// point: one division per grain rather than one per sample.
	uint32_t wstep = (256u << 16) / (uint32_t)L;
	uint32_t wpos = wstep / 2;
	for (int32_t i = 0; i < L; i++, wpos += wstep) {
		int32_t at = start + i;
		if (at < pos) continue;			// already played
		int32_t w = (int32_t)hann[wpos >> 16];
		if (wt != 32768) w = (w * wt) >> 15;
		int32_t smp = pcm ? pcm[lo + i] : mulaw[mu[lo + i]];
		acc[at & (ACC - 1)] += (smp * w) >> 15;
		wac[at & (ACC - 1)] += w;
	}
	return true;

}

// Lays down the grain for output mark t -- blended with the neighbouring
// unit's near a join -- and moves t on.
static void grain(void) {

	while (seg_j + 1 < nsegs && t >= segs[seg_j].hi) seg_j++;
	const seg_t *sg = &segs[seg_j];
	if (t >= last_hi) { lattice_done = true; return; }
	if (t < sg->lo) { t = sg->lo; return; }		// a hole between units

	// CROSS-FADE: within XFADE of a join with a neighbouring unit, that
	// unit's grain for this moment is added too, from nothing to half at
	// the join. A hard switch between recordings made the joins 1.4x as
	// abrupt as the recording's own: the bumps between sounds.
	int32_t wt = 32768;
	bool v2;
	int32_t p2;
	if (seg_j + 1 < nsegs && segs[seg_j + 1].lo == sg->hi && sg->hi - t < XFADE) {
		int32_t wn = (16384 * (XFADE - (sg->hi - t))) / XFADE;
		if (add_grain(&segs[seg_j + 1], t, wn, &v2, &p2)) wt -= wn;
	}
	if (seg_j > 0 && segs[seg_j - 1].hi == sg->lo && t - sg->lo < XFADE) {
		int32_t wp = (16384 * (XFADE - (t - sg->lo))) / XFADE;
		if (add_grain(&segs[seg_j - 1], t, wp, &v2, &p2)) wt -= wp;
	}

	bool mv;
	int32_t p;
	if (!add_grain(sg, t, wt, &mv, &p)) { t += STEP_UV; return; }

	// The pitch of the frame at or after t, as the prototype takes it
	// (diphone.py's target_hz: the first frame starting at or after t).
	int32_t ms = (int32_t)((int64_t)t * 1000 / SYNTH_FS);
	int f = (ms * SYNTH_FS) / (SYNTH_FRAME * 1000);
	while (f > 0 && (int32_t)((int64_t)(f - 1) * SYNTH_FRAME * 1000 / SYNTH_FS) >= ms) f--;
	while ((int32_t)((int64_t)f * SYNTH_FRAME * 1000 / SYNTH_FS) < ms) f++;
	uint32_t hz16 = f0s[f < nframes ? f : nframes - 1];
	bool voiced = mv && hz16 > 0;
	int32_t step = voiced ? (int32_t)((SYNTH_FS * 16u) / hz16) : STEP_UV;
	t += step < 20 ? 20 : step;

}

int dsyn_render(int16_t *out, int n) {

	if (pos >= total) return 0;
	while (!lattice_done && t < pos + n + MAXP) grain();
	int m = total - pos < n ? total - pos : n;
	for (int i = 0; i < m; i++) {
		int idx = (pos + i) & (ACC - 1);
		int32_t a = acc[idx], w = wac[idx];
		// Where grains overlap more or less than half, even the level out.
		// In 32 bits: bounding the sum first keeps a << 15 inside int32,
		// and rv32im divides 32-bit numbers in hardware but not 64-bit.
		if (a > 65535) a = 65535;
		if (a < -65535) a = -65535;
		int32_t v = (w > 6553) ? (a << 15) / w : a;
		v = (v * (int32_t)volume) >> 8;
		out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		acc[idx] = wac[idx] = 0;
	}
	pos += m;
	return m;

}
