/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- the catalogue. lcat.h.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lcat.h"
#include "lplat.h"
#include "zpaths.h"

lk_t LK;

// The packs: every folder of /opt/ark whose name starts as Z_DIR_ARK_EDU's
// does (edu, edu-k8, edu-zeitlos, ...), in name order; then a stick's.
#define MAX_ROOTS 16
static char roots[MAX_ROOTS][LF_PATH];
static int nroots;

static void pack(void *arg, const char *name, bool is_dir) {
	(void)arg;
	const char *prefix = Z_DIR_ARK_EDU + sizeof Z_DIR_ARK;		// "edu"
	if (!is_dir || strncmp(name, prefix, strlen(prefix)) || nroots >= MAX_ROOTS - 1) return;
	snprintf(roots[nroots++], LF_PATH, "%s/%s", Z_DIR_ARK, name);
}

static int cmp_root(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void found(void *arg, const char *name, bool is_dir) {
	const char *root = (const char *)arg;
	// a pack's lib/ is its figures' Scheme, not a course
	if (!is_dir || name[0] == '.' || !strcmp(name, "lib") || LK.npending >= LK_MAX) return;
	snprintf(LK.pending[LK.npending++], LF_PATH, "%s/%s", root, name);
}

void lk_begin(void *(*alloc)(unsigned long)) {
	LK.n = LK.npending = LK.next = 0;
	LK.alloc = alloc;
	nroots = 0;
	LP->fs.list(LP->fs.ctx, Z_DIR_ARK, pack, NULL);
	qsort(roots, (size_t)nroots, sizeof roots[0], cmp_root);
	snprintf(roots[nroots++], LF_PATH, "%s", Z_DIR_USB_EDU);
	for (int i = 0; i < nroots; i++)
		LP->fs.list(LP->fs.ctx, roots[i], found, roots[i]);
	LK.scanning = true;
}

int lk_level_rank(const char *l) {
	if (!strcmp(l, "K")) return 0;
	if (l[0] >= '1' && l[0] <= '9') return atoi(l);
	if (!strcmp(l, "ms")) return 7;
	if (!strcmp(l, "hs")) return 10;
	if (!strcmp(l, "col")) return 13;
	return 20;
}

int lk_area_of(const lf_info_t *m) {
	if (!strcmp(m->area, "school")) return LK_SCHOOL;
	if (!strcmp(m->area, "college")) return LK_COLLEGE;
	if (!strcmp(m->area, "life")) return LK_LIFE;
	if (!strcmp(m->area, "lang")) return LK_LANG;
	if (!strcmp(m->area, "zeit")) return LK_ZEIT;
	return LK_LIFE;
}

const char *lk_area_name(int a) {
	static const char *const n[] = { "All courses", "School K-12", "College", "Life", "Languages", "Using Zeitlos" };
	return a >= 0 && a < LK_AREAS ? n[a] : "";
}

void lk_level_name(const lf_info_t *m, char *out, int cap) {
	const char *l = m->level;
	if (!strcmp(l, "K")) snprintf(out, (size_t)cap, "Kindergarten");
	else if (l[0] >= '1' && l[0] <= '9') snprintf(out, (size_t)cap, "Grade %s", l);
	else if (!strcmp(l, "ms")) snprintf(out, (size_t)cap, "Middle school");
	else if (!strcmp(l, "hs")) snprintf(out, (size_t)cap, "High school");
	else if (!strcmp(l, "col")) snprintf(out, (size_t)cap, "College");
	else if (!strcmp(l, "lang")) snprintf(out, (size_t)cap, "Languages");
	else if (!strcmp(l, "zeit")) snprintf(out, (size_t)cap, "Zeitlos");
	else snprintf(out, (size_t)cap, "Life");
}

static int cmp(const void *a, const void *b) {
	const lf_info_t *x = *(lf_info_t *const *)a, *y = *(lf_info_t *const *)b;
	int ax = lk_area_of(x), ay = lk_area_of(y);
	if (ax != ay) return ax - ay;
	int lx = lk_level_rank(x->level), ly = lk_level_rank(y->level);
	if (lx != ly) return lx - ly;
	return strcmp(x->title, y->title);
}

bool lk_step(void) {
	if (!LK.scanning) return false;
	if (LK.next >= LK.npending) {
		qsort(LK.c, (size_t)LK.n, sizeof LK.c[0], cmp);
		LK.scanning = false;
		return false;
	}
	const char *path = LK.pending[LK.next++];
	lf_info_t *m = (lf_info_t *)LK.alloc(sizeof(lf_info_t));
	if (!m) { LK.next = LK.npending; return true; }
	char err[96];
	if (lf_info_load(&LP->fs, path, m, err, sizeof err) && LK.n < LK_MAX) {
		// the same course on the stick and in the pack: the first found wins
		bool dup = false;
		for (int i = 0; i < LK.n; i++) if (!strcmp(LK.c[i]->id, m->id)) dup = true;
		if (!dup) LK.c[LK.n++] = m;
	}
	return true;
}

const lf_info_t *lk_find(const char *id) {
	for (int i = 0; i < LK.n; i++) if (!strcmp(LK.c[i]->id, id)) return LK.c[i];
	return NULL;
}
