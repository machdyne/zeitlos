#ifndef MEMFS_H
#define MEMFS_H

/*
 * A card and a backup drive in memory, for backup's host tests
 * (test_bk.c): bk_io_t over two volumes, "/" (the card) and "/usb"
 * (the drive), case-insensitive as FAT is, with a fixed size each.
 *
 * It can lose its power. After memfs_cut_after(n), the n-th write to
 * the drive (open for writing, write, sync, close, unlink, mkdir,
 * rename) is where the power goes: that operation happens or not, a
 * file open for writing keeps what was synced plus some of the rest
 * (perhaps with rubbish at its end), and every call after it fails
 * until memfs_reboot(). That is what FAT on a stick can do when the
 * power goes; what it cannot do (lose a synced file, change one nobody
 * was writing) is what backup's rules rely on, and this does not do
 * either.
 */

#include <stdint.h>
#include <stdbool.h>
#include "bk.h"

typedef struct memfs memfs_t;

memfs_t *memfs_new(uint32_t card_kb, uint32_t usb_kb);
void memfs_free(memfs_t *m);
memfs_t *memfs_clone(const memfs_t *m);

void memfs_io(memfs_t *m, bk_io_t *io);

// The clock: set or not; local time; seconds since power-on.
void memfs_clock(memfs_t *m, bool set, int Y, int M, int D, int h, int mi, int s);
void memfs_tick(memfs_t *m, int seconds);

// Test helpers: put a file on the card (or the drive) with that content.
void memfs_put(memfs_t *m, const char *path, const void *data, uint32_t len);
void memfs_puts(memfs_t *m, const char *path, const char *text);
// Its content (NULL if none), and length.
const uint8_t *memfs_get(const memfs_t *m, const char *path, uint32_t *len);
bool memfs_rm(memfs_t *m, const char *path);
void memfs_mkdir_p(memfs_t *m, const char *path);
bool memfs_exists(const memfs_t *m, const char *path);
// Change one byte of a file, keeping its date (a damaged copy).
bool memfs_poke(memfs_t *m, const char *path, uint32_t off, uint8_t v);
// Cut a file to len bytes, keeping its date.
bool memfs_truncate(memfs_t *m, const char *path, uint32_t len);
// The drive gone (unplugged) or back.
void memfs_usb(memfs_t *m, bool there);

// The drive says it has kb more room than it has (as a FAT's free count
// can), so a write runs out in the middle.
void memfs_free_lie(memfs_t *m, uint32_t kb);

// Power cuts. Counted over writes to either volume (backup writes only
// the drive; a restore writes the card).
void memfs_cut_after(memfs_t *m, int n);   // 0: never
bool memfs_dead(const memfs_t *m);
void memfs_reboot(memfs_t *m);             // power back: clock unset, uptime 0
int memfs_drive_ops(const memfs_t *m);     // writes so far, to either volume
int memfs_card_ops(const memfs_t *m);      // writes to the card so far

// Byte-for-byte the same card (every file under "/" but "/usb")?
bool memfs_card_same(const memfs_t *a, const memfs_t *b);

// Every file, for dumping: calls fn(path, data, len, dir).
void memfs_each(const memfs_t *m, void (*fn)(void *arg, const char *path,
	const uint8_t *data, uint32_t len, bool dir), void *arg);

// The owner that LOCK.TXT names: alive while this "boot" lasts.
void memfs_owner(memfs_t *m, const char *who);

// What backup said.
const char *memfs_out(const memfs_t *m);
void memfs_out_clear(memfs_t *m);
void memfs_verbose(memfs_t *m, bool v);
// Stop after n progress calls (0 never), as a person pressing Esc.
void memfs_stop_after(memfs_t *m, int n);

#endif
