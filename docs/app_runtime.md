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

Adding a new syscall means adding a `Z_MKSYSCALL(...)` line to
`syscalls.def`, a handler in the kernel, and (usually) a thin
app-side wrapper here following the same pattern.

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
real hazards follow directly from that, and both bit this project for
real before being fixed:

1. **Whoever wrote the clip registers last wins, for every
   subsequent caller, not just themselves.** `wm.c` draws its own
   chrome unclipped; `gpu3d`/`gpudemo` draw clipped to their own
   window. Before this was centralized, each of those files managed
   `gpu_clip_*` itself, on its own schedule (`wm.c` never touched it
   at all; the apps set it only when their own window moved) -- so
   once any app had run at all, whichever clip state it left behind
   silently applied to every other process's next draw call too,
   regardless of what that process actually wanted. Symptom: window
   borders vanishing (clipped away by some other window's bounds) or
   content escaping its own window (drawn while clipping had been
   left disabled by something else's chrome draw). See
   `docs/window_manager.md`'s "Known limitations" for the full
   incident.
2. **The register-writes-then-trigger sequence isn't atomic.** Six-plus
   separate MMIO writes (clip bounds, `x0`/`y0`/`x1`/`y1`/`color`,
   then `start`) with the scheduler able to preempt between any of
   them. A process preempted partway through can have another
   process's own complete sequence interleaved into it -- the
   trigger that actually fires could end up carrying a mix of two
   callers' parameters.

Both are closed by `z_fb_hw_line()`/`z_fb_hw_box()` (`zgfx.c`), the
only sanctioned way into this hardware now -- `wm.c`, `gpu3d`, and
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

One residual, accepted gap: `gpu_wait_fifo()`'s check and the
subsequent masked write aren't a single atomic unit either -- another
process could theoretically claim the last free FIFO slot in the
narrow window between this call's wait returning and its own mask
taking effect. Bounded and low-severity (worst case is a delayed or
dropped line segment, not corruption or a crash), and would need
either widening the masked region to include the wait (reintroducing
the scheduler-stall problem above) or a more involved retry-inside-
mask design to close -- not done, not currently a known problem in
practice.

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
  hardware operations for a full-screen clear, against 512×384
  individual bit operations the software path would need). `wm.c`'s
  `fill_rect()`/`clear_screen()` both draw through this now, not a
  software VRAM loop.
- **Glyph mode**, via `z_fb_draw_char()`/`z_fb_draw_text()` in
  `Z_GFX_HW_BLIT` builds -- see `docs/window_manager.md`, "Hardware
  glyph blitting" for that design in full; not covered again here.

Same two hazards as the line rasterizer, confirmed directly in the
RTL rather than assumed by analogy, and only one of them fixed so
far:

1. **Bounds checking is conditional, not automatic.** `docs/gpu_blitter.md`
   documents the blitter as safe against out-of-range coordinates
   ("Automatic Clipping... Prevents buffer overruns") -- true, but
   only when `CTRL_CLIP` is set, and the same doc recommends
   *skipping* it for "known-safe" operations like full-screen clears.
   In `ST_CLIP` (`gpu_blit.v`), the unclipped path computes the
   destination address directly from the caller's raw
   `dst_x`/`dst_y`/`width`/`height` with no bounds check of any kind
   -- only the clipped path checks against `screen_clip_x_end`/
   `_y_end`. And exactly like the rasterizer, `ST_WAIT_READ`/
   `ST_WAIT_WRITE` wait on the framebuffer bus's ack with no timeout
   of their own. An out-of-range destination computed from unclipped
   coordinates can hang the blitter's state machine forever, the same
   way an out-of-range coordinate could hang the rasterizer.
2. **No arbitration between processes.** `docs/gpu_blitter.md`
   documents this directly: "Concurrent register access: NOT
   arbitrated between processes... one process's `dst_x`/`dst_y`/etc.
   writes could land in between another's, corrupting both
   operations." Same underlying problem as the rasterizer's shared
   clip register, just a different register set.

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

**Not yet fixed**: the glyph-blit path above shares these same
registers but isn't `maskirq()`-protected -- a fill from one process
and a glyph blit from another could still interleave badly. Not
unified with the fill-mode fix here, since it wasn't what motivated
this work; worth doing if the glyph path gets touched again for
another reason.

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
