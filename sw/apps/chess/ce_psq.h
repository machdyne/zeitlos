#ifndef CE_PSQ_H
#define CE_PSQ_H

/*
 * Zeitlos chess -- piece values and piece-square tables.
 *
 * Its own file rather than part of ce_eval.c because BOTH layers need
 * it and they need it for different reasons:
 *
 *   ce_core.c  accumulates these incrementally in make/unmake, so a
 *              position always knows its own material + placement
 *              score without anybody walking the board.
 *   ce_eval.c  adds everything that cannot be incremental (pawn
 *              structure, king safety, open files) on top.
 *
 * Having ce_core.c reach into ce_eval.h for the tables would invert
 * the layering -- the core would depend on the evaluator, and the
 * perft tests, which have no business knowing what a bishop is worth,
 * would have to link it. This file is the shared bottom instead.
 *
 * -- reading the tables --
 *
 * Row 0 is RANK 8 and column 0 is file a, so each table is laid out
 * the way a board is drawn, from Black's back rank down to White's.
 * CE_PST_IDX() does the flip: a white piece on a square looks up the
 * row counted from the top, a black piece the row counted from the
 * bottom, which makes every table White's point of view and mirrors
 * automatically.
 *
 * -- how these numbers were chosen --
 *
 * By hand, from ordinary positional principles, not tuned against a
 * game database and not copied from anywhere. They are deliberately
 * modest: the largest placement term is 35 centipawns, about a third
 * of a pawn, so placement can break a tie between two moves but never
 * outvote material. An engine this shallow gets into more trouble
 * from a confident wrong evaluation than from a bland one.
 */

#include <stdint.h>
#include "ce_core.h"

/* Row 0 = rank 8. White counts rows from the top, Black from the
 * bottom, so one table serves both. */
#define CE_PST_IDX(color, sq) \
	((color) == CE_WHITE ? ((7 - CE_RANK(sq)) * 8 + CE_FILE(sq)) \
	                     : (CE_RANK(sq) * 8 + CE_FILE(sq)))

extern const int16_t ce_val_mg[7];
extern const int16_t ce_val_eg[7];

extern const int8_t ce_pst_mg[7][64];
extern const int8_t ce_pst_eg[7][64];

/* Material plus placement for one piece, as ce_core.c accumulates it. */
static inline int ce_psq_mg_of(int color, int type, uint8_t sq) {
	return ce_val_mg[type] + ce_pst_mg[type][CE_PST_IDX(color, sq)];
}

static inline int ce_psq_eg_of(int color, int type, uint8_t sq) {
	return ce_val_eg[type] + ce_pst_eg[type][CE_PST_IDX(color, sq)];
}

#endif
