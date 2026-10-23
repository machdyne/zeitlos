/*
 * planedemo -- a sixty-second film about Zeitlos, in 256 colours, with
 * music: a demo in the demoscene's sense. See docs/planedemo.md.
 *
 *   > run planedemo
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Made for an 8-plane board (Sergei ML2): the arcade runtime full
 * screen in 256 colours, the hardware mixer for the music. It plays on
 * fewer planes too, with fewer colours. Esc leaves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"

#ifndef PD_HOST
#include "../../common/zsoc.h"         /* z_game_in_vblank() */
#endif

/* ===================================================================
 * helpers
 * =================================================================== */

/* The palette as the scenes write it, logical (pd_gfx.h, "fewer
 * planes"): straight through on 8 planes; else kept, and shown at the
 * end of the frame through pd_logical(). */
static uint16_t stage[256];

void pd_pal(int i, uint16_t rgb) {
	stage[i & 255] = rgb;
	if (pd_planes >= 8) ar_pal(i, rgb);
}

static void pal_flush(void) {
	unsigned j;
	if (pd_planes >= 8) return;
	for (j = 0; j < (1u << pd_planes); j++) ar_pal((int)j, stage[pd_logical(j)]);
}

uint16_t pd_rgb(int r, int g, int b) {
	if (r < 0) r = 0;
	if (r > 15) r = 15;
	if (g < 0) g = 0;
	if (g > 15) g = 15;
	if (b < 0) b = 0;
	if (b > 15) b = 15;
	return (uint16_t)(r << 8 | g << 4 | b);
}


void pd_clear(int c) { ar_fill(0, 0, PD_W, PD_H, c); }

void pd_text_c(int x, int y, const char *s, int c, int scale) {
	int w = ar_text_width(s, &z_font_8x8, scale);
	ar_text(x - w / 2, y, s, c, &z_font_8x8, scale);
}

int pd_beat_phase(uint32_t t) {
	/* a beat is 4 rows: 225/8 frames */
	uint32_t f = t * 8u % 225u;
	return (int)(f * 256u / 225u);
}

static uint32_t rnd = 0x13579bdu;
uint32_t pd_rand(void) {
	rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
	return rnd;
}

/* ===================================================================
 * the film
 * =================================================================== */

static void paint(void *user, const ar_rect_t *r) {
	/* Everything is drawn as the film runs; a repaint the runtime asks
	 * for (a mode change) starts the scene's picture again. */
	(void)user; (void)r;
}

static ar_desc_t desc = {
	"planedemo", "planedemo", "A sixty-second film about Zeitlos.\n\nEsc leaves.",
	"planedemo",
	{ 0x000, 0xfff, 0x888, 0x444, 0xf00, 0x0f0, 0x00f, 0xff0,
	  0x0ff, 0xf0f, 0x800, 0x080, 0x008, 0x880, 0x088, 0x808 },
	{ { { 0 } } },
	paint, NULL,
	false,
	NULL,
	false
};

int pd_late;

#ifdef PD_MEASURE
/* Counting instructions on an emulator (tests/rv): a call it watches. */
void pd_mark(int what) __attribute__((noinline));
void pd_mark(int what) { __asm__ volatile("" : : "r"(what) : "memory"); }
#define MARK(k) pd_mark(k)
#else
#define MARK(k) ((void)0)
#endif

/* The frame budget, a line a scene on the console (the UART): frames
 * drawn and dropped, what begin() took, the slowest frame's drawing,
 * and whether the jobs behind it were done. Times in ms of the 732 Hz
 * uptime counter (1.4 ms steps); on the host, in calls. */
/* The CPU's own counters (picorv32's rdcycle, rdinstret: zprof.h):
 * what a scene's frames cost in cycles, and how many instructions --
 * a frame at 60 a second is 800,000 cycles of the 48 MHz CPU. They
 * count the whole machine, so a frame the scheduler gave to someone
 * else for a moment looks dearer (the slowest frame most of all). */
static inline uint32_t cyc(void) {
#ifdef PD_HOST
	return 0;
#else
	register uint32_t v __asm__("a0");
	__asm__ volatile (".word 0xC0002573" : "=r"(v));
	return v;
#endif
}
static inline uint32_t ins(void) {
#ifdef PD_HOST
	return 0;
#else
	register uint32_t v __asm__("a0");
	__asm__ volatile (".word 0xC0202573" : "=r"(v));
	return v;
#endif
}

typedef struct {
	uint32_t shown, dropped, begin, worst;
	uint64_t frame_cyc, frame_ins, jobs_cyc;
	bool jobs_done;
	uint64_t draw_cyc;              /* the queue run at the boundary */
	uint32_t draw_worst, late, behind;
} stat_t;

/* -- racing the beam (docs/planedemo.md) --
 *
 * A frame's drawing is queued while the scene works it out (pd_queue())
 * and run by draw_frame(), which the runtime calls straight after the
 * frame boundary and the palette (ar_frame_hook()), a band of rows at a
 * time from the top. The beam: 45 blank lines from the boundary, then
 * a picture row every two lines, a line 1,525 cycles of the 48 MHz CPU
 * (800 pixels of 25.175 MHz) -- about 69,000 cycles of blanking, then
 * 3,050 a row. A band finished after the beam reached its first row
 * may have shown half drawn: counted ("behind"), as is a frame whose
 * drawing began outside the blanking ("late": the frame was dropped,
 * the beam is anywhere). */
#define PD_BANDS 8
#define LINE_CYC 1525u
static stat_t *cur_st;

static void draw_frame(void) {
	uint32_t c0 = cyc(), d;
	int b, behind = 0;
#ifndef PD_HOST
	if (!z_game_in_vblank() && cur_st) cur_st->late++;
#endif
	pd_queue_prepare(PD_BANDS);
	for (b = 0; b < pd_queue_bands(); b++) {
		int r0;
		/* (from the hook's start: the palette before it makes this a
		 * little kind) */
		pd_queue_band(b, &r0);
		if (cyc() - c0 > (45u + 2u * (uint32_t)r0) * LINE_CYC) behind = 1;
	}
	pd_queue_done();
	d = cyc() - c0;
	if (cur_st) {
		cur_st->draw_cyc += d;
		if (d > cur_st->draw_worst) cur_st->draw_worst = d;
		cur_st->behind += (uint32_t)behind;
	}
}

static void report(const char *name, const stat_t *st) {
	uint32_t n = st->shown ? st->shown : 1;
	uint32_t avg = (uint32_t)(st->frame_cyc / n), ipc100 = st->frame_cyc ? (uint32_t)(st->frame_ins * 100 / st->frame_cyc) : 0;
	printf("planedemo: %s: %lu frames, %lu dropped; frame %lu kcyc (slowest %lu), ipc 0.%02lu; "
		"drawn %lu kcyc (slowest %lu), %lu late, %lu behind the beam; begin %lu kcyc; jobs %lu Mcyc%s\n",
		name, (unsigned long)st->shown, (unsigned long)st->dropped,
		(unsigned long)(avg / 1000), (unsigned long)(st->worst / 1000), (unsigned long)ipc100,
		(unsigned long)(st->draw_cyc / n / 1000), (unsigned long)(st->draw_worst / 1000),
		(unsigned long)st->late, (unsigned long)st->behind,
		(unsigned long)(st->begin / 1000), (unsigned long)(st->jobs_cyc / 1000000),
		st->jobs_done ? "" : ", not done");
}

static int scene_at(uint32_t t) {
	int i, s = 0;
	for (i = 0; i < pd_nscenes; i++)
		if (t >= pd_frame_of_bar(pd_scenes[i]->bar)) s = i;
	return s;
}

int main(void) {

	uint32_t t = 0;
	int cur = -1;
	bool quit = false;
	/* the frame budget, a line a scene on the console: how many frames
	 * it drew, how many it dropped, whether the jobs were done */
	uint32_t t0, c0, i0;
	stat_t st;
	bool jobs_done = false;
	memset(&st, 0, sizeof(st));

	pd_trig_init();

	if (!ar_init(&desc)) return 1;
	ar_text_input(true);                  /* C and M are not ours to give away */
	{
		int n = ar_set_colours(256);
		pd_planes = n >= 256 ? 8 : n >= 64 ? 6 : 4;
		pd_keep(0xff, 0xff);
		if (n < 256) printf("planedemo: made for 256 colours; this board shows %d\n", n);
	}

	/* everything the scenes show, made on the build host and linked in
	 * (pd_blob.c); a build without it makes it all here, slowly, while
	 * the first scenes play */
	{
		pd_store_t st;
		bool ok = false;
		memset(&st, 0, sizeof(st));
		if (pd_blob) {
			st.in = pd_blob;
			st.in_len = pd_blob_len;
			ok = pd_film_visit(&st, pd_planes >= 8);
			if (!ok) printf("planedemo: the film's data does not fit this program: making it here\n");
		} else printf("planedemo: no film data in this build: making it here (minutes on the board)\n");
		t0 = pd_ticks();
		if (!pdm_init()) printf("planedemo: no 16-bit mixer here -- the film plays silently\n");
		if (!ok) {
			printf("planedemo: instruments made in %lu ms\n", (unsigned long)((pd_ticks() - t0) * 1000 / 732));
			pd_job_add(pd_plasma_job);
			pd_job_add(pd_city_job);
			pd_job_add(pd_donut_job);
			pd_job_add(pd_tunnel_job);
			pd_job_add(pd_sunrise_job);
		}
		if (!ok || pd_planes < 8) pd_job_add(pd_ending_job);
	}
	if (!ar_set_fullscreen(true))
		printf("planedemo: no game mode here -- playing in the window, in monochrome\n");

	/* every frame's drawing at the frame boundary, ahead of the beam */
	cur_st = &st;
#ifdef PD_HOST
	/* PD_HOST_DIRECT=1: drawn as asked, not queued -- the same frames
	 * (tests/Makefile, "check") */
	if (!getenv("PD_HOST_DIRECT"))
#endif
	pd_queue(true);
	ar_vsync_exact(true);
	ar_frame_hook(draw_frame);

	while (!quit && t < PD_END_FRAME) {
		ar_event_t e;
		int s;
		while (ar_poll(&e)) {
			if (e.type == AR_EV_QUIT) quit = true;
			/* back in the window (Esc, F11): the film is over; full
			 * screen again: the picture is gone, start the scene again */
			if (e.type == AR_EV_MODE) {
				if (!ar_fullscreen()) quit = true;
				cur = -1;
			}
			/* after the help box (F1, on the screen): the picture under
			 * it is gone, start the scene again */
			if (e.type == AR_EV_SUSPEND) cur = -1;
		}
		if (quit) break;

		pdm_frame(t);
		s = scene_at(t);
		if (s != cur) {
			if (cur >= 0) { st.jobs_done = jobs_done; report(pd_scenes[cur]->name, &st); }
			memset(&st, 0, sizeof(st));
			cur = s;
			c0 = cyc();
			MARK(5);
			if (pd_scenes[s]->begin) pd_scenes[s]->begin();
			MARK(6);
			st.begin = cyc() - c0;
		}
		t0 = pd_ticks();
		c0 = cyc();
		i0 = ins();
		MARK(1);
		pd_scenes[s]->frame(t, t - pd_frame_of_bar(pd_scenes[s]->bar));
		MARK(2);
		{
			uint32_t fc = cyc() - c0;
			st.frame_cyc += fc;
			st.frame_ins += ins() - i0;
			if (fc > st.worst) st.worst = fc;
		}
		{
			/* the rest of the frame for the work ahead: what the
			 * scene left of it, less a margin (the counter's steps are
			 * coarse), and at least a little */
			uint32_t used = pd_ticks() - t0, left;
			c0 = cyc();
			left = used + 3 < PD_TICKS_PER_FRAME ? PD_TICKS_PER_FRAME - 3 - used : 1;
#ifdef PD_HOST
			/* PD_HOST_JOBS_EVERY=n: the jobs only every nth frame, a
			 * board slower than the host (tests/Makefile, "slow") */
			{
				static int every = -1;
				if (every < 0) every = getenv("PD_HOST_JOBS_EVERY") ? atoi(getenv("PD_HOST_JOBS_EVERY")) : 1;
				if (every > 1 && t % (uint32_t)every) left = 0;
			}
			if (left)
#endif
			jobs_done = pd_jobs_run(left);
			st.jobs_cyc += cyc() - c0;
		}
		MARK(3);
		pal_flush();
		{
			uint32_t n = ar_frame();
			MARK(4);
			st.shown++;
			if (n > 1) st.dropped += n - 1;
			t += n;
			/* a scene can do less while frames are being dropped */
			if (n > 1) pd_late = 60;
			else if (pd_late) pd_late--;
		}
	}
	if (cur >= 0) { st.jobs_done = true; report(pd_scenes[cur]->name, &st); }

	ar_frame_hook(NULL);
	pd_queue(false);
	pdm_shutdown();
	ar_shutdown();
	return 0;
}
