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
	uint8_t		*types;		// OUT: optional, may be NULL -- one
							// Z_FS_TYPE_* byte per entry, in the same
							// order as `out`. Must have room for
							// max_entries bytes when non-NULL.
} z_fs_list_args_t;

/*
 * Entry types for z_fs_list_args_t.types above.
 *
 * Added because a file browser has to draw a directory differently
 * from a file and, more importantly, has to DO something different
 * when one is picked -- and the packed name list alone can't say
 * which is which. FatFs knows (FILINFO.fattrib & AM_DIR) and was
 * simply throwing the answer away.
 *
 * A separate optional out-buffer rather than, say, a trailing '/' on
 * directory names: the names in `out` are already documented as
 * directly usable with fs_size()/fs_mallocfile()/fs_unlink(), and
 * decorating them would quietly break every existing caller
 * (sw/apps/repl's `ls`) at the same time. A NULL `types` behaves
 * exactly as this syscall always has.
 */
#define Z_FS_TYPE_FILE   0
#define Z_FS_TYPE_DIR    1

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

// FS_MKDIR / FS_TOUCH -- both take nothing but a path, so they share
// one shape rather than carrying two identical single-field structs.
// Deliberately NOT reusing z_fs_unlink_args_t above for this despite
// the identical layout: these are different operations, and a future
// revision that needs to add a field to one of them (a mode/flags
// argument, say) shouldn't have to first untangle it from the other
// two that happened to share a struct.
typedef struct {
	char		*name;		// directory (mkdir) or file (touch) to create
} z_fs_path_args_t;

// FS_SEEK -- repositions an open handle from FS_OPEN_READ/_OPEN_WRITE.
// `offset` is absolute, from the start of the file, matching FatFs's
// own f_lseek(). Seeking past EOF on a READ handle is not an error at
// this level (FatFs clamps to the file size); the next FS_READ_CHUNK
// simply reports 0 bytes, the same clean-EOF result that call already
// documents. Added for sw/apps/repl's `page` -- see this project's
// syscalls.def for the full reasoning.
typedef struct {
	int32_t		handle;
	uint32_t	offset;		// absolute byte offset from start of file
	uint32_t	pos;		// OUT: resulting position (0 on failure)
} z_fs_seek_args_t;

// FS_DF -- filesystem capacity. Reported in KILOBYTES, not bytes,
// deliberately: these are uint32_t, and a 32GB card's byte count
// overflows one. KB covers up to 4TB, which is well past anything this
// OS will see on an SD card, and it matches what the underlying
// fs_total()/fs_free() (sw/os/fs/fs.c) already return -- both compute
// `clusters * csize / 2`, i.e. 512-byte sectors halved into KB.
typedef struct {
	uint32_t	total_kb;	// OUT: whole volume (0 if unmounted/failed)
	uint32_t	free_kb;	// OUT: unallocated space
} z_fs_df_args_t;

#endif
