/*
 * Zeitlos mahjong -- host test for the music.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A misspelt cell plays as silence or as the wrong note, and nobody
 * notices which until it is on a board. So every cell is read with
 * zsfx's own parser (as sw/apps/mausrun/tests/test_sound.c does), and
 * the tune is held to its scale -- G A B D E -- which is what makes it
 * sound like it belongs to the game.
 */

#include <stdio.h>
#include <string.h>

#include "../mj_music.h"
#include "../mj_play.h"

/* The mixer, which the test never reaches: present, so that the song
 * is played and the notes it asks for can be counted. */
static int notes_played, notes_by_ch[8];
bool zsfxh_present(void) { return true; }
bool zsfxh_speech(void) { return false; }
uint32_t zsfxh_out_hz(void) { return 48000; }
void zsfxh_note(int ch, const int8_t *w, uint32_t l, uint32_t st, uint8_t a, uint8_t b)
{
	(void)w; (void)l; (void)st; (void)a; (void)b;
	notes_played++;
	if (ch >= 0 && ch < 8) notes_by_ch[ch]++;
}
void zsfxh_gain(int ch, uint32_t st, uint8_t a, uint8_t b) { (void)ch; (void)st; (void)a; (void)b; }
void zsfxh_off(int ch) { (void)ch; }
void zsfxh_release(void) { }

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	const zsfx_song_t *s = &mj_song;
	int r, ch, i, notes = 0, off_scale = 0;

	CHECK(s->nch == 3, "three channels: tune, harmony, bass");
	CHECK(s->rows_per_min >= 180 && s->rows_per_min <= 300, "a slow tempo (%u)", (unsigned)s->rows_per_min);
	CHECK(s->ins[0].gain + s->ins[1].gain + s->ins[2].gain < 100,
	      "the music well under the effects (sfx.md: equal levels bury every effect)");
	for (i = 0; i < zsfx_order_len(s->order); i++)
		CHECK(zsfx_order_at(s->order, i) < s->npat, "order names pattern %d", zsfx_order_at(s->order, i));
	CHECK(zsfx_order_len(s->order) == 8, "eight patterns before it comes round");

	for (r = 0; r < s->npat * 16; r++) {
		const char *row = s->rows[r];
		CHECK(strlen(row) == 11, "row %d is three cells (\"%s\")", r, row);
		for (ch = 0; ch < 3; ch++) {
			char cell[4];
			uint32_t v;
			memcpy(cell, row + ch * 4, 3);
			cell[3] = 0;
			if (!strcmp(cell, "---") || !strcmp(cell, "===")) continue;
			v = zsfx_parse_cell(cell);
			CHECK(v != ZSFX_CELL_HOLD && v < ZSFX_CELL_DRUM, "row %d ch %d: a note (\"%s\")", r, ch, cell);
			notes++;
			if (ch == 0 && !strchr("GABDE", cell[0])) off_scale++;
			if (ch == 0 && cell[1] == '#') off_scale++;
		}
	}
	CHECK(notes > 60, "%d notes", notes);
	CHECK(off_scale == 0, "the tune keeps to G A B D E (%d notes off it)", off_scale);

	/* Played: half a minute is one time round the order. */
	CHECK(zsfx_init(), "the (pretend) mixer is there");
	zsfx_music_play(&mj_song);
	for (i = 0; i < 60 * 32; i++) zsfx_tick(1);
	CHECK(notes_by_ch[0] > 20 && notes_by_ch[1] > 10 && notes_by_ch[2] > 10,
	      "the tune, the harmony and the bass all play (%d, %d, %d)",
	      notes_by_ch[0], notes_by_ch[1], notes_by_ch[2]);
	zsfx_music_enable(false);
	notes_played = 0;
	for (i = 0; i < 60 * 4; i++) zsfx_tick(1);
	CHECK(notes_played == 0, "M: off is off (%d notes)", notes_played);
	zsfx_shutdown();

	CHECK(mj_nfx == MJ_SND_SHUFFLE + 1, "a sound for every MJ_SND_*");
	for (i = 1; i < mj_nfx; i++)
		CHECK(mj_fx[i].n >= 1 && mj_fx[i].n <= 8 && mj_fx[i].seg, "sound %d has segments", i);

	printf("music_test: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
