# sim: the whole system on a host, and in a browser (plan)

**Status: plan, round 0. Nothing here is built yet.** Decisions needed
are at the end.

`sim/` today runs **one app** against host code that stands in for the
kernel ([sim/README.md](../sim/README.md), "The OS layer"). The goal is
functional parity: the **real BIOS, kernel, drivers and apps**, the
same binaries a release flashes, booting to the same desktop. And the
same emulator built for WebAssembly, so a page can run Zeitlos.

Uses:

- the browser: Zeitlos on machdyne.com, live, before anyone buys a
  board;
- app development on the host: run an app with the real kernel, wm, fs
  and net beneath it, at the speed of a host;
- tests: boot a release image headless in CI, run an automate script
  against it ([automate.md](automate.md)), compare the screen;
- screenshots for the docs, made from the release itself.

## The approach: emulate the SoC, not the OS

Two ways to get there:

| | what is emulated | parity | cost |
|---|---|---|---|
| **A. the SoC** | the CPU, its interrupts, the bus and each device register, as the RTL defines them | by construction: the real binaries run, so every syscall, driver and app is the real one | a device model per block used, each small |
| B. the OS, wider | more syscalls in `simos.c`, more apps | never: each syscall is a second implementation to keep in step | grows with every change to the kernel |

**A** is proposed. `simos.c` stays, for running one app fast without a
kernel (its tests use it); the system path is new beside it.

What makes A tractable here:

- **One kernel for every board.** The kernel and apps probe the
  bitstream's features (`docs/csrs.md`); they are not built per board.
  So the emulator presents one board's CSRs and devices, and the
  release's own flash image runs on it unchanged.
- **The flash is memory-mapped** (`MEM_ROM`, `0x1000_0000`): the BIOS
  copies the kernel from it, and core apps load from the archive in it.
  A file is enough to model it.
- **A device absent from the CSRs is never touched** -- the software
  checks feature bits first (zrng.h, zrtc.h, csrs.md on why). So the
  emulated board can leave out the TRNG, the crypto blocks, Ethernet,
  T2 and the rest until they are wanted, and everything else still
  runs.

## What the system needs, block by block

Read from `rtl/`, `sw/bios/`, `sw/os/` and `sim/` at `1fd182f`.

| block | today in `sim/` | needed for parity | size |
|---|---|---|---|
| CPU: RV32IM, `rdcycle`/`instret` | yes (`cpu.c`) | -- | -- |
| PicoRV32 interrupts: `getq`/`setq`/`retirq`/`waitirq`/`maskirq`/`timer`, the q registers, the vector at the BIOS's `irq_vec`, IRQ 1 and 2 for illegal and misaligned (crash reporting, [mpu.md](mpu.md)) | `maskirq` only | all of them, as `rtl/cpu/picorv32` and `sw/bios/boot_picorv32.S` use them | medium |
| MTU: the app window at `0x8000_0000` remapped per process ([kernel.md](kernel.md)) | skipped (one app, mapped flat) | the translation base register and the remap on every access in the window | small |
| MPU ([mpu.md](mpu.md)) | -- | optional at first: leave `FEATURES` without it | later |
| memory: BIOS RAM at 0, main memory at `0x4000_0000`, VRAM at `0x2000_0000`, flash window at `0x1000_0000` | VRAM, an app's RAM | the four regions, main memory sized as the board's (32MB) | small |
| CSRs at `0x7000_0000` (magic, FEATURES 1-3, memory size, flash base word 63) | magic and FEATURES3 | the whole table for the chosen board | small |
| UART0 at `0xf000_0000` (16550) | stdin/stdout | the same, and its interrupt | small |
| timer (`timer` instruction, the kernel's tick) | -- | yes: the scheduler and every timeout depend on it | small |
| socctl (video mode, brightness, colour window, reboot) | partial | the registers the kernel and wm read and write | small |
| GPU: line rasterizer, blitter, cursor sprites | raster and fill; **blit copy is a no-op** | copy mode, the cursor sprites, game-mode planes | medium |
| SD card: bit-banged SPI on `reg_sdcard` (`0xb000_0000`, `SPI_SDCARD`) | reads as zero | an SD card in SPI mode behind the pins: CMD0/8/55/41/58/17/24/25, CRC optional, a card image file | medium |
| keyboard and mouse | the mouse cursor register | see "Input", below | medium |
| RTC | -- | counting seconds from the host clock | small |
| audio mixer | -- | its registers, mixed to SDL audio / WebAudio | medium, later |
| VMOUSE, `hid_inject` | -- | for automate and the remote desktop page | small, later |
| network | -- | later, if at all in a browser (see the end) | large |

### Input

How keys reach the kernel depends on the board's USB gateware:

- **`USB_HOST`** (Lakritz, Mozart, Sergei, ...): a general USB host
  controller ([usb_host.md](usb_host.md)); the `usb` driver enumerates
  devices through it and its auto-poll slots deliver HID reports to the
  `reg_usbN_*` registers. Emulating it means answering USB transactions
  as a keyboard and a mouse would: descriptors, SET_CONFIGURATION,
  interrupt IN reports.
- **HID only**: the older gateware enumerates the keyboard itself and
  the kernel reads reports from the same `reg_usbN_*` registers. The
  emulator writes the host's key and mouse events there directly.

The second is much less work and exercises the same kernel path from
the registers up. The first exercises the `usb` driver too, and is the
board most people will have.

## The board it emulates

One virtual board, `sim`, described the way a real one is: a feature
set the CSRs report, the memory size, which devices answer. Proposed
first: **a Lakritz without the USB host** -- 32MB, the GPU with raster,
blit and cursor, `SPI_SDCARD`, UART0, the RTC, HID-only input, no
colour planes at first. It runs the Lakritz release image's kernel and
core apps; only the gateware is replaced by the emulator.

## Milestones

Each one is a round: built, host-tested, and the docs updated.

1. **The kernel's prompt on the host's terminal.** PicoRV32 interrupts,
   the timer, the MTU, the full memory map and CSRs, UART0. The BIOS
   (`sw/bios`, built for the board) loads the kernel from a flash image
   (`release/lib/mkflashimg.py`'s output). Headless: `zsim
   --flash zeitlos-lakritz.img` reaches the kernel shell, `init` runs
   and fails cleanly at the card. Test: a script types `ps` and
   `help` at the UART and checks the answers.
2. **The desktop.** The SD card model with the release's card image,
   the blitter's copy mode and cursor sprites, socctl. Boot reaches
   `init: done`; wm draws the desktop. Test: a headless boot writes a
   screenshot and compares it with one checked in.
3. **Typing and pointing.** HID input from SDL; the SDL front end runs
   the whole system interactively at host speed. `term`, `text`,
   `files`, `welcome` usable. Test: an automate script drives it.
4. **The browser.** The same emulator through Emscripten (`main_sdl.c`
   is SDL2, which Emscripten provides). A page that fetches the flash
   and card images, keyboard and mouse from the page, the card image
   kept in the browser's storage between visits if the owner chooses.
   Size and speed measured; the goal is a desktop that is usable at
   the board's speed or better.
5. **Sound and colour.** The audio mixer to SDL audio and WebAudio;
   game-mode colour planes and the colour window; T2 (`sim/gtri.c`
   exists for its triangles).
6. **Later, by decision:** the USB host controller model (the real
   `usb` driver path), the MPU, the TRNG and crypto blocks, network.

## Speed

The emulator interprets RV32IM. A simple interpreter on a current PC
runs a few hundred million instructions a second natively and perhaps
a third of that as WebAssembly; the board runs 48 million cycles a
second with several cycles an instruction. So the host should be
faster than the board, and the browser about as fast -- to be measured
in milestone 1 with `tools/simprof.py`. The cycle model (`sim/cost.c`)
stays available for timing questions.

## Decisions needed

1. **The approach:** emulate the SoC (A, proposed), keeping `simos.c`
   for single-app runs?
2. **The board:** a Lakritz without the USB host, as proposed, or with
   it -- the USB host model added to milestone 3?
3. **The CPU model:** PicoRV32's interrupt scheme first (as
   `boot_picorv32.S`); Zeitlos32 uses the same IRQ numbers -- any
   difference that matters to you?
4. **The images:** run release images as they are (`zeitlos-<board>.img`
   with the gateware region ignored, and `zeitlos.img.gz`), so the
   browser shows exactly what ships?
5. **The browser's network:** none (the page says so), or a later
   round to tunnel the emulated Ethernet over a WebSocket to a relay?
