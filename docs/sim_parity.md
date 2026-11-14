# sim: the whole system on a host, and in a browser (plan)

**Status: milestones 1 to 4 done** -- the real BIOS, kernel and apps
run in `sim/sys` ([zsys.md](zsys.md)) to a usable desktop, with a
release's card image, a keyboard and a mouse: in a window on a PC, and
in a page (`zrelease web`), with sound. Releases carry
`zeitlos-sim.img`. Milestone 5 is done, T2 included; milestone 6 is
next.

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
| socctl (video mode, brightness, colour window, reboot) | partial | the registers the kernel and wm read and write | small -- **done** (milestone 2), less colour and game mode |
| GPU: line rasterizer, blitter, cursor sprites | raster and fill; **blit copy is a no-op** | copy mode, the cursor sprites, game-mode planes | medium -- **done** (milestones 2 and 5): game mode, eight planes, chunky, the colour window, desktop colour |
| SD card: the SPI controller (`rtl/spim.v`, `0xb000_0000`, `SPI_SDCARD`) | reads as zero | an SD card in SPI mode behind it: CMD0/8/55/41/58/17/24/25, CRC optional, a card image file | medium -- **done** (milestone 2) |
| keyboard and mouse | the mouse cursor register | see "Input", below | medium -- **done** (milestone 3): the HID-only ports |
| RTC | -- | counting seconds from the host clock | small -- **done** (milestone 2): from 0 as on the board, or the host's time (`--clock`) |
| audio mixer | -- | its registers, mixed to SDL audio / WebAudio | medium -- **done** (milestone 5): the FIFO, the mixer, FSDIV, the volume, IRQ 7; SDL audio, an AudioWorklet, `--wav` |
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

## Decided (2026-11-10)

| | |
|---|---|
| the approach | **emulate the SoC**; `simos.c` stays, for single-app runs |
| the board | **a Lakritz without the USB host**: HID reports written straight into the `reg_usbN_*` registers. The USB host controller later |
| the CPU | **Zeitlos32**, with zeitlos32p's IRQ timer (the kernel needs it) |
| the images | **the release images as they ship**, and a **sim image** in every release -- the BIOS where the gateware would be -- for the web page, so visitors are not sent the gateware |
| the network | **not yet** |

## The board it emulates

One virtual board, described the way a real one is: a feature set the
CSRs report, the memory size, which devices answer. **A Lakritz
without the USB host** -- 32MB, the GPU with raster, blit and cursor,
`SPI_SDCARD`, UART0, the RTC, HID-only input, no colour planes at
first -- built up a milestone at a time; milestone 1 has the headless
part of it ([zsys.md](zsys.md), "The board"). It runs the release's
kernel and core apps; only the gateware is replaced by the emulator.

## Screen modes

socctl's VMODE is emulated ([screen_modes.md](screen_modes.md)):
`ZSYS_VMODES=wide` (the default, a 45F board), `dbl` (a 25F: 1280x720 is
640x360) or `none`. The picture, the screenshot and the window take the
mode's desktop size; the hardware's doubling is not drawn.

## Milestones

Each one is a round: built, host-tested, and the docs updated.

1. **The kernel's prompt on the host's terminal. Done.** Zeitlos32's
   interrupts and timer, the MTU, the memory map and CSRs, UART0, the
   card's SPI controller with no card. The BIOS loads the kernel from
   the flash image; `fs`, `net` and `console` start from the core apps'
   archive; `init: done` at 1.03 seconds of the machine's time.
   Tests: the core's own nine test programs, a boot that types `ps`
   and reads the answer, a release-style image with `--bios`
   ([zsys.md](zsys.md), "Testing").
2. **The desktop. Done.** The blitter in every mode, the line
   rasterizer, glyph memory, the cursor's slots, socctl, the RTC, and an
   SD card in SPI mode with the release's card image, written through
   (or a snapshot). `fs` mounts the card, wm draws the desktop and the
   dock, `welcome` greets a new card, apps started at the console open
   windows; `reboot` reconfigures. Tests: the devices register by
   register (`test_zdev`, 55 checks), and a boot that writes a
   screenshot on which every window wm placed must have its frame --
   in place of comparing with a checked-in picture, which would fail
   at every change to wm's look ([zsys.md](zsys.md), "Testing").
3. **Typing and pointing. Done.** The two USB HID ports, a keyboard
   and a mouse, reporting and interrupting as `rtl/usb_hid.v` does --
   both through the registers, not `VMOUSE`, so the kernel's and wm's
   real input paths run; the flash's write path (`FLASHW`), so the
   key/value store is writable and `welcome` greets a machine with no
   card too. `--window`: SDL2, the screen redrawn a dirty row at a
   time. Headless, `--keys` and `--click` script the devices. Tests:
   the registers (`test_zdev`, 74 checks in all), the store kept
   across boots, Tab and Enter in welcome, Skip, a click on the dock.
   (An automate script instead was the plan; the scripted devices
   test the same path without depending on a card app.)
4. **The browser. Done.** The same machine through Emscripten -- but
   **not through SDL**, as planned: the page owns the canvas, input and
   fetches anyway, so `web/zsys_web.c` is a dozen calls and
   `web/zsys-page.js` the rest; nothing to download at build time, 39KB
   of WebAssembly and 68KB of glue for a visitor. Gzipped images
   (`zrelease web`: 7.8MB in all with the base card); a visitor's
   changes kept in IndexedDB if they tick the box. **85 million
   instructions a second, 72% of native, eight times a board.** Test:
   the page in a headless Chromium (Playwright), a visitor's path from
   the boot to a reload that is the same machine.
5. **Sound and colour.** **Sound done**: the audio block and its mixer
   with the RTL's arithmetic, to SDL's audio, an AudioWorklet in the
   page, and `--wav` for tests (`audiotest`'s tone measured at 440 Hz;
   tts's speech reaching the page). The audio set is back on the
   emulator's card. **Colour done**: game mode, eight planes (a 45F
   board's video: more than Lakritz's six, so every program with colour
   shows it), chunky, the colour window and desktop colour's exact path
   -- mausrun, planedemo and colwin run, and mausrun3d and mauskart3d
   in software chunky (15 frames a second), so all five are back on
   the emulator's card. **T2 done** (`zt2.c`): its registers and, at
   each frame's END, T2's own model -- the geometry unit's program and
   the raster's model the RTL bench is held to -- into chunky VRAM, as
   sergei_ml2's build has it beside the desktop: mausrun3d and
   mauskart3d at 60 frames a second, textured (`--no-t2`: software, 15).
   The console profile's picture in main memory (`VRAM_SDRAM`) is not
   emulated. A full-screen button on the page, Ctrl+Alt+Enter in the
   window.
6. **Later, by decision:** the USB host controller model (the real
   `usb` driver path), the MPU, the TRNG and crypto blocks, network.

## Speed

Measured at milestone 1, on the PC it was written on:

| | instructions a second |
|---|---|
| a real board: Zeitlos32 at 48MHz, about 5 clocks an instruction ([zeitlos32.md](zeitlos32.md)) | about 10 million |
| `zsys`, a plain interpreter, `-O2` | about 140 million |
| `zsys` at the desktop, milestone 2: the GPU's work and the card counted in -- 8 seconds of the machine's time, five apps open, in 0.45 | about 100 million |
| the WebAssembly build, milestone 4 (timed in Node): the boot to the desktop with a card | **85 million**, 72% of native (the estimate was 60-90) |

So the CPU alone is several times a real board, even in a browser.
`--cpi` sets how many of the board's clocks each instruction costs: at
the default 2, the emulated board runs about 24 million instructions
in each of its seconds, and keeps to wall-clock time; 1 makes it 48.

### Graphics

**The graphics hardware is not emulated gate by gate, and does not
run on the emulated CPU.** Each GPU block is its registers in front of
host code doing the same work: when the kernel starts a blit, the
emulator performs the whole blit, in C, before the next instruction --
a full-screen fill is a `memset` of 38KB, microseconds. So:

| | on the board | in the emulator |
|---|---|---|
| the blitter: fills, patterns, copies, glyphs | bus-bound, a few hundred thousand pixels per frame | host memory speed: effectively free |
| the line rasterizer | a pixel a clock | the same algorithm in C (`sim/gtri.c` and `machine.c` already have these): free |
| T2, the triangle engine | its own pipeline | `sim/gtri.c`'s rasteriser, native: tens of millions of pixels a second, more than 320x240 at 60 frames needs |
| software drawing (zcanvas, an app's own pixels) | the CPU | the emulated CPU: 6-14 times faster than the board |

What costs time in a browser is **getting the picture onto the
page**, not drawing it. VRAM is 1 bit a pixel (and up to 8 planes in
game mode), the page wants RGBA. Three things keep that cheap:

1. **Only what changed.** The emulator marks VRAM rows dirty as they
   are written -- by the CPU or by a GPU block -- and the page converts
   only those rows. A typed character dirties 8 to 16 rows, not 480.
2. **The GPU does the conversion.** The page uploads VRAM as it is --
   38KB for the 640x480 desktop -- as a WebGL texture, and a small
   shader expands bits and planes to colours through the palette, the
   phosphor colour and the colour window. The CPU then does almost
   nothing per frame.
3. **Frames at the display's rate.** The page draws once per
   `requestAnimationFrame`, whatever the emulated machine wrote in
   between, as a monitor scans out VRAM.

If the CPU ever is the bottleneck -- software 3D, a large zcanvas page
-- the next steps, in order of cost: an instruction cache of decoded
instructions (often twice as fast), then translating hot code to
WebAssembly at run time (several times more). Neither is needed for
milestone 2 or 3; both can be measured with `--stats`.

## Decisions needed

- **Where the page lives**, and which card it serves: the base card
  (7MB gzipped) is what `zrelease web --card` was tested with.
- **Whether a visitor's machine is kept by default.** It is off: the
  box is unticked until they tick it, and the choice is remembered.
- After colour and T2: milestone 6 -- the USB host, the MPU, the
  network (which would bring the net set's apps to the page).
