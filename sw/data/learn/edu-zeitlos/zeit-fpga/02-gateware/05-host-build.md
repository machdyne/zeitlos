# How Zeitlos's gateware is built

```lesson
(objective "Name the tools that build Zeitlos's gateware on a larger computer, and say why zfpga cannot build it.")
(minutes 15)
(keywords "yosys (tool)" "nextpnr (tool)" "ecppack (tool)" "ecpbram (tool)" "openFPGALoader (tool)" "seed (pattern)")
```

Zeitlos's gateware is built on a larger computer with open tools, one
for each step from Verilog to bitstream:

```text
yosys            rtl/*.v           ->  netlist (soc.json)
nextpnr-ecp5     the netlist       ->  placed and routed (soc.config)
ecpbram          puts the BIOS in  ->  soc_final.config
ecppack          compresses        ->  soc.bit
openFPGALoader   writes it to the board's flash
```

**yosys** synthesises, **nextpnr** places and routes, **ecppack**
packs the bitstream and **openFPGALoader** writes it over a cable. One
command runs them all, in the Zeitlos source, with the tools of
[docs:toolchain](docs:toolchain) installed:
`make BOARD=lakritz CABLE=dirtyJtag flash`.

```sources
(source "docs/zfpga.md, section 1; docs/toolchain.md, sections 1, 3 and 6")
```

```check
(choice
  (ask "Which tool decides where on the chip each LUT of the SOC goes?")
  (right "nextpnr")
  (wrong "yosys"
    "yosys synthesises: it turns the Verilog into LUTs and flip-flops, but does not say where they go.")
  (wrong "ecppack"
    "ecppack only turns the finished, placed design into a bitstream.")
  (hint "Placing and routing is one tool's job."))
```

---

## The BIOS, put in afterwards

The **BIOS**, the boot program, lives in block RAM inside the
gateware, at address 0. It is compiled from C separately. Placing and
routing the whole SOC again each time the BIOS changed would take many
minutes, so the build does something cleverer.

The Verilog fills the BIOS's block RAM with a random **seed** pattern.
After placing and routing, **ecpbram** finds that pattern in the
design and puts the real BIOS in its place. A new BIOS then costs
seconds, not a whole build.

```sources
(source "docs/zfpga.md, sections 1.1 and 25")
```

```check
(choice
  (ask "Why is the seed pattern random?")
  (right "So that it can be found in the finished design, and nothing else is mistaken for it")
  (wrong "So that the BIOS is encrypted"
    "Nothing is hidden. The seed is replaced by the real BIOS, which is stored as it is.")
  (wrong "Because the block RAM must start random"
    "Block RAM can start with anything. The pattern only has to be unique, so the tool can find exactly where it was put.")
  (hint "ecpbram has to find the pattern again."))
```

---

## Why not zfpga?

zfpga has a stage for each of these tools, and `zfpga bram` does
ecpbram's job. But it cannot build the SOC, and does not try to:

- it refuses parts of Verilog the SOC uses: memories large enough for
  block RAM, tristate pins, and `generate`;
- the SOC fills most of the chip, far more than zfpga places and
  routes today, and needs its global clock wiring;
- zfpga has no timing analysis, and the SOC runs close to its limits.

Building the SOC is one of zfpga's named non-goals.

```sources
(source "docs/zfpga.md, sections 1 (non-goals), 23.3 and 24.6")
```

```check
(choice
  (ask "Which of these can zfpga do for Zeitlos's own gateware on the machine?")
  (right "Put a new BIOS into a finished SOC bitstream, as ecpbram does")
  (wrong "Synthesise, place and route the whole SOC"
    "The SOC is a named non-goal: too large, and using Verilog zfpga refuses.")
  (wrong "Check the SOC's timing"
    "zfpga has no timing analysis at all.")
  (hint "Which of the host's steps needs no synthesis or routing?"))
```

So: yosys, nextpnr, ecpbram and ecppack build the SOC on a larger
computer; zfpga can stand in for ecpbram and ecppack, but not for
yosys and nextpnr.
