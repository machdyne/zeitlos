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
#include "usb/usbh.h"
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
#include "procapi.h"
#include "flashapi.h"	// k_flash, referenced by the syscall table
#include "usbnetapi.h"	// k_usbnet, referenced by the syscall table
#include "usbcdcapi.h"	// k_usbcdc_*, referenced by the syscall table	// k_proc_list(), referenced by the syscall
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
z_obj_t *k_reboot(z_obj_t *args);	// Z_SYS_REBOOT: see its definition
z_obj_t *k_jump(z_obj_t *args);		// Z_SYS_JUMP
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

// Same reason -- defined below, needed visible here. Named k_video_*
// rather than z_video_* because sw/common/zsoc.h already declares
// z_video_get_mode()/z_video_set_mode() with different signatures (the
// direct-MMIO inline helpers these wrap), and zsoc.h is included
// above. Same k_/z_ split as k_getpid/z_getpid and k_msg_send/
// z_msg_send already use, for the same collision.
z_obj_t *k_video_get_mode(z_obj_t *args);
z_obj_t *k_video_set_mode(z_obj_t *args);
z_obj_t *k_hid_inject(z_obj_t *obj);
z_obj_t *k_kbd_layout(z_obj_t *args);	// sw/os/hid.c
z_obj_t *k_wm_wake(z_obj_t *args);

// CFG_GET/_ENTRY/_RELOAD handlers -- see cfg.h.
#include "cfg.h"

typedef z_obj_t* (*z_syscall_t)(z_obj_t *args);

z_syscall_t z_syscall_table[Z_SYSCALL_COUNT] = {
#define Z_MKSYSCALL(name, fn) [Z_SYS_##name] = fn,
#include "../common/syscalls.def"
#undef Z_SYSCALL
};

// -- filesystem serialisation (see k_no_preempt in kernel.h) --
//
// Non-zero means "the process currently running is inside a syscall
// that is touching FatFs; do not swap it out". Not a lock: nothing
// ever waits on it, and it is only ever read by the KTIMER swap
// below.
volatile uint32_t k_no_preempt = 0;

// The tick at which k_no_preempt last went 0 -> 1, and the number of
// ticks after which the swap happens ANYWAY.
//
// Why a cap at all: sdmm.c's wait_ready() spins for up to 500ms and
// rcvr_datablock() for up to 100ms before giving up. Deferring
// preemption across one of those would freeze every process on the
// machine for half a second, and a counter leaked by some future
// error path would freeze it permanently. A wedged machine is worse
// than the corruption this is preventing, so the deferral is allowed
// to lose.
//
// 64 ticks is ~87ms at Z_TICK_HZ (~732Hz) -- far longer than any
// healthy operation (a 512-byte sector is ~0.5ms at 48 cycles/byte,
// so even a 64KB single-syscall k_fs_read lands around 64ms) and far
// shorter than the driver's own timeouts.
static uint32_t k_no_preempt_start = 0;
#define K_NO_PREEMPT_MAX_TICKS 64

// The guard, for code that reaches FatFs WITHOUT going through the
// syscall dispatcher.
//
// The dispatcher covers anything an app calls. It does not cover the
// kernel calling fs_* directly, which sh.c's init() does -- see
// core_src_of() there. That path was the reason one dock probe still
// came back FR_DISK_ERR after the dispatcher guard landed: init was
// loading `net` through fs_exec_info_any() while wm probed through the
// EXEC_EXISTS syscall, and only one of the two was holding anything
// off.
//
// Counted, not boolean, so nesting is harmless: fs.c's own entry
// points call each other (fs_exec_info_any -> fs_exec_info), and a
// syscall handler that calls fs_* nests inside the dispatcher's own
// increment. Every enter has exactly one matching leave.
void k_fs_enter(void) {
	if (k_no_preempt == 0) k_no_preempt_start = z_kernel_ticks;
	k_no_preempt++;
}

void k_fs_leave(void) {
	if (k_no_preempt) k_no_preempt--;
}

// Which syscalls reach FatFs, and therefore must run to completion.
//
// PROC_RUN is in here and is the important one: it resolves and LOADS
// an executable, so init starting `net` and wm's dock_build() probing
// for apps are two processes in FatFs at the same time -- which is
// exactly the overlap that left the card returning FR_DISK_ERR for
// the rest of the boot.
//
// Deliberately a switch rather than a flag in syscalls.def: that file
// is shared with app-side code, and its comment warns that inserting
// an entry shifts every later enum value.
static int k_syscall_touches_fs(uint32_t id) {
	switch (id) {
		case Z_SYS_PROC_RUN:
		case Z_SYS_FS_SIZE:
		case Z_SYS_FS_READ:
		case Z_SYS_FS_WRITE:
		case Z_SYS_FS_UNLINK:
		case Z_SYS_FS_LIST:
		case Z_SYS_FS_OPEN_WRITE:
		case Z_SYS_FS_OPEN_READ:
		case Z_SYS_FS_READ_CHUNK:
		case Z_SYS_FS_WRITE_CHUNK:
		case Z_SYS_FS_CLOSE:
		case Z_SYS_FS_MKDIR:
		case Z_SYS_FS_TOUCH:
		case Z_SYS_FS_SEEK:
		case Z_SYS_FS_DF:
		case Z_SYS_FS_OPEN_RW:
		case Z_SYS_FS_SYNC:
		case Z_SYS_FS_TRUNCATE:
		case Z_SYS_EXEC_EXISTS:
		case Z_SYS_CFG_RELOAD:
		// Not files: these share the one USB transaction engine with
		// mass storage, which is used from inside FatFs. Holding the
		// scheduler for them, as for FatFs, keeps a process switched
		// out mid-transaction from letting another start on top of it.
		case Z_SYS_USBCDC_READ:
		case Z_SYS_USBCDC_WRITE:
		// USB ethernet: RECV and SEND hold the engine for a frame and
		// use MSC's sector area of the packet buffer (usbh_ecm.c).
		case Z_SYS_USBNET:
			return 1;
		default:
			return 0;
	}
}

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
// memory protection (docs/mpu.md); defined with k_fault() below
static bool k_mpu;
static void k_mpu_init(void);
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

// Reconfigure the FPGA -- to `target` through the jumploader, or plain
// reboot (target 0). docs/zboot.md secs. 5 and 6. Returns only if it
// could not, having said why:
//
//   -1  this gateware cannot pull PROGRAMN (FEATURES2 bit 5)
//   -2  it jumps through the jumploader (FEATURES2 bit 7), and there is
//       no valid one at Z_JUMP_FLASH_OFFSET -- PROGRAMN would reload
//       into nothing, and only a power cycle would bring it back
//   -3  the jumploader could not be re-pointed
//   -4  a target other than 0, and this gateware does not jump through
//       a jumploader (built without the Makefile's JUMP)
//   -5  PROGRAMN was pulled and the FPGA did not reconfigure
//
// Open files are synced only once nothing else can fail.
int k_boot_to(uint32_t target) {
	if (!z_soc_has_feature2(Z_FEATURE2_RECONFIG) ||
			((reg_socctl_reconfig >> 16) & 0xffffu) != Z_SOCCTL_RECONFIG_SIG) {
		printf("reboot: this board's gateware cannot reconfigure the FPGA "
			"(no PROGRAMN pin; see docs/zboot.md)\n");
		return -1;
	}
	if (z_soc_has_feature2(Z_FEATURE2_JUMP)) {
		int jr = k_jump_read(NULL);
		if (jr) {
			k_jump_explain("reboot", jr);
			printf("reboot: this gateware reloads from 0x%06lx, so rebooting "
				"now would stop the FPGA: power-cycle instead\n",
				(unsigned long)Z_JUMP_FLASH_OFFSET);
			return -2;
		}
		if (k_jump_point(target)) return -3;
	} else if (target) {
		printf("jump: this gateware does not reload through a jumploader "
			"(built without JUMP; docs/zboot.md sec. 5)\n");
		return -4;
	}
	int n = k_fs_sync_all();
	if (target) printf("jump: %d open file%s synced; to 0x%06lx\n", n, n == 1 ? "" : "s", (unsigned long)target);
	else printf("reboot: %d open file%s synced; reconfiguring\n", n, n == 1 ? "" : "s");
	// let the console line leave before the FPGA does
	for (volatile uint32_t d = 0; d < 200000; d++) ;
	reg_socctl_reconfig = Z_SOCCTL_RECONFIG_KEY;
	// PROGRAMN reconfigures within microseconds; this is ~0.5 s
	for (volatile uint32_t d = 0; d < 5000000; d++) ;
	printf("reboot: PROGRAMN was asserted but the FPGA did not reconfigure\n");
	return -5;
}

// Z_SYS_REBOOT: k_boot_to(0).
z_obj_t *k_reboot(z_obj_t *args) {
	(void)args;
	k_boot_to(0);
	return &z_fail;
}

// Z_SYS_JUMP: k_boot_to(the target in args).
z_obj_t *k_jump(z_obj_t *args) {
	z_jump_args_t *a = (z_jump_args_t *)args;
	if (!a) return &z_fail;
	a->result = k_boot_to(a->target);
	return &z_fail;
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

// -- virtual phosphor mode (rtl/socctl.v's VIDEO register) --
//
// Returns the current mode in args. On a bitstream that predates the
// register this reports Z_VIDEO_MODE_WHITE and still succeeds, because
// white is what such a board is genuinely displaying -- see
// z_video_get_mode() in sw/common/zsoc.h. A caller that needs to tell
// "white" from "can't change it" uses the set path, which does fail.
z_obj_t *k_video_get_mode(z_obj_t *args) {
	args->type = Z_UINT32;
	args->val.uint32 = z_video_get_mode();
	return (&z_ok);
}

// Sets the mode. Fails, writing nothing, on an out-of-range value or a
// bitstream without the register -- the distinction matters to the
// caller (a typo vs. gateware that needs reflashing) but not here, and
// z_video_set_mode() already refuses both.
//
// The mode is echoed back into args on the way out, whether or not the
// write succeeded, so a caller gets the mode actually in effect rather
// than the one it asked for. That is the difference between a `color`
// command that reports what the screen is doing and one that reports
// what it hoped.
z_obj_t *k_video_set_mode(z_obj_t *args) {

	bool ok = false;

	if (args && args->type == Z_UINT32)
		ok = z_video_set_mode(args->val.uint32);

	args->type = Z_UINT32;
	args->val.uint32 = z_video_get_mode();

	return ok ? (&z_ok) : (&z_fail);

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
// Long enough for a full PATH, not just a bare program name. The file
// browser launches an executable the user double-clicked, which may
// be several directories deep, and fs_exec_info_any() (sw/os/fs/fs.c)
// resolves a path perfectly well -- but a name longer than this is
// silently TRUNCATED here, which turns into a confusing "no such
// file" rather than an error about length. 64 matches
// Z_FLIST_PATH_MAX (sw/common/zflist.h), which is what the browser
// can produce.
#define Z_PROC_RUN_NAME_MAX 64
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

	int cur = -1;
	bool any_on_line = false;

	for (int i = 0; i < z_soc_features_count; i++) {

		if (!z_soc_has_feature(z_soc_features[i].bit)) continue;

		if (z_soc_features[i].group != cur) {
			if (any_on_line) printf("\n");
			printf("     %s ", z_soc_feature_groups[z_soc_features[i].group]);
			cur = z_soc_features[i].group;
			any_on_line = true;
		}

		printf("%s ", z_soc_features[i].name);

	}

	// The FEATURES2 half (sw/common/zsoc.c), continuing the same run
	// of lines rather than starting its own section -- which register
	// a bit lives in is an implementation detail of the CSR block, not
	// something a boot log should make the reader think about.
	//
	// Continuing works because `cur` and `any_on_line` carry over and
	// every group used by this table sorts after every group used by
	// the one above (see zsoc.c, and Z_FEAT_GROUP_IO's own note in
	// zsoc.h on why it is last in the enum). If that ever stops being
	// true, the symptom is a duplicated group heading, not an error.
	//
	// z_soc_has_feature2() silently reports false on a bitstream with
	// no FEATURES2 register at all, so an older gateware prints
	// exactly what it used to print rather than an "io (none)" line
	// about a register it does not have.
	for (int i = 0; i < z_soc_features2_count; i++) {

		if (!z_soc_has_feature2(z_soc_features2[i].bit)) continue;

		if (z_soc_features2[i].group != cur) {
			if (any_on_line) printf("\n");
			printf("     %s ", z_soc_feature_groups[z_soc_features2[i].group]);
			cur = z_soc_features2[i].group;
			any_on_line = true;
		}

		printf("%s ", z_soc_features2[i].name);

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

	printf(" - cpu: %s %ld.%02ld MIPS @ %ld MHz (%ld.%02ld IPC)\n",
		z_soc_cpu_name(),
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

	// Data cache (docs/dcache.md). The hardware comes out of reset with
	// it OFF so the BIOS -- including its memory test, which would
	// otherwise test the cache instead of the RAM -- runs exactly as on
	// a bitstream without it. Turn it on here, as early as possible.
	// Nothing else in the OS needs to know it exists: it is coherent
	// with every store the CPU makes, and no other master writes main
	// memory. Does nothing on a bitstream without `DCACHE.
	k_mpu_init();

	if (z_icache_present())
		printf(" - icache: %ldKB, %ld-word lines\n",
			(long)z_icache_kb(), (long)z_icache_line_words());
	if (z_dcache_set(true, true))
		printf(" - dcache: %ldKB, %ld-word lines, %ld-entry write buffer%s\n",
			(long)z_dcache_kb(), (long)z_dcache_line_words(),
			(long)z_dcache_wbuf_depth(), z_cache_burst() ? ", burst fills" : "");

	// init usb hid keyboard event queue
	z_hid_init();

	// USB host controller (docs/usb_host.md). Checks
	// Z_FEATURE2_USB_HOST itself and does nothing on a board built
	// without it, so this is unconditional here.
	//
	// AFTER z_hid_init(), not before: enumeration writes typ into the
	// compat blocks and z_hid_init() clears them, so the other order
	// would wipe a device that had already been found.
	z_usbh_init();
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

	// The ramdisk, at /ram.
	//
	// A fixed share of main memory rather than a fixed size: the
	// point is scratch space proportional to the machine, and a
	// number that suits a 32MB board is most of a 1MB one.
	//
	// SKIPPED entirely at 1MB. There the pool is the whole of memory
	// and every megabyte is already spoken for; an app wanting
	// scratch on such a board should use the card, which is what it
	// had to do anyway.
	//
	// -- WHY IT IS HERE, AND NOT NEXT TO k_mem_init() --
	//
	// It has to come after k_proc_create() has reserved process
	// zero. The kernel runs from Z_MEM_BASE (0x40000000) and the pool
	// starts there too; nothing tells k_mem_alloc() that the running
	// kernel occupies the bottom of it until process zero is created.
	//
	// Allocating before that returns the kernel's OWN image and
	// stack, and f_mkfs() then writes a FAT over it. The symptom is a
	// hang immediately after "memory initialized", which is
	// indistinguishable from k_mem_init() itself having failed.
	if (mem_total > (1024 * 1024)) {
		uint32_t ram_bytes = mem_total / Z_RAMDISK_DIVISOR;
		if (ram_bytes > Z_RAMDISK_MAX) ram_bytes = Z_RAMDISK_MAX;
		fs_ramdisk_create(ram_bytes);
	}

	printf(" - starting shell.\n");

	// the kernel shell is process zero
	sh();

}

// - a pointer to the kernel entry function can be found at 0x0000000c
//   (if the kernel started)
// - this is called by the BIOS interrupt handler which uses the interrupt stack
// - it can also be called by apps to make system calls

// Interrupt-entry census, measurement only. [0] counts every
// entry on the interrupt path, [3..8] count each source line seen in
// `irqs`, [2] counts entries carrying any line outside 3..8. Read out
// (cumulative) at the end of k_proc_dump(), so two `ps` runs bracketing
// a workload give the IRQ rate by source.
uint32_t z_irq_census[9];

// Virtualise the GPU scissor per process. The registers are
// global hardware with no privilege, so the kernel cannot intercept
// writes -- it reads them on the way out and puts them back on the
// way in. Drain before restoring a *different* clip: the rasterizer
// samples clip live per pixel (FIFO entries do not carry it) and a
// glyph blit samples the vertical scissor live per row. Fills latch
// clip at start and are safe in flight. Spins are bounded; this runs
// inside the IRQ handler so it must not printf.
static void k_gpu_clip_save(z_proc *p)
{
	p->gpu_rast_clip[0] = gpu_clip_x0;
	p->gpu_rast_clip[1] = gpu_clip_y0;
	p->gpu_rast_clip[2] = gpu_clip_x1;
	p->gpu_rast_clip[3] = gpu_clip_y1;
	p->gpu_rast_clip[4] = gpu_clip_enable;
	p->gpu_blit_clip[0] = gpu_blit_clip_x0;
	p->gpu_blit_clip[1] = gpu_blit_clip_y0;
	p->gpu_blit_clip[2] = gpu_blit_clip_x1;
	p->gpu_blit_clip[3] = gpu_blit_clip_y1;
	p->gpu_clip_valid = 1;
}

static void k_gpu_clip_restore(z_proc *p)
{
	uint32_t rx0, ry0, rx1, ry1, ren;
	uint32_t bx0, by0, bx1, by1;
	uint32_t n;

	if (p->gpu_clip_valid) {
		rx0 = p->gpu_rast_clip[0];
		ry0 = p->gpu_rast_clip[1];
		rx1 = p->gpu_rast_clip[2];
		ry1 = p->gpu_rast_clip[3];
		ren = p->gpu_rast_clip[4];
		bx0 = p->gpu_blit_clip[0];
		by0 = p->gpu_blit_clip[1];
		bx1 = p->gpu_blit_clip[2];
		by1 = p->gpu_blit_clip[3];
	} else {
		rx0 = 0; ry0 = 0; rx1 = 639; ry1 = 479; ren = 0;
		bx0 = 0; by0 = 0; bx1 = 640; by1 = 480;
	}

	if (gpu_clip_x0 != rx0 || gpu_clip_y0 != ry0 ||
	    gpu_clip_x1 != rx1 || gpu_clip_y1 != ry1 ||
	    gpu_clip_enable != ren) {
		n = 0;
		while ((gpu_busy & 1) && n < 50000u)
			n++;
		gpu_clip_x0 = rx0;
		gpu_clip_y0 = ry0;
		gpu_clip_x1 = rx1;
		gpu_clip_y1 = ry1;
		gpu_clip_enable = ren;
	}

	if (gpu_blit_clip_x0 != bx0 || gpu_blit_clip_y0 != by0 ||
	    gpu_blit_clip_x1 != bx1 || gpu_blit_clip_y1 != by1) {
		if ((gpu_blit_status & 1) &&
		    (gpu_blit_ctrl & GPU_BLIT_CTRL_GLYPH)) {
			n = 0;
			while ((gpu_blit_status & 1) && n < 10000u)
				n++;
		}
		gpu_blit_clip_x0 = bx0;
		gpu_blit_clip_y0 = by0;
		gpu_blit_clip_x1 = bx1;
		gpu_blit_clip_y1 = by1;
	}
}

// Save the interrupted frame, pick the next RUNNABLE process, return
// its frame. Same dance the KTIMER path has always done -- extracted
// so a BLOCKED process can switch on any IRQ, not only on the tick.
//
// Does not advance z_kernel_ticks and does not sweep wake_tick: those
// stay the KTIMER caller's job. A yield (UART THRE pulse after
// k_proc_wait) must not look like time passing.
static uint32_t *k_sched_switch(uint32_t *regs) {

	int sched_scanned;

	// don't switch if there's at most one process that could run.
	// Deliberately runnable, not active: if wm/net/repl are all
	// blocked on their mailboxes, the one process with work to do
	// keeps the CPU instead of round-robining through three
	// processes that would each immediately block again.
	// Only skip the switch if the CURRENT process is itself still
	// runnable. Otherwise we would decline to switch AWAY FROM a
	// process that has just blocked itself, and go on running it --
	// which is both wrong and, with exactly two processes, fatal.
	//
	// Concretely: with only the shell and net, net calls
	// z_proc_wait(), marks itself BLOCKED, and the runnable count
	// drops to 1 (the shell). The old test then returned `regs` --
	// net's own context -- so net kept running while blocked and the
	// shell was never scheduled again. The serial console simply
	// stopped responding. It went unnoticed because wm and repl are
	// normally running, which keeps the count above 2.
	//
	// This test predates Z_PROC_FLAG_BLOCKED, when "runnable" meant
	// "active" and the current process was always counted.
	if (Z_PROC_RUNNABLE(z_procs[z_pid]) &&
		k_proc_runnable_count() < 2) return regs;

	// save current process registers and GPU scissor
	k_gpu_clip_save(&z_procs[z_pid]);
	for (int i = 0; i < 32; i++) {
		z_procs[z_pid].regs[i] = *(regs + i);
	}

	// Bounded scan. Before BLOCKED existed, this loop was
	// guaranteed to terminate because the current process was
	// itself active and would be reached again. That is no longer
	// true -- every process can now be unschedulable at once -- and
	// an unbounded scan here would spin forever INSIDE the
	// interrupt handler, which is unrecoverable. The count is the
	// safety net; the k_proc_runnable_count() check above means it
	// should never actually be hit.
	sched_scanned = 0;

	// find next runnable process (round-robin scheduling)
	next_process:
	if (++sched_scanned > Z_PROCS_MAX) return regs;
	z_pid++;
	if (z_pid >= Z_PROCS_MAX) z_pid = 0;

	if ((z_procs[z_pid].flags & Z_PROC_FLAG_DIE) == Z_PROC_FLAG_DIE) {
		// NOTE: this whole branch runs inside the interrupt
		// handler itself (this function's interrupt path, not the
		// syscall path) -- picorv32's interrupt model here doesn't
		// nest, so nothing on this path can safely call
		// printf()/anything that waits on another interrupt to
		// make progress. uart.c's own _write() documents exactly
		// this hazard: `while (k_uart_tx_full()) /* wait */;` has
		// no timeout, and the TX fifo is only ever drained by the
		// UART TX interrupt -- which can never fire while we're
		// already inside THIS interrupt handler. A printf() briefly
		// lived right here (paired with one in k_proc_kill()
		// below, which runs via the syscall path instead and is
		// fine) -- it caused a genuine, total hang the moment it
		// landed at a point where the TX fifo happened to already
		// be full (far more likely right after a burst of unrelated
		// output, e.g. telnet's own connect-sequence prints), with
		// nothing able to recover it since even the scheduler
		// itself never gets to run again. Removed; k_proc_kill()'s
		// own print (this file) still shows every DIE request as it
		// happens, which is what actually matters for the
		// investigation this was added for -- exactly when the
		// resulting free below actually runs is a fixed, short
		// delay after that (at most one full round-robin cycle),
		// not additional information worth this risk to observe
		// directly.
		k_mem_free((void *)z_procs[z_pid].base);
		k_pidreg_release_all(z_pid);
		// release any file handles this process left open.
		// Same shape and same reason as the pidreg sweep
		// above: without it a process killed by wm's close
		// icon (Z_WIN_FLAG_CLOSE_KILLS_OWNER) keeps its
		// handles forever, and Z_FS_MAX_OPEN is 8 -- so
		// relaunching one app that holds a handle exhausts
		// the table in a session and only a reboot recovers
		// it. See k_fs_release_all() in fsapi.c, and the
		// KNOWN LIMITATION note in sw/common/zfs.h that this
		// closes.
		k_fs_release_all(z_pid);
		// and its flash session, if it held one (flashapi.c)
		k_flash_release_pid(z_pid);
		z_procs[z_pid].base = 0x00000000;
		z_procs[z_pid].flags = 0x00000000;
		goto next_process;
	}

	// skips both inactive and blocked slots -- see
	// Z_PROC_RUNNABLE()/Z_PROC_FLAG_BLOCKED in kernel.h
	if (!Z_PROC_RUNNABLE(z_procs[z_pid]))
		goto next_process;

	reg_mtu = z_procs[z_pid].base;
	// the MPU's idea of "this app's own memory" follows the MTU's
	if (k_mpu) reg_mpu_size = z_procs[z_pid].size;
	k_gpu_clip_restore(&z_procs[z_pid]);
	return (uint32_t *)z_procs[z_pid].regs;

}

// -- crashes and memory protection (docs/mpu.md) ------------------
//
// A crash used to hang the machine: picorv32 raises IRQ 1 for an
// illegal instruction (and ebreak/ecall) and IRQ 2 for a misaligned
// access, the BIOS unmasks both, and nothing here handled them -- so
// the handler returned and the CPU re-executed the faulting
// instruction forever. Now an app that crashes is ended with a report
// and the rest of the system carries on; a crash in kernel code stops
// the machine with a panic report instead of a silent hang.
//
// The MPU (rtl/mpu.v) adds a third source, IRQ 10: an app storing
// outside its own memory, jumping outside its own code, or touching a
// kernel-only register. In report-only mode those are logged and the
// app continues; enforcing, they end it like any other crash.

extern char _etext[];           // end of kernel text (riscv-os.ld)

// k_mpu (declared at the top): MPU present and programmed
static uint32_t k_mpu_logged;   // report-only lines printed so far
#define K_MPU_LOG_MAX 16

// Exit status recorded for a process ended by a crash, so a shell
// waiting on it (posix) sees it fail rather than succeed.
#define K_EXIT_CRASHED (-128)

static const char *const k_mpu_kinds[] = { "fetch", "load", "store", "?" };
static const char *const k_mpu_reasons[] = {
	"?",
	"outside its own memory",
	"a kernel-only address",
	"an address space it may not use",
	"a jump into the middle of the kernel",
};

// Where an address points, for humans.
static void k_fault_where(uint32_t a) {
	if ((a & 0xf0000000u) == 0x80000000u) {
		if (z_pid < Z_PROCS_MAX && (a & 0x0fffffffu) >= z_procs[z_pid].size)
			printf("past the end of its own memory");
		else
			printf("its own memory");
		return;
	}
	if (a < 0x2000u) { printf("BIOS RAM"); return; }
	if ((a & 0xf0000000u) == 0x40000000u) {
		// Kernel first: process 0 (init0) runs from the kernel image,
		// so its block covers kernel code and data, and a lookup by
		// block would call kernel code "pid 0's memory".
		if (a < (uint32_t)(uintptr_t)_etext) { printf("kernel code"); return; }
		for (uint32_t p = 0; p < Z_PROCS_MAX; p++) {
			if (z_procs[p].base == 0x40000000u) continue;
			if (z_procs[p].base && a >= z_procs[p].base &&
				a < z_procs[p].base + z_procs[p].size) {
				const char *n = k_pidreg_name_for(p);
				if (p == z_pid) printf("its own memory");
				else printf("pid %lu's memory%s%s%s", (unsigned long)p,
					n ? " (" : "", n ? n : "", n ? ")" : "");
				return;
			}
		}
		printf("kernel memory");
		return;
	}
	switch (a >> 28) {
		case 0x1: printf("flash"); return;
		case 0x2: printf("VRAM"); return;
		case 0x7: printf("system registers"); return;
		case 0x9: printf("MTU/MPU registers"); return;
		case 0xb: printf("the SD card"); return;
		default:  printf("peripheral registers"); return;
	}
}

static void k_fault_regs(uint32_t *regs) {
	printf("    ra %08lx  sp %08lx  gp %08lx  a0 %08lx  a1 %08lx\n",
		(unsigned long)regs[1], (unsigned long)regs[2],
		(unsigned long)regs[3], (unsigned long)regs[10],
		(unsigned long)regs[11]);
}

// The faulting instruction's address, from the PC the CPU saved.
//
// Both cores RETIRE an illegal instruction (and ebreak/ecall) or a
// misaligned load/store before raising IRQ 1/2 at the next boundary
// (picorv32.v's next_irq_pending[irq_ebreak] with cpu_state_fetch;
// zeitlos32.v's irq_pending_n[1]/[2] with retire = 1), so the saved PC
// is the address AFTER the faulting instruction. Returning to it would
// silently skip the instruction, which is what happened before this
// handler existed. The exception is a misaligned jump, where the saved
// PC is the bad target itself -- recognisable by not being aligned.
static uint32_t k_fault_pc(uint32_t pc) {
	return (pc & 3u) ? pc : pc - 4;
}

// Describe an IRQ 1/2 cause. The instruction is read through the
// current MTU mapping, which is still the faulting process's.
static void k_fault_cause(uint32_t irqs, uint32_t pc) {
	if ((irqs & (1u << Z_IRQ_MISALIGN)) && (pc & 3u)) {
		printf("jump to a misaligned address");
		return;
	}
	if (irqs & (1u << Z_IRQ_MISALIGN)) { printf("misaligned memory access"); return; }
	uint32_t insn = *(volatile uint32_t *)(uintptr_t)k_fault_pc(pc);
	if (insn == 0x00100073u) printf("breakpoint (ebreak)");
	else if (insn == 0x00000073u) printf("ecall (not used by this OS)");
	else if ((insn & 0x7f) == 0x33 && ((insn >> 25) & 0x7f) == 0x01)
		printf("multiply/divide instruction %08lx: this bitstream has no M extension",
			(unsigned long)insn);
	else printf("illegal instruction %08lx", (unsigned long)insn);
}

static void k_panic(uint32_t *regs, uint32_t irqs, uint32_t pc) {
	maskirq(0xffffffffu);
	k_uart_polled = true;
	printf("\n*** KERNEL PANIC: ");
	k_fault_cause(irqs, pc);
	printf("\n    pc %08lx", (unsigned long)k_fault_pc(pc));
	if (z_pid < Z_PROCS_MAX && z_procs[z_pid].base) {
		const char *n = k_pidreg_name_for(z_pid);
		printf(", during a system call from pid %lu%s%s%s",
			(unsigned long)z_pid, n ? " (" : "", n ? n : "", n ? ")" : "");
	}
	printf("\n");
	k_fault_regs(regs);
	printf("    system halted\n");
	k_uart_flush();
	// and on the screen, for a machine with no serial cable
	k_klog_panic_screen();
	for (;;) ;
}

// Called from the interrupt path for IRQ 1, 2 and 10. Returns true if
// the current process has been ended and the caller must switch away
// from it; false to carry on (report-only log, nothing to do).
static bool k_fault(uint32_t *regs, uint32_t irqs) {
	uint32_t pc = regs[0];
	uint32_t info = 0, faddr = 0, fpc = 0, fcount = 0;
	bool mpu = false;

	if (k_mpu && (reg_mpu_fault_info & Z_MPU_FAULT_VALID)) {
		mpu = true;
		info = reg_mpu_fault_info;
		faddr = reg_mpu_fault_addr;
		fpc = reg_mpu_fault_pc;
		fcount = reg_mpu_count;
		reg_mpu_fault_info = 0;   // clears it, and drops IRQ 10
		reg_mpu_count = 0;
	}
	bool enforce = k_mpu && (reg_mpu_ctrl & Z_MPU_CTRL_ENFORCE);
	bool cpu_trap = (irqs & ((1u << Z_IRQ_ILLEGAL) | (1u << Z_IRQ_MISALIGN))) != 0;
	// "view (pid 7)", or just "pid 6" for a process that never
	// registered a name (e.g. a program built with zcc)
	const char *name = (z_pid < Z_PROCS_MAX) ? k_pidreg_name_for(z_pid) : NULL;
	char who[40];
	if (name) snprintf(who, sizeof(who), "%s (pid %lu)", name, (unsigned long)z_pid);
	else snprintf(who, sizeof(who), "pid %lu", (unsigned long)z_pid);

	// -- report only: log, and let it run
	if (mpu && !enforce && !cpu_trap) {
		if (k_mpu_logged < K_MPU_LOG_MAX) {
			printf("mpu: %s would fault: %s %08lx (",
				who, k_mpu_kinds[Z_MPU_FAULT_KIND(info)], (unsigned long)faddr);
			k_fault_where(faddr);
			printf(") at pc %08lx", (unsigned long)fpc);
			if (fcount > 1) printf(" (+%lu more)", (unsigned long)(fcount - 1));
			printf("\n");
			if (++k_mpu_logged == K_MPU_LOG_MAX)
				printf("mpu: further reports suppressed (`mpu` for the count)\n");
		}
		return false;
	}
	if (!mpu && !cpu_trap) return false;

	// -- a crash. In kernel code, nothing can be trusted: panic. An
	//    MPU fault is by construction from app code.
	bool app = mpu || ((pc & 0xf0000000u) == 0x80000000u);
	if (!app || z_pid >= Z_PROCS_MAX || !z_procs[z_pid].base)
		k_panic(regs, irqs, pc);

	printf("\n*** %s crashed: ", who);
	if (mpu) {
		printf("%s %s\n", k_mpu_kinds[Z_MPU_FAULT_KIND(info)],
			k_mpu_reasons[Z_MPU_FAULT_REASON(info) < 5 ? Z_MPU_FAULT_REASON(info) : 0]);
		printf("    address %08lx (", (unsigned long)faddr);
		k_fault_where(faddr);
		printf(")  at pc %08lx\n", (unsigned long)fpc);
	} else {
		k_fault_cause(irqs, pc);
		printf("\n    pc %08lx\n", (unsigned long)k_fault_pc(pc));
	}
	k_fault_regs(regs);
	printf("    ended; the rest of the system is unaffected\n");

	k_proc_exit_record(z_pid, K_EXIT_CRASHED);
	z_procs[z_pid].flags |= Z_PROC_FLAG_DIE;
	return true;
}

// Program the MPU at boot, enforcing: a violation ends the app with a
// crash report. `mpu report` in the shell switches to logging only.
static void k_mpu_init(void) {
	if (!z_mpu_present()) return;
	reg_mpu_ktext = (uint32_t)(uintptr_t)_etext;
	reg_mpu_gate = (uint32_t)(uintptr_t)z_kernel_entry;
	reg_mpu_mask = Z_MPU_MASK_DEFAULT;
	reg_mpu_size = 0;
	reg_mpu_fault_info = 0;
	reg_mpu_count = 0;
	reg_mpu_ctrl = Z_MPU_CTRL_ENABLE | Z_MPU_CTRL_ENFORCE | Z_MPU_CTRL_IRQ;
	k_mpu = true;
	printf(" - mpu: enforcing (`mpu report` to only log)\n");
}

// -- syscall pointer checks --
//
// True if [ptr, ptr+len) is memory the running app may have the kernel
// write to: its own block, through its window (0x8xxx_xxxx) or at its
// physical address. A refusal is logged (the first 16) and the handler
// fails the call, which the app sees as an ordinary error rather than
// the kernel scribbling over some other process's memory.
//
// "The running app" is z_pid, not something recorded at syscall entry:
// syscalls other than FatFs ones can be preempted, and z_pid is what a
// switch saves and restores, so it is always the process whose syscall
// this is. The kernel's own process (pid 0) has its block at the kernel
// image, below _end, and apps are all above it -- so kernel code calling
// a handler directly is never refused.
static uint32_t k_user_bad_logged;

bool k_user_ok(const void *ptr, uint32_t len) {
	if (z_pid >= Z_PROCS_MAX ||
		z_procs[z_pid].base < (uint32_t)(uintptr_t)&_end) return true;
	uint32_t pid = z_pid;
	uint32_t a = (uint32_t)(uintptr_t)ptr;
	uint32_t size = z_procs[pid].size;
	uint32_t lo = ((a & 0xf0000000u) == 0x80000000u) ? 0x80000000u
		: z_procs[pid].base;
	if (a >= lo && a - lo <= size && len <= size - (a - lo)) return true;
	if (k_user_bad_logged < 16) {
		const char *n = k_pidreg_name_for(pid);
		printf("syscall: %s%spid %lu passed a bad pointer %08lx (%lu bytes); call refused\n",
			n ? n : "", n ? " " : "", (unsigned long)pid,
			(unsigned long)a, (unsigned long)len);
		if (++k_user_bad_logged == 16)
			printf("syscall: further bad-pointer reports suppressed\n");
	}
	return false;
}

// k_user_ok(), and word-aligned: for anything the kernel reads or writes
// a word at a time (the arguments themselves, arrays of structures). A
// misaligned word access is an exception, and in kernel code an
// exception is a panic -- so an app's misaligned pointer used to be able
// to take the whole system down. It is refused and logged instead.
bool k_user_ok_words(const void *ptr, uint32_t len) {
	if (((uint32_t)(uintptr_t)ptr & 3u) &&
		z_pid < Z_PROCS_MAX && z_procs[z_pid].base >= (uint32_t)(uintptr_t)&_end) {
		if (k_user_bad_logged < 16) {
			const char *n = k_pidreg_name_for(z_pid);
			printf("syscall: %s%spid %lu passed a misaligned pointer %08lx; call refused\n",
				n ? n : "", n ? " " : "", (unsigned long)z_pid,
				(unsigned long)(uintptr_t)ptr);
			if (++k_user_bad_logged == 16)
				printf("syscall: further bad-pointer reports suppressed\n");
		}
		return false;
	}
	return k_user_ok(ptr, len);
}

// `mpu` shell command support (sh.c)
bool k_mpu_active(void) { return k_mpu; }
void k_mpu_reset_log(void) { k_mpu_logged = 0; }

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
			// Hold off the KTIMER swap for the duration of anything
			// that touches FatFs. FF_FS_REENTRANT is 0 (ffconf.h) --
			// FatFs is explicitly not reentrant -- and underneath it
			// sdmm.c drives CS and clocks a command through the SPI
			// shifter. Swapping to another process mid-transaction
			// leaves the card mid-command and corrupts the shared
			// FATFS window buffer; the card then returns FR_DISK_ERR
			// to everyone until disk_initialize() is re-run by a
			// manual `mount`.
			//
			// Deferring rather than taking a blocking mutex is
			// deliberate. A mutex would let the holder be preempted,
			// which is fine for the card (SPI mode tolerates gaps
			// between bytes) but not for this driver: dly_us()
			// (sdmm.c) measures with rdcycle, a free-running counter
			// that keeps advancing while the process is descheduled.
			// Every timeout in the driver would then depend on
			// scheduling rather than on the card.
			//
			// Nested by construction -- a syscall cannot be made from
			// inside a syscall handler -- but counted rather than set
			// so that stays true if it ever stops being.
			int fslock = k_syscall_touches_fs(syscall_id);

			// Syscall pointer checks (docs/mpu.md). The MPU cannot
			// stop the kernel writing on an app's behalf, so a bad
			// pointer an app hands to a syscall is checked here
			// instead: the arguments themselves (many handlers write
			// results back into them) must be in the caller's own
			// memory, and handlers check any buffer they write
			// through with k_user_ok(). Only apps are checked: the
			// kernel's own process (pid 0) runs from the kernel image.
			if (regs && !k_user_ok_words(regs, 4)) {
				ret = (uint32_t *)&z_fail;
			} else {
				if (fslock) k_fs_enter();

				ret = (uint32_t *)z_syscall_table[syscall_id]((z_obj_t *)regs);

				// Single exit -- this path has no early returns between
				// the enter and here, which is what keeps the counter from
				// leaking.
				if (fslock) k_fs_leave();
			}
		}

		goto done;

	}

	// not a system call; must be an interrupt

	// Printing from here must not block (sw/os/uart.c): cleared at done.
	k_uart_polled = true;

	// Crashes and protection faults first: an app that faulted must not
	// be resumed at the faulting instruction. See k_fault() above.
	bool k_ended = false;
	if (irqs & ((1u << Z_IRQ_ILLEGAL) | (1u << Z_IRQ_MISALIGN) | (1u << Z_IRQ_MPU)))
		k_ended = k_fault(regs, irqs);

	// Census: which line(s) brought us in, nothing else.
	z_irq_census[0]++;
	for (int b = 3; b <= 8; b++)
		if (irqs & (1u << b)) z_irq_census[b]++;
	if (irqs & ~0x1F8u) z_irq_census[2]++;

	// only the KTIMER IRQ should advance the tick counter -- it was
	// previously incremented for ANY interrupt (including UART RX/TX,
	// which fires far more often, especially under heavy printf
	// activity from multiple processes), inflating z_kernel_ticks
	// well beyond real elapsed time. this made every tick-based
	// timeout (z_msg_wait_timeout(), tftp.c's retry timer) fire much
	// sooner than intended.
	if ((irqs & (1 << Z_IRQ_KTIMER)) != 0) {
		++z_kernel_ticks;
		z_usbh_poll();
		// cpu_ticks is charged after the wake sweep below, and
		// only if that process is RUNNABLE. Billing whoever
		// last waitirq'd made idle time look like work: the
		// moment wm also slept, net inherited every idle tick
		// and looked busy again. A wait(1) loop is still billed
		// -- the sweep unblocks it first, then this charges it.
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

	// USB host. A LEVEL, not a pulse -- z_usbh_poll() clears IRQSTAT
	// itself, which is what lowers it. Non-latched in rtl/sysctl.v's
	// LATCHED_IRQ for the same reason Z_IRQ_UART is: a latched level
	// re-fires the instant the handler returns.
	//
	// Also advanced from the ktimer below, because a device sitting in
	// a timed state -- waiting out the 2 ms after SET_ADDRESS, say --
	// has no interrupt coming to move it along.
	if ((irqs & (1 << Z_IRQ_USB)) != 0) {
		z_usbh_poll();
	}

	// Ethernet receive -- unblock whoever is waiting on packets.
	//
	// No handler and no queue: the frame is sitting in the MAC's RX
	// buffer and the driver reads it from there. All this interrupt
	// has to do is make sure the driver gets scheduled to look, which
	// is exactly what unblocking it does.
	//
	// Z_IRQ_ETH is a rising-edge PULSE, one per arrival -- NOT a
	// level. rtl/sysctl.v edge-detects the MAC's level deliberately:
	// bit 8 is latched, and a latched level re-fires forever, which
	// brought the machine to a crawl. Read that file's comment before
	// changing anything here.
	//
	// This comment used to claim the opposite, and concluded there
	// was "no window in which the interrupt has been acknowledged but
	// a packet is still unread". That is true of a level and false of
	// an edge, and the gap it hid was a real one: a frame arriving
	// while net was still running unblocked a process that was not
	// blocked yet, which was a no-op, and the frame sat unread until
	// net's backstop timeout expired ~100ms later. Interactive
	// traffic over telnet felt exactly as slow as that sounds.
	//
	// k_proc_unblock() now RECORDS a wakeup that arrives too early
	// (Z_PROC_FLAG_WAKE, zproc.h) and k_proc_wait() consumes it
	// instead of sleeping, so a single pulse cannot be missed however
	// it is timed.
	//
	// The pid is looked up rather than fixed: net registers itself
	// like any other service, and hardwiring a pid here would break
	// the moment it were restarted.
	if ((irqs & (1 << Z_IRQ_ETH)) != 0) {
		uint32_t net_pid;
		if (k_pid_find("net0", &net_pid)) k_proc_unblock(net_pid);
	}

	// swap process on KTIMER interrupt
	if ((irqs & (1 << Z_IRQ_KTIMER)) != 0) {

		// A process is inside a FatFs syscall -- let it finish rather
		// than corrupting the card (see the note at the dispatch site
		// above). z_kernel_ticks has already been advanced by this
		// point, so timers and z_msg_wait_timeout() are unaffected;
		// only the process swap is postponed. Interrupts stay enabled
		// throughout, so ethernet RX still lands in its buffer.
		//
		// The cap is the escape hatch: past it, swap anyway. See
		// K_NO_PREEMPT_MAX_TICKS.
		if (k_no_preempt &&
			(z_kernel_ticks - k_no_preempt_start) < K_NO_PREEMPT_MAX_TICKS) {
			if (z_pid < Z_PROCS_MAX && Z_PROC_RUNNABLE(z_procs[z_pid]))
				++z_procs[z_pid].cpu_ticks;
			ret = regs;
			goto done;
		}

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

		// Bill the process that is actually schedulable this
		// tick, after the sweep. A blocked waiter is idle.
		if (z_pid < Z_PROCS_MAX && Z_PROC_RUNNABLE(z_procs[z_pid]))
			++z_procs[z_pid].cpu_ticks;

		ret = k_sched_switch(regs);
		goto done;
	}

	// A process that has just blocked (k_proc_wait, UART
	// wait) pokes UART THRE so this path runs with a real irq_vec
	// frame. Switch now rather than burning the rest of the slice.
	if (k_ended || (!Z_PROC_RUNNABLE(z_procs[z_pid]) && k_proc_runnable_count() >= 1)) {
		ret = k_sched_switch(regs);
		goto done;
	}

	ret = regs;

	done:
	k_uart_polled = false;
	__asm__ volatile ("mv gp, %0" :: "r"(saved_gp) : "memory");
	return ret;

}

// Make a blocked process schedulable again. Safe to call on a process
// that isn't blocked, which is what lets k_msg_send() call it
// unconditionally on every delivery.
//
// Calling it on a process that has NOT blocked yet is not a no-op any
// more: the wakeup is recorded, so it cannot be lost in the window
// between a process deciding to wait and actually being marked
// BLOCKED. See Z_PROC_FLAG_WAKE in zproc.h for the case that made
// this necessary.
void k_proc_unblock(uint32_t pid) {
	if (pid >= Z_PROCS_MAX) return;
	if (z_procs[pid].flags & Z_PROC_FLAG_BLOCKED) {
		z_procs[pid].flags &= ~Z_PROC_FLAG_BLOCKED;
		z_procs[pid].wake_tick = 0;
	} else {
		z_procs[pid].flags |= Z_PROC_FLAG_WAKE;
	}
}

// Z_SYS_WM_WAKE. The visor's pointer is a plain MMIO write to
// reg_vmouse with no interrupt behind it, so net calls this after the
// write to get the reader looking. It is the same wake the HID ISR
// does -- the subscriber registered through Z_SYS_HID_PTR_SUBSCRIBE
// (sw/os/hid.c) -- and deliberately not a second registry: there is
// one pointer as far as a reader is concerned, whichever wire it
// came in on.
z_obj_t *k_wm_wake(z_obj_t *args) {
	(void)args;
	k_hid_wake_subscriber();
	return (&z_ok);
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
// After marking BLOCKED we yield on the spot, via the
// existing IRQ path rather than a new syscall frame. The syscall is a
// jalr that does not save registers (docs/app_runtime.md), so the
// kernel cannot switch from here. Two ways were considered:
//
//  1. Force an IRQ so irq_vec (which already saves all 32 GPRs)
//     runs as soon as this syscall returns, then k_sched_switch
//     sees us BLOCKED and picks someone else. Zero new context
//     code. Chosen.
//  2. A ~40-instruction save in the app stub and save/scan/restore
//     from the syscall itself (~800-1200 cycles per wait). A second
//     switch path to keep in sync with irq_vec.
//
// Path 1 is the picorv32 `timer` insn, which fires IRQ 0. That
// needs ENABLE_IRQ_TIMER=1 in the bitstream (sysctl.v); with it
// off the insn is illegal (IRQ 1: both cores retire it and trap with
// the PC of the NEXT instruction, so it is skipped, not retried --
// and k_fault() now ends the process instead; docs/mpu.md).
// A UART-THRE pulse was tried first so the bitstream could stay:
// writing IER.THRE is a no-op when THRE is already enabled (the
// printf path), and waitirq in that hole froze every RUNNABLE
// until the next IRQ -- unm barely moved. timer(1) is independent
// of the UART. If nobody else is RUNNABLE, waitirq sleeps the
// core until any IRQ instead of spinning.
z_obj_t *k_proc_wait(z_obj_t *args) {
	uint32_t timeout = (args->type == Z_UINT32) ? args->val.uint32 : 0;

	// THE TEST AND THE BLOCK MUST BE ATOMIC TOGETHER.
	//
	// z_mailbox_empty() (msg.c) masks interrupts to read `count`, but
	// it RELEASES the mask before returning -- so without the mask
	// held here, there is a window between "mailbox is empty" and
	// "flags |= Z_PROC_FLAG_BLOCKED" in which a KTIMER tick can
	// preempt this process, run another one, and have it deliver a
	// message:
	//
	//   this process   z_mailbox_empty() -> true, IRQs re-enabled
	//   *** KTIMER preempts ***
	//   sender         z_mailbox_push()      message queued
	//   sender         k_proc_unblock(us)    clears BLOCKED -- which
	//                                        is not set yet, so this
	//                                        is a NO-OP and the wakeup
	//                                        is LOST
	//   this process   flags |= BLOCKED      sleeps, holding an
	//                                        unread message
	//
	// With `timeout` 0 -- what an app's idle loop passes -- wake_tick
	// is the "indefinite" sentinel, so nothing ever wakes it on its
	// own. It sleeps until the NEXT message happens to arrive and
	// unblocks it as a side effect.
	//
	// The visible symptom is an app that ignores a request it was
	// definitely sent, then services it later when something unrelated
	// wakes it: "wm: timed out waiting for pid N to ack a redraw" on a
	// perfectly healthy, idle app, followed by the redraw appearing
	// anyway a moment later. Rare, load-dependent, and it survives
	// every fix applied further up the stack, because the app is
	// ASLEEP rather than slow.
	//
	// This function's own comment already said the test had to happen
	// "in the same syscall" that sets the flag. That is necessary but
	// not sufficient: it has to happen under the same mask.
	//
	// maskirq() nests correctly here -- z_mailbox_empty()'s internal
	// mask/restore is a no-op while we already hold it.
	uint32_t old_mask = maskirq(0xFFFFFFFF);

	// A wakeup that landed before this process got here. Consume it
	// and do not sleep: whatever it signalled has already happened,
	// and for a direct unblock there is nothing left to re-check the
	// way the mailbox test below re-checks a message.
	//
	// Tested under the same mask as that test, and for the same
	// reason -- checking it outside would reopen the very window this
	// is here to close.
	if (z_procs[z_pid].flags & Z_PROC_FLAG_WAKE) {
		z_procs[z_pid].flags &= ~Z_PROC_FLAG_WAKE;
		maskirq(old_mask);
		return (&z_fail);
	}

	// something already waiting -- don't block, let the caller read it
	if (!z_mailbox_empty(z_pid)) {
		maskirq(old_mask);
		return (&z_fail);
	}

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

	maskirq(old_mask);

	k_proc_yield_blocked();

	return (&z_ok);
}

void k_proc_yield_blocked(void) {

	// Woken already (message, UART, timeout landed between the
	// BLOCKED store and here) -- nothing to give up.
	if (Z_PROC_RUNNABLE(z_procs[z_pid])) return;

	if (k_proc_runnable_count() >= 1) {
		// Someone else can run: pulse picorv32 IRQ 0 via
		// timer(1). irq_vec then saves the real frame and
		// k_sched_switch (the !RUNNABLE branch in
		// z_kernel_entry) picks them. Independent of the UART
		// -- a THRE pulse is a no-op when IER already has THRE,
		// which is the common case during printf, and waitirq
		// would freeze every RUNNABLE until the next IRQ.
		timer(1);
	} else {
		// Nobody to switch to. Sleep the core until any IRQ
		// (KTIMER, UART RX, ETH, HID) rather than spinning.
		waitirq();
	}

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
		z_procs[p].gpu_clip_valid = 0;
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

		// cpu is the process's LIFETIME tick count, not a
		// percentage -- `ps` is a snapshot and has no second sample
		// to difference against. sw/apps/info takes two and reports
		// a real percentage; see z_proc_info_t in zproc.h.
		printf(" pid: %2i %s base: %.8lx size: %.8lx pc %.8lx sp: %.8lx flags: %.8lx cpu: %lu wake: %lu now: %lu\n",
			i, state, z_procs[i].base, z_procs[i].size,
			z_procs[i].regs[0], z_procs[i].regs[2], z_procs[i].flags,
			(unsigned long)z_procs[i].cpu_ticks,
			(unsigned long)z_procs[i].wake_tick, (unsigned long)z_kernel_ticks);
	}
	printf(" irq: total=%lu tmr=%lu uart=%lu hid0=%lu hid1=%lu aud=%lu eth=%lu oth=%lu\n",
		(unsigned long)z_irq_census[0], (unsigned long)z_irq_census[3],
		(unsigned long)z_irq_census[4], (unsigned long)z_irq_census[5],
		(unsigned long)z_irq_census[6], (unsigned long)z_irq_census[7],
		(unsigned long)z_irq_census[8], (unsigned long)z_irq_census[2]);
	return Z_OK;
}

// --

void kprint(const char *s) {
    // writes UART0 directly, so it records into the console log itself
    // (sw/os/uart.c, docs/console.md)
    while (*s) {
        if (*s == '\n') {
            while ((reg_uart0_lsr & 0x20) == 0);
            k_klog_putc('\r');
            reg_uart0_data = '\r';
        }
        while ((reg_uart0_lsr & 0x20) == 0);
        k_klog_putc((uint8_t)*s);
        reg_uart0_data = *s++;
    }
}

void kprint_hex_digit(uint8_t val) {
    // Wait for the transmitter like kprint() does (this used to write
    // unconditionally and pace itself with a 500-iteration spin loop,
    // which the data cache made 2-3x shorter). Records into the
    // console log, like kprint().
    char ch = (val < 10) ? ('0' + val) : ('A' + (val - 10));
    while ((reg_uart0_lsr & 0x20) == 0);
    k_klog_putc((uint8_t)ch);
    reg_uart0_data = ch;
}

void kprint_hex32(uint32_t val) {
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        kprint_hex_digit(nibble);
    }
}

// --

/* -- the exit ring --
 *
 * Sixteen recently-exited processes and their statuses.
 *
 * A zombie slot is the textbook answer and it leaks: nothing in this
 * system is obliged to reap anything, so an unreaped child holds a
 * process slot forever, and the shell that forgets to ask is exactly
 * the shell that will run many children. A ring cannot leak. It can
 * only forget, and it forgets oldest-first -- which is the order
 * nobody is still waiting on.
 *
 * Sixteen because a shell asks within a few hundred milliseconds of
 * the exit and nothing else asks at all. If something ever misses, the
 * symptom is Z_PROC_STATE_UNKNOWN rather than a wrong answer.
 *
 * .bss-attributed for the same reason as z_procs[] and z_mailboxes[]:
 * kernel globals reached through a syscall run with the CALLER's gp
 * (docs/kernel.md, "The gp hazard").
 */
#define K_EXIT_RING_SIZE 16

typedef struct {
	uint32_t	pid;
	int32_t		status;
	bool		valid;
} k_exit_entry_t;

static __attribute__((section(".bss"))) k_exit_entry_t
	k_exit_ring[K_EXIT_RING_SIZE];
static __attribute__((section(".bss"))) uint32_t k_exit_next;

void k_proc_exit_record(uint32_t pid, int32_t status) {

	/* An earlier entry for the same pid is replaced rather than
	 * duplicated. Pids are reused, and a stale entry for a slot that
	 * has since been recycled would answer a question about the NEW
	 * process with the OLD one's status -- which is worse than
	 * answering UNKNOWN. */
	for (uint32_t i = 0; i < K_EXIT_RING_SIZE; i++) {
		if (k_exit_ring[i].valid && k_exit_ring[i].pid == pid) {
			k_exit_ring[i].status = status;
			return;
		}
	}

	k_exit_ring[k_exit_next].pid = pid;
	k_exit_ring[k_exit_next].status = status;
	k_exit_ring[k_exit_next].valid = true;
	k_exit_next = (k_exit_next + 1) % K_EXIT_RING_SIZE;
}

z_obj_t *k_proc_status(z_obj_t *args) {

	z_proc_status_args_t *a = (z_proc_status_args_t *)args;

	if (!a) return &z_fail;

	a->state = Z_PROC_STATE_UNKNOWN;
	a->status = 0;

	if (a->pid < Z_PROCS_MAX &&
		(z_procs[a->pid].flags & Z_PROC_FLAG_ACTIVE) &&
		!(z_procs[a->pid].flags & Z_PROC_FLAG_DIE)) {
		a->state = Z_PROC_STATE_RUNNING;
		return &z_ok;
	}

	for (uint32_t i = 0; i < K_EXIT_RING_SIZE; i++) {
		if (k_exit_ring[i].valid && k_exit_ring[i].pid == a->pid) {
			a->state = Z_PROC_STATE_EXITED;
			a->status = k_exit_ring[i].status;
			return &z_ok;
		}
	}

	/* UNKNOWN is a successful answer, not a failure: "I do not know"
	 * is information, and a caller that got Z_FAIL could not tell it
	 * apart from a malformed call. */
	return &z_ok;
}

z_obj_t *z_exit(z_obj_t *obj) {
	uint32_t pid = z_pid;

	/* The status used to be dropped here.
	 *
	 * This function took its argument and ignored it, so every exit
	 * status in the system was discarded at the first step -- and
	 * sw/apps/zcc's entry stub packs one, carefully, for nobody. A
	 * shell cannot make `a && b` mean anything without it: the best it
	 * can do is report whether the program STARTED.
	 *
	 * Z_INT32 is what an exit status is; anything else is recorded as
	 * 0, which is the same thing falling off the end of main() means. */
	int32_t status = 0;
	if (obj && obj->type == Z_INT32) status = obj->val.int32;
	else if (obj && obj->type == Z_UINT32) status = (int32_t)obj->val.uint32;

	k_proc_exit_record(pid, status);

	k_proc_kill(pid);
	while (1) /* wait to die */;
}

