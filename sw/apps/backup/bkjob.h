#ifndef BKJOB_H
#define BKJOB_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A bk_*() call run a slice at a time, for the window (docs/backup.md,
 * "The window"; docs/responsive.md): the engine runs in a coroutine
 * (zco.h) and hands back after every request it makes to fs -- one
 * request a slice, about 22 ms at the most (4 KB to a USB stick) --
 * and in its long loops that make none (bk_io_t.yield). The main loop
 * answers its messages between slices.
 *
 * Stopping is cooperative, not zco_abandon(): the engine holds files,
 * its lock on the drive and memory of its own, so it is told to stop
 * (bk_io_t.stop) and resumed until it has let go of them, which it does
 * at its next check -- the next 4 KB at most. bkjob_cancel() then
 * bkjob_step() until it is done.
 *
 * The coroutine's stack must not be in the heap's way: the engine
 * calls malloc(), and newlib's _sbrk() refuses to grow the heap past
 * the stack pointer (zco.h, "where the stack lives"; zeitlos.c) -- a
 * buffer in .bss lies below the heap and would make every malloc fail.
 * So the caller passes a buffer in main()'s own frame, as posix does.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "bk.h"

#define BKJOB_BACKUP    1   // bk_backup()
#define BKJOB_SUMMARY   2   // bk_summary()
#define BKJOB_LIST      3   // bk_list()
#define BKJOB_FILES     4   // bk_files()
#define BKJOB_HISTORY   5   // bk_history()
#define BKJOB_RESTORE   6   // bk_restore()
#define BKJOB_CHECK     7   // bk_check()
#define BKJOB_PIN       8   // bk_pin()

#define BKJOB_LINES     4   // the last lines the engine said
#define BKJOB_LINE_MAX  120

typedef struct {
	int kind;
	bk_opts_t o;            // the options the call gets (the caller's)
	// arguments, as the call takes them ("" for NULL)
	char backup[BK_NAME_MAX];
	char path[BK_PATH_MAX];
	char out[BK_PATH_MAX];
	bool replace, all, pin;
	// results
	int (*list_fn)(void *arg, const bk_info_t *b);
	int (*file_fn)(void *arg, const bk_file_t *f);
	int (*hist_fn)(void *arg, const char *backup, const bk_stat_t *st);
	void *arg;
	int rc;
	bk_result_t r;
	bk_summary_t sum;
	// what it is doing, for the window
	bk_progress_t prog;
	char path_now[BK_PATH_MAX];
	char lines[BKJOB_LINES][BKJOB_LINE_MAX];
	int nlines;             // lines said so far (lines[] holds the last BKJOB_LINES)
	bool said;              // something new since the caller last looked
	// measured, for the tests
	uint32_t slices;
	uint32_t stack_used;
} bkjob_t;

// The files, clock and owner the jobs use (bk_zfs_io() and the app's
// own); out, progress, stop and yield are the job's. The stack: see
// above.
void bkjob_init(const bk_io_t *base, void *stack, size_t bytes);

// Starts j (one at a time: false if one is running). The job is copied.
bool bkjob_start(const bkjob_t *j);

// Runs one slice. true while it is still running; when it returns
// false, bkjob_result() has the outcome.
bool bkjob_step(void);

bool bkjob_running(void);
// Asks it to stop; it lets go of what it holds within a slice or two.
void bkjob_cancel(void);
bool bkjob_cancelling(void);

// The job running, or the last one finished.
bkjob_t *bkjob_current(void);

// The last line it said ("" none), and clears `said`.
const char *bkjob_last_line(void);

#endif
