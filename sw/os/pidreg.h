#ifndef Z_PIDREG_H
#define Z_PIDREG_H

#include <stdbool.h>

#include "kernel.h"

/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * PID name registry -- lets a process register itself under a
 * human-readable name ("term", "port") and get back a kernel-numbered
 * full name ("term0", "port3") unique among currently-registered
 * instances of that base name, so other processes can look a pid up
 * by name instead of relying on it always landing on the same fixed
 * pid (the Z_PID_WM/Z_PID_NET convention -- see zwm.h/znet.h -- which
 * only ever worked because sh.c's init() starts things in a fixed
 * order; doesn't help for something like `term`, which the user can
 * start any number of times in any order).
 *
 * Every registration gets a number, even the first ("wm" registers as
 * "wm0", not bare "wm") -- one uniform scheme rather than a special
 * case for "the first one". Numbers are the smallest currently-unused
 * one for that base name, not a permanent historical counter: an
 * exited, reaped process's names are released (see
 * k_pidreg_release_all(), called from kernel.c's reap path) and their
 * numbers become available for reuse. Don't rely on a number for
 * anything beyond "distinct among what's registered right now" --
 * e.g. don't assume "term0" today is the same underlying instance as
 * "term0" an hour ago.
 */

#define Z_PIDREG_MAX 32          // total live registrations, across all processes/names
#define Z_PIDREG_BASENAME_MAX 16 // caller-supplied base name, e.g. "term"
#define Z_PIDREG_NAME_MAX 24     // full assigned name (base + digits)

// -- syscall handlers, registered in syscalls.def --
//
// named k_pid_* (not z_pid_*) for the same reason k_msg_send/
// k_msg_read aren't named z_msg_send/z_msg_read -- see msg.h's
// comment. z_pid_register()/z_pid_lookup() below are the nicer,
// same-shape-either-side-of-the-syscall-boundary wrappers: the
// app-facing ones live in ../common/zeitlos.h/.c (reached through the
// z_kernel_ptr trampoline), the kernel-native ones are right here
// (for sh.c, which IS the kernel, same reasoning as z_msg_send's
// kernel-native copy in msg.c).
//
// k_pid_register: args is Z_STR (the base name) on input. Registers
// it for the CALLING process (z_pid at the time of the call -- there's
// no way to register on behalf of a different pid, same as
// k_msg_send() stamps `from` itself rather than trusting the caller).
// On success, mutates args in place to Z_STR pointing at the stored
// full name (stable for as long as that registration stays active)
// and returns &z_ok; on failure (empty/too-long base name, or the
// registry is full) sets args->type = Z_NONE and returns &z_fail.
// Same in-place-mutation-plus-status-return convention every other
// syscall handler in this codebase uses (z_uptime(), k_msg_read(),
// etc.) -- check success via the VALUE at the returned pointer
// (rv->val.uint32 == Z_OK), never by comparing the pointer itself:
// &z_ok/&z_fail are `static` (zobj.h), so a kernel-compiled handler's
// copy is never pointer-equal to an app's own -- see z_msg_send()'s
// existing precedent, and pidreg.c's comment above k_pid_register()'s
// implementation.
//
// k_pid_lookup: args is Z_STR (a full name, e.g. "term3") on input.
// On success, mutates args to Z_UINT32 (the owning pid) and returns
// &z_ok; on failure (nothing active matches) sets args->type = Z_NONE
// and returns &z_fail. Same conventions as above.
z_obj_t *k_pid_register(z_obj_t *args);
z_obj_t *k_pid_lookup(z_obj_t *args);

// releases every registration owned by `pid`. Called from kernel.c's
// scheduler reap path (where a Z_PROC_FLAG_DIE process's slot gets
// freed) -- without this, a freed pid slot later reused by a
// completely unrelated process would leave stale registry entries
// pointing at whatever new thing happens to land on that same pid
// number, resolvable by name lookup to the WRONG process. Same class
// of bug z_translate()'s pid-0 special case in msg.c guards against
// (a real one, once, elsewhere) -- worth being proactive about here
// rather than waiting to find it the same way.
void k_pidreg_release_all(uint32_t pid);

// zeroes the registry table. MUST be called once at kernel startup,
// before any process can possibly reach k_pid_register()/k_pid_lookup()
// -- kernel.c calls this right alongside its existing z_procs[]
// zeroing loop, for the same reason: .bss is NOT reliably zero at
// boot on this hardware (main memory is SDRAM, nothing pre-zeroes it,
// and there's no crt0-style bss-clear loop in boot_picorv32.S --
// z_procs[] being manually zeroed in kernel.c despite living in .bss
// is the existing precedent this follows). Without this, `.active`
// can start as garbage-nonzero and `.name` as non-NUL-terminated
// garbage, and k_pid_register()'s strcmp() against that garbage name
// walks off the end of the array into unmapped memory on the very
// first registration -- a real, hardware-only bug (works fine in any
// host-side test or simulation environment that happens to zero
// static storage by convention, which is exactly why this wasn't
// caught before real hardware).
void k_pidreg_init(void);

// prints every active registration (slot, full name, owning pid) to
// the console -- `pr` in sh.c. Same style/purpose as k_proc_dump()
// ("ps") in kernel.c -- a debugging aid, not something any other code
// should parse or depend on.
z_rv k_pidreg_dump(void);

// -- kernel-native wrappers, same shape as ../common/zeitlos.h's
// app-facing versions -- see the comment above k_pid_register/
// k_pid_lookup for why both exist --

bool z_pid_register(const char *basename, char *out, uint32_t outlen);
bool z_pid_lookup(const char *name, uint32_t *pid);

#endif
