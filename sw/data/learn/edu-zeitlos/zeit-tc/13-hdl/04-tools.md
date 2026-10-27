# From description to bitstream

```lesson
(objective "Name the steps from a Verilog description to a working FPGA, and say what each step and report is for.")
(minutes 20)
(keywords "simulation" "testbench" "synthesis" "netlist" "place and route" "constraints file" "utilization" "known good bitstream")
```

You will follow a design from its text to a running circuit.

A value inside a chip cannot be seen directly, so hardware is hard to
debug once it is built. Designs are first tested by **simulation**: a
program works out what the circuit would do, cycle by cycle, and every
signal can be looked at.

A simulation is driven by a **testbench**: a Verilog module that is
not hardware but a script. It makes an instance of the design, gives it
inputs and checks its outputs. The heart of the full adder's testbench
tries all eight inputs and compares each result with ordinary addition:

```verilog
        for (i = 0; i < 8; i = i + 1) begin
            {a, b, cin} = i;
            #1;
            if ({cout, sum} !== a + b + cin)
                errors = errors + 1;
        end
```

`#1` waits one unit of time for the logic to settle. A testbench may
use such things, which cannot become hardware.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: Simulation")
```

```check
(choice
  (ask "Why is a design tested in simulation before it goes into an FPGA?")
  (right "In simulation every signal can be seen at every cycle; inside the chip it cannot")
  (wrong "Simulation runs faster than the FPGA"
    "Simulation is many thousands of times slower than the hardware. Its value is that every signal is visible.")
  (wrong "The FPGA cannot run a design that has not been simulated"
    "An FPGA runs any bitstream it is given. Simulation is for finding faults while every signal can still be seen.")
  (hint "What can you see in a simulation that you cannot see in a chip?"))
```

---

## Waveforms

A simulator can record every signal, to be shown as waveforms: one
line per signal, drawn high or low, against a common time axis, as the
counter was drawn in the lesson before. A fault is found by finding the
first moment a signal differs from what was meant, then following it
back to its cause.

A whole computer can be simulated this way, running a short program.
Most faults in a new processor are found so, before the design is ever
put in an FPGA.

```check
(choice
  (ask "A simulated design gives a wrong answer at cycle 900. Where do you start looking?")
  (right "At the first moment any signal differs from what you meant")
  (wrong "At cycle 900, where the wrong answer appears"
    "Cycle 900 is where the fault shows, not where it started. Find the first signal that goes wrong and follow it back.")
  (wrong "At the last signal in the list"
    "The order of the list means nothing. What matters is which signal goes wrong first.")
  (hint "A fault spreads from where it starts."))
```

---

## The steps

```text
  Verilog source          what the circuit does
       |
   synthesis              -> netlist: LUTs, flip-flops, RAMs, wires
       |
   place and route        -> each part a place on the chip,
       |                     each wire a path through the routing
   bitstream generation   -> the configuration bits
       |
   programming            -> into the FPGA or its flash
```

**Synthesis** reads the description, works out its logic, simplifies
it and maps it onto the FPGA's blocks. The result, a **netlist**, lists
every LUT and flip-flop and how they connect. **Place and route**
chooses the block on the chip for each part, then a path through the
routing for every connection. Then the bits are written, and loaded.

Open tools do each step: Yosys for synthesis, nextpnr for place and
route.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: From Description to Bitstream")
```

```check
(choice
  (ask "Which step decides which physical logic block on the chip each LUT of the design will use?")
  (right "Place and route")
  (wrong "Synthesis"
    "Synthesis decides what LUTs and flip-flops are needed and how they connect, the netlist. Where each one goes on the chip is decided next.")
  (wrong "Programming"
    "Programming only loads the finished bits. Every place was decided before.")
  (hint "Which step's name says it gives each part a place?"))
```

---

## The constraints file

The tools also need a **constraints file**: which signal goes to which
pin of the chip, what voltage standard each pin uses, and how fast each
clock runs. A board's constraints file is written once and used by
every design for that board.

These steps take seconds for a small design and many minutes for a
large one, and a large design needs far more memory and speed than a
small computer has. Tools that run on an FPGA computer itself can
build simple designs, but not one as large as the computer's own.

```check
(choice
  (ask "In simulation the blink design works, but on the board the LED stays dark. The constraints file gives led the wrong pin. Would simulation have found this?")
  (right "No: simulation does not know which pin is which")
  (wrong "Yes: simulation checks every signal"
    "It checks every signal of the design, but pins belong to the board. The constraints file is used only when building for the chip.")
  (wrong "Yes, if the testbench ran long enough"
    "No length of simulation shows a wrong pin, because the simulated design has no pins on a board at all.")
  (hint "Is the constraints file part of the simulated design?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: From Description to Bitstream; Debugging on the Board")
```

---

## Reading the reports

Two numbers matter on every build. **Utilization** is how much of the
chip the design uses: LUTs, block RAMs and the rest, out of those
there are. Above about three quarters full, designs become noticeably
harder to build. The **timing** report gives the fastest each clock can
run; if that is below the clock the design uses, it can fail when warm.

```check
(number
  (ask "A design uses 18,000 of an FPGA's 24,000 LUTs. What is its LUT utilization, in per cent?")
  (answer 75) (unit "%") (from "18000 / 24000 * 100")
  (wrong-if 25 "25% is the part still free. Utilization is the part used.")
  (hint "Divide the LUTs used by the LUTs there are, then multiply by 100."))
```

---

## Debugging on the board

Signals inside an FPGA cannot be probed, so a design is given ways to
show what it does: **LEDs** driven by inner signals; **spare pins** for
an instrument that draws signals over time, such as a logic analyser; **a serial port** that reports
values as text, often the first sign of life; a small **logic analyser
inside the design**, which records chosen signals into block RAM; and
**JTAG**, a standard debug connection.

Keep a **known good bitstream**, one you know works, change one thing at a time, and test each
new part alone before joining it to the rest.

```check
(choice
  (ask "You are adding three new parts to a working design. What is the safer way?")
  (right "Add and test them one at a time, keeping the working bitstream")
  (wrong "Add all three, then test the whole design once"
    "If it fails, any of the three, or how they meet, could be the cause. One at a time, a failure points to the part just added.")
  (wrong "Replace the working bitstream first, to save space"
    "The known good bitstream is what you go back to when a change fails. Keep it.")
  (hint "When something breaks, how do you know which change did it?"))
```

So: a design is simulated with a testbench, then synthesized, placed
and routed, turned into a bitstream and loaded, with its reports read
at every build.
