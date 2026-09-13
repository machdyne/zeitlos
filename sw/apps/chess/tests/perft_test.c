/*
 * Zeitlos chess -- move generation test. Runs on the HOST:
 *
 *   cd sw/apps/chess && make test
 *
 * Links the REAL ce_core.c. There is no second copy of the move
 * generator in this tree.
 *
 * -- what perft actually proves --
 *
 * Perft counts the leaves of the legal move tree at a fixed depth. It
 * is the only test of a move generator worth having, because every
 * rule that is easy to get wrong -- en passant, the pin that makes an
 * en passant capture illegal by discovered check, castling through an
 * attacked square, castling rights lost because a ROOK was captured
 * rather than moved, promotion under check -- changes the count, and
 * changes it at a depth shallow enough to find.
 *
 * A spot check does not do this. "Does castling work" tests castling
 * in the position somebody thought of. Perft tests it in every
 * position reachable in four plies from six starting points.
 *
 * -- the two independent sources of truth --
 *
 * 1. PUBLISHED COUNTS. The six positions below and their node counts
 *    are the standard public-domain perft suite that has circulated
 *    in chess programming for decades. They are facts about chess,
 *    not code: nothing here is derived from any implementation.
 *
 * 2. A SECOND GENERATOR, written in this file, deliberately unlike
 *    the one under test. ce_core.c uses 0x88 squares, piece lists and
 *    incremental legality; naive_perft() below scans all 64 squares
 *    on a plain 8x8 array, generates with file/rank arithmetic and
 *    bounds checks rather than an off-board mask, and tests legality
 *    by looking for the king in the resulting position's capture
 *    list. It shares no data structure and no line of logic with
 *    ce_core.c.
 *
 * Why both. The published counts catch a generator that is wrong;
 * they cannot catch a generator that is wrong in the same way the
 * numbers were transcribed wrong. The second generator catches
 * exactly that, and also makes the suite self-contained: if somebody
 * doubts a published number, naive_perft() is an answer that owes
 * nothing to it. They are checked against each other as well as
 * against ce_core.c, so any two of the three disagreeing names the
 * odd one out.
 *
 * The naive generator is slow -- it is meant to be obviously correct,
 * not fast -- so it only runs to shallow depths.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../ce_core.h"

static int checks, fails;

static void okmsg(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}

static void eq_u64(uint64_t got, uint64_t want, const char *what) {
	checks++;
	if (got != want) {
		fails++;
		printf("  FAIL: %s -- got %llu, want %llu\n",
			what, (unsigned long long)got, (unsigned long long)want);
	}
}

/* ------------------------------------------------------------------ *
 * An independent, deliberately naive reference generator.
 *
 * 8x8 array indexed [rank][file]. Pieces are the FEN letters, so
 * there is no shared encoding with ce_core.c either. Everything is
 * done the slow obvious way.
 * ------------------------------------------------------------------ */

typedef struct {
	char b[8][8];        /* [rank][file], rank 0 = rank 1; ' ' = empty */
	int  white;          /* 1 if white to move */
	int  cwk, cwq, cbk, cbq;
	int  epf, epr;       /* en passant target file/rank, -1 if none */
} nb_t;

typedef struct { int ff, fr, tf, tr; char promo; } nm_t;

static int nb_white_piece(char c) { return c >= 'A' && c <= 'Z'; }
static int nb_black_piece(char c) { return c >= 'a' && c <= 'z'; }
static int nb_empty(char c) { return c == ' '; }

static int nb_mine(const nb_t *n, char c) {
	if (nb_empty(c)) return 0;
	return n->white ? nb_white_piece(c) : nb_black_piece(c);
}

static int nb_theirs(const nb_t *n, char c) {
	if (nb_empty(c)) return 0;
	return n->white ? nb_black_piece(c) : nb_white_piece(c);
}

static char nb_low(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int nb_on(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

static void nb_from_fen(nb_t *n, const char *fen) {

	int f = 0, r = 7;
	const char *s = fen;

	memset(n, 0, sizeof(*n));
	for (r = 0; r < 8; r++) for (f = 0; f < 8; f++) n->b[r][f] = ' ';
	n->epf = n->epr = -1;

	r = 7; f = 0;
	while (*s && *s != ' ') {
		if (*s == '/') { r--; f = 0; }
		else if (*s >= '1' && *s <= '8') f += *s - '0';
		else n->b[r][f++] = *s;
		s++;
	}

	while (*s == ' ') s++;
	n->white = (*s == 'w');
	while (*s && *s != ' ') s++;
	while (*s == ' ') s++;

	while (*s && *s != ' ') {
		if (*s == 'K') n->cwk = 1;
		if (*s == 'Q') n->cwq = 1;
		if (*s == 'k') n->cbk = 1;
		if (*s == 'q') n->cbq = 1;
		s++;
	}
	while (*s == ' ') s++;

	if (*s && *s != '-') { n->epf = s[0] - 'a'; n->epr = s[1] - '1'; }

}

/* Pseudo-legal moves for the side to move, written the long way. */
static int nb_gen(const nb_t *n, nm_t *out) {

	int c = 0, f, r, i;
	static const int kf[8] = { 1, 2, 2, 1, -1, -2, -2, -1 };
	static const int kr[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };
	static const int df[8] = { 1, 1, 1, 0, 0, -1, -1, -1 };
	static const int dr[8] = { 1, 0, -1, 1, -1, 1, 0, -1 };

	for (r = 0; r < 8; r++) for (f = 0; f < 8; f++) {

		char pc = n->b[r][f];
		char lo = nb_low(pc);

		if (!nb_mine(n, pc)) continue;

		if (lo == 'p') {

			int dir = n->white ? 1 : -1;
			int start = n->white ? 1 : 6;
			int last = n->white ? 7 : 0;
			int tr = r + dir, cf;

			if (nb_on(f, tr) && nb_empty(n->b[tr][f])) {
				if (tr == last) {
					const char *pr = n->white ? "QRBN" : "qrbn";
					for (i = 0; i < 4; i++) {
						out[c].ff = f; out[c].fr = r;
						out[c].tf = f; out[c].tr = tr;
						out[c].promo = pr[i]; c++;
					}
				} else {
					out[c].ff = f; out[c].fr = r;
					out[c].tf = f; out[c].tr = tr;
					out[c].promo = 0; c++;
					if (r == start && nb_empty(n->b[r + 2 * dir][f])) {
						out[c].ff = f; out[c].fr = r;
						out[c].tf = f; out[c].tr = r + 2 * dir;
						out[c].promo = 0; c++;
					}
				}
			}

			for (cf = f - 1; cf <= f + 1; cf += 2) {
				if (!nb_on(cf, tr)) continue;
				if (nb_theirs(n, n->b[tr][cf]) ||
					(cf == n->epf && tr == n->epr)) {
					if (tr == last) {
						const char *pr = n->white ? "QRBN" : "qrbn";
						for (i = 0; i < 4; i++) {
							out[c].ff = f; out[c].fr = r;
							out[c].tf = cf; out[c].tr = tr;
							out[c].promo = pr[i]; c++;
						}
					} else {
						out[c].ff = f; out[c].fr = r;
						out[c].tf = cf; out[c].tr = tr;
						out[c].promo = 0; c++;
					}
				}
			}

		} else if (lo == 'n') {

			for (i = 0; i < 8; i++) {
				int tf = f + kf[i], tr = r + kr[i];
				if (!nb_on(tf, tr)) continue;
				if (nb_mine(n, n->b[tr][tf])) continue;
				out[c].ff = f; out[c].fr = r;
				out[c].tf = tf; out[c].tr = tr;
				out[c].promo = 0; c++;
			}

		} else if (lo == 'k') {

			for (i = 0; i < 8; i++) {
				int tf = f + df[i], tr = r + dr[i];
				if (!nb_on(tf, tr)) continue;
				if (nb_mine(n, n->b[tr][tf])) continue;
				out[c].ff = f; out[c].fr = r;
				out[c].tf = tf; out[c].tr = tr;
				out[c].promo = 0; c++;
			}

		} else {

			for (i = 0; i < 8; i++) {
				int tf = f, tr = r;
				int diagonal = df[i] != 0 && dr[i] != 0;
				if (lo == 'b' && !diagonal) continue;
				if (lo == 'r' && diagonal) continue;
				for (;;) {
					tf += df[i]; tr += dr[i];
					if (!nb_on(tf, tr)) break;
					if (nb_mine(n, n->b[tr][tf])) break;
					out[c].ff = f; out[c].fr = r;
					out[c].tf = tf; out[c].tr = tr;
					out[c].promo = 0; c++;
					if (nb_theirs(n, n->b[tr][tf])) break;
				}
			}
		}
	}

	return c;

}

static void nb_apply(nb_t *n, const nm_t *m);

/* Is `sq` attacked by the side NOT to move?
 *
 * Everything except pawns is answered by asking the other side for
 * its pseudo-legal moves and seeing if any lands there -- slow, and
 * obviously right, which is the trade this file wants.
 *
 * PAWNS CANNOT BE ANSWERED THAT WAY, and the first version of this
 * file got it wrong. A pawn's capture is only generated when there is
 * something on the target square, so "can any move reach f1" answers
 * no for an empty f1 even with an enemy pawn on g2 bearing down on
 * it. The consequence was not a missed capture -- it was White being
 * allowed to castle THROUGH an attacked square, which showed up as
 * this file disagreeing with both ce_core.c and the published perft
 * count by exactly one node per affected line.
 *
 * Worth keeping as the header note it is: "attacked" and "reachable
 * by a legal move" are the same set for every piece on the board
 * except the pawn, and the pawn is the one that matters here. */
static int nb_attacked_by_other(const nb_t *n, int f, int r) {

	nb_t o = *n;
	nm_t mv[256];
	int c, i, df;
	int other_white = !n->white;
	char pawn = other_white ? 'P' : 'p';
	int back = other_white ? -1 : 1;   /* where such a pawn must stand */

	for (df = -1; df <= 1; df += 2) {
		int af = f + df, ar = r + back;
		if (nb_on(af, ar) && n->b[ar][af] == pawn) return 1;
	}

	o.white = other_white;
	o.epf = o.epr = -1;      /* an ep capture cannot capture the king */
	c = nb_gen(&o, mv);

	for (i = 0; i < c; i++) {
		if (nb_low(o.b[mv[i].fr][mv[i].ff]) == 'p') continue;
		if (mv[i].tf == f && mv[i].tr == r) return 1;
	}

	return 0;

}

static void nb_find_king(const nb_t *n, int white, int *f, int *r) {
	int i, j;
	char k = white ? 'K' : 'k';
	*f = *r = -1;
	for (j = 0; j < 8; j++) for (i = 0; i < 8; i++)
		if (n->b[j][i] == k) { *f = i; *r = j; }
}

static void nb_apply(nb_t *n, const nm_t *m) {

	char pc = n->b[m->fr][m->ff];
	char lo = nb_low(pc);
	int was_white = n->white;

	/* en passant capture: the captured pawn is not on the target */
	if (lo == 'p' && m->tf == n->epf && m->tr == n->epr &&
		nb_empty(n->b[m->tr][m->tf]))
		n->b[m->fr][m->tf] = ' ';

	n->b[m->fr][m->ff] = ' ';
	n->b[m->tr][m->tf] = m->promo ? m->promo : pc;

	/* rook follows a castling king */
	if (lo == 'k' && m->ff == 4 && (m->tf == 6 || m->tf == 2)) {
		int rank = m->tr;
		if (m->tf == 6) { n->b[rank][5] = n->b[rank][7]; n->b[rank][7] = ' '; }
		else            { n->b[rank][3] = n->b[rank][0]; n->b[rank][0] = ' '; }
	}

	n->epf = n->epr = -1;
	if (lo == 'p' && (m->tr - m->fr == 2 || m->fr - m->tr == 2)) {
		n->epf = m->ff;
		n->epr = (m->fr + m->tr) / 2;
	}

	if (pc == 'K') { n->cwk = n->cwq = 0; }
	if (pc == 'k') { n->cbk = n->cbq = 0; }
	if ((m->ff == 0 && m->fr == 0) || (m->tf == 0 && m->tr == 0)) n->cwq = 0;
	if ((m->ff == 7 && m->fr == 0) || (m->tf == 7 && m->tr == 0)) n->cwk = 0;
	if ((m->ff == 0 && m->fr == 7) || (m->tf == 0 && m->tr == 7)) n->cbq = 0;
	if ((m->ff == 7 && m->fr == 7) || (m->tf == 7 && m->tr == 7)) n->cbk = 0;

	n->white = !was_white;

}

/* Castling in the naive generator, handled separately from nb_gen()
 * so the slide/step logic above stays uncluttered. Appended to the
 * move list by the caller below. */
static int nb_gen_castles(const nb_t *n, nm_t *out) {

	int c = 0;
	int rank = n->white ? 0 : 7;
	char king = n->white ? 'K' : 'k';
	char rook = n->white ? 'R' : 'r';
	int ks = n->white ? n->cwk : n->cbk;
	int qs = n->white ? n->cwq : n->cbq;

	if (n->b[rank][4] != king) return 0;

	if (ks && n->b[rank][7] == rook &&
		nb_empty(n->b[rank][5]) && nb_empty(n->b[rank][6]) &&
		!nb_attacked_by_other(n, 4, rank) &&
		!nb_attacked_by_other(n, 5, rank) &&
		!nb_attacked_by_other(n, 6, rank)) {
		out[c].ff = 4; out[c].fr = rank;
		out[c].tf = 6; out[c].tr = rank; out[c].promo = 0; c++;
	}

	if (qs && n->b[rank][0] == rook &&
		nb_empty(n->b[rank][1]) && nb_empty(n->b[rank][2]) &&
		nb_empty(n->b[rank][3]) &&
		!nb_attacked_by_other(n, 4, rank) &&
		!nb_attacked_by_other(n, 3, rank) &&
		!nb_attacked_by_other(n, 2, rank)) {
		out[c].ff = 4; out[c].fr = rank;
		out[c].tf = 2; out[c].tr = rank; out[c].promo = 0; c++;
	}

	return c;

}

/* The real entry point: nb_gen() plus castles, with legality filtered
 * the same way. */
static uint64_t naive_perft2(nb_t *n, int depth) {

	nm_t mv[256];
	uint64_t total = 0;
	int c, i, kf, kr;

	if (depth == 0) return 1;

	c = nb_gen(n, mv);
	c += nb_gen_castles(n, mv + c);

	for (i = 0; i < c; i++) {
		nb_t next = *n;
		int was_white = n->white;
		nb_apply(&next, &mv[i]);
		nb_find_king(&next, was_white, &kf, &kr);
		next.white = was_white;
		if (nb_attacked_by_other(&next, kf, kr)) continue;
		next.white = !was_white;
		total += naive_perft2(&next, depth - 1);
	}

	return total;

}

/* ------------------------------------------------------------------ *
 * The suite
 * ------------------------------------------------------------------ */

typedef struct {
	const char *name;
	const char *fen;
	int         maxdepth;
	uint64_t    nodes[7];   /* nodes[d] for d = 1..maxdepth */
} perft_case_t;

static const perft_case_t cases[] = {

	{ "start position",
	  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	  5, { 0, 20, 400, 8902, 197281, 4865609 } },

	/* Dense middlegame with both sides able to castle both ways, a
	 * pawn able to capture en passant, and pins in every direction.
	 * This is the position that finds castling-through-check and the
	 * rights-lost-by-rook-capture bug. */
	{ "open middlegame",
	  "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
	  4, { 0, 48, 2039, 97862, 4085603 } },

	/* Sparse endgame, rook and pawns. Finds en passant legality
	 * errors: the white pawn's capture is pinned along the fifth
	 * rank in some continuations. */
	{ "rook endgame",
	  "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
	  5, { 0, 14, 191, 2812, 43238, 674624 } },

	/* Promotions under check, including underpromotions that give
	 * check and ones that do not. */
	{ "promotion tangle",
	  "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
	  4, { 0, 6, 264, 9467, 422333 } },

	{ "cramped kingside",
	  "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
	  4, { 0, 44, 1486, 62379, 2103487 } },

	{ "symmetrical middlegame",
	  "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
	  4, { 0, 46, 2079, 89890, 3894594 } },

};

#define NCASES ((int)(sizeof(cases) / sizeof(cases[0])))

int main(void) {

	ce_pos_t p;
	int i, d;
	char what[128];

	ce_init();

	printf("perft: move generation\n");

	for (i = 0; i < NCASES; i++) {

		if (!ce_set_fen(&p, cases[i].fen)) {
			printf("  FAIL: %s -- FEN rejected\n", cases[i].name);
			fails++; checks++;
			continue;
		}

		/* The FEN round trip belongs here rather than in its own
		 * test: every position in this file is one somebody might
		 * paste in, and a parser that silently drops the en passant
		 * square would show up as a perft mismatch that looks like a
		 * generator bug. */
		{
			char out[100];
			ce_get_fen(&p, out, sizeof(out));
			snprintf(what, sizeof(what), "%s: FEN round trip", cases[i].name);
			okmsg(!strcmp(out, cases[i].fen), what);
			if (strcmp(out, cases[i].fen))
				printf("        got  %s\n        want %s\n", out, cases[i].fen);
		}

		for (d = 1; d <= cases[i].maxdepth; d++) {
			uint64_t got = ce_perft(&p, d);
			snprintf(what, sizeof(what), "%s: perft(%d)", cases[i].name, d);
			eq_u64(got, cases[i].nodes[d], what);
		}

		/* The key must be exactly what it was before all that
		 * make/unmake -- a perft run is the longest make/unmake
		 * sequence in the test suite and therefore the best place to
		 * notice a restore that is not exact. */
		snprintf(what, sizeof(what), "%s: key intact after perft", cases[i].name);
		okmsg(p.key == ce_compute_key(&p), what);
	}

	/* -- cross-check against the independent generator -------------- */

	printf("perft: independent generator agrees\n");

	for (i = 0; i < NCASES; i++) {

		nb_t n;
		int depth = 3;      /* the naive generator is O(awful) */

		nb_from_fen(&n, cases[i].fen);
		ce_set_fen(&p, cases[i].fen);

		for (d = 1; d <= depth; d++) {
			uint64_t a = ce_perft(&p, d);
			uint64_t b = naive_perft2(&n, d);
			snprintf(what, sizeof(what),
				"%s: ce vs naive at depth %d", cases[i].name, d);
			eq_u64(a, b, what);
			/* And both against the published number, so a
			 * disagreement names which one is odd. */
			snprintf(what, sizeof(what),
				"%s: naive vs published at depth %d", cases[i].name, d);
			eq_u64(b, cases[i].nodes[d], what);
		}
	}

	printf("perft: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;

}
