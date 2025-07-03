# ![Zeitlos](https://github.com/machdyne/zeitlos/blob/bcca7d8a5dbba752f1f5e41afce82037e9b3b3ec/zeitlos.png)

Zeitlos is a work-in-progress SOC (System-on-a-Chip) and OS (Operating System) developed in tandem and intended to provide a responsive graphical environment for running timeless applications on FPGA computers.

Zeitlos is the successor to [Zucker](https://github.com/machdyne/zucker).

## Features

### SOC

| Component | Features/Notes |
|-----------|----------|
| CPU | 32-bit RISC-V (PicoRV32) |
| GPU | Line rasterizer and blitter with clipping |
| MTU | Virtual addressing through Memory Translation Unit |
| Bus | 32-bit Wishbone |
| Main Memory | SDRAM, PSRAM or SRAM |
| Framebuffer | 512x384x1bpp |
| Video | VGA, HDMI |
| Storage | MicroSD |
| HID | USB mouse/keyboard |
| I/O | GPIO, SPI, 16550 UART |

### OS

 - Pre-emptive multitasking
 - Flat memory model with virtual address space for apps
 - FAT16/32 filesystem

#### Memory Translation Unit

Zeitlos doesn't have an MMU but instead has a single virtual address space that is remapped to a main memory address during context switches.

The Zeitlos kernel is located at `0x4000_0000` which is the beginning of main memory, and apps are loaded immediately after the kernel. However, each app executes at fixed address `0x8000_0000` which is a mirror of their actual address in the main memory. The translation base address register is set during context switches so that each app can access its own memory through `0x8000_0000`.

With the MTU, there is no need for position independent code or complicated address relocation.

### Boards

Zeitlos will initially support ECP5, Artix-7, GateMate FPGAs.

The following boards are currently supported:

 - [Machdyne Obst](https://github.com/machdyne/obst)
 - [Machdyne Lakritz](https://github.com/machdyne/lakritz)
 - (more soon)

If you have an unsupported board and want to try Zeitlos, please open an issue.

## Usage

1. Connect using a USB-UART PMOD, for example:

```
$ minicom -D /dev/ttyACM0 -b 1000000
```

2. Build and write the gateware and kernel to flash:

Building Zeitlos requires [Yosys](https://github.com/YosysHQ/yosys), [nextpnr-ecp5](https://github.com/YosysHQ/nextpnr), [prjtrellis](https://github.com/YosysHQ/prjtrellis) and a [RV32I toolchain](https://github.com/YosysHQ/picorv32#building-a-pure-rv32i-toolchain).

```
$ make BOARD=lakritz CABLE=dirtyJtag flash
```

3. The BIOS will automatically boot the kernel within a few seconds if no keys are pressed.

4. Use `xf` to upload apps to a FAT-formatted SD card and `run <file>` to start them. You will need to have the [xfer](https://github.com/machdyne/xfer) utility installed and configured in minicom. 

## Developers

### Documentation

The Zeitlos documentation will be the [Timeless Computing](https://github.com/machdyne/tc) book, which will be included in the default Zeitlos distribution. The later chapters will explain the system, list the API, etc. 

The Zeitlos implementation portions of the book are currently located in the `docs` directory.

### LLM-generated code

To the extent that there is LLM-generated code in this repo, it should be space indented. Any space indented code should be carefully audited and then converted to tabs (eventually). 

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md) with the following exceptions:

- rtl/cpu/picorv32 uses the ISC license.
- rtl/ext/uart16550 uses the LGPL license.
- rtl/ext/usb\_hid\_host uses the Apache 2.0 license.
- sw/os/fs/fatfs uses a BSD compatible license.
