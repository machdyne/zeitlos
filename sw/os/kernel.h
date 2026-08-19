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

uint32_t k_proc_create(uint32_t size);
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
