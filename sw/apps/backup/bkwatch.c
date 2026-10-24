/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Watch: see bkwatch.h and docs/backup.md, "Watch".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"          /* Z_TICK_HZ */
#include "../../common/zfsc.h"
#include "../../common/zargs.h"
#include "bkwatch.h"
#include "../../common/zpaths.h"

// Is `inner` the folder `outer`, or inside it? Case-insensitive, as FAT.
static bool within(const char *outer, const char *inner) {
	size_t n = strlen(outer);
	while (n > 1 && outer[n - 1] == '/') n--;
	if (n == 1 && outer[0] == '/') return inner[0] == '/';
	for (size_t i = 0; i < n; i++) {
		char a = outer[i], b = inner[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b) return false;
	}
	return inner[n] == 0 || inner[n] == '/';
}

static void why_add(bkw_t *w, const char *path, const char *reason) {
	size_t k = strlen(w->why);
	snprintf(w->why + k, sizeof w->why - k, "%s%s %s", k ? "; " : "", path, reason);
}

bool bkw_start(bkw_t *w, const bk_opts_t *o, const char *spec) {
	char buf[2 * 256 + 2], *v[BKW_WATCHES + 4];
	memset(w, 0, sizeof *w);
	if (!spec || !spec[0] || !strcmp(spec, "none") || !strcmp(spec, "-")) return false;
	w->set = true;
	int n = z_args_split(spec, buf, sizeof buf, v, NULL, BKW_WATCHES + 4);
	for (int i = 0; i < n; i++) {
		char p[BK_PATH_MAX];
		snprintf(p, sizeof p, "%s", v[i]);
		size_t l = strlen(p);
		while (l > 1 && p[l - 1] == '/') p[--l] = 0;
		bool in_from = false;
		for (int k = 0; k < o->nfrom && !in_from; k++) in_from = o->from[k] && within(o->from[k], p);
		if (p[0] != '/') { why_add(w, p, "is not a path (" Z_DIR_HOME "/book)"); continue; }
		if (o->dest && (within(o->dest, p) || within(p, o->dest))) { why_add(w, p, "is the backup drive's"); continue; }
		if (!in_from) { why_add(w, p, "is not in From, so not backed up"); continue; }
		if (w->n == BKW_WATCHES) { why_add(w, p, "is one too many (4 at most)"); continue; }
		int32_t r = zfs_watch(p);
		if (r < 0) {
			why_add(w, p, r == -Z_EMFILE ? "cannot be watched: fs's 4 watches are in use" :
				"cannot be watched");
			continue;
		}
		w->wd[w->n] = r;
		snprintf(w->what[w->n], BK_PATH_MAX, "%s", p);
		w->n++;
	}
	return w->n > 0;
}

static void free_list(char **l, int *n) {
	for (int i = 0; i < *n; i++) free(l[i]);
	*n = 0;
}

void bkw_stop(bkw_t *w) {
	for (int i = 0; i < w->n; i++) zfs_unwatch(w->wd[i]);
	w->n = 0;
	free_list(w->pend, &w->npend);
	free_list(w->taken, &w->ntaken);
}

bool bkw_msg(bkw_t *w, const z_msg_t *m) {
	if (m->subject != Z_FS_CHANGED) return false;
	for (int i = 0; i < w->n; i++)
		if ((int32_t)m->tag == w->wd[i]) { w->kicked = true; return true; }
	return true;        // a watch of ours that has ended: still nobody else's
}

static void add(bkw_t *w, const char *p, bool folder) {
	for (int i = 0; i < w->npend; i++)
		if (within(w->pend[i], p)) return;      // the same, or in a folder already there
	if (folder) {       // a folder replaces what is inside it
		int k = 0;
		for (int i = 0; i < w->npend; i++) {
			if (within(p, w->pend[i])) free(w->pend[i]);
			else w->pend[k++] = w->pend[i];
		}
		w->npend = k;
	}
	if (w->npend == BKW_PENDING) {
		// too many to name: the watched folders, whole (the backup
		// copies only what changed in them)
		free_list(w->pend, &w->npend);
		for (int i = 0; i < w->n; i++) add(w, w->what[i], true);
		return;
	}
	size_t l = strlen(p) + 1;
	char *s = malloc(l);
	if (!s) return;
	memcpy(s, p, l);
	w->pend[w->npend++] = s;
}

void bkw_poll(bkw_t *w, uint32_t now) {
	if (!w->n) return;
	if (!w->kicked && now - w->asked < BKW_ASK_EVERY * Z_TICK_HZ) return;
	bool notice = w->kicked;
	w->kicked = false;
	w->asked = now;
	char paths[Z_FS_WATCH_PATHS * Z_FS_PATH_MAX];
	uint32_t writers = 0;
	int had = w->npend;
	bool changed = false;
	uint32_t lost = 0;      // watches fs no longer has
	for (int i = 0; i < w->n; i++) {
		z_fs_watch_t h;
		int32_t k = zfs_watch_read(w->wd[i], &h, paths, sizeof paths);
		if (k == -Z_EBADF) { lost |= 1u << i; continue; }
		if (k < 0) continue;
		writers += h.writers;
		const char *p = paths;
		for (int32_t j = 0; j < k; j++, p += strlen(p) + 1) {
			add(w, p, false);
			snprintf(w->last_saved, sizeof w->last_saved, "%.*s", (int)sizeof w->last_saved - 1, p);
			w->seen++;
			changed = true;
		}
		if (h.flags & Z_FS_WATCH_MORE) { add(w, w->what[i], true); changed = true; w->seen++; }
	}
	// fs no longer has these: fs restarted (its watches are in its
	// memory), or swept them. Watched again only now, after every read:
	// a number fs gives out again here could be one still to be read
	// above, and two watches would become one. What was saved in
	// between is not known, so the folder is backed up (which copies
	// only what changed).
	for (int i = 0; i < w->n; i++) {
		if (!(lost >> i & 1u)) continue;
		int32_t r = zfs_watch(w->what[i]);
		if (r < 0) continue;
		w->wd[i] = r;
		add(w, w->what[i], true);
		changed = true;
	}
	// a file closed after writing is a change too: it came with a notice
	if (writers != w->writers) changed = true;
	w->writers = writers;
	if (changed || (notice && w->npend)) {
		if (!had && w->npend) w->first = now;
		w->last = now;
	}
}

bool bkw_due(const bkw_t *w, uint32_t now) {
	if (!w->npend || w->ntaken) return false;
	uint32_t quiet = now - w->last;
	if (quiet < BKW_QUIET * Z_TICK_HZ) return false;
	return !w->writers || quiet >= BKW_WRITING * Z_TICK_HZ;
}

int bkw_take(bkw_t *w, const char **only, int max) {
	free_list(w->taken, &w->ntaken);
	int n = 0;
	for (int i = 0; i < w->npend; i++) {
		if (n < max) { w->taken[n] = w->pend[i]; only[n] = w->pend[i]; n++; }
		else free(w->pend[i]);
	}
	w->ntaken = n;
	w->npend = 0;
	return n;
}

void bkw_done(bkw_t *w) { free_list(w->taken, &w->ntaken); }

void bkw_drop(bkw_t *w) {
	if (w->npend) w->lost++;
	free_list(w->pend, &w->npend);
}

int bkw_state(const bkw_t *w, bool drive_there) {
	if (!w->set) return BKW_OFF;
	if (!w->n) return BKW_NONE;
	if (!drive_there) return BKW_NO_DRIVE;
	if (w->npend || w->ntaken) return BKW_WAITING;
	return BKW_WATCHING;
}

const char *bkw_why(const bkw_t *w) { return w->why; }
