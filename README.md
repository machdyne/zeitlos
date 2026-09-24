# ![Zeitlos](https://github.com/machdyne/zeitlos/blob/bcca7d8a5dbba752f1f5e41afce82037e9b3b3ec/zeitlos.png)

Zeitlos is a work-in-progress SOC (System-on-a-Chip) and OS (Operating System) developed in tandem and intended to provide a responsive graphical environment for using and developing timeless applications on FPGA computers.

The core applications allow Zeitlos to be used as an extensible multi-window network terminal.

![Zeitlos Screenshot #0](https://github.com/machdyne/zeitlos/blob/a248944a4e42393ce93cabf0eae6cbc2d1255f9b/ss0.png)

Zeitlos is the successor to [Zucker](https://github.com/machdyne/zucker).

## Features

### SOC

| Component | Features/Notes |
|-----------|----------|
| CPU | 32-bit RISC-V (PicoRV32 or [Zeitlos32](docs/zeitlos32.md)) RV32IM @ 48MHz |
| GPU | [Line rasterizer](docs/gpu_raster.md) and [blitter](docs/gpu_blitter.md) |
| MTU | Virtual addressing through Memory Translation Unit |
| MPU | [Memory protection unit](docs/mpu.md) and crash reporting |
| Cache | Optional [instruction cache](docs/icache.md), or unified instruction + [data cache](docs/dcache.md) with write buffer and SDRAM burst fills |
| Bus | 32-bit Wishbone |
| Main Memory | SDRAM, PSRAM or SRAM (1MB minimum) |
| Framebuffer | 640x480x1bpp (monochrome; white, amber, green or paper -- [socctl](docs/socctl.md)) |
| Viewport | Optional 320x240 pixel-doubled [viewport](docs/game_mode.md) |
| Video | VGA, DVI, DVI over HDMI, [composite](docs/composite.md) NTSC and PAL |
| Audio | 8 channel 16-bit [hardware mixer](docs/audio.md) with stereo output |
| Storage | MicroSD |
| Network | Ethernet (SPI/USB), Ethernet MAC (for RMII PHY) or [ESP32](docs/esp32link.md) |
| Entropy | Ring-oscillator [TRNG](docs/trng.md) |
| Crypto | Optional [Montgomery multiplier](docs/montmul.md) for TLS |
| USB Host | [Dual-port USB host controller](docs/usb_host.md) (HID, MSC, CDC-ACM, [CDC-ECM](docs/usb_ethernet.md), hubs) |
| USB Device | [USB CDC](docs/usb_cdc.md) serial console |
| HID | USB keyboard ([22 layouts](docs/keyboard_layouts.md)) + optional USB mouse/[gamepad](docs/gamepad.md) |
| I/O | Optional [GPIO](docs/gpio.md) on PMOD ports with bit-banged [I2C](docs/i2c.md) and [SPI](docs/spi.md), hardware SPI, 16550 UART, optional second [UART](docs/uart1.md) |

![Zeitlos Hardware Map](https://github.com/machdyne/zeitlos/blob/main/hwmap.png)

Build the hardware map from RTL with `make hwmap` (see [docs/hwmap.md](docs/hwmap.md)).

### OS

 - Pre-emptive multitasking
 - Flat memory model with virtual address space for apps
 - Text [configuration file](docs/config.md) (`/zeitlos.cfg`), loaded at boot and reloadable
 - FAT filesystem, on MicroSD and on an optional [RAM disk](docs/ramdisk.md)
 - [Core apps in flash](docs/flash_apps.md) -- boots to a desktop with no sdcard
 - Object-based interprocess [messaging](docs/messaging.md), streaming and [ports](docs/ports.md)
 - International text: UTF-8 throughout, [22 keyboard layouts](docs/keyboard_layouts.md) (US, UK, German, French, Spanish, Italian, Portuguese, Nordic, Swiss, Belgian, Japanese and more) with dead keys and AltGr, switched with Super+Space; accented letters and the euro sign drawn by the hardware fonts ([ISO 8859-15](docs/text_encoding.md)); Japanese drawn from a public-domain 12x12 font and typed with a romaji [input method](docs/keyboard_layouts.md#japanese-input)
 - [Speech](docs/tts.md) for blind and headless use
 - Image decoding and [vector rendering](docs/svg.md) shared by every app (`sw/common`)
 - IP/ARP/ICMP/UDP/DHCP/NTP/DNS/TFTP/TCP/telnet/ssh [networking](docs/networking.md)
 - TLS 1.3 with X.509 certificate verification -- see [tls](docs/tls.md) and [x509](docs/x509.md)
 - [Remote desktop](docs/remote_desktop.md) in a browser, with keyboard and mouse back, on boards with the [ESP32 link](docs/esp32link.md)

#### Memory Translation Unit

Zeitlos doesn't have an MMU but instead has a single virtual address space that is remapped to a main memory address during context switches.

The Zeitlos kernel is located at `0x4000_0000` which is the beginning of main memory, and apps are loaded immediately after the kernel. However, each app executes at fixed address `0x8000_0000` which is a mirror of their actual address in the main memory. The translation base address register is set during context switches so that each app can access its own memory through `0x8000_0000`.

With the MTU, there is no need for position independent code or complicated address relocation.

### Apps

#### Core Apps

| App | Description |
|-----|-------------|
| kernel | Kernel + kernel shell (serial console) |
| [wm](docs/window_manager.md) | Window manager + dock |
| [net](docs/networking.md) | Networking service |
| [term](docs/terminal.md) | Terminal emulator (VT100, UTF-8; start panel, scrollback; connects to shells and services) |

#### Shells

On the sdcard, started at boot when a card is present. A `term` window can connect to either `repl` or `posix` (or a remote system).

| App | Description |
|-----|-------------|
| [repl](docs/scheme_api.md) | App service + [Lisp interpreter (subset of R4RS Scheme)](https://github.com/machdyne/ms) |
| [posix](docs/posix.md) | POSIX compatibility layer |

#### Windowed Apps

| App | Description |
|-----|-------------|
| [text](docs/text_editor.md) | Text editor (UTF-8 and Latin-9 files, Japanese) |
| [sheet](docs/sheet_app.md) | Spreadsheet |
| [web](docs/web_app.md) | Web browser: HTTP/1.1 and TLS 1.3, gzip, in-place images and SVG |
| [read](docs/read_app.md) | Text reader for files of unlimited size (with rendered Markdown) |
| [hex](docs/hex_editor.md) | Hex editor for files of unlimited size |
| draw | MacPaint-inspired drawing app |
| [view](docs/view_app.md) | Image viewer: BMP, PNM, GIF, JPEG, [PNG](docs/png.md) and [SVG](docs/svg.md) |
| files | File browser |
| [calc](docs/calc_app.md) | Calculator |
| [info](docs/info_app.md) | System info |
| [clock](docs/clock_app.md) | Analog and digital clock |
| [cal](docs/cal_app.md) | Month calendar |
| [settings](docs/settings_app.md) | System settings; editor for [`/zeitlos.cfg`](docs/config.md) |
| [keyboard](docs/keyboard_app.md) | On-screen keyboard for any layout: for touchscreens and pointer-only use, and for trying layouts |
| [ask](docs/ask_app.md) | Local dataset search |
| [play](docs/play_app.md) | WAV/AU/RAW audio file player |
| [track](docs/track_app.md) | MOD audio file player |
| [mmod](docs/mmod.md) | [MMOD](https://github.com/machdyne/mmod) reader/writer |
| [logic](docs/logic_app.md) | Logic analyzer (under development) |
| [gpu3d](docs/gpu3d_app.md) | Spinning 3D cube demo + STL viewer |

#### Games

| App | Description |
|-----|-------------|
| [chess](docs/chess_app.md) | Chess, with a [built-in engine](docs/chess_engine.md) |
| [chip8](docs/chip8_app.md) | CHIP-8 game emulator |
| [gamedemo](docs/gamedemo.md) | 2D side-scrolling platformer game |
| space3d | First-person 3D space shooter game |
| [kidgames](docs/kidgames_app.md) | Educational games for kids |
| [casino](docs/casino.md) | The front desk: bankroll, loans, and it launches the five below |
| [blackjack](docs/blackjack.md) | 6 decks, S17, 3:2, with a basic-strategy hint |
| [craps](docs/craps.md) | The whole felt, including the free odds -- the only bet in a casino with no house edge |
| [poker](docs/poker_app.md) | Hold'em, draw and stud against 1-7 opponents, with a [built-in engine](docs/poker_engine.md) |
| [roulette](docs/roulette.md) | American and European wheels, with an animated spin |
| [slots](docs/slots.md) | Three reels, five paylines, an exactly-computed 94.641% return |

#### Other Apps

| App | Description |
|-----|-------------|
| [serial](docs/uart1.md) | Serial port service |
| [console](docs/console.md) | Console service |
| [tts](docs/tts.md) | Text-to-speech service (Super+S to turn speech on) |
| [jfont](docs/text_encoding.md#japanese) | Japanese font service: holds the font once for every app (`system.font.japanese: yes`) |
| [zcc](docs/zcc.md) | C compiler |
| [zfpga](docs/zfpga.md) | FPGA toolchain (synthesis, place-and-route, bitstream packing) |
| vi | Port of the [nextvi](https://github.com/kyx0r/nextvi) terminal text editor |

### Boards

Zeitlos will initially support ECP5, Artix-7, GateMate FPGAs.

The following boards are fully supported:

 - [Machdyne Obst](https://github.com/machdyne/obst) (see [DFU upgrade docs](docs/dfu_upgrade.md))
 - [Machdyne Lakritz](https://github.com/machdyne/lakritz) (see [DFU upgrade docs](docs/dfu_upgrade.md))
 - [Machdyne Mozart](https://github.com/machdyne/mozart) / [ML1](https://github.com/machdyne/sechzig)
 - [Machdyne Sergei](https://github.com/machdyne/sergei) / [ML1](https://github.com/machdyne/sechzig)
 - [Radiona ULX3S](https://radiona.org/ulx3s/) (85F tested, see [docs/ulx3s.md](docs/ulx3s.md))
 - (more soon)

Packed utilisation and routed clocks for every board that builds:
[docs/boards.md](docs/boards.md).

The following boards are currently partially supported or untested:

 - [Machdyne Kölsch](https://github.com/machdyne/kolsch)
 - [Machdyne Lebkuchen](https://github.com/machdyne/lebkuchen)

**If you have an unsupported board and want to try Zeitlos, please open an issue.**

#### Minimum Hardware Requirements

 - ECP5 LFE5U-12F (25K LUTs with open-source tools) or above
 - 1MB of main memory
 - 2MB of NOR flash

## Usage

**An sdcard is optional.** The core apps (`wm`, `net`, `term`, `console`) are
programmed into flash alongside the kernel, so a freshly flashed board
boots straight to the graphical desktop with nothing else attached. The
shells, `repl` and `posix`, come from the card -- without one, `term`
still reaches telnet, ssh and serial through its Open bar (F11). See
[Core apps in flash](#core-apps-in-flash) below.

### Quick start: prebuilt images

Each [release](https://github.com/machdyne/zeitlos/releases/latest)
ships one image per supported board, containing the gateware, boot
splash, kernel and core apps. Flash it and the board boots to a desktop
— nothing to build.

Pick the image matching your hardware, for example a Lakritz with a
GPIO PMOD:

```
$ curl -LO https://github.com/machdyne/zeitlos/releases/latest/download/zeitlos-lakritz_gpio.img
$ openFPGALoader -c dirtyJtag -f -o 0 zeitlos-lakritz_gpio.img
or
$ sudo dfu-util -a 0 -D zeitlos-lakritz_gpio-dfu.bin
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

### Connecting to the console

With a USB-UART PMOD:

```
$ minicom -o -D /dev/ttyUSB0 -b 1000000
```

On Obst and Lakritz the console can instead be a USB CDC-ACM device on
the board's own USB-C socket, freeing the PMOD connector entirely and
removing the need for a USB-UART adapter at all — see
[docs/usb\_cdc.md](docs/usb_cdc.md). There the baud rate is ignored:

```
$ minicom -o -D /dev/ttyACM0
```

**Use `-o`.** Without it minicom sends a modem init string when it
opens the port, which the BIOS reads as a keypress and which cancels
autoboot. On a USB-CDC console that happens on every single boot,
because the console blocks until a terminal opens the port and opening
the port is exactly when the greeting is sent.

Linux users should also install the udev rule, which stops
ModemManager doing the same thing with AT probes and gives the console
a stable name:

```
$ sudo cp tools/70-zeitlos.rules /etc/udev/rules.d/
$ sudo udevadm control --reload-rules
$ minicom -o -D /dev/zeitlos
```

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
gateware, kernel, boot splash and core apps to flash -- and, on some boards, the
[jumploader](docs/zboot.md).

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
four. To build one from the tree you just compiled:

```
$ release/zrelease sdcard
$ gzip -dc images/zeitlos.img.gz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
```

`tools/mkfatimg.sh` does the same thing and is kept for habit. To write
a prebuilt image instead, see
[Quick start](#quick-start-prebuilt-images) above.

### Core apps in flash

`wm`, `net`, and `term` are written to flash as part of a normal
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

### Hardware map

`make hwmap` draws the SoC from `rtl/sysctl.v` into `output/docs/hwmap.pdf` (plus PNG and SVG), showing every optional feature with the define that includes it, and checks the RTL for define combinations that would not build or would hang the bus. See [docs/hwmap.md](docs/hwmap.md).

### Releases

Prebuilt images are built and published by `release/zrelease`, which
builds one image per board/PMOD combination, assembles it, checks it and
uploads it. See [docs/releases.md](docs/releases.md).

### LLM-generated code

This project makes use of LLMs for code and documentation.

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md) with the following exceptions:

- rtl/cpu/picorv32 uses the ISC license.
- rtl/mem/sdram\_kianv uses the Apache 2.0 license.
- rtl/ext/usb\_hid\_host uses the Apache 2.0 license.
- rtl/ext/usb\_cdc uses the MIT license.
- sw/os/fs/fatfs uses a BSD compatible license.
- sw/common/zkbd\_layouts.c (keyboard layout data generated from [xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config)) uses X11/MIT-style licenses; see sw/common/zkbd\_layouts.LICENSE.
- sw/data/font: the BDF fonts (Markus Kuhn's misc-fixed ucs-fonts) and jp12.zfn (converted from the Shinonome fonts) are in the public domain.
- sw/data/ark uses Creative Commons Attribution-ShareAlike 4.0 International License (CC BY-SA) and the GNU Free Documentation License (GFDL).
- sw/ext/nextvi uses an ISC license.
- sw/apps/zfpga/ext/prjtrellis-db (the Project Trellis ECP5 database) uses the CC0 1.0 license.
- sw/apps/zfpga/ext/nextpnr-base (baseline data extracted from nextpnr) uses the ISC license.
