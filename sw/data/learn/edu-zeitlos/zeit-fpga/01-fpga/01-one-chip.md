# One chip, many machines

```lesson
(objective "Say what the FPGA in a Zeitlos machine does, and what its gateware is.")
(minutes 15)
(keywords "FPGA (chip)" "ECP5 (FPGA)" "SOC (FPGA)" "zfpga (tool)" "configuration memory (FPGA)" "configuration flash (FPGA)" "bitstream (FPGA)" "gateware (FPGA)" "memory (of a computer)" "flash (memory)")
```

Your machine has no processor chip of the usual kind. Its processor,
its graphics, its USB and its sound are all circuits inside one chip:
an **FPGA**, a field-programmable gate array. An FPGA is a grid of
small, identical logic blocks with programmable wiring between them.
Set the blocks and the wiring one way, and the chip is a blinking
light; set them another way, and it is a whole computer.

On Machdyne's computers the FPGA is a Lattice **ECP5**. The rest of
the board is what an FPGA cannot be: **memory** (the chips that hold
the programs and data the computer is working on), connectors, a
clock crystal and power.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: The Big Picture; The Machine: The Hardware")
```

```check
(choice
  (ask "Where is the processor that runs Zeitlos?")
  (right "It is a circuit set up inside the FPGA")
  (wrong "On a separate processor chip beside the FPGA"
    "There is none. The processor is one of the circuits the FPGA is set up to be, built from its logic blocks like the graphics and the USB.")
  (wrong "In the memory chips"
    "The memory holds programs and data. The circuit that runs them is in the FPGA.")
  (hint "Everything the computer does is a circuit in one chip."))
```

---

## Set up at every start

How each block is set and how the blocks are wired is held in the
FPGA's **configuration memory**. On the ECP5 that is static RAM: it
forgets everything when the power goes.

So the settings are kept in a small **flash** chip beside the FPGA,
the **configuration flash**. Flash is memory that keeps its contents
without power. At every power-on the FPGA reads its settings from
there, and only then does the circuit start to run.

The file of settings is a **bitstream**. Loading a bitstream is what
turns the blank chip into your computer, a fraction of a second after
you switch it on.

```check
(choice
  (ask "You switch the machine off and on again. Where does the FPGA get its circuit from?")
  (right "It reads the bitstream again from the configuration flash")
  (wrong "Its configuration memory still holds it"
    "On the ECP5 the configuration memory is static RAM, which forgets everything without power. It has to be loaded again at every start.")
  (wrong "Zeitlos sends it over USB"
    "Zeitlos is not running yet: it needs the circuit first. The FPGA loads the bitstream itself, from the flash beside it.")
  (hint "Which of the two keeps its contents without power: the configuration memory or the flash?"))
```

---

## Gateware

The design that the FPGA becomes when your machine starts is called
the **gateware**, by analogy with software. Zeitlos's gateware is the
**Zeitlos SOC**, a system on a chip (**SOC**): a processor, its memory
controller, graphics, sound, USB and the rest, written in Verilog. Its
source is open, in the folder `rtl` of the Zeitlos source.

The software -- the desktop, the apps, this lesson -- runs on the
processor that the gateware makes. Change a program, and the computer
is the same computer running something else. Change the gateware, and
the computer itself is different.

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip; The Machine: Adapting the Machine")
```

```check
(choice (several)
  (ask "Which of these are part of the gateware? Choose every one.")
  (right "The processor's circuit")
  (right "The blitter, which copies rectangles of pixels")
  (wrong "The text editor"
    "The text editor is a program. It runs on the processor; it is not a circuit in the FPGA.")
  (wrong "The file browser"
    "The file browser is software, loaded into memory and run by the processor. The processor itself is gateware.")
  (hint "Gateware is circuits. Software is what those circuits run."))
```

---

## A different bitstream, a different machine

Because the whole computer is a configuration, the same board can be
another computer. Load a different bitstream and the FPGA becomes
whatever that bitstream describes: a test pattern, a recreation of an
old home computer, or a system that runs Linux.

On many Machdyne boards the configuration flash is on a small
removable module. Swap the module, and at the next power-on the board
is a different machine; put the first module back, and it is Zeitlos
again.

```check
(choice
  (ask "A friend's board runs Zeitlos. They load a bitstream for another system. What has changed?")
  (right "The circuit the FPGA is: the computer itself")
  (wrong "Only the programs; the processor stays the same"
    "A bitstream sets the whole FPGA. The new one may have a different processor, or none at all.")
  (wrong "Nothing, until they reinstall Zeitlos's programs"
    "The circuit changes as soon as the new bitstream is loaded. Zeitlos's programs need Zeitlos's processor and devices, which are gone while the other bitstream runs.")
  (hint "What does a bitstream set?"))
```

---

## Building gateware on the machine

Gateware is usually built on a larger computer, with open tools. This
course is about doing it on the machine itself, with **zfpga**, an
experimental set of FPGA tools that runs on Zeitlos. It can build
small designs, such as a blinking light, but not a design as large as
Zeitlos's own gateware. [docs:zfpga](docs:zfpga) is its full story.

```sources
(source "docs/zfpga.md, the status at the top and section 1")
```

```check
(choice
  (ask "What can zfpga build on the machine today?")
  (right "Small designs, such as a counter that blinks a light")
  (wrong "Zeitlos's own gateware"
    "Zeitlos's gateware fills most of the chip and uses parts of Verilog zfpga does not take yet. It is built on a larger computer.")
  (wrong "Nothing yet: it is only a plan"
    "zfpga works: a blinking-light design built on a Lakritz, by the Lakritz, blinks its light.")
  (hint "It is experimental, and it has worked on a real board."))
```

So: the FPGA becomes the whole computer each time it loads a bitstream
from its configuration flash, and the design it becomes is the gateware.
