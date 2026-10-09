#ifndef Z_ZAR_H
#define Z_ZAR_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Core apps in flash ("ZAR" -- Zeitlos ARchive).
 *
 * -- Why --
 *
 * The core apps (wm, net, term) used to live only on the SD
 * card, which meant a freshly flashed board booted to a shell and
 * nothing else. Two costs, and the second is the bigger one:
 *
 *   1. Updating them during development meant hand-driving `xf` in
 *      minicom four times, about a minute of interactive work per
 *      iteration.
 *   2. A new user had to write an SD card before seeing anything.
 *      "Flash the board and you get a desktop" is a much better first
 *      five minutes than "flash the board, now go format a card".
 *
 * So the core apps are also programmed into flash, immediately after
 * the kernel, and `make flash` writes them as part of a normal build.
 * An SD card becomes optional rather than required.
 *
 * `repl` was a core app until the shells moved to the card: it and
 * `posix` are what a term window connects to, init() starts both from
 * the card when one is present, and term's start panel says when they
 * are not there. See docs/flash_apps.md.
 *
 * -- How --
 *
 * Flash is memory-mapped on this SOC, which is what makes this cheap:
 * loading from it is a memcpy, no filesystem and no SPI driver, and it
 * is FASTER than the SD card (which is SPI through the SOC's
 * hardware master, sdmm.c). The
 * BIOS already loads the kernel this way (load_zeitlos(), bios.c) and
 * sw/os/logo.c already reads the boot splash straight out of it -- see
 * logo.h's own comment. This is the same trick a third time.
 *
 * -- Deliberately boot-time only --
 *
 * These apps are NOT a filesystem. They don't appear in `ls`, `run`
 * doesn't look for them, and nothing but init() consults this archive.
 * The rule is exactly:
 *
 *   at boot, per core app: in /apps on the card?  -> use that
 *                          otherwise              -> use the flash copy
 *
 * An app on the card is assumed to be newer, because the only way it
 * got there was somebody deliberately putting it there: a release
 * card does not carry the core apps (docs/layout.md). That keeps
 * `xf apps/wm` working as a single-app hot-swap during development without
 * needing a version scheme, a timestamp comparison, or any notion of
 * precedence beyond "the card wins if it has one".
 *
 * Keeping it out of the general file path is the point, not a
 * limitation: a second namespace that shadows the filesystem is
 * exactly the kind of thing that produces "why is it running the old
 * one" bug reports. Boot says which source each app came from.
 *
 * -- Names: apps are flat, files are paths --
 *
 * An app is "wm", not "/apps/wm", even though that is where the card
 * keeps its copy: sw/os/fs/fs.c's fs_exec_resolve() is what knows about
 * /apps, so nothing here has to.
 *
 * A FILE (an entry with Z_ZAR_FILE set, since ZAR2) is named by its path
 * without the leading slash, "docs/welcome.txt", and is an underlay
 * under the card at that path: read-only, there with or without a card,
 * and shadowed by a file of the same name on the card. fsapi.c's
 * open-for-read, stat and directory listing fall through to it
 * (docs/flash_apps.md, "Files in flash"). Apps are listed as files too,
 * at /apps/<name>, so the file browser shows what is in flash.
 *
 * Names are NUL-terminated, in a string table after the entries, up to
 * Z_ZAR_NAME_MAX characters: short names cost no padding, and a path
 * has room.
 *
 * -- Layout (ZAR2) --
 *   offset  size  field
 *   0       4     magic     "ZAR2"
 *   4       4     count     number of entries
 *   8       8     reserved  must be 0
 *   16      ...   entries[count], 16 bytes each:
 *                   0   4   name      offset of its NUL-terminated name,
 *                                     from the start of the archive
 *                   4   4   flags     bit 0 Z_ZAR_FILE: a file, not an app
 *                   8   4   offset    of the data, from the start
 *                   12  4   size      bytes (an app: the whole ZEXE file)
 *   ...           the names, then the data, each item 4-byte aligned
 *
 * Built by tools/mkzar.py. See Makefile's flash_apps target for where
 * it lands in flash.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../common/zexec.h"
#include "../common/zsoc.h"

// Base of the memory-mapped SPI flash window -- the same MEM_ROM the
// BIOS uses (bios.c) and the same constant logo.h spells out for the
// same reason. KEEP IN SYNC with both.
#define Z_ZAR_ROM_BASE       0x10000000

// Immediately after the kernel's 256KB, as an offset INSIDE the Zeitlos
// flash region -- whose base is read at run time (z_flash_base(),
// sw/common/zsoc.h), so this is never an absolute flash address.
// KEEP IN SYNC with Z_FLASH_ZAR_OFF; release/lib/layout.py checks it
// against that, against bios.c's ROM_OS_SIZE and against the
// Makefile's flash_apps offset.
#define Z_ZAR_FLASH_OFFSET   0x040000

static inline uint32_t z_zar_addr(void) {
	return Z_ZAR_ROM_BASE + z_flash_base() + Z_ZAR_FLASH_OFFSET;
}

#define Z_ZAR_MAGIC0 'Z'
#define Z_ZAR_MAGIC1 'A'
#define Z_ZAR_MAGIC2 'R'
#define Z_ZAR_MAGIC3 '2'

#define Z_ZAR_NAME_MAX   127
#define Z_ZAR_HEADER_SIZE 16
#define Z_ZAR_ENTRY_SIZE  16
#define Z_ZAR_FILE       0x1u	// entry flags: a file, not an app

// A sane upper bound on entry count, purely so a blank or garbage
// flash region can't send the scan below off into nowhere. Erased
// flash reads as 0xFF, so count would come back as 0xFFFFFFFF.
#define Z_ZAR_MAX_ENTRIES 32

// true if a valid archive is present in flash. Everything else here is
// meaningless if this is false -- an unprogrammed flash region is the
// normal case on a board that has never had `make flash_apps` run.
bool z_zar_present(void);

// Number of entries in the archive, apps and files, or 0 if none/invalid.
uint32_t z_zar_count(void);

// Name of entry `i`, into `out` (at least Z_ZAR_NAME_MAX+1 bytes).
// Returns false if `i` is out of range.
bool z_zar_name(uint32_t i, char *out);

// true if entry `i` is a file rather than an app.
bool z_zar_is_file(uint32_t i);

// -- files in flash: the underlay (docs/flash_apps.md, "Files in flash") --
//
// `path` is absolute ("/docs/welcome.txt", "/apps/wm") and matched
// without regard to case, as FAT paths are. An app is the file
// /apps/<name>.

// The data of the file at `path`: its offset in the archive and size.
// 0 if there is one, non-zero if not (or a flash write is in progress).
int z_zar_file(const char *path, uint32_t *off, uint32_t *size);

// Copies `n` bytes at archive offset `off` to `dst`.
void z_zar_read(uint32_t off, uint8_t *dst, uint32_t n);

// The entries directly inside directory `dir` ("/", "/docs"), one per
// call: `*iter` starts at 0. A subdirectory that exists only because a
// file is in it ("docs" in "/") is reported once, as a directory.
// Returns false when there are no more.
bool z_zar_child(const char *dir, uint32_t *iter, char *name,
	uint32_t *size, bool *is_dir);

// true if `dir` is a directory in flash ("/", "/apps", "/docs").
bool z_zar_is_dir(const char *dir);

// Fills `info` for the named app, the same way fs_exec_info() does for
// a file on the card, so callers can treat the two identically.
// Returns 0 on success, non-zero if not found -- matching
// fs_exec_info()'s convention, NOT the usual bool.
int z_zar_exec_info(const char *name, z_exec_info_t *info);

// Copies the named app's image to `dst` and zeroes its .bss, the flash
// counterpart of fs_load_exec(). Returns 0 on success.
//
// Like fs_load_exec(), this flushes the instruction cache afterwards:
// it has just written code through the data path, and the cache caches
// fetches only, so it never saw those stores. See z_icache_flush() in
// sw/common/zsoc.h for the failure this prevents.
int z_zar_load_exec(uint32_t dst, const char *name,
	const z_exec_info_t *info);

#endif
