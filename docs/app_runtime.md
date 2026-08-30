# Zeitlos App Runtime


## Overview

Every app in `sw/apps/*` links against `sw/common/zeitlos.c/h` --
this is the runtime layer that sits between an app's own `main()`
and the kernel: newlib's syscall stubs (so `printf`/`malloc`/etc.
work at all), the syscall trampoline into the kernel proper, direct
register access to the SoC's peripherals, and a handful of
convenience wrappers (messaging, uptime, line-editing) built on top.
`sw/common/zgfx.c/h` and `sw/common/zwin.c/h` layer graphics-specific
functionality on top of that -- covered in detail below and in
`docs/window_manager.md` respectively.

This document is the map of that whole stack: what's in it, how the
pieces fit together, and -- for the GPU rasterizer specifically --
why parts of it look the way they do, since that shape came out of a
real, previously-shipped bug, not an upfront design.

Related documents, so this one doesn't duplicate them:
- `docs/messaging.md` -- the object system and mailbox messaging
  (`z_msg_t`, `z_obj_t`) in full detail. This document only covers
  the app-side wrapper functions.
- `docs/window_manager.md` -- the window manager's own protocol,
  `zwin.c`'s window-relative drawing helpers, and hardware glyph
  blitting.
- `docs/networking.md` -- `zstream.h`, the streaming layer built on
  top of messaging, and the TFTP client that uses it.

## Process model

Apps are built as ordinary RISC-V ELF executables (`ENTRY(_start)` in
`sw/common/riscv-app.ld`) using the toolchain's standard startup code
-- there's no custom `crt0` in this project (an earlier, unused
attempt at one is still referenced, commented out, in some app
Makefiles; the file it points to doesn't exist). Standard newlib
startup zeroes `.bss`, then calls `main()`; `main()` returning (or
calling `exit()`) flows into `_exit()` below, same as any other
newlib target.

Every process is linked to run at a fixed virtual base, `0x80000000`
(`. = 0x80000000;` in `riscv-app.ld`), regardless of where its code
actually lives in physical memory -- the kernel's MTU (memory
translation unit) peripheral remaps that virtual base to each
process's own physical allocation on every context switch (see
`docs/networking.md`'s TFTP debugging notes for the bug this
indirection was involved in once: an `objcopy` build step silently
truncating a binary before `.bss`, understating how much physical
memory a process actually needed).

`kernel.bin` itself (`sw/os/Makefile`) had this same `objcopy`
truncation gap until it was given the same `--pad-to=$END` treatment
every app `Makefile` already uses (`hello_win`'s is the clearest
example) -- found by inspection, not by a reproduced symptom, so
whether it was ever actually observed causing a problem on real
hardware is unconfirmed. Worth taking seriously regardless: `.bss`
zero-init has already been shown *not* reliable on this hardware once
before (the pid name registry's own table needed explicit zeroing in
`main()` after `.bss` alone "broke real hardware almost immediately"
-- see `k_pidreg_init()`'s call site in `sw/os/kernel.c`), and an
untruncated, zero-padded flash image is what actually makes that
zero-init assumption true in the first place, for every `.bss`
variable, not just the ones that have separately needed their own
explicit-zeroing workaround so far.

`_end` (provided by the linker, right after `.bss`) marks the top of
a process's static footprint -- everything above it, up to its own
stack, is heap. `_sbrk()` (`zeitlos.c`) grows the heap up from `_end`
one `malloc()` at a time, with one safety check: it refuses to grow
past the *current* stack pointer (`register char *sp asm("sp")`,
read fresh on every call), so a heap that's grown too large can't
silently collide with a deeply-nested stack -- `malloc()` just starts
failing instead. There's no MMU here, so this is a soft check against
one process's own accidental self-collision, not protection against
anything else -- see "Trust model" below.

**`k_proc_create()` (`sw/os/kernel.c`) used to have a real bug in this
same area**, only exercised once something other than `sh.c` (pid 0)
could trigger it -- worth understanding since it's the same class of
mistake as the TFTP one just above, and the `z_translate()` one in
`sw/os/msg.c`, all three being different ways of getting a
`0x8000_0000`-relative address wrong. Setting up a brand-new process's
initial registers, `k_proc_create()` needs to write that process's
initial return address onto the *new* process's own stack -- before
that process has been started, while some other process (whichever
one called `k_proc_create()`) is still the one actually executing.
Because the MTU only remaps `0x8000_0000` "on every context switch"
(see above) -- not on demand for whatever the currently-running code
happens to be setting up -- a virtual-address write through that
window at this point resolves through the *caller's own* current
mapping, not the new process's. The original code did exactly this
(`*(uint32_t *)(0x80000000 + mem_size - 4) = ...`), which happened to
be harmless every time it was actually exercised, since the only
caller was `sh.c` (pid 0), and pid 0's own memory region rarely
collided with anything that mattered. It stopped being harmless the
moment a second caller existed: `Z_SYS_PROC_RUN` (see the syscall
table below) lets *any* running process launch another one, and when
a live process with active heap/stack of its own calls it, that stray
write lands squarely inside the caller's own memory -- in practice,
this crashed `wm` immediately after it used the dock (see
`docs/window_manager.md`, "The dock") to launch an app. Fixed by
writing through the physical address instead (`base + mem_size - 4`,
using the physical `base` already computed earlier in the same
function) -- no translation needed or wanted, since this is a direct
physical write to memory the kernel already knows the real address
of.

**A second, unrelated bug in the same function, found immediately
after fixing the first one** (diagnostic `kprint()`/`kprint_hex32()`
tracing through `k_proc_run()`/`k_proc_create()` narrowed it down --
see either function's git history if those traces are still present).
`k_proc_create()`'s out-of-memory path used to read
`if (!mem) return(Z_FAIL);`. `Z_FAIL` is `1` (`zmsg.h`) -- a real,
valid pid a caller could otherwise legitimately get back on *success*,
not a sentinel distinguishable from one. Every caller (`sh.c`'s `run`,
`k_proc_run()`) checks success/failure with `if (pid)`/`if (!pid)`,
treating any nonzero return as "created, this is the new pid" -- so an
allocation failure was silently read as "successfully created process
1". Process 1 is normally a real, active process (`wm` itself, see
`Z_PID_WM` in `zwm.h`) -- so the caller went on to call
`k_proc_base(1)` (returning `wm`'s own, currently-in-use base) and
`fs_load()` the app being launched directly into it, overwriting `wm`'s
own live memory out from under itself. This reproduced immediately:
launching `gpu3d` from the dock failed to allocate its ~100KB
(`k_mem_alloc()` returned `NULL` -- worth checking separately whether
that's a genuine capacity issue, given the pool is `Z_MEM_SIZE` = 1MB
total and every process consumes at least `Z_MEM_MIN_BLOCK_SIZE` =
32KB regardless of how small it actually is, `mem.h`), and that
failure, misread as "created pid 1", corrupted `wm` while it was the
very process making the call. Fixed by returning `0` instead --
matching this function's own actual convention elsewhere (the
`return(0);` at the very end, for "no free slot", already used `0`,
not `Z_FAIL` -- this was a one-line inconsistency within the same
function, not a codebase-wide convention mismatch). This bug
predates the dock/`Z_SYS_PROC_RUN` work -- it was always latent in
`k_proc_create()`, just never observed, since `sh.c` (pid 0) was the
only caller before, and pid 0 is always occupied (permanently, from
boot) so a caller could never mistake a failure return of `1` for a
legitimately-returned pid 0 the way it could -- and did -- for pid 1.

**A third bug, found chasing the second one further** -- with the
`Z_FAIL` issue fixed, `gpu3d` still failed to launch (cleanly this
time, no crash) with `k_mem_alloc()` genuinely returning `NULL` for a
~100KB request that should have had plenty of room (only the kernel
and `wm` itself were running). The actual cause: `mem.c`'s metadata
pool counter, `mem_block_count` -- a small `static int`, read and
incremented on every single `k_mem_alloc()` call via
`alloc_metadata()` -- wasn't tagged
`__attribute__((section(".bss")))`. This matters a great deal here,
and there's already a precedent for exactly this fix a few lines above
it in `kernel.c` (`z_pid`/`z_procs[]`/`z_kernel_ticks`, tagged with the
comment "force bss because `__global_pointer$` will be wrong in the
interrupt handler") and in `mem.c` itself (`block_list`, right next to
`mem_block_count`, already had the tag -- `mem_block_count` didn't).

The mechanism, chased down in full this time: `gp` (the global
pointer register, used by RISC-V's "small data" addressing relaxation
to reach `.sdata`/`.sbss`/tagged-`.bss` globals in a single
instruction) is set once, per binary, by that binary's own linker
script (`__global_pointer$ = . + 0x800;` in both `riscv-os.ld` and
`riscv-app.ld`, each relative to that binary's own `.sdata`) -- the
kernel's own `gp` and any given app's own `gp` are different values,
pointing at different memory entirely (kernel linked at
`0x40000000`, apps at `0x80000000`). A **syscall**, as implemented
here (see "The syscall trampoline" below), is a plain `jalr` from
inside the app's own compiled code straight into kernel code -- `gp`
isn't part of the C ABI's caller/callee-saved convention (compilers
assume it's a whole-program constant, never touched across calls), so
nothing changes it. Kernel code executing via a syscall runs with
whatever `gp` the *calling app* had, not the kernel's own -- so any
small-enough kernel global the compiler chooses to address
`gp`-relative resolves through the wrong base, silently reading and
writing memory inside the calling app's own address space instead of
the kernel's real variable. This is genuinely different from (not a
duplicate of) the interrupt/scheduler path, which is NOT affected the
same way: `sw/bios/boot_picorv32.S`'s `irq_vec` saves and restores all
32 GPRs -- `gp` included -- around every hardware interrupt, so each
process's own `gp` correctly survives being preempted and resumed.
Syscalls don't go through that path at all, so there's no equivalent
save/restore for them.

`mem_block_count` was never affected before, purely because it was
never *reached* this way before -- `sh.c` (kernel code, always
correctly running with the kernel's own `gp`) was the only caller of
anything in `mem.c` until `Z_SYS_PROC_RUN` gave an app (`wm`, via its
dock) a way to reach `k_mem_alloc()` for the first time. Garbage read
through the wrong `gp` happened to come back `>= Z_MEM_MAX_BLOCKS`,
so `alloc_metadata()` reported the metadata pool full, and
`k_mem_alloc()` propagated that as a clean, ordinary-looking
allocation failure -- no crash, no obviously wrong value, just a
plausible "out of memory" that wasn't real.

The `.bss` tag on `mem_block_count` (matching the existing precedents
above) turned out NOT to fix this on its own -- the very next test
hung instead of cleanly failing, a *different* symptom from the exact
same underlying cause, which is itself informative: the linker's
relaxation pass decides `gp`-relative vs. absolute addressing by a
symbol's FINAL LINKED ADDRESS being within reach of `gp`, not which
named section it's tagged into. A `.bss`-tagged symbol placed early
enough (right after `.sdata`/`.sbss`, exactly where `gp` points) can
still get relaxed -- and exactly where any given symbol lands is a
build-layout detail the section tag doesn't control, so the same tag
can appear to "work" for one variable and not another, or stop working
after an unrelated change shifts layout. Confirmed by compiling a test
build for a similar (but not identical -- rv64, not this project's
rv32i) RISC-V target and disassembling the result: `mem_block_count`,
`.bss`-tagged, still generated a `gp`-relative access.

**The actual, general fix**: make `gp` correct -- the kernel's own,
not whatever the calling app's was -- for the entire duration any
kernel code executes via the syscall path, in `z_kernel_entry()`
itself, rather than chasing individual variables. Right at the top of
the syscall branch (before touching `z_syscall_table[]`, which has
the exact same exposure), save the caller's `gp`, switch to the
kernel's own (`&__global_pointer$`, the same linker symbol
`riscv-os.ld`'s `.sdata` section already defines), dispatch to the
syscall handler, then restore the caller's `gp` before returning --
so the calling app's own `gp`-relative addressing is undisturbed once
control goes back to it. This protects every kernel global uniformly,
present and future, without depending on anyone remembering to tag a
new one. The same test build's disassembly of the fixed function shows
exactly the intended shape (register names/addressing forms will
differ some on the project's actual rv32i target, but the sequence is
the same): save caller's `gp`, load `__global_pointer$`'s address,
switch `gp` to it, call the syscall handler through `z_syscall_table[]`,
switch `gp` back to the saved value, return.

The existing `.bss` tags (`kernel.c`'s `z_pid`/`z_procs[]`/
`z_kernel_ticks`, `mem.c`'s `block_list`/`mem_block_count`) are
redundant now, but left in place -- harmless, and no reason to
disturb working (if no longer load-bearing) code.

An alternative considered and not taken: disabling `gp`-relative
addressing entirely for the kernel build (`-mno-relax`, or
`-msmall-data-limit=0`). Would also work, and is simpler to reason
about (no `gp`-relative addressing anywhere in the kernel, ever, so
nothing to get wrong), at the cost of one extra instruction for every
small-global access kernel-wide -- code size and a marginal, likely
unmeasurable runtime cost. The syscall-dispatch fix above is more
precisely targeted (only the syscall path pays anything, and it pays
two `mv` instructions total, not a systemic per-access cost) and
doesn't require touching every kernel `Makefile`'s flags, so it's the
one implemented -- but `-mno-relax` remains a reasonable fallback if
`gp` mismatches are ever suspected somewhere this fix doesn't reach
(`sw/bios/`, a separate, tiny binary with its own `gp`, isn't covered
by this kernel-side fix at all, though its own C code -- `irq()`,
relaying into `z_kernel_entry` -- doesn't appear to touch anything but
`irq_regs`/`irq_stack`, both plain assembly-addressed, not C globals,
so this doesn't look like it currently needs the same treatment).

This is a **general hazard for any new small kernel global reached
from a syscall for the first time** that no longer needs to be
worried about per-variable, now that the fix lives at the dispatch
point instead.

## The syscall trampoline

`reg_kernel` (`0x0000000c`) holds a function pointer the kernel
installs at boot -- `z_kernel_ptr_t`, `uint32_t *(*)(uint32_t,
uint32_t*, uint32_t)`. Every actual syscall is a direct call through
that pointer:

```c
z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
z_obj_t obj;
z_kernel_ptr(Z_SYS_UPTIME, (uint32_t *)&obj, 0);
```

`Z_SYS_*` ids come from `sw/common/syscalls.def`, X-macro'd into the
`z_syscall_id_t` enum in `zeitlos.h`:

| id | kernel handler | app-side wrapper |
|---|---|---|
| `Z_SYS_EXIT` | `z_exit` | `_exit()` |
| `Z_SYS_UI_PRINT` | `z_ui_print` | *(none in `zeitlos.c` -- kernel-internal use, see `sw/os/ui.c`)* |
| `Z_SYS_UART_GETC` | `z_uart_getc` | `uart_getc()` |
| `Z_SYS_UART_PUTC` | `z_uart_putc` | `uart_putc()` |
| `Z_SYS_UART_RX_EMPTY` | `z_uart_rx_empty` | `uart_rx_empty()` |
| `Z_SYS_UART_TX_FULL` | `z_uart_tx_full` | `uart_tx_full()` |
| `Z_SYS_MSG_SEND` | `k_msg_send` | `z_msg_send()` |
| `Z_SYS_MSG_READ` | `k_msg_read` | `z_msg_read()` |
| `Z_SYS_UPTIME` | `z_uptime` | `z_uptime_ticks()` |
| `Z_SYS_HID_READ_KEY` | `z_hid_read_key` | `hid_read_key()` |
| `Z_SYS_PID_REGISTER` | `k_pid_register` | `z_pid_register()` |
| `Z_SYS_PID_LOOKUP` | `k_pid_lookup` | `z_pid_lookup()` |
| `Z_SYS_GETPID` | `k_getpid` | `z_getpid()` |
| `Z_SYS_PROC_RUN` | `k_proc_run` | `z_proc_run()` |

Adding a new syscall means adding a `Z_MKSYSCALL(...)` line to
`syscalls.def`, a handler in the kernel, and (usually) a thin
app-side wrapper here following the same pattern. Kernel handler and
app-side wrapper can't share a name if the kernel file also includes
`zeitlos.h` (most do, for the register/type definitions) -- `kernel.c`
does, which is why `Z_SYS_GETPID`/`Z_SYS_PID_REGISTER`/
`Z_SYS_PID_LOOKUP`/`Z_SYS_PROC_RUN`'s handlers are `k_getpid`/
`k_pid_register`/`k_pid_lookup`/`k_proc_run`, not their `z_`-prefixed
app-facing names -- those were already taken by the wrappers declared
in `zeitlos.h`, and would otherwise collide, `-Wall`-visibly.
`k_proc_run`/`k_msg_send`/`k_msg_read`/etc. follow this existing
`k_`-prefix convention for kernel-internal functions that don't share
their app-facing name.

`Z_SYS_PID_REGISTER`/`Z_SYS_PID_LOOKUP`/`Z_SYS_GETPID` are the pid
name registry (`sw/os/pidreg.c/h`) -- lets a process register a
kernel-numbered name for itself (`z_pid_register("term", ...)` ->
`"term0"`, `"term1"`, ...) and lets others resolve that name back to a
pid (`z_pid_lookup()`), instead of relying on fixed pid constants like
`Z_PID_WM` that only worked because of boot-order convention. `wm.c`
registers itself as `"wm0"`; `zwin.c`'s `z_win_create()` looks that up
(falling back to `Z_PID_WM` if the lookup fails, e.g. an old wm build
that predates the registry).

`Z_SYS_PROC_RUN` (`k_proc_run()`, `sw/os/kernel.c`) is what lets a
running process launch another one -- before it existed, only
kernel-space code (`sh.c`, compiled directly into `kernel.bin`, not a
syscall caller at all) could call `k_proc_create()`/`fs_load()`/
`k_proc_start()`. It's the same three-call sequence sh.c's `run`
command and `init()` use, just wrapped as a syscall so e.g.
`sw/apps/wm`'s dock (see `docs/window_manager.md`, "The dock") can
call it too. Takes a bare filename (`z_proc_run("term")`, no path or
extension -- same name you'd type after `run` at the shell prompt),
returns the new pid or 0 on failure (file not found, or no free
process slot).

The kernel (`sw/os/*`) doesn't link `zeitlos.c` itself -- it *is* the
privileged side these wrappers are calling into, so it has its own,
separate implementations of the few things it also needs (messaging,
`maskirq()`; see `sw/os/msg.c`/`sw/os/kernel.h`) rather than routing
through the trampoline it owns. `sw/common/zstream.c` is written to
work either way, for exactly this reason -- see `docs/networking.md`.

## Terminal I/O

`_read()`/`_write()` (newlib's stdio hooks) and `getch()`/
`readline()`/`echo()`/`noecho()` are all built on the four UART
syscalls above -- `_write()` translates `\n` to `\r\n` for a real
terminal, `readline()` handles backspace/delete with the VT100
cursor-left-space-cursor-left dance, `echo()`/`noecho()` toggle
whether typed characters get echoed back (`term_echo`, checked by
both `_read()` and `readline()`). `VT100_*` macros (`zeitlos.h`) cover
the handful of escape sequences these use.

Note this is genuinely different from `sw/os/kruntime.c`'s
kernel-side `getch()`/`readline()`/`echo()`/`noecho()` -- same names,
same job, but talking to the UART hardware/software FIFO directly
rather than through a syscall, since the kernel doesn't need the
trampoline to reach its own hardware. This is *why* `sw/os/sh.c`
can't link `zeitlos.c` even though it wants `z_msg_*`/`z_obj_*` --
linking both would collide on these four names. `sw/os/msg.c`'s
kernel-side messaging wrappers exist specifically so `sh.c` gets
`z_msg_send`/`z_msg_read`/`z_msg_new_send`/`z_uptime_ticks` (matching
signatures, see the table above) without dragging in `zeitlos.c`'s
copy of `getch()` and friends.

## Messaging

`z_msg_send()`, `z_msg_read()`, `z_msg_new_send()`, `z_msg_wait()`
(blocking, discards non-matching messages) are thin wrappers around
the two messaging syscalls -- see `docs/messaging.md` for the object
system and mailbox model these operate on; this file only exists to
point at that one so app code and this document don't drift apart
the way a few other things in this project already have.

## Direct hardware register access

`zeitlos.h` `#define`s every peripheral register this SoC exposes as
a plain volatile pointer dereference -- `reg_uart0_*`, `reg_led`/
`reg_leds`, `reg_usb_*`, `reg_sdcard`, `gpu_*` (line rasterizer),
`gpu_blit_*` (blitter), `GLYPH_MEM_*`, `reg_eth`, `reg_mtu`. Any app
can read/write any of these directly, with zero OS-level mediation --
see "Trust model" below for what that means and doesn't mean.

Most of the actual protocol/timing knowledge for using any of these
correctly lives with their respective drivers/consumers, not here --
`sw/apps/net/enc28j60.c` for `reg_eth`, `zgfx.c` for the GPU
registers, `sw/os/fs/*` for `reg_sdcard`. This header is just the
address map.

## `maskirq()`: protecting multi-register hardware sequences

A raw PicoRV32 custom instruction (`.insn r 0x0B, 0x6, 0x03, ...`),
not privilege-gated, callable directly by any app. Sets a new
interrupt mask and returns the *previous* one, so the normal pattern
is save-mask-restore:

```c
uint32_t old_mask = maskirq(0xFFFFFFFF);   // mask everything
... a short, multi-register hardware sequence that must not be
    interrupted partway through ...
maskirq(old_mask);                          // restore whatever it was before
```

Always restore the *previous* mask, not a hardcoded "unmask
everything" -- this same sequence could itself be running inside an
already-masked outer context (nested use), and hardcoding would
incorrectly unmask that outer context's interrupts early.

Two current users, both protecting the same underlying kind of
hazard -- a sequence of MMIO register writes plus a trigger that has
to complete as one atomic unit, because the peripheral has no queue/
buffering that would tolerate the sequence being interrupted
partway through:

- `sw/apps/net/enc28j60.c` -- every SPI bit-bang transaction (a
  single `reg_eth` bit toggled many times to clock a byte in/out) is
  wrapped, because a timer/UART IRQ firing mid-transaction stretches
  whichever clock half-cycle it interrupted by however long the
  interrupt takes to service -- a real SPI timing violation.
- `zgfx.c`'s `z_fb_hw_line()` -- see below.

Keep the masked region as short as possible, and never include
anything that can block for an unbounded time (like waiting on a
hardware FIFO to drain) inside it -- that would stall the *scheduler*
for every other process, not just delay the one holding the mask. See
`z_fb_hw_line()` below for a concrete example of drawing that
boundary correctly.

## The GPU line rasterizer

`rtl/gpu/gpu_raster.v`, registers `gpu_x0`/`y0`/`x1`/`y1`/`color`/
`start`, plus `gpu_clip_x0`/`y0`/`x1`/`y1`/`enable`. Draws one line
per trigger, hardware-clipped if enabled (inclusive bounds on both
ends -- confirmed directly against the RTL: `cur_x >= clip_x0 &&
cur_x <= clip_x1`, etc.).

This hardware has no concept of "which process is asking" -- it's
global, shared peripheral state, sitting behind ordinary memory-
mapped registers with no per-process view, no locking, nothing. Two
hazards follow directly from that:

1. **Whoever wrote the clip registers last wins, for every
   subsequent caller, not just themselves.** `wm.c` draws its own
   chrome unclipped; `gpu3d`/`gpudemo` draw clipped to their own
   window.
2. **The register-writes-then-trigger sequence isn't atomic.** Six-plus
   separate MMIO writes (clip bounds, `x0`/`y0`/`x1`/`y1`/`color`,
   then `start`) with the scheduler able to preempt between any of
   them. A process preempted partway through can have another
   process's own complete sequence interleaved into it -- the
   trigger that actually fires could end up carrying a mix of two
   callers' parameters.

Both are closed by `z_fb_hw_line()`/`z_fb_hw_box()` (`zgfx.c`), the
only sanctioned way into this hardware -- `wm.c`, `gpu3d`, and
`gpudemo` all draw through these rather than touching the registers
themselves:

```c
void z_fb_hw_line(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip) {

	gpu_wait_fifo();                 // outside the masked section --
	                                  // see below for why

	uint32_t old_mask = maskirq(0xFFFFFFFF);

	if (clip) {
		gpu_clip_x0 = clip->x0; gpu_clip_y0 = clip->y0;
		gpu_clip_x1 = clip->x1; gpu_clip_y1 = clip->y1;
		gpu_clip_enable = 1;
	} else {
		gpu_clip_enable = 0;
	}

	gpu_x0 = x0; gpu_y0 = y0; gpu_x1 = x1; gpu_y1 = y1;
	gpu_color = color & 1;
	gpu_start = 1;

	maskirq(old_mask);

}
```

Fixing hazard (1) is `clip`'s job: reasserted *fresh, on every single
call*, never assumed to still be correctly set from a previous call
-- `clip == NULL` explicitly disables clipping (for `wm.c`'s chrome,
which draws at coordinates it already knows are valid and never wants
clipping at all) rather than leaving the hardware's clip state
ambiguous. Fixing hazard (2) is `maskirq()`'s job, scoped precisely:
it wraps only the register-writes-then-trigger sequence, not
`gpu_wait_fifo()` beforehand. That distinction matters --
`gpu_wait_fifo()` can legitimately take a while if the FIFO's backed
up, and masking through that would stall the *scheduler* (blocking
every other process from running at all, not just delaying this
draw) for as long as the wait takes. The masked section itself is a
handful of single-cycle MMIO stores -- at this SoC's clock rate,
worst case is on the order of a few hundred nanoseconds, against a
~1.37ms timer-IRQ period (~732Hz) -- not something a preempted
process elsewhere in the system could ever notice.

`z_win_hw_line()`/`z_win_hw_box()` (`zwin.h`) are the window-aware
version of the same two functions, automatically clipping to the
caller's own window content area -- see `docs/window_manager.md`,
"Drawing content" for those, and for `z_win_content_rect()`, which
exposes that content-area rectangle to any app that needs to know its
own drawable bounds for something other than drawing (centering
content, bouncing something off the edges) without duplicating the
formula itself.

## The GPU blitter

`rtl/gpu/gpu_blit.v`, registers `gpu_blit_ctrl`/`_status`/`_dst_x`/
`_dst_y`/`_width`/`_height`/`_pattern` (fill/copy mode) plus
`_glyph_addr`/`_w`/`_h`/`_fg_color`/`_bg_color` (glyph mode) -- see
`docs/gpu_blitter.md` for the full register map and both modes' wire
protocol. A separate piece of shared hardware from the line
rasterizer above, currently used two ways:

- **Fill mode**, via `zgfx.c`'s `z_fb_hw_fill_rect()` -- a
  hardware-accelerated rectangle fill, dramatically faster than
  iterating pixels in software (`docs/gpu_blitter.md` quotes ~1,200
  hardware operations for a full-screen clear, against 640×480
  individual bit operations the software path would need). `wm.c`'s
  `fill_rect()`/`clear_screen()` both draw through this now, not a
  software VRAM loop.
- **Glyph mode**, via `z_fb_draw_char()`/`z_fb_draw_text()` in
  `Z_GFX_HW_BLIT` builds -- see `docs/window_manager.md`, "Hardware
  glyph blitting" for that design in full; not covered again here.

The blitter has the same two classes of hazard as the line
rasterizer above:

1. **Bounds checking is conditional, not automatic.** It's only
   applied when `CTRL_CLIP` is set -- see `docs/gpu_blitter.md`,
   "Clipping Behavior" for the details and why an unclipped
   out-of-range destination can hang the blitter's state machine.
2. **No arbitration between processes.** Register writes aren't
   serialized across processes -- see `docs/gpu_blitter.md`, "Error
   Handling".

`z_fb_hw_fill_rect()` closes both, for the fill path: coordinates are
clamped to the actual screen bounds unconditionally (not just when
`CTRL_CLIP` happens to be requested), the register-writes-then-trigger
sequence is `maskirq()`-protected, and `gpu_blit_wait_idle()` (the
busy-wait on `gpu_blit_status`) is bounded and reports if it ever
times out, rather than spinning forever -- the same defensive pattern
as `gpu_wait_fifo()` for the rasterizer.

One difference from the rasterizer functions worth calling out:
`z_fb_hw_line()` only waits for FIFO *space* before submitting, then
returns immediately, letting the hardware drain asynchronously in the
background -- consecutive calls naturally pipeline. `z_fb_hw_fill_rect()`
instead waits for the *entire* operation to actually finish before
returning. The blitter has no FIFO or queue at all (one operation at a
time, globally shared, exactly the "no arbitration" hazard above), and
critically, whatever a caller does immediately after a fill is very
often *not* going through this same hardware -- `wm.c`'s own
`draw_window_box()`, called right after `fill_rect()` in
`repair_region()`, draws through the line rasterizer instead. Unlike
the glyph-blit path (where consecutive glyphs share the same
hardware, so each one's own "wait before I start" naturally serializes
against the previous one, and only the very last glyph in a run needs
an explicit final wait -- see `z_fb_draw_text()`), there's no other
point in a fill-then-different-hardware sequence where that next
operation would otherwise wait for the fill's pixel writes to have
actually landed.

**Known gap**: the glyph-blit path above shares these same registers
but isn't `maskirq()`-protected -- a fill from one process and a
glyph blit from another could still interleave badly.

## Trust model

There's no memory protection between processes (flat physical memory,
see `docs/messaging.md`), and every register described above is a
plain, unguarded volatile pointer dereference available to any app
that includes `zeitlos.h` -- nothing at the OS level stops an app
from writing directly to `gpu_x0` and racing every hazard described
above, or writing straight to VRAM outside its own window. The
higher-level layers this document describes (`z_fb_hw_line()`,
`z_win_*`) are the *compliance mechanism*: the easy, correct, natural
way to do these things, not a hard guarantee. See
`docs/window_manager.md`, "App trust model" for the same point made
about window boundaries specifically.

## Known limitations and historical notes

- **Line rasterizer FIFO check/mask gap.** `z_fb_hw_line()` waits for
  FIFO space *before* masking IRQs, then masks separately for the
  actual register writes. Another process could theoretically claim
  the last free FIFO slot in the narrow window between the wait
  returning and the mask taking effect. This is bounded and
  low-severity (worst case is a delayed or dropped line segment, not
  corruption or a crash) and is not currently a known problem in
  practice; closing it fully would mean either widening the masked
  region to include the wait (which would stall the scheduler for as
  long as the FIFO wait takes) or a more involved retry-inside-mask
  design.
- **Shared clip-register hazard, before it was centralized.** Before
  `z_fb_hw_line()`/`z_fb_hw_box()` existed, `wm.c` and the apps each
  managed the line rasterizer's `gpu_clip_*` registers independently
  and on their own schedule. Because the hardware has no per-process
  clip state, whichever clip rectangle was left behind by the last
  caller silently applied to the next caller too -- symptoms included
  window borders vanishing (clipped away by another window's bounds)
  or content escaping its own window. This is fixed by routing all
  drawing through `z_fb_hw_line()`/`z_fb_hw_box()`, which reassert
  clip state fresh on every call. See `docs/window_manager.md`,
  "Known limitations" for the full incident.

## DMA masters bypass the MTU

The MTU (`rtl/mtu.v`) translates addresses **the CPU issues**. It is not
a system-wide MMU, and nothing else on the bus goes through it.

That matters now that the GPU blitter can read main memory
(`docs/gpu_blitter.md`, "Copy modes"). The blitter is its own bus
master, so an app pointer -- virtual `0x8000_xxxx` -- means nothing to
it. Handing one over points it at whatever lives at *physical*
`0x8000_0000`, which is not that app's data and quite possibly not
memory at all.

Software translates before handing an address to any such master:

```c
phys = reg_mtu_base ? reg_mtu_base + (virt & 0x0FFFFFFF) : virt;
```

`reg_mtu_base` (`sw/common/zeitlos.h`) reads the MTU's own translation
base. It is readable from an app because only `0x8xxx_xxxx` is
translated, so the load reaches the MTU rather than being remapped. A
base of 0 means no translation is active (the kernel's own context) and
the address is already physical. `z_fb_hw_blit_mem()` does this for you;
anything else driving a bus master with an app pointer must do the same.

A physical address stays valid across a context switch, so latching one
before starting a long operation is safe -- which is what the blitter
does.

## Idling: why background processes must block

The scheduler divides the CPU between **runnable** processes. A process
that spins in its main loop is runnable forever, so it takes a full
share whether or not it has anything to do — and that share comes
directly out of whatever is in the foreground.

This was measured, not theorised. `gamedemo` running full-screen showed
`dt 4-5` (four to five display frames per update) with `wm`, `net` and
`repl` alongside it, and `dt 2` with them killed. `ps` showed every
process in `run` with no `Z_PROC_FLAG_BLOCKED` (0x4) set.

**A quarter of the CPU means a quarter of the frame rate**, and for a
full-screen app the frame rate *is* the smoothness — there is no amount
of interpolation that makes 15 positions a second look like 60.

### The primitive

`z_proc_wait(timeout_ticks)` blocks until a message arrives or the
timeout expires; 0 waits indefinitely. It handles the lost-wakeup race
properly, testing the mailbox in the same syscall that sets
`Z_PROC_FLAG_BLOCKED` (`k_proc_wait()` in `sw/os/kernel.c`), so a
message arriving between a mailbox read and the block wakes the process
rather than being missed.

### Who blocks, and how

| process | wait | why |
|---|---|---|
| `calc`, `settings` | `z_proc_wait(0)` | purely message-driven |
| `clock` | `Z_TICK_HZ / 8` | redraws on a timer |
| `info` | sample interval | samples periodically |
| `net` | 1 tick | polls the ENC28J60 |
| `repl` | `Z_TICK_HZ / 20` | message-driven; see below |
| `wm` | 1 tick | polls the pointer |

**`repl`** was a pure spin — it read an empty mailbox and went round
again, forever. Every branch in its loop is message-driven, so it could
block indefinitely with `z_proc_wait(0)`. A timeout is used instead
because the port layer has retransmit and connection state that is
currently advanced only when a message happens to arrive; blocking
forever is correct today and would stall silently the first time that
stops being true. Waking 20 times a second costs nothing and removes
the trap.

**`wm`** cannot block indefinitely: the pointer is polled from
`usb_hid.v`'s cursor register, not delivered as a message, so nothing
would wake it when the mouse moves. One tick wakes it at ~732Hz — over
twelve times the display refresh, far finer than anyone can see in a
pointer — while returning the ~99% of each timeslice previously spent
re-reading an unchanged register. An arriving message cuts the wait
short, so app requests are still serviced immediately.

### Still outstanding

**Process 0** (kernel and serial console) does not block. It was left
alone deliberately: it may double as the idle fallback, and making the
scheduler's own process blockable is not a change to guess at. With
everything else killed the game still measured `dt 2`, so pid 0 is worth
investigating — but the remaining gap there is as likely to be the
drawing as the scheduling.

**`net` polls the ENC28J60** on a 1-tick timer. Its own comment notes
the controller's INT pin is already wired to `spim.v`'s STATUS bit 2, so
waking on the interrupt would be strictly better than a timer.

## term and text were spinning too

Measured on hardware with the render instrumentation:

```
term: 2869 renders, 0 rows, 0 glyphs, 2869 no-ops     <- completely idle
```

**1435 renders per second while doing nothing.** Every one called
`render()`, found nothing dirty and returned. Cheap individually,
ruinous collectively — the scheduler divides the CPU between *runnable*
processes, so an idle terminal took a full share from whatever was in
the foreground.

`text` was the same. Both now `z_proc_wait(Z_TICK_HZ / 30)`.

That is the same fault `wm` and `repl` had. Four of the five processes a
normal desktop runs were spinning, which is most of the explanation for
a full-screen app measuring a quarter of the machine.

## Text rendering is not the bottleneck

Worth recording, because it is where the optimisation effort was
originally aimed:

| | glyphs/sec | CPU |
|---|---|---|
| term, typing | 1366 | **0.3%** |
| term, scrolling output | 5005 | **1.2%** |
| text, scrolling | 4560 | **1.1%** |

At the measured ~112 cycles per glyph, none of it is close to
significant. The idle spinning cost far more than the drawing ever did.

The one visible cost is a **burst**: a full 80x25 repaint is 2000 glyphs
= 224,000 cycles = **4.7ms**, which is a perceptible hitch when output
scrolls. That is a latency problem, not a throughput one, and the fix is
scroll-by-blit rather than anything per-glyph.

### term redrew whole rows

`vt` tracks dirt per ROW, so typing one character marked its row dirty
and `render()` redrew all 80 columns — measured at exactly 80 glyphs per
keystroke where 1 would do.

`render()` now keeps a **shadow of what is actually on the glass** and
draws only cells that differ. The dirty flags still decide which rows
are worth examining, so a quiet screen costs nothing; the shadow decides
what within them is worth drawing.

Pushing cell-level tracking into `zvt100` would have meant touching
every routine that writes a cell. The shadow needs 4KB and no changes
to the VT model at all.

It also covers a case dirty flags never could: a row marked dirty whose
contents happen to be unchanged now draws nothing.

**The shadow must be invalidated whenever the window is repainted from
outside** — a move, an occlusion, a `Z_WM_REDRAW`. Otherwise it still
agrees with the model, `render()` draws nothing, and the terminal is
left blank. All three such sites call `shadow_invalidate()`.

## Every app now yields

Audited all twenty. Eleven were spinning; the rest already used
`z_proc_wait`.

| policy | apps | wait |
|---|---|---|
| message-driven | `wm`, `repl`, `term`, `text`, `files`, `draw`, `hello_win`, `portdemo`, `ping` | `Z_TICK_HZ / 30` |
| animated / periodic | `gpu3d`, `space3d`, `pong`, `track` | 1 tick |
| already blocking | `calc`, `clock`, `info`, `net`, `read`, `settings` | unchanged |

**Six of the twelve dock apps were spinners** — `term`, `text`, `files`,
`draw`, `track`, `space3d`, `gpu3d` — so a desktop with a couple of
windows open was dividing the CPU among several processes that were
doing nothing.

Animated apps get one tick rather than a longer sleep. They genuinely do
need to run every frame, but `z_proc_wait(1)` still returns essentially
the whole timeslice: waking at 732Hz is more than twenty times any of
them needs, and an arriving message cuts the wait short.

`track` gets the same, and it is the one with a hard constraint —
feeding the mixer. Safe by a wide margin: the audio FIFO holds several
milliseconds and the hardware mixer is fed on tracker ticks at about
50Hz, against a 1.37ms wake period.

## Scrolling by blit: 4.4x

For an 80x25 terminal (400x200 px at 5x8):

| | cycles | time |
|---|---|---|
| full re-render, 2000 glyphs | 224,000 | 4.67 ms |
| VRAM to VRAM blit of the text area | 42,432 | 0.88 ms |
| plus the one new row, 80 glyphs | 8,960 | |
| **total** | **51,392** | **1.07 ms** |

Worth doing, and it is the only remaining text cost that is visible: a
perceptible hitch every time output scrolls.

**Upward scroll only.** The blitter walks rows top-down, so copying a
region UP is safe — each row is read before anything overwrites it.
Scrolling down would need a bottom-up walk, which the hardware cannot
do, and must stay a re-render. That is the rarer case in a terminal and
in a reader.

It applies equally to `read` and `text`, which scroll far more than a
terminal does. Neither is CPU-bound today (both measured near 1%), so
this is about latency, not throughput.
