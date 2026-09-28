#ifndef ZPLAT_H
#define ZPLAT_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The platform, for programs whose core is portable C that runs both on
 * Zeitlos and on a Linux server: the BBS (sw/apps/bbs) and zfed's fed
 * (sw/apps/fed). Files, the clock, randomness, the log.
 *
 *   zplat_zeitlos.c   on Zeitlos: the card through zfsapp.h, the RTC,
 *                     the TRNG, the console
 *   zplat_posix.c     on Linux (and any POSIX system); the host tests
 *                     link it too, with its fake clock
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -- what the platform provides --

uint32_t plat_now(void);               // Unix seconds, UTC; 0 if the clock is not set
uint32_t plat_ms(void);                // milliseconds since some start; wraps
void plat_random(void *buf, uint32_t n);
void plat_log(const char *line);       // one line, no newline

// Files. Paths are full paths (the core joins the data directory).
// Handles are small integers >= 0; -1 is failure. Open briefly: a
// Zeitlos machine has eight handles for the whole system.
#define PLAT_READ    0                 // read; fails if missing
#define PLAT_UPDATE  1                 // read and write, no truncation; fails if missing
#define PLAT_CREATE  2                 // write, created or truncated
int plat_open(const char *path, int mode);
int plat_read(int h, void *buf, int n);          // bytes read, 0 at end, <0 error
int plat_write(int h, const void *buf, int n);   // bytes written, <0 error
bool plat_seek(int h, uint32_t off);
void plat_close(int h);
int32_t plat_size(const char *path);             // -1 if missing
uint32_t plat_mtime(const char *path);           // Unix seconds, 0 if unknown
bool plat_mkdir(const char *path);               // true if it exists afterwards
bool plat_rename(const char *from, const char *to);   // replaces `to`
bool plat_unlink(const char *path);
// The names (not paths) in a directory, NUL-separated into buf,
// sorted; returns how many, -1 if the directory is missing.
int plat_list(const char *dir, char *buf, uint32_t cap);

// Commits what has been written through h to the card or disk: after
// it returns, a crash does not lose it. (POSIX: fsync. Zeitlos: fs_sync.)
bool plat_sync(int h);

#endif
