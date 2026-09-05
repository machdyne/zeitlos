#ifndef ZFSAPP_H
#define ZFSAPP_H

#include <stdint.h>
#include <stdbool.h>

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

// Same listing, into storage the CALLER already owns -- no malloc()
// anywhere, and no ownership to hand back.
//
// `buf` receives the entries packed as NUL-terminated strings
// back-to-back (exactly the wire format z_fs_list_args_t.out
// describes -- walk it with strlen()+1), each already a full
// "/"-prefixed path just like fs_list()'s. `types`, if non-NULL,
// receives one Z_FS_TYPE_* byte per entry, in the same order, and
// MUST have room for max_entries of them -- which is why a non-NULL
// `types` with max_entries == 0 is rejected outright rather than
// guessed at (see zfs.h and k_fs_list()).
//
// `*count` gets the number of entries; `*truncated` (either pointer
// may be NULL) gets 1 if buf_cap or max_entries cut the listing
// short. Returns 1 on success, 0 on failure -- this file's usual
// convention.
//
// Prefer this over fs_list() in anything long-lived or repeated. An
// app gets its stack and heap out of one 16KB allocation
// (Z_PROC_STACK_SIZE_DEFAULT, sw/os/kernel.h), and fs_list() takes
// two mallocs plus one per entry, then hands the caller the job of
// freeing all of them -- fine once at startup, needlessly risky for
// something a file browser does every time the user opens a folder.
// sw/common/zflist.c uses this one.
int fs_list_into(const char *path, char *buf, uint32_t buf_cap,
	uint8_t *types, uint32_t max_entries, uint32_t *count,
	uint32_t *truncated);

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

// creates a directory / an empty file. 1 on success, 0 on failure --
// this file's own convention, which is the INVERSE of what the
// kernel-native fs_mkdir()/fs_touch() (sw/os/fs/fs.c) return; the
// translation happens inside these wrappers so no caller has to think
// about it. Added alongside the Scheme API's (mkdir ...) and
// (touch-file ...), see docs/scheme_api.md.
int fs_mkdir(const char *path);
int fs_touch(const char *path);

// repositions an open handle from fs_open_read()/fs_open_write() to an
// absolute byte offset. 1 on success, 0 on failure (bad/foreign
// handle, or a FatFs-level error). Seeking past EOF on a read handle
// is not a failure -- FatFs clamps to the file size and the next
// fs_read_chunk() reports a clean 0-byte EOF. Added for sw/apps/repl's
// `page`, which needs to scroll BACKWARDS through a file far too big
// to hold in memory; every other chunked call only moves forward.
int fs_seek(int handle, uint32_t offset);

// -- in-place modification -- the three calls a file EDITOR needs.
//
// Everything above either reads a file or replaces it. fs_write_file()
// rewrites the whole thing from a buffer, and fs_open_write() is
// FA_CREATE_ALWAYS, so it truncates the moment it opens. Neither can
// change a byte in the middle of a file too large to hold in memory,
// which is what sw/apps/hex is (docs/hex_editor.md).
//
// Opens `filename` for reading AND writing without truncating it,
// positioned at byte 0. Returns a handle (>= 0) usable with
// fs_read_chunk()/fs_write_chunk()/fs_seek()/fs_close_handle() exactly
// as the other two open calls' handles are, or -1 on failure.
//
// REFUSES TO CREATE. A path that doesn't exist is a failure, not an
// empty new file -- call fs_touch() first if creating is what you
// meant. -1 therefore covers "no such file", "no free handle slot"
// (Z_FS_MAX_OPEN is 8, board-wide) and "this kernel predates the
// syscall"; none of them is distinguishable from the return alone,
// same limitation fs_mallocfile() already documents.
int fs_open_rw(const char *filename);

// Commits an open write handle -- its buffered data AND its directory
// entry -- without closing it. 1 on success, 0 on failure.
//
// Worth calling at every point the user would consider their work
// saved. Until it happens, a file written through a handle has the
// right data in the right clusters and the wrong size in the
// directory, so pulling the card leaves the edit unfindable. Harmless
// (and successful) on a read handle.
int fs_sync(int handle);

// Sets an open handle's file size, growing or shrinking. 1 on success,
// 0 on failure. Leaves the handle positioned at the new end of file.
//
// GROWING DOES NOT ZERO. The new region reads back as whatever was
// left on those sectors by whatever used them last, so a caller that
// wants zeros must write them itself -- see z_fs_truncate_args_t in
// sw/common/zfs.h for why that is the deliberate choice.
int fs_truncate(int handle, uint32_t size);

// filesystem capacity, both figures in KB (not bytes -- a 32GB card
// overflows a uint32_t of bytes; see z_fs_df_args_t in zfs.h). Either
// pointer may be NULL. Returns false if the syscall itself failed;
// note that an unmounted or absent card reports 0/0 with a true
// return, since that's a successful answer of "nothing there".
//
// Named fs_df() rather than fs_free() to avoid colliding with the
// kernel-native fs_free() (sw/os/fs/fs.c) -- see zeitlos.h's own note
// on exactly that class of collision.
bool fs_df(uint32_t *total_kb, uint32_t *free_kb);

#endif
