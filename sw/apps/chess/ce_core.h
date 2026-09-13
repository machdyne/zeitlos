#ifndef CE_CORE_H
#define CE_CORE_H

/*
 * Zeitlos chess -- engine core.
 *
 * Board representation, move generation, make/unmake, FEN, SAN and
 * Zobrist hashing. Clean-room: written for this project, from the
 * rules, not derived from any existing engine.
 *
 * -- no I/O, deliberately --
 *
 * Nothing in this file or ce_core.c touches MMIO, the window manager,
 * the filesystem or stdio. That is what lets tests/ link the SHIPPED
 * source on the build machine rather than a copy of it -- the same
 * arrangement sw/apps/cal uses for cal_core.c, and for the same
 * reason: a copy drifts, and the thing that drifts is never the thing
 * you are looking at when the bug shows up.
 *
 * -- why 0x88 and not bitboards --
 *
 * Bitboards want 64-bit shifts, multiplies and a population count.
 * This target is rv32i in the worst case (sw/common/arch.mk), where a
 * 64-bit shift is a libgcc call and there is no popcount at all. A
 * 0x88 mailbox needs none of that: every off-board test is one AND
 * against a constant, and every ray is an add.
 *
 *   sq = rank * 16 + file,  rank 0 = rank 1, file 0 = file a
 *   a1 = 0x00, h1 = 0x07, a8 = 0x70, h8 = 0x77
 *   (sq & 0x88) != 0  <=>  sq is off the board
 *
 * -- why piece lists --
 *
 * docs/icache.md: instruction fetch is cached, data access is NOT, and
 * a load costs ~11 cycles on SDRAM and ~63 on QQSPI PSRAM. Scanning
 * 128 board squares to find the 16 pieces on them is therefore not a
 * small constant factor, it is the whole cost. The piece list is
 * maintained incrementally by ce_make()/ce_unmake() so that anything
 * wanting "every white piece" reads at most 16 bytes.
 */

#include <stdint.h>
#include <stdbool.h>

/* -- colours and piece types --------------------------------------- */

#define CE_WHITE 0
#define CE_BLACK 1

#define CE_EMPTY  0
#define CE_PAWN   1
#define CE_KNIGHT 2
#define CE_BISHOP 3
#define CE_ROOK   4
#define CE_QUEEN  5
#define CE_KING   6

/* A board byte is (colour << 3) | type, so white is 1..6 and black is
 * 9..14 and the colour test is one shift. 0 is empty and 7/8/15 never
 * occur, which means CE_TYPE() of an empty square is 0 and needs no
 * special case at most call sites. */
#define CE_PIECE(c, t) (uint8_t)(((c) << 3) | (t))
#define CE_TYPE(p)     ((p) & 7)
#define CE_COLOR(p)    (((p) >> 3) & 1)

/* -- squares -------------------------------------------------------- */

#define CE_SQ(file, rank) (uint8_t)(((rank) << 4) | (file))
#define CE_FILE(sq)       ((sq) & 7)
#define CE_RANK(sq)       ((sq) >> 4)
#define CE_OFFBOARD(sq)   ((sq) & 0x88)
#define CE_SQ64(sq)       ((((sq) >> 4) << 3) | ((sq) & 7))
#define CE_SQ_NONE        0xFF

/* -- castling rights ------------------------------------------------ */

#define CE_CASTLE_WK 1
#define CE_CASTLE_WQ 2
#define CE_CASTLE_BK 4
#define CE_CASTLE_BQ 8

/* -- moves ---------------------------------------------------------- */

/*
 * One move packs into a uint32_t:
 *
 *   bits  0..7   from square (0x88)
 *   bits  8..15  to square (0x88)
 *   bits 16..18  promotion piece type, 0 if none
 *   bits 19..23  flags
 *
 * The captured piece is NOT in here. It lives in the undo record,
 * because that is the only place it is ever needed and putting it in
 * the move would mean two encodings of the same move (one from the
 * generator, one from the transposition table) that compare unequal.
 * That class of bug is silent: the TT move simply never matches and
 * move ordering quietly gets worse.
 */
#define CE_MF_CAPTURE (1u << 19)
#define CE_MF_EP      (1u << 20)
#define CE_MF_CASTLE  (1u << 21)
#define CE_MF_PAWN2   (1u << 22)
#define CE_MF_PROMO   (1u << 23)

#define CE_MOVE(from, to, promo, flags) \
	((uint32_t)(from) | ((uint32_t)(to) << 8) | \
	 ((uint32_t)(promo) << 16) | (uint32_t)(flags))

#define CE_MOVE_FROM(m)  ((uint8_t)((m) & 0xFF))
#define CE_MOVE_TO(m)    ((uint8_t)(((m) >> 8) & 0xFF))
#define CE_MOVE_PROMO(m) ((uint8_t)(((m) >> 16) & 7))
#define CE_MOVE_FLAGS(m) ((m) & 0x00F80000u)

#define CE_MOVE_NONE 0u

/* Moves compare by from/to/promo only -- the flags are a function of
 * the position, so a move recovered from the transposition table or
 * typed by a person carries none of them. Every comparison in the
 * search goes through this. */
#define CE_MOVE_SAME(a, b) \
	((((a) ^ (b)) & 0x0007FFFFu) == 0)

/* The 16-bit form stored in a transposition table entry: from, to and
 * promotion, which is exactly CE_MOVE_SAME's comparison set. */
#define CE_MOVE_PACK16(m) \
	((uint16_t)((CE_SQ64(CE_MOVE_FROM(m))) | \
	 (CE_SQ64(CE_MOVE_TO(m)) << 6) | (CE_MOVE_PROMO(m) << 12)))

/* -- position ------------------------------------------------------- */

#define CE_MAX_MOVES 256   /* a legal chess position has far fewer than
                            * this; the bound is for the generator's
                            * pseudo-legal output, which is still well
                            * under 128 in any reachable position. */
#define CE_MAX_PLY   64
#define CE_MAX_GAME  512   /* plies of game history we can undo */

typedef struct {

	uint8_t  board[128];

	/* Piece lists. plist[c][0..pnum[c]-1] holds the squares occupied
	 * by colour c, in no particular order; pidx[sq] is that piece's
	 * index within its own list, so removing a piece is a swap with
	 * the last entry rather than a scan. */
	uint8_t  plist[2][16];
	uint8_t  pidx[128];
	uint8_t  pnum[2];
	uint8_t  ksq[2];

	/* Per-colour, per-type counts. Used by the phase calculation and
	 * by eval's bishop-pair and pawn terms, both of which would
	 * otherwise walk the piece list for something it already knows. */
	uint8_t  count[2][7];

	uint8_t  side;
	uint8_t  castle;
	uint8_t  ep;          /* 0x88 square, or CE_SQ_NONE */
	uint16_t halfmove;    /* plies since last capture or pawn move */
	uint16_t fullmove;

	uint32_t key;

	/* Incrementally maintained evaluation terms -- see ce_eval.c.
	 * Kept here rather than in the evaluator because make/unmake is
	 * the only place they can be updated, and a term the evaluator
	 * owns but cannot update is a term that goes stale. */
	int16_t  psq_mg[2];   /* material + piece-square, middlegame */
	int16_t  psq_eg[2];   /* material + piece-square, endgame */
	int16_t  npmat[2];    /* non-pawn material, for phase and null move */

} ce_pos_t;

typedef struct {
	uint32_t move;
	uint32_t key;
	uint8_t  captured;    /* piece byte removed, CE_EMPTY if none */
	uint8_t  castle;
	uint8_t  ep;
	uint16_t halfmove;
} ce_undo_t;

/* -- setup ---------------------------------------------------------- */

void ce_init(void);                       /* one-time: Zobrist tables */
void ce_start_position(ce_pos_t *p);
bool ce_set_fen(ce_pos_t *p, const char *fen);
void ce_get_fen(const ce_pos_t *p, char *out, int outlen);

/* Recomputes the Zobrist key from scratch. Only the tests and
 * ce_set_fen() call this; everything else maintains it incrementally.
 * tests/core_test.c checks the two agree after every move of a long
 * random game, which is the check that actually finds an incremental
 * update somebody forgot to write. */
uint32_t ce_compute_key(const ce_pos_t *p);

/* -- queries -------------------------------------------------------- */

bool ce_attacked(const ce_pos_t *p, uint8_t sq, uint8_t by);
bool ce_in_check(const ce_pos_t *p, uint8_t side);

/* -- move generation ------------------------------------------------ */

/* Pseudo-legal: may leave the mover's own king in check. Returns the
 * number of moves written to `out`, which must have room for
 * CE_MAX_MOVES. */
int ce_gen_moves(const ce_pos_t *p, uint32_t *out);

/* Captures and promotions only -- what the quiescence search wants. */
int ce_gen_captures(const ce_pos_t *p, uint32_t *out);

/* Fully legal, by making each pseudo-legal move and discarding the
 * ones that leave the king attacked. This is what the UI and the
 * SAN/parse paths use; the search does the same filtering itself,
 * inline, so it does not pay for a second move array per ply. */
int ce_gen_legal(ce_pos_t *p, uint32_t *out);

/* -- make / unmake -------------------------------------------------- */

/* Makes `move` and returns false, having fully undone it, if it left
 * the mover's own king in check. A true return leaves the move made
 * and `u` filled in for ce_unmake(). */
bool ce_make(ce_pos_t *p, uint32_t move, ce_undo_t *u);
void ce_unmake(ce_pos_t *p, const ce_undo_t *u);

/* A null move: pass the turn. Used by the search's null-move pruning.
 * Clears the en passant square, since a passed turn cannot be
 * answered by an en passant capture. */
void ce_make_null(ce_pos_t *p, ce_undo_t *u);
void ce_unmake_null(ce_pos_t *p, const ce_undo_t *u);

/* -- game state ----------------------------------------------------- */

typedef enum {
	CE_RESULT_NONE = 0,
	CE_RESULT_CHECKMATE,     /* side to move is mated */
	CE_RESULT_STALEMATE,
	CE_RESULT_FIFTY,
	CE_RESULT_MATERIAL,      /* insufficient material */
	CE_RESULT_REPETITION,
} ce_result_t;

/* `history` is the Zobrist key after each ply of the game so far,
 * `hn` its length -- pass NULL/0 to skip the repetition test. */
ce_result_t ce_result(ce_pos_t *p, const uint32_t *history, int hn);

bool ce_insufficient_material(const ce_pos_t *p);

/* -- notation ------------------------------------------------------- */

/* "e2e4", "e7e8q" -- five bytes plus NUL. Always available, never
 * ambiguous, and the format the UI stores moves in. */
void ce_move_to_coord(uint32_t move, char *out);

/* Standard algebraic: "Nf3", "exd5", "O-O", "e8=Q+", "Qxf7#".
 * Needs the position the move is made FROM, both to disambiguate and
 * to work out the check/mate suffix -- which means it makes and
 * unmakes the move, so `p` must be mutable. Restored exactly.
 * `out` needs 10 bytes. */
void ce_move_to_san(ce_pos_t *p, uint32_t move, char *out);

/* Accepts either form, plus the sloppier things people type: "e2-e4",
 * "O-O"/"0-0"/"o-o", "Ng1f3", a missing capture "x", a missing "="
 * before a promotion piece. Matches against the legal move list, so
 * anything it accepts is legal by construction. Returns CE_MOVE_NONE
 * on no match, and sets *ambiguous if more than one legal move fits. */
uint32_t ce_parse_move(ce_pos_t *p, const char *s, bool *ambiguous);

/* -- perft ---------------------------------------------------------- */

uint64_t ce_perft(ce_pos_t *p, int depth);

#endif
