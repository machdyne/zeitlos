#ifndef CE_EVAL_H
#define CE_EVAL_H

/*
 * Zeitlos chess -- position evaluation.
 *
 * Returns a score in centipawns FROM THE SIDE TO MOVE'S POINT OF
 * VIEW, which is what a negamax search wants. Positive means the side
 * to move is better.
 *
 * -- how much evaluation is worth having here --
 *
 * Every term costs time that would otherwise go into search depth,
 * and on this machine (docs/icache.md: ~12 MIPS best case, data loads
 * uncached) that trade is much harsher than on a desktop. One extra
 * ply is worth roughly 60-70 Elo at this level; no evaluation term
 * below is worth anything like that.
 *
 * So the rule applied here is: a term earns its place only if leaving
 * it out produces a move a human would call obviously bad. Material,
 * placement, passed pawns, king shelter and the bishop pair all clear
 * that bar. Mobility, pawn-chain shape, outposts and space do not --
 * they make the engine a little better and noticeably slower, and a
 * slower engine is a worse OPPONENT here, because the person is
 * sitting in front of a window waiting for it.
 */

#include "ce_core.h"

/* Score from the side to move's point of view, in centipawns. */
int ce_eval(const ce_pos_t *p);

/* Material only, side to move's point of view. Used by the search's
 * delta pruning and by the UI's captured-material display. */
int ce_material(const ce_pos_t *p, int color);

/* 0 in a pure endgame, 256 with all the pieces on. Exposed because
 * the search uses it to decide whether null-move pruning is safe. */
int ce_phase(const ce_pos_t *p);

#endif
