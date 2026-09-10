# Zeitlos Hardware Map

`tools/hwmap/hwmap` draws a map of the SoC -- buses, arbiters, masters,
slaves, the address map, clocks, interrupts and external interfaces --
straight from `rtl/sysctl.v`, and checks the RTL for define combinations
that would not work.

```
make hwmap
```

writes, into `output/docs/`:

| File | What |
|------|------|
| `hwmap.pdf` | A4 landscape. Page 1 is the map; the pages after it are reference tables and the check findings |
| `hwmap.png` | Page 1 at 200 dpi, for a wiki, a README or a chat |
| `hwmap.svg` | Page 1 as SVG |
| `hwmap.json` | The model hwmap built, with `--json` (see below) |

`output/` is ignored by git, so the map is a build artefact: regenerate it
whenever the RTL changes rather than committing a copy that will go stale.

## Why no board

The map is not the SoC of any one board. It shows **every optional
feature** of `rtl/sysctl.v`, each labelled with the define that includes
it.

That is the only map that stays true. A per-board map answers "what does
Obst build", which `rtl/boards.vh` already answers in a few lines; it
cannot show that slot 0x4 is SRAM *or* SDRAM *or* PSRAM, that the icache
can be removed from the CPU path, or that audio needs the main arbiter
to fetch its samples. Those are properties of the design, not of a board.

So hwmap never preprocesses. It follows `` `include "boards.vh" `` like
any other include, but never evaluates a define, never selects a board,
and has no `BOARD=` parameter. It reads each
`` `ifdef `` / `` `elsif `` / `` `else `` as a *condition* and attaches it
to everything inside: an instance, a decode, one term of a mux, one port
connection. `` `elsif B `` after `` `ifdef A `` is recorded as `B & !A`,
exactly as the preprocessor would behave, but for all define sets at
once.

## Reading the map

Page 1 has five regions:

- **Clocks** (top left) -- each clock net, its frequency, what generates it
  (with the defines that select that generator) and what uses it.
- **Interrupts** (below) -- CPU `irq[n]` and what drives each line.
- **Bus masters** (top centre) -- everything that can start a cycle on the
  main bus, traced back from the bus through arbiters, bridges and address
  stages to the masters.
- **Other buses and sideband blocks** (top right) -- buses that are not the
  decoded main bus (the VRAM bus behind its arbiter), and blocks with no bus
  interface at all (video out, the hardware cursor) with the signals that
  connect them.
- **The main bus** (middle, full width) -- one column per top address nibble.
  A slave sits in the column of its base address, so the block diagram and
  the memory map are the same picture.

A block's outline says whether it is there:

| Drawn as | Meaning |
|----------|---------|
| solid outline | always present |
| dashed outline with a **TAG** | optional: present when `TAG` is defined (nested blocks show only what they add to their parent's tag) |
| box with an inner outline, **one of** | alternatives from one `` `ifdef `` / `` `elsif `` / `` `else `` chain; the first matching tag wins, **else** is the fallback |
| dashed box, "absent: wired straight through" | *bypassable*: when absent, its `` `else `` branch wires its input straight to its output (the icache, both arbiters) |
| dotted, grey | a register window answered **off** the bus, e.g. the icache's `CFG_BASE` inside the CSR window, or the MTU's virtual window |

Colour is only the category (CPU, memory, video, ...). Presence is always
in the outline and the tag, so a monochrome print loses nothing.

A tag can hold a combination, such as `GPU_VGA & !GPU_COMPOSITE`. `!X`
means "when X is **not** defined".

### Reference pages

- **Address map** -- every decoded window with base, size or mask, the
  slave(s) that answer it, the condition, and the line in `sysctl.v`.
- **Interrupts**, **Clocks**, **External interfaces** (top-level ports
  grouped by the block they belong to) and **Blocks** (every instance, its
  role and its doc).
- **Feature index** -- every define `sysctl.v` depends on and what it
  controls: blocks, decodes, pins, instance parameters, localparams, bits of
  `CSR_FEATURES`, and code inside submodules. Value defines show the default
  `sysctl.v` or `csrs.vh` supplies. Uses listed under "without" are present
  when the define is *absent*.
- **hwmap check** -- see below.

## Checks

A build exercises one combination of defines, so something broken in a
different combination compiles, boots and ships. Because hwmap knows the
condition of everything, it can check all combinations. Each finding names
the combination it applies to:

| Kind | Meaning |
|------|---------|
| `no-ack` | a window is decoded but no slave acks it there -- the CPU waits forever |
| `overlap` | two decodes claim the same address at the same time |
| `drivers` | a net has two drivers at the same time |
| `no-mux` | a decode that no data/ack mux uses at all |
| `undeclared` | an identifier used where its declaration does not exist (a compile error, or an implicit 1-bit net) |
| `unconnected` | a bus initiator whose address goes nowhere |
| `undriven` | an instance input connected to a net that nothing drives |
| `no-clock` | a clock net with no source |
| `exclusive` | an `` `elsif `` chain used outside its guard, which makes its defines one-of |

Findings with the same combination are grouped, because one missing guard
usually shows up as several symptoms. For example, `MONTMUL` without `TRNG`
is reported as the montmul window being decoded but never acked, *and* as
`wbs_montmul_dat_o` being declared only under `TRNG`. Both come from the
mux terms being nested inside `` `ifdef TRNG ``.

hwmap does not know which combinations boards use, because it never
evaluates the board blocks in `boards.vh`. Most findings are about combinations no board uses today, which
makes them traps for the next board or the next feature rather than bugs in
a shipping image. Some imply a requirement worth writing down (`ESP32_LINK`
needs `UART1`); some are just the RTL saying "not supported" (no FPGA family
defined).

```
tools/hwmap/hwmap --check            print the findings, write nothing
tools/hwmap/hwmap -v                 generate, and list the findings
tools/hwmap/hwmap --strict           exit 1 if any error combination exists
```

## What the RTL has to look like

hwmap has no list of Zeitlos peripherals. It recognises structure by the
conventions `rtl/sysctl.v` already follows, so a new block that follows
them appears on the map without touching hwmap. If you break one, the
block is still found but may be drawn in the wrong place, and hwmap says
so.

**Files.** The file list is the Makefile's `RTL_PICO`, the same list
synthesis uses. (`rtl/mem/sdram.v` and `rtl/mem/sdram_kianv.v` both define
`sdram_wb`; only the second is built, and only `RTL_PICO` knows that.)
The top module is `sysctl`.

**Preprocessor.** `` `ifdef ``, `` `ifndef ``, `` `elsif ``, `` `else `` and
`` `endif `` in any position, including inside port lists, parameter lists
and expressions. `` `ifndef X `` directly followed by `` `define X `` is an
include guard or a default value and adds no condition; a default's value
is shown in the feature index. `` `include `` is followed, and a missing
include inside a condition (`zspec.vh` under `ZSPEC`) is fine.

**Wishbone interfaces** are found by port names on the *submodule*: a
prefix `p` with `p` + `cyc_i` and `p` + `ack_o` is a **target**; with
`p` + `cyc_o` and `p` + `ack_i` it is an **initiator**. `wb_`, `m0_`,
`s_`, `cfg_`, `c_`, `mx_` and the empty prefix all work.

**Arbiters and bridges** are recognised by interface shape, not name: an
instance with an initiator interface and two or more target interfaces
whose addresses do not come from the decoded bus is an arbiter; with
exactly one it is a bridge (the icache).

**Address stages.** An instance whose non-Wishbone output feeds an address
(the MTU's `addr_out`) is followed back through its input ports whose names
contain `adr` or `addr`.

**Decodes** are wires or assigns of one of these forms, optionally ANDed
with `!other_decode` terms (which make the decode "the rest of the
window"):

```verilog
wire cs_x = ((wbm_adr & 32'hf000_0000) == 32'h4000_0000);
wire cs_x = (wbm_adr[31:13] == 19'd0);
```

The mask must cover the top nibble. The net they compare is the bus.

**Data/ack muxes** are OR chains of AND terms, with the replication
marking the data mux:

```verilog
assign wbm_dat_i = ({32{cs_x}} & wbs_x_dat_o) | ... ;
assign wbm_ack   = (cs_x & wbs_x_ack_o) | ... ;
```

A slave's place on the map comes from its **ack** term: hwmap traces the
ack signal back to the instance output that drives it. A slave implemented
inline (an always block rather than an instance, like the ESP32 control
register) is drawn under its decode name. Name it in `INLINE` in
`tools/hwmap/lib/hints.py`.

**Alternatives.** Instances with the same instance name under mutually
exclusive conditions (`wbm_cpu0_i`, `wbs_uart0_i`), several ack drivers of
one decode, or several decodes of the same window under exclusive
conditions (`cs_sram` / `cs_sdram` / `cs_qqspi`) become one "one of" box.

**Bypass.** An optional arbiter, bridge or stage whose absence leaves its
own input in its place -- the `` `else `` branch being pure wiring -- is
drawn as bypassable.

**Interrupts** are assignments to `<net>[N]` where `<net>` is connected to
an instance port named `irq`.

**Clocks** are nets connected to input ports whose name contains `clk`.
The frequency comes from the net or pin name: `clk25_2mhz` is 25.2 MHz,
`CLK_48` is 48 MHz.

**Off-bus windows.** An instance parameter whose name contains `BASE` and
whose value lies inside a decoded window, on a block that is not itself a
slave, is drawn as a dotted window there (the icache's `CFG_BASE`). A module
may declare a virtual window with the `window` hint, naming its address and
mask parameters; the values are read from the module's RTL.

**External interfaces.** A top-level port belongs to a block if it connects
to one of its ports directly, or through one assign (the tri-state pattern
GPIO uses). Anything further away is listed as board glue.

## Hints

`tools/hwmap/lib/hints.py` is **cosmetic only**: display names, colour
category, the doc to list for a module, instance parameters worth printing,
pin-group labels. It never decides what is drawn or how it is connected. A
module without an entry is drawn under its module name, and `hwmap` prints a
note naming it.

## Options

```
tools/hwmap/hwmap [--root DIR] [--out DIR] [--json] [--no-png] [--dpi N]
                  [--strict] [-v]
tools/hwmap/hwmap --check | --selftest | --diff | --update-golden
```

The tool uses only the Python standard library. The PNG comes from
`pdftoppm` (poppler-utils), or `rsvg-convert`, or ImageMagick, whichever
is installed; with none of them the PDF and SVG are still written. The
PDF uses the fonts built into every PDF viewer, embeds nothing, and is
byte-identical from run to run on the same RTL.

## Testing hwmap

```
tools/hwmap/hwmap --selftest
```

runs two kinds of test:

1. A small synthetic SoC with known answers, covering the condition algebra,
   the lexer, decode and mux recognition, alternatives, bypass folding, every
   check, and rendering. It does not use the real RTL, so it only fails when
   hwmap is broken.
2. The **golden model**, `tools/hwmap/golden/sysctl.model`: the complete
   model hwmap built from the RTL when it was recorded, plus a hash of that
   RTL. If the RTL is unchanged and the model differs, hwmap's analysis
   changed and the test fails with the difference. If the RTL has changed,
   the comparison is skipped.

After an RTL change, `hwmap --diff` shows what hwmap now believes is
different: a new slave, a moved window, a new finding. It is a quick way to
confirm a change did what you meant. `hwmap --update-golden` accepts it.
(The file is JSON but deliberately not named `.json`, which the top-level
`.gitignore` ignores.)

## Troubleshooting

**"not drawn on the map"** -- a block did not fit any region: it has a bus
interface but no decode, arbiter or bridge connects it. Usually a naming
convention above is broken, for example an ack signal driven through logic
hwmap cannot trace. It still appears in the Blocks table.

**A slave is missing from its column** -- check that its ack is in the ack
mux in the form above, and that the decode's mask covers the top nibble.

**"column(s) ... too full"** -- a column had more notes than fit; the lowest
notes of the biggest boxes were dropped from page 1. The reference pages
are complete.

**"cannot read the RTL"** -- a structural parse error with file and line,
usually an unbalanced `` `ifdef `` or bracket. hwmap refuses to guess.

**A finding looks wrong** -- run `--check` and read the lines it cites;
every finding names the exact combination and the lines involved. If the
RTL really is fine under that combination, it is a hwmap bug: a small test
case in `tools/hwmap/lib/selftest.py` is the best report.
