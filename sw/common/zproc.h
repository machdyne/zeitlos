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
// -- process flags --
//
// These live here, in the APP-facing header, rather than in
// sw/os/kernel.h where they used to. z_proc_info_t below reports
// them, so an app needs them to make any sense of what it got -- and
// kernel.h is not includable from app code (it declares the process
// table, the scheduler, and everything else the kernel keeps to
// itself). kernel.h includes this header and uses these same
// definitions, so there is one copy rather than two that agree by
// convention.

#define Z_PROC_FLAG_ACTIVE	0x000000001
#define Z_PROC_FLAG_DIE		0x000000002

// Process is waiting for something and must NOT be given a timeslice.
//
// Without this, waiting is spelled as a spin: z_msg_wait() in
// zeitlos.c loops on z_msg_read() until something arrives, so a
// process with an empty mailbox burns its entire ~1.37ms turn asking
// "anything yet?" a few thousand times. The scheduler cannot tell that
// apart from real work, so with wm + net + repl idle, a busy
// foreground app still gets only ~1/4 of the CPU -- and adding a fifth
// process slows everything down even if it does nothing.
//
// A blocked process is skipped entirely by the round-robin scan, and
// becomes runnable again via either:
//   - k_msg_send() delivering a message to it (immediate), or
//   - the KTIMER sweep, once z_kernel_ticks reaches wake_tick.
//
// ACTIVE stays set while blocked. BLOCKED is about schedulability, not
// liveness, so k_proc_kill() and the DIE path are unaffected.
//
// It is also the only CPU-load signal this system has: active and not
// blocked is exactly "contending for the CPU right now", which is what
// sw/apps/info charts.
#define Z_PROC_FLAG_BLOCKED	0x000000004

// A wakeup that arrived while the target was still RUNNING.
//
// k_proc_unblock() on a process that has not blocked yet would
// otherwise be a no-op and the wakeup simply lost. That is harmless
// for messages -- the message stays in the mailbox, and k_proc_wait()
// re-checks the mailbox under the same interrupt mask before sleeping,
// which is what that function's long comment is about. It is NOT
// harmless for a source that unblocks DIRECTLY, leaving nothing behind
// for k_proc_wait() to find.
//
// The ethernet receive interrupt is exactly that source: one pulse per
// arrival (rtl/sysctl.v turns the MAC's level into a rising edge on
// purpose -- a latched level re-fires forever), no queue, and no level
// still asserted by the time the driver looks. A frame arriving
// anywhere in net's loop body -- between its eth_poll() and its wait
// -- unblocked a process that was still running, and the frame then
// sat unread until the backstop timeout expired.
//
// So record the wakeup instead of dropping it, and let the next
// k_proc_wait() consume it and decline to sleep.
#define Z_PROC_FLAG_WAKE	0x000000008

// Longest process name reported below. Matches Z_PIDREG_NAME_MAX
// (sw/os/pidreg.h), which is where the names come from -- declared
// separately rather than including that header, which is kernel-side
// and must not be pulled into app builds.
#define Z_PROC_NAME_MAX 24

typedef struct {
	uint32_t	pid;
	uint32_t	base;
	uint32_t	size;
	uint32_t	pc;			// regs[0]
	uint32_t	sp;			// regs[2]
	uint32_t	flags;		// Z_PROC_FLAG_ACTIVE / _DIE / _BLOCKED,
							// see kernel.h
	// The name this process registered, or "" if it never registered
	// one. Filled from the pid registry (sw/os/pidreg.c).
	//
	// The registry is the only place a name exists -- the process
	// table itself has never held one, because nothing needed it: a
	// process is started by filename and identified by pid
	// thereafter. A process list showing nothing but numbers is
	// close to useless to a person, though, which is what this is
	// for.
	//
	// Not every process has one. Only apps that call
	// z_pid_register() appear here by name; the rest report "",
	// and a caller should fall back to the pid.
	char		name[Z_PROC_NAME_MAX];

	// KTIMER ticks this process has been the running one for, since
	// it was created. Free-running and never reset.
	//
	// A percentage is a DIFFERENCE over an interval, not this value:
	// sample it twice, subtract, and divide by the elapsed
	// z_uptime_ticks() over the same interval. Reading it once tells
	// you how much CPU a process has used since boot, which is
	// occasionally what you want and usually not.
	//
	// Sampled at Z_TICK_HZ (~732Hz), so work that starts and finishes
	// between two ticks is invisible. Fine over a second; do not
	// trust a single 100ms reading.
	uint32_t	cpu_ticks;
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
