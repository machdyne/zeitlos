# Configuration and choice

```lesson
(objective "Say how an FPGA gets its configuration at every start, how big designs are, and when an FPGA is the right chip.")
(minutes 20)
(keywords "open tools" "bitstream" "microcontroller" "MCU" "ASIC" "application-specific integrated circuit")
```

You will see how an FPGA becomes its circuit each time it starts, and
when an FPGA is the right kind of chip to use.

The file that sets every LUT, flip-flop, switch and pin is a
**bitstream**. It is typically from a few tens of kilobytes to a few
megabytes.

At power-on, the FPGA reads its bitstream from a small SPI flash chip
beside it, which takes a fraction of a second. Only then does the
circuit it describes start to run.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Configuration")
```

```check
(choice
  (ask "Why does an FPGA board have a flash chip beside the FPGA?")
  (right "The FPGA's configuration memory forgets at power-off, so the bitstream is kept in flash and read at every start")
  (wrong "The FPGA runs its circuit from the flash chip"
    "The circuit runs in the FPGA itself, from its configuration memory. The flash only keeps the bitstream while the power is off.")
  (wrong "The flash chip holds the FPGA's LUTs"
    "The LUTs are inside the FPGA. Flash keeps their bits, and the rest of the bitstream, while the power is off.")
  (hint "Recall what kind of memory an FPGA's configuration memory is."))
```

---

## Other ways in

For testing, a bitstream can also be sent straight into the FPGA from
another computer over a USB cable. It stays only until the power is
turned off.

On many of Machdyne's boards, the flash chip is on a small removable
module. Change the module and you change the whole design of the
computer: one board can be one kind of computer with one module, and a
quite different one with another. A flash chip can also hold more than
one bitstream, and some FPGAs can be told to reload themselves from
another place in it.

```check
(choice
  (ask "You send a new bitstream to a board over USB, try it, then switch the board off and on. What runs now?")
  (right "The bitstream in the board's flash")
  (wrong "The new bitstream you sent over USB"
    "A bitstream sent over the cable goes only into the configuration memory, which forgets at power-off. At the next start the FPGA reads its flash again.")
  (wrong "Nothing, until a bitstream is sent again"
    "At power-on the FPGA reads its bitstream from flash by itself. The one from the cable is gone, but the one in flash is still there.")
  (hint "Where does the FPGA look for its bitstream at power-on?"))
```

---

## How big designs are

An FPGA's size is usually given in LUTs. Boards for small computers,
Machdyne's among them, use chips of a few thousand to tens of
thousands of LUTs.

| Design | LUTs, roughly |
|---|---|
| blinking an LED (a small light) | a dozen or two |
| a serial port | a few hundred |
| a small 32-bit RISC-V processor | 1,000 to 3,000 |
| a complete computer (Zeitlos) | 15,000 to 20,000 |

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: FPGA Capacity")
```

```check
(number
  (ask "A complete computer of 18,000 LUTs is built on an FPGA of 45,000 LUTs. What percentage of the LUTs does it use?")
  (answer 40) (unit "%") (from "18000 / 45000 * 100")
  (wrong-if 60 "60% is the part left free. The computer uses 18,000 of the 45,000.")
  (wrong-if 250 "That is 45,000 divided by 18,000, the wrong way round. Divide the part used by the whole.")
  (hint "Divide 18,000 by 45,000, then multiply by 100."))
```

---

## Families and open tools

Several companies make FPGAs. Some families have **open tools**: tools
whose source code anyone may read, change and share, written by people
who studied the chips and wrote down their configuration formats.

- **Lattice iCE40**: small, simple, low-power; the first with a complete set of open tools.
- **Lattice ECP5**: larger, with DSP blocks and plenty of block RAM; the main family in Machdyne's computers.
- **Xilinx (AMD) 7-series**: large and fast, such as the Artix-7, with partial open support.
- **Cologne Chip GateMate**: designed and made in Germany, with open tools.

A design that can be built only with one company's licensed software
can be kept up only while that software is available and runs. A design
built with open tools, whose own source can be rebuilt, can be kept up
indefinitely.

```check
(choice
  (ask "Why do open tools matter for a computer meant to last?")
  (right "The design can still be built after a company stops offering its software")
  (wrong "Open tools make the circuit run faster"
    "Open or not, the tools make the same kind of circuit. What open tools give is that they can always be had and rebuilt.")
  (wrong "Only open tools can make a bitstream"
    "Each maker's own tools make bitstreams too. The trouble is that they can be built and run only while the maker allows.")
  (hint "Think of the design in twenty years."))
```

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: FPGA Families and Open Tools")
```

---

## Three kinds of chip

A **microcontroller**, or **MCU**, is a small processor chip with its
devices built in. An **ASIC**, an **application-specific integrated
circuit**, is a chip made for one design.

| | MCU | FPGA | ASIC |
|---|---|---|---|
| What it is | fixed CPU, devices | programmable logic | fixed logic |
| Changed by | new software | new bitstream | a new chip |
| Speed | modest | modest | highest |
| Cost each | lowest | moderate | lowest in bulk |
| Start-up cost | minimal | low | enormous |

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: FPGAs, Processors and Custom Chips")
```

```check
(choice
  (ask "A company will make ten million copies of a phone charger's controller, and its design will never change. Which kind of chip fits?")
  (right "An ASIC")
  (wrong "An FPGA"
    "An FPGA's strength is that its hardware can be changed. Here nothing will change, and an ASIC costs least each when millions are made.")
  (wrong "Whichever is fastest; speed is all that matters"
    "A charger needs little speed. What decides it here is ten million copies of a design that never changes: an ASIC.")
  (hint "Look at the rows for cost each and start-up cost."))
```

---

## When an FPGA fits

An FPGA fits when the hardware itself must be designed, studied or
changed: for parallel work, unusual interfaces, recreations of old
machines, and computers whose owners can read and improve the
processor.

It also guards against a chip going out of production. A processor
written as a design is not tied to one chip. When that FPGA is no
longer made, the design can be moved to another, and the computer
outlasts the chip it was first built on.

```check
(choice
  (ask "The FPGA in a computer goes out of production. What happens to the computer's design?")
  (right "It can be moved, with some changes, to another FPGA")
  (wrong "It is lost with the chip"
    "The design is a description, kept apart from any one chip. It can be built again for another FPGA.")
  (wrong "It must be made into an ASIC"
    "An ASIC costs an enormous amount to start. The design can simply be moved to another FPGA.")
  (hint "Where does the design of an FPGA computer live?"))
```

So: an FPGA reads its bitstream from flash at every start, and it is the
chip to choose when the hardware itself must be able to change.
