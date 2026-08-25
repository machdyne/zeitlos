#ifndef ZPROC_H
#define ZPROC_H

#include <stdint.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Wire shape for the Z_SYS_PROC_LIST and Z_SYS_MEM_STATS syscalls --
 * shared between the kernel-side handlers (sw/os/procapi.h/.c and
 * sw/os/mem.c) and the app-facing wrappers (z_proc_list()/
 * z_mem_stats(), sw/common/zeitlos.c). Exactly the role sw/common/zfs.h
 * already plays for the FS_* syscalls; see sw/os/fsapi.h's own header
 * comment for the full design writeup on why these are plain structs
 * rather than z_obj_t/Z_MAP, and why the kernel can dereference the
 * caller's own pointers directly with no z_translate().
 *
 * Both of these exist because the kernel already had this information
 * but only ever PRINTED it: k_proc_dump() and k_mem_dump() are
 * printf()-to-the-serial-console functions backing sh.c's `ps` and
 * `free`. Neither is reachable from an app, and neither returns
 * anything a caller could filter, sort, or act on. These two syscalls
 * return the same information as data instead, which is what lets the
 * Scheme API's (ps) and (free) hand back real lists (see
 * docs/scheme_api.md). The print-only versions stay exactly as they
 * are -- same relationship k_fs_list() already has to fs_list_dir().
 */

// One process table entry, mirroring sw/os/kernel.h's own z_proc for
// exactly the fields k_proc_dump() already chose to show -- NOT the
// whole struct: z_proc carries all 32 saved GPRs (regs[32], 128 bytes
// per process), and copying those into userland for every process on
// every (ps) call would be both wasteful and a much larger promise
// about kernel-internal layout than this needs to make. pc/sp are
// pulled out individually (regs[0]/regs[2], the same two indices
// k_proc_dump() itself reads) because those two are genuinely useful
// to see; the rest are deliberately not exposed.
typedef struct {
	uint32_t	pid;
	uint32_t	base;
	uint32_t	size;
	uint32_t	pc;			// regs[0]
	uint32_t	sp;			// regs[2]
	uint32_t	flags;		// Z_PROC_FLAG_ACTIVE / _DIE, see kernel.h
} z_proc_info_t;

typedef struct {
	z_proc_info_t	*out;		// OUT: caller-owned array, >= max entries
	uint32_t		max;		// capacity of `out`, in entries
	uint32_t		count;		// OUT: entries actually written
	uint32_t		truncated;	// OUT: 1 if `max` cut the listing short
} z_proc_list_args_t;

// The same numbers k_mem_dump() prints, in BYTES rather than the KB it
// rounds to for display -- a caller that wants KB can divide, a caller
// that wanted bytes can't un-round. `blocks_max` is Z_MEM_MAX_BLOCKS
// (mem.h), returned rather than assumed so an app built against a
// different header than the running kernel still reports the kernel's
// own real limit.
typedef struct {
	uint32_t	total;			// OUT: whole pool, bytes
	uint32_t	used;			// OUT: bytes in allocated blocks
	uint32_t	free;			// OUT: total - used
	uint32_t	largest_free;	// OUT: biggest single free block, bytes
	uint32_t	used_blocks;	// OUT: count of allocated blocks
	uint32_t	free_blocks;	// OUT: count of free blocks
	uint32_t	blocks_used;	// OUT: block descriptors consumed
	uint32_t	blocks_max;		// OUT: Z_MEM_MAX_BLOCKS
} z_mem_stats_args_t;

#endif
