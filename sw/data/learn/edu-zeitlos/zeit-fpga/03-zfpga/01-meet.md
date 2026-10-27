# Meet zfpga

```lesson
(objective "Start the posix shell in a terminal, and use zfpga to ask about the chip database.")
(minutes 15)
(keywords "posix (shell)" "subcommand (FPGA)")
```

zfpga is one program with several **subcommands**: the word after
`zfpga` says which job to do.

| Subcommand | Does |
|---|---|
| build | every stage below, from a board's settings |
| synth, place, pnr, pack | the four stages, one at a time |
| unpack | a bitstream back to text |
| bram | new block RAM contents in a finished design |
| info | what the chip database knows |
| flash, run | put a bitstream into this machine's flash |

It is experimental. It needs an ECP5 board with at least 32 MB of
memory: it asks for 4 MB of its own while it works.

```sources
(source "sw/apps/zfpga/README.md; sw/apps/zfpga/main.c, usage(); docs/zfpga.md, sections 2.6 and 20.1")
```

```check
(choice
  (ask "Which subcommand turns a Verilog design into a bitstream in one go?")
  (right "zfpga build")
  (wrong "zfpga pack"
    "pack is only the last stage: it turns a finished, routed design into a bitstream. build runs every stage.")
  (wrong "zfpga synth"
    "synth is only the first stage: Verilog to a netlist.")
  (hint "Which one runs all the stages?"))
```

---

## The posix shell

zfpga has no window. You run it from **posix**, a Unix-style shell
that runs in a terminal window:

1. Open `term` from the dock.
2. Press **POSIX** on its panel (or Tab to it and press Enter).
3. A `$` prompt appears. Type a command and press Enter.

posix needs about 4 MB as well, and each must find its 4 MB free in one
piece; so start posix early, before other large programs. F12 in `term`
leaves the shell and goes back to the panel.

```sources
(source "docs/terminal.md, Starting out: the panel; docs/posix.md, Getting there; docs/zfpga-test.md, section 2")
```

```try
(try (app "term")
  (task "Open term from the dock, press POSIX, and at the $ prompt type: zfpga info LFE5U-25F (then Enter). It takes a few seconds to read the database. Come back here.")
  (check (number
    (ask "How many tiles does it say the 25F has?")
    (answer 4312)
    (wrong-if 134 "134 is the number of tile types. The line says how many tiles there are of those types.")
    (hint "Look at the line that starts with tiles.")
    (hint "It reads: tiles ... of 134 types."))))
```

---

## What it told you

`zfpga info LFE5U-25F` shows the database for the 25F:

```text
device    LFE5U-25F (IDCODE 0x41111043)
frames    7562 x 592 bits = 559588 bytes of CRAM
tiles     4312 of 134 types
serves    LFE5U-25F LFE5U-12F
```

The configuration memory is 559,588 bytes, which you met in the first
unit. The 4,312 tiles are of only 134 kinds, and the database
describes each kind once. That is why it fits in 1.5 MB, small enough
to read on this machine.

```sources
(source "zfpga info LFE5U-25F; docs/zfpga.md, section 2.4")
```

```check
(choice
  (ask "Why does the database describe 134 kinds of tile, not 4,312 tiles?")
  (right "Tiles of one kind are alike, so one description serves them all, and the database stays small")
  (wrong "Only 134 of the tiles can be configured"
    "Every tile can be configured. Tiles of the same kind have the same bits with the same meanings, so one description covers all of them.")
  (hint "Think of the 3,036 logic tiles. Are they different from each other?"))
```

---

## Where zfpga keeps its files

On the card, zfpga looks in `/data/zfpga`:

```text
/data/zfpga/lfe5u25f.zdb       the 25F's database (12F too)
/data/zfpga/lfe5u45f.zdb       the 45F's database
/data/zfpga/boards/            a profile and pins for each board
/data/zfpga/examples/          blink.v and other designs to build
```

Type `ls /data/zfpga/examples` at the `$` prompt to see the examples.

```sources
(source "docs/zfpga-formats.md, section 7; release/lib/mkfatimg.py, FPGA_FILES and FPGA_EXAMPLES")
```

```check
(text
  (ask "What command lists the files in zfpga's examples folder?")
  (accept "ls /data/zfpga/examples") (accept "ls /data/zfpga/examples/")
  (wrong-if "ls examples" "posix looks for examples in the folder you are in. Give the whole path, from /data.")
  (hint "ls, then the folder's whole path.")
  (hint "The examples are in /data/zfpga/examples."))
```

So: zfpga is one program with subcommands, run from the posix shell,
and its databases, boards and examples are in /data/zfpga.
