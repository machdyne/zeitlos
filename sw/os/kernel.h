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
// get. It stays asserted for as long as (IRQSTAT & IRQEN) is non-zero,
// so the handler MUST clear IRQSTAT before returning or it is
// re-entered immediately. z_usbh_poll() does that first thing.
//
// Unlike Z_IRQ_ETH this can safely be a level rather than a pulse,
// because the handler CAN lower the source: clearing IRQSTAT is a
// register write, not something that needs a userspace process to run.
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
// -- why this is a FLASH budget, not just a RAM one --
//
// Kernel .bss costs image bytes, one for one. sw/os/Makefile links
// kernel.bin with `objcopy --pad-to=_end`, so .bss is present in the
// file as zeros, because the BIOS copies a FIXED 256KB from flash
// (ROM_OS_SIZE, sw/bios/bios.c) and that copy is what zeroes .bss.
// Anything past the end of the file would be filled with whatever
// happens to be in flash after it.
//
// So `_end` must stay under 256KB, and every static array in the
// kernel spends that budget. 64 slots put the image at 269,784 bytes
// -- 7,640 over -- and the first sign of it was the BIOS truncating
// the kernel on a real board.
//
// 32 is comfortably enough for what raised it: docs/posix.md's Phase 5
// runs one process per pipeline stage, and a four-stage pipeline
// alongside wm/net/repl/term/posix/zcc is about a dozen.
//
// The padding is NOT the thing to remove if more slots are ever
// wanted -- it is load-bearing, and dropping it would leave .bss
// holding whatever the BIOS copied out of the flash beyond the image.
// The options, in order of how much they disturb:
//
//   1. Z_MAILBOX_DEPTH (zmsg.h), which is 84% of a slot's cost at 780
//      bytes against z_procs[]'s 148. Halving it to 16 halves that.
//   2. A per-board Z_PROCS_MAX, which needs a board define in the
//      kernel build -- sw/os/Makefile passes only ARCH_DEFS today.
//   3. Moving Z_ZAR_FLASH_OFFSET (zar.h, 0x140000) and ROM_OS_SIZE
//      (sw/bios/bios.c) to give the kernel more than 256KB. That is a
//      flash-layout change: it moves the core-app archive, so a board
//      flashed with the old layout and a new BIOS finds neither.
#define Z_PROCS_MAX 32

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

// The kernel work k_deferred_request() (uart.h) asked for, run by pid 0
// while it waits for input.
void k_deferred_run(void);

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
