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
#include "procapi.h"	// k_proc_list(), referenced by the syscall
						// table built from syscalls.def below
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

// Prototyped here rather than with the other process helpers below,
// because the syscall table immediately after this line references it
// and syscalls.def is expanded at that point.
z_obj_t *k_proc_wait(z_obj_t *args);

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
	// ZEXE-aware, same as sh.c's `run` -- image size is data + bss,
	// which is not the file size for the new format (sw/common/zexec.h).
	z_exec_info_t xi;
	// _any: filesystem first, flash core-app archive underneath (fs.c).
	// This is what lets wm's dock launch `term` on a board with no SD
	// card, and what lets a killed core app be restarted.
	uint32_t size = fs_exec_info_any(name, &xi) ? 0 : xi.total;

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
			fs_load_exec_any(base, name, &xi);
			k_proc_start(pid);
		}
	}

	args->type = Z_UINT32;
	args->val.uint32 = pid;

	return pid ? (&z_ok) : (&z_fail);

}

// --



// -- SOC feature inventory --
//
// rtl/csrs.v exposes a bitmap of what was actually synthesized into the
// running bitstream (sw/common/zsoc.h's Z_FEATURE_* bits). Printing it
// at boot turns a whole class of confusing bring-up failure into a
// glance at the log: "the network doesn't work" on a board whose
// bitstream simply has no ethernet PHY looks identical, from software,
// to a driver bug -- until the boot log says which one it is.
//
// Grouped rather than dumped as a flat list or a hex word: the groups
// are how someone actually reasons about a board ("does this one have a
// GPU? does it have storage?"), and a raw 0x000c53f7 helps nobody.
//
// The bit/name/group table itself lives in sw/common/zsoc.c, next to
// the Z_FEATURE_* defines it mirrors, so everything that has to track
// rtl/sysctl.v's CSR_FEATURES is in one directory. This function owns
// only the layout.
static void k_soc_report(void) {

	if (!z_soc_csrs_present()) {
		// An older bitstream has nothing mapped at 0x7000_0000 at all.
		// Say "unknown" rather than printing an empty feature list --
		// see z_soc_has_feature()'s own comment in zsoc.h on why
		// "can't confirm" is a genuinely different answer from "no".
		printf(" - soc: features unknown (bitstream predates rtl/csrs.v)\n");
		return;
	}

	printf(" - soc features:\n");

	int n = z_soc_features_count;
	int cur = -1;
	bool any_on_line = false;

	for (int i = 0; i < n; i++) {

		if (!z_soc_has_feature(z_soc_features[i].bit)) continue;

		if (z_soc_features[i].group != cur) {
			if (any_on_line) printf("\n");
			printf("     %s ", z_soc_feature_groups[z_soc_features[i].group]);
			cur = z_soc_features[i].group;
			any_on_line = true;
		}

		printf("%s ", z_soc_features[i].name);

	}

	if (any_on_line) printf("\n");
	else printf("     (none reported)\n");

	// Gateware/software agreement. If this binary was built for rv32im
	// but the bitstream has no multiplier, every mul is an illegal
	// instruction -- which on this SOC is not a clean trap but IRQ 1,
	// which nothing handles, so the machine would spin somewhere that
	// looks unrelated. Say so here instead. See zsoc.h's own
	// z_soc_check_cpu_arch() comment for the full failure mode.
	printf("     build   %s\n", z_soc_build_arch());

	if (!z_soc_check_cpu_arch()) {
		printf("\n");
		printf(" *** CPU MISMATCH ***\n");
		printf(" this kernel is built for %s but the bitstream\n",
			z_soc_build_arch());
		printf(" has no hardware multiply/divide. every mul/div\n");
		printf(" will be an illegal instruction.\n");
		printf(" rebuild the gateware (rtl/boards.vh: CPU_MUL,\n");
		printf(" CPU_DIV) or the software (sw/common/arch.mk:\n");
		printf(" ARCH=rv32i), and flash both together.\n");
		printf("\n");
	}

}


// -- CPU speed report --
//
// picorv32 is instantiated with ENABLE_COUNTERS/ENABLE_COUNTERS64 at
// their defaults of 1 (rtl/sysctl.v overrides neither), so rdcycle and
// rdinstret are real, free-running hardware counters. Only the low 32
// bits are read: the benchmark window below is ~50ms, which at any
// plausible clock is a few million counts, nowhere near a wrap.
static inline uint32_t rd_cycle(void) {
	uint32_t v; __asm__ volatile ("rdcycle %0" : "=r"(v)); return v;
}
static inline uint32_t rd_instret(void) {
	uint32_t v; __asm__ volatile ("rdinstret %0" : "=r"(v)); return v;
}

// how long to measure for, in KTIMER ticks (~732Hz, so ~50ms). Long
// enough that the tick quantisation (one tick = ~1.4ms, so ~2.7% at
// this window) doesn't dominate, short enough to be invisible in the
// boot.
#define CPU_BENCH_TICKS ((Z_TICK_HZ * 50) / 1000)	// ~50ms

// Measures and prints the CPU's instruction rate.
//
// The clock is NOT measured -- it is Z_SYSCLK_HZ, a stated constant
// (sw/common/zsoc.h, which explains why measuring it is impossible on
// this SOC: the KTIMER and rdcycle share sys_clk, so cycles-per-tick is
// always exactly 65536 no matter what the PLL is actually doing). An
// earlier version of this function derived "MHz" from those two and
// printed a number that would have read ~48 on a board clocked at 24.
//
// So MIPS comes from the two hardware counters and the stated clock --
// di/dc is the real, measured part, Z_SYSCLK_HZ scales it -- rather
// than from elapsed wall time, which would have inherited the same
// assumption twice over. IPC (di/dc alone) is the one figure here that
// depends on no assumption at all.
//
// MIPS is measured over a deliberately plain integer loop. Worth being
// honest about what that means: rdinstret counts instructions retired
// whatever they are, so a figure measured while polling a UART register
// would mostly report Wishbone stalls, not compute. This loop touches
// no peripherals, so what comes out is a compute-bound best case, not
// an average over real work. IPC alongside it makes the CPI visible
// (picorv32 is a multi-cycle design, so expect well under 1).
//
// Must be called AFTER reg_kernel is set: z_kernel_ticks only advances
// once the IRQ handler is installed and KTIMER is firing. The cycle
// counter is independent of that, so it doubles as an escape hatch --
// if ticks never advance, this gives up and says so rather than
// spinning forever and hanging the boot.
static void k_cpu_report(void) {

	uint32_t guard = rd_cycle();
	uint32_t t0 = z_kernel_ticks;

	while (z_kernel_ticks == t0) {
		// ~4s at any sane clock -- see this function's own comment
		if (rd_cycle() - guard > 200000000u) {
			printf(" - cpu: ktimer not running, skipping speed check\n");
			return;
		}
	}

	volatile uint32_t sink = 0;
	uint32_t x = 12345;

	t0 = z_kernel_ticks;
	uint32_t i0 = rd_instret();
	uint32_t c0 = rd_cycle();

	while (z_kernel_ticks - t0 < CPU_BENCH_TICKS) {
		// Plain integer work: shifts, adds and xors only, no memory
		// beyond the loop itself and deliberately no multiply.
		//
		// The no-multiply part predates rv32im (rtl/boards.vh's
		// `CPU_MUL) and is now a deliberate choice rather than a
		// limitation: keeping this loop identical across builds is
		// what makes the number comparable over time. It does mean
		// this figure is blind to hardware multiply -- it barely
		// moved when MUL was enabled, because there is nothing here
		// for MUL to do. Use the `bench` shell command (sw/os/sh.c)
		// to measure mul/div/memory separately.
		for (int i = 0; i < 64; i++) {
			x += i;
			x ^= x >> 7;
			x += x << 3;
		}
		sink = x;
	}

	uint32_t di = rd_instret() - i0;
	uint32_t dc = rd_cycle() - c0;
	(void)sink;

	if (!dc) return;

	// Integer math throughout -- no float in kernel code.
	//
	// MIPS x100 = (di / dc) * (Z_SYSCLK_HZ / 1e6) * 100, rearranged to
	// divide FIRST so nothing overflows: di * 4800 would be ~1.2e10 at
	// this window size, well past 32 bits. Dividing dc by the scale
	// factor instead costs ~0.02% precision and stays in range.
	uint32_t scale = (Z_SYSCLK_HZ / 1000000u) * 100u;	// 4800 at 48MHz
	uint32_t denom = dc / scale;
	if (!denom) return;

	uint32_t mips_x100 = di / denom;
	uint32_t ipc_x100 = (di / 100) * 10000 / (dc / 100) / 100;

	printf(" - cpu: %ld.%02ld MIPS @ %ld MHz (%ld.%02ld IPC)\n",
		(long)(mips_x100 / 100), (long)(mips_x100 % 100),
		(long)(Z_SYSCLK_HZ / 1000000u),
		(long)(ipc_x100 / 100), (long)(ipc_x100 % 100));

}


int main(void) {

	// boot splash -- the image lives in flash (see logo.h) and is
	// copied straight to VRAM, so this costs no main memory at all.
	// VRAM is plain memory-mapped hardware with no
	// init of its own needed, so this can run before literally
	// anything else (uart/hid/mem init below), the earliest the OS
	// can put anything on screen. Stays up until something else
	// writes over it -- normally wm's own startup clear_screen()
	// call, whenever the user eventually runs wm; nothing here
	// coordinates that handoff explicitly, it's just whichever writes
	// to VRAM last. If it ever displays with foreground/background
	// swapped, regenerate the flashed image with pad_logo.py --invert
	// rather than changing anything here -- see logo.h's own comment.
	z_boot_logo_show();

	kprint("\nZEITLOS\n");

	// init uart
	z_uart_init();
	printf(" - uart initialized.\n");

	// straight after uart, so the hardware inventory is the first thing
	// in the log -- CSRs are plain memory-mapped registers needing no
	// init of their own, so this can run as early as there is somewhere
	// to print to.
	k_soc_report();

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

	// now that KTIMER is actually firing (z_kernel_ticks only advances
	// once reg_kernel above is set), the CPU speed check can run --
	// see k_cpu_report()'s own comment for what the numbers mean.
	k_cpu_report();

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
	int sched_scanned;

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

		// Wake anything whose timeout has expired, BEFORE counting
		// runnable processes or picking the next one -- otherwise a
		// process whose sleep just elapsed would wait another full
		// round before being noticed.
		//
		// wake_tick 0 means "no timeout, wait indefinitely"; such a
		// process is woken only by k_msg_send(). The comparison is
		// written as a subtraction so it stays correct across the
		// 32-bit wrap of z_kernel_ticks (~68 days at 732Hz): a plain
		// `ticks >= wake_tick` would fail for a sleep that straddles
		// the wrap and hang that process for another full period.
		for (int i = 0; i < Z_PROCS_MAX; i++) {
			if ((z_procs[i].flags & Z_PROC_FLAG_BLOCKED) == 0) continue;
			if (z_procs[i].wake_tick == 0) continue;
			if ((int32_t)(z_kernel_ticks - z_procs[i].wake_tick) > 0)
				k_proc_unblock(i);
		}

		// don't switch if there's at most one process that could run.
		// Deliberately runnable, not active: if wm/net/repl are all
		// blocked on their mailboxes, the one process with work to do
		// keeps the CPU instead of round-robining through three
		// processes that would each immediately block again.
		if (k_proc_runnable_count() < 2) { ret = regs; goto done; }

		// save current process registers
  		for (int i = 0; i < 32; i++) {
			z_procs[z_pid].regs[i] = *(regs + i);
		}

		// Bounded scan. Before BLOCKED existed, this loop was
		// guaranteed to terminate because the current process was
		// itself active and would be reached again. That is no longer
		// true -- every process can now be unschedulable at once -- and
		// an unbounded scan here would spin forever INSIDE the timer
		// interrupt handler, which is unrecoverable. The count is the
		// safety net; the k_proc_runnable_count() check above means it
		// should never actually be hit.
		sched_scanned = 0;

		// find next runnable process (round-robin scheduling)
		next_process:
		if (++sched_scanned > Z_PROCS_MAX) { ret = regs; goto done; }
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

		// skips both inactive and blocked slots -- see
		// Z_PROC_RUNNABLE()/Z_PROC_FLAG_BLOCKED in kernel.h
		if (!Z_PROC_RUNNABLE(z_procs[z_pid]))
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

// Make a blocked process schedulable again. Safe to call on a process
// that isn't blocked (does nothing), which is what lets k_msg_send()
// call it unconditionally on every delivery.
void k_proc_unblock(uint32_t pid) {
	if (pid >= Z_PROCS_MAX) return;
	z_procs[pid].flags &= ~Z_PROC_FLAG_BLOCKED;
	z_procs[pid].wake_tick = 0;
}

// -- k_proc_wait syscall --
//
// "Block me until a message arrives, or until `timeout` ticks have
// passed." A timeout of 0 means wait indefinitely.
//
// THE RACE THIS AVOIDS is the whole reason this is a syscall rather
// than two: check the mailbox, find it empty, then set BLOCKED. If a
// message could arrive between those two steps, the sender would
// unblock a process that isn't blocked yet, and the process would then
// mark itself blocked and sleep forever holding a message it never
// noticed -- a hang that depends on exact timing and would be
// miserable to reproduce.
//
// It is safe here because both halves happen inside one syscall.
// picorv32's interrupt model doesn't nest, and no other process can
// run until this handler returns, so nothing can deliver a message in
// between. Do NOT split this into a "peek" and a separate "block".
//
// Returns Z_OK if the caller is now blocked, Z_FAIL if a message was
// already waiting and it should just carry on reading.
//
// Note this does not switch away immediately -- the caller keeps
// whatever remains of its current timeslice and spins in the
// z_msg_wait() loop until the next KTIMER tick, which then skips it.
// So at most one partial timeslice is wasted per block, once, rather
// than every timeslice forever. Yielding on the spot would need the
// syscall path to do the full save/switch dance the KTIMER path does;
// that's a worthwhile follow-up, not a correctness issue.
z_obj_t *k_proc_wait(z_obj_t *args) {

	uint32_t timeout = (args->type == Z_UINT32) ? args->val.uint32 : 0;

	// something already waiting -- don't block, let the caller read it
	if (!z_mailbox_empty(z_pid))
		return (&z_fail);

	// wake_tick 0 is the sentinel for "indefinite", so a timeout that
	// happens to land exactly on tick 0 is nudged to 1. At 732Hz that
	// is a 1.4ms error once every ~68 days.
	if (timeout) {
		uint32_t w = z_kernel_ticks + timeout;
		z_procs[z_pid].wake_tick = w ? w : 1;
	} else {
		z_procs[z_pid].wake_tick = 0;
	}

	z_procs[z_pid].flags |= Z_PROC_FLAG_BLOCKED;

	return (&z_ok);

}

// Processes that could actually be given a timeslice right now, as
// opposed to k_proc_active_count()'s "processes that exist".
uint32_t k_proc_runnable_count(void) {

	uint32_t count = 0;

	for (int i = 0; i < Z_PROCS_MAX; i++)
		if (Z_PROC_RUNNABLE(z_procs[i]))
			count++;

	return(count);

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
		// state is derived from flags rather than printed as another
		// number: which processes are actually schedulable is the whole
		// point of Z_PROC_FLAG_BLOCKED, and reading it out of a hex
		// bitmask at a serial console is needless work.
		const char *state = "run";
		if (z_procs[i].flags & Z_PROC_FLAG_DIE) state = "die";
		else if (z_procs[i].flags & Z_PROC_FLAG_BLOCKED) state = "blk";
		else if (!(z_procs[i].flags & Z_PROC_FLAG_ACTIVE)) state = "---";

		printf(" pid: %2i %s base: %.8lx size: %.8lx pc %.8lx sp: %.8lx flags: %.8lx\n",
			i, state, z_procs[i].base, z_procs[i].size,
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

