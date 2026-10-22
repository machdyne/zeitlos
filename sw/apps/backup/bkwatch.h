#ifndef BKWATCH_H
#define BKWATCH_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Watch (docs/backup.md, "Watch"): the folders and files of
 * apps.backup.watch, watched through fs (zfsproto.h, "Watching") while
 * the backup window -- or `backup watch` in a shell -- is open. A file
 * saved there is backed up on its own, a watch backup, once nothing
 * more has been saved for BKW_QUIET seconds.
 *
 * No daemon, no saved list: what was saved while the drive is out is
 * not backed up by the watch (the next Back up now has it), and nothing
 * survives the window closing. One module for the window and the shell:
 * they call bkw_msg() for every message, bkw_due() in their loop, and
 * run the backup bkw_take() hands them.
 */

#include <stdbool.h>
#include <stdint.h>
#include "../../common/zeitlos.h"
#include "bk.h"

#define BKW_WATCHES     4       // Z_FS_WATCHES: fs's, for everyone
#define BKW_PENDING     32      // saved files remembered; more: their folders
#define BKW_QUIET       10      // seconds with nothing saved before the backup
#define BKW_WRITING     60      // ... unless a file is still open for writing:
                                // then this long after the last change at most
#define BKW_ASK_EVERY   30      // seconds: asked anyway, should a notice be lost

// What the watch is doing, for status lines.
enum {
	BKW_OFF,                // apps.backup.watch is empty
	BKW_NONE,               // set, but nothing it names can be watched (why: bkw_why())
	BKW_WATCHING,           // watching; nothing saved since the last watch backup
	BKW_WAITING,            // saved: backing up after the quiet
	BKW_NO_DRIVE,           // watching, but the drive is not there: saves not backed up
};

typedef struct {
	int         n;                          // watched
	int32_t     wd[BKW_WATCHES];
	char        what[BKW_WATCHES][BK_PATH_MAX];
	bool        set;                        // apps.backup.watch names something
	char        why[160];                   // what could not be watched, and why
	char       *pend[BKW_PENDING];          // saved since the last take
	int         npend;
	char       *taken[BKW_PENDING];         // in the backup running now
	int         ntaken;
	uint32_t    first, last;                // ticks: the first and last change pending
	uint32_t    asked;                      // ticks: the last WATCHREAD
	uint32_t    writers;                    // open for writing under the watches
	uint32_t    seen;                       // changes heard, ever (for tests)
	bool        kicked;                     // a notice came: read
	uint32_t    lost;                       // saves dropped: the drive was not there
	char        last_saved[BK_PATH_MAX];    // the newest saved path heard
} bkw_t;

// Watches what `spec` (apps.backup.watch, as from: paths separated by
// spaces, quoted when they have one) names -- each must be inside one of
// o->from and not on the backup drive. False if nothing is watched;
// bkw_why() says what was not, either way.
bool bkw_start(bkw_t *w, const bk_opts_t *o, const char *spec);
void bkw_stop(bkw_t *w);

// A message: true if it was a watch's notice (it is taken).
bool bkw_msg(bkw_t *w, const z_msg_t *m);

// Reads what changed if a notice came, or every BKW_ASK_EVERY seconds.
// Call it in the loop, as often as it turns; one request at most.
void bkw_poll(bkw_t *w, uint32_t now);

// Is a watch backup due now?
bool bkw_due(const bkw_t *w, uint32_t now);

// The saved paths for that backup, into only[] (at most max). They
// stay valid until bkw_done().
int bkw_take(bkw_t *w, const char **only, int max);
void bkw_done(bkw_t *w);

// Due, but the drive is not there: what was saved is let go.
void bkw_drop(bkw_t *w);

int bkw_state(const bkw_t *w, bool drive_there);
const char *bkw_why(const bkw_t *w);

#endif
