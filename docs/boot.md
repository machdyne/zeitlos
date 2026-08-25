# Boot

What happens between power-on and a usable desktop, and the two places
you can intervene.

## Sequence

```
gateware  ->  BIOS (BRAM)  ->  kernel (flash -> RAM)  ->  sh()  ->  init()  ->  wm, net, repl
```

1. **BIOS** (`sw/bios/bios.c`) runs from block RAM baked into the
   bitstream. It draws the boot splash, then `memcpy()`s the kernel
   from flash into main memory (`load_zeitlos()`). A keypress during
   its `AUTOLOAD_CNT` window drops you at the BIOS monitor instead.
2. **Kernel** (`sw/os/kernel.c`) redraws the splash, brings up UART,
   HID and the memory pool, registers itself as pid 0, and calls
   `sh()`.
3. **`sh()`** (`sw/os/sh.c`) mounts the filesystem, offers the
   **init-cancel window** below, then runs `init()`.

   If the core apps are in flash and nothing on the card shadows them,
   this happens immediately. Otherwise it waits up to
   `AUTOINIT_TIMEOUT_TICKS` (~3s) for a slow sdcard to become
   readable -- pointless on a board with no card at all, which is why
   the flash case is checked first. See
   [`flash_apps.md`](flash_apps.md).
4. **`init()`** starts `wm`, `net` and `repl`, in that order. `term` is
   launched on demand from wm's dock rather than at boot. `wm`'s
   startup `clear_screen()` is what wipes the splash.

   Each app is resolved independently: filesystem first, flash
   underneath, and the source is printed (`init: wm (flash)`).

## Cancelling init

```
starting init in 500ms -- press ESC to cancel ...
```

500ms, right after the filesystem mounts and *before* the wait for apps
to appear (that wait can block for seconds; you shouldn't have to sit
through it to bail out). ESC drops you at the shell prompt with the
graphical environment never started; `init` runs it by hand once
whatever was wrong is sorted.

This exists because once `wm` starts it clears the screen and takes
over, and if something in the graphical stack is broken -- a bad `wm`
build, an app that wedges, a display showing nothing -- there was
otherwise no way back to the console short of reflashing.

**Serial console only.** `k_uart_getc()`/`k_uart_rx_empty()`
(`sw/os/uart.h`) read the UART and nothing else; the USB keyboard is an
entirely separate path (`z_hid_read_key()`, `sw/os/hid.c`) that isn't
polled here. That's structural, not policy, and it's the right split:
this is a recovery mechanism for when the graphical side is what's
broken, so it should depend on as little of the system as possible, and
the console is the one interface guaranteed to work when the display
isn't.

**ESC specifically, not any key** (unlike the BIOS, which has a much
longer window and a prompt you're already watching). A byte left in the
FIFO from what you typed at the BIOS prompt, or line noise, would
otherwise silently drop you at a bare shell wondering where your
desktop went. Held ESC works fine -- key repeat sends repeated `0x1b`.

## The splash lives in flash

The logo used to be a compiled-in `const uint8_t[24576]`
(`sw/os/logo_data.c`). Because `k_proc_create()` sizes a process's
memory block from its image, that cost **24KB of the 1MB main-memory
budget permanently**, for something displayed once for a couple of
seconds. On the minimum-spec board that was the difference between
fitting a second `term` and not.

Flash is memory-mapped on this SOC -- the BIOS's `load_zeitlos()` is a
plain `memcpy()` out of `ROM_OS_ADDR` -- so the splash needs no RAM at
all. It's read straight from flash into VRAM.

| | offset | size |
|---|---|---|
| gateware | `0x000000` | ~400KB (varies by board) |
| **logo** | **`0x0F0000`** | **38,400 bytes** |
| kernel | `0x100000` | 256KB |

The flashed artifact is `sw/data/images/zeitlos_fb.bin`: a full 640x480
1bpp framebuffer image, pre-centred and pre-padded from the 512x384
`zeitlos.bin` by `sw/data/images/pad_logo.py`. Doing the centring once,
at build time, is what lets both the BIOS and the kernel display it
with a **single flat `memcpy`** instead of a row-by-row copy with
per-row offset arithmetic -- which matters in the BIOS, where the
budget is measured in bytes against `BRAM_WORDS`. It also means the
splash clears VRAM rather than leaving a border of reset garbage around
the logo.

Polarity is baked in the same way: `pad_logo.py --invert` flips every
bit at build time. If the splash ever shows with foreground and
background swapped, regenerate the image rather than changing any C.

```
make flash_logo
cd sw/data/images && python3 pad_logo.py zeitlos.bin zeitlos_fb.bin
```

`flash_logo` is part of the full `make flash` chain. There is no
is-it-programmed check anywhere: the logo is flashed alongside the
gateware and kernel, so a board that can boot at all has it.

**The offset appears in three places** -- `Z_BOOT_LOGO_FLASH_OFFSET`
(`sw/os/logo.h`), `ROM_LOGO_ADDR` (`sw/bios/bios.c`), and
`LOGO_FLASH_OFFSET_HEX`/`_DEC` (top-level `Makefile`) -- with no
build-time link between them, because all three live in separately
built artifacts: the BIOS is baked into the bitstream's BRAM, the
kernel is a flashed binary, and the Makefile drives an external
flashing tool. A mismatch shows up only as a missing or garbled splash.

### Why you may barely see it

The splash is up from the moment the BIOS draws it until `wm`'s
`clear_screen()`. On a monitor that takes a second or two to sync,
most of that window is gone before anything is visible. Drawing it
earlier (the BIOS, rather than only the kernel) can only ever make the
visible window longer, never shorter -- if it seems to vary between
boots, that's monitor sync variance, not the software.

The init-cancel window above adds 500ms to every boot, which is 500ms
more splash time as a side effect. If you want more than that, the
lever is a **minimum splash time**: a floor in `sh()` before `init()`
runs, costing nothing when boot already took longer. Don't put that
wait inside `wm` before `clear_screen()` -- `wm` would stop servicing
messages during it, and `term`'s `z_win_create()` has a timeout.

## What the boot log tells you

```
ZEITLOS

 - uart initialized.
 - soc features:
     memory  sram vram rom
     gpu     gpu raster blit cursor vga
     input   uart0 usb-hid
     storage sdcard flash
     network spi-eth
     led     debug
 - hid initialized.
 - main memory: 1MB
 - memory initialized.
 - kernel process size 233472
 - kernel active.
 - cpu: 48.0 MHz, 12.34 MIPS (0.26 IPC)
 - starting shell.
```

The feature list comes from `rtl/csrs.v`'s bitmap of what was actually
synthesized into the running bitstream (`Z_FEATURE_*`, `sw/common/zsoc.h`),
printed by `k_soc_report()` in `sw/os/kernel.c`.

This turns a whole class of confusing bring-up failure into a glance at
the log. "The network doesn't work" on a board whose bitstream simply
has no ethernet PHY looks identical, from software, to a driver bug --
until the boot log says which one it is. Same for a missing GPU, a
board built without VRAM, or an SD interface that was never wired in.

Grouped rather than dumped flat or as hex, because the groups are how
you actually reason about a board.

The bit/name/group table lives in **`sw/common/zsoc.c`**, next to the
`Z_FEATURE_*` defines it mirrors, so everything that has to track the
RTL is in one directory -- adding a feature is one bit in `zsoc.h` and
one row in `zsoc.c`, rather than a bit in a header and a table buried
in whichever consumer first wanted to print it. The table is **data
only**: `k_soc_report()` owns the layout, because a shared file pulling
in `printf()` would be unusable from any context without stdio, and
would bake one consumer's formatting into everyone else's. Rows must
stay sorted by group -- consumers start a new line when the group
changes, so an out-of-order row duplicates a heading rather than
erroring.

**The bit positions are kept in sync with `rtl/sysctl.v`'s
`CSR_FEATURES` localparam by hand** -- `zsoc.h` says so explicitly, and
there is no shared source between the Verilog and C sides. Worth
sanity-checking the output against what a board actually has: a feature
that is obviously present but missing from the list (say `flash`, on a
board that demonstrably boots from it) points at the RTL side not
setting the bit, not at a software decode problem. Adding a feature is one line in
`feat_bits[]` plus the bit in `zsoc.h`; entries must stay sorted by
group, since the printer starts a new line whenever the group changes.

On a bitstream predating `rtl/csrs.v` there is nothing mapped at
`0x7000_0000`, and the log says **`features unknown`** rather than
printing an empty list -- "can't confirm" is a genuinely different
answer from "no", and `zsoc.h` is careful about that distinction
throughout (see `z_soc_has_feature()` vs
`z_soc_feature_confirmed_absent()`).

## CPU speed check

```
 - cpu: 6.93 MIPS @ 48 MHz (0.14 IPC)
```

picorv32 is instantiated with `ENABLE_COUNTERS`/`ENABLE_COUNTERS64` at
their defaults (`rtl/sysctl.v` overrides neither), so `rdcycle` and
`rdinstret` are real hardware counters. `k_cpu_report()`
(`sw/os/kernel.c`) reads both across a ~50ms window.

**The clock is stated, not measured** -- `Z_SYSCLK_HZ`
(`sw/common/zsoc.h`), fixed at 48MHz on every board. It is chosen over
an otherwise-rounder 50 because it divides cleanly for a 1Mbaud UART.

This SOC *cannot* measure its own clock, and it's worth understanding
why, because the opposite is an easy assumption to make. `rtc_ctr` in
`rtl/sysctl.v` -- the counter that generates the KTIMER interrupt --
is clocked from `sys_clk`, the same clock `rdcycle` counts:

```verilog
always @(posedge sys_clk) begin
    rtc_ctr <= rtc_ctr + 1;
end
```

So cycles-per-tick is *always exactly 65536*, whatever the PLL is
really producing. Any "measured MHz" derived from those two counters is
a tautology that hands back the constant it started from -- an earlier
version of this function did exactly that and would have printed ~48MHz
on a board clocked at 24. The UART baud divisor comes off `sys_clk`
too, so there is no independent time reference anywhere on chip. A
misconfigured PLL shows up as everything running proportionally fast or
slow, with nothing reporting it.

Given that, **MIPS** is computed from the two counters and the stated
clock (`di/dc` is the measured part, `Z_SYSCLK_HZ` scales it) rather
than from elapsed wall time, which would have inherited the assumption
twice over. **IPC is the only figure here that depends on no assumption
at all** -- it's a ratio of two hardware counters.

An IPC around 0.14 (CPI ~7) is normal: picorv32 is multi-cycle, has no
cache, and fetches every instruction over Wishbone from external RAM.
What you're mostly measuring is instruction fetch, not the ALU.

MIPS is also a compute-bound best case, not an average. `rdinstret`
counts whatever retires, so the same measurement taken while polling a
UART register would mostly report Wishbone stalls. The benchmark loop
touches no peripherals and avoids multiply (`ENABLE_MUL` is 0, so a `*`
would become a libgcc call and measure that instead).

Only the low 32 bits of each counter are read; at ~50ms that's a few
million counts, nowhere near a wrap. The check must run after
`reg_kernel` is set, since `z_kernel_ticks` only advances once the IRQ
handler is installed -- the cycle counter is independent of that, so it
doubles as an escape hatch: if ticks never advance, the check gives up
and says so rather than hanging the boot.

## Memory budget

`k_proc_create()` allocates `max(align_up(image + stack, 4096), 32768)`
per process, out of a 1MB pool on the minimum-spec board. Stack tiers
are in `sw/os/kernel.h`.

Because a process's block is sized from its **image**, every byte of
`.rodata` and `.bss` in a binary costs a byte of RAM for that process's
whole lifetime. That's why moving 24KB of logo data out of `kernel.bin`
freed 24KB of RAM, and why `--gc-sections` (on by default in every app
Makefile, `GC_SECTIONS=0` to disable) is worth real memory rather than
just disk.

### Stack tiers

`Z_PROC_STACK_SIZE_*` (`sw/os/kernel.h`) is the stack **and malloc
heap** a process gets on top of its image. It is *not* where static
data lives -- code, `.rodata` and `.bss` are in the binary, and
`k_proc_create()` allocates image + tier. So `repl`'s 96KB Scheme cell
heap (`MS_HEAP_SIZE * sizeof(ms_val)`, a `.bss` array in `ms.o`) is
completely unaffected by its tier: moving repl from LARGE to MEDIUM
costs zero Scheme cells.

| tier | size | processes |
|---|---|---|
| SMALL | 8KB | `wm`, `term` |
| DEFAULT | 16KB | anything unnamed |
| MEDIUM | 32KB | `net`, `repl` |
| LARGE | 64KB | (none -- kept for a one-word revert) |

With those, and `GC_SECTIONS=1`, the 1MB board fits
`wm net repl term0 term1` with ~44KB spare:

| | image | tier | block |
|---|---|---|---|
| kernel | ~195K | 16K | 208K |
| wm | 93,884 | 8K | 100K |
| net | 121,004 | 32K | 152K |
| repl | 293,048 | 32K | 320K |
| term0 | 92,992 | 8K | 100K |
| term1 | 92,992 | 8K | 100K |
| | | | **980K of 1024K** |

SMALL is a return to what this project originally shipped with;
DEFAULT doubled it as a blanket margin, and `kernel.h` notes no app
ever showed a confirmed need for more. `wm` and `term` are plain
message-loop apps, so they're the two with least reason to pay it.

For `net` and `repl`, MEDIUM became defensible once the unbounded
per-message `zport` leak was fixed (`Z_PORT_DATA_ACK`). repl reports
its own baseline at every boot -- `heap grown 5960 bytes by end of
stdlib load` -- leaving ~26KB of headroom at 32KB.

**The residual risk for repl is stack, not heap.** Deep non-tail Scheme
recursion nests `ms_eval()` frames on the C stack, bounded by
`MS_PROTECT_STACK_SIZE` (192). That should sit comfortably inside 32KB,
but it's worth exercising with something deliberately recursive. The
failure mode is silent exhaustion, not a clean error; `(free)`'s
`c-heap` figure is the number to watch, and putting repl back on LARGE
is a one-word revert.

`free` (shell) and `(free)` (Scheme) report the pool; `df` and `(df)`
report the SD card. See `docs/scheme_api.md` for the Scheme forms,
which return data rather than printed text.
