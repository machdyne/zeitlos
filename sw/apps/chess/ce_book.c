/*
 * Zeitlos chess -- opening book.
 * See ce_book.h for what this is for and why the lines are written in
 * coordinate notation.
 *
 * The lines are ordinary main lines of well known openings -- they
 * are facts about how chess is played, written out here by hand, not
 * extracted from anybody's book file.
 */

#include <string.h>

#include "ce_book.h"

#define CE_BOOK_MAX 384

typedef struct {
	uint32_t key;
	uint16_t move;
} book_entry_t;

static book_entry_t book[CE_BOOK_MAX];
static int book_n;
static bool book_ready;

static const char *const lines[] = {

	/* -- 1.e4 e5 -- */
	"e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1 f8e7",  /* Ruy Lopez */
	"e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5c6 d7c6 e1g1 f7f6",  /* Exchange */
	"e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d3 d7d6",  /* Italian */
	"e2e4 e7e5 g1f3 b8c6 d2d4 e5d4 f3d4 g8f6 b1c3 f8b4",  /* Scotch */
	"e2e4 e7e5 g1f3 g8f6 f3e5 d7d6 e5f3 f6e4 d2d4 d6d5",  /* Petrov */
	"e2e4 e7e5 b1c3 g8f6 f2f4 d7d5 f4e5 f6e4",            /* Vienna */
	"e2e4 e7e5 f2f4 e5f4 g1f3 g7g5 h2h4 g5g4",            /* King's Gambit */

	/* -- 1.e4, other replies -- */
	"e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6",  /* Najdorf */
	"e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g7g6 b1c3 f8g7",  /* Dragon-ish */
	"e2e4 c7c5 g1f3 e7e6 d2d4 c5d4 f3d4 a7a6",            /* Kan */
	"e2e4 c7c5 b1c3 b8c6 g2g3 g7g6 f1g2 f8g7",            /* Closed Sicilian */
	"e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5 f8e7",            /* French, Classical */
	"e2e4 e7e6 d2d4 d7d5 e4e5 c7c5 c2c3 b8c6",            /* French, Advance */
	"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5",            /* Caro-Kann */
	"e2e4 d7d5 e4d5 d8d5 b1c3 d5a5 d2d4 g8f6",            /* Scandinavian */
	"e2e4 g8f6 e4e5 f6d5 d2d4 d7d6 g1f3 c8g4",            /* Alekhine */
	"e2e4 d7d6 d2d4 g8f6 b1c3 g7g6 g1f3 f8g7",            /* Pirc */

	/* -- 1.d4 -- */
	"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7",            /* QGD */
	"d2d4 d7d5 c2c4 d5c4 g1f3 g8f6 e2e3 e7e6",            /* QGA */
	"d2d4 d7d5 c2c4 c7c6 g1f3 g8f6 b1c3 d5c4",            /* Slav */
	"d2d4 g8f6 c2c4 e7e6 b1c3 f8b4 d1c2 e8g8",            /* Nimzo-Indian */
	"d2d4 g8f6 c2c4 e7e6 g1f3 b7b6 g2g3 c8b7",            /* Queen's Indian */
	"d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6",            /* King's Indian */
	"d2d4 g8f6 c2c4 g7g6 b1c3 d7d5 c4d5 f6d5",            /* Gruenfeld */
	"d2d4 g8f6 c2c4 c7c5 d4d5 e7e6 b1c3 e6d5",            /* Benoni */
	"d2d4 g8f6 g1f3 g7g6 c1f4 f8g7 e2e3 e8g8",            /* London */
	"d2d4 f7f5 g2g3 g8f6 f1g2 e7e6 g1f3 f8e7",            /* Dutch */
	"d2d4 d7d5 c1f4 g8f6 e2e3 e7e6 g1f3 f8d6",            /* London vs d5 */

	/* -- flank -- */
	"c2c4 e7e5 b1c3 g8f6 g1f3 b8c6 g2g3 d7d5",            /* English */
	"c2c4 g8f6 b1c3 e7e6 e2e4 d7d5 e4e5 d5d4",            /* English/Mikenas */
	"g1f3 d7d5 d2d4 g8f6 c2c4 e7e6 b1c3 f8e7",            /* transposes */

};

#define NLINES ((int)(sizeof(lines) / sizeof(lines[0])))

int ce_book_line_count(void) { return NLINES; }
const char *ce_book_line(int i) {
	return (i >= 0 && i < NLINES) ? lines[i] : "";
}

int ce_book_size(void) { return book_n; }

/* Finds the legal move whose coordinate form is the four (or five)
 * characters at `s`. Returns CE_MOVE_NONE if there is none, which is
 * how a mistyped book line gets caught rather than silently skipped. */
static uint32_t coord_lookup(ce_pos_t *p, const char *s) {

	uint32_t moves[CE_MAX_MOVES];
	int n = ce_gen_legal(p, moves);
	int i;

	for (i = 0; i < n; i++) {
		char c[8];
		int len;
		ce_move_to_coord(moves[i], c);
		len = (int)strlen(c);
		if (strncmp(c, s, (size_t)len)) continue;
		/* The book writes promotions with the piece letter, so a
		 * four-character book token must not match a five-character
		 * promotion move. */
		if (s[len] != ' ' && s[len] != 0) continue;
		return moves[i];
	}

	return CE_MOVE_NONE;

}

static void book_add(uint32_t key, uint32_t move) {

	int i;

	if (book_n >= CE_BOOK_MAX) return;

	/* Lines overlap heavily -- every 1.e4 line shares its first ply.
	 * Without this the book would hold the same entry twenty times
	 * and the random pick would be biased towards whichever opening
	 * happened to be written out most often. */
	for (i = 0; i < book_n; i++)
		if (book[i].key == key && book[i].move == CE_MOVE_PACK16(move))
			return;

	book[book_n].key = key;
	book[book_n].move = CE_MOVE_PACK16(move);
	book_n++;

}

void ce_book_init(void) {

	int l;

	if (book_ready) return;
	book_ready = true;
	book_n = 0;

	ce_init();

	for (l = 0; l < NLINES; l++) {

		ce_pos_t p;
		const char *s = lines[l];

		ce_start_position(&p);

		while (*s) {

			uint32_t mv;
			ce_undo_t u;

			while (*s == ' ') s++;
			if (!*s) break;

			mv = coord_lookup(&p, s);
			/* An illegal token truncates this line and leaves the
			 * rest of the book alone. tests/search_test.c walks every
			 * line and fails if any token does not resolve, so a
			 * typo here is caught on the build machine rather than
			 * quietly shortening an opening. */
			if (mv == CE_MOVE_NONE) break;

			book_add(p.key, mv);

			if (!ce_make(&p, mv, &u)) break;

			while (*s && *s != ' ') s++;
		}
	}

}

uint32_t ce_book_move(ce_pos_t *p, uint32_t r) {

	uint16_t cand[8];
	int nc = 0, i;
	uint32_t moves[CE_MAX_MOVES];
	int n;
	uint16_t want;

	ce_book_init();

	for (i = 0; i < book_n && nc < 8; i++)
		if (book[i].key == p->key) cand[nc++] = book[i].move;

	if (nc == 0) return CE_MOVE_NONE;

	want = cand[r % (uint32_t)nc];

	/* The book is keyed by a 32-bit hash, so a collision would hand
	 * back a move from an unrelated position. Resolving it against
	 * the legal move list is what makes that harmless -- a collision
	 * costs a missed book hit, never an illegal move. */
	n = ce_gen_legal(p, moves);
	for (i = 0; i < n; i++)
		if (CE_MOVE_PACK16(moves[i]) == want) return moves[i];

	return CE_MOVE_NONE;

}
