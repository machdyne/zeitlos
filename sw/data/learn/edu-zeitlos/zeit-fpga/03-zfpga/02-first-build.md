# Your first build

```lesson
(objective "Build the example blinky for your board with zfpga build, and know what it writes.")
(minutes 20)
(keywords "board profile (FPGA)")
```

zfpga must know which chip and which pins to build for. It reads them
from a **board profile**: a small file in `/data/zfpga/boards`, four
lines long. The Lakritz's, `lakritz.brd`:

```text
device  LFE5U-25F
package CABGA256
lpf     lakritz.lpf
pack    -c
```

That is the part, its package, the constraints file beside it, and the
options for packing (`-c`: compress). You name a profile with `-b`:
`lakritz`, `klinge`, `konfekt`, `minze`, `schoko` or `noir` for those
boards; `mozart_ml0`, `mozart_ml1` or `mozart_ml2` for a Mozart; and
`sergei_ml0`, `sergei_ml1` or `sergei_ml2` for a Sergei.

```sources
(source "docs/zfpga-formats.md, section 7; sw/apps/zfpga/boards/*.brd; sw/apps/zfpga/build.c, board_aliases")
```

```check
(choice
  (ask "What does a board profile tell zfpga?")
  (right "The FPGA part and package, where the pin constraints are, and how to pack")
  (wrong "Which Verilog file to build"
    "The design is what you give on the command. The profile is about the board: the same blink.v builds for any board with its profile.")
  (wrong "How fast the board's clock is"
    "zfpga has no timing analysis, so a profile says nothing of clocks. The clock is just a pin, CLK_48, named in the constraints file.")
  (hint "Read the four lines of lakritz.brd."))
```

---

## The command

```text
$ zfpga build /data/zfpga/examples/blink.v -b lakritz
```

Put your own board's name after `-b`. Two things to know:

- **Give whole paths**, starting with `/`. posix passes a program its
  words exactly as typed, so zfpga does not know which folder you are
  in: `blink.v` alone would be looked for at the top of the card.
- **Wait for it to finish**, until a line starting `zfpga: wrote`.
  Run one build at a time: posix's `&&` waits only for a program to
  start, not to end.

```sources
(source "docs/zfpga-formats.md, section 7; docs/zfpga-test.md, section 2")
```

```check
(choice
  (ask "You are in /data/zfpga/examples and type: zfpga build blink.v -b lakritz. What happens?")
  (right "zfpga looks for /blink.v, at the top of the card, and does not find it")
  (wrong "It builds the blink.v in the folder you are in"
    "posix hands zfpga the words as typed. zfpga does not know your folder, so blink.v means /blink.v.")
  (hint "Does zfpga see which folder you are in?"))
```

---

```try
(try (app "term")
  (task "In term, at posix's $ prompt, run: zfpga build /data/zfpga/examples/blink.v -b NAME, with your board's name for NAME. It takes about a quarter of a minute. Wait for the line starting zfpga: wrote, then come back.")
  (check (number
    (ask "On the line that starts zfpga: synthesised, how many flip-flops are there?")
    (answer 24)
    (wrong-if 12 "12 is the number of carry cells, at the end of that line. The flip-flops come before.")
    (hint "The counter in blink.v has 24 bits."))))
```

---

## What it printed

zfpga says what it is doing as it goes, and how long each stage took:

```text
zfpga: board lakritz: LFE5U-25F, CABGA256, pins from /data/zfpga/boards/lakritz.lpf
zfpga: loading the chip database...
zfpga: synthesised 0 LUTs, 24 flip-flops, 1 carry chains (12 cells)
zfpga: placed 0 LUTs, 12 carry cells in 1 chains and 24 flip-flops in 13 slices; ...
zfpga: routed 49 nets: 99 arcs, 2 iterations, 51 reroutes, 7687 wires searched
zfpga: wrote /data/zfpga/examples/blink.bit in 13.1 s (peak memory 3152 KB)
```

On a Lakritz the whole build takes about 15 seconds, a few of them to
read the database from the card.

```sources
(source "docs/zfpga-test.md, section 2 (the times are illustrative)")
```

```check
(number
  (ask "The build peaked at 3,152 KB of memory. zfpga is given 4,096 KB. How many KB were left over?")
  (answer 944) (from "4096 - 3152")
  (hint "Subtract 3,152 from 4,096."))
```

---

## What it wrote

zfpga leaves each stage's output beside the input, with the same name:

```text
blink.zl    blink.zn    blink.cfg    blink.bit
```

`blink.bit` is the bitstream: about 99 KB for a 25F, about 160 KB for
a 45F, since every frame of the bigger chip costs a few bytes however
empty it is. The others are text, which the next lessons open.

Names on the card must be short for zfpga: at most 8 characters, a
dot and 3. `zfpga build` refuses `my_blinky.v` before it starts, and
takes `myblinky.v`.

```sources
(source "docs/zfpga-formats.md, section 7; docs/zfpga.md, section 22; sw/apps/zfpga/build.c")
```

```check
(choice
  (ask "Which name can you give a design that zfpga build will take?")
  (right "slow.v")
  (wrong "slowblinky.v"
    "slowblinky has 10 characters before the dot. zfpga build wants at most 8.")
  (wrong "blink.verilog"
    "The part after the dot may have 3 characters at most, and zfpga needs .v to know the file is Verilog.")
  (hint "8 characters, a dot, then at most 3."))
```

---

## A default board

To leave out `-b`, write your board's name once into
`/data/zfpga/board`:

```text
$ echo lakritz > /data/zfpga/board
$ zfpga build /data/zfpga/examples/blink.v
```

```check
(choice
  (ask "You wrote mozart_ml2 into /data/zfpga/board. You then type zfpga build /data/zfpga/examples/blink.v -b lakritz. Which board does it build for?")
  (right "Lakritz: -b, when given, wins")
  (wrong "Mozart ML2: the file sets the board"
    "The file is used only when there is no -b. Here -b names the Lakritz.")
  (hint "The file is the default: used when nothing else is said."))
```

So: zfpga build takes a whole path and a board, and writes a .zl, .zn,
.cfg and .bit beside the design.
