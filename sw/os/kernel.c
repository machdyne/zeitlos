/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * This is the Zeitlos microkernel.
 *
 * The kernel is loaded at the beginning of main memory (0x4000_0000).
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "kernel.h"
#include "zeitlos.h"
#include "mem.h"
#include "uart.h"
#include "ui.h"
#include "msg.h"
#include "hid.h"
#include "pidreg.h"
#include "logo.h"
#include "fs/fs.h"

// Z_PROCS_MAX now lives in kernel.h (msg.c needs it too)
#define Z_PROC_STACK_SIZE  8*1024
#define Z_KERNEL_STACK_SIZE  8*1024

z_obj_t *z_uptime(z_obj_t *args);	// defined below; forward-declared
									// since syscalls.def (included next)
									// needs it visible for the table
z_obj_t *k_getpid(z_obj_t *args);	// same reasoning -- named k_getpid,
									// not z_getpid, since zeitlos.h
									// (pulled in above) already
									// declares an app-facing
									// z_getpid() with a DIFFERENT
									// signature (uint32_t, no args) --
									// same k_/z_ naming split
									// k_msg_send/z_msg_send and
									// k_pid_register/z_pid_register
									// already use, for the same reason
z_obj_t *k_proc_run(z_obj_t *args);	// ditto -- see definition below

typedef z_obj_t* (*z_syscall_t)(z_obj_t *args);

z_syscall_t z_syscall_table[Z_SYSCALL_COUNT] = {
#define Z_MKSYSCALL(name, fn) [Z_SYS_##name] = fn,
#include "../common/syscalls.def"
#undef Z_SYSCALL
};

extern char _start, _end;

// linker-provided, see riscv-os.ld's .sdata section ("__global_pointer$
// = . + 0x800;") -- the kernel's own correct gp value. Referenced by
// z_kernel_entry()'s syscall dispatch below; see that comment for why.
extern char __global_pointer$;

// force bss because __global_pointer$ will be wrong in the interrupt handler
volatile uint32_t __attribute__((section(".bss"))) z_pid = 0;
volatile z_proc __attribute__((section(".bss"))) z_procs[Z_PROCS_MAX];
volatile uint32_t __attribute__((section(".bss"))) z_kernel_ticks = 0;

// --

void sh(void);
uint32_t *z_kernel_entry(uint32_t cmd, uint32_t *args, uint32_t val);
uint32_t k_proc_active_count(void);

void kprint(const char *s);
void kprint_hex32(uint32_t);

// returns z_kernel_ticks -- increments at ~732Hz (the KTIMER IRQ
// rate, see rtl/sysctl.v's rtc_ctr). apps use this for elapsed-time
// measurement (e.g. sw/apps/net/tftp.c's retry timeout) where a
// loop-iteration count (like enc28j60.c's ETH_TX_TIMEOUT) isn't
// precise enough.
z_obj_t *z_uptime(z_obj_t *args) {
	args->type = Z_UINT32;
	args->val.uint32 = z_kernel_ticks;
	return (&z_ok);
}

// returns the CALLING process's own pid. z_pid correctly identifies
// the caller here because a syscall executes synchronously as a plain
// function call from the currently-scheduled process -- z_pid is only
// ever changed by the scheduler's own KTIMER-driven swap (see
// z_kernel_entry() below), never mid-syscall. First real use: wm.c
// needs to know its own actual pid to correctly identify its own
// windows (previously done via the Z_PID_WM constant, which only
// worked because wm happens to always be started first -- see
// zwm.h's comment on that convention, and sw/os/pidreg.h for the
// name-registry this is meant to work alongside). Named k_getpid, not
// z_getpid -- see the forward declaration above for why.
z_obj_t *k_getpid(z_obj_t *args) {
	args->type = Z_UINT32;
	args->val.uint32 = z_pid;
	return (&z_ok);
}

// launches a new process from a named file on the FAT filesystem --
// same fs_size()/k_proc_create()/k_proc_base()/fs_load()/k_proc_start()
// sequence as sh.c's "run" shell command and init() (see sh.c), but
// reachable via syscall from any running process, not just the kernel
// shell. This is what lets sw/apps/wm's dock launch apps (e.g. "term",
// "gpu3d") when an icon is clicked -- before this syscall existed,
// only kernel-space code (sh.c, compiled directly into kernel.bin)
// could start a new process at all.
//
// named k_proc_run(), not z_proc_run() (which would collide with --
// and originally did, before this rename -- the userland wrapper of
// the same name declared in zeitlos.h and defined in zeitlos.c, which
// this file's own zeitlos.h #include also pulls in): same k_-prefix
// convention as k_proc_create()/k_proc_base()/k_proc_start() above
// and k_getpid()/k_pid_register()/k_pid_lookup() just above, all of
// which are kernel-side syscall handlers with a same-named or
// differently-named userland-facing counterpart.
//
// args->val.str is the filename, same bare names ("term", not
// "term.bin") sh.c's `run`/init() use -- no path, no extension. copied
// into a fixed local buffer rather than used in place: unlike
// z_ui_print()'s obj->val.str (read once, in one straight pass), this
// name gets read multiple times across several calls below, and it's
// cheap insurance against the caller's string being something other
// than a stable literal.
//
// result convention matches z_uptime()/k_getpid() above, not
// z_exit()/z_ui_print(): the actual result (new pid, or 0 on failure)
// is written back into args->val.uint32 (in/out parameter), while the
// z_ok/z_fail return value is just success/fail -- see zeitlos.c's
// z_proc_run() wrapper for the caller side.
#define Z_PROC_RUN_NAME_MAX 32
z_obj_t *k_proc_run(z_obj_t *args) {

	if (!args) return (&z_fail);

	if (args->type != Z_STR || !args->val.str) {
		args->type = Z_UINT32;
		args->val.uint32 = 0;
		return (&z_fail);
	}

	char name[Z_PROC_RUN_NAME_MAX];
	strncpy(name, args->val.str, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;

	uint32_t pid = 0;
	uint32_t size = fs_size(name);

	if (size) {
		pid = k_proc_create(size);
		if (pid) {
			uint32_t base = k_proc_base(pid);
			fs_load(base, name);
			k_proc_start(pid);
		}
	}

	args->type = Z_UINT32;
	args->val.uint32 = pid;

	return pid ? (&z_ok) : (&z_fail);

}

// --

int main(void) {

	// boot splash -- VRAM is plain memory-mapped hardware with no
	// init of its own needed, so this can run before literally
	// anything else (uart/hid/mem init below), the earliest the OS
	// can put anything on screen. Stays up until something else
	// writes over it -- normally wm's own startup clear_screen()
	// call, whenever the user eventually runs wm; nothing here
	// coordinates that handoff explicitly, it's just whichever writes
	// to VRAM last. Flip to true if it displays with foreground/
	// background swapped on real hardware -- see logo.h's own comment.
	z_boot_logo_show(false);

	kprint("\nZEITLOS\n");

	// init uart
	z_uart_init();
	printf(" - uart initialized.\n");

	// init usb hid keyboard event queue
	z_hid_init();
	printf(" - hid initialized.\n");

	// init memory management
	k_mem_init();
	printf(" - memory initialized.\n");

	// set all processes as available
	for (int p = 0; p < Z_PROCS_MAX; p++) {
		z_procs[p].base = 0x00000000;
		z_procs[p].flags = 0x00000000;
	}

	// zero the pid name registry -- see k_pidreg_init()'s comment in
	// pidreg.h for why this can't just be left to .bss (short
	// version: it can't be trusted to start zero on this hardware,
	// same reason z_procs[] above is zeroed explicitly too, and this
	// one -- unlike z_procs[] -- didn't get that treatment the first
	// time around, which broke real hardware almost immediately).
	// must happen before ANY process can possibly reach
	// k_pid_register()/k_pid_lookup() -- right here, this early, is
	// the only place that's actually guaranteed.
	k_pidreg_init();

	// create process zero (this process):
	uint32_t k_size = k_mem_align_up((((uint32_t)&_end - (uint32_t)&_start) +
		Z_KERNEL_STACK_SIZE), Z_MEM_ALIGNMENT);
	printf(" - kernel process size %ld\n", k_size);


	// set the kernel stack pointer
	__asm__ volatile (
		"mv sp, %0"
		:
		: "r" (0x40000000 + k_size)
	);

	// call some function ...

	k_proc_create((uint32_t)&_end - (uint32_t)&_start);
	k_proc_start(0);

	// set the kernel register so the irq handler knows who to call
	reg_kernel = (uint32_t)(uintptr_t)z_kernel_entry;
	printf(" - kernel active.\n");

//	while (1) {
//		if ((z_kernel_ticks % 100) == 0) z_kernel_dump();
//	};

	printf(" - starting shell.\n");

	// the kernel shell is process zero
	sh();

}

// - a pointer to the kernel entry function can be found at 0x0000000c
//   (if the kernel started)
// - this is called by the BIOS interrupt handler which uses the interrupt stack
// - it can also be called by apps to make system calls

uint32_t *z_kernel_entry(uint32_t syscall_id, uint32_t *regs, uint32_t irqs) {

	if (syscall_id != Z_SYSCALL_NONE) {

		// A syscall (see docs/app_runtime.md, "The syscall trampoline")
		// is a plain jalr straight from the CALLING APP's own compiled
		// code into this function -- not a hardware trap, so nothing
		// automatically fixes up gp the way sw/bios/boot_picorv32.S's
		// irq_vec does for real interrupts (it saves/restores all 32
		// GPRs, gp included, around every hardware IRQ -- which is
		// exactly why preemptive scheduling has always worked fine:
		// each process's own gp correctly survives being preempted
		// and resumed). Kernel code reached via a syscall instead
		// executes with whatever gp the calling app had -- wrong for
		// z_syscall_table[] itself (two lines down) and for ANY
		// small-enough kernel global any syscall handler goes on to
		// touch (this is what broke mem.c's mem_block_count the
		// moment Z_SYS_PROC_RUN gave an app a way to reach
		// k_mem_alloc() for the first time -- see that fix's own
		// comment in mem.c). Tagging individual kernel globals
		// __attribute__((section(".bss"))) (kernel.c's own
		// z_pid/z_procs[]/z_kernel_ticks, mem.c's block_list/
		// mem_block_count) is NOT a reliable fix by itself: the
		// linker's relaxation pass decides gp-relative vs. absolute
		// addressing by a symbol's FINAL LINKED ADDRESS being within
		// reach of gp, not which section it's tagged into -- a
		// .bss-tagged symbol placed early enough (right after
		// .sdata/.sbss, exactly where gp points) can still get
		// relaxed, and where exactly it lands depends on overall
		// build layout, not anything the tag controls. The actual
		// fix: make gp correct -- the kernel's own -- for the entire
		// duration ANY kernel code runs via this path, done right
		// here, before touching anything else, and restored to the
		// caller's own value before returning, so the app's own
		// gp-relative addressing is undisturbed once control goes
		// back to it. (This makes the .bss tags mentioned above
		// redundant going forward, but they're left in place --
		// harmless, and exactly what the rest of this codebase
		// already does for this situation elsewhere.)
		uint32_t caller_gp;
		__asm__ volatile ("mv %0, gp" : "=r"(caller_gp));
		__asm__ volatile ("mv gp, %0" ::
			"r"((uint32_t)(uintptr_t)&__global_pointer$) : "memory");

		uint32_t *ret;

		if (syscall_id >= Z_SYSCALL_COUNT || !z_syscall_table[syscall_id]) {
			ret = (uint32_t *)&z_fail;
		} else {
			ret = (uint32_t *)z_syscall_table[syscall_id]((z_obj_t *)regs);
		}

		__asm__ volatile ("mv gp, %0" :: "r"(caller_gp) : "memory");

		return ret;

	}

	// not a system call; must be an interrupt

	// only the KTIMER IRQ should advance the tick counter -- it was
	// previously incremented for ANY interrupt (including UART RX/TX,
	// which fires far more often, especially under heavy printf
	// activity from multiple processes), inflating z_kernel_ticks
	// well beyond real elapsed time. this made every tick-based
	// timeout (z_msg_wait_timeout(), tftp.c's retry timer) fire much
	// sooner than intended.
	if ((irqs & (1 << Z_IRQ_KTIMER)) != 0) {
		++z_kernel_ticks;
	}

	// handle interrupts
	if ((irqs & (1 << Z_IRQ_UART)) != 0) {
		z_uart_irq();
	}

	if ((irqs & (1 << Z_IRQ_HID)) != 0) {
		z_hid_irq0();
	}

	if ((irqs & (1 << Z_IRQ_HID1)) != 0) {
		z_hid_irq1();
	}

	// swap process on KTIMER interrupt
	if ((irqs & (1 << Z_IRQ_KTIMER)) != 0) {

		// don't switch if there's only one process
		if (k_proc_active_count() < 2) return regs;

		// save current process registers
  		for (int i = 0; i < 32; i++) {
			z_procs[z_pid].regs[i] = *(regs + i);
		}

		// find next active process (round-robin scheduling)
		next_process:
		z_pid++;
		if (z_pid >= Z_PROCS_MAX) z_pid = 0;

		if ((z_procs[z_pid].flags & Z_PROC_FLAG_DIE) == Z_PROC_FLAG_DIE) {
			// free the memory
			k_mem_free((void *)z_procs[z_pid].base);
			// release any names this process registered (see
			// pidreg.h -- without this, a later, unrelated process
			// reusing this same pid slot would inherit stale name
			// registrations that were never its own)
			k_pidreg_release_all(z_pid);
			// kill the process
			z_procs[z_pid].base = 0x00000000;
			z_procs[z_pid].flags = 0x00000000;
			z_pid++;
			if (z_pid >= Z_PROCS_MAX) z_pid = 0;
		}

		if ((z_procs[z_pid].flags & Z_PROC_FLAG_ACTIVE) != Z_PROC_FLAG_ACTIVE)
			goto next_process;

		// configure address translation
		reg_mtu = z_procs[z_pid].base;

		// return the registers
		return z_procs[z_pid].regs;
	}

	return regs;

}

uint32_t k_proc_active_count(void) {

	uint32_t count = 0;

	for (int i = 0; i < Z_PROCS_MAX; i++)
		if ((z_procs[i].flags & Z_PROC_FLAG_ACTIVE) == Z_PROC_FLAG_ACTIVE)
			count++;

	return(count);

}

// return process id or 0 on fail
uint32_t k_proc_create(uint32_t size) {

	uint32_t mem_size = k_mem_align_up(size + Z_PROC_STACK_SIZE,
		Z_MEM_ALIGNMENT);

	// find first available process slot
	for (int p = 0; p < Z_PROCS_MAX; p++) {

		if (z_procs[p].base != 0x00000000) continue;

		void *mem = k_mem_alloc(mem_size);
		if (!mem) return(0);	// NOT Z_FAIL (1) -- this function's
					// return convention is "0 = no pid
					// assigned", same as the plain
					// `return(0);` at the end of this
					// function for "no free slot" below,
					// NOT the z_rv Z_OK/Z_FAIL convention.
					// Z_FAIL is 1 (zmsg.h) -- a real,
					// valid pid a caller could otherwise
					// legitimately get back on success,
					// so returning it here on failure was
					// indistinguishable from successfully
					// creating a process AT THAT EXACT PID.
					// Every caller (sh.c, k_proc_run() in
					// this file) checks `if (!pid)`/
					// `if (pid)` to tell success from
					// failure -- Z_FAIL (1) is truthy, so
					// this was read as "created, pid 1"
					// rather than "failed". Caught when it
					// corrupted pid 1's (wm's own, see
					// Z_PID_WM in zwm.h) live memory: the
					// caller went on to call
					// k_proc_base(1)/fs_load(that base,
					// ...)/k_proc_start(1) as if pid 1 were
					// the process just created, overwriting
					// wm's own running memory with
					// whatever app was actually being
					// launched.
		uint32_t base = (int32_t)(uintptr_t)mem;
		z_procs[p].base = base;
		z_procs[p].size = mem_size;
		for (int i = 0; i < 32; i++) {
			z_procs[p].regs[i] = 0x00000000;
		}

		if (p == 0) {
			z_procs[p].regs[0] = 0x40000000;	// pc
			z_procs[p].regs[2] = 0x40000000 + mem_size;	// sp
		} else {
			z_procs[p].regs[0] = 0x80000000;	// pc
			z_procs[p].regs[2] = 0x80000000 + mem_size - 4;	// sp
			// writes the initial return address onto the NEW
			// process's own stack -- via its PHYSICAL address
			// (base + ...), not the 0x8000_0000 virtual window
			// used above for regs[2]/regs[0]. This is deliberate,
			// not a style inconsistency: those two are values that
			// become the PC/SP *once this process is actually
			// scheduled and reg_mtu is switched to `base`* (see
			// z_kernel_entry()'s KTIMER handler, the only place
			// reg_mtu is ever written) -- correct as virtual
			// addresses for that future moment. This write happens
			// RIGHT NOW, before the process has been started
			// (k_proc_start() hasn't been called yet), while
			// reg_mtu still reflects whichever process is currently
			// executing THIS CALL -- sh.c (pid 0) for a plain `run`
			// command, but any process for k_proc_run() (see
			// zeitlos.h), including wm itself via the dock (see
			// docs/window_manager.md). A virtual-address write here
			// would land in the CALLER's own memory, translated
			// through the caller's base -- not this new process's.
			// Previously written as `0x80000000 + mem_size - 4]`
			// (the same virtual form as regs[2]): harmless-looking
			// from sh.c, since pid 0's own memory region rarely
			// collides with anything that mattered, but a real,
			// immediate memory-corruption bug the moment a *live*
			// process (with active heap/stack of its own right
			// where the stray write landed) calls this -- which is
			// exactly what launching an app from wm's dock does.
			// `base` is already the correct physical address for
			// process p (computed just above), so this needs no
			// translation at all.
			*((uint32_t *)(base + mem_size - 4)) = z_procs[p].regs[1];	// sp = ra
		}

		return(p);

	}

	return(0);
}

z_rv k_proc_start(uint32_t pid) {
	z_procs[pid].flags |= Z_PROC_FLAG_ACTIVE;
}

z_rv k_proc_stop(uint32_t pid) {
	z_procs[pid].flags &= ~Z_PROC_FLAG_ACTIVE;
}

z_rv k_proc_kill(uint32_t pid) {
	z_procs[pid].flags |= Z_PROC_FLAG_DIE;
	return Z_OK;
}

uint32_t k_proc_base(uint32_t pid) {
	return z_procs[pid].base;
}

z_rv k_kernel_dump(void) {
	kprint(" kticks: ");
	kprint_hex32(z_kernel_ticks);
	kprint("\n");
	return Z_OK;
}

z_rv k_proc_dump(void) {
	for (int i = 0; i < Z_PROCS_MAX; i++) {
		if (!z_procs[i].base) continue;
		printf(" pid: %2i base: %.8lx size: %.8lx pc %.8lx sp: %.8lx flags: %.8lx\n",
			i, z_procs[i].base, z_procs[i].size,
			z_procs[i].regs[0], z_procs[i].regs[2], z_procs[i].flags);
	}
	return Z_OK;
}

// --

void kprint(const char *s) {
    while (*s) {
        if (*s == '\n') {
            while ((reg_uart0_lsr & 0x20) == 0);
            reg_uart0_data = '\r';
        }
        while ((reg_uart0_lsr & 0x20) == 0);
        reg_uart0_data = *s++;
    }
}

void kprint_hex_digit(uint8_t val) {
    if (val < 10) {
        reg_uart0_data = '0' + val;
    } else {
        reg_uart0_data = 'A' + (val - 10);
    }
    for (volatile uint32_t i = 0; i < 500; i++);
}

void kprint_hex32(uint32_t val) {
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        kprint_hex_digit(nibble);
    }
}

// --

z_obj_t *z_exit(z_obj_t *obj) {
	uint32_t pid = z_pid;
	k_proc_kill(pid);
	while (1) /* wait to die */;
}

