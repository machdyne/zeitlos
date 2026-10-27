# Changing Zeitlos's own gateware

```lesson
(objective "Say how Zeitlos's own gateware is changed today, and which part of that zfpga can do on the machine.")
(minutes 15)
(keywords "known good bitstream (FPGA)")
```

To change the Zeitlos SOC itself -- leave a part out, add one, try
the Zeitlos32 core -- you change its Verilog and build it on a larger
computer, with the tools you met in the second unit:

1. Change the source: for example, uncomment `` `define CPU_ZEITLOS32``
   in `rtl/boards.vh`.
2. Build for your board: `make BOARD=lakritz` and the rest, as
   [docs:toolchain](docs:toolchain) describes.
3. Check the timing report: every clock must meet its target.
4. Try it where it cannot harm the working system: over a cable into
   configuration memory, or on a spare MMOD.
5. Only then write it to the board's own flash.

```sources
(source "docs/toolchain.md, sections 3 and 6; docs/zeitlos32.md; Timeless Computing (docs/tc.md), The Machine: An Example Adaptation")
```

```check
(choice
  (ask "Your changed SOC builds, but the report shows CLK_48 below 48 MHz. What do you do?")
  (right "Do not use it yet: change the design until timing is met")
  (wrong "Flash it: it built, so it works"
    "A design that misses timing still programs, and may fail when warm or in rare, confusing ways. Fix the timing first.")
  (wrong "Try it on the board's own flash and see"
    "Even if it seems to work, it may fail later. And the own flash is the last place to try anything.")
  (hint "What does fmax below the clock mean?"))
```

---

## Keep a known good bitstream

Change one thing at a time, and keep a **known good bitstream**: the
last one that worked, on its own MMOD or as a file you can flash
again. Then any experiment can be undone in a few minutes, and when
something breaks you know which change broke it.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: Debugging on the Board; The Machine: Getting Back to Safety")
```

```check
(choice
  (ask "You made three changes to the SOC at once and it no longer starts. What should you have done?")
  (right "Made one change at a time, testing each, with a known good bitstream to go back to")
  (wrong "Nothing different: three changes are as easy to check as one"
    "With three changes you cannot tell which broke it. One at a time, each failure points at its cause.")
  (hint "How do you find which of three changes broke it?"))
```

---

## What zfpga can do for the SOC

On the machine, zfpga can do some of the host's jobs on bitstreams:

- **`zfpga bram`** puts new block RAM contents into a finished SOC
  bitstream, as ecpbram does: a new BIOS, without placing and routing
  again. It needs the seed file the gateware was built with, and
  keeps the bitstream's boot address.
- **`zfpga pack`** and **`unpack`** turn tile settings into a
  bitstream and back, byte for byte as the host's tools do on every
  design tested so far. The whole SOC's settings are still to be
  tried.

What it cannot do yet: build the SOC from its Verilog, or add a block
of your own to the running system. A way to place a small design of
yours into a space left in Zeitlos's gateware is planned, not built.

```sources
(source "docs/zfpga.md, sections 1.1, 7 (Phase 9) and 25; docs/zfpga-formats.md, section 9")
```

```check
(choice
  (ask "You want Zeitlos with a new hardware block of your own. What does that take today?")
  (right "Change the SOC's Verilog and build it on a larger computer with the host tools")
  (wrong "zfpga build on the machine, with the SOC's rtl folder"
    "zfpga cannot build the SOC: it is too large and uses Verilog zfpga refuses.")
  (wrong "Build the block with zfpga and load it beside the running Zeitlos"
    "The ECP5 loads whole, so a bitstream replaces Zeitlos. Fitting your block into Zeitlos's gateware on the machine is planned, not built.")
  (hint "Which tools can build the whole SOC?"))
```

---

```check
(choice
  (ask "Which job on Zeitlos's own gateware can zfpga do on the machine?")
  (right "Put a new BIOS into the finished bitstream with zfpga bram")
  (wrong "Switch the processor to Zeitlos32"
    "That changes the Verilog, and the SOC has to be synthesised, placed and routed again: a job for the host's tools.")
  (wrong "Check the SOC's timing"
    "zfpga has no timing analysis.")
  (hint "Which job needs no synthesis or routing?"))
```

So: the SOC is changed in its Verilog and built on a larger computer,
one change at a time with a known good bitstream kept; on the machine,
zfpga can already give it a new BIOS.
