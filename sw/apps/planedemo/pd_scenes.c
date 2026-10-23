/*
 * planedemo -- the scenes, in the order of the film. See pd.h and
 * docs/planedemo.md, "The story".
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include "pd.h"
#include "pd_scenes.h"

const pd_scene_t *const pd_scenes[] = {
	&pd_scene_clock,
	&pd_scene_plasma,
	&pd_scene_storm,
	&pd_scene_mouse,
	&pd_scene_donut,
	&pd_scene_tunnel,
	&pd_scene_vectors,
	&pd_scene_sunrise,
	&pd_scene_ending,
};
const int pd_nscenes = (int)(sizeof(pd_scenes) / sizeof(pd_scenes[0]));

/* The instruments, made on the build host (pd_gfx.h): the samples
 * point into the film's data. */
void pd_synth_store(pd_store_t *st) {
	int i;
	if (st->writing) pd_synth_all();
	for (i = 0; i < PD_I_COUNT; i++) {
		pd_sample_t *s = &pd_samples[i];
		uint32_t base = (uint32_t)s->base_note, len = s->n * 2;
		void *d = s->d;
		pd_store_u32(st, &s->n);
		pd_store_u32(st, &s->loop_st);
		pd_store_u32(st, &s->loop_n);
		pd_store_u32(st, &base);
		len = s->n * 2;
		pd_store_block(st, &d, &len);
		if (!st->writing) { s->base_note = (int)base; s->d = (int16_t *)d; }
	}
}

/* The film's data (pd_gfx.h, "made on the build host"), in this order,
 * written by tests/mkfilm.c and read on the board: the instruments and
 * every scene's pictures. The ending's are for eight planes; with
 * fewer, `ending` is false and it makes its own. */
#define PD_FILM_MAGIC 0x31444450u      /* "PDD1" */

bool pd_film_visit(pd_store_t *st, bool ending) {
	uint32_t magic = PD_FILM_MAGIC;
	pd_store_u32(st, &magic);
	if (magic != PD_FILM_MAGIC) { st->bad = true; return false; }
	pd_synth_store(st);
	pd_clock_store(st);
	pd_plasma_store(st);
	pd_city_store(st);
	pd_donut_store(st);
	pd_tunnel_store(st);
	pd_sunrise_store(st);
	if (ending) pd_ending_store(st);
	return !st->bad;
}
