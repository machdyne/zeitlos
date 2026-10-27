/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learncheck -- checks learn courses before anyone reads them
 * (docs/learn_format.md, "Checks").
 *
 *   learncheck [--release] [--quiet] PATH...
 *
 * PATH is a course folder (it has a course.cfg) or a folder of courses
 * (the edu pack): every course in it is checked, and links and `needs`
 * between them are resolved. Problems are printed as
 * `file:line: error: ...`, like a compiler's. The exit status is 1 if
 * there was an error, 0 otherwise; warnings do not fail.
 *
 * --release applies the release rules too: every course reviewed, a
 * qualified reviewer for class B, two reviewers for class D.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include <sys/time.h>

#include "lcheck.h"
#include "lfig.h"
#include "../../../sw/apps/learn/tests/hostfs.h"

static bool quiet;

static void report(lc_ctx_t *c, int level, const char *file, int line, const char *msg) {
	(void)c;
	if (quiet && level != LC_ERROR) return;
	static const char *const names[] = { "error", "warning", "note" };
	if (line) printf("%s:%d: %s: %s\n", file, line, names[level], msg);
	else printf("%s: %s: %s\n", file, names[level], msg);
}

// -- figures: run as learn runs them, with every control at its ends --

static uint32_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static char lib[24576], lib_for[1024];
static int lib_len;

static int scm_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// one folder's *.scm, in name order, added to lib
static void lib_dir(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return;
	char *names[16];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 16) {
		size_t l = strlen(e->d_name);
		if (l > 4 && !strcmp(e->d_name + l - 4, ".scm")) names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof names[0], scm_cmp);
	for (int i = 0; i < n; i++) {
		char p[1400];
		snprintf(p, sizeof p, "%s/%s", dir, names[i]);
		FILE *f = fopen(p, "rb");
		free(names[i]);
		if (!f) continue;
		size_t got = fread(lib + lib_len, 1, sizeof lib - (size_t)lib_len - 2, f);
		fclose(f);
		lib_len += (int)got;
		lib[lib_len++] = '\n';
		lib[lib_len] = 0;
	}
}

// as learn: the pack's lib/ (the course's folder's folder), then the course's
static void load_lib(const char *course) {
	if (!strcmp(lib_for, course)) return;
	snprintf(lib_for, sizeof lib_for, "%s", course);
	lib_len = 0;
	lib[0] = 0;
	char dir[1100];
	snprintf(dir, sizeof dir, "%s", course);
	char *slash = strrchr(dir, '/');
	if (slash) { snprintf(slash, sizeof dir - (size_t)(slash - dir), "/lib"); lib_dir(dir); }
	snprintf(dir, sizeof dir, "%s/lib", course);
	lib_dir(dir);
}

static int fig_problem(lc_ctx_t *c, int level, const char *file, int line, const char *what, const char *why) {
	char m[300];
	snprintf(m, sizeof m, "figure%s%s: %s", what[0] ? ", " : "", what, why);
	if (level == LC_WARN) c->warnings++;
	report(c, level, file, line, m);
	return level == LC_ERROR;
}

static int run_figure(lc_ctx_t *c, const char *course, const char *src, const char *file, int line) {
	load_lib(course);
	if (!lg_load(lib_len ? lib : NULL, src)) return fig_problem(c, LC_ERROR, file, line, "", LG.err);
	uint32_t t0 = now_ms();
	if (!lg_frame(0, 3000)) return fig_problem(c, LC_ERROR, file, line, "", LG.frame_err);
	uint32_t took = now_ms() - t0;
	int errors = 0;
	bool outside = LG.outside, cut = LG.cut;
	if (LG.animated) {
		static const double ts[] = { 0.37, 1.0, 2.9, 7.5, 13.1, 61.0 };
		for (int i = 0; i < 6; i++) {
			if (!lg_frame(ts[i], 3000)) {
				char w[40];
				snprintf(w, sizeof w, "at %.2f seconds", ts[i]);
				return errors + fig_problem(c, LC_ERROR, file, line, w, LG.frame_err);
			}
			outside |= LG.outside;
			cut |= LG.cut;
		}
	}
	// every control at each end (and each choice), the others as they start
	int nctl = LG.nctl;
	for (int i = 0; i < nctl; i++) {
		lg_ctl_t keep = LG.ctl[i];
		int tries = keep.kind == LG_CHOICE ? keep.nopt : 2;
		if (keep.kind == LG_DRAG) tries = 3;
		for (int k = 0; k < tries; k++) {
			char w[80];
			if (keep.kind == LG_SLIDER) { lg_ctl_set(i, k ? keep.max : keep.min); snprintf(w, sizeof w, "with %s at %g", keep.name, LG.ctl[i].value); }
			else if (keep.kind == LG_TOGGLE) { lg_ctl_set(i, k); snprintf(w, sizeof w, "with %s %s", keep.name, k ? "on" : "off"); }
			else if (keep.kind == LG_CHOICE) { lg_ctl_set(i, k); snprintf(w, sizeof w, "with %s at \"%s\"", keep.name, keep.opt[k]); }
			else {
				static const double at[3][2] = { { 0, 0 }, { 320, 240 }, { 160, 120 } };
				lg_ctl_drag_to(i, at[k][0], at[k][1]);
				snprintf(w, sizeof w, "with %s dragged to (%g, %g)", keep.name, at[k][0], at[k][1]);
			}
			if (!lg_frame(0, 3000)) { errors += fig_problem(c, LC_ERROR, file, line, w, LG.frame_err); break; }
			outside |= LG.outside && keep.kind != LG_DRAG;
			cut |= LG.cut && keep.kind != LG_DRAG;
		}
		LG.ctl[i] = keep;
	}
	// every point to drag at the same place: shapes that fold to nothing
	bool drags = false;
	for (int i = 0; i < nctl; i++) if (LG.ctl[i].kind == LG_DRAG) { lg_ctl_drag_to(i, 160, 120); drags = true; }
	if (drags && !lg_frame(0, 3000)) errors += fig_problem(c, LC_ERROR, file, line, "with every point in one place", LG.frame_err);
	if (cut) errors += fig_problem(c, LC_ERROR, file, line, "", "words go past its edge and would be cut off (shorter, smaller, or wrapped)");
	else if (outside) fig_problem(c, LC_WARN, file, line, "", "it draws outside its 320 x 240");
	if (took > 60) {
		char w[120];
		snprintf(w, sizeof w, "a frame took %u ms here: on the board it will be slow to draw", took);
		fig_problem(c, LC_WARN, file, line, "", w);
	}
	return errors;
}

static bool is_course(const char *dir) {
	char p[2400];
	struct stat st;
	snprintf(p, sizeof p, "%s/course.cfg", dir);
	return !stat(p, &st);
}

#define MAXC 2048
static char *paths[MAXC];
static char *ids[MAXC];
static int npaths;

static void add(const char *p) {
	if (npaths >= MAXC) return;
	paths[npaths] = strdup(p);
	const char *b = strrchr(p, '/');
	ids[npaths] = strdup(b ? b + 1 : p);
	npaths++;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
	lc_ctx_t c;
	memset(&c, 0, sizeof c);
	c.fs = &host_fs;
	c.report = report;
	c.run_figure = run_figure;
	lg_set_clock(now_ms);
	static unsigned char mem[2 << 20];
	c.mem = mem;
	c.memsize = sizeof mem;
	int given = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--release")) { c.release = true; continue; }
		if (!strcmp(argv[i], "--quiet")) { quiet = true; continue; }
		if (argv[i][0] == '-') {
			fprintf(stderr, "usage: learncheck [--release] [--quiet] COURSE-OR-FOLDER...\n");
			return 2;
		}
		given++;
		char p[1024];
		snprintf(p, sizeof p, "%s", argv[i]);
		size_t l = strlen(p);
		while (l > 1 && p[l - 1] == '/') p[--l] = 0;
		if (is_course(p)) { add(p); continue; }
		DIR *d = opendir(p);
		if (!d) { fprintf(stderr, "learncheck: %s: no such folder\n", p); return 2; }
		struct dirent *e;
		int start = npaths;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char q[2200];
			snprintf(q, sizeof q, "%s/%s", p, e->d_name);
			if (is_course(q)) add(q);		// (a pack's lib/ has no course.cfg)
		}
		closedir(d);
		qsort(paths + start, (size_t)(npaths - start), sizeof paths[0], cmp);
		for (int k = start; k < npaths; k++) {
			const char *b = strrchr(paths[k], '/');
			free(ids[k]);
			ids[k] = strdup(b ? b + 1 : paths[k]);
		}
	}
	if (!given) {
		fprintf(stderr, "usage: learncheck [--release] [--quiet] COURSE-OR-FOLDER...\n");
		return 2;
	}
	c.known_courses = (const char *const *)ids;
	c.nknown = npaths;
	for (int i = 0; i < npaths; i++) lc_check_course(&c, paths[i]);
	printf("learncheck: %d course%s, %d lessons, %d steps, %d checks, %d cards, %d figures, "
		"%d words, about %d minutes: %d error%s, %d warning%s\n",
		npaths, npaths == 1 ? "" : "s", c.lessons, c.steps, c.checks, c.cards, c.figures, c.words,
		c.minutes, c.errors, c.errors == 1 ? "" : "s", c.warnings, c.warnings == 1 ? "" : "s");
	return c.errors ? 1 : 0;
}
