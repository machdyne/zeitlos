# Below the Verilog

```lesson
(objective "Read a netlist written by hand, place parts of it by hand, and turn a bitstream back into text.")
(minutes 20)
(keywords "loc (placement)")
```

You do not need Verilog to make a design. `/data/zfpga/examples/hand.zl`
is a 4-bit counter for the Lakritz, written straight as a logical
netlist:

```text
input  clk A7 net=clk
output led A2 net=q3

lut inc0 func=~a          a=q0                     z=d0
lut inc1 func=a^b         a=q1 b=q0                z=d1
lut inc2 func=a^(b&c)     a=q2 b=q1 c=q0           z=d2
lut inc3 func=a^(b&c&d)   a=q3 b=q2 c=q1 d=q0      z=d3

ff r0 d=d0 q=q0 clk=clk loc=R2C4.A0
ff r1 d=d1 q=q1 clk=clk loc=R2C4.A1
ff r2 d=d2 q=q2 clk=clk
ff r3 d=d3 q=q3 clk=clk
```

Each bit toggles when every bit below it is 1: that is counting. Bit
3, toggled when bits 2, 1 and 0 are all 1, is `a^(b&c&d)`.

```sources
(source "sw/apps/zfpga/examples/hand.zl; docs/zfpga-formats.md, section 2")
```

```check
(choice
  (ask "q1 and q0 are both 1. What happens to q2 at the next clock edge?")
  (right "It changes: 0 becomes 1, or 1 becomes 0")
  (wrong "It stays as it is"
    "inc2 is a^(b&c): a is q2, and b&c is q1 AND q0, which is 1. So q2 is toggled.")
  (wrong "It becomes 1, whatever it was"
    "XOR with 1 flips a bit. If q2 was 1, it becomes 0, as a counter's bit does when the carry reaches it.")
  (hint "XOR with 1 flips; XOR with 0 keeps."))
```

---

## Placing by hand

`r0` and `r1` carry **loc=**: they are placed exactly there, in tile
R2C4, slice A, halves 0 and 1. `r2` and `r3` have none, so the placer
chooses. To see what is at a place on the chip:

```text
$ zfpga info LFE5U-25F R2C4
R2C4:PLC2                  logic: loc=R2C4.A0 .. R2C4.D1
```

PLC2 is the logic tile; only those take LUTs and flip-flops. And
`zfpga place hand.zl -l placed.zl` writes your file back with a `loc=`
on every part, saying where each went, for you to edit and place again.

```sources
(source "docs/zfpga-formats.md, sections 2.5 and 3")
```

```check
(text
  (ask "Write the loc= that puts a flip-flop in tile R5C10, slice C, half 1.")
  (accept "loc=R5C10.C1") (accept "R5C10.C1")
  (wrong-if "loc=R5C10.C" "That is slice C, but either half. Add the half: 1.")
  (hint "The form is loc=RrowCcolumn.Slice and half.")
  (hint "loc=R5C10.C..."))
```

---

## A bitstream back to text

A bitstream means nothing without the chip database. `zfpga unpack`
reads one and writes the tile settings as text, a `.cfg`:

```text
$ zfpga unpack /home/slow.bit
zfpga: to rebuild this bitstream exactly: zfpga pack /home/slow.cfg -c
```

The `.cfg` is a list of tiles and their settings: `arc:` lines for
the wiring, `word:` lines such as a LUT's INIT, and `enum:` lines for
choices such as a pin's voltage. It works on any ECP5 bitstream, from
this machine or from another computer.

```sources
(source "docs/zfpga-formats.md, sections 5 and 6.1")
```

```check
(choice
  (ask "Why does unpack print a pack command?")
  (right "A bitstream holds settings the .cfg cannot, such as compression and a boot address; the command puts them back")
  (wrong "To show the command that made the file in the first place"
    "It does not know what command was typed. It reads the settings from the bitstream itself and prints the options that restore them.")
  (wrong "Because the .cfg is damaged until it is packed again"
    "The .cfg is complete text of the tile settings. What it lacks is the packing options, which the command restores.")
  (hint "Packed without those options, the edited bitstream might not boot from the same address."))
```

---

```check
(text
  (ask "In a .cfg, a LUT is set with word: SLICEA.K0.INIT and 16 bits. What hex INIT does func=a^b give?")
  (accept "0x6666") (accept "6666")
  (wrong-if "0x8888" "That is a AND b. XOR is 1 at addresses 1 and 2.")
  (hint "You met this in the first unit: addresses 1 and 2 give 1."))
```

So: a .zl can be written and placed by hand with func= and loc=, and
zfpga unpack turns any ECP5 bitstream back into readable settings.
