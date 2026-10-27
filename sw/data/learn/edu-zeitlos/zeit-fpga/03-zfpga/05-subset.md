# What zfpga accepts

```lesson
(objective "Tell whether zfpga will accept a piece of Verilog, and read the message when it refuses.")
(minutes 20)
(keywords "latch (Verilog)")
```

zfpga reads Verilog-2001, as the Zeitlos gateware is written. Among
what it takes:

- several modules and files, instances with parameters, `` `define``,
  `` `ifdef`` and `` `include``;
- `wire` and `reg` with initial values, vectors, small memories (up to
  4,096 bits, built from flip-flops);
- `always @(posedge clk)`, with or without a reset, and `always @(*)`;
- `if`, `case`, `for` loops with fixed bounds, and functions;
- the operators, including `+`, `-` and `*`, comparisons and shifts.

```sources
(source "docs/zfpga-formats.md, section 8.1")
```

```check
(choice
  (ask "Which of these will zfpga take?")
  (right "A 16-word memory of 8-bit values, reg [7:0] m [0:15]")
  (wrong "A 2,048-word memory of 8-bit values"
    "That is 16,384 bits. zfpga builds memories from flip-flops, up to 4,096 bits, and does not use block RAM yet.")
  (hint "Multiply words by bits, and compare with 4,096."))
```

---

## What it refuses

zfpga refuses what it cannot build exactly, naming the file and
line, and writes nothing:

- memories big enough to need block RAM;
- `generate`, tristate pins (`1'bz`, `inout`), `x` in numbers;
- `/` and `%` on values that are not constants, and `**`;
- `initial` blocks, so no `$readmemh`; tasks;
- **latches** (memory made by accident, below), combinational loops,
  and two drivers on one signal.

Refusing is on purpose. A wrong bitstream does not crash: it programs
the FPGA into a circuit nobody designed, on pins that may also drive
the board's memory. A tool that refuses is safer than one that
guesses.

```sources
(source "docs/zfpga-formats.md, section 8.2; docs/zfpga.md, section 4.2")
```

```check
(choice
  (ask "Why does zfpga refuse a design instead of building its best guess?")
  (right "A wrong bitstream still loads, and becomes a circuit nobody intended")
  (wrong "Because guessing would make the build slower"
    "Speed is not the reason. The danger is a bitstream that loads and quietly does the wrong thing, on pins wired to real chips.")
  (wrong "Because the bitstream format forbids it"
    "Any bits can be packed. That is exactly the problem: nothing after zfpga would catch the mistake.")
  (hint "What happens when a wrong bitstream is loaded?"))
```

---

## A latch

In an `always @(*)` block, every signal it sets must be set on every
path. Here `led` is set only for two values of `step`:

```verilog
always @(*) begin
    case (step)
        3'd0: led = 1'b1;
        3'd2: led = 1'b1;
    endcase
end
```

For the other values the circuit would have to remember `led`'s old
value: a **latch**, which is almost never meant. zfpga says:

```text
beat.v:14: error: 'led' is not assigned on every path through always @(*): that is a latch
```

A `default: led = 1'b0;` line in the `case` gives `led` a value on
every path, and the design builds.

```sources
(source "the message zfpga synth gives; Timeless Computing (docs/tc.md), Describing Hardware: Classic Mistakes")
```

```check
(choice
  (ask "Which change stops the latch?")
  (right "Add default: led = 1'b0; to the case")
  (wrong "Change = to <= in the case"
    "<= in an always @(*) block is refused too. The trouble is the paths where led is not set at all.")
  (wrong "Make step wider"
    "More bits in step means more values with no led set. Every value must give led one.")
  (hint "Which values of step set led now?"))
```

---

## Two drivers

A signal can be driven from one place only:

```verilog
always @(posedge CLK_48) led <= ctr[23];
always @(posedge CLK_48) led <= ctr[22];
```

Two `always` blocks both set `led`, and zfpga refuses:

```text
two.v:6: error: 'led' has more than one driver
```

```check
(choice
  (ask "You want the LED from bit 23 when a switch is on and bit 22 when it is off. How do you write it?")
  (right "One always block, with if (sw) led <= ctr[23]; else led <= ctr[22];")
  (wrong "Two always blocks, one for each case"
    "That is two drivers for led, which zfpga refuses. Put both choices in one block.")
  (wrong "Two assign statements to LED_B"
    "Two assigns to one wire are two drivers as well.")
  (hint "One signal, one place that sets it."))
```

---

```check
(choice (several)
  (ask "Which of these will zfpga refuse? Choose every one.")
  (right "assign data = en ? out : 8'bz;")
  (right "An initial block that loads a memory from a file")
  (right "assign q = a / b; with a and b both inputs")
  (wrong "assign q = a * b; with a and b both inputs"
    "zfpga builds * from carry chains: it is on the list of operators it takes. / and % are what it refuses.")
  (hint "Look for tristate, initial blocks and division."))
```

So: zfpga takes the plain Verilog of the gateware, and refuses, by file
and line, anything it cannot build exactly.
