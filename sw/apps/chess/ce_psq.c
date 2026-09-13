/*
 * Zeitlos chess -- piece values and piece-square tables.
 * See ce_psq.h for the layout convention and where the numbers come
 * from.
 */

#include "ce_psq.h"

/* Middlegame values. The queen is deliberately a little under the
 * usual nine pawns: at this search depth the engine cannot see the
 * compensation for a queen sacrifice, so a slightly cheaper queen
 * makes it marginally less reluctant to trade one for real material
 * it CAN count. */
const int16_t ce_val_mg[7] = {
	0,     /* empty */
	100,   /* pawn   */
	325,   /* knight */
	340,   /* bishop */
	500,   /* rook   */
	950,   /* queen  */
	0      /* king -- never counted, it is always on the board */
};

/* Endgame values. Pawns and rooks matter more as material comes off;
 * knights slightly less, because a knight is worst on an empty board
 * with pawns on both wings. */
const int16_t ce_val_eg[7] = {
	0, 125, 320, 350, 545, 985, 0
};

const int8_t ce_pst_mg[7][64] = {

	/* empty */
	{ 0 },

	/* pawn -- push the centre, keep the shelter in front of a
	 * castled king at home, and make the d/e pawns' first move worth
	 * something so the opening is not a shuffle. The -15 on d2/e2 is
	 * the "you have not developed yet" nudge. */
	{
	   0,   0,   0,   0,   0,   0,   0,   0,
	  35,  35,  35,  35,  35,  35,  35,  35,
	  12,  14,  20,  26,  26,  20,  14,  12,
	   4,   6,  12,  22,  22,  12,   6,   4,
	   0,   2,   6,  18,  18,   4,   2,   0,
	   2,  -2,  -6,   2,   2,  -8,  -2,   2,
	   4,   6,   6, -15, -15,   8,   8,   4,
	   0,   0,   0,   0,   0,   0,   0,   0
	},

	/* knight -- a pure centralisation gradient with a hard rim
	 * penalty. Corners are worst; f3/c3 and f6/c6 are the natural
	 * developing squares and get a small nudge. */
	{
	 -40, -28, -18, -14, -14, -18, -28, -40,
	 -28, -12,   0,   4,   4,   0, -12, -28,
	 -18,   2,  12,  16,  16,  12,   2, -18,
	 -14,   6,  16,  20,  20,  16,   6, -14,
	 -14,   4,  14,  18,  18,  14,   4, -14,
	 -18,   2,  10,  14,  14,  10,   2, -18,
	 -28, -12,   2,   6,   6,   2, -12, -28,
	 -40, -24, -18, -14, -14, -18, -24, -40
	},

	/* bishop -- long diagonals, and a real penalty for the square it
	 * starts on, so it moves. b5/g5 and b4/g4 are the two pins worth
	 * encouraging. */
	{
	 -16,  -8,  -8,  -6,  -6,  -8,  -8, -16,
	  -8,   4,   2,   2,   2,   2,   4,  -8,
	  -6,   4,   8,   8,   8,   8,   4,  -6,
	  -6,   6,   8,  12,  12,   8,   6,  -6,
	  -6,   4,  10,  12,  12,  10,   4,  -6,
	  -6,  10,   8,   8,   8,   8,  10,  -6,
	  -8,   8,   4,   4,   4,   4,   8,  -8,
	 -16,  -6, -10,  -6,  -6, -10,  -6, -16
	},

	/* rook -- the seventh rank, the centre files, and a small bonus
	 * for the two squares a rook reaches by castling. */
	{
	   0,   2,   4,   6,   6,   4,   2,   0,
	  10,  14,  14,  14,  14,  14,  14,  10,
	  -2,   0,   2,   4,   4,   2,   0,  -2,
	  -2,   0,   2,   4,   4,   2,   0,  -2,
	  -2,   0,   2,   4,   4,   2,   0,  -2,
	  -2,   0,   2,   4,   4,   2,   0,  -2,
	  -4,   0,   2,   4,   4,   2,   0,  -4,
	  -2,   0,   4,   8,   8,   4,   0,  -2
	},

	/* queen -- barely anything. A big queen table mostly teaches a
	 * shallow engine to bring her out early, which is the one thing
	 * it must not do. Only the centre in the middlegame, gently, and
	 * a nudge off the back rank corners. */
	{
	 -14,  -8,  -6,  -4,  -4,  -6,  -8, -14,
	  -8,   0,   2,   2,   2,   2,   0,  -8,
	  -6,   2,   4,   4,   4,   4,   2,  -6,
	  -4,   2,   4,   6,   6,   4,   2,  -4,
	  -4,   2,   4,   6,   6,   4,   2,  -4,
	  -6,   2,   4,   4,   4,   4,   2,  -6,
	  -8,   0,   2,   2,   2,   2,   0,  -8,
	 -14,  -8,  -6,  -4,  -4,  -6,  -8, -12
	},

	/* king, middlegame -- stay home and stay behind pawns. The g1/c1
	 * peaks are the castled squares; the centre of the board is
	 * where a middlegame king dies. */
	{
	 -60, -70, -70, -80, -80, -70, -70, -60,
	 -55, -60, -65, -70, -70, -65, -60, -55,
	 -45, -50, -55, -60, -60, -55, -50, -45,
	 -35, -40, -45, -50, -50, -45, -40, -35,
	 -25, -30, -35, -40, -40, -35, -30, -25,
	 -12, -18, -22, -26, -26, -22, -18, -12,
	  16,  16,  -4, -10, -10,  -6,  16,  16,
	  18,  28,   8, -10,   0,  -8,  28,  18
	}
};

const int8_t ce_pst_eg[7][64] = {

	{ 0 },

	/* pawn -- the endgame table is almost entirely "how close to
	 * promoting". The file-by-file variation of the middlegame table
	 * is gone, because in an endgame an a-pawn on the seventh is
	 * worth exactly as much as a d-pawn on the seventh. */
	{
	   0,   0,   0,   0,   0,   0,   0,   0,
	  80,  80,  80,  80,  80,  80,  80,  80,
	  48,  48,  46,  44,  44,  46,  48,  48,
	  26,  26,  24,  22,  22,  24,  26,  26,
	  12,  12,  10,  10,  10,  10,  12,  12,
	   4,   4,   4,   4,   4,   4,   4,   4,
	   0,   0,   0,   0,   0,   0,   0,   0,
	   0,   0,   0,   0,   0,   0,   0,   0
	},

	/* knight -- same shape as the middlegame, slightly flatter. */
	{
	 -36, -24, -16, -12, -12, -16, -24, -36,
	 -24, -10,   0,   4,   4,   0, -10, -24,
	 -16,   0,  10,  14,  14,  10,   0, -16,
	 -12,   4,  14,  18,  18,  14,   4, -12,
	 -12,   4,  14,  18,  18,  14,   4, -12,
	 -16,   0,  10,  14,  14,  10,   0, -16,
	 -24, -10,   0,   4,   4,   0, -10, -24,
	 -36, -24, -16, -12, -12, -16, -24, -36
	},

	/* bishop -- worth more with open lines, so the gradient is
	 * gentler and the corners hurt less than in the middlegame. */
	{
	 -10,  -6,  -4,  -2,  -2,  -4,  -6, -10,
	  -6,   0,   2,   4,   4,   2,   0,  -6,
	  -4,   2,   6,   8,   8,   6,   2,  -4,
	  -2,   4,   8,  10,  10,   8,   4,  -2,
	  -2,   4,   8,  10,  10,   8,   4,  -2,
	  -4,   2,   6,   8,   8,   6,   2,  -4,
	  -6,   0,   2,   4,   4,   2,   0,  -6,
	 -10,  -6,  -4,  -2,  -2,  -4,  -6, -10
	},

	/* rook -- the seventh rank matters even more, and the file
	 * preference disappears. */
	{
	   4,   4,   6,   6,   6,   6,   4,   4,
	  14,  16,  16,  16,  16,  16,  16,  14,
	   2,   4,   4,   4,   4,   4,   4,   2,
	   2,   2,   4,   4,   4,   4,   2,   2,
	   0,   2,   2,   4,   4,   2,   2,   0,
	   0,   0,   2,   2,   2,   2,   0,   0,
	  -2,   0,   0,   2,   2,   0,   0,  -2,
	   0,   0,   2,   2,   2,   2,   0,   0
	},

	/* queen -- centralisation is genuinely worth something once
	 * there is nothing left to harass her. */
	{
	 -20, -12,  -8,  -4,  -4,  -8, -12, -20,
	 -12,  -2,   2,   6,   6,   2,  -2, -12,
	  -8,   2,   8,  12,  12,   8,   2,  -8,
	  -4,   6,  12,  16,  16,  12,   6,  -4,
	  -4,   6,  12,  16,  16,  12,   6,  -4,
	  -8,   2,   8,  12,  12,   8,   2,  -8,
	 -12,  -2,   2,   6,   6,   2,  -2, -12,
	 -20, -12,  -8,  -4,  -4,  -8, -12, -20
	},

	/* king, endgame -- the sign of the middlegame table flips
	 * entirely. An endgame king belongs in the middle, and this is
	 * the single largest table in the file because a king that will
	 * not walk forward cannot win a pawn endgame at all. */
	{
	 -50, -32, -20, -12, -12, -20, -32, -50,
	 -32, -12,   4,  12,  12,   4, -12, -32,
	 -20,   4,  20,  28,  28,  20,   4, -20,
	 -12,  12,  28,  36,  36,  28,  12, -12,
	 -12,  12,  28,  36,  36,  28,  12, -12,
	 -20,   4,  20,  28,  28,  20,   4, -20,
	 -32, -12,   4,  12,  12,   4, -12, -32,
	 -50, -32, -20, -12, -12, -20, -32, -50
	}
};
