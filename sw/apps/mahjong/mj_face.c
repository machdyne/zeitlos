/*
 * Zeitlos mahjong -- putting the canvas on a face. See mj_face.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include "mj_face.h"

mj_canvas_t mj_cv;
uint32_t    mj_cvmem[MJ_CV_WORDS] __attribute__((aligned(8)));
bool        mj_face_ready;

int mj_face_planes(void)
{
	int n;
	if (!ar_color()) return 0;
	n = ar_colours();
	return n >= 64 ? 6 : n >= 16 ? 4 : 3;
}

void mj_face_layout(mj_play_t *p, bool screen, int w, int h)
{
	int all = (1 << MJ_B_NEW) | (1 << MJ_B_UNDO) | (1 << MJ_B_HINT) | (1 << MJ_B_SHUFFLE);
	if (screen) {
		p->can_zoom = true;
		p->v.buttons = all | (1 << MJ_B_ZOOM);
		p->v.size = p->zoom ? MJ_ART_LARGE : MJ_ART_SMALL;
		mj_view_layout(&p->v, AR_W, AR_H);
	} else {
		p->can_zoom = false;
		p->v.buttons = all;
		p->v.size = mj_view_fits(&p->g, MJ_ART_LARGE, w, h) ? MJ_ART_LARGE : MJ_ART_SMALL;
		mj_view_layout(&p->v, w, h);
	}
	if (p->v.cursor >= 0) mj_view_reveal(&p->v, p->v.cursor);
}

void mj_face_screen(mj_play_t *p)
{
	int planes = mj_face_planes();
	mj_cv_setup(&mj_cv, mj_cvmem, MJ_CV_WORDS, AR_W, AR_H, planes, mj_inks, mj_ink_map);
	p->v.rich = planes >= 6;
	mj_face_layout(p, true, AR_W, AR_H);
	mj_render_all(&p->v, &mj_cv);
	mj_face_ready = true;
	mj_face_composes++;
	ar_dirty_all();
}

int mj_face_composes;
static int composed = -1;   /* the planes the canvas holds; -1 none yet */
static int wait_colour;

void mj_face_begin(bool full)
{
	composed = -1;
	wait_colour = full ? 0 : MJ_FACE_WAIT_COLOUR;
	mj_face_ready = false;
}

void mj_face_end(void)
{
	composed = -1;
	mj_face_ready = false;
}

bool mj_face_check(mj_play_t *p)
{
	int want = mj_face_planes();
	if (want == composed) return false;
	if (wait_colour > 0 && want == 0 && !ar_fullscreen() && ar_color_available()) {
		wait_colour--;
		return false;
	}
	wait_colour = 0;
	mj_face_screen(p);
	composed = mj_cv.planes;
	return true;
}

void mj_face_paint(void *user, const ar_rect_t *r)
{
	int k;
	(void)user;
	if (!mj_face_ready) {
		ar_fill(r->x, r->y, r->w, r->h, MJ_C_BG);
		return;
	}
	if (mj_cv.planes) {
		for (k = 0; k < mj_cv.planes; k++)
			ar_plane(k, mj_cv.plane[k], mj_cv.stride * 4, r->x, r->y,
				 r->x, r->y, r->w, r->h);
	} else {
		ar_bitmap_opaque(mj_cv.plane[0], mj_cv.stride * 4, r->x, r->y,
				 r->x, r->y, r->w, r->h);
	}
}

void mj_face_pulse(const mj_play_t *p, uint32_t frame)
{
	int t = (int)(frame % 64);
	t = t < 32 ? t * 8 : (64 - t) * 8;
	if (p->v.hint_a >= 0) ar_pal(MJ_C_HINT, ar_rgb_mix(0xffe, 0x5cf, t));
	else if (ar_pal_get(MJ_C_HINT) != mj_palette[MJ_C_HINT])
		ar_pal(MJ_C_HINT, mj_palette[MJ_C_HINT]);
}

void mj_face_flush(mj_play_t *p, bool screen, mj_show_fn show)
{
	int i;

	if (p->relayout) {
		mj_face_layout(p, screen, mj_cv.w, mj_cv.h);
		p->all = true;
	}
	/* a scroll and nothing else: move the picture, draw the new strip */
	if (!p->all && (p->sdx || p->sdy)) {
		if (mj_render_scrolled(&p->v, &mj_cv, p->sdx, p->sdy))
			show(p->v.area_x, p->v.area_y, p->v.area_w, p->v.area_h);
		else
			p->all = true;
	}
	p->sdx = p->sdy = 0;
	if (p->all) {
		mj_render_all(&p->v, &mj_cv);
		show(0, 0, mj_cv.w, mj_cv.h);
	} else {
		for (i = 0; i < p->nrect; i++) {
			mj_rect_t *r = &p->rect[i];
			mj_render(&p->v, &mj_cv, r->x, r->y, r->w, r->h);
			show(r->x, r->y, r->w, r->h);
		}
		if (p->bar) {
			mj_render_bar(&p->v, &mj_cv);
			show(0, p->v.bar_y, mj_cv.w, MJ_BAR_H);
		}
	}
	p->nrect = 0;
	p->all = p->relayout = p->bar = false;
}
