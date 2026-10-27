# The Zeitlos SOC

```lesson
(objective "Find the gateware's source, its top module and its processor.")
(minutes 15)
(keywords "top module (FPGA)" "PicoRV32 (core)" "Zeitlos32 (core)" "define (Verilog)")
```

Zeitlos's gateware is open Verilog, in the folder `rtl` of the Zeitlos
source (github.com/machdyne/zeitlos). It is about 175 Verilog files.

One module holds all the others: the **top module**, `sysctl`, in
`rtl/sysctl.v`. It makes an instance of the processor, the memory
controller, the graphics, the sound, USB and every other part, and
wires them together. Its ports are the FPGA's pins.

```sources
(source "rtl/sysctl.v; Timeless Computing (docs/tc.md), Describing Hardware: Verilog Modules")
```

```check
(choice
  (ask "Where would you start reading to see how the parts of the SOC are joined?")
  (right "rtl/sysctl.v, the top module")
  (wrong "rtl/boards.vh"
    "boards.vh chooses what each board includes. The joining of the parts, instance by instance, is in the top module.")
  (wrong "The kernel's source"
    "The kernel is software that runs on the SOC. The SOC's own wiring is Verilog, in rtl.")
  (hint "Which module makes instances of all the others?"))
```

---

## The processor

The processor is **PicoRV32**, a small, well-tested open RISC-V core,
in `rtl/cpu/picorv32`. It runs the RV32IM instructions at 48 MHz, from
the board's CLK_48.

Beside it is **Zeitlos32**, an experimental core of Zeitlos's own, in
`rtl/cpu/zeitlos32`: one state machine of nine states, with no
pipeline. It is not the default. One **define**, a name set for the
Verilog to test, chooses it: the line `` `define CPU_ZEITLOS32`` in
`rtl/boards.vh`, left commented out. Uncommented, it chooses Zeitlos32 in place of
PicoRV32; everything else in the SOC stays the same.

```sources
(source "docs/zeitlos32.md; Timeless Computing (docs/tc.md), The Machine: The System on the Chip")
```

```check
(choice
  (ask "What does uncommenting `define CPU_ZEITLOS32 in rtl/boards.vh change?")
  (right "Which processor core the gateware is built with")
  (wrong "Nothing until the kernel is rewritten for the new core"
    "Both cores run the same instructions and the same interrupt scheme, so the software is untouched.")
  (wrong "The running machine's processor, at once"
    "It changes the source. The gateware has to be built again and loaded before the FPGA is a different processor.")
  (hint "It is a line in the Verilog source."))
```

---

## Plain Verilog

The whole of `rtl` is written in plain Verilog-2001: modules, `wire`,
`reg`, `assign` and `always` blocks, with no SystemVerilog. That
matters here: the plainer the language, the smaller the tool that can
read it. zfpga reads this Verilog, and 15 of the SOC's own modules,
among them the UART, the GPIO and the audio mixer, come out of zfpga's
synthesis doing exactly what they do from the usual tool's.

```sources
(source "docs/zfpga.md, sections 4.2 and 24.6")
```

```check
(choice
  (ask "zfpga synthesises 15 of the SOC's modules correctly. Can it build the whole SOC?")
  (right "No: other modules use things it does not take yet, and the SOC is far larger")
  (wrong "Yes: if each module works, the whole works"
    "Many modules are refused: those with block RAM, tristate pins or generate. And the whole SOC fills most of the chip, far beyond what zfpga places and routes today.")
  (hint "15 is some of the modules, not all."))
```

So: the gateware is the Verilog in rtl, joined by the top module
sysctl, with PicoRV32 as its processor.
