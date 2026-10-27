/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * tools/learn/preview -- every step of a course as learn draws it, one
 * picture a step, for a writer or a reviewer to look at before the
 * course goes near a board (docs/learn_authoring.md).
 *
 *   make -C tools/learn/preview
 *   /tmp/learn_preview sw/data/learn/edu-zeitlos/zeit-tc out/zeit-tc
 *
 * The course is put on a card in its pack (the folder it is in), with
 * the pack's lib/, and opened by learn's own code (lapp, lstep, lfig,
 * ldraw), drawn by the software device: the pictures are the board's,
 * pixel for pixel, but for its colour. A figure is drawn at its start,
 * a moving one at 2.5 seconds. Pictures are NN-MM.pgm (lesson, step);
 * the Makefile makes PNGs of them.
 *
 * Then every question is answered, rightly, through the keys as a
 * learner would (typed answers typed, choices by their number): a
 * question learn will not take its own right answer for is a mistake.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include "lapp_int.h"
#include "ldraw.h"
#include "lfig.h"
#include "hostplat.h"

#define W 640
#define H 480
static uint32_t bits[((W + 31) / 32) * H + 2];
static ld_page_t page;
static uint8_t mem[3u << 20];
static uint8_t scratch[96 << 10], cache[128 << 10];
static const char *fontdir = "sw/data/font";

static const void *load_font(const char *file, uint32_t *len) {
	char p[512];
	const char *sub = strstr(file, ".zhf") ? "zhf/" : strstr(file, "jp12") ? "" : "zfn/";
	snprintf(p, sizeof p, "%s/%s%s", fontdir, sub, file);
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *d = malloc((size_t)n);
	if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return d;
}

static void shot(const char *dir, const char *name) {
	la_draw(&page);
	char p[600];
	snprintf(p, sizeof p, "%s/%s.pgm", dir, name);
	FILE *f = fopen(p, "wb");
	if (!f) { fprintf(stderr, "preview: cannot write %s\n", p); exit(1); }
	fprintf(f, "P5\n%d %d\n255\n", W, H);
	int wpl = (W + 31) / 32;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			fputc((bits[y * wpl + (x >> 5)] >> (x & 31)) & 1 ? 230 : 0, f);
	fclose(f);
}

static void key(uint32_t k);

// each check of the step answered rightly, by keys; false and why if learn does not take it
static int answered;

static bool answer_all(const char *n) {
	bool ok = true;
	for (int i = 0; i < A.nchecks; i++) {
		la_check_t *k = &A.checks[i];
		const lq_q_t *q = &k->q;
		char t[96] = "";
		if (q->kind == LQ_CHOICE) {
			for (int d = 0; d < q->nopt; d++)
				if (q->opt[q->order[d]].right) key((uint32_t)('1' + d));
			if (q->several && !k->answered) key(0x0d);
		} else if (q->kind == LQ_NUMBER || q->kind == LQ_FRACTION || q->kind == LQ_TEXT) {
			if (q->kind == LQ_NUMBER) snprintf(t, sizeof t, "%.10g", q->answer);
			else if (q->kind == LQ_FRACTION) snprintf(t, sizeof t, "%ld/%ld", q->fnum, q->fden);
			else snprintf(t, sizeof t, "%s", q->naccept ? q->accept[0] : "");
			A.typing = i;
			for (const char *c = t; *c; c++) key((uint8_t)*c);
			key(0x0d);
		} else continue;
		if (k->answered && k->last.verdict == LQ_RIGHT) answered++;
		else {
			fprintf(stderr, "preview: %s: check %d (\"%s\") not taken as right: %s%s\n", n, i + 1, t,
				k->last.feedback ? k->last.feedback : "", k->last.said);
			ok = false;
		}
	}
	return ok;
}

static void key(uint32_t k) {
	la_ev_t e;
	memset(&e, 0, sizeof e);
	e.type = LA_KEY;
	e.key = k;
	la_event(&e);
	la_draw(&page);
}

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: preview COURSE-FOLDER OUT-FOLDER [FONT-FOLDER]\n"); return 2; }
	if (argc > 3) fontdir = argv[3];
	char course[4096], pack[4096], cmd[9000];
	if (!realpath(argv[1], course)) { fprintf(stderr, "preview: no %s\n", argv[1]); return 1; }
	snprintf(pack, sizeof pack, "%s", course);
	char *packdir = dirname(pack), *name = strrchr(course, '/') + 1;
	const char *packname = strrchr(packdir, '/') ? strrchr(packdir, '/') + 1 : packdir;
	if (strncmp(packname, "edu", 3)) packname = "edu-preview";
	snprintf(hp_root, sizeof hp_root, "/tmp/learn_preview_card");
	hp_reset_card();
	snprintf(cmd, sizeof cmd, "mkdir -p '%s/opt/ark/%s' '%s' && cp -r '%s'/* '%s/opt/ark/%s/'",
		hp_root, packname, argv[2], packdir, hp_root, packname);
	if (system(cmd)) { fprintf(stderr, "preview: cannot make the card\n"); return 1; }

	LP = &host_plat;
	ld_fonts_load(load_font);
	ld_init(scratch, sizeof scratch, cache, sizeof cache);
	ld_page(&page, bits, W, H);
	la_init(mem, sizeof mem);
	la_resize(W, H);
	while (la_work(50)) hp_ms_now += 50;
	snprintf(hp_prompt_answer, sizeof hp_prompt_answer, "Preview");
	key('n');
	char path[4400];
	snprintf(path, sizeof path, "/opt/ark/%s/%s", packname, name);
	if (!la_open_course(path)) { fprintf(stderr, "preview: learn cannot open %s\n", path); return 1; }
	int shots = 0, mistakes = 0;
	for (int l = 0; l < A.course->nlessons; l++) {
		la_open_lesson(l);
		if (A.review) key(0x1b);
		if (A.screen != SC_LESSON || !A.les) { fprintf(stderr, "preview: lesson %d does not open: %s\n", l + 1, A.status); mistakes++; continue; }
		for (int s = 0; s < A.les->nsteps; s++) {
			A.step = s;
			lv_enter();
			uint32_t t0 = hp_ms_now;
			la_draw(&page);
			if (LG.animated) { hp_ms_now = t0 + 2500; A.fig_need = true; }
			char n[32];
			snprintf(n, sizeof n, "%02d-%02d", l + 1, s + 1);
			shot(argv[2], n);
			if (!answer_all(n)) mistakes++;
			if (A.fig && (LG.err[0] || LG.frame_err[0])) {
				fprintf(stderr, "preview: %s: the figure: %s%s\n", n, LG.err, LG.frame_err);
				mistakes++;
			}
			shots++;
		}
	}
	printf("preview: %s: %d steps drawn in %s, %d questions answered rightly by keys%s\n", name, shots, argv[2],
		answered, mistakes ? ", with mistakes (above)" : "");
	return mistakes != 0;
}
