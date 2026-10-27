# The rest of the chip

```lesson
(objective "Name the parts of an FPGA besides its LUTs, and say what each is for.")
(minutes 20)
(keywords "carry chain" "block RAM" "DSP block" "I/O block" "I/O bank" "differential signal" "configuration memory")
```

You will meet the other parts an FPGA is made of. They sit in a
regular grid:

- **logic blocks**, each with its LUTs;
- **routing**: wires running between the blocks, and switches that
  join them where they cross;
- **block RAM**: blocks of memory built into the chip;
- **DSP blocks**: ready-made multipliers;
- **I/O blocks** around the edge, each joining the inside of the chip
  to one of its pins.

```figure
(define names '("I/O" "I/O" "I/O" "I/O" "I/O" "I/O"
                "I/O" "LB" "LB" "RAM" "LB" "I/O"
                "I/O" "LB" "LB" "RAM" "LB" "I/O"
                "I/O" "LB" "DSP" "LB" "LB" "I/O"
                "I/O" "I/O" "I/O" "I/O" "I/O" "I/O"))
(for-range 0 5 (lambda (r) (line 8 (+ 52 (* 40 r)) 312 (+ 52 (* 40 r)) 'grey 1)))
(for-range 0 7 (lambda (c) (line (+ 12 (* 48 c)) 8 (+ 12 (* 48 c)) 214 'grey 1)))
(for-range 0 30
  (lambda (k)
    (let ((x (+ 16 (* 48 (modulo k 6)))) (y (+ 16 (* 40 (floor (/ k 6))))))
      (rect x y 40 32 'paper)
      (frame x y 40 32 'ink 1)
      (text (+ x 20) (+ y 21) (list-ref names k) 'centre 'small))))
(caption "LB: logic block. Grey lines: the routing between the blocks.")
```

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: The Big Picture")
```

```check
(choice
  (ask "A signal inside the FPGA has to reach a pin of the chip. Which part does it go out through?")
  (right "An I/O block")
  (wrong "A logic block"
    "Logic blocks do the design's logic, inside the chip. Only the I/O blocks, around the edge, are joined to the pins.")
  (wrong "Block RAM"
    "Block RAM holds data inside the chip. The parts joined to the pins are the I/O blocks.")
  (hint "Look at what sits around the edge of the grid."))
```

---

## A flip-flop with every LUT

Each LUT in a logic block is paired with a **flip-flop**, so that its
output can be stored at a clock edge. A LUT and its flip-flop make a
small piece of combinational logic followed by a register: the basic
unit of a clocked design.

Logic blocks also have **carry chains**: fast, dedicated wires that
pass the carry from one block to the next. Adders and counters are so
common that the chip gives their carries a path of their own.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Flip-Flops and Carry Chains")
```

```check
(number
  (ask "On a Lattice ECP5 FPGA, each carry cell adds two bits. How many carry cells does a 24-bit counter use?")
  (answer 12) (from "24 / 2")
  (wrong-if 24 "24 is the number of flip-flops, one for each bit. Each carry cell handles two bits.")
  (wrong-if 48 "Each carry cell handles two bits, so there are fewer cells than bits, not more.")
  (hint "24 bits, two bits a cell."))
```

---

## Routing

Most of an FPGA's area is **routing**, not logic. Each switch where two
wires can be joined is set by one stored bit.

Much of the delay in an FPGA is in the routing too. A signal that
crosses the chip through many switches arrives later than one that goes
to a neighbouring block. So the same design can reach a faster or a
slower clock depending on where its parts are placed. And an FPGA runs
a circuit several times more slowly than a custom chip, whose
connections are direct metal wires.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Routing")
```

```check
(choice
  (ask "The same design is built twice, with its parts placed differently on the chip. One version can run at a faster clock. Why?")
  (right "Its signals pass through shorter routes, with fewer switches")
  (wrong "Its LUTs hold simpler functions"
    "The functions are the same in both. A LUT takes the same time whatever it holds; the difference is in the routing between blocks.")
  (wrong "It uses fewer flip-flops"
    "Both versions are the same design, with the same flip-flops. Only where the parts sit, and so the routes between them, changed.")
  (hint "Where is much of the delay in an FPGA?"))
```

---

## Block RAM and DSP blocks

Memory made of LUTs and flip-flops would use them up fast. So FPGAs
have **block RAM**: SRAM blocks of a few kilobits each (a kilobit is
1,024 bits), spread across the chip. A design uses them where it needs fast memory on the chip: a
processor's caches and its first program, a font for the screen,
buffers for data coming in.

**DSP blocks** are multipliers, often with adders, for arithmetic in
sound, graphics and cryptography. A multiplier made of LUTs would be
large and slow; a DSP block multiplies in one clock cycle.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Block RAM and DSP Blocks")
```

```check
(number
  (ask "An FPGA has 56 blocks of RAM, each of 18 kilobits. How many kilobytes is that in all? (8 bits make a byte.)")
  (answer 126) (unit "KB") (from "56 * 18 / 8")
  (wrong-if 1008 "1,008 is the total in kilobits. Divide by 8 for kilobytes.")
  (wrong-if 144 "That is 18 times 8. Multiply the number of blocks by the size of each first, then divide by 8.")
  (hint "First the total in kilobits: 56 times 18.")
  (hint "Then 8 bits make a byte, so divide by 8."))
```

---

## I/O blocks

Each pin has an **I/O block**, set up as an input, an output or both.
Its configuration sets which voltage standard it uses, how strongly it
drives an output, and whether it captures data at a clock edge right at
the pin.

Two pins can work together as a pair for a **differential signal**:
fast data on two wires, one the opposite of the other. Fast links use
them, such as HDMI video.

Every I/O block belongs to an **I/O bank**, a group of pins sharing
one supply voltage. The board's designer chooses that voltage, often
3.3 V, and every pin in the bank must use it.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: I/O Blocks")
```

```check
(choice
  (ask "A board supplies one I/O bank with 3.3 V. Can one pin in that bank work at 1.8 V?")
  (right "No: every pin in a bank uses the bank's voltage")
  (wrong "Yes: each I/O block sets its own voltage"
    "An I/O block sets its standard within its bank, but the supply voltage is shared by the whole bank. A 1.8 V pin goes in a bank supplied with 1.8 V.")
  (wrong "Yes, if the pin is an input"
    "Inputs and outputs alike take the bank's supply voltage. A pin that needs 1.8 V belongs in a bank supplied with 1.8 V.")
  (hint "What do the pins of one bank share?"))
```

---

## Configuration memory

Which blocks are used, how each is set up, and how they are joined is
held in **configuration memory** spread throughout the chip: the bits
of every LUT and every switch. In most FPGAs, including all of those
in Machdyne's computers, this memory is SRAM.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: The Big Picture")
```

```check
(choice
  (ask "An FPGA's configuration memory is SRAM. What does that mean when the power is switched off and on?")
  (right "The configuration is lost, and must be loaded again at every start")
  (wrong "The chip keeps its circuit, ready to run at once"
    "SRAM keeps its bits only while it has power. At power-off every LUT and switch setting is gone.")
  (wrong "Only the LUTs are lost; the routing stays"
    "The routing's switches are set by bits in the same configuration memory. All of it is SRAM, and all of it is lost.")
  (hint "What happens to SRAM without power?"))
```

---

```check
(choice
  (ask "A design needs 64 KB of fast memory on the chip for a screen's pixels. What should it use?")
  (right "Block RAM")
  (wrong "LUTs and flip-flops"
    "Memory made of LUTs and flip-flops would use them up very quickly. Block RAM is there for exactly this.")
  (wrong "DSP blocks"
    "DSP blocks multiply; they do not store data. Fast memory on the chip is block RAM.")
  (hint "Which part is memory built into the chip?"))
```

So: besides its LUTs and flip-flops, an FPGA has carry chains, routing,
block RAM, DSP blocks and I/O blocks, all set by its configuration
memory.
