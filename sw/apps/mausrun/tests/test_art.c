/*
 * mausrun -- the card's sheets over the compiled-in art (art.c), on a
 * build host. The release's own sheets must change nothing; a changed
 * pixel must land in every form of the sprite, the mirrored ones too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../art.h"
#include "../sprites.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *path, uint32_t *len) {
	FILE *f = fopen(path, "rb");
	char *b;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return b;
}

/* every word of the art, in table order */
static int snap(uint32_t *out) {
	int i, r, n = 0;
	for (i = 0; i < MR_ART_COUNT; i++) {
		const mr_art_t *a = &mr_art[i];
		for (r = 0; r < 16; r++) {
			if (a->tile) { out[n++] = a->tile[r]; continue; }
			out[n++] = a->spr[r]; out[n++] = a->msk[r];
			out[n++] = a->hspr[r]; out[n++] = a->hmsk[r];
			out[n++] = a->hspr_m[r]; out[n++] = a->hmsk_m[r];
		}
	}
	return n;
}

static bool load(zsp_sheet_t *s, const char *path) {
	uint32_t len;
	char err[80];
	char *t = slurp(path, &len);
	bool ok;
	if (!t) { CHECK(0, "cannot read %s", path); return false; }
	ok = zsp_parse(s, t, len, err, sizeof err);
	CHECK(ok, "%s: %s", path, err);
	free(t);
	return ok;
}

int main(void) {
	static uint32_t before[MR_ART_COUNT * 16 * 6], after[MR_ART_COUNT * 16 * 6];
	zsp_sheet_t t, s;
	char why[64];
	int n, i;

	/* pix (and anything else using zsprite) writes them back unchanged */
	{
		static const char *paths[] = { "../../../data/mausrun/tiles.zsp",
			"../../../data/mausrun/sprites.zsp" };
		for (i = 0; i < 2; i++) {
			uint32_t len, olen = 0;
			char *orig = slurp(paths[i], &len), *out;
			zsp_sheet_t x;
			if (!orig || !load(&x, paths[i])) continue;
			out = zsp_write(&x, &olen);
			CHECK(out && olen == len && !memcmp(out, orig, len),
				"%s written back as it is (%u bytes, was %u)", paths[i], olen, len);
			free(out); free(orig); zsp_free(&x);
		}
	}

	n = snap(before);
	CHECK(load(&t, "../../../data/mausrun/tiles.zsp"), "tiles.zsp");
	CHECK(load(&s, "../../../data/mausrun/sprites.zsp"), "sprites.zsp");
	CHECK(art_apply(&t, true, why, sizeof why) == MR_TILE_COUNT && !why[0], "every tile (%s)", why);
	CHECK(art_apply(&s, false, why, sizeof why) == 25 && !why[0], "every sprite (%s)", why);
	CHECK(snap(after) == n && !memcmp(before, after, sizeof(uint32_t) * (size_t)n),
		"the release's sheets change nothing");

	/* one pixel of the mouse: set, in all six forms */
	i = zsp_find(&s, "mouse_stand");
	CHECK(i >= 0, "mouse_stand in the sheet");
	s.spr[i].px[2 * 16 + 1] = 1;                    /* row 2, column 1 */
	s.spr[i].px[2 * 16 + 0] = 2;                    /* column 0: outline */
	art_apply(&s, false, NULL, 0);
	CHECK((mr_spr_mouse_stand[2] & 2) && (mr_hspr_mouse_stand[2] & 2) &&
		(mr_hspr_mouse_stand_m[2] & (1u << 14)), "the pixel, mirrored too");
	CHECK(!(mr_spr_mouse_stand[2] & 1) && (mr_msk_mouse_stand[2] & 1) &&
		(mr_hmsk_mouse_stand_m[2] & (1u << 15)), "the outline: mask only");

	/* a name the game does not have, and a wrong size, are skipped */
	zsp_add(&s, "dragon", 16, 16);
	zsp_add(&s, "cat1", 8, 8);                      /* a second cat1: refused */
	art_apply(&s, false, why, sizeof why);
	CHECK(strstr(why, "dragon") != NULL, "said: %s", why);

	zsp_free(&t);
	zsp_free(&s);
	printf("mausrun art: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
