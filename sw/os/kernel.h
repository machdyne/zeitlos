#ifndef Z_KERNEL_H
#define Z_KERNEL_H

#include <string.h>
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

	// Tick at which a BLOCKED process becomes runnable again, or 0 for
	// "no timeout, wait indefinitely". Only meaningful while BLOCKED is
	// set. See k_proc_wait() in kernel.c.
	uint32_t		wake_tick;

	uint32_t		regs[32];

} z_proc;

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
#define Z_PROC_FLAG_BLOCKED	0x000000004

// Scheduler helpers -- k_proc_unblock() is called from msg.c on every
// delivery, so it has to be visible outside kernel.c.
uint32_t k_proc_runnable_count(void);
void k_proc_unblock(uint32_t pid);
z_obj_t *k_proc_wait(z_obj_t *args);

// Eligible for a timeslice: active and not blocked.
#define Z_PROC_RUNNABLE(p) \
	(((p).flags & (Z_PROC_FLAG_ACTIVE | Z_PROC_FLAG_BLOCKED)) \
		== Z_PROC_FLAG_ACTIVE)

#define Z_PROCS_MAX 16

// Per-process stack+heap allowance (see mem.h's own comment on why
// there's no separate heap region at all -- this is the ONLY room a
// process's call stack AND malloc()'d heap ever get, shared, for its
// entire lifetime).
//
// IMPORTANT, because it is easy to get backwards: this is NOT where a
// process's static footprint lives. Code, .rodata and .bss are part of
// the BINARY, and k_proc_create() sizes the block as image + this. So
// `repl`'s 96KB Scheme cell heap (MS_HEAP_SIZE * sizeof(ms_val), a
// .bss array inside ms.o) is entirely unaffected by the tier chosen
// here -- changing repl from LARGE to MEDIUM below costs it zero
// Scheme cells. What the tier bounds is the C stack plus whatever
// malloc() hands out at runtime.
//
// Four tiers:
//
// - Z_PROC_STACK_SIZE_SMALL (8KB): `wm` and `term`. This is the size
//   this project originally shipped with for everything; DEFAULT below
//   doubled it as a blanket safety margin, and the note there is
//   explicit that no app had ever shown a confirmed need for more.
//   These two are plain message-loop apps -- no interpreter, no
//   per-message allocation that outlives a call -- so they're the two
//   with the least reason to pay the doubled margin, and returning
//   them to 8KB is what made room for a second `term` instance on a
//   1MB board (see docs/boot.md's memory budget).
//
// - Z_PROC_STACK_SIZE_DEFAULT (16KB): anything not named below. Still
//   the right default for an unknown app: the margin costs little when
//   only one or two processes are unaccounted for, and an app nobody
//   has measured is exactly the one that shouldn't get the smallest
//   tier.
//
// - Z_PROC_STACK_SIZE_MEDIUM (32KB): `net` AND `repl`.
//
//   Both used to be LARGE, for the same reason: their z_port_send()
//   call leaked a small z_obj_blob() allocation per message relayed,
//   for the lifetime of the connection -- confirmed on real hardware
//   as the cause of a heap-exhaustion crash during a long telnet
//   session. **That leak is fixed** (see zport.h's Z_PORT_DATA_ACK and
//   docs/messaging.md), and the question of whether either could come
//   back down was left deliberately open, pending a real number.
//
//   Both now have one. `net` still holds the one-shot,
//   intentionally-leaked RPC replies (DHCP/DNS/TFTP in net.c), bounded
//   by request COUNT rather than session length. `repl` reports its own
//   figure at every boot -- "heap grown 5960 bytes by end of stdlib
//   load" -- so its baseline C-heap use is ~6KB, leaving ~26KB of
//   headroom at this tier.
//
//   THE REMAINING RISK FOR `repl` IS STACK, NOT HEAP, and it's worth
//   knowing what to watch: deep non-tail Scheme recursion nests
//   ms_eval() frames on the C stack. MS_PROTECT_STACK_SIZE (192,
//   sw/apps/repl/Makefile) bounds that depth, so the worst case is
//   roughly 192 frames -- comfortably inside 32KB at any plausible
//   frame size, but not by so much that it's beyond testing. Something
//   deliberately recursive is the thing to try. The symptom of getting
//   this wrong is the silent heap/stack exhaustion this tier system
//   exists to prevent, not a clean error; `(free)`'s own "c-heap"
//   figure (docs/scheme_api.md) is the number to watch, and putting
//   repl back on LARGE is the fix.
//
// - Z_PROC_STACK_SIZE_LARGE (64KB): nothing, currently. Kept defined
//   rather than deleted precisely so the line above is a one-word
//   change if MEDIUM proves too tight for either of them.
#define Z_PROC_STACK_SIZE_SMALL    8*1024
#define Z_PROC_STACK_SIZE_DEFAULT  16*1024
#define Z_PROC_STACK_SIZE_MEDIUM   32*1024
#define Z_PROC_STACK_SIZE_LARGE    64*1024

// which tier (above) a process named `name` should get -- the one
// place this decision is made, used by every path that can start a
// process by name (sh.c's `run`/`init`, and k_proc_run()'s own
// Z_SYS_PROC_RUN syscall handler in kernel.c, which is how wm's dock
// launches apps). A single shared check specifically so a future
// tier change doesn't require finding and updating every call site
// individually the way `net` joining `repl` here once did.
static inline uint32_t z_proc_stack_size_for(const char *name) {
	if (!strcmp(name, "repl") || !strcmp(name, "net"))
		return Z_PROC_STACK_SIZE_MEDIUM;
	if (!strcmp(name, "wm") || !strcmp(name, "term"))
		return Z_PROC_STACK_SIZE_SMALL;
	return Z_PROC_STACK_SIZE_DEFAULT;
}

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
