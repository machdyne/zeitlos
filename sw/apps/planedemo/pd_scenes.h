#ifndef PD_SCENES_H
#define PD_SCENES_H

/*
 * planedemo -- the scenes, one file each (pd_s_*.c), in film order in
 * pd_scenes.c.
 */

#include "pd.h"
#include "pd_gfx.h"

extern const pd_scene_t pd_scene_clock, pd_scene_plasma, pd_scene_storm, pd_scene_mouse,
	pd_scene_donut, pd_scene_tunnel, pd_scene_vectors, pd_scene_sunrise, pd_scene_ending;

/* the work done while earlier scenes play (pd_gfx.h, jobs) */
bool pd_plasma_job(void);
bool pd_city_job(void);
bool pd_donut_job(void);
bool pd_tunnel_job(void);
bool pd_sunrise_job(void);
bool pd_ending_job(void);

/* what each makes, made on the build host (pd_gfx.h) */
void pd_clock_store(pd_store_t *st);
void pd_plasma_store(pd_store_t *st);
void pd_city_store(pd_store_t *st);
void pd_donut_store(pd_store_t *st);
void pd_tunnel_store(pd_store_t *st);
void pd_sunrise_store(pd_store_t *st);
void pd_ending_store(pd_store_t *st);
void pd_synth_store(pd_store_t *st);
bool pd_film_visit(pd_store_t *st, bool ending);

#endif
