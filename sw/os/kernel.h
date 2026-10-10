#ifndef Z_KERNEL_H
#define Z_KERNEL_H

#include <string.h>
#include "../common/zeitlos.h"
#include "../common/zexec.h"
#include "../common/zproc.h"

// z_rv, Z_OK and Z_FAIL are defined in ../common/zmsg.h (pulled in via
// zeitlos.h above) since apps need them too, not just the kernel.

#define Z_IRQ_TIMER			0	// picorv32 internal timer (yield pulse)
#define Z_IRQ_KTIMER			3
#define Z_IRQ_UART			4
#define Z_IRQ_HID				5
#define Z_IRQ_HID1				6	// second USB HID port -- see
									// rtl/sysctl.v's cpu_irq[6],
									// sw/os/hid.c

// rtl/audio.v's FIFO watermark (cpu_irq[7]). LEVEL-SENSITIVE, and
// non-latched in rtl/sysctl.v's LATCHED_IRQ mask for that reason --
// the same treatment Z_IRQ_UART gets. It stays asserted for as long as
// the FIFO is below its watermark, so a handler that returns without
// pushing samples will be re-entered immediately. That is the intended
// behaviour, not a bug, but it does mean the handler MUST either fill
// the FIFO or clear CTRL.IRQEN before returning.
//
// Optional hardware: check Z_FEATURE_AUDIO before enabling it. See
// sw/common/zaudio.h.
#define Z_IRQ_AUDIO				7

// Ethernet receive -- a frame is waiting in the MAC's RX buffer.
//
// A rising-edge PULSE, one per arrival -- not a level.
//
// This said "LEVEL, not a pulse" and argued that an edge has a window
// in which a packet can be dropped. The concern was right; the
// description was wrong. rtl/sysctl.v edge-detects on purpose, because
// bit 8 is latched and a latched level re-fires forever -- the level
// version brought the machine to a crawl.
//
// The window the old comment worried about was therefore real and
// open. It is closed on the software side instead: a wakeup that
// arrives while the driver is still running is recorded rather than
// dropped (Z_PROC_FLAG_WAKE in sw/common/zproc.h), so a single pulse
// cannot be missed no matter how it is timed against net's loop.
//
// One line for both MACs. rtl/ethmac_rmii.v drives it from its own
// rx_ready (STATUS bit 2) and the ENC28J60's active-low INT pin is
// inverted into the same wire in rtl/sysctl.v, so sw/apps/net sees one
// interrupt regardless of which ethernet the board has.
#define Z_IRQ_ETH				8

// USB host controller -- rtl/usb/usb_host.v. See docs/usb_host.md.
//
// LEVEL-SENSITIVE, and non-latched in rtl/sysctl.v's LATCHED_IRQ mask
// for that reason -- the same treatment Z_IRQ_UART and Z_IRQ_AUDIO
// get. It stays asserted for as long as (IRQSTAT & IRQEN) is non-zero.
// The `usb` process holds it (z_irq_bind(), Phase 3): the kernel clears
// IRQEN when it fires, which lowers it, and `usb` turns IRQEN back on
// after it has polled (sw/apps/usb/usb.c).
#define Z_IRQ_USB				9
// Memory protection fault (rtl/mpu.v, docs/mpu.md): a level, held until
// the kernel clears the MPU's FAULT_INFO.
#define Z_IRQ_MPU				10
// picorv32/zeitlos32 internal: illegal instruction (also ebreak and
// ecall), and misaligned access. Unmasked by the BIOS at boot.
#define Z_IRQ_ILLEGAL			1
#define Z_IRQ_MISALIGN			2

// Syscall pointer checks (docs/mpu.md): true if the app inside the
// current syscall may have the kernel write `len` bytes at `ptr`.
// Always true when not inside a syscall from an app. Handlers call it on
// every buffer they write through, and fail the call if it is false.
bool k_user_ok(const void *ptr, uint32_t len);
// ...and word-aligned, for buffers the kernel accesses a word at a time
// (structures, the syscall arguments themselves).
bool k_user_ok_words(const void *ptr, uint32_t len);

// -- scheduler trace (sw/os/ktrace.c, docs/kernel.md "Scheduler trace") --
//
// Built only with `make KTRACE=1`: a fixed ring of timestamped
// scheduler events, armed and stopped by Z_SYS_KTRACE (or `ktrace` in
// the shell) and printed by `ktrace`. In a normal build k_trace() is
// nothing and costs nothing.
enum {
	K_TR_SEND = 1,		// a: recipient pid
	K_TR_UNBLOCK,		// a: pid | 0x100 if it was BLOCKED (else WAKE recorded)
	K_TR_BLOCK,			// a: timeout ticks
	K_TR_NOBLOCK,		// a: 1 WAKE consumed, 2 mail waiting
	K_TR_YIELD,			// a: 1 timer(1), 0 waitirq, 2 already runnable
	K_TR_IRQ,			// a: pending irq bits
	K_TR_SWITCH,		// a: pid switched to
	K_TR_KEEP,			// a: 1 k_sched_switch kept the current process
	K_TR_RESCHED,		// owed switch requested by k_fs_leave()
	K_TR_DEFER,			// KTIMER swap deferred by the filesystem guard
	K_TR_TICK,			// a: 0 KTIMER branch entered, 1 after USB poll, 2 after the sweep
	K_TR_HANDOFF,		// a: pid handed the CPU by a process that blocked after sending to it
	K_TR_STEP,			// a: where k_sched_switch() is -- 1 GPU clip saved, 2 registers saved, 3 MMU/MPU set, 4 GPU clip restored
};
z_obj_t *k_ktrace(z_obj_t *args);	// Z_SYS_KTRACE, in every build
#ifdef KTRACE
void k_trace(uint32_t ev, uint32_t a);
void k_ktrace_cmd(const char *arg);	// the shell's `ktrace [arm|off]`
#else
#define k_trace(ev, a) ((void)0)
#endif

typedef struct {

	uint32_t		base;
	uint32_t		size;
	uint32_t		flags;

	// Tick at which a BLOCKED process becomes runnable again, or 0 for
	// "no timeout, wait indefinitely". Only meaningful while BLOCKED is
	// set. See k_proc_wait() in kernel.c.
	uint32_t		wake_tick;

	// KTIMER ticks this process has been the RUNNING one for, since
	// it was created. Free-running; a reader samples it twice and
	// divides the difference by the elapsed ticks to get a
	// percentage over that interval.
	//
	// Counted in the KTIMER interrupt handler, which fires at
	// Z_TICK_HZ (~732Hz) and knows which process it interrupted --
	// that is the whole measurement, and it costs one increment per
	// tick. There is no finer resolution available: a process that
	// starts and finishes work entirely between two ticks is
	// invisible, which is the standard limitation of sampled
	// accounting and worth remembering before trusting a single
	// short interval.
	//
	// This is the only CPU-time measurement in the system. Before it,
	// the closest thing available was counting runnable processes
	// (Z_PROC_FLAG_BLOCKED), which says how many things WANT the CPU
	// but nothing about who is getting it.
	uint32_t		cpu_ticks;

	uint32_t		regs[32];

	// The hardware scissor, saved across a context switch.
	// Raster gpu_clip_{x0,y0,x1,y1,enable} and blitter
	// gpu_blit_clip_{x0,y0,x1,y1} -- all readable. gpu_clip_valid
	// is 0 until the first save (a brand-new process has never
	// written the GPU); restore then installs the hardware reset
	// (raster clip off, blitter scissor = full screen).
	uint32_t		gpu_rast_clip[5];
	uint32_t		gpu_blit_clip[4];
	uint8_t			gpu_clip_valid;

	// The MPU grant window (docs/mpu.md, sw/common/zproc.h Z_GRANT_*):
	// the process whose memory this driver has open, as pid + 1 so
	// that 0 means none.
	uint32_t		gwin;

	// The MPU's MASK while this process runs: which address nibbles it
	// may use (docs/mpu.md). Z_MPU_MASK_DEFAULT for an app; all of them
	// for a driver, which drives hardware -- the SD card's nibble is
	// the one an app is denied. Written on every switch where it
	// differs from what the MPU holds.
	uint32_t		mpu_mask;

	// The process this one last woke by sending it a message, as
	// pid + 1 (0: none). When this one then blocks -- a request sent,
	// waiting for the answer; an answer sent, waiting for the next
	// request -- the CPU goes to that process next, ahead of the
	// round-robin order (k_sched_switch(), "hand-off"). Set by
	// k_msg_send(), cleared on every switch away from this process.
	uint32_t		handoff;

	// Cycles this process has run, interrupts excluded (kernel.c,
	// "CPU accounting"; Z_SYS_CPU_STAT).
	uint64_t		cpu_cycles;

} z_proc;

// Z_PROC_FLAG_ACTIVE / _DIE / _BLOCKED are defined in
// ../common/zproc.h, included above -- they are reported to apps
// through z_proc_info_t, so they cannot live in this header, which
// app code must not include. See that file for the full writeup on
// what BLOCKED means for the scheduler.

// Scheduler helpers -- k_proc_unblock() is called from msg.c on every
// delivery, so it has to be visible outside kernel.c.
uint32_t k_proc_runnable_count(void);

// Records a process's exit status in the ring k_proc_status() reads.
// Called from z_exit(); see syscalls.def's PROC_STATUS entry for why a
// ring rather than a zombie slot.
void k_proc_exit_record(uint32_t pid, int32_t status);
z_obj_t *k_proc_status(z_obj_t *args);
void k_proc_unblock(uint32_t pid);
z_obj_t *k_proc_wait(z_obj_t *args);
void k_proc_wait_ticks(uint32_t ticks);	// kernel code, in its own process
z_obj_t *k_msg_recv(z_obj_t *args);	// Z_SYS_MSG_RECV
z_obj_t *k_mpu_grant(z_obj_t *args);	// Z_SYS_MPU_GRANT
z_obj_t *k_irq(z_obj_t *args);		// Z_SYS_IRQ
z_obj_t *k_cpu_stat(z_obj_t *args);	// Z_SYS_CPU_STAT

// Z_SYS_WM_WAKE: unblock whoever subscribed to pointer reports
// (Z_SYS_HID_PTR_SUBSCRIBE, sw/os/hid.c). For input that reaches the
// machine without an interrupt behind it -- today the visor's pointer,
// which net writes straight into reg_vmouse.
z_obj_t *k_wm_wake(z_obj_t *args);

// After the caller has marked itself BLOCKED: if someone else can
// run, poke the UART so the existing IRQ path saves a full frame and
// switches; if not, sleep the core until any IRQ. See k_proc_wait().
void k_proc_yield_blocked(void);

// Eligible for a timeslice: active and not blocked.
#define Z_PROC_RUNNABLE(p) \
	(((p).flags & (Z_PROC_FLAG_ACTIVE | Z_PROC_FLAG_BLOCKED)) \
		== Z_PROC_FLAG_ACTIVE)


// 32, not 16 -- and NOT 64, which was tried and did not fit.
//
// Each slot is z_procs[] at 148 bytes plus z_mailboxes[] at 780, so
// 928 bytes; 32 slots is 29KB against 16's 15KB.
//
// -- a RAM budget, and no longer a flash one --
//
// Until Phase 3 kernel .bss cost image bytes one for one: kernel.bin
// was padded to _end, on the belief that the BIOS copy was what zeroed
// .bss. The toolchain's crt0 does that (sw/os/Makefile checks), so the
// image now stops at _edata. 64 slots once put the padded image at
// 269,784 bytes, 7,640 over the 256KB the BIOS copied then, and the
// first sign was the BIOS truncating the kernel on a real board.
//
// 32 is comfortably enough for what raised it: docs/posix.md's Phase 5
// runs one process per pipeline stage, and a four-stage pipeline
// alongside wm/net/repl/term/posix/zcc is about a dozen.
//
// More slots cost RAM on every board (the smallest has 1MB). If ever
// wanted, the cheapest place is Z_MAILBOX_DEPTH (zmsg.h), 84% of a
// slot's cost at 780 bytes against z_procs[]'s 148.
#define Z_PROCS_MAX 32

#include "kpidset.h"	// k_pidset_t, sized from Z_PROCS_MAX above

// Change a process's flags. EVERY write to z_procs[].flags goes through
// here, because the scheduler no longer reads them slot by slot: it
// keeps two pid sets -- k_run_set (Z_PROC_RUNNABLE) and k_die_set
// (Z_PROC_FLAG_DIE, waiting to be reaped) -- and this keeps them in
// step. A direct write to .flags would leave the sets stale, and the
// scheduler would skip a process that can run or run one that cannot.
// docs/kernel.md, "The scheduler".
void k_proc_flags(uint32_t pid, uint32_t set, uint32_t clear);

// A process has just set a wake deadline (wake_tick): the tick's sweep
// looks only at these (k_sleep_set). Call it wherever wake_tick is set
// non-zero.
void k_sleep_note(uint32_t pid);

// The tick's own cost and the two comparison switches -- kernel.c,
// "the tick's own cost"; the shell's `tick`. KDIAG=1 builds only.
#ifdef KDIAG
extern volatile uint32_t k_tick_n, k_tick_sw, k_tick_max;
extern volatile uint64_t k_tick_cyc;
extern volatile bool k_tick_sleepset;
#endif
extern volatile int32_t k_sched_late;	// `sched`, sh.c
extern volatile bool k_sched_drvwake;
extern volatile k_pidset_t k_run_set;
extern volatile k_pidset_t k_die_set;
extern volatile k_pidset_t k_drv_set;	// Z_PROC_FLAG_DRIVER

// Per-process stack+heap allowance (see mem.h's own comment on why
// there's no separate heap region at all -- this is the ONLY room a
// process's call stack AND malloc()'d heap ever get, shared, for its
// entire lifetime).

// IMPORTANT, because it is easy to get backwards: this is NOT where a
// process's static footprint lives. Code, .rodata and .bss are part of
// the BINARY, and k_proc_create() sizes the block as image + this. So
// `repl`'s 96KB Scheme cell heap (MS_HEAP_SIZE * sizeof(ms_val), a
// .bss array inside ms.o) is entirely unaffected by the size chosen
// here. What it bounds is the C stack plus whatever malloc() hands out
// at runtime.
//
// The program asks, with a size code in the ZEXE header
// (sw/common/zexec.h). Which app asks for how much, and why that size
// and not the next one down, is in that app's Makefile (APP_STACK);
// the longer notes are in docs/executables.md. A name is the wrong
// key: the card stores `cryptobench` as `cryptob`, and an app that is
// not in a table cannot ask for more without a new kernel.
//
// flags 0 and a binary with no header get the default.
// k_proc_create_exec() (kernel.c) refuses the rest of what it cannot
// give -- code 15, a reserved flag bit, a code above the cap, or a
// block the pool cannot hold -- and says so on the console. It never
// grants less than was asked for.
//
// The cap is the one number a board with a lot of RAM might want to
// raise. 8MB is code 11: one doubling above the 4MB that `zcc`,
// `posix` and `zfpga` ask for, so the largest ask in the tree can grow
// once without a kernel change, and nothing in sight needs more. The
// cap is not what protects a small board; the allocator is. On 1MB a
// 4MB request is refused because the pool cannot hold it, cap or no
// cap. k_mem_alloc() is a first-fit walk over a block list with
// Z_MEM_ALIGNMENT of 4096 (mem.h), so a large request late in a
// fragmented pool can fail while that much is nominally free; the
// refusal prints the largest free block for that reason.
//
// Process zero is not a ZEXE. kernel.c passes Z_PROC_STACK_SIZE_DEFAULT
// for it directly: that process is the kernel.
#define Z_PROC_STACK_SIZE_DEFAULT  (16 * 1024)
#define Z_PROC_STACK_CAP           (8 * 1024 * 1024)

// Names for the sizes apps in this tree ask for, because comments and
// docs refer to them. The header carries a size code, not these, and
// the loader does not use them.
#define Z_PROC_STACK_SIZE_SMALL    (8 * 1024)
#define Z_PROC_STACK_SIZE_MEDIUM   (32 * 1024)
#define Z_PROC_STACK_SIZE_LARGE    (64 * 1024)
#define Z_PROC_STACK_SIZE_BIG      (1024 * 1024)
#define Z_PROC_STACK_SIZE_HUGE     (4 * 1024 * 1024)

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
uint32_t k_proc_create_exec(const char *name, const z_exec_info_t *xi);
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

// Non-zero while a syscall that touches FatFs is running, which tells
// the KTIMER scheduler in kernel.c not to swap that process out.
// FatFs is built non-reentrant (FF_FS_REENTRANT 0) and sdmm.c holds CS
// asserted across a whole SPI transaction, so two processes in the
// filesystem at once leaves the card returning FR_DISK_ERR until it is
// re-initialised. Maintained ONLY by the syscall dispatcher -- see the
// full writeup there.
extern volatile uint32_t k_no_preempt;

// Bracket any code that reaches FatFs without going through the
// syscall dispatcher -- i.e. kernel code calling fs_* directly. Nests
// safely; every k_fs_enter() needs exactly one k_fs_leave() on every
// return path. fs.c's public entry points already do this, so callers
// of fs_* do NOT need to.
void k_fs_enter(void);
void k_fs_leave(void);

// Restarting a driver that crashed (kernel.c, "restarting a driver"):
// pid 0 asks before it waits for input, and runs what is due.
// k_drv_start() is sh.c's: how a driver is started, as at boot.
void k_drv_crashed(uint32_t pid);
uint32_t k_drv_restart_wait(void);
void k_drv_restart_run(void);
void k_drv_start(const char *name);

// Wake the driver holding an interrupt line, as if it had fired
// (kernel.c). hid.c's way of telling `usb` the lock LEDs changed.
void k_irq_poke(uint32_t irq);

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

// Z_SYS_REBOOT's handler, also called by the kernel shell: sync every
// open file, then reconfigure the FPGA. Returns only on failure.
// sw/os/kernel.c, docs/zboot.md.
z_obj_t *k_reboot(z_obj_t *args);
// Reboot (0) or jump (a flash address) through the jumploader; returns
// only on failure, negative, having printed why. kernel.c.
int k_boot_to(uint32_t target);

#endif
