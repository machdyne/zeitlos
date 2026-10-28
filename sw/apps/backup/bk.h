#ifndef BK_H
#define BK_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup's core (docs/backup.md): dated backups of chosen folders on a
 * second drive, and getting any file back from any of them.
 *
 * Portable C over bk_io_t. The app (Phase 1) binds it to fs and the
 * terminal or window; tests/test_bk.c binds it to two volumes in memory
 * -- a card and a backup drive -- that can lose their power after any
 * write, so the rules below are tested, not only stated.
 *
 * -- the rules that make a power cut harmless --
 *
 *   - The card is only read. Every write goes to the backup drive,
 *     except a restore, which writes NAME.part and renames it.
 *   - On the backup drive nothing that exists is ever changed: a run
 *     only adds files (copies, one INDEX.TXT). Nothing is renamed or
 *     moved there, ever, so FAT's rename window (new entry written
 *     before the old one is removed) cannot leave two entries on one
 *     cluster chain. Making room only unlinks: a cut leaks space at
 *     worst.
 *   - A backup exists only when its INDEX.TXT ends with
 *     "end <files> <crc>". A run cut short has none, is invisible, and
 *     the next run removes what it left.
 *   - Every copy is synced and read back before the index names it.
 *
 * -- on the drive --
 *
 *   <dest>/README.TXT                        what this is (written once)
 *   <dest>/<name>/LOCK.TXT                   while a run is going
 *   <dest>/<name>/<backup>/INDEX.TXT         every file the card had
 *   <dest>/<name>/<backup>/PINNED            pinned (never removed)
 *   <dest>/<name>/<backup>/home/book.txt     copies: what changed
 *   <dest>/<name>/check/<backup>.txt         copies `check` found damaged
 *
 * <backup> is YYYY-MM-DD_HH-MM-SS (local time), +NNNN when the clock
 * was not set (counting from the newest backup), w at the end for a
 * watch backup. See bk_name_next().
 */

#include <stdint.h>
#include <stdbool.h>

// Results of bk_io_t calls and of bk_*().
#define BK_OK        0
#define BK_ERR      -1     // anything else
#define BK_ENOENT   -2     // no such file
#define BK_EBUSY    -3     // open for writing elsewhere (rename)
#define BK_EEXIST   -4     // exists (mkdir: fine; rename: refused)
#define BK_ENOSPC   -5     // the drive is full

// The longest path handled, the NUL included (Z_FS_PATH_MAX).
#define BK_PATH_MAX 256

// A backup's name: "2026-10-21_11-30-48+0001w" and the NUL.
#define BK_NAME_MAX 28

// How much is copied per read/write: one fs request, ~20 ms to USB.
#define BK_CHUNK 4096

typedef struct {
	uint32_t size;
	uint16_t fdate, ftime;  // FAT's packed date and time (UTC); 0 = unknown
	bool dir;
} bk_stat_t;

// Local wall-clock time, for names.
typedef struct {
	int year, month, day, hour, min, sec;
} bk_tm_t;

// What a run is doing, for a progress bar.
#define BK_PH_SCAN     1   // looking at the card
#define BK_PH_ROOM     2   // making room
#define BK_PH_COPY     3   // copying
#define BK_PH_INDEX    4   // writing the index
#define BK_PH_CHECK    5   // reading copies back (check)
#define BK_PH_RESTORE  6

typedef struct {
	int phase;
	const char *path;       // the file now, or NULL
	uint32_t files_done, files_total;
	uint64_t bytes_done, bytes_total;
} bk_progress_t;

typedef struct {
	void *ctx;

	// -- files. Paths are absolute: "/home/book.txt", "/usb/backup/..." --
	int  (*open_read)(void *ctx, const char *path);       // handle >= 0, or BK_E*
	int  (*open_write)(void *ctx, const char *path);      // creates or truncates
	int  (*read)(void *ctx, int h, void *buf, int n);     // bytes; 0 at the end
	int  (*write)(void *ctx, int h, const void *buf, int n); // bytes written
	int  (*seek)(void *ctx, int h, uint32_t off);         // BK_OK
	int  (*sync)(void *ctx, int h);                       // BK_OK: on the medium
	int  (*close)(void *ctx, int h);                      // BK_OK
	int  (*stat)(void *ctx, const char *path, bk_stat_t *st); // BK_OK or BK_ENOENT
	// Calls fn for every entry of a directory (not . and ..); stops when
	// fn returns nonzero, and returns that. BK_ENOENT if path is no
	// directory.
	int  (*list)(void *ctx, const char *path,
		int (*fn)(void *arg, const char *name, const bk_stat_t *st), void *arg);
	int  (*unlink)(void *ctx, const char *path);          // a file or an empty directory
	int  (*mkdir)(void *ctx, const char *path);           // BK_OK or BK_EEXIST
	int  (*rename)(void *ctx, const char *from, const char *to); // restore only
	int  (*free_kb)(void *ctx, const char *path, uint32_t *kb);  // on that volume

	// -- the machine --
	// The local time now; false when the clock is not set. *uptime_s is
	// seconds since power-on either way.
	bool (*clock)(void *ctx, bk_tm_t *t, uint32_t *uptime_s);
	void (*sleep_ms)(void *ctx, int ms);
	// Who runs this ("pid 7 serial 1234"), for LOCK.TXT; and whether the
	// owner a LOCK.TXT names is still running.
	void (*owner)(void *ctx, char *buf, int len);
	bool (*owner_alive)(void *ctx, const char *owner);

	// -- the person --
	void (*out)(void *ctx, const char *text);   // \n ends a line; \r redoes one
	void (*progress)(void *ctx, const bk_progress_t *p);  // may be NULL
	bool (*stop)(void *ctx);                    // true: stop as soon as it is safe
	// May be NULL. Called now and then in loops that do no I/O (reading
	// an index, comparing the card with it), so a caller running this
	// a slice at a time (the window, bkjob.c) can hand back there too.
	void (*yield)(void *ctx);
} bk_io_t;

typedef struct {
	const char *dest;       // apps.backup.to: "/usb/backup"
	const char *name;       // apps.backup.name: "zeitlos"
	const char *const *from; int nfrom;   // apps.backup.from: folders and files
	const char *const *skip; int nskip;   // apps.backup.skip: "/data/ask", "*.tmp"
	int keep;               // apps.backup.keep: the newest backups never thinned (10)
	int refresh;            // a copy older than this many backups is made again (10; 0 never)
	bool verify;            // apps.backup.verify: read every copy back
	bool dry_run;           // -n: say what would be done, write nothing
	const char *manifest;   // "/sys/manifest": files update put there unchanged
	                        // are not backed up; NULL: everything is
	uint32_t reserve_kb;    // always left free on the drive (1024)
	// A watch backup: only these files (absolute) -- or folders, every
	// file in them that changed -- from the window when they were
	// saved. nonly == 0: a full backup.
	const char *const *only; int nonly;
	// A source that is not there is not mentioned (the defaults name
	// /sys/zeitlos.cfg, which a card may not have).
	bool quiet_missing;
} bk_opts_t;

typedef struct {
	char name[BK_NAME_MAX]; // the backup made ("" if none)
	uint32_t files;         // in it (its index)
	uint32_t copied;        // copies made
	uint64_t bytes;         // bytes copied
	uint32_t removed;       // copies removed to make room
	uint64_t removed_bytes;
	uint32_t skipped;       // left out: path too long, unreadable, ...
	uint32_t changed;       // copied, but the file changed meanwhile
	uint32_t shipped;       // left out: installed by update, unchanged
	uint32_t bad;           // check: damaged or missing copies
	uint32_t thinned;       // check: copies removed to make room (expected)
	// backup: the drive's free space after it, when the run had the
	// drive count it anyway (making room); else 0. Asking for it then
	// costs nothing, where a first ask can cost the whole FAT
	// (docs/backup.md, "Running alongside everything").
	uint32_t free_kb;
} bk_result_t;

// Exit statuses, as update's.
#define BK_DONE      0   // done (or -n, or nothing to do)
#define BK_PARTIAL   1   // done, but something was left out or found damaged
#define BK_FAILED    2   // nothing was done: no drive, no room, locked, ...
#define BK_USAGE     3

int bk_backup(const bk_io_t *io, const bk_opts_t *o, bk_result_t *r);

// What a backup holds or was, for list / the window.
typedef struct {
	char name[BK_NAME_MAX];
	bool watch, pinned, clock_set, complete;
	uint32_t files, copied;
	uint64_t bytes;         // copied in it
	char time[24];          // "2026-10-21 11:30:48", or "" (clock not set)
} bk_info_t;

// Every backup on the drive, oldest first (incomplete ones too,
// complete=false). fn returns nonzero to stop.
int bk_list(const bk_io_t *io, const bk_opts_t *o,
	int (*fn)(void *arg, const bk_info_t *b), void *arg);

// The drive at a glance, for a status line: cheap (one small read a
// backup, no index read whole). BK_DONE, or BK_FAILED with no drive.
typedef struct {
	int backups;            // complete ones (by their last line)
	int cut;                // cut short (the next backup removes them)
	char newest[BK_NAME_MAX], oldest[BK_NAME_MAX];  // complete full ones; "" none
	uint32_t newest_files;
	char newest_watch[BK_NAME_MAX];     // the newest complete watch backup; "" none
	uint32_t free_kb;       // 0 if not known
} bk_summary_t;

int bk_summary(const bk_io_t *io, const bk_opts_t *o, bk_summary_t *s);

// A glance at the drive, for the window: the newest complete full
// backup (newest, newest_files), and the newest complete watch backup
// newer than it (newest_watch; or the newest of all when there is no
// full one). backups counts the backup folders, complete or not; cut,
// oldest and free_kb are not read. Lists the folder once and reads
// the last line of an index or two, however many backups there are,
// and never asks for the free space (a first ask can make fs count
// the whole FAT). Checks io->stop between requests: BK_FAILED when
// stopped, as with no drive.
int bk_glance(const bk_io_t *io, const bk_opts_t *o, bk_summary_t *s);

// One file as a backup has it.
typedef struct {
	const char *path;       // "/home/book.txt"
	uint32_t crc, size;
	uint16_t fdate, ftime;
	const char *where;      // the backup holding its copy
	bool present;           // the copy is there (else removed to make room)
} bk_file_t;

// The files a backup had (NULL: the newest) under dir ("/" all), sorted.
int bk_files(const bk_io_t *io, const bk_opts_t *o, const char *backup,
	const char *dir, int (*fn)(void *arg, const bk_file_t *f), void *arg);

// Every copy of one file, oldest first: the backup and its size.
int bk_history(const bk_io_t *io, const bk_opts_t *o, const char *path,
	int (*fn)(void *arg, const char *backup, const bk_stat_t *st), void *arg);

// Gets a file or a folder back as backup had it (NULL: the newest).
// out: where to (a file, or a folder for a folder); NULL: where it was.
// A file that is there and different is not replaced unless replace:
// the copy goes beside it, "book (2026-10-21 11-30).txt".
int bk_restore(const bk_io_t *io, const bk_opts_t *o, const char *backup,
	const char *path, const char *out, bool replace, bk_result_t *r);

// Every index complete and consistent; all: every copy read and its CRC
// checked. Damaged copies are listed in check/<backup>.txt, and the next
// backup copies those files again from the card when they are unchanged.
int bk_check(const bk_io_t *io, const bk_opts_t *o, bool all, bk_result_t *r);

int bk_pin(const bk_io_t *io, const bk_opts_t *o, const char *backup, bool pin);

// Frees kb (beyond reserve_kb) by the rules of docs/backup.md,
// "Making room". BK_DONE if it could.
int bk_make_room(const bk_io_t *io, const bk_opts_t *o, uint32_t kb, bk_result_t *r);

// -- exposed for the tests and the app --

uint32_t bk_crc32(uint32_t crc, const void *p, uint32_t n);

// Backup names. bk_name_parse: true if s is one; base gets the
// YYYY-MM-DD_HH-MM-SS part, *n the +NNNN (0 none), *watch the w.
bool bk_name_parse(const char *s, char base[20], int *n, bool *watch);
// Orders names as the backups were made: by base, then by +N.
int bk_name_cmp(const char *a, const char *b);
// The name for a new backup, given the newest on the drive ("" none):
// the clock's time if set and later than newest's base, else newest's
// base and +N one more. Returns 1 when the clock's time equals newest's
// base to the second (the caller waits a second and asks again), else 0.
int bk_name_next(const char *newest, const bk_tm_t *now, bool clock_set,
	bool watch, char out[BK_NAME_MAX]);

// glob of '*' and '?' (ASCII case-insensitive, as FAT)
bool bk_glob(const char *pat, const char *name);

#endif
