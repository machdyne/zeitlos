/*
 * Zeitlos chess -- engine core.
 *
 * See ce_core.h for the representation and why it is the one chosen.
 * This file is pure logic: no MMIO, no stdio, no allocation.
 */

#include <string.h>

#include "ce_core.h"
#include "ce_psq.h"

/* -- ray offsets ---------------------------------------------------- */

static const int8_t off_knight[8] = { 33, 31, 18, 14, -14, -18, -31, -33 };
static const int8_t off_bishop[4] = { 17, 15, -15, -17 };
static const int8_t off_rook[4]   = { 16, 1, -1, -16 };
static const int8_t off_king[8]   = { 17, 16, 15, 1, -1, -15, -16, -17 };

/* -- Zobrist -------------------------------------------------------- */

/*
 * Generated at ce_init() rather than stored as a table, which saves
 * 3KB of flash and, more usefully, keeps the host tests and the target
 * bit-for-bit identical without a generated file to keep in sync. The
 * generator is a fixed-seed xorshift, so "random" here means "fixed
 * and well spread", not "different every run" -- a different key set
 * per boot would make a saved game's repetition history meaningless.
 *
 * 32-bit keys, not 64. A 64-bit key on rv32i costs a libgcc call at
 * every XOR, and this engine's transposition table is 2048 entries:
 * the birthday bound on a table that small is nowhere near the point
 * where 32 bits is the limiting factor. The search verifies the move
 * it takes from a TT hit is pseudo-legal before using it, so a
 * collision costs a wasted ordering hint, not a crash.
 */
static uint32_t zob_piece[12][64];
static uint32_t zob_castle[16];
static uint32_t zob_ep[8];
static uint32_t zob_side;
static bool zob_ready;

static uint32_t zrand(uint32_t *s) {
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

void ce_init(void) {

	uint32_t s = 0x1BADB002u;
	int i, j;

	if (zob_ready) return;

	for (i = 0; i < 12; i++)
		for (j = 0; j < 64; j++)
			zob_piece[i][j] = zrand(&s);

	/* Indexed by the whole 4-bit rights mask rather than by
	 * individual rights, so a rights change is one XOR out and one
	 * in regardless of how many bits moved. Four separate keys would
	 * need the caller to work out which bits changed, which is
	 * exactly the arithmetic that goes wrong when a rook is captured
	 * on its home square. */
	for (i = 0; i < 16; i++) zob_castle[i] = zrand(&s);
	for (i = 0; i < 8; i++)  zob_ep[i] = zrand(&s);

	zob_side = zrand(&s);

	zob_ready = true;

}

static inline int zpi(uint8_t pc) {
	return CE_COLOR(pc) * 6 + (CE_TYPE(pc) - 1);
}

uint32_t ce_compute_key(const ce_pos_t *p) {

	uint32_t k = 0;
	int c, i;

	for (c = 0; c < 2; c++)
		for (i = 0; i < p->pnum[c]; i++) {
			uint8_t sq = p->plist[c][i];
			k ^= zob_piece[zpi(p->board[sq])][CE_SQ64(sq)];
		}

	k ^= zob_castle[p->castle & 15];
	if (p->ep != CE_SQ_NONE) k ^= zob_ep[CE_FILE(p->ep)];
	if (p->side == CE_BLACK) k ^= zob_side;

	return k;

}

/* -- piece bookkeeping ---------------------------------------------- */

/*
 * Every change to the board goes through these three, and they are
 * the ONLY place board[], plist[], pidx[], count[], ksq[], key and the
 * incremental evaluation terms are written. That is the point: a
 * position has seven pieces of derived state, and the way they go
 * wrong is one of them being updated at a call site that forgot the
 * other six. tests/core_test.c re-derives all seven from board[] after
 * every move of a long random game and compares.
 */

static void add_piece(ce_pos_t *p, uint8_t sq, uint8_t pc) {

	int c = CE_COLOR(pc), t = CE_TYPE(pc);

	p->board[sq] = pc;
	p->pidx[sq] = p->pnum[c];
	p->plist[c][p->pnum[c]] = sq;
	p->pnum[c]++;
	p->count[c][t]++;

	p->key ^= zob_piece[zpi(pc)][CE_SQ64(sq)];
	p->psq_mg[c] += ce_psq_mg_of(c, t, sq);
	p->psq_eg[c] += ce_psq_eg_of(c, t, sq);

	if (t == CE_KING) p->ksq[c] = sq;
	else if (t != CE_PAWN) p->npmat[c] += ce_val_mg[t];

}

static void remove_piece(ce_pos_t *p, uint8_t sq) {

	uint8_t pc = p->board[sq];
	int c = CE_COLOR(pc), t = CE_TYPE(pc);
	int i = p->pidx[sq];
	int last;

	p->pnum[c]--;
	last = p->pnum[c];

	/* Swap-remove. When i == last this writes sq back over itself,
	 * which is correct and needs no branch. */
	p->plist[c][i] = p->plist[c][last];
	p->pidx[p->plist[c][i]] = (uint8_t)i;

	p->board[sq] = CE_EMPTY;
	p->count[c][t]--;

	p->key ^= zob_piece[zpi(pc)][CE_SQ64(sq)];
	p->psq_mg[c] -= ce_psq_mg_of(c, t, sq);
	p->psq_eg[c] -= ce_psq_eg_of(c, t, sq);

	if (t != CE_PAWN && t != CE_KING) p->npmat[c] -= ce_val_mg[t];

}

static void move_piece(ce_pos_t *p, uint8_t from, uint8_t to) {

	uint8_t pc = p->board[from];
	int c = CE_COLOR(pc), t = CE_TYPE(pc);
	int i = p->pidx[from];

	p->board[from] = CE_EMPTY;
	p->board[to] = pc;

	p->plist[c][i] = to;
	p->pidx[to] = (uint8_t)i;

	p->key ^= zob_piece[zpi(pc)][CE_SQ64(from)];
	p->key ^= zob_piece[zpi(pc)][CE_SQ64(to)];

	p->psq_mg[c] += ce_psq_mg_of(c, t, to) - ce_psq_mg_of(c, t, from);
	p->psq_eg[c] += ce_psq_eg_of(c, t, to) - ce_psq_eg_of(c, t, from);

	if (t == CE_KING) p->ksq[c] = to;

}

/* -- setup ---------------------------------------------------------- */

static void clear_pos(ce_pos_t *p) {
	memset(p, 0, sizeof(*p));
	p->ep = CE_SQ_NONE;
	p->fullmove = 1;
	p->ksq[0] = p->ksq[1] = CE_SQ_NONE;
}

void ce_start_position(ce_pos_t *p) {
	ce_set_fen(p,
		"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

static uint8_t fen_piece(char c) {
	switch (c) {
	case 'P': return CE_PIECE(CE_WHITE, CE_PAWN);
	case 'N': return CE_PIECE(CE_WHITE, CE_KNIGHT);
	case 'B': return CE_PIECE(CE_WHITE, CE_BISHOP);
	case 'R': return CE_PIECE(CE_WHITE, CE_ROOK);
	case 'Q': return CE_PIECE(CE_WHITE, CE_QUEEN);
	case 'K': return CE_PIECE(CE_WHITE, CE_KING);
	case 'p': return CE_PIECE(CE_BLACK, CE_PAWN);
	case 'n': return CE_PIECE(CE_BLACK, CE_KNIGHT);
	case 'b': return CE_PIECE(CE_BLACK, CE_BISHOP);
	case 'r': return CE_PIECE(CE_BLACK, CE_ROOK);
	case 'q': return CE_PIECE(CE_BLACK, CE_QUEEN);
	case 'k': return CE_PIECE(CE_BLACK, CE_KING);
	}
	return CE_EMPTY;
}

static char piece_fen(uint8_t pc) {
	static const char w[7] = { ' ', 'P', 'N', 'B', 'R', 'Q', 'K' };
	static const char b[7] = { ' ', 'p', 'n', 'b', 'r', 'q', 'k' };
	return CE_COLOR(pc) == CE_WHITE ? w[CE_TYPE(pc)] : b[CE_TYPE(pc)];
}

bool ce_set_fen(ce_pos_t *p, const char *fen) {

	int rank = 7, file = 0;
	const char *s = fen;

	ce_init();
	clear_pos(p);

	if (!fen) return false;

	/* 1. placement */
	while (*s && *s != ' ') {
		if (*s == '/') {
			rank--;
			file = 0;
			if (rank < 0) return false;
		} else if (*s >= '1' && *s <= '8') {
			file += *s - '0';
		} else {
			uint8_t pc = fen_piece(*s);
			if (pc == CE_EMPTY || file > 7 || rank < 0) return false;
			/* Two kings of one colour would break ksq and every
			 * legality test built on it, so refuse rather than
			 * produce a position that looks fine and searches
			 * nonsense. */
			if (CE_TYPE(pc) == CE_KING &&
				p->count[CE_COLOR(pc)][CE_KING]) return false;
			if (p->pnum[CE_COLOR(pc)] >= 16) return false;
			add_piece(p, CE_SQ(file, rank), pc);
			file++;
		}
		s++;
	}

	if (rank != 0) return false;

	while (*s == ' ') s++;

	/* 2. side to move */
	if (*s == 'b') p->side = CE_BLACK;
	else if (*s == 'w') p->side = CE_WHITE;
	else return false;
	s++;

	while (*s == ' ') s++;

	/* 3. castling rights */
	if (*s == '-') {
		s++;
	} else {
		while (*s && *s != ' ') {
			switch (*s) {
			case 'K': p->castle |= CE_CASTLE_WK; break;
			case 'Q': p->castle |= CE_CASTLE_WQ; break;
			case 'k': p->castle |= CE_CASTLE_BK; break;
			case 'q': p->castle |= CE_CASTLE_BQ; break;
			default: return false;
			}
			s++;
		}
	}

	while (*s == ' ') s++;

	/* 4. en passant target */
	if (*s == '-') {
		s++;
	} else if (s[0] >= 'a' && s[0] <= 'h' && s[1] >= '1' && s[1] <= '8') {
		p->ep = CE_SQ(s[0] - 'a', s[1] - '1');
		s += 2;
	} else if (*s) {
		return false;
	}

	while (*s == ' ') s++;

	/* 5. halfmove clock, 6. fullmove number -- both optional, because
	 * plenty of FEN found in the wild stops after the en passant
	 * field and refusing it would be pedantry rather than safety. */
	if (*s >= '0' && *s <= '9') {
		int v = 0;
		while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
		p->halfmove = (uint16_t)v;
		while (*s == ' ') s++;
		if (*s >= '0' && *s <= '9') {
			v = 0;
			while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
			p->fullmove = (uint16_t)(v ? v : 1);
		}
	}

	/* Both kings must exist. Without this every attack test would
	 * dereference ksq == CE_SQ_NONE. */
	if (!p->count[CE_WHITE][CE_KING] || !p->count[CE_BLACK][CE_KING])
		return false;

	/* Rights that no rook or king can support are dropped rather than
	 * refused -- the position is still playable, and a FEN that says
	 * "KQkq" out of habit is common enough that refusing it would be
	 * unhelpful. */
	if (p->board[CE_SQ(4, 0)] != CE_PIECE(CE_WHITE, CE_KING))
		p->castle &= ~(CE_CASTLE_WK | CE_CASTLE_WQ);
	if (p->board[CE_SQ(7, 0)] != CE_PIECE(CE_WHITE, CE_ROOK))
		p->castle &= ~CE_CASTLE_WK;
	if (p->board[CE_SQ(0, 0)] != CE_PIECE(CE_WHITE, CE_ROOK))
		p->castle &= ~CE_CASTLE_WQ;
	if (p->board[CE_SQ(4, 7)] != CE_PIECE(CE_BLACK, CE_KING))
		p->castle &= ~(CE_CASTLE_BK | CE_CASTLE_BQ);
	if (p->board[CE_SQ(7, 7)] != CE_PIECE(CE_BLACK, CE_ROOK))
		p->castle &= ~CE_CASTLE_BK;
	if (p->board[CE_SQ(0, 7)] != CE_PIECE(CE_BLACK, CE_ROOK))
		p->castle &= ~CE_CASTLE_BQ;

	p->key = ce_compute_key(p);

	return true;

}

void ce_get_fen(const ce_pos_t *p, char *out, int outlen) {

	char buf[100];
	int n = 0, rank, file, v;

	for (rank = 7; rank >= 0; rank--) {
		int run = 0;
		for (file = 0; file < 8; file++) {
			uint8_t pc = p->board[CE_SQ(file, rank)];
			if (pc == CE_EMPTY) { run++; continue; }
			if (run) { buf[n++] = (char)('0' + run); run = 0; }
			buf[n++] = piece_fen(pc);
		}
		if (run) buf[n++] = (char)('0' + run);
		if (rank) buf[n++] = '/';
	}

	buf[n++] = ' ';
	buf[n++] = p->side == CE_WHITE ? 'w' : 'b';
	buf[n++] = ' ';

	if (!p->castle) {
		buf[n++] = '-';
	} else {
		if (p->castle & CE_CASTLE_WK) buf[n++] = 'K';
		if (p->castle & CE_CASTLE_WQ) buf[n++] = 'Q';
		if (p->castle & CE_CASTLE_BK) buf[n++] = 'k';
		if (p->castle & CE_CASTLE_BQ) buf[n++] = 'q';
	}

	buf[n++] = ' ';
	if (p->ep == CE_SQ_NONE) {
		buf[n++] = '-';
	} else {
		buf[n++] = (char)('a' + CE_FILE(p->ep));
		buf[n++] = (char)('1' + CE_RANK(p->ep));
	}

	buf[n++] = ' ';
	v = p->halfmove;
	if (v >= 100) buf[n++] = (char)('0' + v / 100);
	if (v >= 10)  buf[n++] = (char)('0' + (v / 10) % 10);
	buf[n++] = (char)('0' + v % 10);

	buf[n++] = ' ';
	v = p->fullmove;
	if (v >= 1000) buf[n++] = (char)('0' + v / 1000);
	if (v >= 100)  buf[n++] = (char)('0' + (v / 100) % 10);
	if (v >= 10)   buf[n++] = (char)('0' + (v / 10) % 10);
	buf[n++] = (char)('0' + v % 10);

	buf[n] = 0;

	if (outlen > 0) {
		int i;
		for (i = 0; i < outlen - 1 && buf[i]; i++) out[i] = buf[i];
		out[i] = 0;
	}

}

/* -- attack detection ----------------------------------------------- */

/*
 * Asked from the square, not from the pieces. Walking the attacker's
 * piece list would be 16 iterations with a switch in each; this is
 * eight short ray walks and three fixed-offset loops, and every load
 * it makes is from board[], which is 128 contiguous bytes rather than
 * scattered.
 */
bool ce_attacked(const ce_pos_t *p, uint8_t sq, uint8_t by) {

	int i, s;
	uint8_t pc;
	const uint8_t pawn = CE_PIECE(by, CE_PAWN);
	const uint8_t knight = CE_PIECE(by, CE_KNIGHT);
	const uint8_t king = CE_PIECE(by, CE_KING);
	const uint8_t bishop = CE_PIECE(by, CE_BISHOP);
	const uint8_t rook = CE_PIECE(by, CE_ROOK);
	const uint8_t queen = CE_PIECE(by, CE_QUEEN);

	/* Pawns. A white pawn attacking `sq` stands one rank BELOW it,
	 * hence the subtraction -- getting this direction backwards is
	 * the classic version of this bug and it is invisible until a
	 * king walks into a pawn's teeth. */
	if (by == CE_WHITE) {
		s = (int)sq - 17;
		if (s >= 0 && !(s & 0x88) && p->board[s] == pawn) return true;
		s = (int)sq - 15;
		if (s >= 0 && !(s & 0x88) && p->board[s] == pawn) return true;
	} else {
		s = (int)sq + 17;
		if (s < 128 && !(s & 0x88) && p->board[s] == pawn) return true;
		s = (int)sq + 15;
		if (s < 128 && !(s & 0x88) && p->board[s] == pawn) return true;
	}

	for (i = 0; i < 8; i++) {
		s = (int)sq + off_knight[i];
		if (s < 0 || s > 127 || (s & 0x88)) continue;
		if (p->board[s] == knight) return true;
	}

	for (i = 0; i < 8; i++) {
		s = (int)sq + off_king[i];
		if (s < 0 || s > 127 || (s & 0x88)) continue;
		if (p->board[s] == king) return true;
	}

	for (i = 0; i < 4; i++) {
		s = (int)sq + off_bishop[i];
		while (s >= 0 && s <= 127 && !(s & 0x88)) {
			pc = p->board[s];
			if (pc != CE_EMPTY) {
				if (pc == bishop || pc == queen) return true;
				break;
			}
			s += off_bishop[i];
		}
	}

	for (i = 0; i < 4; i++) {
		s = (int)sq + off_rook[i];
		while (s >= 0 && s <= 127 && !(s & 0x88)) {
			pc = p->board[s];
			if (pc != CE_EMPTY) {
				if (pc == rook || pc == queen) return true;
				break;
			}
			s += off_rook[i];
		}
	}

	return false;

}

bool ce_in_check(const ce_pos_t *p, uint8_t side) {
	return ce_attacked(p, p->ksq[side], (uint8_t)(side ^ 1));
}

/* -- move generation ------------------------------------------------ */

static void gen_promos(uint32_t **out, uint8_t from, uint8_t to,
	uint32_t flags) {
	/* Queen first: it is the right promotion in the overwhelming
	 * majority of positions, and emitting it first means the search's
	 * move ordering starts with it for free. Knight next, because it
	 * is the only underpromotion that is ever anything but a
	 * curiosity. */
	*(*out)++ = CE_MOVE(from, to, CE_QUEEN,  flags | CE_MF_PROMO);
	*(*out)++ = CE_MOVE(from, to, CE_KNIGHT, flags | CE_MF_PROMO);
	*(*out)++ = CE_MOVE(from, to, CE_ROOK,   flags | CE_MF_PROMO);
	*(*out)++ = CE_MOVE(from, to, CE_BISHOP, flags | CE_MF_PROMO);
}

/* captures_only folds the two generators into one body. They differ in
 * four places and duplicating a move generator to save four branches
 * is how the two copies end up disagreeing about en passant. */
static int gen(const ce_pos_t *p, uint32_t *out, bool captures_only) {

	uint32_t *w = out;
	const int us = p->side, them = us ^ 1;
	int n, i, s, dir;

	for (n = 0; n < p->pnum[us]; n++) {

		uint8_t from = p->plist[us][n];
		uint8_t pc = p->board[from];
		int t = CE_TYPE(pc);

		if (t == CE_PAWN) {

			int fwd = us == CE_WHITE ? 16 : -16;
			int start_rank = us == CE_WHITE ? 1 : 6;
			int promo_rank = us == CE_WHITE ? 7 : 0;
			int capt[2];

			capt[0] = fwd - 1;
			capt[1] = fwd + 1;

			/* pushes */
			s = (int)from + fwd;
			if (s >= 0 && s <= 127 && !(s & 0x88) &&
				p->board[s] == CE_EMPTY) {

				if (CE_RANK(s) == promo_rank) {
					gen_promos(&w, from, (uint8_t)s, 0);
				} else if (!captures_only) {
					*w++ = CE_MOVE(from, s, 0, 0);
					if (CE_RANK(from) == start_rank) {
						int s2 = s + fwd;
						if (p->board[s2] == CE_EMPTY)
							*w++ = CE_MOVE(from, s2, 0, CE_MF_PAWN2);
					}
				}
			}

			/* captures, including en passant */
			for (i = 0; i < 2; i++) {
				s = (int)from + capt[i];
				if (s < 0 || s > 127 || (s & 0x88)) continue;
				if (p->board[s] != CE_EMPTY) {
					if (CE_COLOR(p->board[s]) != them) continue;
					if (CE_RANK(s) == promo_rank)
						gen_promos(&w, from, (uint8_t)s, CE_MF_CAPTURE);
					else
						*w++ = CE_MOVE(from, s, 0, CE_MF_CAPTURE);
				} else if ((uint8_t)s == p->ep) {
					*w++ = CE_MOVE(from, s, 0,
						CE_MF_CAPTURE | CE_MF_EP);
				}
			}

			continue;
		}

		if (t == CE_KNIGHT || t == CE_KING) {

			const int8_t *offs = t == CE_KNIGHT ? off_knight : off_king;

			for (i = 0; i < 8; i++) {
				s = (int)from + offs[i];
				if (s < 0 || s > 127 || (s & 0x88)) continue;
				if (p->board[s] == CE_EMPTY) {
					if (!captures_only) *w++ = CE_MOVE(from, s, 0, 0);
				} else if (CE_COLOR(p->board[s]) == them) {
					*w++ = CE_MOVE(from, s, 0, CE_MF_CAPTURE);
				}
			}

			continue;
		}

		/* sliders */
		{
			const int8_t *offs;
			int ndir;

			if (t == CE_BISHOP)      { offs = off_bishop; ndir = 4; }
			else if (t == CE_ROOK)   { offs = off_rook;   ndir = 4; }
			else                     { offs = off_king;   ndir = 8; }

			for (dir = 0; dir < ndir; dir++) {
				s = (int)from + offs[dir];
				while (s >= 0 && s <= 127 && !(s & 0x88)) {
					if (p->board[s] == CE_EMPTY) {
						if (!captures_only)
							*w++ = CE_MOVE(from, s, 0, 0);
					} else {
						if (CE_COLOR(p->board[s]) == them)
							*w++ = CE_MOVE(from, s, 0, CE_MF_CAPTURE);
						break;
					}
					s += offs[dir];
				}
			}
		}
	}

	/* Castling. Quiet by definition, so never generated for
	 * quiescence. The three squares tested for attack are the king's
	 * origin, the square it passes over and its destination -- the
	 * rook's path is only tested for OCCUPANCY, which is why the
	 * queenside has three empty-square tests and two attack tests
	 * beyond the origin, not three of each. b1 may be attacked. */
	if (!captures_only) {
		if (us == CE_WHITE) {
			if ((p->castle & CE_CASTLE_WK) &&
				p->board[0x05] == CE_EMPTY && p->board[0x06] == CE_EMPTY &&
				!ce_attacked(p, 0x04, CE_BLACK) &&
				!ce_attacked(p, 0x05, CE_BLACK) &&
				!ce_attacked(p, 0x06, CE_BLACK))
				*w++ = CE_MOVE(0x04, 0x06, 0, CE_MF_CASTLE);
			if ((p->castle & CE_CASTLE_WQ) &&
				p->board[0x03] == CE_EMPTY && p->board[0x02] == CE_EMPTY &&
				p->board[0x01] == CE_EMPTY &&
				!ce_attacked(p, 0x04, CE_BLACK) &&
				!ce_attacked(p, 0x03, CE_BLACK) &&
				!ce_attacked(p, 0x02, CE_BLACK))
				*w++ = CE_MOVE(0x04, 0x02, 0, CE_MF_CASTLE);
		} else {
			if ((p->castle & CE_CASTLE_BK) &&
				p->board[0x75] == CE_EMPTY && p->board[0x76] == CE_EMPTY &&
				!ce_attacked(p, 0x74, CE_WHITE) &&
				!ce_attacked(p, 0x75, CE_WHITE) &&
				!ce_attacked(p, 0x76, CE_WHITE))
				*w++ = CE_MOVE(0x74, 0x76, 0, CE_MF_CASTLE);
			if ((p->castle & CE_CASTLE_BQ) &&
				p->board[0x73] == CE_EMPTY && p->board[0x72] == CE_EMPTY &&
				p->board[0x71] == CE_EMPTY &&
				!ce_attacked(p, 0x74, CE_WHITE) &&
				!ce_attacked(p, 0x73, CE_WHITE) &&
				!ce_attacked(p, 0x72, CE_WHITE))
				*w++ = CE_MOVE(0x74, 0x72, 0, CE_MF_CASTLE);
		}
	}

	return (int)(w - out);

}

int ce_gen_moves(const ce_pos_t *p, uint32_t *out) {
	return gen(p, out, false);
}

int ce_gen_captures(const ce_pos_t *p, uint32_t *out) {
	return gen(p, out, true);
}

int ce_gen_legal(ce_pos_t *p, uint32_t *out) {

	uint32_t tmp[CE_MAX_MOVES];
	int n = gen(p, tmp, false);
	int i, k = 0;

	for (i = 0; i < n; i++) {
		ce_undo_t u;
		if (ce_make(p, tmp[i], &u)) {
			out[k++] = tmp[i];
			ce_unmake(p, &u);
		}
	}

	return k;

}

/* -- make / unmake -------------------------------------------------- */

/* Which rights a square's involvement destroys. Indexed by 0x88
 * square, so both the from and the to square can be looked up with no
 * branches: a rook captured on h8 must clear Black's kingside right
 * just as surely as a rook moving off h8 does, and a table gets both
 * cases right without either being written twice. */
static uint8_t castle_mask(uint8_t sq) {
	switch (sq) {
	case 0x00: return CE_CASTLE_WQ;
	case 0x04: return CE_CASTLE_WQ | CE_CASTLE_WK;
	case 0x07: return CE_CASTLE_WK;
	case 0x70: return CE_CASTLE_BQ;
	case 0x74: return CE_CASTLE_BQ | CE_CASTLE_BK;
	case 0x77: return CE_CASTLE_BK;
	}
	return 0;
}

bool ce_make(ce_pos_t *p, uint32_t move, ce_undo_t *u) {

	uint8_t from = CE_MOVE_FROM(move);
	uint8_t to = CE_MOVE_TO(move);
	uint32_t flags = CE_MOVE_FLAGS(move);
	uint8_t pc = p->board[from];
	int us = p->side, them = us ^ 1;
	uint8_t new_castle;

	u->move = move;
	u->key = p->key;
	u->castle = p->castle;
	u->ep = p->ep;
	u->halfmove = p->halfmove;
	u->captured = CE_EMPTY;

	/* The old en passant square leaves the key here, once, whatever
	 * happens below. */
	if (p->ep != CE_SQ_NONE) p->key ^= zob_ep[CE_FILE(p->ep)];
	p->ep = CE_SQ_NONE;

	if (flags & CE_MF_EP) {
		uint8_t cap_sq = (uint8_t)(us == CE_WHITE ? to - 16 : to + 16);
		u->captured = p->board[cap_sq];
		remove_piece(p, cap_sq);
	} else if (flags & CE_MF_CAPTURE) {
		u->captured = p->board[to];
		remove_piece(p, to);
	}

	move_piece(p, from, to);

	if (flags & CE_MF_PROMO) {
		remove_piece(p, to);
		add_piece(p, to, CE_PIECE(us, CE_MOVE_PROMO(move)));
	}

	if (flags & CE_MF_CASTLE) {
		/* The king has already moved; the rook follows. to is g1/c1/
		 * g8/c8, so its file decides which side. */
		if (CE_FILE(to) == 6)
			move_piece(p, (uint8_t)(to + 1), (uint8_t)(to - 1));
		else
			move_piece(p, (uint8_t)(to - 2), (uint8_t)(to + 1));
	}

	if (flags & CE_MF_PAWN2) {
		p->ep = (uint8_t)(us == CE_WHITE ? from + 16 : from - 16);
		p->key ^= zob_ep[CE_FILE(p->ep)];
	}

	new_castle = (uint8_t)(p->castle & ~(castle_mask(from) | castle_mask(to)));
	if (new_castle != p->castle) {
		p->key ^= zob_castle[p->castle & 15];
		p->key ^= zob_castle[new_castle & 15];
		p->castle = new_castle;
	}

	if (CE_TYPE(pc) == CE_PAWN || (flags & CE_MF_CAPTURE))
		p->halfmove = 0;
	else
		p->halfmove++;

	if (us == CE_BLACK) p->fullmove++;

	p->side = (uint8_t)them;
	p->key ^= zob_side;

	/* Legality last. Generating pseudo-legal moves and filtering here
	 * is one make/unmake per illegal move, against a pin-and-ray
	 * analysis that would have to be right for every one of en
	 * passant's peculiar discovered-check cases. At this search depth
	 * the extra make/unmake is not where the time goes. */
	if (ce_attacked(p, p->ksq[us], (uint8_t)them)) {
		ce_unmake(p, u);
		return false;
	}

	return true;

}

void ce_unmake(ce_pos_t *p, const ce_undo_t *u) {

	uint32_t move = u->move;
	uint8_t from = CE_MOVE_FROM(move);
	uint8_t to = CE_MOVE_TO(move);
	uint32_t flags = CE_MOVE_FLAGS(move);
	int us = p->side ^ 1;

	p->side = (uint8_t)us;
	if (us == CE_BLACK) p->fullmove--;

	if (flags & CE_MF_CASTLE) {
		if (CE_FILE(to) == 6)
			move_piece(p, (uint8_t)(to - 1), (uint8_t)(to + 1));
		else
			move_piece(p, (uint8_t)(to + 1), (uint8_t)(to - 2));
	}

	if (flags & CE_MF_PROMO) {
		remove_piece(p, to);
		add_piece(p, to, CE_PIECE(us, CE_PAWN));
	}

	move_piece(p, to, from);

	if (flags & CE_MF_EP) {
		uint8_t cap_sq = (uint8_t)(us == CE_WHITE ? to - 16 : to + 16);
		add_piece(p, cap_sq, u->captured);
	} else if (flags & CE_MF_CAPTURE) {
		add_piece(p, to, u->captured);
	}

	p->castle = u->castle;
	p->ep = u->ep;
	p->halfmove = u->halfmove;

	/* The key is restored wholesale rather than unwound XOR by XOR.
	 * Unwinding would duplicate every one of ce_make()'s key
	 * operations in reverse, which is twice the code and exactly the
	 * place a missed case hides -- and the saving would be four bytes
	 * of undo record. */
	p->key = u->key;

}

void ce_make_null(ce_pos_t *p, ce_undo_t *u) {

	u->move = CE_MOVE_NONE;
	u->key = p->key;
	u->castle = p->castle;
	u->ep = p->ep;
	u->halfmove = p->halfmove;
	u->captured = CE_EMPTY;

	if (p->ep != CE_SQ_NONE) p->key ^= zob_ep[CE_FILE(p->ep)];
	p->ep = CE_SQ_NONE;

	p->side ^= 1;
	p->key ^= zob_side;
	p->halfmove++;

}

void ce_unmake_null(ce_pos_t *p, const ce_undo_t *u) {
	p->side ^= 1;
	p->ep = u->ep;
	p->castle = u->castle;
	p->halfmove = u->halfmove;
	p->key = u->key;
}

/* -- game state ----------------------------------------------------- */

bool ce_insufficient_material(const ce_pos_t *p) {

	int c;

	/* Any pawn, rook or queen anywhere means a mate is constructible,
	 * so only the minor-piece cases need thinking about. */
	for (c = 0; c < 2; c++)
		if (p->count[c][CE_PAWN] || p->count[c][CE_ROOK] ||
			p->count[c][CE_QUEEN]) return false;

	for (c = 0; c < 2; c++) {
		int minors = p->count[c][CE_KNIGHT] + p->count[c][CE_BISHOP];
		/* Two bishops mate; two knights cannot force one but the
		 * position is not drawn by rule, and a bishop and knight
		 * mate. So anything past a single minor is "material
		 * sufficient" for this purpose. */
		if (minors > 1) return false;
	}

	return true;

}

ce_result_t ce_result(ce_pos_t *p, const uint32_t *history, int hn) {

	uint32_t moves[CE_MAX_MOVES];
	int n = ce_gen_legal(p, moves);

	if (n == 0)
		return ce_in_check(p, p->side) ? CE_RESULT_CHECKMATE
		                               : CE_RESULT_STALEMATE;

	if (p->halfmove >= 100) return CE_RESULT_FIFTY;

	if (ce_insufficient_material(p)) return CE_RESULT_MATERIAL;

	if (history && hn > 0) {
		int i, reps = 0;
		for (i = 0; i < hn; i++)
			if (history[i] == p->key) reps++;
		if (reps >= 3) return CE_RESULT_REPETITION;
	}

	return CE_RESULT_NONE;

}

/* -- notation ------------------------------------------------------- */

void ce_move_to_coord(uint32_t move, char *out) {

	static const char pc[7] = { 0, 0, 'n', 'b', 'r', 'q', 0 };
	uint8_t from = CE_MOVE_FROM(move), to = CE_MOVE_TO(move);
	int n = 0;

	if (move == CE_MOVE_NONE) { out[0] = '-'; out[1] = '-'; out[2] = 0; return; }

	out[n++] = (char)('a' + CE_FILE(from));
	out[n++] = (char)('1' + CE_RANK(from));
	out[n++] = (char)('a' + CE_FILE(to));
	out[n++] = (char)('1' + CE_RANK(to));
	if (CE_MOVE_PROMO(move)) out[n++] = pc[CE_MOVE_PROMO(move)];
	out[n] = 0;

}

void ce_move_to_san(ce_pos_t *p, uint32_t move, char *out) {

	static const char letter[7] = { 0, 0, 'N', 'B', 'R', 'Q', 'K' };
	uint32_t legal[CE_MAX_MOVES];
	uint8_t from = CE_MOVE_FROM(move), to = CE_MOVE_TO(move);
	uint8_t pc = p->board[from];
	int t = CE_TYPE(pc);
	uint32_t flags = CE_MOVE_FLAGS(move);
	int n = 0, i, nl;
	bool same_file = false, same_rank = false, ambiguous = false;
	ce_undo_t u;

	if (move == CE_MOVE_NONE) { out[0] = '-'; out[1] = '-'; out[2] = 0; return; }

	if (flags & CE_MF_CASTLE) {
		if (CE_FILE(to) == 6) { out[n++] = 'O'; out[n++] = '-'; out[n++] = 'O'; }
		else { out[n++] = 'O'; out[n++] = '-'; out[n++] = 'O';
		       out[n++] = '-'; out[n++] = 'O'; }
	} else if (t == CE_PAWN) {
		if (flags & CE_MF_CAPTURE) {
			out[n++] = (char)('a' + CE_FILE(from));
			out[n++] = 'x';
		}
		out[n++] = (char)('a' + CE_FILE(to));
		out[n++] = (char)('1' + CE_RANK(to));
		if (flags & CE_MF_PROMO) {
			out[n++] = '=';
			out[n++] = letter[CE_MOVE_PROMO(move)];
		}
	} else {

		out[n++] = letter[t];

		/* Disambiguation is decided against the LEGAL move list, not
		 * the pseudo-legal one. A second knight that is pinned cannot
		 * actually go there, so naming it would be noise -- and
		 * worse, a reader would look for a move that is not
		 * available. */
		nl = ce_gen_legal(p, legal);
		for (i = 0; i < nl; i++) {
			uint8_t f2 = CE_MOVE_FROM(legal[i]);
			if (f2 == from) continue;
			if (CE_MOVE_TO(legal[i]) != to) continue;
			if (CE_TYPE(p->board[f2]) != t) continue;
			ambiguous = true;
			if (CE_FILE(f2) == CE_FILE(from)) same_file = true;
			if (CE_RANK(f2) == CE_RANK(from)) same_rank = true;
		}

		if (ambiguous) {
			if (!same_file) {
				out[n++] = (char)('a' + CE_FILE(from));
			} else if (!same_rank) {
				out[n++] = (char)('1' + CE_RANK(from));
			} else {
				out[n++] = (char)('a' + CE_FILE(from));
				out[n++] = (char)('1' + CE_RANK(from));
			}
		}

		if (flags & CE_MF_CAPTURE) out[n++] = 'x';
		out[n++] = (char)('a' + CE_FILE(to));
		out[n++] = (char)('1' + CE_RANK(to));
	}

	/* The check/mate suffix needs the position AFTER the move, so
	 * this makes it. ce_make() can return false for a move the caller
	 * believed legal (a caller replaying a saved game against a
	 * different position, say) -- in that case the suffix is simply
	 * omitted rather than the function lying about the position. */
	if (ce_make(p, move, &u)) {
		if (ce_in_check(p, p->side)) {
			uint32_t reply[CE_MAX_MOVES];
			out[n++] = ce_gen_legal(p, reply) == 0 ? '#' : '+';
		}
		ce_unmake(p, &u);
	}

	out[n] = 0;

}

/* Strips everything that carries no information: separators, capture
 * and promotion markers, check and mate suffixes, and the annotator's
 * exclamation marks. What is left is letters and digits, which is the
 * part that identifies the move. */
static void san_squash(const char *in, char *out, int outlen, bool lower) {

	int n = 0;

	while (*in && n < outlen - 1) {
		char c = *in++;
		if (c == 'x' || c == 'X' || c == '=' || c == '+' || c == '#' ||
			c == '!' || c == '?' || c == ' ' || c == '\t' || c == '-')
			continue;
		if (c == '0') c = 'O';         /* 0-0 is a common way to type it */
		if (lower && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		out[n++] = c;
	}

	out[n] = 0;

}

uint32_t ce_parse_move(ce_pos_t *p, const char *s, bool *ambiguous) {

	uint32_t legal[CE_MAX_MOVES];
	char want[24], want_lc[24];
	char cand[24], cand_lc[24];
	char buf[16];
	int n, i;
	uint32_t exact = CE_MOVE_NONE, loose = CE_MOVE_NONE;
	int nexact = 0, nloose = 0;

	if (ambiguous) *ambiguous = false;
	if (!s || !*s) return CE_MOVE_NONE;

	san_squash(s, want, sizeof(want), false);
	san_squash(s, want_lc, sizeof(want_lc), true);

	if (!want[0]) return CE_MOVE_NONE;

	n = ce_gen_legal(p, legal);

	for (i = 0; i < n; i++) {

		int form;
		bool hit_exact = false, hit_loose = false;

		/* Three ways the same move can be written, all compared: the
		 * coordinate form the UI uses, the SAN a person would write,
		 * and long algebraic, which is what people type when they are
		 * being careful and what a lot of chess software emits.
		 *
		 * Counted once per MOVE and not once per form -- a move whose
		 * SAN and long-algebraic spellings both match the input is
		 * still one move, and counting the forms would report it as
		 * ambiguous with itself. */
		for (form = 0; form < 3; form++) {

			if (form == 0) {
				ce_move_to_coord(legal[i], buf);
			} else if (form == 1) {
				ce_move_to_san(p, legal[i], buf);
			} else {
				static const char letter[7] =
					{ 0, 0, 'N', 'B', 'R', 'Q', 'K' };
				int t = CE_TYPE(p->board[CE_MOVE_FROM(legal[i])]);
				int k = 0;
				if (t != CE_PAWN) buf[k++] = letter[t];
				ce_move_to_coord(legal[i], buf + k);
			}

			san_squash(buf, cand, sizeof(cand), false);
			san_squash(buf, cand_lc, sizeof(cand_lc), true);

			if (!strcmp(cand, want)) hit_exact = true;
			if (!strcmp(cand_lc, want_lc)) hit_loose = true;
		}

		if (hit_exact) { nexact++; exact = legal[i]; }
		if (hit_loose) { nloose++; loose = legal[i]; }
	}

	/* Case-sensitive first. "bxc3" and "Bxc3" are different moves and
	 * both may be legal; only fall back to ignoring case when the
	 * exact spelling matched nothing, which is where "NF3" and "e2E4"
	 * get accepted without ever letting a pawn move be read as a
	 * bishop move. */
	if (nexact == 1) return exact;
	if (nexact > 1) { if (ambiguous) *ambiguous = true; return CE_MOVE_NONE; }
	if (nloose == 1) return loose;
	if (nloose > 1) { if (ambiguous) *ambiguous = true; return CE_MOVE_NONE; }

	return CE_MOVE_NONE;

}

/* -- perft ---------------------------------------------------------- */

uint64_t ce_perft(ce_pos_t *p, int depth) {

	uint32_t moves[CE_MAX_MOVES];
	uint64_t total = 0;
	int n, i;

	if (depth <= 0) return 1;

	n = ce_gen_moves(p, moves);

	for (i = 0; i < n; i++) {
		ce_undo_t u;
		if (!ce_make(p, moves[i], &u)) continue;
		/* Depth 1 counts the move itself rather than recursing, which
		 * halves the work at the leaves. The legality filter has
		 * already run by the time we get here, so this counts legal
		 * moves and not pseudo-legal ones -- which is what perft
		 * means. */
		total += depth == 1 ? 1 : ce_perft(p, depth - 1);
		ce_unmake(p, &u);
	}

	return total;

}
