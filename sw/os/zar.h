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
 * The core apps (wm, net, repl, term) used to live only on the SD
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
 * -- How --
 *
 * Flash is memory-mapped on this SOC, which is what makes this cheap:
 * loading from it is a memcpy, no filesystem and no SPI driver, and it
 * is FASTER than the SD card (which is bit-banged SPI, sdmm.c). The
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
 *   at boot, per core app: on the SD card?  -> use that
 *                          otherwise        -> use the flash copy
 *
 * An app on the card is assumed to be newer, because the only way it
 * got there was somebody deliberately putting it there. That keeps
 * `xf wm` working as a single-app hot-swap during development without
 * needing a version scheme, a timestamp comparison, or any notion of
 * precedence beyond "the card wins if it has one".
 *
 * Keeping it out of the general file path is the point, not a
 * limitation: a second namespace that shadows the filesystem is
 * exactly the kind of thing that produces "why is it running the old
 * one" bug reports. Boot says which source each app came from.
 *
 * -- Layout --
 *
 *   offset  size  field
 *   0       4     magic     "ZAR1"
 *   4       4     count     number of entries
 *   8       8     reserved  must be 0
 *   16      ...   entries[count], 24 bytes each:
 *                   0   16  name (NUL-padded, not necessarily
 *                               NUL-terminated if exactly 16 chars)
 *                   16  4   offset  from start of archive
 *                   20  4   size    bytes, the whole ZEXE file
 *   ...           the ZEXE files themselves, in entry order
 *
 * Built by tools/mkzar.py. See Makefile's flash_apps target for where
 * it lands in flash.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../common/zexec.h"

// Base of the memory-mapped SPI flash window -- the same MEM_ROM the
// BIOS uses (bios.c) and the same constant logo.h spells out for the
// same reason. KEEP IN SYNC with both.
#define Z_ZAR_ROM_BASE       0x10000000

// Immediately after the kernel, which occupies 256KB at the 1MB mark
// (see Makefile's flash_os target, which writes at offset 1048576, and
// bios.c's ROM_OS_ADDR/ROM_OS_SIZE). KEEP IN SYNC with Makefile's
// flash_apps target -- there is no way for the two to check each other.
#define Z_ZAR_FLASH_OFFSET   (1024 * 1024 + 1024 * 256)   // 0x140000

#define Z_ZAR_ADDR           (Z_ZAR_ROM_BASE + Z_ZAR_FLASH_OFFSET)

#define Z_ZAR_MAGIC0 'Z'
#define Z_ZAR_MAGIC1 'A'
#define Z_ZAR_MAGIC2 'R'
#define Z_ZAR_MAGIC3 '1'

#define Z_ZAR_NAME_MAX   16
#define Z_ZAR_HEADER_SIZE 16
#define Z_ZAR_ENTRY_SIZE  24

// A sane upper bound on entry count, purely so a blank or garbage
// flash region can't send the scan below off into nowhere. Erased
// flash reads as 0xFF, so count would come back as 0xFFFFFFFF.
#define Z_ZAR_MAX_ENTRIES 32

// true if a valid archive is present in flash. Everything else here is
// meaningless if this is false -- an unprogrammed flash region is the
// normal case on a board that has never had `make flash_apps` run.
bool z_zar_present(void);

// Number of apps in the archive, or 0 if none/invalid.
uint32_t z_zar_count(void);

// Name of entry `i`, into `out` (at least Z_ZAR_NAME_MAX+1 bytes).
// Returns false if `i` is out of range.
bool z_zar_name(uint32_t i, char *out);

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
