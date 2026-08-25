/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Process-table syscalls -- currently just Z_SYS_PROC_LIST, the
 * data-returning counterpart to k_proc_dump()'s console-only printf()
 * output (sw/os/kernel.c, backing sh.c's `ps`). See sw/common/zproc.h
 * for the wire shape and why this exists at all.
 *
 * A separate translation unit rather than more code in kernel.c: this
 * needs nothing from kernel.c's own statics (z_procs[] is already
 * `extern` in kernel.h, precisely so msg.c and friends can reach it),
 * and kernel.c is already the largest file in the tree. Same reasoning
 * that put the FS_* handlers in their own fsapi.c rather than inline
 * in kernel.c when those were added.
 *
 * Every convention here matches fsapi.c's: args arrive as a plain
 * struct cast from the generic z_obj_t* the syscall table's
 * function-pointer type requires, results are written back into
 * caller-owned storage in place, and the return value is &z_ok/&z_fail.
 * No kernel-side malloc(), no printf(), no snprintf() -- see
 * sw/os/fsapi.h's header comment for why kernel-compiled code in this
 * project stays away from that libc machinery.
 */

#include <stdint.h>
#include <stdbool.h>

#include "kernel.h"
#include "../common/zeitlos.h"
#include "../common/zproc.h"
#include "procapi.h"

// Walks the whole fixed-size process table and copies out every slot
// that's actually in use. "In use" is `base != 0`, exactly the test
// k_proc_dump() itself uses -- deliberately NOT Z_PROC_FLAG_ACTIVE,
// which would hide a process that exists but is currently stopped
// (k_proc_stop()), and hiding those is the opposite of what a `ps`
// is for.
//
// The pid reported is the table INDEX, again matching k_proc_dump()
// (and k_proc_kill()/k_proc_base(), which both take that same index)
// -- so a pid from this listing can be handed straight back to
// (kill ...) without translation.
//
// Truncation is reported rather than silently swallowed: a caller
// that sized `out` too small gets `truncated == 1` and can either
// grow its buffer or at least say so, the same contract k_fs_list()
// already established for directory listings.
z_obj_t *k_proc_list(z_obj_t *args) {

	z_proc_list_args_t *a = (z_proc_list_args_t *)args;

	if (!a || !a->out || a->max == 0) {
		if (a) { a->count = 0; a->truncated = 0; }
		return (&z_fail);
	}

	a->count = 0;
	a->truncated = 0;

	uint32_t n = 0;

	for (uint32_t i = 0; i < Z_PROCS_MAX; i++) {

		if (z_procs[i].base == 0) continue;

		if (n >= a->max) {
			a->truncated = 1;
			break;
		}

		a->out[n].pid   = i;
		a->out[n].base  = z_procs[i].base;
		a->out[n].size  = z_procs[i].size;
		a->out[n].pc    = z_procs[i].regs[0];
		a->out[n].sp    = z_procs[i].regs[2];
		a->out[n].flags = z_procs[i].flags;
		n++;

	}

	a->count = n;

	return (&z_ok);

}
