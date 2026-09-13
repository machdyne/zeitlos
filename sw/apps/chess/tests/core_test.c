/*
 * Zeitlos chess -- core invariants. Runs on the HOST:
 *
 *   cd sw/apps/chess && make test
 *
 * tests/perft_test.c already proves the move generator produces the
 * right SET of moves. It proves almost nothing about the seven pieces
 * of DERIVED state a position carries alongside board[] -- the piece
 * lists, the index map, the per-type counts, the king squares, the
 * non-pawn material, the incremental evaluation terms and the Zobrist
 * key.
 *
 * That gap matters because perft cannot see any of it. A make/unmake
 * that corrupts the piece list but leaves board[] correct passes
 * every perft in the suite, and then the engine plays a move with a
 * rook that is not there -- at which point the symptom is a wrong
 * move in a real game, weeks later, with nothing to reproduce it
 * from.
 *
 * So the technique here is: play long random games, and after EVERY
 * single ply re-derive all seven from board[] alone and compare. Then
 * unwind the whole game and check the position is byte-identical to
 * where it started.
 */

#include <stdio.h>
#include <string.h>

#include "../ce_core.h"
#include "../ce_psq.h"

static int checks, fails;

static void okmsg(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}

static void eq_int(long got, long want, const char *what) {
	checks++;
	if (got != want) {
		fails++;
		printf("  FAIL: %s -- got %ld, want %ld\n", what, got, want);
	}
}

/* -- derived state, rebuilt from board[] alone ---------------------- */

static int verify_derived(const ce_pos_t *p, const char *where) {

	int c, t, sq, bad = 0;
	int count[2][7];
	int pnum[2];
	int npmat[2];
	int mg[2], eg[2];
	uint8_t ksq[2];

	memset(count, 0, sizeof(count));
	memset(npmat, 0, sizeof(npmat));
	memset(mg, 0, sizeof(mg));
	memset(eg, 0, sizeof(eg));
	pnum[0] = pnum[1] = 0;
	ksq[0] = ksq[1] = CE_SQ_NONE;

	for (sq = 0; sq < 128; sq++) {
		uint8_t pc;
		if (CE_OFFBOARD(sq)) continue;
		pc = p->board[sq];
		if (pc == CE_EMPTY) continue;
		c = CE_COLOR(pc);
		t = CE_TYPE(pc);
		count[c][t]++;
		pnum[c]++;
		mg[c] += ce_psq_mg_of(c, t, (uint8_t)sq);
		eg[c] += ce_psq_eg_of(c, t, (uint8_t)sq);
		if (t == CE_KING) ksq[c] = (uint8_t)sq;
		else if (t != CE_PAWN) npmat[c] += ce_val_mg[t];
	}

	for (c = 0; c < 2; c++) {

		int i;

		if (p->pnum[c] != pnum[c]) {
			printf("  FAIL: %s -- pnum[%d] %d, board says %d\n",
				where, c, p->pnum[c], pnum[c]);
			bad = 1;
		}
		if (p->ksq[c] != ksq[c]) {
			printf("  FAIL: %s -- ksq[%d] %02x, board says %02x\n",
				where, c, p->ksq[c], ksq[c]);
			bad = 1;
		}
		if (p->npmat[c] != npmat[c]) {
			printf("  FAIL: %s -- npmat[%d] %d, board says %d\n",
				where, c, p->npmat[c], npmat[c]);
			bad = 1;
		}
		if (p->psq_mg[c] != mg[c] || p->psq_eg[c] != eg[c]) {
			printf("  FAIL: %s -- psq[%d] %d/%d, board says %d/%d\n",
				where, c, p->psq_mg[c], p->psq_eg[c], mg[c], eg[c]);
			bad = 1;
		}
		for (t = 1; t <= 6; t++)
			if (p->count[c][t] != count[c][t]) {
				printf("  FAIL: %s -- count[%d][%d] %d, board says %d\n",
					where, c, t, p->count[c][t], count[c][t]);
				bad = 1;
			}

		/* Every square in the list must hold a piece of that colour,
		 * and pidx must point back at the same slot. This is what
		 * catches a swap-remove that updated the list but not the
		 * index map -- which looks fine until the NEXT removal moves
		 * the wrong piece. */
		for (i = 0; i < p->pnum[c]; i++) {
			uint8_t s = p->plist[c][i];
			if (CE_OFFBOARD(s) || p->board[s] == CE_EMPTY ||
				CE_COLOR(p->board[s]) != c) {
				printf("  FAIL: %s -- plist[%d][%d] = %02x is not a "
					"%s piece\n", where, c, i, s, c ? "black" : "white");
				bad = 1;
			} else if (p->pidx[s] != i) {
				printf("  FAIL: %s -- pidx[%02x] = %d, list slot %d\n",
					where, s, p->pidx[s], i);
				bad = 1;
			}
		}
	}

	checks++;
	if (bad) fails++;

	return !bad;

}

/* Two positions are the same position when every rule-bearing field
 * agrees. That is NOT the same as memcmp() being zero, and the
 * difference is worth stating because the first version of this file
 * used memcmp() and failed on correct code.
 *
 * plist[] holds each side's occupied squares in NO PARTICULAR ORDER.
 * remove_piece() fills the hole with the list's last entry and
 * add_piece() appends, so capturing a piece and taking it back leaves
 * the same set of squares in a different order. The position is
 * identical; the bytes are not.
 *
 * So the list is checked for CONSISTENCY (verify_derived() already
 * does that, on every ply) and for being the same SET here, and
 * everything else is compared exactly.
 */
static int same_position(const ce_pos_t *a, const ce_pos_t *b,
	const char *where) {

	int bad = 0, c, i;

	checks++;

	if (memcmp(a->board, b->board, sizeof(a->board))) {
		printf("  FAIL: %s -- board differs\n", where); bad = 1;
	}
	if (a->key != b->key) {
		printf("  FAIL: %s -- key differs\n", where); bad = 1;
	}
	if (a->side != b->side) {
		printf("  FAIL: %s -- side differs\n", where); bad = 1;
	}
	if (a->castle != b->castle) {
		printf("  FAIL: %s -- castling rights differ (%d vs %d)\n",
			where, a->castle, b->castle); bad = 1;
	}
	if (a->ep != b->ep) {
		printf("  FAIL: %s -- en passant square differs (%02x vs %02x)\n",
			where, a->ep, b->ep); bad = 1;
	}
	if (a->halfmove != b->halfmove) {
		printf("  FAIL: %s -- halfmove clock differs (%d vs %d)\n",
			where, a->halfmove, b->halfmove); bad = 1;
	}
	if (a->fullmove != b->fullmove) {
		printf("  FAIL: %s -- fullmove number differs\n", where); bad = 1;
	}

	for (c = 0; c < 2; c++) {
		if (a->pnum[c] != b->pnum[c] || a->ksq[c] != b->ksq[c] ||
			a->npmat[c] != b->npmat[c] ||
			a->psq_mg[c] != b->psq_mg[c] || a->psq_eg[c] != b->psq_eg[c]) {
			printf("  FAIL: %s -- derived totals differ for colour %d\n",
				where, c);
			bad = 1;
		}
		for (i = 1; i <= 6; i++)
			if (a->count[c][i] != b->count[c][i]) {
				printf("  FAIL: %s -- piece counts differ\n", where);
				bad = 1;
				break;
			}
		/* Same set of squares, order ignored. */
		for (i = 0; i < a->pnum[c] && !bad; i++) {
			int j, found = 0;
			for (j = 0; j < b->pnum[c]; j++)
				if (a->plist[c][i] == b->plist[c][j]) { found = 1; break; }
			if (!found) {
				printf("  FAIL: %s -- piece list sets differ\n", where);
				bad = 1;
			}
		}
	}

	if (bad) fails++;
	return !bad;

}

/* -- a deterministic walk over random games ------------------------- */

static uint32_t rng_state = 0xC0FFEE01u;

static uint32_t rnd(void) {
	uint32_t x = rng_state;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	rng_state = x;
	return x;
}

static void random_games(const char *fen, int games, int max_plies) {

	int g;

	for (g = 0; g < games; g++) {

		ce_pos_t p, start;
		ce_undo_t undo[512];
		int nundo = 0, ply;

		if (!ce_set_fen(&p, fen)) { okmsg(0, "random_games: bad FEN"); return; }
		start = p;

		for (ply = 0; ply < max_plies; ply++) {

			uint32_t moves[CE_MAX_MOVES];
			int n = ce_gen_legal(&p, moves);
			uint32_t mv;

			if (n == 0) break;
			if (p.halfmove >= 100) break;

			mv = moves[rnd() % (uint32_t)n];

			if (!ce_make(&p, mv, &undo[nundo])) {
				okmsg(0, "random_games: ce_gen_legal returned an "
					"illegal move");
				return;
			}
			nundo++;

			if (p.key != ce_compute_key(&p)) {
				char c[8];
				ce_move_to_coord(mv, c);
				printf("  FAIL: incremental key wrong after %s "
					"(ply %d)\n", c, ply);
				checks++; fails++;
				return;
			}
			checks++;

			if (!verify_derived(&p, "after make")) return;
		}

		/* Unwind the entire game and check we are back where we
		 * started. */
		while (nundo > 0) {
			nundo--;
			ce_unmake(&p, &undo[nundo]);
		}

		if (!same_position(&p, &start, "after unwinding a game")) return;
	}

}

/* -- notation ------------------------------------------------------- */

static void notation_round_trip(const char *fen) {

	ce_pos_t p;
	uint32_t moves[CE_MAX_MOVES];
	int n, i;

	if (!ce_set_fen(&p, fen)) { okmsg(0, "notation: bad FEN"); return; }

	n = ce_gen_legal(&p, moves);

	for (i = 0; i < n; i++) {

		char san[16], coord[8];
		uint32_t back;
		bool amb;

		ce_move_to_san(&p, moves[i], san);
		ce_move_to_coord(moves[i], coord);

		/* SAN must be unambiguous BY CONSTRUCTION: if it needs a
		 * disambiguator, ce_move_to_san() is the thing that had to
		 * add it. Parsing its own output and getting a different
		 * move back means the disambiguation logic is wrong, which
		 * is precisely the failure that makes a move list unreadable
		 * two hours into a game. */
		back = ce_parse_move(&p, san, &amb);
		checks++;
		if (!CE_MOVE_SAME(back, moves[i]) || amb) {
			fails++;
			printf("  FAIL: SAN round trip: %s parsed back as %s%s\n",
				san, back == CE_MOVE_NONE ? "nothing" : "another move",
				amb ? " (ambiguous)" : "");
		}

		back = ce_parse_move(&p, coord, &amb);
		checks++;
		if (!CE_MOVE_SAME(back, moves[i]) || amb) {
			fails++;
			printf("  FAIL: coord round trip: %s did not parse back\n",
				coord);
		}
	}

	/* The position must be untouched by all that SAN generation,
	 * which makes and unmakes every move to work out the check
	 * suffix. */
	{
		ce_pos_t fresh;
		ce_set_fen(&fresh, fen);
		same_position(&p, &fresh, "after generating SAN for every move");
	}

}

int main(void) {

	ce_pos_t p;
	uint32_t mv;
	bool amb;

	ce_init();

	printf("core: derived state and make/unmake\n");

	/* Four very different shapes of position. The promotion tangle
	 * and the open middlegame between them exercise every branch in
	 * ce_make(): castling both sides, en passant, promotion with and
	 * without capture, and rights lost by rook capture. */
	random_games("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
		12, 120);
	random_games("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/"
		"R3K2R w KQkq - 0 1", 12, 120);
	random_games("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/"
		"R2Q1RK1 w kq - 0 1", 12, 120);
	random_games("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 12, 120);

	printf("core: notation\n");

	notation_round_trip("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	notation_round_trip("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/"
		"R3K2R w KQkq - 0 1");
	notation_round_trip("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/"
		"R2Q1RK1 w kq - 0 1");
	/* Four queens and two knights that can both reach the same
	 * square: the position that needs file, rank AND both. */
	notation_round_trip("8/8/8/1Q2Q3/8/1Q2Q3/8/K5Nk w - - 0 1");

	/* Disambiguation, checked explicitly rather than only through the
	 * round trip -- the round trip would still pass if SAN emitted a
	 * needlessly long form everywhere. */
	{
		char san[16];
		uint32_t moves[CE_MAX_MOVES];
		int n, i;

		/* Knights on g1 and e1 both reach f3: files differ, so a
		 * file letter is enough. */
		ce_set_fen(&p, "4k3/8/8/8/8/8/8/4NKN1 w - - 0 1");
		n = ce_gen_legal(&p, moves);
		for (i = 0; i < n; i++) {
			if (CE_MOVE_TO(moves[i]) != CE_SQ(5, 2)) continue;
			ce_move_to_san(&p, moves[i], san);
			checks++;
			if (strlen(san) != 4 || san[0] != 'N' ||
				(san[1] != 'e' && san[1] != 'g')) {
				fails++;
				printf("  FAIL: expected file disambiguation, got %s\n", san);
			}
		}

		/* Rooks on a1 and a8 both reach a4: same file, so it must
		 * fall back to the rank digit. */
		ce_set_fen(&p, "R7/7k/8/8/8/8/8/R6K w - - 0 1");
		n = ce_gen_legal(&p, moves);
		for (i = 0; i < n; i++) {
			if (CE_MOVE_TO(moves[i]) != CE_SQ(0, 3)) continue;
			ce_move_to_san(&p, moves[i], san);
			checks++;
			if (strcmp(san, "R1a4") && strcmp(san, "R8a4")) {
				fails++;
				printf("  FAIL: expected rank disambiguation, got %s\n", san);
			}
		}
	}

	/* The sloppy forms a person actually types. */
	ce_set_fen(&p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	okmsg(ce_parse_move(&p, "e2e4", &amb) != CE_MOVE_NONE, "parse e2e4");
	okmsg(ce_parse_move(&p, "e2-e4", &amb) != CE_MOVE_NONE, "parse e2-e4");
	okmsg(ce_parse_move(&p, "e4", &amb) != CE_MOVE_NONE, "parse e4");
	okmsg(ce_parse_move(&p, "Nf3", &amb) != CE_MOVE_NONE, "parse Nf3");
	okmsg(ce_parse_move(&p, "nf3", &amb) != CE_MOVE_NONE, "parse nf3 (lower)");
	okmsg(ce_parse_move(&p, "Ng1f3", &amb) != CE_MOVE_NONE, "parse Ng1f3");
	okmsg(ce_parse_move(&p, "e5", &amb) == CE_MOVE_NONE, "reject e5 for white");
	okmsg(ce_parse_move(&p, "", &amb) == CE_MOVE_NONE, "reject empty");
	okmsg(ce_parse_move(&p, "zz", &amb) == CE_MOVE_NONE, "reject nonsense");

	/* Castling, in all the spellings, and the one case where a pawn
	 * move and a bishop move are spelled the same but for case. */
	ce_set_fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
	okmsg(ce_parse_move(&p, "O-O", &amb) != CE_MOVE_NONE, "parse O-O");
	okmsg(ce_parse_move(&p, "0-0", &amb) != CE_MOVE_NONE, "parse 0-0");
	okmsg(ce_parse_move(&p, "o-o-o", &amb) != CE_MOVE_NONE, "parse o-o-o");
	okmsg(ce_parse_move(&p, "e1g1", &amb) != CE_MOVE_NONE, "parse castle by coords");

	/* The one place case genuinely carries meaning: a b-pawn and a
	 * bishop can both capture on c4, and "bxc4" and "Bxc4" are
	 * different moves. Lower-casing the input before matching would
	 * make them the same string, so the case-sensitive pass has to
	 * run first -- this is the test that says so. */
	ce_set_fen(&p, "4k3/8/8/8/2n5/1P6/4B3/4K3 w - - 0 1");
	mv = ce_parse_move(&p, "bxc4", &amb);
	okmsg(mv != CE_MOVE_NONE && !amb &&
		CE_TYPE(p.board[CE_MOVE_FROM(mv)]) == CE_PAWN, "bxc4 is the pawn");
	mv = ce_parse_move(&p, "Bxc4", &amb);
	okmsg(mv != CE_MOVE_NONE && !amb &&
		CE_TYPE(p.board[CE_MOVE_FROM(mv)]) == CE_BISHOP, "Bxc4 is the bishop");

	printf("core: game results\n");

	/* Fool's mate. */
	ce_set_fen(&p, "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");
	eq_int(ce_result(&p, NULL, 0), CE_RESULT_CHECKMATE, "fool's mate detected");

	/* A classic stalemate: Black to move, no legal move, not in
	 * check. */
	ce_set_fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
	eq_int(ce_result(&p, NULL, 0), CE_RESULT_STALEMATE, "stalemate detected");

	/* Not stalemate -- same shape, but Black has a move. */
	ce_set_fen(&p, "7k/8/5Q2/8/8/8/8/6K1 b - - 0 1");
	eq_int(ce_result(&p, NULL, 0), CE_RESULT_NONE,
		"in check with an escape is not stalemate");

	ce_set_fen(&p, "4k3/8/8/8/8/8/8/4K3 w - - 0 1");
	okmsg(ce_insufficient_material(&p), "K vs K is insufficient");
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/3BK3 w - - 0 1");
	okmsg(ce_insufficient_material(&p), "KB vs K is insufficient");
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/3NK3 w - - 0 1");
	okmsg(ce_insufficient_material(&p), "KN vs K is insufficient");
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/2BBK3 w - - 0 1");
	okmsg(!ce_insufficient_material(&p), "KBB vs K is sufficient");
	ce_set_fen(&p, "4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
	okmsg(!ce_insufficient_material(&p), "a pawn is always sufficient");
	ce_set_fen(&p, "3bk3/8/8/8/8/8/8/3BK3 w - - 0 1");
	okmsg(ce_insufficient_material(&p), "KB vs KB is insufficient");

	/* Fifty moves: the clock is in plies, the rule is in moves. */
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/R3K3 w - - 99 60");
	eq_int(ce_result(&p, NULL, 0), CE_RESULT_NONE, "99 plies is not fifty moves");
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/R3K3 w - - 100 60");
	eq_int(ce_result(&p, NULL, 0), CE_RESULT_FIFTY, "100 plies is");

	/* Threefold: the same key three times in the history. */
	{
		uint32_t hist[8];
		ce_set_fen(&p, "4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
		hist[0] = p.key; hist[1] = 0x1234; hist[2] = p.key;
		eq_int(ce_result(&p, hist, 3), CE_RESULT_NONE, "twice is not a draw");
		hist[3] = p.key;
		eq_int(ce_result(&p, hist, 4), CE_RESULT_REPETITION, "three times is");
	}

	printf("core: FEN\n");

	/* A FEN with no halfmove/fullmove fields, which plenty of real
	 * FEN has. Must parse, and must default sanely. */
	okmsg(ce_set_fen(&p, "8/8/8/8/8/8/8/K6k w - -"), "short FEN accepted");
	eq_int(p.halfmove, 0, "short FEN halfmove defaults to 0");
	eq_int(p.fullmove, 1, "short FEN fullmove defaults to 1");

	okmsg(!ce_set_fen(&p, "8/8/8/8/8/8/8/8 w - - 0 1"), "no kings rejected");
	okmsg(!ce_set_fen(&p, "KK6/8/8/8/8/8/8/7k w - - 0 1"), "two white kings rejected");
	okmsg(!ce_set_fen(&p, "garbage"), "garbage rejected");

	/* Castling rights that nothing supports are dropped, not
	 * refused -- a position pasted from a diagram often claims
	 * rights it cannot have. */
	okmsg(ce_set_fen(&p, "4k3/8/8/8/8/8/8/4K2R w KQkq - 0 1"),
		"unsupported rights accepted");
	eq_int(p.castle, CE_CASTLE_WK, "unsupported rights dropped");

	printf("core: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;

}
