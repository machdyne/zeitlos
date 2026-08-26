# zeitlos32

An experimental RV32IM core for Zeitlos. **Not the default** — the SOC
still builds with PicoRV32 unless you uncomment one line in
`rtl/boards.vh`:

```verilog
`define CPU_ZEITLOS32
```

Full write-up: [`docs/zeitlos32.md`](../../../docs/zeitlos32.md).

```
zeitlos32.v            the core: a nine-state multicycle FSM
zeitlos32_muldiv.v     RV32M, sequential or DSP-backed
tests/                 make && go
```

Both files are self-contained. The only thing outside this directory
that knows zeitlos32 exists is the `ifdef`'d instantiation in
`rtl/sysctl.v`, the switch in `rtl/boards.vh`, and two lines in the
top-level `Makefile`.

## Quick start

```
cd tests && make
```

Needs `iverilog`, `cpp`, and RISC-V binutils — not the toolchain that
builds the OS.

## What it is

RV32IM, plus `rdcycle`/`rdinstret` (which `sw/os/` reads directly),
plus PicoRV32's custom interrupt ABI so `sw/bios/boot_picorv32.S` and
`sw/bios/custom_ops.S` need no changes. Native wishbone master, no
adapter. No PCPI, no compressed ISA, no IRQ timer, no trace port —
nothing in this tree uses them.

Roughly 20–35% fewer cycles than PicoRV32 on the same programs, and
about 15% more LUT4s. Meets 60MHz on ECP5 with margin. Not yet run on
hardware.

## Before committing a change

Simulation is not enough — three of the four bugs found in this core
so far were invisible to it. Always also run:

```
yosys -p "read_verilog zeitlos32.v zeitlos32_muldiv.v; \
          hierarchy -top zeitlos32_wb; synth_ecp5 -top zeitlos32_wb"
```

and read the warnings. "Multiple conflicting drivers" in particular
means a signal is assigned from both a combinational and a sequential
block; yosys resolves it to a constant, and the result simulates
perfectly and does nothing in hardware.
