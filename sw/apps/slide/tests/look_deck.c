/*
 * Draws every card of a deck as PGM, at 100% (1bpp, as the editor and
 * the show draw it) -- to look at a deck without a board:
 *
 *   make -C sw/apps/slide look-deck DECK=../../data/slide/tour.zsl
 *   -> /tmp/slide/deck_N.pgm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../deck.h"
#include "../deckio.h"
#include "../render.h"

static uint8_t mem[8 << 20];
static uint8_t scratch[96 * 1024];
static const char *root = "../../..";
extern const char *hostfs_root;

static uint8_t *load(const char *path, uint32_t *len) {
	char p[512];
	FILE *f;
	long n;
	uint8_t *b;
	if (!strncmp(path, "/sys/fonts/", 11)) {
		const char *e = strrchr(path, '.');
		snprintf(p, sizeof(p), "%s/sw/data/font/%s/%s", root, e ? e + 1 : "", path + 11);
	} else if (!strncmp(path, "/media/images/", 14)) {
		snprintf(p, sizeof(p), "%s/sw/data/images/%s", root, path + 14);
	} else snprintf(p, sizeof(p), "%s", path);
	f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = pool_alloc((size_t)n + 1);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { pool_free(b); b = NULL; }
	fclose(f);
	if (b) b[n] = 0;
	*len = (uint32_t)n;
	return b;
}

int main(int argc, char **argv) {
	static uint8_t cache[128 * 1024];
	uint32_t len;
	uint8_t *text;
	deck_t *d;
	char err[96];
	int i;
	if (argc < 3) { fprintf(stderr, "look_deck DECK OUTDIR [ROOT]\n"); return 2; }
	if (argc > 3) root = hostfs_root = argv[3];
	pool_init(mem, sizeof(mem));
	render_init(scratch, sizeof(scratch), load);
	zc_text_cache(cache, sizeof(cache));
	text = load(argv[1], &len);
	if (!text) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
	d = deckio_read((const char *)text, len, err, sizeof(err));
	if (!d) { fprintf(stderr, "%s: %s\n", argv[1], err); return 1; }
	for (i = 0; i < d->ncard; i++) {
		int wpl = (d->page.gw + 31) / 32, x, y;
		uint32_t *bits = calloc((size_t)wpl * d->page.gh + 2, 4);
		zc_target_t t;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, 0, 0);
		char p[512];
		FILE *f;
		zc_target_bits(&t, bits, wpl, d->page.gw, d->page.gh);
		render_card(&t, &xf, d, d->card[i], RENDER_SHOW);
		if (render_loading()) {
			// its pictures, as the app's main loop would load them; then
			// the card again
			bool fin;
			while (render_loading()) render_load_step(1000, &fin);
			memset(bits, 0, (size_t)wpl * d->page.gh * 4);
			render_card(&t, &xf, d, d->card[i], RENDER_SHOW);
		}
		snprintf(p, sizeof(p), "%s/deck_%d.pgm", argv[2], i + 1);
		f = fopen(p, "wb");
		fprintf(f, "P5 %d %d 255\n", d->page.gw, d->page.gh);
		for (y = 0; y < d->page.gh; y++)
			for (x = 0; x < d->page.gw; x++) fputc(zc_get_bit(&t, x, y) ? 255 : 0, f);
		fclose(f);
		free(bits);
	}
	printf("look_deck: %d cards\n", d->ncard);
	return 0;
}

