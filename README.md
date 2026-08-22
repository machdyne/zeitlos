# ![Zeitlos](https://github.com/machdyne/zeitlos/blob/bcca7d8a5dbba752f1f5e41afce82037e9b3b3ec/zeitlos.png)

Zeitlos is a work-in-progress SOC (System-on-a-Chip) and OS (Operating System) developed in tandem and intended to provide a responsive graphical environment for using and developing timeless applications on FPGA computers.

The core applications allow Zeitlos to be used as an extensible multi-window network terminal with scripting and graphics.

Zeitlos is the successor to [Zucker](https://github.com/machdyne/zucker).

## Features

### SOC

| Component | Features/Notes |
|-----------|----------|
| CPU | 32-bit RISC-V (PicoRV32) @ 48MHz |
| GPU | Line rasterizer and blitter |
| MTU | Virtual addressing through Memory Translation Unit |
| Bus | 32-bit Wishbone |
| Main Memory | SDRAM, PSRAM or SRAM (1MB minimum) |
| Framebuffer | 640x480x1bpp |
| Video | VGA, DVI, DVI over HDMI |
| Storage | MicroSD |
| Network | Ethernet (SPI) and Ethernet MAC (for RMII PHY) |
| HID | USB keyboard + optional USB mouse |
| I/O | GPIO, SPI, 16550 UART |

### OS

 - Pre-emptive multitasking
 - Flat memory model with virtual address space for apps
 - FAT16/32 filesystem
 - Object-based interprocess messaging and streaming
 - IP/ARP/ICMP/UDP/DHCP/DNS/TFTP/TCP/TELNET networking

#### Memory Translation Unit

Zeitlos doesn't have an MMU but instead has a single virtual address space that is remapped to a main memory address during context switches.

The Zeitlos kernel is located at `0x4000_0000` which is the beginning of main memory, and apps are loaded immediately after the kernel. However, each app executes at fixed address `0x8000_0000` which is a mirror of their actual address in the main memory. The translation base address register is set during context switches so that each app can access its own memory through `0x8000_0000`.

With the MTU, there is no need for position independent code or complicated address relocation.

### Apps

| App | Description |
|-----|-------------|
| kernel | Kernel + kernel shell (serial console) |
| wm | Window manager + dock |
| net | Networking server |
| term | Terminal emulator (connects to services; VT100 emulation) |
| repl | App server + Lisp interpreter (subset of R4RS Scheme) |

### Boards

Zeitlos will initially support ECP5, Artix-7, GateMate FPGAs.

The following boards are currently supported:

 - [Machdyne Obst](https://github.com/machdyne/obst)
 - [Machdyne Lakritz](https://github.com/machdyne/lakritz)
 - [Machdyne Kölsch](https://github.com/machdyne/kolsch)
 - [Machdyne Lebkuchen](https://github.com/machdyne/lebkuchen)
 - [Machdyne Mozart](https://github.com/machdyne/mozart) / [ML1](https://github.com/machdyne/sechzig)
 - [ULX3S](https://radiona.org/ulx3s/) **UNTESTED**
 - (more soon)

If you have an unsupported board and want to try Zeitlos, please open an issue.

## Usage

1. Connect using a USB-UART PMOD, for example:

```
$ minicom -D /dev/ttyACM0 -b 1000000
```

2. Build and flash the system:

Building Zeitlos requires [Yosys](https://github.com/YosysHQ/yosys), [nextpnr-ecp5](https://github.com/YosysHQ/nextpnr), [prjtrellis](https://github.com/YosysHQ/prjtrellis) and a [RV32I toolchain](https://github.com/YosysHQ/picorv32#building-a-pure-rv32i-toolchain).

```
$ git submodule update --init --recursive
$ make BOARD=lakritz CABLE=dirtyJtag flash
```

The above command will build the SOC, BIOS, OS and apps and then write the gateware and kernel to flash.

The BIOS will automatically boot the kernel within a few seconds if no keys are pressed.

3. Put the apps on an sdcard:

You can either use the demo image (available soon):

```
gzip -dc images/zeitlos.img.gz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
```

Or use the `xf` command to upload apps to a FAT-formatted SD card and `run <file>` to start them. You will need to have the [xfer](https://github.com/machdyne/xfer) utility installed and configured in minicom.

## Developers

### Documentation

The Zeitlos documentation will be the [Timeless Computing](https://github.com/machdyne/tc) book, which will be included in the default Zeitlos distribution. The later chapters will explain the system, list the API, etc. 

The Zeitlos implementation portions of the book are currently located in the `docs` directory.

### LLM-generated code

This project makes use of LLMs for code and documentation.

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md) with the following exceptions:

- rtl/cpu/picorv32 uses the ISC license.
- rtl/ext/uart16550 uses the LGPL license.
- rtl/mem/sdram\_kianv uses the Apache 2.0 license.
- rtl/ext/usb\_hid\_host uses the Apache 2.0 license.
- sw/os/fs/fatfs uses a BSD compatible license.
