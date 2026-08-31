# ![Zeitlos](https://github.com/machdyne/zeitlos/blob/bcca7d8a5dbba752f1f5e41afce82037e9b3b3ec/zeitlos.png)

Zeitlos is a work-in-progress SOC (System-on-a-Chip) and OS (Operating System) developed in tandem and intended to provide a responsive graphical environment for using and developing timeless applications on FPGA computers.

The core applications allow Zeitlos to be used as an extensible multi-window network terminal with scripting and graphics.

![Zeitlos Screenshot #0](https://github.com/machdyne/zeitlos/blob/a248944a4e42393ce93cabf0eae6cbc2d1255f9b/ss0.png)

Zeitlos is the successor to [Zucker](https://github.com/machdyne/zucker).

## Features

### SOC

| Component | Features/Notes |
|-----------|----------|
| CPU | 32-bit RISC-V (PicoRV32 or Zeitlos32) RV32IM @ 48MHz |
| GPU | Line rasterizer and blitter |
| MTU | Virtual addressing through Memory Translation Unit |
| Bus | 32-bit Wishbone |
| Main Memory | SDRAM, PSRAM or SRAM (1MB minimum) |
| Framebuffer | 640x480x1bpp (monochrome; white, green, or amber) |
| Viewport | Optional 320x240 pixel-doubled viewport |
| Video | VGA, DVI, DVI over HDMI, composite NTSC and PAL |
| Audio | 8 channel hardware mixer with stereo output |
| Storage | MicroSD |
| Network | Ethernet (SPI) and Ethernet MAC (for RMII PHY) |
| Entropy | Ring-oscillator TRNG |
| HID | USB keyboard + optional USB mouse/gamepad |
| I/O | GPIO, SPI, 16550 UART |

### OS

 - Pre-emptive multitasking
 - Flat memory model with virtual address space for apps
 - FAT16/32 filesystem
 - Core apps in flash -- boots to a desktop with no sdcard ([docs/flash_apps.md](docs/flash_apps.md))
 - Object-based interprocess messaging and streaming
 - IP/ARP/ICMP/UDP/DHCP/NTP/DNS/TFTP/TCP/telnet/ssh networking

#### Memory Translation Unit

Zeitlos doesn't have an MMU but instead has a single virtual address space that is remapped to a main memory address during context switches.

The Zeitlos kernel is located at `0x4000_0000` which is the beginning of main memory, and apps are loaded immediately after the kernel. However, each app executes at fixed address `0x8000_0000` which is a mirror of their actual address in the main memory. The translation base address register is set during context switches so that each app can access its own memory through `0x8000_0000`.

With the MTU, there is no need for position independent code or complicated address relocation.

### Apps

#### Core Apps

| App | Description |
|-----|-------------|
| kernel | Kernel + kernel shell (serial console) |
| wm | Window manager + dock |
| net | Networking server |
| repl | App server + [Lisp interpreter (subset of R4RS Scheme)](https://github.com/machdyne/ms) |
| term | Terminal emulator (connects to services; VT100 emulation) |

#### Additional Apps

| App | Description |
|-----|-------------|
| text | Text editor |
| read | Text reader for files of unlimited size (with rendered Markdown) |
| draw | MacPaint-inspired drawing app |
| files | File browser |
| calc | Calculator |
| info | System info |
| clock | Analog and digital clock |
| settings | System settings |
| track | MOD audio file player |
| space3d | First-person 3D space shooter game |
| gpu3d | Spinning 3D cube demo + STL viewer |

### Boards

Zeitlos will initially support ECP5, Artix-7, GateMate FPGAs.

The following boards are currently supported:

 - [Machdyne Obst](https://github.com/machdyne/obst)
 - [Machdyne Lakritz](https://github.com/machdyne/lakritz)
 - [Machdyne Kölsch](https://github.com/machdyne/kolsch)
 - [Machdyne Lebkuchen](https://github.com/machdyne/lebkuchen)
 - [Machdyne Mozart](https://github.com/machdyne/mozart) / [ML1](https://github.com/machdyne/sechzig)
 - [Machdyne Sergei](https://github.com/machdyne/sergei) / [ML1](https://github.com/machdyne/sechzig)
 - [ULX3S](https://radiona.org/ulx3s/) **UNTESTED** (see [bounty](https://machdyne.com/bounties/))
 - (more soon)

If you have an unsupported board and want to try Zeitlos, please open an issue.

## Usage

**An sdcard is optional.** The core apps (`wm`, `net`, `repl`, `term`)
are programmed into flash alongside the kernel, so a freshly flashed
board boots straight to the graphical desktop with nothing else
attached. See [Core apps in flash](#core-apps-in-flash) below.

### Quick start: prebuilt images

Each [release](https://github.com/machdyne/zeitlos/releases/latest)
ships one image per supported board, containing the gateware, boot
splash, kernel and core apps. Flash it and the board boots to a desktop
— nothing to build.

Pick the image matching your hardware, for example a Lakritz with a
USB-UART PMOD:

```
$ curl -LO https://github.com/machdyne/zeitlos/releases/latest/download/zeitlos-lakritz_uart.img
$ openFPGALoader -c dirtyJtag -f -o 0 zeitlos-lakritz_uart.img
```

Adjust `-c` to match your programming cable. The release page and the
`README.txt` shipped with it list every available image and its exact
flashing command.

Optionally add an sdcard for the additional apps, the documentation and
storage:

```
$ curl -LO https://github.com/machdyne/zeitlos/releases/latest/download/zeitlos.img.gz
$ gzip -dc zeitlos.img.gz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
```

Replace `/dev/sdX` with your sdcard's device node (check with `lsblk`
first — writing to the wrong device will destroy its contents).

If there's no image for your board, build from source below and please
open an issue.

### Building from source

1. Build and flash the system:

Building Zeitlos requires FPGA tools (Yosys, nextpnr, and a bitstream
packer for your FPGA family) and a RISC-V toolchain. Most of these are
available as Debian/Ubuntu packages:

```
$ sudo apt install yosys nextpnr-ecp5 fpga-trellis fpga-trellis-database \
                   openfpgaloader
```

The RISC-V compiler is the one piece not to take from apt: Zeitlos is
built against newlib, and Ubuntu's `gcc-riscv64-unknown-elf` ships no C
library at all. Use the [xPack prebuilt
toolchain](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases)
(GCC + binutils + newlib, no building required) and set `RISCV_PREFIX`
in `sw/common/arch.mk` to point at it.

See [docs/toolchain.md](docs/toolchain.md) for current upstream
versions, the OSS CAD Suite bundle, GateMate boards, and the trade-offs
between the RISC-V toolchain options.

Note that Zeitlos now builds `rv32im` (hardware multiply and divide) --
see [docs/muldiv.md](docs/muldiv.md). Gateware and software must be
flashed together.

```
$ git clone https://github.com/machdyne/zeitlos
$ cd zeitlos
$ git submodule update --init --recursive
$ make BOARD=lakritz CABLE=dirtyJtag flash
```

The above command builds the SOC, BIOS, OS and apps, then writes the
gateware, kernel, boot splash and core apps to flash.

The BIOS will automatically boot the kernel if no keys are pressed, and
the kernel starts `wm`, `net` and `repl` automatically -- you'll land
straight in the graphical desktop. See [`docs/welcome.md`](docs/welcome.md)
for how to use it from there.

**The mouse pointer tells you when it's ready.** It is a **Z** while
the system is still starting up and an **X** once it isn't. The dock
won't launch anything while the Z is showing -- `term` connects to
`repl` the moment it starts, and launching it too early gives you a
blank window rather than a terminal. Wait for the X. See
[`docs/socctl.md`](docs/socctl.md).

2. Optionally, add an sdcard:

An sdcard is only needed for storing files and for apps beyond the core
four. See [Quick start](#quick-start-prebuilt-images) above for how to
write the image.

### Core apps in flash

`wm`, `net`, `repl` and `term` are written to flash as part of a normal
`make flash`, immediately after the kernel. They are an *underlay*
beneath the filesystem, not a separate namespace: there is still exactly
one name for `term`, and `run term` behaves identically whether it came
from flash or from a card.

The rule is one line:

> if the filesystem has it, use that; otherwise use the flash copy.

A file on the card wins, because the only way it got there was somebody
deliberately putting it there — which is what makes `xf wm` still work
as a single-app hot-swap during development, with no version scheme or
timestamps involved. `ls` lists the flash copies in a separate section,
skipping any that a real file is shadowing, so what you see is what
`run` would actually launch.

For iterating on the OS itself, `make dev-flash` rebuilds and reflashes
the kernel and core apps without touching the gateware:

```
$ make clean && make BOARD=obst dev-flash
```

See [`docs/flash_apps.md`](docs/flash_apps.md) for the archive format
and the design reasoning.

## Developers

### Documentation

The Zeitlos documentation will be the [Timeless Computing](https://github.com/machdyne/tc) book, which will be included in the default Zeitlos distribution. The later chapters will explain the system, list the API, etc. 

The Zeitlos implementation portions of the book are currently located in the `docs` directory.

### Releases

Prebuilt images are built and published by `release/zrelease`, which
builds one image per board/PMOD combination, assembles it, checks it and
uploads it. See [docs/releases.md](docs/releases.md).

### LLM-generated code

This project makes use of LLMs for code and documentation.

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md) with the following exceptions:

- rtl/cpu/picorv32 uses the ISC license.
- rtl/ext/uart16550 uses the LGPL license.
- rtl/mem/sdram\_kianv uses the Apache 2.0 license.
- rtl/ext/usb\_hid\_host uses the Apache 2.0 license.
- sw/os/fs/fatfs uses a BSD compatible license.
