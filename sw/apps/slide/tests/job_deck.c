/*
 * Writes a deck as a print job (zpaper, docs/print.md), the way slide's
 * /print does, for the print service's host tests:
 *
 *   make -C sw/apps/slide job DECK=../../data/slide/tour.zsl
 *   -> /tmp/slide/deck.zpp
 *
 * Options after the paths: key=value job options (nup=4 cut=1 ...).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../deck.h"
#include "../deckio.h"
#include "../render.h"
#include "../printjob.h"

static uint8_t mem[8 << 20];
static uint8_t scratch[96 * 1024];

static const char *root = "../../..";

static uint8_t *load(const char *path, uint32_t *len) {
	char p[600];
	FILE *f;
	if (!strncmp(path, "/sys/fonts/", 11)) {
		const char *e = strrchr(path, '.');
		snprintf(p, sizeof(p), "%s/sw/data/font/%s/%s", root, e ? e + 1 : "", path + 11);
	} else snprintf(p, sizeof(p), "%s", path);
	f = fopen(p, "rb");
	long n;
	uint8_t *b;
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

static bool to_file(void *ctx, const void *d, uint32_t n) {
	return fwrite(d, 1, n, (FILE *)ctx) == n;
}

int main(int argc, char **argv) {
	uint32_t len;
	uint8_t *text;
	deck_t *d;
	char err[96];
	struct { const char *key, *value; } opt[16];
	int nopt = 0, i;
	FILE *f;
	if (argc < 3) { fprintf(stderr, "job_deck DECK OUT.zpp [key=value ...]\n"); return 2; }
	pool_init(mem, sizeof(mem));
	render_init(scratch, sizeof(scratch), load);
	text = load(argv[1], &len);
	if (!text) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
	d = deckio_read((const char *)text, len, err, sizeof(err));
	if (!d) { fprintf(stderr, "%s: %s\n", argv[1], err); return 1; }
	opt[nopt].key = "title";
	opt[nopt++].value = d->title[0] ? d->title : "deck";
	for (i = 3; i < argc && nopt < 16; i++) {
		char *eq = strchr(argv[i], '=');
		if (!strncmp(argv[i], "root=", 5)) { root = argv[i] + 5; continue; }
		if (!eq) continue;
		*eq = 0;
		opt[nopt].key = argv[i];
		opt[nopt++].value = eq + 1;
	}
	f = fopen(argv[2], "wb");
	{
		zp_writer_t w;
		if (!f) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
		zp_writer_init(&w, to_file, f);
		for (i = 0; i < nopt; i++) zp_job(&w, opt[i].key, opt[i].value);
		if (!printjob_pages(d, 0, -1, &w)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
	}
	printf("job_deck: %d cards, %ld bytes\n", d->ncard, ftell(f));
	fclose(f);
	return 0;
}

