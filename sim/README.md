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

- **CPU**: a from-scratch RV32I interpreter (`cpu.c`). This matches the
  real picorv32 configuration in `rtl/sysctl.v` exactly
  (`ENABLE_MUL=0`, `ENABLE_DIV=0`, `COMPRESSED_ISA=0`) -- no
  multiply/divide/compressed instructions exist in the real hardware,
  so none are needed here either. Any float math in an app (e.g.
  `gpu3d.c`) compiles down to ordinary soft-float library calls, which
  are just more RV32I instructions -- nothing float-specific to
  emulate.

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
  there and intercepts it in the run loop, implementing `EXIT`,
  `UART_GETC/PUTC/RX_EMPTY/TX_FULL` directly in host code. `UI_PRINT`
  is stubbed (see "Known limitations" below).

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

## Building

```
$ make
```

Produces:
- `zeitlos-sim` -- the SDL2 GUI tool (`./zeitlos-sim app.bin [instructions_per_frame]`)
- `zsim-headless` -- no display; dumps the framebuffer as PBM files periodically. Useful for CI or environments without a display server (`./zsim-headless app.bin [total_insns] [dump_every] [outdir]`)
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
