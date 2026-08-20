#ifndef Z_KERNEL_H
#define Z_KERNEL_H

#include "../common/zeitlos.h"

// z_rv, Z_OK and Z_FAIL are defined in ../common/zmsg.h (pulled in via
// zeitlos.h above) since apps need them too, not just the kernel.

#define Z_IRQ_KTIMER			3
#define Z_IRQ_UART			4
#define Z_IRQ_HID				5
#define Z_IRQ_HID1				6	// second USB HID port -- see
									// rtl/sysctl.v's cpu_irq[6],
									// sw/os/hid.c

typedef struct {

	uint32_t		base;
	uint32_t		size;
	uint32_t		flags;
	uint32_t		regs[32];

} z_proc;

#define Z_PROC_FLAG_ACTIVE	0x000000001
#define Z_PROC_FLAG_DIE		0x000000002

#define Z_PROCS_MAX 16

// Per-process stack+heap allowance (see mem.h's own comment on why
// there's no separate heap region at all -- this is the ONLY room a
// process's call stack AND malloc()'d heap ever get, shared, for its
// entire lifetime). Two tiers, not one blanket size for every
// process:
//
// - Z_PROC_STACK_SIZE_LARGE (64KB): confirmed necessary for `repl`
//   specifically (sw/apps/repl/repl.c) -- Scheme stdlib loading alone
//   consumes several KB of this on startup (see repl's own "heap
//   grown N bytes by end of stdlib load" boot log line), and every
//   zport.h z_port_send() call after that (one per keystroke echoed,
//   plus every reply) leaks a small, DELIBERATELY never-freed
//   z_obj_blob() allocation into this same region for the rest of
//   the connection's lifetime -- see zport.c's own z_port_send()
//   comment for why that leak is intentional, not a bug still to
//   fix. A smaller allowance here would just reproduce the original
//   real-hardware crash that motivated raising this in the first
//   place (silent Z_BLOB allocation failures, see zobj.c's
//   z_obj_blob() comment), just later instead of immediately.
// - Z_PROC_STACK_SIZE_DEFAULT (16KB): everything else (the kernel's
//   own self-registration, wm, net, term, ...). None of these have
//   ever shown a confirmed need for more than the original 8KB this
//   project shipped with -- doubled here as a safety margin (there's
//   no hard data ruling out needing slightly more), not because any
//   of them have their own known Scheme-stdlib-style story the way
//   `repl` does. Confirmed on real hardware (Obst's 1MB variant,
//   `MEM 1` in rtl/boards.vh) that paying the 64KB default for every
//   process left no room to run wm+net+repl+term all at once --
//   see docs/csrs.md and this project's own memory-budget history
//   around this constant for the full story.
#define Z_PROC_STACK_SIZE_DEFAULT  16*1024
#define Z_PROC_STACK_SIZE_LARGE    64*1024

// the live process table and the pid of the process currently
// scheduled/executing -- defined in kernel.c. msg.c (and anything
// else that needs to translate another process's pointers) needs
// direct access to z_procs[pid].base, and z_pid to know who's
// calling.
extern volatile uint32_t z_pid;
extern volatile z_proc z_procs[Z_PROCS_MAX];

// ticks since boot, ~732Hz (the KTIMER IRQ rate -- see
// rtl/sysctl.v's rtc_ctr). apps reach this via the Z_SYS_UPTIME
// syscall/z_uptime_ticks() (zeitlos.c); sh.c, being the kernel itself,
// reads it directly.
extern volatile uint32_t z_kernel_ticks;

// --

uint32_t k_proc_create(uint32_t size, uint32_t stack_size);
uint32_t k_proc_base(uint32_t pid);
z_rv k_proc_start(uint32_t pid);
z_rv k_proc_stop(uint32_t pid);
z_rv k_proc_dump(void);
z_rv k_proc_kill(uint32_t pid);
z_rv k_kernel_dump(void);

// raw, unbuffered UART print -- no libc stdio involved at all (no
// buffering, no heap). defined in kernel.c. exposed here (was
// private to kernel.c) because it's the right tool for exactly the
// class of bug that found snprintf()'s hang in pidreg.c: something
// worth reaching for whenever debugging a hardware-only issue where
// full libc stdio itself might be part of what's broken.
void kprint(const char *s);
void kprint_hex32(uint32_t val);

// --

/*
static inline uint32_t maskirq(uint32_t new_mask) {
    uint32_t old_mask;

    __asm__ volatile (
        ".insn r 0x0B, 0x6, 0x03, %0, %1, zero"
        : "=r"(old_mask)      // output: destination register
        : "r"(new_mask)       // input: source register
        : "memory"
    );

    return old_mask;
}
*/

// --

z_obj_t *z_exit(z_obj_t *obj);

#endif
