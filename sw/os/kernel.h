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
// - Z_PROC_STACK_SIZE_LARGE (64KB): `repl` AND `net`, currently --
//   both are zport.h providers whose own z_port_send() call, on every
//   message relayed to a connected `term` (repl: one per keystroke
//   echoed/reply; net: one per chunk of telnet traffic relayed,
//   telnet_on_data() in net.c), used to leak a small z_obj_blob()
//   allocation into this same region for the rest of the connection's
//   lifetime -- confirmed on real hardware as the cause of a heap-
//   exhaustion crash during a long enough session (a chatty telnet
//   BBS, specifically), which is what motivated this LARGE tier's
//   existence at all, and (via a second, separate real bug -- `net`
//   was left off the check below when this became two tiers instead
//   of one blanket size) a second near-identical crash before that
//   omission was caught and fixed.
//
//   **That leak itself is fixed now**, not just budgeted around --
//   see sw/common/zport.h's Z_PORT_DATA_ACK and docs/messaging.md's
//   "Known limitations" for the full design (an explicit ack from the
//   receiver once it's genuinely done reading a DATA message's
//   payload, at which point the sender frees it, rather than holding
//   it for the rest of the connection). Whether `repl`/`net` could
//   safely move back down to the DEFAULT tier below as a result is a
//   real, open question -- both still have OTHER heap usage this fix
//   doesn't touch (repl's Scheme stdlib load at startup; both
//   processes' one-shot RPC-style replies elsewhere, e.g. DHCP/DNS/
//   TFTP responses in net.c, which are still small-and-intentionally-
//   leaked per docs/messaging.md, just bounded by request COUNT now
//   rather than by session length or byte volume) -- but the
//   dominant, unbounded cost that specifically justified LARGE is
//   gone. Left at LARGE here deliberately, not downgraded as a side
//   effect of the DATA_ACK fix: this is a real hardware memory
//   allocation with no data yet on what a downgraded budget actually
//   looks like under load, and getting it wrong here reproduces the
//   exact silent-heap-exhaustion failure mode this whole tier exists
//   to prevent. Worth a real, deliberate, separately-tested pass at
//   DEFAULT for both once there's hardware to check it against, not a
//   guess made here.
// - Z_PROC_STACK_SIZE_DEFAULT (16KB): everything else (the kernel's
//   own self-registration, wm, term, ...). None of these have ever
//   shown a confirmed need for more than the original 8KB this
//   project shipped with -- doubled here as a safety margin (there's
//   no hard data ruling out needing slightly more), not because any
//   of them have their own known Scheme-stdlib-/zport-leak-style
//   story the way `repl`/`net` do. Confirmed on real hardware (Obst's
//   1MB variant, `MEM 1` in rtl/boards.vh) that paying the 64KB
//   default for every process left no room to run wm+net+repl+term
//   all at once -- see docs/csrs.md and this project's own
//   memory-budget history around this constant for the full story.
#define Z_PROC_STACK_SIZE_DEFAULT  16*1024
#define Z_PROC_STACK_SIZE_LARGE    64*1024

// which tier (above) a process named `name` should get -- the one
// place this decision is made, used by every path that can start a
// process by name (sh.c's `run`/`init`, and k_proc_run()'s own
// Z_SYS_PROC_RUN syscall handler in kernel.c, which is how wm's dock
// launches apps). A single shared check specifically so a future
// third LARGE-tier process doesn't require finding and updating every
// call site individually the way `net` joining `repl` here once did.
static inline uint32_t z_proc_stack_size_for(const char *name) {
	return (!strcmp(name, "repl") || !strcmp(name, "net")) ?
		Z_PROC_STACK_SIZE_LARGE : Z_PROC_STACK_SIZE_DEFAULT;
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
