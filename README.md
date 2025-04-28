# ![Zeitlos](https://github.com/machdyne/zeitlos/blob/bcca7d8a5dbba752f1f5e41afce82037e9b3b3ec/zeitlos.png)

Zeitlos is a work-in-progress SOC (System-on-a-Chip) and OS (Operating System) developed in tandem and intended to provide a responsive graphical environment for running timeless applications on FPGA computers.

Zeitlos is the successor to [Zucker](https://github.com/machdyne/zucker).

## Usage

1. Connect using a USB-UART PMOD, for example:

```
$ minicom -D /dev/ttyACM0 -b 1000000
```

2. Build and write the gateware (and BIOS) to the FPGA SRAM:

Building Zeitlos requires [Yosys](https://github.com/YosysHQ/yosys), [nextpnr-ecp5](https://github.com/YosysHQ/nextpnr), [prjtrellis](https://github.com/YosysHQ/prjtrellis) and a [RV32I toolchain](https://github.com/YosysHQ/picorv32#building-a-pure-rv32i-toolchain).

```
$ make PREFIX=/opt/riscv32i/bin/riscv32-unknown-elf BOARD=lakritz
$ openFPGALoader -c dirtyJtag output/lakritz/soc.bin
```

3. At the BIOS prompt, load the kernel into main memory by pressing 'x' and then upload `sw/os/kernel.bin`. You will need to have the [xfer](https://github.com/machdyne/xfer) utility installed and configured in minicom.

4. Boot the kernel by pressing 'b'.

5. Load an app using xfer and start a new process (for example `sw/apps/blinky.bin`):

```
> pu
```

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md) with the following exceptions:

- rtl/cpu/picorv32 uses the ISC license.
- rtl/ext/uart16550 uses the LGPL license.
