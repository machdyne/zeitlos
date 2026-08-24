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
#include "fsapi.h"
#include "../common/zsoc.h"

// Z_PROCS_MAX now lives in kernel.h (msg.c needs it too)
//
// Real-hardware finding, in two parts (see kernel.h's
// Z_PROC_STACK_SIZE_DEFAULT/_LARGE for where this landed): a single
// blanket 8KB stack+heap allowance per process (there's no separate
// heap region at all -- zeitlos.c's own _sbrk() grows the C heap
// upward from a process's own `_end` but bounds it against the
// CURRENT STACK POINTER, so the real call stack and the C heap share
// this one region for the process's entire lifetime, nothing ever
// handed back) was NOT enough for `repl` specifically -- Scheme
// stdlib loading plus zport.h's own per-connection z_obj_blob() leak
// (zport.c's own comment) exhausted it. Raised to 64KB (Z_PROC_
// STACK_SIZE_LARGE) for `repl`, first as a single blanket constant
// for every process -- which then turned out to be too generous on
// the smallest supported board (Obst's 1MB variant, `MEM 1` in
// rtl/boards.vh): paying 64KB per process for `kernel`+`wm`+`net`+
// `repl` left no room to also run `term`. Now per-process
// (Z_PROC_STACK_SIZE_DEFAULT, 16KB, for everything that isn't
// `repl` -- see sh.c's own `run`/`init` call sites for where that
// choice is made) -- see kernel.h for the full reasoning either way.
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
z_obj_t *k_proc_kill_syscall(z_obj_t *args);	// ditto -- named _syscall, not
									// k_proc_kill, since that name is already
									// taken by the existing z_rv k_proc_kill
									// (uint32_t) below (used directly by sh.c's
									// `kill` command, and now by this syscall
									// handler too) -- same k_/z_ naming-collision
									// reasoning as k_proc_run()'s own comment
									// just above.

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

	// see kernel.h's z_proc_stack_size_for() comment -- the same
	// shared decision sh.c's own `run`/`init` use, so launching
	// `repl`/`net` via wm's dock (this syscall's own motivating case)
	// gets the same stack+heap allowance either one needs regardless
	// of which path started it.
	uint32_t stack_size = z_proc_stack_size_for(name);

	if (size) {
		pid = k_proc_create(size, stack_size);
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

	// init memory management -- pool size comes from the SOC
	// capability CSRs (rtl/csrs.v, sw/common/zsoc.h, docs/csrs.md)
	// when available, so this board's REAL amount of main RAM gets
	// used (Lakritz/mozart_ml1: 32MB, some boards more) instead of
	// the Obst-only 1MB this used to hardcode unconditionally. Falls
	// back to Z_MEM_SIZE_DEFAULT (mem.h) on a bitstream that predates
	// rtl/csrs.v entirely -- z_soc_mem_mb() itself already returns 0
	// in that case (z_soc_csrs_present() is false), so this check
	// doesn't need to duplicate that logic, just decide what to do
	// with a 0.
	uint32_t mem_mb = z_soc_mem_mb();
	uint32_t mem_total = mem_mb ? (mem_mb * 1024 * 1024) : Z_MEM_SIZE_DEFAULT;
	printf(" - main memory: %ldMB%s\n", (long)(mem_total / (1024 * 1024)),
		mem_mb ? "" : " (CSRs not present -- assumed default)");
	k_mem_init(mem_total);
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

	k_proc_create((uint32_t)&_end - (uint32_t)&_start, Z_PROC_STACK_SIZE_DEFAULT);
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

	// gp must be correct -- the kernel's own -- for the ENTIRE
	// duration this function (and everything it calls) executes, no
	// matter which of the two ways it got here: a syscall (a plain
	// jalr straight from the calling app's own code, see
	// docs/app_runtime.md) or a real hardware interrupt, routed
	// through sw/bios/boot_picorv32.S's irq_vec. That assembly sets
	// up a correct, fixed sp for this handler, but only ever SAVES
	// gp (`sw x3, 3*4(x1)`) -- it never assigns gp a new value before
	// calling in. Either way, without this fixup, kernel code here
	// runs with whatever gp the interrupted/calling process happened
	// to have -- wrong for z_syscall_table[] and any small-enough
	// kernel global any handler goes on to touch.
	//
	// Previously this fixup was scoped to the syscall branch only,
	// on the reasoning that the interrupt path's own full
	// 32-register save/restore already handled gp correctly -- true
	// for PRESERVING each process's own gp across being interrupted
	// and resumed later, but NOT the same as gp being correct DURING
	// this function's own C code execution in between. That gap is
	// exactly what mem.c's allocator globals hit the first time
	// `kill <pid>` (sh.c) actually ran a process's death cleanup
	// (k_mem_free(), below) through the KTIMER branch -- same
	// mechanism as the syscall-side bug already fixed here, different
	// path, previously unprotected.
	//
	// Restored to the caller's/interrupted-process's own value before
	// every return below (all of them go through the `done` label),
	// so nothing about its own gp-relative addressing is disturbed
	// once control goes back to it.
	uint32_t saved_gp;
	__asm__ volatile ("mv %0, gp" : "=r"(saved_gp));
	__asm__ volatile ("mv gp, %0" ::
		"r"((uint32_t)(uintptr_t)&__global_pointer$) : "memory");

	uint32_t *ret;

	if (syscall_id != Z_SYSCALL_NONE) {

		if (syscall_id >= Z_SYSCALL_COUNT || !z_syscall_table[syscall_id]) {
			ret = (uint32_t *)&z_fail;
		} else {
			ret = (uint32_t *)z_syscall_table[syscall_id]((z_obj_t *)regs);
		}

		goto done;

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
		if (k_proc_active_count() < 2) { ret = regs; goto done; }

		// save current process registers
  		for (int i = 0; i < 32; i++) {
			z_procs[z_pid].regs[i] = *(regs + i);
		}

		// find next active process (round-robin scheduling)
		next_process:
		z_pid++;
		if (z_pid >= Z_PROCS_MAX) z_pid = 0;

		if ((z_procs[z_pid].flags & Z_PROC_FLAG_DIE) == Z_PROC_FLAG_DIE) {
			// NOTE: this whole branch runs inside the KTIMER
			// interrupt handler itself (this function's interrupt
			// path, not the syscall path) -- picorv32's interrupt
			// model here doesn't nest, so nothing on this path can
			// safely call printf()/anything that waits on another
			// interrupt to make progress. uart.c's own _write()
			// documents exactly this hazard: `while (k_uart_tx_full())
			// /* wait */;` has no timeout, and the TX fifo is only
			// ever drained by the UART TX interrupt -- which can
			// never fire while we're already inside THIS interrupt
			// handler. A printf() briefly lived right here (paired
			// with one in k_proc_kill() below, which runs via the
			// syscall path instead and is fine) -- it caused a
			// genuine, total hang the moment it landed at a point
			// where the TX fifo happened to already be full (far more
			// likely right after a burst of unrelated output, e.g.
			// telnet's own connect-sequence prints), with nothing
			// able to recover it since even the scheduler itself
			// never gets to run again. Removed; k_proc_kill()'s own
			// print (this file) still shows every DIE request as it
			// happens, which is what actually matters for the
			// investigation this was added for -- exactly when the
			// resulting free below actually runs is a fixed, short
			// delay after that (at most one full round-robin cycle),
			// not additional information worth this risk to observe
			// directly.
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
			goto next_process;
		}

		if ((z_procs[z_pid].flags & Z_PROC_FLAG_ACTIVE) != Z_PROC_FLAG_ACTIVE)
			goto next_process;

		// configure address translation
		reg_mtu = z_procs[z_pid].base;

		// return the registers
		ret = (uint32_t *)z_procs[z_pid].regs;
		goto done;
	}

	ret = regs;

	done:
	__asm__ volatile ("mv gp, %0" :: "r"(saved_gp) : "memory");
	return ret;

}

uint32_t k_proc_active_count(void) {

	uint32_t count = 0;

	for (int i = 0; i < Z_PROCS_MAX; i++)
		if ((z_procs[i].flags & Z_PROC_FLAG_ACTIVE) == Z_PROC_FLAG_ACTIVE)
			count++;

	return(count);

}

// return process id or 0 on fail. `stack_size` is the per-process
// stack+heap allowance -- see kernel.h's Z_PROC_STACK_SIZE_DEFAULT/
// _LARGE comment for which one a given caller should pass.
uint32_t k_proc_create(uint32_t size, uint32_t stack_size) {

	uint32_t mem_size = k_mem_align_up(size + stack_size,
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
	if (pid >= Z_PROCS_MAX) return Z_FAIL;
	// diagnostic: this flag is the ONLY mechanism in this kernel that
	// can free a running process's memory block mid-session (the
	// scheduler's own death-cleanup below actually does the free, one
	// full round-robin cycle later -- see that code's own new
	// diagnostic print). Logging the call itself, here, catches WHO
	// asked for this and WHEN, which the cleanup-side print alone
	// can't show (by the time cleanup runs, the caller's own stack
	// frame -- and any context about why -- is long gone). Added
	// while investigating a real-hardware symptom: a running process
	// (net) reappearing under a new pidreg name with no visible `run`/
	// `creating process` message, consistent with its memory being
	// freed and immediately reused rather than a fresh k_proc_create()
	// -- this pins down whether k_proc_kill() is actually involved at
	// all, and if so, from where (z_proc_kill_syscall()'s only current
	// caller is wm.c's handle_close_click(), which prints its own
	// diagnostic before calling this -- if THIS print appears without
	// that one, something else is calling k_proc_kill() directly,
	// which as of this commit should only be sh.c's `kill` command).
	printf("k_proc_kill: pid %ld marked to die (base=%08lx)\n",
		(long)pid, (long)z_procs[pid].base);
	z_procs[pid].flags |= Z_PROC_FLAG_DIE;
	return Z_OK;
}

// syscall wrapper around k_proc_kill() above -- see Z_SYS_PROC_KILL
// (syscalls.def) and z_proc_kill() (zeitlos.h/.c) for the userland
// side. Added for sw/apps/wm's Z_WIN_FLAG_CLOSE_KILLS_OWNER
// (docs/window_manager.md): before this existed, only kernel-space
// code (sh.c's `kill` command, compiled directly into kernel.bin)
// could kill an arbitrary process -- same gap k_proc_run() closed for
// STARTING one, for the same reason (wm needing to reach a kernel
// facility no syscall exposed yet).
//
// No ownership/permission check -- any process can kill any other by
// pid, same "apps are fully trusted" model the rest of this kernel
// already runs on (docs/window_manager.md's own "apps are trusted"
// note). args->val.uint32 is the target pid; result convention
// matches k_proc_run() just above (z_ok/z_fail only, nothing written
// back into args -- there's no "new pid" equivalent to report here).
z_obj_t *k_proc_kill_syscall(z_obj_t *args) {
	if (!args || args->type != Z_UINT32) return (&z_fail);
	return (k_proc_kill(args->val.uint32) == Z_OK) ? (&z_ok) : (&z_fail);
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

