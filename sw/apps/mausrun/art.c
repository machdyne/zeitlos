/*
 * mausrun -- the card's sprite sheets laid over the compiled-in art.
 * See art.h and docs/mausrun.md, "Art".
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "art.h"
#include "sprites.h"

static const mr_art_t *find(const char *name) {
	char low[ZSP_NAME];
	int i;
	for (i = 0; name[i] && i < ZSP_NAME - 1; i++) low[i] = (char)tolower((unsigned char)name[i]);
	low[i] = 0;
	for (i = 0; i < MR_ART_COUNT; i++)
		if (!strcmp(mr_art[i].name, low)) return &mr_art[i];
	return NULL;
}

/* An index as the art's few colours: a sheet of more than 4 is seen
 * through its 4-colour form, as a board with fewer colours would. */
static int four(const zsp_palette_t *p, int v) {
	if (v == ZSP_T) return v;
	return p->colours > 4 ? zsp_reduce(p, v, 4) : v;
}

int art_apply(const zsp_sheet_t *s, bool tiles, char *why, int whylen) {

	int n = 0, i, x, y;

	if (why && whylen) why[0] = 0;
	for (i = 0; i < s->n; i++) {
		const zsp_sprite_t *e = &s->spr[i];
		const mr_art_t *a = find(e->name);

		if (!a || (tiles ? !a->tile : !a->spr)) {
			if (why && !why[0])
				snprintf(why, (size_t)whylen, "no %s called %s", tiles ? "tile" : "sprite", e->name);
			continue;
		}
		if (e->w != 16 || e->h != 16) {
			if (why && !why[0]) snprintf(why, (size_t)whylen, "%s is not 16x16", e->name);
			continue;
		}

		for (y = 0; y < 16; y++) {
			uint32_t d = 0, m = 0;
			for (x = 0; x < 16; x++) {
				int v = four(&s->p, e->px[y * 16 + x]);
				if (tiles) {
					if (v != ZSP_T && v != 0) d |= 1u << x;
				} else if (v != ZSP_T) {
					m |= 1u << x;              /* 0 and 2: the outline, mask only */
					if (v != 0 && v != 2) d |= 1u << x;
				}
			}
			if (tiles) {
				a->tile[y] = d;
				continue;
			}
			a->spr[y] = (uint16_t)d;
			a->msk[y] = (uint16_t)m;
			a->hspr[y] = d;
			a->hmsk[y] = m;
			{
				uint32_t dm = 0, mm = 0;
				for (x = 0; x < 16; x++) {
					if (d & (1u << x)) dm |= 1u << (15 - x);
					if (m & (1u << x)) mm |= 1u << (15 - x);
				}
				a->hspr_m[y] = dm;
				a->hmsk_m[y] = mm;
			}
		}
		n++;
	}
	return n;
}

#ifndef ART_HOST
#include "../../common/zfsapp.h"
#include "../../common/zdata.h"

static void one(const char *leaf, bool tiles) {

	char path[96], err[80], why[64];
	zsp_sheet_t s;
	char *text;
	int len, n;

	if (!z_data_file(path, sizeof path, leaf)) return;
	len = fs_size(path);
	if (len <= 0) return;                    /* none on the card: the built-in art */
	text = malloc((size_t)len + 1);
	if (!text || fs_read_file(path, text, len + 1) != len) {
		printf("mausrun: %s: cannot read it\n", path);
		free(text);
		return;
	}
	if (!zsp_parse(&s, text, (uint32_t)len, err, sizeof err)) {
		printf("mausrun: %s: %s -- the built-in art\n", path, err);
		free(text);
		zsp_free(&s);
		return;
	}
	free(text);
	n = art_apply(&s, tiles, why, sizeof why);
	printf("mausrun: %s: %d of %d %s from the card%s%s\n", path, n, s.n,
		tiles ? "tiles" : "sprites", why[0] ? "; " : "", why);
	zsp_free(&s);
}

void art_load(void) {
	one("tiles.zsp", true);
	one("sprites.zsp", false);
}
#endif
