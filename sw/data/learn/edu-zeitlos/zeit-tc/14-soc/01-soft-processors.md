# Soft processors

```lesson
(objective "Say what a system on a chip and a soft processor are, and what a soft processor gives up and gains.")
(minutes 15)
(keywords "system on a chip" "SOC" "gateware" "soft processor" "softcore" "hard processor")
```

You will see how a whole computer becomes one FPGA design.

A **system on a chip**, or **SOC**, puts a complete computer on one
chip: its processor, memory controller, graphics and other devices.
The processors in phones and tablets are SOCs: each is designed once,
made in millions, and cannot be changed once made.

In an FPGA, an SOC is a design like any other. Such a design is often
called **gateware**, by analogy with software, and every part of it can
be read and changed.

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip")
```

```check
(choice
  (ask "What is the difference between the SOC in a phone and an SOC in an FPGA?")
  (right "The phone's is fixed when the chip is made; the FPGA's is gateware that can be changed")
  (wrong "Only the phone's has a processor"
    "An SOC in an FPGA has a processor too, built from the FPGA's logic. The difference is that it can be read and changed.")
  (wrong "There is none: both are fixed chips"
    "An FPGA's SOC is a design loaded as a bitstream. Load a changed design, and the computer changes.")
  (hint "Which of the two can be loaded again with a new design?"))
```

---

## Soft and hard processors

A processor described in an HDL and built in an FPGA's logic is a
**soft processor**, or **softcore**. Some FPGAs also have a fixed
**hard processor** beside their logic: faster, but not changeable.

There are many open RISC-V softcores. Small ones finish an instruction
every few cycles and use one or two thousand LUTs; bigger ones work on
several instructions at once and add caches, at several times the size. The Zeitlos SOC uses
PicoRV32, a small, well-tested RISC-V core, and has an experimental
core of its own, [Zeitlos32](docs:zeitlos32).

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: Soft Processors")
(source "Zeitlos documentation: zeitlos32 (docs/zeitlos32.md)")
```

```check
(choice
  (ask "A soft processor has a mistake in how it does one instruction. How is it put right?")
  (right "Change its HDL source and build a new bitstream")
  (wrong "Replace the FPGA chip"
    "The processor is not in the chip as made; it is in the bitstream. A corrected description makes a corrected processor in the same chip.")
  (wrong "It cannot be put right"
    "That is true of a hard processor, fixed when made. A soft processor is a description, and descriptions can be corrected.")
  (hint "Where does a soft processor's design live?"))
```

---

## What a softcore gives

A soft processor is much slower than a hard one, typically tens of
megahertz. In return:

- **It can be read.** Every part of it is in its source, so it can be
  checked, studied and trusted as no commercial chip can.
- **It can be fixed**, by changing the source and rebuilding.
- **It can be extended** with new instructions for a particular need.
- **It can be replaced.** A machine can later be given another
  instruction set, by loading another design into the same hardware.

```check
(choice (several)
  (ask "Which of these does a soft processor allow that a fixed processor chip does not?")
  (right "Reading every part of the processor's design")
  (right "Adding a new instruction")
  (wrong "Running faster than a hard processor"
    "A soft processor is slower, typically tens of megahertz, because it is built from FPGA logic and routing.")
  (wrong "Running without any configuration"
    "Like every FPGA design, it must be loaded as a bitstream at each start.")
  (hint "Two are true; think of what a description lets you do."))
```

---

```check
(choice
  (ask "A small computer built on an FPGA runs at 48 MHz, not thousands of MHz. Why?")
  (right "Its processor is a soft processor, built from FPGA logic and routing")
  (wrong "Its clock is set low to save power"
    "A slower clock does save power, but that is not the main reason. Logic built from LUTs and routing is several times slower than a custom chip's.")
  (wrong "RISC-V processors cannot run faster"
    "RISC-V processors in custom chips run at thousands of MHz. The limit here is the FPGA's logic and routing.")
  (hint "Recall where much of the delay in an FPGA is."))
```

So: an SOC in an FPGA is gateware, and its soft processor is slower
than a hard one but can be read, fixed, extended and replaced.
