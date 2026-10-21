/*
 * mausrun -- the tunes read as the player reads them. make test
 *
 * Every cell of every tune (sound.c) through zsfx's own parser
 * (ZSFX_HOSTED): a note where a note is meant, a drum letter only on the
 * drum channel, sixteen rows a pattern, an order that names patterns
 * there are. A typo in a score is otherwise a silent beat, heard (or
 * not) only on the board.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../sound.h"

/* the hardware, which these tests never reach */
bool zsfxh_present(void) { return false; }
bool zsfxh_speech(void) { return false; }
uint32_t zsfxh_out_hz(void) { return 48000; }
void zsfxh_note(int ch, const int8_t *w, uint32_t l, uint32_t s, uint8_t a, uint8_t b) {
	(void)ch; (void)w; (void)l; (void)s; (void)a; (void)b;
}
void zsfxh_gain(int ch, uint32_t s, uint8_t a, uint8_t b) { (void)ch; (void)s; (void)a; (void)b; }
void zsfxh_off(int ch) { (void)ch; }
void zsfxh_release(void) { }

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void one(const char *name, const zsfx_song_t *s) {
	int r, ch, notes = 0, drums = 0, i;
	CHECK(s->nch == 4 && (s->ins[3].wave & ZSFX_DRUMS), "%s: tune, harmony, bass, drums", name);
	CHECK(s->rows_per_min >= 360 && s->rows_per_min <= 540, "%s: a tempo for children (%u)",
		name, (unsigned)s->rows_per_min);
	for (i = 0; i < zsfx_order_len(s->order); i++)
		CHECK(zsfx_order_at(s->order, i) < s->npat, "%s: order names pattern %d", name,
			zsfx_order_at(s->order, i));
	for (r = 0; r < s->npat * 16; r++) {
		const char *row = s->rows[r];
		CHECK(strlen(row) == 15, "%s: row %d is four cells (\"%s\")", name, r, row);
		for (ch = 0; ch < 4; ch++) {
			const char *c = row + ch * 4;
			char cellb[4];
			uint32_t v;
			memcpy(cellb, c, 3);
			cellb[3] = 0;
			v = zsfx_parse_cell(cellb);
			if (!strcmp(cellb, "---")) continue;
			if (!strcmp(cellb, "===")) { CHECK(ch < 3, "%s: row %d: off on drums", name, r); continue; }
			if (ch == 3) {
				CHECK(v >= ZSFX_CELL_DRUM && v <= ZSFX_CELL_DRUM + 3, "%s: row %d: drum \"%s\"", name, r, cellb);
				drums++;
			} else {
				CHECK(v != ZSFX_CELL_HOLD && v < ZSFX_CELL_DRUM, "%s: row %d ch %d: note \"%s\"",
					name, r, ch, cellb);
				notes++;
			}
		}
	}
	CHECK(notes > 30 && drums > 8, "%s: %d notes, %d drums", name, notes, drums);
}

int main(void) {
	one("meadow", &snd_song_meadow);
	one("garden", &snd_song_garden);
	one("kitchen", &snd_song_kitchen);
	one("roofs", &snd_song_roofs);
	CHECK(snd_song(0) == &snd_song_meadow && snd_song(3) == &snd_song_roofs &&
		snd_song(9) == &snd_song_meadow, "a song for every theme");
	printf("mausrun tunes: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
