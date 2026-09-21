# zfpga file formats

Every file `zfpga` reads or writes, what it is for, and how to edit it
by hand. The design notes are in `docs/zfpga.md`; this is the reference.

---

## 1. The pipeline, and which file to edit

```
design.v  --synth-->  design.zl  --place-->  design.zn  --pnr-->  design.cfg  --pack-->  design.bit
                       (logical     (physical            (tile     <--unpack--  (binary)
                        netlist)     netlist)             settings)
```

| File | What it is | Edit it to | Text? |
|---|---|---|---|
| `.zl` | logical netlist: cells on named nets, optionally pinned | **write a design by hand; place cells by hand** | yes |
| `.zn` | physical netlist: cells at sites, nets by pin wire, or explicit routing | route by hand; experiment below the placer | yes |
| `.config` | Trellis tile settings: arcs, words, enums | change single configuration bits | yes |
| `.bit` | the bitstream | -- unpack it to a `.config`, edit that, pack again (section 6) | no |
| `.zdb` | the chip database | -- (regenerate with `tools/mkzdb.py`) | no |

The `.v` is ordinary Verilog in the subset of `docs/zfpga.md` §19.1,
typed in the editor like any other file:

```
$ zfpga synth blink.v -l /fpga/lakritz.lpf
zfpga: synthesised 0 LUTs, 24 flip-flops, 1 carry chains (12 cells)
```

Or every stage at once, from a board profile (section 7):

```
$ zfpga build /fpga/examples/blink.v -b lakritz
```

The `.zl` it writes keeps the Verilog's names on its nets, so it is the
place to look at what a design became, and to pin cells with `loc=`
before placing (section 3).

**For almost anything done by hand, edit the `.zl`.** It is the level at
which names mean something -- `inc3`, `q0`, `clk` -- and every stage
below it is generated. The `.zn` names sites and wires, and moving a cell
there means rewriting every net terminal that mentions it; to move a cell,
give it `loc=` in the `.zl` instead (section 3).

All the text formats share these rules:

- one statement per line, words separated by spaces or tabs;
- `#` begins a comment where a word would begin;
- options are `key=value`, in any order, **with no spaces inside the
  value** -- `func=a^(b&c)`, not `func=a ^ (b & c)`;
- an error names the file and line, and nothing is written.

---

## 2. `.zl` -- the logical netlist

```
device LFE5U-25F
package CABGA256

input  clk A7 net=clk
output led A2 net=q3

lut inc1 func=a^b a=q1 b=q0 z=d1
ff  r1   d=d1 q=q1 clk=clk loc=R2C4.A1
```

### 2.1 Statements

| Statement | |
|---|---|
| `device NAME` | first; `LFE5U-25F` or `LFE5U-12F` today |
| `package NAME` | before any IO; e.g. `CABGA256` |
| `input NAME PIN net=N [options]` | an input pad on package pin PIN |
| `output NAME PIN net=N [options]` | an output pad |
| `lut NAME ...` | a 4-input LUT |
| `ff NAME ...` | a D flip-flop |
| `ccu2 NAME ...` | a carry cell: two LUT halves and a carry, chained |

Names of cells and nets are any word without spaces. A net is created by
being mentioned; it must have exactly one driver (a pad, a LUT's `z`, a
flip-flop's `q`, a carry cell's `s0`/`s1`/`cout`).

**IO options:** `io_type=` (default `LVCMOS33`; also `LVCMOS25`,
`LVCMOS18`, `LVCMOS15`, `LVCMOS12`, `LVTTL33`), `pullmode=UP|DOWN|NONE`,
`hysteresis=ON|OFF`, `slewrate=FAST|SLOW`, `drive=4|8|12|16` (3.3V only),
`clamp=`, `opendrain=ON`. Outputs in one bank must share a voltage.

### 2.2 `lut`

```
lut NAME (func=EXPR | init=0xHHHH) [a=N] [b=N] [c=N] [d=N] z=N [loc=...]
```

`a`-`d` are the inputs; each may be a net, `0` or `1`, and an input not
given is `0`. **`func=`** is the comfortable way:

| | |
|---|---|
| variables | `a` `b` `c` `d` |
| constants | `0` `1` |
| not | `~a` or `!a` |
| and, xor, or | `&` binds tighter than `^`, which binds tighter than `\|` |
| grouping | `( )` |

So `func=a^(b&c&d)` is "a, toggled when b, c and d are all 1". A `func`
that uses an input you did not connect is refused.

**`init=`** is the 16-bit truth table, for when you have one: bit *i* is
the output when `a` is bit 0 of *i*, `b` bit 1, `c` bit 2, `d` bit 3. So
`func=a` is `init=0xAAAA`, `func=a&b` is `0x8888`, `func=a^b` is `0x6666`.

### 2.3 `ff`

```
ff NAME d=N q=N clk=N [ce=N] [lsr=N] [option=value ...] [loc=...]
```

| Option | Default | |
|---|---|---|
| `ce=` | `1` (always enabled) | a clock-enable net |
| `lsr=` | `0` (no reset) | a set/reset net |
| `regset=` | `RESET` | `SET`: reset (and power-on) value is 1 |
| `srmode=` | `LSR_OVER_CE` | `ASYNC` for an asynchronous reset |
| `gsr=` | `ENABLED` | global set/reset at configuration |
| `lsrmux=` | `LSR` | `INV`: reset is active low |
| `cemux=` `clkmux=` `lsrmode=` | | as yosys's TRELLIS_FF; `clkmux` must be `CLK` |

Two flip-flops can share a slice only if they share clock, enable, reset
and these settings; `zfpga place` pairs them itself, and refuses a `loc=`
that would force an unshareable pair together.

### 2.4 `ccu2`

```
ccu2 NAME init0=0xHHHH init1=0xHHHH [inject0=YES|NO] [inject1=YES|NO]
     a0= b0= c0= d0= a1= b1= c1= d1= cin=N cout=N s0=N s1=N [loc=RrCc]
```

yosys's `CCU2C`: two LUT halves (`s0`, `s1`) with a carry through them.
Cells whose `cout` is another's `cin` form a **chain**, placed as one
block running east from slice A. Hand-writing carry INITs is rarely worth
it; these usually come from synthesis. `loc=` on a chain goes on its
**first** cell and names a tile, where the chain's feed-in takes slice A.

### 2.5 `loc=` -- placing by hand

| Form | Means |
|---|---|
| `loc=R2C4` | somewhere in logic tile R2C4 |
| `loc=R2C4.B` | slice B of R2C4 |
| `loc=R2C4.B1` | slice B, half 1 |

A cell with `loc=` is placed exactly there and never moved. A LUT and the
flip-flop it feeds share a logic cell, so give them the same `loc=` or
give it to one of them. Only logic (PLC2) tiles take cells; to see what
is at a location:

```
$ zfpga info LFE5U-25F R2C4
R2C4:PLC2                  logic: loc=R2C4.A0 .. R2C4.D1
TAP_R2C4:TAP_DRIVE
$ zfpga info LFE5U-25F R0C4
MIB_R0C4:PIOT0
TAP_R0C4:DUMMY_TILE_A
PIOA at R0C4                bank 0
PIOB at R0C4                bank 0
```

---

## 3. Placing by hand: the workflow

1. Write the design as a `.zl`, or get one from synthesis.
2. Let the placer do it, and ask for the placement back:

   ```
   zfpga place design.zl -l placed.zl
   ```

   `placed.zl` is your file again, with `loc=` on every cell saying
   where it went. Every line is kept, comment lines included; a comment
   at the *end* of a cell line is not, since the cell line is rewritten.
3. Edit `placed.zl` -- move cells, or delete the `loc=` from the ones you
   want the placer to choose again.
4. `zfpga place placed.zl`. Cells with `loc=` stay put; the rest are
   placed around them. With every cell pinned, the placement is exactly
   what the file says: `tests/run.sh` places the dense design, writes it
   back and re-places it, and checks the result is identical.
5. `zfpga pnr placed.zn` and `zfpga pack placed.config`.

`examples/hand.zl` is a complete hand-written design: a counter on the
LED, `func=` LUTs, two flip-flops pinned and two left to the placer.

---

## 4. `.zn` -- the physical netlist

What `zfpga place` writes and `zfpga pnr` reads. Edit it to route by hand
or to experiment beneath the placer.

```
device LFE5U-25F
package CABGA256
comb R2C4 SLICED 0 init=0xFFFF pins=-
io   A2 dir=OUTPUT type=LVCMOS33
arc  R2C4:PLC2 F6 F6_SLICE
net  q1 R2C4/Q1_SLICE R2C4/B0 R2C4/A1
ff   R2C4 SLICEA 1 clk=?@clk lsr=- sd=1
```

| Statement | |
|---|---|
| `comb TILE SLICEx H [mode= init= pins= inject=]` | a LUT or carry half: INIT in **physical** pin order, `pins=` the inputs actually wired (others are tied to 1) |
| `ff TILE SLICEx H clk=W@NET lsr=W@NET\|- sd=0\|1 ...` | a flip-flop; `W` is the tile wire (`CLK0`/`CLK1`, `LSR0`/`LSR1`), or `?` to take it from routing; `sd=1` takes data from its own LUT |
| `io PIN\|RrCc.L dir=INPUT\|OUTPUT\|BIDIR type= [tristate] ...` | a pad |
| `dcc TILE NAME` | a global clock buffer (no enable) |
| `arc TILE SINK SOURCE` | one routing connection, Trellis tile-relative names |
| `net NAME SOURCE SINK...` | a net for the router, terminals as `R<r>C<c>/<wire>` |
| `sinks NAME SINK...` | more sinks for a declared net |

A `.zn` gives `arc`s (routed by hand; `examples/on.zn` is four of them,
each explained) or `net`s (routed by `zfpga pnr`), not both.

---

## 5. `.config` -- tile settings

Project Trellis's textual configuration, the format nextpnr writes and
ecppack reads; `zfpga pack` produces bitstreams byte-identical to
ecppack's from it. Reference: https://prjtrellis.readthedocs.io/,
"Textual Configuration Format".

```
.device LFE5U-25F
.tile R2C4:PLC2
arc: F6 F6_SLICE
word: SLICED.K0.INIT 1111111111111111
enum: SLICED.MODE LOGIC
```

Editing it changes configuration bits directly: a LUT's INIT, a mux
setting, an IO standard. There is no check here that the result is a
sensible circuit -- only that every name exists in the database.

---

## 6. The binary files

### 6.1 `.bit` -- read it as text

A bitstream -- from this machine, from a host, from any ECP5 tool -- is
read back to a `.config` with:

```
$ zfpga unpack design.bit
zfpga: to rebuild this bitstream exactly: zfpga pack design.config -c -f 2.4
```

`design.config` is the text `ecpunpack` would print for it, line for line
(`tests/run.sh` checks). Edit it, then run the command it printed.

That command matters. A bitstream carries things a `.config` cannot
say -- whether it was compressed, the SPI clock, a multiboot address,
the usercode, the SPI mode -- and packing the edited file without them
gives a different bitstream: one that may not boot from the address
the flash layout expects (`docs/zboot.md`). `zfpga unpack` reads those
settings from the bitstream and prints the `zfpga pack` options that
restore them; with the file unedited, that command rebuilds the input
byte for byte.

A bitstream is checked as it is read: a CRC mismatch, a truncated file,
a device other than an ECP5 zfpga knows, or an unknown command is
refused, and nothing is written.

A `.bit` in the hex editor shows the header strings at the start (the
`.comment` lines) and then configuration frames that mean nothing
without the database; edit the unpacked `.config` instead.

### 6.2 `.zdb`

The packed chip database, on the card in `/fpga`. Built by
`tools/mkzdb.py` from the vendored Trellis database; format in
`zfpga.h`. Not for editing: a hand-changed `.zdb` is refused as corrupt
more often than not, and should be.

---

## 7. Board profiles, and the card's `/fpga`

```
/fpga/lfe5u25f.zdb            the chip database (12F and 25F)
/fpga/boards/lakritz.brd      a board profile
/fpga/boards/lakritz.lpf      its pins: the tree's boards/lakritz_v0.lpf
/fpga/examples/               blink.v, on.zn, empty.zn, ... (docs/zfpga-test.md)
/fpga/board                   optional: one word, the default board
```

On the build machine, `sw/apps/zfpga/db/` is laid out exactly the same,
so `-D db` on the host is `/fpga` on the card.

A profile is four lines, `#` comments allowed:

```
device  LFE5U-25F
package CABGA256
lpf     lakritz.lpf          # beside the profile, or an absolute path
pack    -c                   # options for zfpga pack
```

`zfpga build IN -b NAME` reads `/fpga/boards/NAME.brd`; with no `-b`, the
name in `/fpga/board`. It runs every stage from the one the input's
extension names -- `.v` synthesises, `.zl` places, `.zn` routes,
`.cfg` only packs -- and leaves each stage's output beside the input,
so any of them can be looked at, edited, and built from again.

**Every name is 8.3.** Zeitlos's FatFs is built without long names, so a
file called anything longer than eight characters, a dot and three
cannot be opened on the card, however it looks on a PC that wrote it.
That is why the database is `lfe5u25f.zdb` and a Trellis configuration
here is `.cfg` rather than `.config`; `zfpga pnr` and `unpack` write
`.cfg` by default and every command still reads either. `zfpga build`
refuses, before doing any work, an input whose outputs would not fit.

**Paths to `zfpga` are absolute.** posix hands a program its arguments
as typed, not resolved against its working directory, so `blink.v`
means `/blink.v`.
