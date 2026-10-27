# Hardware, software and boards

```lesson
(objective "Decide when a task belongs in hardware, say what changes when an SOC moves to another board, and how an FPGA can become an old machine.")
(minutes 20)
(keywords "blitter" "wrapper module" "emulation")
```

You will see the choices an FPGA computer allows that an ordinary
computer does not.

Because its hardware can be changed, an FPGA computer can do a task in
software or in hardware. Software is easier to write and change.
Hardware can be much faster for some tasks: it does many operations at
once, and spends no cycles fetching and decoding instructions. The
tasks that suit it are small, regular and repeated often:

- copying and filling rectangles of pixels, done by a **blitter**;
- drawing lines;
- mixing several streams of sound;
- multiplying the large numbers of public-key cryptography;
- hash functions such as SHA-256.

The Zeitlos SOC can include hardware for each, and each makes a task
practical on its 48 MHz processor.

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: Hardware and Software Together")
(source "Zeitlos documentation: gpu_blitter (docs/gpu_blitter.md)")
```

```check
(choice
  (ask "A new program is too slow. What is the usual way to decide what to move into hardware?")
  (right "Write it all in software, measure where the time goes, and move only the slowest part")
  (wrong "Move the whole program into hardware at once"
    "Hardware is much harder to write and change than software. Only the small, regular part that takes the time is worth it.")
  (wrong "Move whatever part is easiest to write in Verilog"
    "The easiest part may take almost no time. Measure first: the slowest part is the one worth moving.")
  (hint "Find out where the time goes before changing anything."))
```

---

## Moving to another board

A design in an HDL is not tied to one board. Moving it is the most
common way of adapting an FPGA computer. The processor, the bus, the
devices, the address map and every program stay the same. What changes
is the physical board:

- **pins**: a new constraints file, each pin suiting its bank's voltage;
- **clocks**: a different crystal means new PLL settings, and a slower
  or fuller chip may need a slower clock;
- **memory**: each kind of RAM chip needs its own memory controller,
  and the amount of memory changes what software can do;
- **chip-specific blocks**: PLLs, block RAM and special I/O differ
  between FPGA families;
- **capacity**: a smaller chip may not hold the optional parts;
- **outputs**: video, USB and audio depend on what the board has.

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: Moving a Design to Another Board")
```

```check
(choice
  (ask "An SOC moves to a board with a different FPGA family and the same kind of memory. Which of these must change?")
  (right "The constraints file, for the new pins")
  (wrong "The programs that run on it"
    "The processor and the address map stay the same, so every program runs as before. The pins are what belong to the board.")
  (wrong "The processor's design"
    "The processor is the part least tied to the board. Pins, clocks and chip-specific blocks are what change.")
  (hint "Which of these depends on how the board is wired?"))
```

---

## Keeping the board in one place

A well-organized design keeps each chip-specific block in a small
**wrapper module**: a module with the same ports on every chip, whose
inside is the only part that changes. Everything for one board is kept
together: a top-level module, a constraints file and a short list of
settings.

A new board is brought up in small steps: first a blinking LED, which
proves the clock, the pins and the way of loading the bitstream; then a
serial port for messages; then a memory test; then the full system.

```check
(choice
  (ask "A new board shows nothing when the full system is loaded on it. What should have come first?")
  (right "A blinking LED, then a serial port, then a memory test")
  (wrong "A larger FPGA"
    "Nothing says the chip is too small. Small steps first show which part, if any, is wrong: the clock, the pins, the memory.")
  (wrong "Rewriting the processor for the board"
    "The processor does not depend on the board. Bring-up tests the board's own parts, one at a time.")
  (hint "Which test proves the clock and pins with the least else involved?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: Moving a Design to Another Board")
```

---

## Linux on an FPGA

Linux can run on an FPGA computer too. It needs a softcore with a
memory management unit, hardware that gives each program its own view
of memory, and at least tens of megabytes of memory. LiteX, an open
framework for building SOCs, can make such a system for many FPGA
boards; Machdyne's Kakao Linux is built this way.

Linux brings thousands of existing programs, at the cost of size,
complexity and time to start. A system designed for the hardware can be
small enough to be understood completely.

```check
(choice
  (ask "What does Linux on an FPGA computer give, and at what cost?")
  (right "Thousands of existing programs, at the cost of size, complexity and start-up time")
  (wrong "More speed, at the cost of fewer programs"
    "Linux makes the machine bigger and slower to start, not faster. What it brings is its many existing programs.")
  (wrong "A smaller system that is easier to understand"
    "That is what a system designed for the hardware gives. Linux is large and complex.")
  (hint "Think of what Linux already has, and how big it is."))
```

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: Linux on an FPGA")
```

---

## Recreating old machines

An FPGA can also become a computer from the past. Projects have
recreated the processors and custom chips of the Apple II, the
Commodore 64, the Amiga, early game consoles and many more, sometimes
down to single gates.

This differs from **emulation**, in which a program on a modern
processor imitates the old one instruction by instruction. The FPGA
becomes hardware that behaves like the original clock cycle by clock
cycle, so software that depended on exact timing also runs correctly.

```check
(choice
  (ask "An old game depends on exactly how many clock cycles its machine's video chip takes. Where is it most likely to run correctly?")
  (right "On an FPGA recreating the old chips cycle by cycle")
  (wrong "In emulation, since the program imitates every instruction"
    "Emulation imitates instructions, but not always the exact timing of every chip. A cycle-by-cycle recreation in hardware keeps the timing.")
  (wrong "Directly on a modern processor, which is faster"
    "A modern processor cannot run the old machine's code by itself at all, and speed is not what the game needs: it needs exact timing.")
  (hint "Which way behaves like the original, clock cycle by clock cycle?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Systems on a Chip: Recreating Old Machines")
```

So: an FPGA computer can move slow tasks into hardware, move to a new
board by changing only what belongs to the board, and become an old
machine, cycle for cycle.
