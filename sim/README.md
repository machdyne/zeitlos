# zeitlos-sim

A minimal Linux (x86_64) emulator for testing Zeitlos apps without FPGA
hardware. Runs unmodified app binaries -- exactly the `app.bin` your
app's own Makefile already produces via `objcopy -O binary` -- against
a software model of the SOC's CPU, framebuffer, blitter, and line
rasterizer, and shows the result in a window.

```
$ ./zeitlos-sim sw/apps/bounce/bounce.bin
```

## What's emulated

- **CPU**: a from-scratch **RV32IM** interpreter (`cpu.c`), plus the
  `cycle`/`instret` counter CSRs.

  M was added because `sw/common/arch.mk` now defaults `ARCH` to
  `rv32im` and the boards that matter set `CPU_MUL`/`CPU_DIV` in
  `rtl/boards.vh` -- a simulator that traps on `mul` cannot run
  anything the current toolchain produces. There is no switch: an
  rv32i binary simply contains no M encodings, so supporting them
  costs it nothing, whereas a switch would have to be set correctly to
  get a correct answer.

  The counter CSRs are not a nicety. They used to read as zero, and
  `dly_us()` in `sw/os/fs/fatfs/sdmm.c` spins until
  `rdcycle() - start` reaches a target -- so any code path through it
  **hung forever**. Both `cycle` and `instret` now return the
  retired-instruction count. That is deliberate rather than lazy: this
  simulator models no bus latency, no cache and no multicycle FSM, so
  any "cycle" figure it invented would be a plausible-looking lie.
  Code using `rdcycle` as a monotonic clock works; code using it to
  measure performance gets an obvious instruction count, which is the
  right way to find out that this is not the tool for that.

  Any float math in an app (e.g. `gpu3d.c`) compiles down to ordinary
  soft-float library calls, which are just more integer instructions --
  nothing float-specific to emulate.

- **Memory map** (`machine.c`): matches `sw/common/zeitlos.h` /
  `rtl/sysctl.v`. RAM is backed directly at `0x80000000` (the address
  apps are linked to run at, per `sw/common/riscv-app.ld`) -- the real
  MTU address translation is skipped entirely, since we only ever run
  one app with no OS/scheduler underneath it.

- **VRAM / framebuffer**: 512x384x1bpp at `0x20000000`, matching the
  `GPU_PIXEL_DOUBLE` board configuration (all four current boards in
  `rtl/boards.vh` use this mode).

- **Line rasterizer** (`0xa0000000`): a direct translation of
  `rtl/gpu/gpu_raster.v`'s Bresenham FSM into a host function --
  same algorithm, same clip-rect behavior, same 1000-pixel safety cap.

- **Blitter** (`0xd0000000`): a direct translation of
  `rtl/gpu/gpu_blit.v`'s word-level fill/clip logic. **Copy mode is
  intentionally a no-op**, matching the real RTL as it stands today
  (see `gpu_blit.v`'s "Copy mode - would need source logic" and the
  open GitHub issue #3) -- this simulator aims to be faithful to
  current hardware behavior, not an idealized target.

- **UART** (`0xf0000000`): raw MMIO register writes go to stdout;
  reads come from stdin (put into raw/non-canonical mode so
  `getch()`-style polling gets bytes immediately, matching real UART
  behavior). This is what apps like `gpudemo.c` use directly.

- **Kernel syscall gate** (`reg_kernel` at `0x0000000c`): apps that use
  `printf`/`getch`/etc. (via `sw/common/zeitlos.c`) call through a
  function pointer at this fixed address -- no `ecall`/trap, no real
  kernel binary involved, just a normal RISC-V function call
  `(syscall_id, obj_ptr, irqs)`. The simulator installs its own address
  there and intercepts it in the run loop, implementing `EXIT` and
  `UART_GETC/PUTC/RX_EMPTY/TX_FULL` directly in host code. Everything
  else goes to `simos.c` -- see "The OS layer" below. `UI_PRINT` is
  still stubbed (see "Known limitations" below).

  **Syscall ids are generated from `sw/common/syscalls.def`** by the
  same X-macro that builds the real `z_syscall_id_t`, rather than
  hand-copied. That file's own header carries a long warning about
  never inserting an entry in the middle, because every later id
  shifts silently; a hand-maintained copy here would be a second place
  for that to go wrong, in the very tool you would reach for to debug
  it. The list used to stop at `UART_TX_FULL`.

- **Executable format**: `machine_load_bin()` understands **ZEXE**
  (`docs/executables.md`) and falls back to the old raw format for a
  file with no magic, applying the same rule `z_exec_parse()` does.
  For a ZEXE image it zeroes `.bss` from the header's `bss_size`,
  because nothing else will -- there is no crt0 doing it on this OS.
  A loader that skipped that would hand the app whatever the previous
  run left there, and would appear to work whenever the host's fresh
  allocation happened to be zero. This tree has been bitten by
  uninitialised `.bss` on real hardware twice (see
  `docs/app_runtime.md`); the last thing a simulator should do is fail
  to reproduce it.

- **Small stubs**: LEDs (`0xe0000000`), a USB mouse cursor register
  (`0xc000000c`, driven from real host mouse motion in the SDL
  frontend), and open-bus reads-as-zero for the SD card / MTU control
  registers, which aren't needed for single-app testing.

## Not emulated (by design, for now)

- **No OS.** The real `sw/os/kernel.c` (scheduler, FAT filesystem,
  process table) never runs. If you want to test the shell/kernel
  itself rather than a single app, that's a materially bigger project
  (SD card image, USB HID stack, etc.) and a natural "phase 2" rather
  than something bolted onto this tool.
- **No video timing.** The real `gpu_video.v` scanout/pixel-clock
  behavior isn't modeled -- apps don't wait on vsync (`bounce.c` and
  friends free-run), so the simulator just snapshots VRAM and blits it
  to the window every `instructions_per_frame` (default 400,000)
  instructions.
- **Blit/raster ops complete instantly** rather than modeling the real
  FSM's cycle-by-cycle timing. `busy` always reads back "done". If an
  app's correctness somehow depended on the real completion latency,
  this wouldn't catch that -- seems unlikely for the apps in this repo,
  but worth knowing.
- **`UI_PRINT` syscall** isn't implemented (would need pinning down
  `z_obj_t`'s string-object convention beyond what's needed for the
  UART calls the demo apps actually use).

## The OS layer (`simos.c`)

Everything an app asks the *kernel* for, answered against the host:
the filesystem, uptime, pids, messaging. Kept separate from
`machine.c` so that file stays about the machine and this one stays
about the operating system the machine does not have.

It is not a reimplementation of `sw/os`. It answers the **syscalls**,
at the ABI boundary, with the simplest host-backed thing that
satisfies the contract in `sw/os/fsapi.h` and `sw/common/zfs.h`. An
app cannot tell the difference; a kernel developer should not try to
use it as one.

### The filesystem

`--root DIR` points it at a host directory. Without one, every `FS_*`
syscall fails, deliberately: silently reading the developer's working
directory because an app asked for `wm` is not a behaviour anybody
wants to discover later.

```
$ ./zsim-headless --root ~/sdcard-contents ../sw/apps/hello_win/hello_win.bin
```

Three things it gets right that are easy to get wrong:

- **No escaping.** Any `..` component is refused outright rather than
  normalised away. Normalising is where that goes wrong quietly.
- **Case.** The card is FAT, and `sw/os/sh.c` passes paths
  *uppercased* -- so an app asking for `WM` finds `wm` on hardware. A
  host filesystem is case-sensitive and would not. An exact match is
  tried first, then a case-insensitive walk, component by component,
  so that `APPS/WM` finds `apps/wm`. Without this, half the tree's own
  paths would miss here and hit on hardware, which is the worst
  possible direction for a simulator to differ.
- **`/ram`.** An ordinary subdirectory of the root, and `1:/` reaches
  the same place, matching `fs_path_resolve()`. It is **not** volatile
  the way the real one is -- the real ramdisk is reformatted at every
  boot, so do not use the simulator to test something that depends on
  it starting empty.

Chunked I/O (`FS_OPEN_READ`/`_WRITE`/`_RW`, `READ_CHUNK`,
`WRITE_CHUNK`, `SEEK`, `TRUNCATE`, `SYNC`, `CLOSE`) is backed by host
`FILE *`s, with the same `Z_FS_MAX_OPEN` of 8 -- deliberately the same
number, so an app that runs out of handles runs out here at the same
point.

### There is one process

So there is no scheduler, no second process to send a message to, and
nothing in the pid registry. `MSG_READ` reports an empty mailbox and
`PID_LOOKUP` always fails, which is less a stub than the truthful
answer: there is no `wm` here. An app that handles that correctly
falls back to standalone behaviour (`term`'s local-echo path when no
port provider answers); one that does not fails here exactly as it
would on a machine where `wm` had not been started, which is a bug
worth finding.

`PROC_WAIT` sleeps 1ms rather than blocking, because blocking for a
message that can never arrive would hang every well-behaved app in the
tree.

Uptime comes from the host wall clock rather than the instruction
count. Instruction-derived ticks would be reproducible, which is
genuinely attractive for a test harness, but the simulator runs at a
completely different rate from the hardware -- so an app waiting
`Z_TICK_HZ / 30` between frames would either spin or crawl depending
on the host. The cost is that runs are not bit-reproducible.

### Syscall return values

Every app-side wrapper **dereferences** the pointer a syscall returns
(`rv->val.uint32 != Z_OK`). The simulator used to return 0, which
happened to appear to work -- `Z_OK` is 0 and unwritten low memory
reads as 0 -- so every syscall silently looked successful and none
could report failure at all. There are now two real `z_obj_t`s in low
memory at `ZS_RETOBJ_OK` / `ZS_RETOBJ_FAIL`.

## Building

```
$ make
```

Produces:
- `zeitlos-sim` -- the SDL2 GUI tool (`./zeitlos-sim app.bin [instructions_per_frame]`)
- `zsim-headless` -- no display, real options, real exit status. This
  is the frontend meant for automated testing:

  ```
  usage: zsim-headless [options] <app.bin>
    -r, --root DIR    host directory backing the guest filesystem
    -m, --ram BYTES   RAM size
    -n, --insns N     instruction budget, 0 = unlimited
    -f, --frames      write PBM frames (off by default)
        --every N     instructions between frames
    -o, --outdir DIR  where frames go
    -q, --quiet       no summary on stderr
  ```

  Exit status: 0 clean (`_exit()` or budget exhausted), 1 usage or load
  failure, 2 illegal instruction, 3 ECALL/EBREAK. It used to return 0
  unconditionally, which made a binary that executed one illegal
  instruction indistinguishable from one that ran to completion --
  fine for a human watching stderr, useless as a test harness.

  Frames are off by default now: writing ten PBMs of a program that
  only printed to the UART was the common case, and nobody wanted the
  files.
- `zsim-debug` -- single-instruction-step trace tool for debugging boot/early-crash issues (`./zsim-debug app.bin [n]`)

Requires SDL2 development headers (`libsdl2-dev` on Debian/Ubuntu) for
the main tool; `zsim-headless` and `zsim-debug` have no dependencies
beyond a C11 compiler.

## Testing without the real toolchain

`testapp/` contains a minimal freestanding `crt0.S` used only to build
test binaries from real, unmodified app sources (`bounce.c`,
`bounceblit.c`, `gpudemo.c`) in an environment without the project's
real `riscv32-unknown-elf-gcc` + newlib toolchain. **This is not part
of the emulator or required to use it** -- with the real toolchain,
apps build exactly as their own Makefiles already describe; the
simulator only needs the resulting `.bin` file. `testapp/synctest.c`
is a minimal standalone test of the `reg_kernel` syscall gate ABI,
useful as a reference if you're debugging that path.

## Library use

`machine.c`/`machine.h` + `cpu.c`/`cpu.h` have no frontend-specific
code in them and can be embedded directly:

```c
machine_t m;
machine_init(&m, 0 /* default RAM size */);
machine_load_bin(&m, "app.bin");
machine_run(&m, 1000000);       /* run up to 1,000,000 instructions */
int pixel = machine_get_pixel(&m, x, y);
machine_destroy(&m);
```

`main_sdl.c` and `main_headless.c` are both thin frontends over this
same API, so it's straightforward to add e.g. a "record framebuffer to
video" tool or a headless CI test harness alongside them.
