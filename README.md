# ![Zeitlos](https://github.com/machdyne/zeitlos/blob/bcca7d8a5dbba752f1f5e41afce82037e9b3b3ec/zeitlos.png)

Zeitlos is a work-in-progress SOC (System-on-a-Chip) and OS (Operating System) developed in tandem and intended to provide a responsive graphical environment for running timeless applications on FPGA computers.

Zeitlos is the successor to [Zucker](https://github.com/machdyne/zucker).

## Features

### SOC

| Component | Features |
|-----------|----------|
| CPU | 32-bit RISC-V (PicoRV32) |
| Bus | 32-bit Wishbone |
| Main Memory | SDRAM, PSRAM or SRAM |
| Framebuffer | 512x384x1bpp |
| Video | VGA, HDMI |
| Storage | MicroSD |
| HID | USB mouse/keyboard |
| I/O | GPIO, SPI, 16550 UART |

### OS

 - Pre-emptive multitasking
 - Flat memory model (no MMU)
 - FAT16/32 filesystem

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

2. Build and write the gateware (and BIOS) to the FPGA SRAM:

Building Zeitlos requires [Yosys](https://github.com/YosysHQ/yosys), [nextpnr-ecp5](https://github.com/YosysHQ/nextpnr), [prjtrellis](https://github.com/YosysHQ/prjtrellis) and a [RV32I toolchain](https://github.com/YosysHQ/picorv32#building-a-pure-rv32i-toolchain).

```
$ make PREFIX=/opt/riscv32i/bin/riscv32-unknown-elf- BOARD=lakritz
$ make PREFIX=/opt/riscv32i/bin/riscv32-unknown-elf- BOARD=lakritz CABLE=dirtyJtag dev-prog
```

3. At the BIOS prompt, load the kernel into main memory by pressing `x` and then upload `sw/os/kernel.bin`. You will need to have the [xfer](https://github.com/machdyne/xfer) utility installed and configured in minicom.

4. Boot the kernel by pressing `b`.

5. Use `xf` to upload apps to an SD card and `run <file>` to start them.

## Developers

### Documentation

The Zeitlos documentation will be the [Timeless Computing](https://github.com/machdyne/tc) book. The later chapters will explain the system, list the API, etc.

### LLM-generated code

To the extent that there is LLM-generated code in this repo, it should be space indented. Any space indented code should be carefully audited and then converted to tabs. 

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md) with the following exceptions:

- rtl/cpu/picorv32 uses the ISC license.
- rtl/ext/uart16550 uses the LGPL license.
- rtl/ext/usb\_hid\_host uses the Apache 2.0 license.
- sw/os/fs/fatfs uses a BSD compatible license.
