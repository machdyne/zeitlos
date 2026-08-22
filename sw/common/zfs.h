#ifndef ZFS_H
#define ZFS_H

#include <stdint.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Wire shape for the Z_SYS_FS_SIZE/_READ/_WRITE syscalls -- shared
 * between the kernel-side handlers (sw/os/fsapi.h/.c, which also
 * needs kernel.h -- NOT included here, so this stays includable from
 * plain app code too) and the app-facing wrappers (fs_size()/
 * fs_mallocfile()/fs_write_file(), sw/common/zeitlos.c). Same role
 * zmsg.h plays for z_msg_t between msg.c and zeitlos.c -- see that
 * header's own precedent.
 *
 * See sw/os/fsapi.h for the full design writeup on why these are
 * plain structs (not z_obj_t/Z_MAP) and why the kernel can read/write
 * the pointers inside them directly, with no z_translate().
 */

typedef struct {
	char		*name;	// filename, caller-owned
	uint32_t	size;	// OUT: file size in bytes, or 0 if not found
} z_fs_size_args_t;

typedef struct {
	char		*name;		// filename to read
	void		*buf;		// destination, caller-owned, >= maxlen bytes
	uint32_t	maxlen;		// capacity of buf
	uint32_t	len;		// OUT: bytes actually read (0 on failure)
} z_fs_read_args_t;

typedef struct {
	char		*name;		// filename to write (created/truncated)
	void		*buf;		// source, caller-owned, >= len bytes
	uint32_t	len;		// bytes to write
	uint32_t	written;	// OUT: bytes actually written (0 on failure)
} z_fs_write_args_t;

typedef struct {
	char		*name;		// file (or empty dir) to delete
} z_fs_unlink_args_t;

typedef struct {
	char		*path;		// directory to list (NULL or "" or "/" = root)
	char		*out;		// OUT: caller-owned buffer -- entries packed
							// as NUL-terminated strings back-to-back,
							// each already a full "/"-prefixed path
							// (e.g. "/WM", not "WM" or "//WM")
	uint32_t	out_cap;	// capacity of `out` in bytes
	uint32_t	max_entries;	// stop after this many entries even if
								// more exist (0 = no extra cap beyond
								// out_cap itself)
	uint32_t	count;		// OUT: how many entries were actually written
	uint32_t	truncated;	// OUT: 1 if out_cap or max_entries cut the
							// listing short, 0 if it's complete
} z_fs_list_args_t;

/*
 * Chunked file I/O -- FS_OPEN_READ/FS_OPEN_WRITE/FS_READ_CHUNK/
 * FS_WRITE_CHUNK/FS_CLOSE. Added for the Zeitlos Scheme API's `tget`/
 * `tput` (docs/scheme_api.md, "Networking") -- the whole-file
 * FS_READ/FS_WRITE above are the right shape for `te`'s small-file
 * ceiling (docs/editor.md) but the wrong one for a TFTP transfer that
 * isn't guaranteed to be small; these hold at most one chunk in
 * memory on either end, same reasoning zstream.h's own header comment
 * gives for the streaming protocol itself.
 *
 * A file open across several syscalls needs somewhere to keep the
 * live FatFs FIL between them -- kept KERNEL-side (a small, bounded
 * table, sw/os/fsapi.c), not in the caller's own memory: FIL is a
 * FatFs-internal struct apps have no reason to see the layout of (no
 * app translation unit includes fs/fatfs/ff.h, only kernel code
 * does), and handing the caller an opaque "allocate exactly
 * sizeof(FIL) bytes, don't look inside" contract would be fragile --
 * a size mismatch between kernel and app builds fails silently rather
 * than at compile time. A caller instead gets back a small integer
 * handle (an index into that kernel-side table) and passes it to
 * every subsequent call -- same shape a Unix file descriptor is,
 * just scoped to exactly this use case rather than general-purpose.
 *
 * Ownership: each handle records which pid opened it (kernel's own
 * z_pid, sw/os/kernel.h -- reliably the calling process's own pid for
 * a syscall's whole duration, k_getpid()'s own comment explains why);
 * every other operation on that handle checks the caller's own z_pid
 * matches before touching it, refusing (Z_FAIL) otherwise -- one
 * process can't read or close a handle another process opened, even
 * by guessing/reusing a small integer.
 *
 * KNOWN LIMITATION, not fixed here: a handle isn't released if its
 * owning process exits (crashes, or is killed) without closing it --
 * there's no process-exit hook wired up to sweep abandoned handles.
 * With Z_FS_MAX_OPEN kept small and this being meant for one
 * at-a-time tget/tput per caller rather than a general-purpose fd
 * table, the practical exposure is narrow (a handful of leaked slots
 * at worst, out of a small fixed table, recoverable by a reboot) --
 * worth a real fix if it proves to matter in practice.
 */

#define Z_FS_MAX_OPEN 4

typedef struct {
	char		*name;
	int32_t		handle;		// OUT: >= 0 on success, -1 on failure
} z_fs_open_args_t;

typedef struct {
	int32_t		handle;
	void		*buf;		// destination, caller-owned, >= maxlen bytes
	uint32_t	maxlen;
	uint32_t	len;		// OUT: bytes actually read (0 = EOF -- NOT a
							// failure, see k_fs_read_chunk()'s own
							// comment -- the return value distinguishes)
} z_fs_read_chunk_args_t;

typedef struct {
	int32_t		handle;
	void		*buf;		// source, caller-owned, >= len bytes
	uint32_t	len;
	uint32_t	written;	// OUT: bytes actually written (0 on failure)
} z_fs_write_chunk_args_t;

typedef struct {
	int32_t		handle;
} z_fs_close_args_t;

#endif
