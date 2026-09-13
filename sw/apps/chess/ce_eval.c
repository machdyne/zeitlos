/*
 * Zeitlos chess -- position evaluation.
 * See ce_eval.h for what is in here and, more usefully, what is
 * deliberately not.
 */

#include <string.h>

#include "ce_eval.h"
#include "ce_psq.h"

/* Non-pawn material with everything on the board, per side:
 * 2N + 2B + 2R + Q at the middlegame values in ce_psq.c. */
#define CE_PHASE_FULL (2 * 325 + 2 * 340 + 2 * 500 + 950)

int ce_phase(const ce_pos_t *p) {

	int m = p->npmat[CE_WHITE] + p->npmat[CE_BLACK];

	/* Scaled to 0..256 with a multiply and a shift rather than a
	 * divide. 2 * CE_PHASE_FULL is 6560, and 6560 / 256 is 25.625,
	 * so (m * 10) >> 8 divides by 25.6 -- within a fifth of a
	 * percent, which is meaninglessly small for a blend weight and
	 * costs no __divsi3 call on an rv32i build. */
	m = (m * 10) >> 8;

	return m > 256 ? 256 : m;

}

int ce_material(const ce_pos_t *p, int color) {
	return p->count[color][CE_PAWN] * ce_val_mg[CE_PAWN] +
	       p->npmat[color];
}

/* -- pawn structure -------------------------------------------------
 *
 * Everything the pawn terms need is derivable from two arrays per
 * colour: how many pawns are on each file, and the most and least
 * advanced rank on each file. Doubled, isolated and passed all fall
 * out of those, so the pawns are walked exactly once.
 */

typedef struct {
	int8_t n[2][8];     /* pawns on this file */
	int8_t hi[2][8];    /* highest rank of a pawn on this file, -1 none */
	int8_t lo[2][8];    /* lowest rank, 8 if none */
} pawn_info_t;

static void pawn_scan(const ce_pos_t *p, pawn_info_t *pi) {

	int c, i, f;

	for (c = 0; c < 2; c++)
		for (f = 0; f < 8; f++) {
			pi->n[c][f] = 0;
			pi->hi[c][f] = -1;
			pi->lo[c][f] = 8;
		}

	for (c = 0; c < 2; c++)
		for (i = 0; i < p->pnum[c]; i++) {
			uint8_t sq = p->plist[c][i];
			int r;
			if (CE_TYPE(p->board[sq]) != CE_PAWN) continue;
			f = CE_FILE(sq);
			r = CE_RANK(sq);
			pi->n[c][f]++;
			if (r > pi->hi[c][f]) pi->hi[c][f] = (int8_t)r;
			if (r < pi->lo[c][f]) pi->lo[c][f] = (int8_t)r;
		}

}

/* Passed-pawn bonus by how close the pawn is to promoting. Index is
 * the pawn's rank counted from its own side: 0 is its home rank, 6 is
 * one square from promotion. Steep at the top, because a pawn on the
 * seventh is a different kind of object from a pawn on the fourth. */
static const int16_t passed_bonus_mg[8] = { 0, 4, 8, 16, 30, 52, 80, 0 };
static const int16_t passed_bonus_eg[8] = { 0, 8, 18, 34, 60, 100, 150, 0 };

static int pawn_terms(const ce_pos_t *p, const pawn_info_t *pi,
	int c, int *eg_out) {

	int mg = 0, eg = 0;
	int i, f, g;
	int them = c ^ 1;

	for (f = 0; f < 8; f++) {
		if (!pi->n[c][f]) continue;
		/* Doubled. A tripled file is charged twice, which is about
		 * right -- the third pawn is no worse than the second. */
		if (pi->n[c][f] > 1) {
			mg -= 12 * (pi->n[c][f] - 1);
			eg -= 20 * (pi->n[c][f] - 1);
		}
		/* Isolated. Worth more in the middlegame than people expect
		 * and less in the endgame than they expect, because in the
		 * endgame the weakness is the SQUARE in front of it, which
		 * the passed-pawn term already prices. */
		if ((f == 0 || !pi->n[c][f - 1]) &&
			(f == 7 || !pi->n[c][f + 1])) {
			mg -= 16;
			eg -= 10;
		}
	}

	for (i = 0; i < p->pnum[c]; i++) {

		uint8_t sq = p->plist[c][i];
		int r, rel, blocked = 0;

		if (CE_TYPE(p->board[sq]) != CE_PAWN) continue;

		f = CE_FILE(sq);
		r = CE_RANK(sq);
		rel = c == CE_WHITE ? r : 7 - r;

		for (g = f - 1; g <= f + 1; g++) {
			if (g < 0 || g > 7) continue;
			if (c == CE_WHITE) {
				if (pi->hi[them][g] > r) { blocked = 1; break; }
			} else {
				if (pi->lo[them][g] < r) { blocked = 1; break; }
			}
		}

		if (!blocked) {
			mg += passed_bonus_mg[rel];
			eg += passed_bonus_eg[rel];
		}
	}

	*eg_out = eg;
	return mg;

}

/* -- pieces on files, king shelter ---------------------------------- */

static int piece_terms(const ce_pos_t *p, const pawn_info_t *pi,
	int c, int *eg_out) {

	int mg = 0, eg = 0;
	int i, f;
	int them = c ^ 1;

	/* The bishop pair. Two bishops cover both colour complexes, which
	 * is worth about half a pawn and is the one "positional" term a
	 * shallow engine can reliably cash in, because it lasts. */
	if (p->count[c][CE_BISHOP] >= 2) { mg += 30; eg += 45; }

	for (i = 0; i < p->pnum[c]; i++) {

		uint8_t sq = p->plist[c][i];
		int t = CE_TYPE(p->board[sq]);

		if (t != CE_ROOK) continue;

		f = CE_FILE(sq);
		if (!pi->n[c][f]) {
			/* Fully open beats half open, and both beat nothing. */
			if (!pi->n[them][f]) { mg += 20; eg += 12; }
			else { mg += 10; eg += 6; }
		}
	}

	/* King shelter, middlegame only -- in an endgame the king wants
	 * to be out in front of its pawns, and ce_psq.c's endgame king
	 * table already says so. Charging for a missing shield there
	 * would fight it. */
	{
		int kf = CE_FILE(p->ksq[c]);
		int g;
		for (g = kf - 1; g <= kf + 1; g++) {
			if (g < 0 || g > 7) continue;
			if (!pi->n[c][g]) {
				/* The file the king is standing on is worse than a
				 * neighbour, because that is where a rook comes. */
				mg -= (g == kf) ? 24 : 14;
			}
		}
	}

	*eg_out = eg;
	return mg;

}

int ce_eval(const ce_pos_t *p) {

	pawn_info_t pi;
	int mg, eg, ph, score;
	int mg_c, eg_c;

	/* A position nobody can win is worth exactly nothing, and saying
	 * so here stops the engine shuffling pieces for fifty moves in a
	 * dead-drawn king-and-bishop ending while the person waits. */
	if (ce_insufficient_material(p)) return 0;

	pawn_scan(p, &pi);

	mg = p->psq_mg[CE_WHITE] - p->psq_mg[CE_BLACK];
	eg = p->psq_eg[CE_WHITE] - p->psq_eg[CE_BLACK];

	mg_c = pawn_terms(p, &pi, CE_WHITE, &eg_c);
	mg += mg_c; eg += eg_c;
	mg_c = pawn_terms(p, &pi, CE_BLACK, &eg_c);
	mg -= mg_c; eg -= eg_c;

	mg_c = piece_terms(p, &pi, CE_WHITE, &eg_c);
	mg += mg_c; eg += eg_c;
	mg_c = piece_terms(p, &pi, CE_BLACK, &eg_c);
	mg -= mg_c; eg -= eg_c;

	ph = ce_phase(p);

	/* Divide, not an arithmetic shift right.
	 *
	 * `>> 8` rounds towards negative infinity, so a position worth
	 * -27.5 becomes -28 while its mirror image worth +27.5 becomes
	 * +27. The evaluation is then not symmetrical by one centipawn,
	 * which is small enough to look like nothing and large enough to
	 * make the engine prefer one colour's version of an identical
	 * position. Integer division truncates towards zero, which
	 * negates cleanly. tests/search_test.c checks this directly.
	 *
	 * A division by a power of two compiles to a shift plus a sign
	 * correction, so this is two extra instructions, not a __divsi3
	 * call. */
	score = (mg * ph + eg * (256 - ph)) / 256;

	/* Everything above this line is from White's point of view. Flip
	 * it FIRST, then add the tempo -- the tempo belongs to whoever is
	 * to move, so adding it before the flip would hand it to White in
	 * both cases and quietly bias every evaluation by 16 centipawns. */
	if (p->side == CE_BLACK) score = -score;

	/* Tempo. Small, but it stops the evaluation being exactly
	 * symmetrical in a symmetrical position, which is what makes an
	 * engine shuffle: with no tempo term every move in a quiet
	 * position scores the same and the choice between them is
	 * arbitrary. */
	score += 8;

	return score;

}
