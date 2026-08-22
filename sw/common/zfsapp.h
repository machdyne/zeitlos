#ifndef ZFSAPP_H
#define ZFSAPP_H

#include <stdint.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * App-facing filesystem access -- fs_size()/fs_mallocfile()/
 * fs_write_file(), backed by the new Z_SYS_FS_SIZE/_READ/_WRITE
 * syscalls (sw/os/fsapi.h/.c). The first way for an ORDINARY APP
 * (previously only sh.c/kernel code, via sw/os/fs/fs.h) to read or
 * write a file. Added to unblock sw/apps/repl's `te` command
 * (docs/editor.md), deliberately general -- any future app wanting
 * file access should link zfsapp.c and include this, rather than
 * inventing its own.
 *
 * Deliberately its OWN header, not folded into zeitlos.h despite
 * being exactly the kind of app-facing syscall wrapper zeitlos.h
 * otherwise holds -- see zeitlos.h's own comment on why: it's also
 * included by kernel-side code, which separately includes
 * sw/os/fs/fs.h (the kernel-native FatFs wrappers), and fs.h ALREADY
 * declares these exact three names for the kernel's own direct-FatFs
 * versions (different signatures -- uint32_t vs int, etc.).
 * Declaring them again under the same names in zeitlos.h would
 * collide at kernel-compile time. Any app that wants file access
 * links zfsapp.c (a few hundred bytes) and includes this header
 * directly instead -- kernel.c never does either.
 *
 * These three names/signatures are deliberately exactly what
 * sw/ext/te's own -DEMBEDDED build expects (see its README.md,
 * "Embedded targets") -- this header/implementation IS that contract
 * for Zeitlos apps, not something repl.c's `te` bridge wraps a second
 * time. See docs/editor.md for the fuller writeup, and sw/os/fsapi.h
 * for the syscall-level design (why a syscall and not a message, why
 * no kernel-side malloc(), the concurrency caveat).
 */

// returns `filename`'s size in bytes, or 0 if it doesn't exist -- NOT
// an error (see te's own README.md on why "doesn't exist yet" is the
// normal way to create a new file).
int fs_size(char *filename);

// reads the WHOLE file into a freshly malloc()'d buffer (the caller
// owns it, and must free() it), or returns NULL if the file doesn't
// exist, or if allocation/reading it failed -- same "can't tell those
// apart from the return alone" limitation te's own README.md already
// documents for this exact function signature.
char *fs_mallocfile(char *filename);

// creates (or truncates) `filename` and writes `len` bytes from
// `buf`, returning the number of bytes actually written (0 on
// failure).
int fs_write_file(char *filename, char *buf, int len);

// deletes `filename` (or an empty directory). Returns 1 on success,
// 0 on failure -- NOTE this is the OPPOSITE convention from
// fs_unlink() inside sw/os/fs/fs.c itself (which returns 0 for
// success) -- deliberately not propagated here, to keep every
// function in THIS header consistent with each other (non-zero =
// success, matching fs_write_file()'s own return).
int fs_unlink(char *filename);

// lists directory `path` (NULL, "", or "/" for the root directory),
// returning up to `max_entries` names (0 = this build's own default
// cap -- see zfsapp.c) as a freshly malloc'd array of freshly
// malloc'd strings, or NULL if the directory doesn't exist or nothing
// could be allocated. `*count` is set to how many entries came back.
// Each name is already a full "/"-prefixed path (e.g. "/WM", not
// "WM") -- FatFs itself doesn't care about the leading slash (a bare
// name and a "/"-prefixed one resolve to the same file), so these are
// safe to pass straight into fs_size()/fs_mallocfile()/
// fs_write_file()/fs_unlink() above unchanged.
//
// Caller owns the result: free() every string in the array, then
// free() the array itself (or just hand the whole thing to
// ms_mk_str_list(), sw/ext/ms/ms.c, which takes ownership of each
// string the same way -- see zapi.c's own zapi_ls()).
char **fs_list(const char *path, uint32_t max_entries, uint32_t *count);

// -- chunked file I/O -- for a transfer too large to hold entirely in
// memory at once (tget/tput, docs/scheme_api.md "Networking") -- the
// three functions above load/hold a whole file; these move it one
// bounded chunk at a time instead. Returns a small opaque handle
// (>= 0) on success, or -1 on failure (file doesn't exist for
// fs_open_read(), or any FatFs-level failure). See sw/os/fsapi.h/
// sw/common/zfs.h for the kernel-side design writeup (why a handle,
// not a raw FIL*; the known limitation on a crashed process's
// handles).
int fs_open_write(const char *filename);
int fs_open_read(const char *filename);

// reads up to `maxlen` bytes into `buf`, returning the number
// actually read -- 0 means end-of-file (NOT an error, same
// convention the kernel-native fs_read_chunk(), sw/os/fs/fs.c,
// already uses), a negative value means a real failure (bad handle,
// or FatFs itself failed).
int fs_read_chunk(int handle, void *buf, int maxlen);

// writes `len` bytes from `buf`, returning the number actually
// written, or a negative value on failure.
int fs_write_chunk(int handle, const void *buf, int len);

// closes a handle from fs_open_read()/fs_open_write() -- 1 on
// success, 0 on failure. Always call this when done (or on any error
// path) -- see the "known limitation" note above on what happens if
// you don't (a leaked slot in a small, bounded kernel-side table).
int fs_close_handle(int handle);

#endif
