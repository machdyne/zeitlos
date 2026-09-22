# zfpga -- FPGA bitstreams built on the machine itself

Synthesis, place-and-route and bitstream packing, running on Zeitlos,
targeting the FPGA Zeitlos is running on.

**Status: experimental, and working on hardware.** On a Lakritz,
`zfpga build /fpga/examples/blink.v -b lakritz` synthesises, places,
routes and packs a Verilog blinky on the machine itself, and the
bitstream it writes, booted from an MMOD, blinks the LED (section 23).
No host computer is involved from Verilog to bitstream.

What exists: `zfpga synth` (Verilog-2001: modules and instances,
parameters, the preprocessor, `for`, memories, signed arithmetic,
functions -- 15 of this tree's own RTL modules come out equivalent to
yosys's synthesis, §24), `place`
(LUTs, flip-flops, carry chains, IO; hand placement with `loc=`), `pnr`
(negotiated-congestion routing over general fabric), `pack` (bitstreams
byte-identical to `ecppack`'s), `unpack` (text identical to
`ecpunpack`'s), `bram` (`ecpbram`'s job: new block RAM contents in a
finished design, §25), and `build`, which runs them all from a board
profile.
Every stage is checked on the host against ecppack, nextpnr or yosys,
and on the device build under `sim/` (`sw/apps/zfpga/tests/`). The
formats are in `docs/zfpga-formats.md`; the on-board test is
`docs/zfpga-test.md`; what is still open is section 23.3.

Sections 1-10 are the original proposal, kept as written except where a
correction is marked **[corrected]**; per the convention in
`docs/posix.md`, sections from 11 onward record what each phase actually
did, including where it departed from the plan above it.

---

## 1. What is actually being asked for

Blink an LED on a Lakritz, from Verilog typed into `vi` on the machine
itself, without another computer in the room. Then, ideally, more than
one LED.

The current chain, from `Makefile`:

```
yosys synth_ecp5            rtl/*.v        -> soc.json
nextpnr-ecp5 --textcfg      soc.json       -> soc.config
ecpbram -f seed -t bios     soc.config     -> soc_final.config
ecppack --compress          soc_final.config -> soc.bit
openFPGALoader                             -> flash
```

Four programs and something close to a million lines of C++, none of
which will ever run on a PicoRV32. But the chain is not what we need to
reproduce. The *artifact* is. And the artifact is a 582 KB file with a
documented format.

### Non-goals, named now

`docs/zcc.md` is 4,600 lines of compiler that produces code 4.5x the
size of GCC's, and says so in a table on its own front page. That is the
right posture for this too, and the non-goals are worth fixing in
writing before optimism sets in:

| | |
|---|---|
| **Competitive quality** | nextpnr is timing-driven, has a real analytical placer and a router that has been tuned for years. This will be worse on every axis. Designs that fit at 45 MHz under nextpnr may not fit at all here. |
| **Building the Zeitlos SOC** | This is the ambition-trap. The SOC is a hundred-odd Verilog files with parameters, `generate`, inferred memories, PLLs, DDR primitives and SERDES-adjacent IO, and it already fills most of a 25F. It is not a target and should not be one for a very long time. |
| **Timing** | No static timing analysis. No delay model. A design either works at the clock you gave it or it does not, and the tool will not tell you which. |
| **Every primitive** | LUT4, FF, carry, and IO buffers. That is the initial primitive set. EBR, DSP, PLL and everything else are later, individually, and each one is a decision rather than a given. |

What *is* the goal: a small design, written in the plain Verilog this
tree already uses, becomes a bitstream that programs and behaves.

### 1.1 What it is for

"Technically cool" is not a reason to spend a year, so this deserves a
straight answer before the plan.

**One fact sorts every application: gateware the user builds *replaces*
Zeitlos while it runs.** An ECP5 reconfigures whole. Partial
reconfiguration exists in silicon, but it needs instructions (`0x79`,
`0x74`) sent over JTAG or the SPI slave port before the partial data,
and neither port is reachable from the fabric -- the same wall that
makes PROGRAMN the only reboot (`docs/zboot.md` §1). So "load an
accelerator into the running system" is **not** on offer, and nothing
below pretends otherwise.

That leaves three kinds of application, and they arrive at very
different points in the plan:

**A. Changing Zeitlos's own gateware without place-and-route.**
Available once `zfpga pack` and `zfpga bram` exist (Phase 2) plus the boot
path (Phase 7). No synthesis, no router.

1. **A gateware launcher.** Keep prebuilt bitstreams on the card and
   boot one from Zeitlos: the LiteX gateware that Lakritz ships for
   Kakao Linux, a test image, anything self-contained or that loads the
   rest of itself from the SD card. Power-cycle to come back. This
   needs *only* the flash write and PROGRAMN from `docs/zboot.md` -- not
   even the packer -- and it is the most immediately useful thing in
   this whole document. A LiteX image packed with `--bootaddr 0`, as
   LiteX's own flow does by default, returns to address 0 on its next
   PROGRAMN as well.
2. **Updating the BIOS on the machine.** The BIOS lives in BRAM
   initialised by `$readmemh` in `rtl/mem/bram.v`, and the existing
   build already swaps it into a finished `.config` with `ecpbram` and a
   seed pattern rather than re-running place-and-route. `zfpga bram` does
   the same on the machine: new BIOS image in, repacked gateware out.
   The safe way to install it is **trial-boot, then commit** -- write
   it to the gateware slot, boot it once, and only if it comes up copy
   it over the working image. A bad BIOS then costs a power cycle, not
   a DFU recovery. (Rebuilding the BIOS *itself* on the machine is a
   separate question: it is freestanding C plus assembly, and `zcc` has
   no assembler.)
3. **An MMOD programming station.** The configuration flash of most of
   these boards is an MMOD, and `sw/apps/mmod` already writes one
   through the Pmod socket. Add a packer and a Zeitlos machine can
   prepare configuration modules for *other* boards in the field --
   repacked, retargeted to a different boot address, with a different
   BIOS -- with no PC. 12F and 25F share a die (§2.2), so a Lakritz
   holds the database for an Obst.

**B. Whole-FPGA appliances you boot into and power-cycle out of.**
Available from Phase 3 (hand-placed) and properly from Phase 6.

4. **Instruments and Pmod drivers at fabric speed.** Things software
   GPIO cannot do: a WS2812 strip driver, a bank of servo PWMs, a
   quadrature decoder, a frequency counter, a clock or pattern
   generator, a video test pattern on the HDMI pins, a UART sniffer.
   `sw/apps/logic` samples pins from software and `docs/gpio.md`
   measures bit-banged SPI at about 60 KB/s; the fabric runs at 48 MHz
   per pin. Twenty to fifty lines of Verilog each. This is the
   "microcontroller dev board" use, with the difference that the board
   is also the development machine.
5. **Bring-up and test images generated from a `.lpf`.** Every pin of
   a Pmod or a board toggling at its own distinct frequency, or looped
   back pairwise, so a probe or `logic` on a *second* machine
   identifies which pin is which and which is dead. Generated from the
   constraint file and a template, so it needs Phase 3-4 but not
   synthesis. A repair-bench tool, and a genuinely useful one for a
   company that makes these boards.

**C. Zeitlos plus a peripheral you wrote.** This is the application
that justifies the project, and it gets its own phase (Phase 9).

6. **The socket.** The host builds the SOC once with a rectangular
   region left empty (nextpnr supports region constraints) and a
   fixed, registered interface -- a small Wishbone slave -- whose
   boundary signals are pinned to known wires at the region's edge.
   `zfpga` then places and routes a small user design *inside the region
   only*, treats every wire the SOC's own `.config` already uses as
   occupied, merges the result into that `.config`, packs it, and boots
   it through the gateware slot. The machine comes back up as Zeitlos
   with a new device at a fixed address, driven by an ordinary app.

   Hardware PWM or a WS2812 driver that `settings` controls; a capture
   engine that samples at fabric speed into BRAM and that `logic` reads
   out; a CRC or hash block for `net`. The user writes the peripheral,
   never the SOC -- which is exactly the scale this tool is good at, and
   exactly the scale the non-goals in §1 allow.

   The honest caveats: a 25F is most of the way full with the SOC in it
   and the free region may be small (the 45F and 85F boards are the
   comfortable home for this); nextpnr's region constraint limits
   placement, not routing, which is why every SOC-used wire must be
   marked occupied (cheap -- we parse that config anyway); and with no
   timing analysis the interface must be registered on both sides and
   the user logic kept shallow.

7. **Teaching.** `docs/tc.md` is a book about computing *and FPGAs*,
   CC0, meant to ship with the system. A reader who can type ten lines
   of Verilog into `vi` on the machine they are reading about, and
   watch it become hardware on that machine, has the whole stack in
   front of them with nothing hidden in another computer. That, and the
   narrower point that small designs stay buildable when a host
   toolchain has bit-rotted, is the "timeless" argument -- real, but it
   only holds at the scale this tool actually handles.

**The ranking, stated plainly:** 1-3 need no synthesis and arrive
earliest; 1 needs almost none of this project at all. 6 is the reason
to build the router and the synthesiser. 4, 5 and 7 are what makes it
pleasant in between.

---

## 2. What the ECP5 permits

Numbers taken from Project Trellis's database and documentation and from
this tree, not assumed. The answer is considerably better than the
reputation of the problem suggests.

### 2.1 The configuration memory is 560 KB, and blank is all zeros

| Device | IDCODE | Frames | Bits/frame | CRAM | Bitstream, uncompressed |
|---|---|---|---|---|---|
| LFE5U-12F | 0x21111043 | 7562 | 592 | 559,588 | ~582 KB |
| LFE5U-25F | 0x41111043 | 7562 | 592 | 559,588 | ~582 KB |
| LFE5U-45F | 0x41112043 | 9470 | 846 | 1,001,452 | ~1,023 KB |
| LFE5U-85F | 0x41113043 | 13294 | 1136 | 1,887,748 | ~1,928 KB |

A Lakritz has 32 MB of SDRAM. The entire configuration memory of its
FPGA is **1.7% of that**, and it is a flat two-dimensional bit array
with no structure a program has to respect. There is no streaming, no
windowing and no cleverness required: allocate it, set bits in it, write
it out.

**[corrected]** This section originally said *the blank device is all
zeros*. **It is not.** libtrellis does start from a zeroed CRAM, but
`ChipConfig::to_chip()` then applies every tile's database defaults to
every tile, configured or not -- its own comment is "not always zero,
e.g. in IO tiles". Packing an empty 25F design (`.device LFE5U-25F` and
nothing else) sets **188 bits, in 68 of 7,562 frames**. `zfpga` does the
same, and would not match `ecppack` otherwise.

What *is* true is that the database is arranged so that the common
defaults cost nothing -- in `PLC2/bits.db`:

```
.config SLICEA.K0.INIT 1111111111111111
!F25B10
!F24B10
...
```

The leading `!` means the CRAM bit is the *inverse* of the value bit, so
the default INIT of all-ones is sixteen zeros in the bitstream. The same
holds for enums, whose default value is listed with `-` for "no bits".

So there is still **no golden bitstream to start from and no base image
to ship**: a zeroed buffer plus the database's defaults is the blank
device, exactly. What the original wording missed is that the defaults
have to be *applied*, to all 4,312 tiles, and that turned out to be the
most expensive step on the device until it was memoised (section 11.4).

### 2.2 12F and 25F are the same die

`tilegrid.json` for LFE5U-12F and LFE5U-25F are byte-identical
(md5 `f96e14359ba5` for both). Same frame count, same bits per frame,
same tile layout; the parts differ only in IDCODE and in what Lattice
guarantees. **One database serves Obst, Lakritz and the 12F/25F ULX3S.**
45F and 85F each need their own.

### 2.3 The bitstream writer is small

The format is byte-oriented, big-endian, and fully documented. A
bitstream is a comment header, a `0xFFFFBDB3` preamble, then a short
sequence of commands:

| | |
|---|---|
| `LSC_RESET_CRC` (3B) | reset the CRC16 |
| `VERIFY_ID` (E2) | 32-bit IDCODE; the device refuses a mismatch |
| `LSC_PROG_CNTRL0` (22) | control register, normally 0x40000000 |
| `LSC_INIT_ADDRESS` (46) | reset the frame address register |
| `LSC_PROG_INCR_RTI` (82) | the frames, uncompressed |
| `LSC_PROG_INCR_CMP` (B8) | the frames, compressed |
| `ISC_PROGRAM_USERCODE` (C2) | |
| `ISC_PROGRAM_DONE` (5E) | end; DONE goes high |

Frames are sent **highest-numbered first**, each followed by a CRC16 and
a dummy `0xFF`. The CRC is polynomial 0x8005 with no bit reversal
("CRC16-BUYPASS"), accumulated across commands and not reset between
frames.

Compression, which `Makefile` uses (`ecppack --compress`), is a
four-case prefix code over bytes: a zero byte is one bit; a byte with
one bit set is six bits; one of eight dictionary bytes is six bits;
anything else is `11` plus the literal byte. The dictionary is the eight
most common remaining bytes, shipped in `LSC_WRITE_COMP_DIC`. This is
half a page of code and it matters, because an uncompressed 582 KB
bitstream against a 1792 KB flash partition is wasteful in a system that
already does not fit its flash comfortably (`docs/dfu_upgrade.md`).

libtrellis's whole serialiser is about 230 lines of C++. **Estimate for
`zfpga pack`, including the `.config` parser and the database lookup: 900 to
1,200 lines of C.** Actual, section 11: 2,524 lines of C including
the port layer, the database validation and the diagnostics, plus a
344-line host generator. Longer than estimated because refusing bad
input with a line number is most of a parser.

### 2.4 The database is 2 MB of ASCII, and it is per *type*

This is the structural fact the whole design turns on.

For a 25F: **4,312 tile instances of 134 distinct types.** The bit
database is stored per type, not per instance:

| Type | Instances in a 25F | `bits.db` |
|---|---|---|
| PLC2 (logic) | 3,036 | 58,425 |
| CIB / CIB_LR / CIB_EBR / CIB_DSP | 415 total | 52,677 each |
| PICL0 / PICR0 (IO) | 12 each | 15,191 |
| PIOT0 / PIOT1 / PICT0 / PICT1 | 28 each | 2,400-4,000 |
| TAP_DRIVE | 176 | 1,129 |

Totalled over every type present in a 25F: **2,004,848 bytes of text**,
containing **54,127 routing arcs**, **10,314 fixed connections**, 2,840
config enums and 159 config words, over 831 distinct mux destination
names.

Two megabytes. That fits on the card without thought, and a packed
binary form of it fits in RAM.

**But it must never be expanded per instance.** 3,036 PLC2 tiles times
roughly 2,500 arcs each is 7.6 million edges, which is what nextpnr
builds and why nextpnr-ecp5 wants a gigabyte. The database stays
per-type and the router asks "what drives `A0` in tile R14C22?" and gets
an answer computed on demand from the PLC2 entry. This is what libtrellis
does and it is the reason this is feasible at all on a 32 MB machine.

### 2.5 The routing model is simple, and its naming is not

Four resource classes, all unidirectional, all originating in the tile
that drives them:

| | |
|---|---|
| **X0** | 8 wires that never leave the tile (`H00L0x00`, `V00T0x00`, ...) |
| **X1** | 8 neighbour wires to the adjacent tile (`H01E0x01`, `V01N0x01`, ...) |
| **X2** | 32 span-2 wires reaching the next two tiles |
| **X6** | 16 span-6 wires reaching two tiles with a gap of two |

BEL inputs are `A0`-`A7`, `B0`-`B7`, `C0`-`C7`, `D0`-`D7` (LUT inputs),
`M0`-`M7`, `CLK0`/`CLK1`, `LSR0`/`LSR1`, `CE0`-`CE3`. Outputs are
`F0`-`F7` (LUT) and `Q0`-`Q7` (FF). CIB tiles use the same names with a
`J` prefix.

Names in the database are **tile-relative**, and a wire visible in one
tile appears in a neighbour's database under a prefixed name --
`N1_V02S0501` in one tile is the same physical wire as `V02S0501` two
tiles away. Section 6 explains why this is the schedule risk rather than
a detail.

### 2.6 The board decides who gets this at all

`rtl/boards.vh`:

| Board | FPGA | RAM | Viable? |
|---|---|---|---|
| Obst | 12F | 1 MB (some 2 MB) | **No.** Same verdict as `zcc`. |
| sergei_ml1 | 45F | 8 MB | Marginal; 45F CRAM alone is 1 MB |
| Lakritz | 25F | 32 MB | **Yes, and this is the development target** |
| mozart_ml1 | 45F | 32 MB | Yes |
| ULX3S | 12F/25F/45F/85F | 32 MB | Yes |
| Kölsch, Lebkuchen | GateMate | 32 MB | Different family; section 9 |

`Z_PROC_STACK_SIZE_HUGE` (4 MB) already exists -- `zcc` needed it and
added it. A blinky-class run on a 25F should sit comfortably inside it;
a 45F or 85F run will not, and that is a real ceiling rather than a
theoretical one (section 7 budgets it).

Following the precedent in `docs/posix.md` §2.2: **refuse below the
floor and say why**, the way `sw/apps/serial` exits on a board with no
UART1. A tool that starts and then fails at the CRAM allocation has
wasted a minute of a 12 MIPS machine's time to tell you something it
knew at startup.

### 2.7 Build in `/ram`

`docs/ramdisk.md` and `docs/posix.md` §2.4 both land on this and it
applies here with more force: the database is read repeatedly, the CRAM
is 560 KB of scratch, and the output is another 582 KB. Read the
database from the card once, work in `/ram`, write the finished `.bit`
back. `sw/apps/web` already does exactly this with its page spool.

### 2.8 The loop closes, and most of the machinery already exists

An earlier draft of this document said there was no path from Zeitlos to
the configuration flash and treated closing the loop as a late, uncertain
phase. That was wrong in three ways, and `docs/zboot.md` now covers the
subject properly. The corrections, because they change the plan:

- **PROGRAMN is wired back to the FPGA on every board in this tree.** It
  has to be: it is how the DFU bootloader hands off to Zeitlos. It is
  also the *only* reconfiguration trigger reachable from fabric, since
  REFRESH is not, so having it is the difference between a closable loop
  and no loop at all.
- **`rtl/sysctl.v` already instantiates `USRMCLK`** and already drives
  the configuration flash's chip select, MOSI and MISO through
  `rtl/spiflashro.v` (now `rtl/spiflash.v`, which also writes -- `docs/spiflash.md`), which serves the memory-mapped ROM window. The
  pins, the primitive and an SPI master are present and working. What is
  missing is that the controller only issues reads.
- **`ecppack --bootaddr` is the whole addressing mechanism**, and the
  shipping bootloader already uses it. A bitstream carries the address
  the *next* configuration is read from, as an 8-bit word (`addr[23:16]`)
  in the `EFB1_PICB1` tile plus bit 20 of `CTRL0`.

Because power-on always reads address 0 -- nothing is configured yet, so
there is nowhere for a BOOTADDR to come from -- a three-slot layout
gives exactly the property this needs:

```
power on          -> 0x000000            bootloader
                  -> USERPART_START      Zeitlos
  PROGRAMN        -> GWPART_START        gateware Zeitlos built
  PROGRAMN/power  -> 0x000000            bootloader, then Zeitlos
```

**The generated bitstream needs nothing special.** Its BOOTADDR defaults
to 0, so any reconfiguration returns to the bootloader and from there to
Zeitlos. The failure mode of a bad bitstream -- hung, wrong, failed CRC,
DONE never asserted -- is a power cycle, with no cable and no host.
Nothing a user builds can strand the machine, because nothing a user
builds sits at address 0.

### 2.9 And there is a path that needs nothing at all

The configuration flash on these boards **is an MMOD**, and
`sw/apps/mmod` already writes MMODs plugged into the Pmod socket --
DETECT, the chip-select interlock, ERASE, WRITE and VERIFY, all chunked
against the event loop (`docs/mmod.md`).

So with two modules, the loop closes today: build a bitstream, write it
to a second MMOD through the Pmod socket, power down, swap, power up.
Swap back to return.

This matters for the plan rather than only for the user, and it is the
reason Phase 3 below can finish on real hardware:

- It is available **now**, with no new RTL, so bitstreams can be tested
  on a real FPGA from the first working packer.
- It cannot brick anything -- the Zeitlos module is never written.
- A module written this way is a complete standalone boot image, which
  is a more useful artifact than a slot inside another layout.

A bitstream written for the configuration socket goes at address 0, not
at `USERPART_START`, and carries no bootloader unless one is written
alongside it. `docs/zboot.md` §8 has the detail.

## 3. Build it backwards

### 3.1 Why not start with the Verilog parser

Because it is the familiar part, and familiarity is what makes it the
wrong end to start at. A Verilog front end with nothing underneath it
can only be tested against itself: it emits a netlist nobody can turn
into a bitstream, so "is this netlist right?" has no answer except
reading it.

Every other stage has a reference implementation already producing
correct output *for this exact tree*, on the host, today. Going
backwards means each stage is written against a known-good artifact.

### 3.2 The differential test that falls out, and it is a good one

`docs/zcc.md`'s headline is that the compiler's output is byte-identical
to GCC's, checked on every test run. The equivalent here is available
immediately and it is stronger:

```
zfpga pack output/lakritz/soc_final.config -c -o soc.bit
md5sum soc.bit output/lakritz/soc.bit      # must match
```

That input is **not a toy**. It is the real Zeitlos SOC: every tile type
the device has, BRAM, PLLs, DDR-capable IO, several IO standards,
7,562 frames of it. A packer that reproduces that file byte-for-byte is
correct in a way no blinky-sized test could establish -- and it can be
written and proven **before a single line of the Verilog front end
exists.**

The same applies one stage up. `zfpga pnr`'s `.config` for a hand-written
netlist can be diffed against nextpnr's `.config` for the equivalent
Yosys JSON, arc by arc. The two will not match -- different placers
choose different sites -- but the *set of tiles touched* and the
legality of every arc can be checked against the database, and any arc
`zfpga pnr` emits that nextpnr would never emit is a bug worth seeing.

`docs/zcc.md` on why differential beats golden applies verbatim: a
golden file records what the tool did the day it was written, which
catches regressions and never catches a bug that was there from the
start. ecppack does not share our bugs.

### 3.3 Keep Trellis's formats at every seam

| Seam | Format | Reference producer | Reference consumer |
|---|---|---|---|
| netlist | Yosys JSON, a strict subset | `yosys synth_ecp5` | nextpnr-ecp5 |
| placed + routed design | Trellis `.config` text | `nextpnr-ecp5 --textcfg` | `ecppack` |
| bitstream | `.bit` | `ecppack` | openFPGALoader, the FPGA |

Three reasons this is not just tidiness:

- **Either side can be swapped for the real tool during bringup.** A
  `.config` from nextpnr, packed by `zfpga pack`, tests the packer alone. A
  `.config` from `zfpga pnr`, packed by `ecppack`, tests the router alone.
  When something produces a dead bitstream, that substitution is how you
  find out which half is wrong, and it costs nothing if the formats
  match.
- **It is diffable**, which is most of the debugging.
- **It is readable on a 25-line screen**, which is the screen this will
  be debugged from eventually.

The one deliberate deviation: **the chip database on the card is a
packed binary, not JSON or the Trellis ASCII.** `tilegrid.json` for a
25F is 2.8 MB of JSON, and parsing that on a PicoRV32 is a minute of
pure waste on every run. A host tool converts it once (Phase 0).

---

## 4. The tools

**One directory, one binary, subcommands.** `sw/apps/zfpga/`, producing
`zfpga`:

| Command | Does | Replaces | Phase |
|---|---|---|---|
| `zfpga pack blink.cfg` | `.config` -> `.bit`; `-c` compress, `-a` bootaddr | `ecppack` | 1-2 |
| `zfpga unpack x.bit` | `.bit` -> `.config` | `ecpunpack` | 2 |
| `zfpga bram x.cfg -f seed.hex -t new.hex` | swap BRAM contents in a finished config | `ecpbram` | 2 |
| `zfpga info` | device, database, flash layout, free slot | -- | 0-2 |
| `zfpga pnr blink.zn` | netlist -> placed, routed `.config`; **Phase 3: placement and routing given** | `nextpnr-ecp5` | 3-5 |
| `zfpga synth blink.v` | Verilog -> netlist | `yosys synth_ecp5` | 6 |
| `zfpga build blink.v` | all three, in one process -> `blink.bit` | the `Makefile` | 6 |
| `reboot` (a shell command, not zfpga) | assert PROGRAMN | -- | 7, step 1 |
| `zfpga run blink.bit` (was `zfpga boot`) | write free flash, verify, jump | `openFPGALoader -f` | 7 |

Plus one host-only generator, `sw/apps/zfpga/tools/mkzdb.py`, which turns
the vendored database into the packed `.zdb` files the card carries --
the same arrangement as `sw/apps/zcc/libz/mksyms.py`.

### Why one binary rather than four

- **The heap tier is granted by exact name.** `z_proc_stack_size_for()`
  in `sw/os/kernel.h` hands `HUGE` to `zcc` and `posix` by `strcmp`. Four
  tools would be four names in the kernel, each a core change; one tool
  is one word, once.
- **The shared half is most of it.** The database reader, the `.config`
  reader and writer, the arena, the diagnostic formatter and the port
  layer are common to every stage. Linked once, not four times.
- **`zfpga build` runs the pipeline in one process**, holding the netlist
  and the configuration in memory between stages instead of writing and
  re-reading them. That matters because SD throughput is still
  unmeasured (`docs/sdcard.md`), so the cost of loading a binary is an
  unknown to design *around*. One load per build, not three.
  Intermediates are written to `/ram` only when asked (`-k`), which is
  what debugging and the differential tests want.
- `zcc` is one program named after what it does. Same shape.

The cost is that `zfpga pack` loads the synthesiser too. Estimated total
150-250 KB; worth revisiting only if measured load times say so.

### Command lines fit in 96 bytes

A program started from `posix` gets its command line as a launch
argument, and `Z_WM_ARG_MAX` is **96 bytes**. That is not a problem so
much as a design constraint worth accepting deliberately:

- **Board profiles, not flags.** `-b lakritz` implies device, package,
  the `.lpf`, `USERPART_START`, the gateware slot and flash size, read
  from `/fpga/boards/lakritz` on the card. One source of truth, and the
  same keep-in-sync hazard `docs/zboot.md` §7 describes, so the release
  should *generate* these files from the values in `Makefile`, not
  carry a second hand-written copy.
- **`-b` can default.** There is no board-identity CSR today (`docs/csrs.md`
  has memory size and feature bits, not a board), and the card image is
  the same for every target, so the card cannot know either. Until a
  `BOARD` CSR exists, a one-line `/fpga/board` the user writes once
  serves as the default.
- **Outputs are derived.** `blink.v` becomes `blink.bit` unless `-o`
  says otherwise.
- **The card layout is the search path**, exactly as `zcc` finds
  `/libz` without being told (`docs/zcc.md`, "The card's layout is the
  default search path"): `/fpga/lfe5u25f.zdb`, `/fpga/boards/`.

### 4.1 Follow zcc's shape exactly

`docs/zcc.md`'s "The seam" section is the whole argument and it applies
without modification: **host build and device build from one set of
sources, behind a port layer of four functions** -- read a file, write a
file, print, stop -- rather than `#ifdef`s threaded through the router.
The device build is the one that matters; the host build is the one that
gets tested; scattered conditionals make those two diverge line by line
in the files where a difference is hardest to see.

And the accompanying warning, which cost that project a crash on real
hardware: **a build shape adopted to work around the development
environment is a build shape nobody is testing.** These are ordinary
Zeitlos apps -- `sw/common`'s runtime, newlib, `riscv-app.ld` -- the same
shape as everything else in `sw/apps`.

### 4.2 The Verilog subset

The tree's own RTL is plain Verilog-2001. A scan of every file under
`rtl/` finds no `always_ff`, no `always_comb`, no `logic` declarations
and no `typedef struct`. **The house style is already the subset**,
which is a considerable piece of luck and worth preserving deliberately.

For blinky-class designs, `zfpga synth` needs:

| | |
|---|---|
| Declarations | `module`/`endmodule`, `input`/`output`/`inout`, `wire`, `reg`, vectors with constant bounds, `localparam` as plain substitution |
| Combinational | `assign`, the operator set (`& \| ^ ~ + - << >> == != < > ?:`), concatenation and replication, constants in all four bases |
| Sequential | `always @(posedge clk)`, `if`/`else`, non-blocking assignment, synchronous reset |
| Structural | instantiation of other modules in the same file |

And must **refuse, loudly**, rather than approximate:

`generate`, `parameter` overrides at instantiation, arrays and inferred
memories, `initial`, delays, `task`/`function`, `case` (until it is
done properly), tri-state, multiple drivers on a net, anything
`always @(*)`-shaped until that is done properly, and every vendor
primitive.

The rule from `docs/zcc.md` transfers and matters more here: **a wrong
answer that compiles is worse than a tool that refuses.** zcc came to
this the hard way, with structs passed by value that compiled silently
and returned garbage. The equivalent failure here is worse still,
because a wrong bitstream *programs*. It does not crash; it configures
an FPGA into a state nobody designed, and on a board where the same
pins drive SDRAM, that is not merely a wrong answer.

Multiple drivers deserve a specific mention: it is the one Verilog
mistake that is silently legal at the language level, disastrous in
gateware, and trivially detected in the netlist. Detect it in `zfpga synth`
and refuse.

---

### 4.3 Where it lives

Everything that *can* be isolated is:

```
sw/apps/zfpga/
  Makefile            device build -> zfpga.bin (an ordinary Zeitlos app)
  Makefile.host       host build -> ./zfpga, same sources
  zfpga.h  main.c     types and the .zdb format; subcommand dispatch
  zfpga_port.h  port_host.c  port_dev.c     the seam (§4.1, §11.2)
  util.c              arena, formatter, diagnostics, chunked I/O
  db.c                the packed database: load, validate, look up
  chip.c              configuration memory; applying tile settings
  cfg.c               the streaming .config reader
  pack.c              the bitstream writer
  tools/mkzdb.py      host: vendored db -> .zdb
  ext/prjtrellis-db/  vendored data, CC0, pinned (§9)
  examples/           blink.v, its .lpf files, fixture provenance
  tests/              run.sh (differential), run_dev.sh (under sim/),
                      cases.txt, fixtures/, expect.md5
```

That is what exists after Phase 2. To come, in the same directory:
`unpack.c` and `bram.c` (Phase 2's remainder), `lpf.c`, `net.c`,
`place.c` and `route.c` (Phases 3-5), `v*.c` for synthesis (Phase 6),
`boot.c` (Phase 7), and board profiles.

`ext/` inside the app directory rather than the tree's usual `sw/ext/`
and `rtl/ext/`, because isolation is the point: removing `sw/apps/zfpga`
should remove all of it.

**What cannot live there**, listed so the footprint is known up front:

| Where | What | When |
|---|---|---|
| `sw/os/kernel.h` | `"zfpga"` added to the `HUGE` name check | Phase 2 |
| `sw/apps/Makefile` | one word in `APPS` | Phase 2 |
| `release/lib/mkfatimg.py` | `/fpga/*.zdb`, board profiles, examples | Phase 2 |
| `rtl/sysctl.v`, `rtl/spiflash.v` (was `spiflashro.v`), `boards/*.lpf`, `Makefile` | PROGRAMN, flash write, `--bootaddr` | Phase 7 |
| `docs/` | `zfpga.md`, `zboot.md` | every phase |

The Phase 7 rows are gateware and cannot live in an app directory. They
are also useful to Zeitlos whether or not `zfpga` exists -- a reboot, and a
gateware launcher -- which is the right test for whether a change belongs
to this project or merely arrives with it.

### 4.4 Running under `posix`

Yes, and for the reasons you gave: `vi` is there, and launching a tool
with arguments is what `posix` already does for `zcc`. What that means
concretely:

- **Arguments** arrive through `z_launch_arg_take()`, the 96-byte limit
  above. `zcc` needed no change to adopt this and neither will `zfpga`.
- **Output** reaches the terminal through the `stdout` relay, which the
  child opts into (`docs/posix.md` §9d, about fifteen lines, which `zcc`
  already carries). Diagnostics in `file:line:col: error:` form, so `vi`
  can jump to them.
- **Exit status** is real, so `zfpga build blink.v && zfpga run blink.bit`
  works as it reads.
- **No prompting.** The relay is one-way and input typed while a child
  runs is dropped. So `zfpga run` cannot ask "are you sure?" -- and
  should not need to. Its safety comes from interlocks (it refuses any
  address outside the gateware slot, and verifies after writing), and
  **writing is separated from leaving**: `zfpga flash` writes and verifies,
  `zfpga run` or `jump` is a second, deliberate step. A write can be checked
  before anyone commits to leaving the operating system.
- **Memory.** `posix` holds a 4 MB tier and `zfpga` would hold another.
  On 32 MB that is unremarkable; on an 8 MB board `docs/posix.md`
  already says "posix or the desktop, not both", and `zfpga` makes that
  sharper. The first-fit allocator can fail a 4 MB request on
  fragmentation, so the same advice applies: start it early.

## 5. The blinky, in full

Making this concrete is the most useful thing this document can do,
because "blink an LED" hides a ladder of three separate milestones with
three separate failure modes.

### 5.1 What the design is

From `boards/lakritz_v0.lpf`:

```
LOCATE COMP "CLK_48" SITE "A7";   IOBUF PORT "CLK_48" IO_TYPE=LVCMOS33;
LOCATE COMP "LED_B"  SITE "A2";   IOBUF PORT "LED_B"  IO_TYPE=LVCMOS33;
```

A 24-bit counter off the 48 MHz clock has its top bit high for 2^23
cycles, 0.17 s, then low for as long: about three blinks a second.
**[corrected]** This said "a 0.7 second blink" until the on-board test
procedure (`docs/zfpga-test.md`) was written and the arithmetic
checked. (LED polarity on this board should be confirmed
rather than assumed; either way the design is the same and only the
inversion moves.)

Resources: **24 FFs and 12 slices in CCU2 mode** -- each ECP5 slice in
carry mode is a 2-bit adder, and a PLC2 tile holds four slices
(`SLICEA`-`SLICED`, two LUT4s and two FFs each). So **three PLC2 tiles**,
stacked vertically so the carry chain runs `FCO` to `FCI` between them,
plus two PIO sites.

### 5.2 What has to be configured, and what does not

Needed:

- `enum: PIOA.BASE_TYPE OUTPUT_LVCMOS33` (or `INPUT_LVCMOS33`) in the
  right PIC tile, plus `PULLMODE`, plus the matching `IOLOGIC` entries
- **the bank's `VCCIO` in the `BANKREF` tile.** This is the classic
  omission: get it wrong and the IO is configured, looks right in the
  `.config`, and does not drive.
- `SLICEx.MODE = CCU2` and the `INJECT1_0`/`INJECT1_1` enums on all
  twelve slices
- `SLICEx.Kn.INIT` for the adder LUTs
- `SLICEx.REGn.REGSET`, `SLICEx.GSR`, `LSR1.SRMODE` and the FF enums
- arcs: clock pin -> `CLK0` in three tiles; counter bit 23 -> the LED
  PIO; carry is a fixed connection and needs no arc

Explicitly **not** needed, and this is what keeps the first milestone
reachable:

- **No PLL.** The 48 MHz clock is an input pin.
- **No global clock network.** Three adjacent tiles can be clocked from
  general routing. The `DCC`/`DCS`/`CMUX` global tree, primary clock
  promotion, and clock-region legality are an entire subsystem, and
  skipping it costs nothing at this size. It becomes mandatory somewhere
  around a few dozen tiles, and that is the right time to pay for it.
- No EBR, no DSP, no carry across non-adjacent tiles, no IO delay, no
  DQS.

### 5.3 The ladder

Three milestones, not one:

1. **A bitstream that loads and does nothing.** Zero CRAM bits set, a
   correct IDCODE, correct CRCs, correct frame count. DONE goes high.
   This tests the packer end to end and nothing else. It is worth its
   own day, because **a bad CRC fails silently** -- the device simply
   does not come up, and there is no distinguishing that from twenty
   other causes.
2. **LED permanently on.** One LUT4 with `INIT` all-ones, one PIO, one
   arc, one bank config. This is the first time the database, the tile
   addressing and the IO configuration are all exercised, and it has
   exactly one visible bit of output -- which is the right amount when
   the alternative is debugging a counter at the same time.
3. **LED blinking.** The counter, the carry chain, the clock route.

Each rung has a different thing that can be wrong, and doing them
separately is the difference between an afternoon and a fortnight.

---

## 6. What is actually hard, ranked

### 6.1 Resolving relative wire names — the schedule risk

**[corrected]** This section originally put the resolver in Phase 0, as
a prerequisite of the packer. **The packer does not need it.** A
`.config` arc is written in the same tile-relative names the database
uses (`arc: A0 N1_V02S0501` in a PLC2 tile is looked up in PLC2's
`.mux A0` directly), so packing never has to know which physical wire a
name denotes. The resolver is a router's problem and moves to Phase 4,
where it still leads. The validation plan below stands unchanged, as
Phase 4's first step.

Everything else here has documentation. This does not.

The database says `.mux A0` has a source `N1_V02S0501`. Turning that
into "tile R13C22's wire `V02S0501`" requires the rules for the
directional prefixes, the span encoding inside the wire name, which end
of a span wire is the driver, and what happens at the edges of the
array. Project Trellis's published architecture documentation describes
the *resources* (section 2.5) but not the *name algebra*, and the
authority is nextpnr-ecp5's chip database generator plus libtrellis's
`RoutingGraph.cpp`.

This is where the project will burn time, and it should be attacked
first and in isolation:

**Write the resolver on the host, first in Phase 4, and validate it by
round-tripping every arc in the real SOC's `.config`.** There are tens of
thousands of them, produced by a router that is known correct, covering
every tile type on the device. For each `arc: <sink> <source>` in a
named tile, the resolver must produce the same (frame, bit) set that
libtrellis produces. If it does, the name algebra is right. If it is
right for 54,000 arcs across the real design, it is right.

Do this before the router exists. A router built on a resolver that is
subtly wrong will present as a routing bug, and those are expensive.

### 6.2 Routing

A* or Dijkstra over a graph generated on demand, with the cost function
doing congestion avoidance across nets. For blinky-class designs a
bounded breadth-first search over a handful of tiles is enough and the
first version should be exactly that, with an explicit tile-window limit
and a clean failure when it is exceeded.

The honest statement: this is where a design stops fitting long before
it runs out of LUTs.

### 6.3 Technology mapping

The least risky part, despite sounding like the hardest. For the target
class, one LUT4 per operator with trivial merging is sufficient and
correct. A real cut-based mapper is a later, contained improvement, in
the same way `docs/zcc.md` describes its register allocator as "a
contained later job" -- and for the same reason: it changes the quality
of the output, not whether the output is right.

### 6.4 Speed

Extrapolating from `zcc`'s measured figures (10.3M instructions for a
ten-line C file with headers, roughly a second at 12 MIPS):

| | Estimate | **Measured** (§11.4, under `sim/`) |
|---|---|---|
| `zfpga pack`, blinky config | well under a second | 14.9M instructions, ~1.2 s |
| the same, compressed | -- | 19.4M, ~1.6 s |
| the DFU bootloader, 641 tiles, compressed | -- | 95M, ~8 s |
| `zfpga pack`, the real SOC config | several seconds | not yet run (§11.6) |
| CRC16 over 7,562 frames | ~5.6M cycles | not the bottleneck, as predicted |
| `zfpga pnr`, blinky | trivial | -- |
| `zfpga synth`, a 50-line module | seconds | -- |

The first blinky measured **100.7M** instructions, and "well under a
second" was wrong by a factor of eight until three fixes in §11.4. The
estimate below is why that was found on the first run rather than on a
board.

**Estimated, not measured** (as originally written). The `zcc` experience is the warning here:
its first device compile took 729 million instructions because `malloc`
was a first-fit walk, and only the device build could have shown it.
Use a bump arena from the start -- `zcc`'s `zalloc` already exists and
this is the same access pattern, one allocation per token, nothing ever
freed.

### 6.5 Memory

For a 25F blinky: 560 KB CRAM, plus a database working set, plus a
netlist that rounds to nothing. Budget 2-3 MB against the 4 MB
`Z_PROC_STACK_SIZE_HUGE` tier. Comfortable.

For an 85F: 1.9 MB of CRAM before anything else. That does not fit the
current tier with room to work, and the answer is either a fifth tier or
frame-at-a-time serialisation that never holds the whole CRAM. Worth
knowing now; not worth solving until a target needs it.

---

## 7. Phased plan

Each phase ends with this document updated, per the convention in
`docs/posix.md`.

### Phase 0 — groundwork, host only — DONE, except as noted (§11.1)

- `sw/apps/zfpga/tools/mkzdb.py`: vendored prjtrellis-db -> packed binary
  `.zdb` per device.
  Per-type bit database, tilegrid, IO/package map, string table.
- ~~**The relative-wire-name resolver**, host-side (§6.1).~~ Moved to
  Phase 4: the packer does not need it. Done there, §13.1.
- Card layout decided: `/fpga/lfe5u25f.zdb` and friends, plus a
  `release/lib/mkfatimg.py` entry so it ships.

**Done when:** every `arc:` in `output/lakritz/soc_final.config`
resolves to the same (frame, bit) set libtrellis computes for it. No
exceptions, no skipped tile types. **As built,** that criterion is met
by Phase 1's instead -- a packer that matches ecppack byte for byte has
resolved every arc to the right bits -- except that the SOC config
itself has not yet been run (§11.6).

### Phase 1 — `zfpga pack`, host build — DONE, SOC case pending (§11.3, §11.6)

`.config` + `.zdb` -> `.bit`. Uncompressed first, then compressed.

It must also handle `--freq`, `--compress` and **`--bootaddr`** from the
start. BOOTADDR is not an afterthought here: it is an 8-bit config word
in one tile plus bit 20 of `CTRL0` (`docs/zboot.md` §2), it costs almost
nothing to support, and without it `zfpga pack` cannot produce an image that
boots anywhere except address 0.

**Done when:** `zfpga pack` output is byte-identical to `ecppack` output for
the real Zeitlos SOC config on a 25F board, and on a 45F and an 85F, and
for the DFU bootloader's own config with its `--bootaddr 0x040000`.
Three devices, because frame geometry and padding differ and a 25F-only
packer will have hardcoded something; and the bootloader because it is
the smallest real design that exercises BOOTADDR.

### Phase 2 — `zfpga pack`, on the device — DONE, on hardware (§23), with `unpack` (§18); `bram` pending

Port layer, ordinary Zeitlos app, plus `zfpga unpack` for the self-contained
round trip.

**Done when:** the device, reading the SOC config off its own SD card,
produces a `.bit` whose md5 matches the host's. This is the `zcc`
"byte-for-byte identical" moment, and it arrives before any of the hard
parts.

It is also **immediately useful on its own**, and `zfpga bram` belongs in
this phase for that reason rather than being left unscheduled: with it,
the machine can swap a BIOS image into its own gateware build without a
host (§1.1, application 2). Its test is the same kind as `zfpga pack`'s:
`zfpga bram` on `output/<board>/soc.config` with the tree's own seed and
BIOS must reproduce `soc_final.config` exactly.

### Phase 3 — manual placement: netlist -> `.config` — DONE (§12); on hardware through the blinky (§23)

A textual netlist with explicit site assignment and explicit arcs. No
router, no placer. The user -- or a script -- says where everything goes.

**Done when:** the three rungs of §5.3 are climbed on real hardware. A
Lakritz blinking, from a bitstream the Lakritz built.

**And it does not need a host to get there.** The bitstream goes to a
second MMOD through the Pmod socket with `sw/apps/mmod` as it stands
today (§2.9), the modules are swapped, and the board boots it. Written
for the configuration socket it goes at address 0. Programming by
`openFPGALoader` from a host stays available and is the faster loop
during development, but the MMOD route is what makes this milestone the
real thing rather than a demonstration.

### Phase 4 — the router — v2 WORKING, on hardware (§13, §14, §23); global clocks to come

Lazy graph over the packed database, bounded-window search, congestion
cost. Arcs no longer have to be given.

**Done when:** the Phase 3 netlist routes itself, and a second, larger
test design (a shift register across several tiles, an 8-bit adder)
routes and works.

### Phase 5 — the placer — WORKING, on hardware (§15, §16, §23): LUTs, flip-flops, carry chains, IO

IO sites from the `.lpf`; logic packed into slices and placed into
adjacent tiles near the IO that needs them. Greedy, bounding-box,
unambitious.

### Phase 6 — `zfpga synth` — DONE, on hardware (§19, §23); instances, loops, memories, signed, functions (§24)

Verilog subset -> netlist. Lexer, parser, elaboration, mapping to
LUT4 + FF + carry. Refusal for everything in §4.2's second list.

**Done when:** `blink.v` typed into `vi` on the machine becomes a
working bitstream, with no host involved except the programming cable.

### Phase 7 — closing the loop in place — step 1 BUILT (`reboot`); the design agreed

**[updated]** `docs/zboot.md` §5 now has the agreed design, which
replaces the fixed gateware slot below: a **jumploader** at `0x1D0000`
on every board -- a small bitstream Zeitlos jumps through, which
reboots (the default, pointing at 0) or boots user gateware placed
wherever there is room. Step 1, PROGRAMN and `reboot`, is built
(`docs/zboot.md` §6) for Lakritz, Obst and Mozart ML1, awaiting a
hardware test. The plan as first written:

Three pieces, in this order, because each is testable alone:

1. **PROGRAMN brought out** (site `M8` on a Lakritz, open-drain, the
   same `BB` instantiation the bootloader uses). This alone gives
   Zeitlos a `reboot`, useful on its own, and it exercises the half that
   can leave a board not coming back -- while the flash is still
   untouched.
2. **Write and erase** -- **done** as `rtl/spiflash.v`, replacing `rtl/spiflashro.v` (`docs/spiflash.md`). The
   pins, `USRMCLK` and the SPI master are already there;
   `usb_spiflash_bridge.v` in the bootloader does the same commands over
   the same pins, but under Apache 2.0 (§9.3) -- a reference, not
   something to vendor.
3. **The third slot**: `--bootaddr $(GWPART_START)` in `Makefile`, the
   partition map, and a driver shaped like `sw/apps/mmod`'s chunked
   state machine, with the ROM window quiesced for the duration
   (`docs/zboot.md` §7).

Note this is now **independent of Phases 3-6** rather than gated behind
them. It can be built in parallel by anyone, and step 1 is worth doing
whatever happens to the rest of this.

### Phase 8 — other families

In order of increasing pain:

| | |
|---|---|
| **iCE40** | Project IceStorm's format is simpler than ECP5's and the database is smaller. `up5k`/`hx8k` boards are already in `Makefile`. This is the cheap second family, and doing it is the test of whether the family abstraction in `zfpga pack`/`zfpga pnr` is real or notional. |
| **GateMate** | Kölsch and Lebkuchen. The bitstream is documented and `nextpnr-himbaechel` supports it, but `Makefile` currently uses Cologne Chip's proprietary `p_r`. Needs investigation before it can be scheduled. |
| **Artix-7** | prjxray's database is an order of magnitude larger, the bitstream format is more involved, and Zeitlos does not currently run on an Artix board. Last, and only if a board arrives. |

---

### Phase 9 — the socket

§1.1 application 6. Host side: a `Makefile` variant that builds the SOC
with an empty region and a registered interface pinned at its boundary,
and records both in a small socket description shipped on the card.
Device side: `zfpga pnr --socket` constrains placement to the region,
marks every wire in the SOC's `.config` as occupied, routes to the
pinned boundary wires, and merges.

**Done when:** a user-written peripheral -- a PWM block is the obvious
first one -- is built on the machine, merged into the running system's
own gateware, booted through the gateware slot, and driven by an app,
with the rest of Zeitlos working normally around it.

This is the phase that most needs the 45F and 85F boards, and the one
whose feasibility on a 25F depends on a number nobody has measured yet:
how large a region the SOC can spare.

## 8. Decisions taken

| Decision | Why |
|---|---|
| **Backwards: packer, then router, then synthesis** | Every stage gets a reference implementation to diff against, and the strongest available test -- byte-identity against `ecppack` on the real SOC -- lands first (§3.1, §3.2). |
| **Trellis `.config` and Yosys JSON at every seam** | Either side can be replaced by the real tool during bringup, which is how you find out which half is broken (§3.3). |
| **Packed binary database on the card; JSON never reaches the device** | 2.8 MB of JSON per device, parsed on a 12 MIPS machine, on every run (§3.3). |
| **Per-type database, graph expanded lazily. Never a full routing graph.** | 7.6 million edges if instantiated; 54,127 if not (§2.4). This is the decision that makes it fit. |
| **Refuse rather than approximate** | A wrong bitstream programs. It does not crash (§4.2). |
| **32 MB floor; refuse below it and say why at startup** | Obst cannot do this, the same way it cannot run `zcc` (§2.6). |
| **Work in `/ram`, output to the card** | The card is slow and the working set is 560 KB (§2.7). |
| **One source, host and device, behind a four-function port layer, from day one** | `docs/zcc.md`, "The seam". A build shape adopted for the development environment is a build shape nobody tests (§4.1). |
| **Bump-arena allocation from the start** | `zcc` lost 70x to a first-fit `malloc` and only the device build showed it (§6.4). |
| **ECP5 first; one database for 12F and 25F** | Identical tilegrids (§2.2), and Lakritz is the development board. |
| **No global clock network in the first working design** | An entire subsystem, skippable at three tiles, mandatory at thirty (§5.2). |
| **The SOC is not a target** | It is the ambition-trap, and naming it now is cheaper than abandoning it later (§1). |
| **`zfpga pack` supports `--bootaddr` from Phase 1** | One config word and one CTRL0 bit; without it nothing can boot from anywhere but address 0 (`docs/zboot.md` §2). |
| **The MMOD swap is the first hardware path, not a fallback** | It needs no new RTL, cannot brick anything, and lets Phase 3 finish on a real FPGA years before Phase 7 (§2.9). |
| **Nothing generated ever goes at flash address 0 on the running machine** | Address 0 is the bootloader, and a power cycle reading it is the recovery path for every possible bad bitstream (§2.8). |
| **One directory, one binary, subcommands** | The heap tier is granted by name in the kernel; the shared code is most of the code; one load per build while SD throughput is unmeasured (§4). |
| **Board profiles on the card, generated by the release** | Command lines are capped at 96 bytes, and a second hand-maintained copy of partition addresses is the bug `boardinfo.vh` already warns about (§4). |
| **`zfpga flash` writes; `zfpga run` / `jump` leave** | `posix` cannot prompt, and a verified write should be separable from giving up the OS (§4.4). |
| **Vendor data, not code** | The database is CC0 and has to ship; every line of code can be clean-room, as `zcc`'s is (§9). |
| **Phase 7 is parallel, not terminal** | Bringing PROGRAMN out is independently useful and exercises the risky half while the flash is untouched. |

---

## 9. Licensing

*Not legal advice; this is what the licence texts say.*

### 9.1 The data: vendor it, and there is nothing to comply with

`prjtrellis-db` is **CC0 1.0**. No conditions: no attribution required,
free to trim, convert and redistribute under this tree's own licence.
Copy its `COPYING` into `sw/apps/zfpga/ext/prjtrellis-db/` anyway and name
it in the README's licence section, because credit costs nothing and
the next person will want to know where the bits came from.

Pin it. Record the upstream commit in the vendored directory -- today
that is `015e0330630d7c238c0e4f2cdd9c8157eb78c54a` (2025-09-15) -- so
a later refresh is a reviewable diff rather than an act of faith.

Vendor it **verbatim, trimmed only by omission** -- no MachXO families,
no UM/UM5G variants, no 12F copy (identical to 25F, §2.2), no `timing/`
since there is no timing analysis -- so that it stays diffable against
upstream. The conversion to something compact is `mkzdb.py`'s job, at
build time.

The sizes, measured:

| | Text |
|---|---|
| LFE5U-25F `tilegrid.json` + `iodb.json` + `globals.json` | 2.9 MB |
| LFE5U-45F, same | 5.2 MB |
| LFE5U-85F, same | 9.8 MB |
| `tiledata/` for the 162 types across all three | 2.2 MB |
| **Total** | **20.0 MB**, 1.2 MB gzipped |

git stores the compressed form, so the repository grows by roughly a
megabyte; the working tree by twenty. Most of that is JSON indentation
-- the three tilegrids are 7.5 MB minified -- but minifying is a
transformation, and verbatim is worth more than 10 MB. **Recommendation:
vendor 25F plus `tiledata/` now (about 5 MB), and 45F/85F when a phase
needs them on the device.** Phase 1's byte-identity checks on 45F and 85F
run on the host and can use the host's installed database
(`fpga-trellis-database`, `docs/toolchain.md`) rather than a vendored
copy.

**Decided since:** a release will eventually carry databases for all
three dies on the card -- `/fpga/lfe5u25f.zdb` (serving 12F and 25F),
`lfe5u45f.zdb` and `lfe5u85f.zdb` -- so that one card works in any
ECP5 board. That is a later stage. Development and testing stay on the
25F, which is the only die vendored so far; `tests/run.sh` covers the
45F and 85F against an installed database, and `mkzdb.py` already
produces all three (1.32, 1.50 and 1.68 MB). Adding them is two
vendored directories and two lines in `release/lib/mkfatimg.py`.

### 9.2 The code: nothing needs vendoring

Every stage is written from scratch, and the `zcc` precedent is the
model: its "Provenance" section records what was looked at and what was
not, and this project should keep the same record.

What will be *read*, and closely: libtrellis and nextpnr-ecp5, both
**ISC**, above all for the relative-wire-name algebra (§6.1). ISC is
permissive enough that even copying a function is allowed with its
notice kept -- so if porting that logic turns out to be the pragmatic
answer, it is permitted, and it becomes one more line in the README's
licence exceptions alongside `rtl/cpu/picorv32`'s ISC. Clean-room is the
preference; the fallback is a line of README, not a licensing problem.

### 9.3 A correction to `docs/zboot.md`

An earlier revision of `docs/zboot.md` said `tinydfu-bootloader`'s
`usb_spiflash_bridge.v` was under the same licence as this tree. It is
not: **tinydfu-bootloader is Apache 2.0.** Zeitlos already carries
Apache 2.0 code (`rtl/ext/usb_hid_host`), so vendoring it would be
permissible with its licence retained. The recommendation is not to: the
flash write path is a small extension of `rtl/spiflashro.v` (since done: `rtl/spiflash.v`), a block that
is already this tree's own, and the command sequences it needs come from
the flash part's datasheet regardless of who else has implemented them.

### 9.4 The outputs

Bitstreams `zfpga` produces are the user's. Nothing in CC0 data or in this
tree's own code attaches to them. Lattice's technical notes are cited as
references; nothing is copied from them.

## 10. Open questions

Things this proposal does not answer and should not pretend to:

- **Where the third slot goes, per board.** A stock 2 MB Lakritz MMOD
  has 448 KB of 64 KB-aligned tail after the Zeitlos image, which is
  enough for a small design and tight for a large one. Whether to use
  that, to shrink `USERPART`, or to simply recommend a larger MMOD is a
  layout decision each board needs (`docs/zboot.md` §5).
- **How large a compressed bitstream actually is** for a design between
  "blinky" and "the SOC". An almost-empty 25F should compress to roughly
  80 KB, since a zero byte costs one bit -- but that is arithmetic, not
  a measurement, and the slot size depends on it.
- **How large `zfpga.bin` is, and what loading it costs.** One binary is
  the right shape (§4) *unless* SD reads turn out slow enough that
  loading 200 KB per invocation hurts. `docs/sdcard.md`'s measurements
  decide this, not reasoning.
- **A `BOARD` CSR.** Would let `-b` default correctly without a file on
  the card. A small core change with uses beyond this project; offsets
  `0x10`-`0x1c` are reserved for `FEATURES3` and must not be used for
  it.
- **The socket's region budget on a 25F** (Phase 9). Needs a real
  nextpnr run with a region carved out, and a count of what is left.
- **Quiescing the ROM window during a flash write.** Core apps are read
  from the flash being written (`docs/zboot.md` §7), and a sector erase
  is longer than `K_NO_PREEMPT_MAX_TICKS`. Needs a mechanism, not just
  care.
- **Whether an 85F fits.** 1.9 MB of CRAM inside a 4 MB tier, with a
  database working set and a netlist alongside it. Probably needs
  frame-at-a-time serialisation (§6.5).
- **`.lpf` parsing.** The constraint files in `boards/` are small and
  regular, but they contain `FREQUENCY`, `BLOCK` and IO-standard
  attributes that a subset parser must either honour or refuse.
- **How `zfpga synth` and `zcc` share infrastructure, if at all.** Both want
  a lexer, an arena, a diagnostic formatter with line numbers, and the
  same `file:line:col: error:` output that `te` and `vi` already parse.
  `sw/common` is the obvious home; whether that is worth doing before
  the second consumer exists is not obvious.
- **Where the sources live.** `docs/zcc.md` notes that zcc's sources sit
  in `sw/apps/zcc` rather than `tools/` specifically so nothing had to
  move when the device build arrived. The same argument applies.

---

# The development record

## 11. Phases 0-2: what actually happened

### 11.1 Using it

On the host, from `sw/apps/zfpga`:

```
make -f Makefile.host              # ./zfpga and db/lfe5u25f.zdb
./zfpga pack blink.config -D db    # -> blink.bit
./tests/run.sh                     # 34 differential and refusal cases
```

On the device, from a `posix` prompt, with the release's
`/fpga/lfe5u25f.zdb` on the card:

```
$ zfpga pack blink.config -c
$ zfpga info LFE5U-25F
```

The options are `ecppack`'s, spelled short to fit the 96-byte launch
argument:

| `zfpga pack` | `ecppack` | |
|---|---|---|
| `-o FILE` | `--bit` / positional | default: input with `.bit` |
| `-c` | `--compress` | |
| `-a ADDR` | `--bootaddr` | 64K aligned; `docs/zboot.md` §2 |
| `-f MHZ` | `--freq` | 2.4 4.8 9.7 19.4 38.8 62.0 |
| `-m MODE` | `--spimode` | fast-read dual-spi qspi |
| `-u N` | `--usercode` | accepts hex, which `ecppack` does not |
| `-i N` | `--idcode` | |
| `--background` | `--background` | |
| `-D DIR` | `--db` | default `/fpga` on the device; none on the host |

So the Zeitlos build's own packing step,
`ecppack --compress --freq 2.4 soc_final.config`, is
`zfpga pack soc_final.config -c -f 2.4`.

### 11.2 The shape, and one departure from zcc's

One directory, one binary, as §4 proposed: `sw/apps/zfpga`, producing
`zfpga.bin` (92 KB) for the device and `./zfpga` for the host from the
same sources. The seam is `zfpga_port.h`.

**The departure:** zcc's seam reads and writes whole files. zfpga's is
chunked -- open, read, write, close -- because its input can be
megabytes and its output up to 2 MB, and because a whole-file read
inside FatFs blocks `wm` for its duration (`docs/posix.md` §2.5). That
made the reader streaming, and streaming is sound only because of a
property checked while designing it: **no two tiles share a CRAM bit,
on any ECP5 die** (25F, 45F, 85F all checked). So configuring tiles in
file order and in libtrellis's name order must give the same bits, and
one record's worth of memory is all the reader holds. A configuration
of any size streams.

The cost of streaming is four refusals where libtrellis would do
something else: a tile configured twice, a `.tile` after a
`.tile_group`, a `.bram_init` without exactly 2,048 values, and a word
of the wrong width. None is anything nextpnr writes; each is an error
naming the line.

### 11.3 Byte identity, and what it covers

`tests/run.sh` packs each case with `zfpga` and with the installed
`ecppack` and requires identical bytes. **All pass**, against ecppack
1.4 (Ubuntu 24.04's `fpga-trellis`), whose database was confirmed
identical to the vendored one before any of it was trusted -- the only
difference is an extra package listing (TQFP144) that no bit depends on.

| Fixture | What it exercises |
|---|---|
| blink | a 24-bit counter on a Lakritz |
| dfu | the real Lakritz DFU bootloader: USRMCLK, open-drain PROGRAMN, EBR contents (`.bram_init`), a `.tile_group`; packed exactly as its own Makefile does, `--compress --bootaddr 0x040000` |
| blink45, blink85 | the other two dies, whose frames are 848 and 1,136 bits with different padding -- a 25F-only packer would have hardcoded something |
| edge | written to cover what nextpnr rarely does: `.comment` before `.device`, `.sysconfig MCCLK_FREQ 62` (ecppack's "62" to "62.0" quirk) and `COMPRESS_CONFIG ON`, two entries on one line, `_NONE_`, `unknown:` bits |

Across those, every option: plain and compressed, boot address,
frequency, SPI mode, usercode, IDCODE override, background
reconfiguration. Then fourteen refusals, each required to exit non-zero,
name the problem and leave no output file, and a truncated database,
which must be refused rather than read past its end.

With no `ecppack` installed, the runner falls back to MD5s recorded
from it (`tests/expect.md5`) and says so. That is the weaker check --
it catches regressions, not original sins -- and the output says which
ran.

**Two ecppack behaviours are reproduced deliberately**, because the
point is to agree and both are commented at their site in `pack.c`:
`--bootaddr` writes each bit by value and ignores the database's
inversion flag; and the `FF` after each frame is counted into the NEXT
frame's CRC, although the format notes say dummy bytes are excluded.

### 11.4 On the device: three fixes, 100M to 15M

`tests/run_dev.sh` runs `zfpga.bin` under `sim/zsim-headless` against
a card directory laid out as a release lays it out, and requires the
same bytes as the host build. **They match, plain and compressed.**

The first run of a blinky took **100.7M instructions** -- eight seconds
at 12 MIPS, against §6.4's estimate of "well under a second". Measured,
then fixed:

| | Blinky | What it was |
|---|---|---|
| first run | 100.7M | |
| defaults memoised per tile type | 42.4M | Applying defaults to all 4,312 tiles was 95M on its own, for an EMPTY design. An unconfigured tile starts at zero and receives only its type's defaults, so every one of a type ends identical: that image is now computed once per type and stamped (`chip_default_unseen()`). Exact by construction, and the tests say so. |
| bulk memory and output | **14.9M** | Zeroing the 1.3 MB database buffer before overwriting it; byte-loop `memset`/`memcpy`; 580 KB of frames written one call per byte. |

Compressed: 19.4M. The DFU bootloader, 641 configured tiles: 95M, about
eight seconds, spread thinly over parsing, lookups and applying
settings.

One result worth keeping: a word-at-a-time bit accumulator for the
compressor was faster on x86 and **slower on RV32** (21.7M against
19.4M), and the first version of it doubled the compressed cost because
`-Os` stopped inlining the per-bit function -- one call per zero byte,
and most of 560 KB is zero bytes. The one-bit writer is back, forced
inline, with the measurement in its comment. Only the device numbers
count, and only the simulator could show this.

**What the simulator cannot show is the card.** zfpga reads its
1.3 MB database on every run, and the SD read rate is still unmeasured
(`docs/sdcard.md`). At the ~19 KB/s once recorded that would be over a
minute; at the rate the SPI clock allows, about a second. That is now
the number that decides whether this is pleasant, and the obvious
saving if it is slow is the arc records, which are half the file and
could be a third smaller.

### 11.5 Memory, and the changes outside the directory

A 25F run holds the database (1.32 MB, resident because lookups are
random-access), the CRAM (560 KB) and small change. That needs the
4 MB `HUGE` tier, so the promised one-line kernel change is made:

| File | Change |
|---|---|
| `sw/os/kernel.h` | `"zfpga"` joins `zcc` and `posix` in `HUGE` |
| `sw/apps/Makefile` | `zfpga` in `APPS` |
| `release/lib/mkfatimg.py` | `apps/zfpga`, and `/fpga/lfe5u25f.zdb` in a new `FPGA_FILES` list |
| `README.md` | the vendored database's CC0 licence, in the exceptions list |

The kernel builds with the change. **`mkfatimg.py` has not been run**
end to end -- it needs every other app built and mtools -- only checked
to parse.

`zfpga.bin` carries about 40 KB of newlib `printf` it never calls. It
arrives through `sw/common/zport.c`, whose connect and send paths print
diagnostics, and which the `posix` output relay needs. `zcc` carries the
same 40 KB for the same reason. The fix belongs in `sw/common`, not in
an isolated app, and is noted here rather than made.

### 11.6 What is not verified

- **Real hardware.** Everything on the device side ran under `sim/`,
  which backs the card with a host directory, has no scheduler, and does
  not enforce heap tiers. Not covered: the real SD card, FatFs
  preemption deferral during 4 KB reads, and the 4 MB allocation
  succeeding on a live, fragmented pool.
- **The Zeitlos SOC's own config**, the headline test of §3.2. On the
  development machine, Ubuntu's nextpnr-ecp5 0.6 either segfaulted after
  packing or was killed for memory (4 GB) on the full SOC. Where the
  SOC already builds:

  ```
  cd sw/apps/zfpga && make -f Makefile.host
  ./zfpga pack ../../../output/lakritz/soc_final.config -c -f 2.4 -D db -o /tmp/z.bit
  cmp /tmp/z.bit ../../../output/lakritz/soc.bit
  ```

  The DFU bootloader is the nearest proxy that has passed: a real
  design with EBR, a tile group and multiboot.
- **A bitstream from `zfpga` programmed into an FPGA.** Byte identity
  with ecppack makes this a formality, but it has not been done.

### 11.7 Next

1. The SOC test above, on a machine where the SOC builds.
2. The same, on a Lakritz, from its own card -- Phase 2 on hardware.
3. `zfpga unpack` and `zfpga bram` to finish Phase 2 -- the second is
   what lets the machine install a new BIOS into its own gateware
   (§1.1, application 2).
4. Phase 3: the textual netlist with manual placement, and the three
   rungs of §5.3 on a real board, through a second MMOD.

## 12. Phase 3: the design, as research settled it

Written before `pnr.c`, from reading nextpnr-ecp5 0.6's
`ecp5/bitstream.cc` -- the function that turns a placed, routed design
into a `.config`. Section 3.2's method still holds: nextpnr is the
reference, and the test is byte identity.

### 12.1 The netlist is physical

§5's plan called for "a textual netlist with explicit sites and explicit
arcs". Reading `bitstream.cc` sharpened what *explicit* has to mean,
because two things nextpnr writes depend on routing, not on the logic:

- **LUT inputs are permuted after routing.** `permute_lut()` rewrites
  each LUT's INIT for the physical pins the router used, and every
  unused physical pin gets `SLICEx.<pin><lc>MUX 1`.
- **Flip-flop clock and reset settings depend on which tile wire a net
  landed on.** Each slice takes its clock from the tile's `CLK0` or
  `CLK1` through its own mux (`MUXCLKn`, a fixed connection in `PLC2`),
  and `CLKk.CLKMUX` is written when the net on wire `CLKk` equals the
  flip-flop's clock net. Likewise `LSRk`. That comparison has a quirk
  worth reproducing on purpose: an unused wire and an unconnected reset
  are both *no net*, so a tile whose flip-flops have no reset gets
  `LSR0` and `LSR1` settings anyway -- visible in every blinky config.

So the `.zn` netlist states physical facts -- the INIT in physical pin
order, the pins actually used, the wire each flip-flop's clock and reset
arrive on -- and `zfpga pnr` translates. Choosing pins and permuting is
the router's job, in Phase 4. That keeps Phase 3 a pure, checkable
translation.

### 12.2 The format

```
device LFE5U-25F
package CABGA256
comb R2C5 SLICEB 1  mode=CCU2 init=0x000E pins=AD inject=NO
ff   R2C5 SLICEA 0  clk=CLK0@clk48 lsr=- gsr=DISABLED sd=1
io   A2  dir=OUTPUT type=LVCMOS33
io   A7  dir=INPUT  type=LVCMOS33
dcc  R0C31 TDCC0
arc  R2C5:PLC2 A0 H02W0701
```

One cell per line, `key=value` options with nextpnr's defaults
(`mode=LOGIC`, `gsr=ENABLED`, `sd=0`, `regset=RESET`, `lsrmode=LSR`,
`cemux=1`, `clkmux=CLK`, `srmode=LSR_OVER_CE`, `lsrmux=LSR`,
`inject=YES` in CCU2 mode; IO `hysteresis=ON` on inputs). `clk=` and
`lsr=` give the tile wire and a net name after `@`, or `-` for none.
IO sites are package pins, or `R0C4.A` form. Arcs are Trellis
tile-relative, exactly as in `.config`; Phase 3 has no router.

### 12.3 What `zfpga pnr` writes, in nextpnr's words

| From | Writes |
|---|---|
| the baseline | 179 settings nextpnr applies before any design (four global-clock mux arcs, DCU/PLL/EFB tie-offs, nine unknown bits). Vendored as data in `ext/nextpnr-base/`, embedded in the `.zdb` |
| `comb` | `SLICEx.MODE`, `SLICEx.K<lc>.INIT`, `SLICEx.CCU2.INJECT1_<lc>` (`_NONE_` outside CCU2), `SLICEx.<P><lc>MUX 1` for each unused physical pin |
| `ff` | `SLICEx.GSR`, `.REG<lc>.SD`, `.REGSET`, `.LSRMODE`, `.CEMUX`; `LSRk.SRMODE`/`LSRMUX` and `CLKk.CLKMUX` by the net comparison of §12.1 |
| `io` | `PIOx.BASE_TYPE <DIR>_<TYPE>` in both the PIO and PIC tiles; for an output with no tristate net, the CIB tie `CIB.J??MUX 0`; `HYSTERESIS` on inputs; SLEWRATE, PULLMODE, CLAMP, DRIVE, OPENDRAIN when given |
| all `io` together | `BANK.VCCIO` in each `BANKREF<n>` tile whose bank has a non-input IO |
| `dcc` | nothing without a clock enable (nextpnr's `write_dcc()`); with one, a tile group -- refused in Phase 3 |
| `arc` | the arc, verbatim |

Single-ended LVCMOS and LVTTL only: differential, referenced and
terminated IO types, DRAM mode, and everything that is not a slice, a
PIO or a clock buffer are refused by name.

### 12.4 Done in this round: the database knows the IO

`.zdb` is now **version 2** (a v1 file is refused with instructions), with
three new sections:

- **PIO**: every PIO site with its PIO tile, PIC tile, bank, and
  tristate-tie target. The PIO/PIC side rules are nextpnr's, computed by
  `mkzdb.py` so the device never carries them. The tie target is the CIB
  mux feeding `JPADDT<L>`, found as a fixed connection in the PIC tile's
  database and named *relative to the PIO's own location* -- `S1_JB0`
  from a top PIO at R0C4 is `CIB_R1C4 JB0`, which is exactly the
  `CIB.JB0MUX 0` in nextpnr's blinky config. **[corrected]** This said
  *all 197 PIOs on the 25F resolve*; at the time, every top-edge PIOB did
  not, and the build said so only under `-v`. Fixed, and all 197 now do
  -- §12.7.
- **PIN**: package pin names to PIOs, all packages -- the `.lpf` step in
  Phase 5 needs this too.
- **BASE**: the embedded baseline.

The 25F database is 1.34 MB. `tests/run.sh` still passes 34 of 34.

That tie resolution is also the first piece of §6.1's name algebra to
exist: direction prefixes (`N`, `S`, `E`, `W` with counts, combinable as
`S1E1`) applied to a base location. It is enough here because the tie is
one fixed hop. The general case -- which *end* of a span wire drives,
and the array edges -- is still Phase 4's.

### 12.5 How it will be tested

`nextpnr-ecp5 --write` saves the placed, routed design as JSON: each
cell's site (`NEXTPNR_BEL`), parameters and attributes. A host
converter, `tools/np2zn.py`, will turn that plus nextpnr's own `.config`
into a `.zn`, taking from the config only the physical facts of §12.1
(arcs, and so which pins and wires were used, and the physical INIT).
Then:

```
zfpga pnr blink.zn -o z.config
zfpga pack z.config  -o z.bit
zfpga pack blink.config -o np.bit      # nextpnr's own config
cmp z.bit np.bit
```

Everything `zfpga pnr` derives itself -- modes, unused-pin muxes,
flip-flop and clock settings, IO types in two tiles, ties, bank
voltages, the baseline -- is then checked against nextpnr, for the
blinky and for the DFU bootloader (IO on several sides of the die).
The three rungs of §5.3 get hand-written `.zn` files beside them.

### 12.6 What actually happened

**It works, and it matches.** `zfpga pnr` exists (`pnr.c`, 651
lines), with `tools/np2zn.py` on the host, and three designs are
byte-identical to nextpnr once packed:

| | What it covers |
|---|---|
| `examples/blink.zn` | the blinky, converted from nextpnr: CCU2 carries, flip-flops, the global clock buffer, both IO directions, the baseline |
| `examples/on.zn` | the LED on -- **written by hand**, one LUT, one pin, four arcs, each explained in the file |
| `tests/fixtures/sides.zn` | IO on all four sides of the die, a 2.5V bank beside 3.3V ones, a real tristate (BIDIR), pull-up and pull-down, slew rate, drive strength, open drain, hysteresis off, asynchronous reset and clock enable |

Rung 1 (`examples/empty.zn`) has no nextpnr reference -- nextpnr
refuses a design with no cells -- and is checked to be exactly the
baseline. Twelve malformed netlists are refused with a line number and
no output. `tests/run.sh` is now 50 cases; `tests/run_dev.sh` runs
`pnr` on the device build under `sim/` too, and its output equals the
host's: 1.8M instructions for `on.zn`, 2.9M for the blinky.

So the three rungs of §5.3 exist as files, each proven to produce the
bitstream nextpnr would. What they have not done is light an LED
(§12.8).

### 12.7 Two findings

**The tie table was wrong for every top-edge PIOB, and said so only
under `-v`.** A top PIO pair shares one site, and PIOB's PIC tile is
one column right -- but its `JPADDTB` fixed connection is listed in
PIOA's tile, not its own. The first table searched only the PIO's own
tiles, left every top PIOB without a tie, and reported that only in
verbose output, so the claim in §12.4 that "all 197 PIOs resolve" was
made from a quiet run. `zfpga pnr` refused the first top PIOB output
it met, by name, which is how it was found; the search now covers the
PIC and PIO tiles around the site, and any unresolved PIO is reported
on every build. After the fix all 197 do resolve, and the four-sides
test is what checks that the resolution is *right*, not just present.

**The refusals caught a bug in `zfpga pnr` itself.** A buffer one byte
short truncated `.K0.INIT` to `.K0.INI`; the packer refused the word
by name and line rather than producing a bitstream. That is the
argument of §4.2 working in the direction nobody plans for: strict
input checking in the second stage is a test of the first.

### 12.8 What is not verified

- **Any rung on a board.** **[closed, §23: the blinky, which exercises every rung's layers, runs on a Lakritz]** Deferred by decision. The chain to do it:
  `zfpga pnr on.zn`, `zfpga pack on.config -c`, then onto a second MMOD
  with `sw/apps/mmod` (§2.9) or a host. LED polarity on Lakritz is
  unconfirmed; `on.zn` says what to change.
- **Designs outside what was tested.** The rules are nextpnr's, but only
  exercised on these three. LUT permutation, DPRAM, EBR, DSP, PLLs, DDR
  IO, differential and referenced IO, and clock buffers with an enable
  are refused rather than guessed.
- **A hand-written route longer than four arcs.** Writing arcs needs the
  names of wires in neighbouring tiles; `on.zn` shows it is possible for
  a short hop, and Phase 4 is what makes it unnecessary.

### 12.9 Next: Phase 4

The router. Its first task is still §6.1's: the relative-name resolver,
validated by reconstructing every net in nextpnr's configs -- now with
three designs whose correct routing is known. The resolver used for the
tristate ties (direction prefixes from a base location) is its seed.

## 13. Phase 4: the resolver and router v1

### 13.1 The schedule risk, retired

§6.1 named the relative-name algebra as the part most likely to burn
time, because nothing documents it. It turned out to be twenty lines,
read out of libtrellis's `RoutingGraph::globalise_net_ecp5()`:

- a tile's location is the `R<row>C<col>` in its **name** -- no special
  cases on ECP5;
- an optional `N`/`S` offset, then an optional `E`/`W` offset, then `_`,
  move the location (N up, S down, W left, E right);
- `G_`, `L_` and `R_` names are globals: `G_` ones (bar VPTX, HPBX,
  HPRX) at (0,0), the rest at the tile;
- `25K_`/`45K_`/`85K_` prefixes apply only to that die; from column 69,
  `PCSA` reads as `PCSB`; anything off the array is no wire.

`tools/resolvecheck.py` implements it in Python and checks it against
nextpnr, whose routed JSON records every pip it used as global wires.
Three checks, all exact: every arc in nextpnr's `.config` resolves to a
pip nextpnr used; every configurable pip nextpnr used is a resolved
database arc; every fixed pip is a resolved fixed connection, apart
from LUT-permutation pseudo-pips, which are nextpnr's invention. **On
the blinky, LED-on, the four-sides design and the DFU bootloader --
16,342 arcs in the last alone, global clock routing and EBR included --
there is no exception.** `route.c`'s `resolve()` is the same rules in C.

One earlier guess is corrected by it: §12.4 described the tristate-tie
names as relative "to the PIO's own location". They are relative to the
listing tile's name location like everything else; the tie connection
lives in `PIOT0`, which sits at the PIO's row, which is why the guess
gave right answers.

Resolved, the 25F is **7,747,276 configurable arcs and 452,217 fixed
connections** -- §2.4's "7.6 million edges if instantiated", measured.

### 13.2 Never building the graph

What a wire drives is found on demand. The whole database uses only a
few dozen distinct offsets in source names (`N1`, `E3`, `S13E2`, ...),
so for a wire at (r, c) the router asks, per offset, "the tile that
would see this wire under that prefix -- what does it drive from it?",
through a per-tile-type index of arcs and fixed connections by source
name, built the first time a type is met. Memory is the wires one search
touches.

### 13.3 Nets by their pins, and fixed connections

A net is declared by its terminals as global wires, and a flip-flop
leaves its clock wire to the router:

```
net n2050 R3C40/Q0_SLICE R0C53/PADDOB_PIO R3C40/B0 R3C42/D1
ff  R3C40 SLICEA 0 clk=?@n2128 ...
```

**Terminals are the cells' own pins, and routes cross fixed
connections** to reach them: a carry chain is nothing but fixed hops, an
output reaches its pad through `JA0 => JPADDOA => PADDOA_PIO`, a clock
enters a slice through `MUXCLKn => CLKn_SLICE`. A first version drew
terminals at the ends of the configurable arcs; the four-sides design
showed that an input's route crosses a configurable IOLOGIC arc, then a
fixed hop, then general routing, so no such terminals exist. `.zdb`
became **version 3**, carrying each tile type's fixed connections
(1.49 MB for the 25F).

The router writes an arc for each configurable hop and nothing for a
fixed one, and afterwards reads each flip-flop's clock and reset wire
back from the arc it chose into the slice's mux.

### 13.4 What v1 is

A* per net, sink by sink, each search starting from every wire the net
already owns; unit cost per wire, Manhattan distance to the sink as the
heuristic; a window of the terminals' bounding box plus four tiles. Every
terminal of every net is claimed before any net is routed, so no route
passes through another net's pin. A wire belongs to one net. **Global
wires are excluded:** a clock is routed over general fabric -- PLC2's
clock muxes accept it -- and `np2zn.py --route` drops nextpnr's clock
buffer and merges its two nets. That is right for small designs and no
others. There is **no rip-up**: a net that cannot get round earlier ones
fails by name.

### 13.5 How it is checked

Routes differ from nextpnr's, so byte identity is not the test.
`tools/routecheck.py` -- independent of `route.c`, sharing only the
resolver 13.1 proves -- checks what makes any routing correct: every
sink reachable from its source through the arcs and the database's
fixed connections; no wire driven by two arcs; no net's source driven
by one; no arc-driven wire reached by two nets.

| Design | Nets | Arcs (nextpnr's) | Legal |
|---|---|---|---|
| LED on | 1 | 3 (4) | yes |
| blinky | 63 | 94 (94, plus the global clock) | yes |
| four sides | 19 | 188 (203) | yes |

LED-on is shorter than both nextpnr's route and the hand-written one: it
found the one-hop neighbour wire `V01N0101` straight into `JA0`.

**One bug, found by that test.** The offsets to search were first taken
from configurable arcs only. A top PIOB's data arrives over PIOT0's
fixed `JPADDOB <= S1E1_JA0`, and no configurable arc anywhere uses
`S1E1` -- so that pin was unroutable, and said so by name. Fixed
connections now contribute offsets.

### 13.6 What it costs, and what that says

The device build routes identically to the host (`tests/run_dev.sh`),
and slowly:

| | Instructions | at ~12 MIPS |
|---|---|---|
| blinky, 63 nets | 206M | ~17 s |
| four sides, 19 nets | 401M | ~33 s |

The cause is visible in the code rather than guessed: each expanded wire
tries every offset, formats a relative name with the general formatter,
and binary-searches it by string -- tens of thousands of instructions
per wire, over 6,000-12,000 wires. Hashing on (offset, interned base
name) removes the formatting and the string compares; that is the first
thing Phase 4 does next, measured the same way.

### 13.7 Not verified, and next

- **Any routed design on a board** -- deferred with everything else. **[closed, §23]**
- ~~**Congestion.**~~ Done in v2, §14.
- **Global clock routing**, for anything past a few dozen tiles.
- ~~**Speed**, 13.6.~~ 4-7x in v2, §14.1.
- LUT pin permutation (routing to any free input and rewriting the
  INIT) -- an easy win for routability once congestion is handled.

## 14. Phase 4: router v2

### 14.1 Speed: the profile, then the fix

Before changing anything, callgrind on the four-sides design: of 440M
instructions, **35% was the formatter and 24% was string comparison** --
every expanded wire had its relative name printed for every offset in
the database and binary-searched by string, as 13.6 suspected. v2 parses
every database name once into (row offset, column offset, interned
base), keys each tile type's index by those integers, and keeps for each
base name only the offsets it actually appears under (a handful, not
53). Each net also keeps its own tree instead of the search rescanning
every wire it has ever seen.

| | v1 | v2 |
|---|---|---|
| host, four sides | 440M | 61M |
| device, blinky | 206M (~17 s) | 45M (~3.7 s) |
| device, four sides | 401M (~33 s) | 99M (~8 s) |

### 14.2 Negotiated congestion

v2 is PathFinder (McMurchie and Ebeling, 1995). Every net is routed; nets
may share a wire, at a cost of `(1 + history) x (1 + present x
sharers)`; after each iteration the present factor doubles and every
wire still shared gains history cost permanently; only nets that touch a
shared wire are ripped up and rerouted; it ends when nothing is shared,
or fails by name after fifty iterations. Costs are integers in
sixteenths, because the CPU has no FPU and soft-float would cost more
than the routing. A tight search window is tried first and a wide one
only if that fails. Terminals stay exclusive to their nets throughout.

### 14.3 A design that needs it

`examples/dense.v`: a 64-bit LFSR, a 16-bit accumulator, an 8x8
multiplier built from LUTs, and XOR reductions -- 346 LUTs, 96
flip-flops, 26 IOs, placed compactly by nextpnr. The resolver is exact on
nextpnr's routing of it (3,040 more arcs). Then:

| | Nets | Arcs (nextpnr's) | Iterations | Reroutes | Legal |
|---|---|---|---|---|---|
| LED on | 1 | 3 (4) | 1 | 1 | yes |
| blinky | 63 | 94 (94) | 1 | 63 | yes |
| four sides | 19 | 189 (203) | 2 | 22 | yes |
| **dense** | **424** | **2,957 (3,040)** | **4** | **527** | **yes** |

Four iterations and 103 reroutes past the first pass is congestion being
negotiated, on a real design, to a legal result that uses fewer arcs than
nextpnr's (which also routes the clock over the global network).

Its clock reaches 48 slices, more sinks than fit on a line, so the
netlist gained a continuation: `sinks NAME W W ...` adds sinks to a
declared net.

### 14.4 What it costs now

| On the device | Instructions | at ~12 MIPS |
|---|---|---|
| blinky, 63 nets | 45M | ~3.7 s |
| four sides, 19 nets | 99M | ~8 s |
| dense, 424 nets | 620M | ~52 s |

The dense profile has no single hotspot left: wire lookup 18%, the heap
8%, the search loop the rest. A further large gain would be structural
-- caching each tile entry's neighbour, or a weighted heuristic -- not a
fix, so it waits until a real design says a minute is too long.

### 14.5 Not verified, and next

- **Any routed design on a board**, still deferred. **[closed, §23]**
- **Global clock routing.** The dense design's clock reaches 48 slices
  over general routing; that is legal and slow to route, and on a
  larger design would cost timing nobody is measuring.
- **LUT pin permutation**, for routability.
- **Phase 5, the placer.** Every design so far is nextpnr's placement.

## 15. Phase 5: the placer, and proof it computes the design

### 15.1 The logical netlist

The placer's input is what synthesis produces: cells with no sites, on
named nets. `.zl` mirrors the primitives yosys's `synth_ecp5` emits, so
that a host converter can feed it real designs now and `zfpga synth`
(Phase 6) has a defined target:

```
device LFE5U-25F
package CABGA256
input  clk A7 net=n2 io_type=LVCMOS33
output led A2 net=n9
lut  $abc$123 init=0x00FF a=0 b=0 c=0 d=n7 z=n19
ff   $auto$45 d=n16 q=n10 clk=n2 ce=n4 lsr=n3 regset=RESET srmode=ASYNC
```

`tools/ys2zl.py` converts a yosys netlist and an `.lpf`. v1's subset is
what `synth_ecp5 -noccu2 -nowidelut -nodsp -nobram -nolutram` leaves:
LUT4 and TRELLIS_FF.

### 15.2 Packing

- **A flip-flop shares a logic cell with the LUT that drives it**:
  `DI<k>` is hard-wired from `F<k>` (PLC2's fixed connections), so the
  pair needs no routing, `SD=1`. Any other flip-flop's data comes in
  through `M<k>`, `SD=0`.
- **Two cells make a slice**, and a slice's two flip-flops share clock,
  clock enable, reset and those muxes' settings, so cells pair only when
  those match.
- **Constant LUT inputs are folded into the INIT.** An input nothing
  drives is tied to 1 by `zfpga pnr`, so the INIT is rewritten to give,
  at 1, what the input's constant gave.

### 15.3 Placing

IO goes where the netlist says. Each slice starts at the free site
nearest the centre of the IO it touches, then simulated annealing
minimises total half-perimeter wirelength, the move window shrinking as
it cools. A tile holds at most two distinct clocks and two distinct
resets -- PLC2 has `CLK0`/`CLK1` and `LSR0`/`LSR1` -- and no move may
break that. Everything is integer, from a seeded generator, with
acceptance `exp(-delta/T)` read from a table: the CPU has no FPU, and the
device must place exactly as the host does (`tests/run_dev.sh` checks
it).

A profile found two thirds of placement in recomputing wirelength, most
of it rescanning every IO for each net; IO never moves, so each net's IO
box is now computed once. Host cost fell from 495M to 341M instructions
on the dense design, with an identical placement.

### 15.4 The functional check: `tools/simcheck.py`

Routing legality is not correctness. A wrong constant fold, a swapped
pin, the wrong data path or a mis-set reset would all route legally,
pack, and do the wrong thing on a board nobody is testing on. So:

1. **Extract** the circuit from the final `.config` alone -- every LUT
   from its INIT word and its tied inputs, every flip-flop from its
   settings, every connection by tracing each input pin backwards
   through the configured arcs and the database's fixed connections,
   resolved by the rules §13.1 proves exact against nextpnr.
2. **Simulate** that and the reference netlist side by side, cycle by
   cycle, from the same random inputs; compare every output every cycle.

It shares no code with `place.c` or `pnr.c`, and it was checked to be
able to fail: flipping one INIT bit in the dense design's config, or
moving one arc to another source, is reported within nine cycles.

**It found a bug in itself first.** Every CIB input wire has a second
fixed driver, a `*_CIBTEST` Lattice test path, so the backwards trace
stopped at every input pad and reported outputs stuck at 0. Test paths
are now ignored. `zfpga` was right; the checker was wrong; the mismatch
said so on the simplest gate in the design, `y = a ^ b`.

### 15.5 Results

From Verilog, through yosys and `ys2zl.py`, then `zfpga place`,
`zfpga pnr` and `zfpga pack` -- no nextpnr anywhere:

| Design | Placed | Wirelength | Routed | Functional check |
|---|---|---|---|---|
| fast blinky (`blinkf.v`) | 4 LUTs, 4 FFs, 2 slices | 55 -> 29 | legal | equivalent; LED toggled 249 times in 2,000 cycles |
| four sides, no tristate (`sidesp.v`) | 6 LUTs, 4 FFs (async reset, enable) | 359 -> 288 | legal | equivalent; outputs changed on 1,639 cycles |
| dense (`dense.v`) | 224 LUTs, 96 FFs, 144 slices | 12,399 -> 1,359 | legal, 337 nets | equivalent; outputs changed on 1,993 cycles |

The full blinky (24-bit counter) also places, routes legally and is
equivalent, but its LED does not change within any practical number of
simulated cycles, which is why the suite uses the fast one.

### 15.6 What it costs

| On the device | Instructions | at ~12 MIPS |
|---|---|---|
| place fast blinky | 4.6M | 0.4 s |
| place dense, 144 slices | 369M (551M before 15.3) | ~31 s |
| route dense, 337 nets | comparable to §14.4's 620M for 424 | ~50 s |

### 15.7 Not verified, and next

- **On a board**, still deferred. **[closed, §23]** What simcheck establishes is that the
  configuration encodes the design, given that the database's meaning of
  each bit is right -- which ecppack's byte identity and nextpnr's own
  hardware use support, but which only a board can close.
- ~~**Carry chains (CCU2).**~~ Done, §16.
- Tristate and bidirectional IO; multiple clock domains per tile pair;
  wide LUTs; distributed and block RAM.
- **Phase 6, `zfpga synth`**, which writes `.zl` on the device and
  closes the chain from Verilog with no host at all.

## 16. Phase 5: carry chains

### 16.1 How nextpnr starts a chain

yosys emits a chain head with `CIN` the constant 0 and `INJECT1_0=NO`, so
the carry into the first slice matters -- and in hardware it arrives over
the fixed chain from whatever is to the west. nextpnr's answer, read from
`ecp5/pack.cc` (`make_carry_feed_in`), is a **feed-in slice** ahead of every
chain: `INIT0=0x000A`, `INIT1=0xFFFF`, `INJECT1_0=NO`, `INJECT1_1=YES`.
Worked through yosys's `CCU2C` model with the unused inputs tied to 1:
the first half's LUT4 reads entry 14 or 15, both 0, so it **gates the
incoming hardware carry off entirely**, and its LUT2 (`INIT[3:0]` on A and
B) makes `A0` the carry; the second half passes it through. So every
chain starts clean, whatever its neighbour. For a constant carry in, the
tied pins alone do it: `INIT0=0x0008` for 1, `0x0000` for 0.

A chain whose last carry out feeds logic ends with nextpnr's **feed-out**:
`INIT0=0`, `INJECT1_0=NO`, so its sum is its carry in, and that sum drives
the logic.

### 16.2 What the placer does

- Chains are found by following each carry out to the next carry in. A
  carry out that also feeds logic in the middle of a chain is refused in
  this version (nextpnr loops it through a LUT; not yet needed).
- A chain is **a rigid block**: feed-in, carry slices, feed-out if any,
  starting at slice A and running east through the fixed `FCOA => FCIB =>
  ... => FCO => HFIE => FCI` wiring, owning whole tiles in consecutive
  columns of one row. It is placed first and moves whole, only into free
  space; single slices never move into it.
- **Flip-flops pack onto carry outputs** as they do onto LUTs: a
  flip-flop whose data is a carry half's sum shares its logic cell,
  `SD=1`. The 24-bit blinky is 24 flip-flops and 12 carry cells in **13
  slices**; without carry logic it was 31 LUTs in 16.

**The constant fold had to become safer.** In carry mode a half's LUT2
reads `INIT[3:0]` whatever C and D are. The old fold rewrote every INIT
entry for a tied pin, which for a constant-1 C or D would have clobbered
the LUT2 half. It now rewrites only the entries the tied pin can reach.

### 16.3 Checked, including the checker

`simcheck.py` models `CCU2C` exactly as yosys's `cells_sim.v` does, on
both sides, and on the extraction side follows each slice's carry in back
through the fixed chain to whichever slice's carry out drives it. Before
trusting it on zfpga's output, it was run on **nextpnr's** carry-chain
implementation of the fast blinky -- feed-in and all -- and found it
equivalent to yosys's netlist. Then:

| Design | Carry | Result |
|---|---|---|
| fast blinky | constant feed-in, FFs on sums | equivalent; LED toggled 249 times |
| blinky (24-bit) | 12 carry cells, 24 FFs, 13 slices | legal, equivalent |
| dense | 2 chains beside 175 LUTs | equivalent; outputs changed on 1,993 cycles |
| **adder** `{co,s} = a+b+ci` | **carry in from a pin**: feed-in routes `ci` to `A0` | equivalent; 1,995 |
| **comparator** `a < b` | **carry out to logic** through a feed-out | equivalent; 1,255 |

A mutation -- one carry half's `INJECT1_1` flipped from NO to YES -- is
reported within twenty cycles.

On the device the placer puts carry chains exactly where the host does
(`tests/run_dev.sh`). It is not yet cheap about it: placing the 24-bit
blinky -- one chain, 13 slices -- costs 173M instructions (~14 s), most
of it the chain's initial search over every head position on the die
and the lift-and-replace of each chain move. Both are obvious to narrow
(search outward from the target; reject a move before lifting); neither
is done yet.

### 16.4 Not verified, and next

- **Chain placement cost**, above.

- On a board, as ever. **[closed, §23: the blinky is a carry chain]**
- Carry used by logic mid-chain; chains longer than a row.
- Tristate IO, wide LUTs (`PFUMX`/`L6MUX21`), distributed and block RAM.
- **Phase 6: `zfpga synth`**, which writes `.zl` on the device.

## 17. Formats for working by hand

Every intermediate format is text -- `.zl`, `.zn`, `.config` -- and
`docs/zfpga-formats.md` is their reference, written for someone at a
`posix` prompt with the text editor open. Three gaps kept "editable" from
meaning "comfortable to edit", and this round closed them:

- **LUTs as expressions.** `lut inc3 func=a^(b&c&d) a=q3 b=q2 c=q1 d=q0
  z=d3` instead of a 16-bit INIT worked out by hand. A small recursive-
  descent parser in `place.c`; `simcheck.py` has its own, independently,
  so the functional check does not trust the one it is checking.
- **Placement constraints in the logical netlist.** `loc=R2C4`,
  `R2C4.B` or `R2C4.B1` on any cell, or on a chain's first cell; the
  placer puts it exactly there and never moves it, and refuses by name
  and line a `loc=` that names a non-logic tile, overfills a slice, or
  forces together flip-flops that cannot share one.
- **Placement written back.** `zfpga place d.zl -l placed.zl` writes the
  input again with `loc=` on every cell. Edit and re-place: fully
  automatic, partly pinned, or fully manual are the same workflow. Moving
  a cell in the `.zn` instead would mean rewriting every net terminal
  that names it, which is why placement lives in the `.zl`.

And to write `loc=` without a map: `zfpga info LFE5U-25F R2C4` lists
the tiles at a location and says which take logic.

Checked:

| | |
|---|---|
| `examples/hand.zl` | a 4-bit counter on the LED, written by hand with `func=`, two flip-flops pinned; they land exactly where pinned, the rest beside them; routed legally, **equivalent**, LED toggling 249 times |
| round trip | the dense design placed with `-l` (273 `loc=` written back: 175 LUTs, 96 flip-flops, 2 chain heads), then the written file placed again: **zero moves, identical `.zn`** |
| refusals | six malformed `.zl` files, each refused with file and line |

`zfpga unpack` -- a bitstream back to `.config`, so that `.bit` joins
the editable formats -- followed, §18.

## 18. `zfpga unpack`: bitstreams back to text

`zfpga unpack design.bit` writes the `.config` for any ECP5 bitstream.
It is `ecpunpack` re-expressed, and `tests/run.sh` requires its output to
be the same **text**, byte for byte, which pins down every choice in
reading a tile back. They are libtrellis's `tile_cram_to_config()`
exactly: a mux's driver is the matching arc with the most bits, a later
one winning a tie, and bitless arcs are never printed; a word is printed
only if it is not its default; an enum is omitted if its bit group
*equals* the default's -- a comparison of groups, not names -- and is
`_NONE_` if nothing matches and a default exists; every set bit no
setting accounts for is `unknown:`, where what a setting accounts for is
only the bits it expects to be 1.

One detail worth keeping: libtrellis's own comment on its decompressor
has the `100` (one-hot) and `101` (dictionary) codes the wrong way
round. Its code, its encoder and ecppack's output all agree on the
other reading, which is what zfpga follows and says so.

| | |
|---|---|
| text identical to ecpunpack | blinky plain, compressed, with boot address, QSPI, usercode, background; the DFU bootloader (26,959 lines, EBR included); the edge-case design; empty 25F and 12F; blinkies on the 45F and 85F |
| damaged bitstream | one byte changed mid-frame: refused by CRC, nothing written |

**What a `.config` cannot say.** A bitstream carries its compression,
SPI clock, multiboot flag and address, usercode and SPI mode, and
`ecpunpack` drops them -- so an edit-and-repack loop would quietly make
a different bitstream, possibly one that no longer boots from the
address the flash layout expects. `zfpga unpack` keeps its file
identical to ecpunpack's and also prints the `zfpga pack` command that
restores them all, reading the multiboot address back from the
`BOOTADDR` word by value, the way `pack` writes it. Every combination
tested -- compression, each clock rate, multiboot at 0 and elsewhere,
usercode, each SPI mode, background reconfiguration, and all of them at
once -- rebuilds byte for byte from the printed command, and
`tests/run.sh` runs that command on every case.

**On the device** the text is the host's (`tests/run_dev.sh`). The first
run cost ~870M instructions for a blinky: every arc of every mux of all
4,312 tiles tested bit by bit, for a design that configures about fifty.
A tile's text depends only on its type and its bits, and nearly all
tiles repeat a handful of patterns -- all zero, or their type's defaults
-- so each type keeps the last few patterns seen, with their text. The
cache first made things *worse* (242M): building each tile's pattern one
`chip_get_bit` at a time cost more than it saved. Reading a tile's row
of a frame as one word, three bytes and a shift, fixed that:

| On the device | Instructions | at ~12 MIPS |
|---|---|---|
| unpack blinky | 870M -> **77M** | ~6 s |
| unpack blinky, compressed | 60M | ~5 s |
| unpack DFU bootloader | 890M -> **226M** | ~19 s |

## 19. Phase 6: `zfpga synth`

```
zfpga synth blink.v -l /fpga/lakritz.lpf     # -> blink.zl
zfpga place blink.zl && zfpga pnr blink.zn && zfpga pack blink.config -c
```

Verilog in, a `.zl` out -- the file `docs/zfpga-formats.md` describes,
with the Verilog's names on its nets (`ctr[3]`, not `$abc$1234`), so the
next thing a person does with it can be to read it. The release puts the
Lakritz constraint file on the card as `/fpga/lakritz.lpf`; the tree's
own `boards/lakritz_v0.lpf` is read as it stands, its `BLOCK` and
`FREQUENCY` statements ignored.

### 19.1 The subset

**[superseded by §24]** -- instances, `for`, functions, `signed`,
`integer`, `*` and the preprocessor are now supported; §24.1 has the
current subset and `docs/zfpga-formats.md` section 8 the reference.

One module; ANSI or old-style ports; `wire` and `reg` vectors;
`parameter` and `localparam`; `assign`; `always @(posedge clk)`, with
`or posedge/negedge rst` for an asynchronous reset; `always @(*)`; `if`,
`else`, `case`; every operator but `*`, `/`, `%` and `**`; constant and
variable bit selects and shifts; part selects; concatenation and
replication; `reg` initial values. Refused by name and line: module
instances, `generate`, `for`, functions, tasks, `initial`, `inout`,
`signed`, `x` and `z`, `*`, `/`, `%`, latches, combinational loops,
signals with two drivers, blocking assignment in a clocked block.

### 19.2 How it works

- **Three files**: `synth_parse.c` (lexer, recursive descent, constant
  expressions), `synth_elab.c` (a bit-level gate graph),
  `synth_map.c` (LUT mapping, pins, the `.zl`).
- **Widths by context, exactly.** An expression is evaluated *at* the
  width it is assigned to. For `+ - & | ^ ~ <<` the low bits of a result
  depend only on the low bits of the operands, so this is the same as
  Verilog's widen-then-truncate -- and it is what makes `{co, s} = a + b`
  keep its carry and `ctr + 1` a 24-bit counter rather than a 32-bit one.
  Comparisons, `>>` and concatenation are self-determined, per the
  standard.
- **Gates are hash-consed with constants folded**, so equal logic is
  one gate and `x & 0` never exists.
- **`always` blocks run symbolically.** A clocked block reads old
  values; `always @(*)` reads its own assignments in order, and a
  variable not assigned on every path is refused as a latch. `case` is
  an if-chain, first match winning. A top-level `if (rst)` in a block
  with an asynchronous reset gives the reset values; `next = en ? x : q`
  becomes a clock enable.
- **Arithmetic is carry chains.** `+`, `-` and `< <= > >=` become `ccu2`
  cells. Tying each half's `D` input to 1 separates the half's LUT4
  (INIT bits 8-15) from its LUT2 (bits 0-3): propagate in one, generate
  in the other -- `0x060A` to add, `0x090A` to subtract, `0x0100` for a
  padding half that passes the carry through. A comparison is the carry
  out of a subtraction.
- **LUT mapping by priority cuts**: each gate keeps its best few cuts of
  at most four inputs by depth then area flow; the cover runs from the
  outputs, flip-flop inputs and carry operands down; each LUT's INIT is
  its cone simulated over sixteen patterns.
- **Dead logic is swept**, bit-precisely through carry chains: a sum bit
  needs only the operand bits below it. The blinky with an 8-bit counter
  whose LED reads bit 3 synthesises to 4 flip-flops and 2 carry cells --
  what yosys makes of it.

### 19.3 Checked against yosys

`tools/simcheck.py` now compares netlists as well as configurations:
given a `.zl` where it would take a `.config`, it simulates it against
the reference directly. yosys is the reference for what the Verilog
means; its netlists are stored as `.zl` fixtures (through `ys2zl.py`),
so the suite does not need yosys to run. For each design, two checks --
zfpga's `.zl` against yosys's, then zfpga's *bitstream* (through `place`,
`pnr`, `pack`) against yosys's:

| Design | zfpga synth | yosys | Both checks |
|---|---|---|---|
| fast blinky | 4 FFs, 2 carry cells | 4 FFs, 2 CCU2C | equivalent |
| blinky, 24-bit | 24 FFs, 12 carry cells | 24 FFs, 12 CCU2C | equivalent |
| `features.v`: the whole subset -- `case` in `always @(*)`, a state machine, active-low async reset, enable, initial value, `{c,s}=a+b+ci`, comparisons, variable shifts and index, reductions, replication | 48 LUTs, 11 FFs, 6 chains | 51 LUTs, 10 FFs, 3 CCU2C | equivalent on all 43 ports, output changing on 4,999 of 5,000 cycles |
| four-sides counter | 2 LUTs, 4 FFs, 1 chain | | equivalent |
| adder, comparator | carry in from a pin; carry out to logic | | equivalent |
| `densen.v`: 96 FFs of LFSR and accumulator | 39 LUTs, 96 FFs, 1 chain | | equivalent |

Eleven malformed designs are refused with file and line, and none leaves
output behind -- which the suite checks, and which caught the first
version writing half a `.zl` before discovering a port had no pin.

### 19.4 The bug only the device could show

`tests/run_dev.sh` synthesises on the RV32 build and requires the host's
`.zl`. It differed: two LUTs swapped, INITs mirrored to match -- both
correct, not the same. The variable-index mux tree was built as
`g_mux(s, select_bit(hi), select_bit(lo))`; C leaves the order of
function arguments unspecified, GCC on x86 and on RV32 take them in
opposite orders, and so the two builds numbered gates differently. Every
call that builds gates in two arguments is now sequenced through
locals. The simulator is the only place this could have been caught
before a board: both outputs were correct, and only a comparison of the
two would ever notice.

### 19.5 What it costs

Synthesising `features.v` on the device: **1.7M instructions**, about a
seventh of a second. Synthesis is the cheap stage; placing and routing
are where the time goes (sec. 14.4, 15.6).

### 19.6 Not verified, and next

- **On a board**, still -- now the whole chain, from `vi` to LED. **[closed, §23]**
- **Module instances**, which most real designs use, and `for` loops.
  Flattening instances is the next thing the front end needs; it is
  what would let it read this tree's own RTL.
- `*`, block RAM inference, distributed RAM, tristate IO, wide muxes.
- Area: the feature design uses 6 carry chains where yosys uses 3 --
  `a < b` and `a >= b` are one subtraction, synthesised twice.

## 20. `zfpga build`, board profiles, and the first test on a board

```
$ zfpga build /fpga/examples/blink.v -b lakritz
```

One command from any starting point to a bitstream, with a board
profile supplying the device, package, pins and pack options
(`docs/zfpga-formats.md` sec. 7). The card's `/fpga` holds the
database, the Lakritz profile with the tree's own `lakritz_v0.lpf`, and
the examples; `sw/apps/zfpga/db/` on the build machine is laid out the
same, so the host's `-D db` is the card's `/fpga`.

### 20.1 One process, and the memory to do it in

Two things made this one process rather than four commands. posix's
`a && b` tests whether `a` *started* (`sw/apps/posix/sh.c`, `bi_run`),
so a chain of commands would race. And four stages in four processes
would each load the 1.5 MB database.

So the database is loaded once and cached, and each stage's own memory
is returned before the next (`zf_mark` / `zf_release`: every arena block
is on a list). Peak memory, measured on the device build:

| From | Peak | Instructions | ~at 12 MIPS |
|---|---|---|---|
| `blink.v` (synth to pack) | 3,152 KB | 236M | ~20 s |
| `hand.zl` (place to pack) | 2,757 KB | 61M | ~5 s |
| `on.zn` (pnr, pack) | 2,062 KB | 19M | ~2 s |

The kernel gives zfpga 4 MB for stack *and* heap, the heap growing up
towards the stack; 3.15 MB leaves room, and `tests/run_dev.sh` now
fails any build that peaks over 3.5 MB, so a change that would stop it
fitting on a board is caught in the simulator. Host figures run higher
(3,346 KB) -- 8-byte pointers -- which is why the budget is checked on
the device build.

### 20.2 The test

`docs/zfpga-test.md` is the procedure: install, build the three
examples on the Lakritz, write one to a spare MMOD with `mmod`, swap it
into the configuration socket, power on. It exists because every check
so far -- byte identity with ecppack, equivalence with nextpnr and
yosys, device against host in the simulator -- stops short of silicon.

Writing it caught a mistake in this document: §5.1 had the blinky's
LED at "a 0.7 second blink". The top bit of a 24-bit counter at 48 MHz
is high for 2^23 cycles, 0.17 s; the LED blinks about three times a
second. A test procedure that told someone to expect the wrong rate
would have produced a confused report of a correct result.

## 21. The first on-board run

`zfpga build /fpga/examples/blink.v -b lakritz` on a Lakritz printed
nothing for over a minute, on the terminal or the serial console, and
then the machine froze. The serial log, read character by character
where two processes' output interleaved, gave the order of events:

```
posix: stdout sink from pid 6 as conn 2, owner set
k_proc_kill: pid 6 marked to die (base=40973000)
```

`k_proc_kill` there is `z_exit()`'s (`sw/os/kernel.c`): **zfpga exited,
almost at once** -- while posix was still printing that it had accepted
zfpga's output connection. So zfpga failed immediately, and its error
message went nowhere.

### 21.1 Where the message went

A port message's payload is a blob in the *sender's* heap, read by the
receiver in place and freed by the sender on the acknowledgement
(`sw/common/zport.h`, the pending table). zfpga's `zio_out_close()`
closed the port and exited without waiting for acknowledgements. An
error at startup was therefore sent and then its memory released --
the whole process's memory -- before posix had even finished accepting
the connection; posix then read the message out of a dead process.
That loses the message, and reading freed memory is the likeliest cause
of the freeze. The port layer was copied from `zcc`'s, which has the
same exit path and has simply never failed that fast.

Fixed: `zio_out_close()` waits, bounded at ~2 s, until every send has
been acknowledged. And every fatal error is now also written straight
to the UART, so that a failure can never again be invisible.

### 21.2 What the immediate failure was

**[resolved]** The next run, with the message getting through, said
what it was: `no database for LFE5U-25F` -- the 8.3 name of section 22.

At the time: unknown, its message being what was lost. Two causes are likely, and both
now say so: a kernel without zfpga in the 4 MB tier -- zfpga now
checks at startup, before anything else, that it has the memory and
names the fix if not -- or a file missing from the card, which was
always reported by name and now reaches the console too.

### 21.3 Why the simulator did not catch it

Three differences between `sim/` and the machine, found here:

- **No posix.** Under the simulator zfpga is the only process, its
  output goes to the UART, and the port relay -- the path that failed
  -- had never run once.
- **No alignment traps.** PicoRV32 traps a misaligned load or store and
  this SOC does not handle the trap, so one would hang the machine; the
  simulator performs it silently. A copy of the simulator that stops on
  any misaligned access ran every device build: **none**. Worth adding
  to `sim/` as an option.
- **Uptime can run backwards.** `sim/simos.c`'s `simos_uptime_ticks()`
  subtracts `tv_nsec` fields without a borrow; zfpga's new stage timings
  showed a 1,079,965-second build. The kernel's counter is hardware and
  has no such bug; the simulator's wants a one-line fix.

### 21.4 Progress, and speed

The run also showed zfpga saying nothing during a long build. `build`
now announces each stage as it starts and reports its time.

And it was slower than it needed to be. Profiling the blinky's build
showed placement at 164M of 214M instructions, in two mechanical costs:
de-duplicating the nets a move touches by searching a list (85M) and
comparing flip-flop control settings as strings on every tile check
(37M). A per-net stamp and per-flip-flop control classes, worked out
once, removed both with **the placement unchanged** -- same wirelength,
same moves, the round-trip test still exact:

| blinky, on the device | before | after |
|---|---|---|
| instructions | 236M | **126M** |
| at ~12 MIPS | ~20 s | **~10 s** |

The profile also corrected an assumption: the carry chain's initial
search was not the cost it was taken to be. It is now a ring search
from the target anyway -- same answer, fewer tries -- but the time was
in the annealer.

## 22. 8.3 names

The second on-board run got as far as the database:

```
zfpga: error: no database for LFE5U-25F (looked for /fpga/lfe5u-25f.zdb)
```

and the file was there. Zeitlos's FatFs is built with `FF_USE_LFN 0`
(`sw/os/fs/fatfs/ffconf.h`): **names are 8.3 or nothing.** `lfe5u-25f`
is nine characters. `mcopy`, writing the release's card image, stored it
under a VFAT long-name entry; Linux reads that back as written, and
FatFs sees only a mangled short alias. Nothing on the host could see
the problem.

Everything zfpga puts on or makes on the card is now 8.3:

| | was | is |
|---|---|---|
| databases | `lfe5u-25f.zdb` (and `-45f`, `-85f`) | `lfe5u25f.zdb`, `lfe5u45f.zdb`, `lfe5u85f.zdb` -- the device name, lower case, without its dash |
| a Trellis configuration | `.config` | `.cfg`, written by `build`, `pnr` and `unpack`; `.config` still read everywhere |
| board profiles, pins, examples | already 8.3 | unchanged |

And so that it stays true:

- **`tests/run.sh` checks every name in `db/`**, which is the card's
  `/fpga` exactly.
- **`release/lib/mkfatimg.py` checks every component of every path** in
  the `/fpga` list, the zcc runtime and the configuration template
  before copying it, and stops the build naming the file. It already
  checked apps and headers; these lists were copied unchecked, which is
  how the database got through.
- **On the device, `zfpga build` refuses an input whose outputs would
  not be 8.3** before doing any work (`tests/run_dev.sh` checks), and
  every "cannot open", "cannot create" and "no database" message says
  when the name is the reason.

One more finding, outside zfpga and not changed here: the card image
also ships every `docs/*.md`, and **45 of the 106 are longer than
8.3** -- `app_runtime.md`, `blackjack.md`, `window_manager.md`, and this
file's own companions `zfpga-formats.md` and `zfpga-test.md`. On the
Lakritz those are unreadable. Extending the new check to the docs list
would stop the release until they are renamed or given short names on
the card, which is a decision for the tree rather than for zfpga.

## 23. First light

On a Machdyne Lakritz running Zeitlos, from `posix`:

```
zfpga build /fpga/examples/blink.v -b lakritz
```

synthesised the 24-bit counter blinky to 24 flip-flops and a 12-cell
carry chain, placed it, routed it, and packed it, peaking at **3,152 KB**
-- the figure the simulator predicted for the device build, to the
kilobyte. The bitstream, written to a spare MMOD with the `mmod` app and
booted from the configuration socket, **blinks the LED.**

### 23.1 What that establishes

Every layer at once, on silicon: the Verilog front end and width rules
(§19), carry-chain mapping with the `D`-tied-high INIT split (§19.2,
§16), packing flip-flops onto carry outputs, the placer's chain blocks
and the feed-in slice that starts a chain clean whatever is to its west
(§16.1), routing across fixed and configurable connections (§13.3),
IO and bank configuration including the tristate tie (§12.4), the
Trellis database's meaning for every bit touched, the packer's CRAM
layout, compression and CRCs (§11) -- and, on the machine side, the
memory tier, the database load from SD, the arena release between
stages (§20.1) and the output relay (§21.1).

The simulator's prediction of peak memory matching the board exactly
is a small thing worth recording: it means the device build under
`sim/` is a trustworthy stand-in for the machine in everything but
the three differences §21.3 lists.

### 23.2 What it does not

- **One design, one board.** The blinky uses one IO standard, one
  clock and no LUT logic after synthesis; `features.v` (§19.3) and the
  dense designs are proven equivalent by simulation, not yet by LED.
- **Timings were not recorded** -- the database load from SD and the
  total. The simulator says ~10 s of computation for the blinky.
- **`on.bit` and `empty.bit` were not needed**: they exist to isolate a
  failure, and there was none.
- **The SOC byte-identity test** of §11.6 is still pending (nextpnr
  needs more memory than the build machine had).
- **The 45F and 85F** have been exercised only by `zfpga pack` against
  ecppack, on the host.

### 23.3 What is still open

In the order section 19.6 and the round after it arrived at:

1. ~~**Module instances and `for` loops**~~ -- done, and more, §24.
2. ~~**`zfpga bram`**~~ -- done, §25.
3. **Global clock routing**, before designs grow past a few dozen
   tiles; and some **timing analysis**, of which there is none.
4. **Phase 7, `zboot`** -- the design agreed (a jumploader at
   `0x1D0000`, `docs/zboot.md` §5) and step 1, `reboot`, built (§6) --: reboot, a writable flash slot, launching
   gateware without swapping modules -- a Zeitlos gateware change,
   tested on a board.
5. **Block RAM inference** -- now the first thing between zfpga and
   this tree's RTL (§24.6) -- tristate IO, `generate`, `/` and `%`.
6. **Speed**: the router and placer on larger designs (§14.4, §15.6);
   the SD load of the database, once measured.
7. **The 45F and 85F databases on the card.**
8. **8.3 names across the card image**: 45 of the 106 `docs/*.md`
   shipped to the card cannot be opened there (§22).
9. **`sim/`**: an option to trap misaligned access, and the uptime
   borrow fix (§21.3).

## 24. `zfpga synth`, second version: real Verilog

Section 19's synthesiser read one module. This one reads the Verilog
this tree is written in, and was measured against it (§24.6).

```
zfpga synth top.v uart.v fifo.v -t top -l pins.lpf
```

### 24.1 What was added

| | |
|---|---|
| **The preprocessor** | `` `define`` (object-like), `` `ifdef``/`` `ifndef``/`` `elsif``/`` `else``/`` `endif``, `` `include``, `` `undef`` |
| **Hierarchy** | any number of modules and files; `-t TOP`, or the one module nothing instantiates; instances with parameters overridden by name or position, ports connected by name, position, or left empty |
| **Parameters** | typed (`parameter integer`, `[7:0]`), `localparam`, `$clog2`, part-selects of parameters -- bounds-checked against the declared range |
| **Declarations** | `signed`, `integer`, memories (`reg [7:0] m [0:15]`) |
| **Statements** | `for` loops (constant bounds, `integer` variables), functions, blocking `=` in clocked blocks, `$display` and friends ignored |
| **Expressions** | signed arithmetic and comparison, `>>>`, `$signed`/`$unsigned`, `*`, indexed part-selects `[s +: n]`/`[s -: n]` with constant or variable `s`, variable-index bit writes |

### 24.2 How: definitions, then flattening

The first version evaluated widths as it parsed. With instances that
cannot work -- a child's `reg [W-1:0]` is as wide as *each instance's*
`W` -- so the front end is now three stages:

- `synth_parse.c` reads files into module **definitions**, declarations
  unevaluated. The **preprocessor lives in the lexer** as a stack of
  sources: `` `include`` pushes a file, a macro use pushes its text, and
  every token keeps its own file and line, so an error in an included
  file points into that file.
- `synth_flat.c` **flattens** from the top down: each instance's
  parameters (its overrides, or its defaults, in its own scope), then
  its widths and memory depths, then its statements cloned with every
  name prefixed by the instance path -- `u_fifo.count` -- and its ports
  as assigns between parent and child. The result is the flat module
  §19's elaborator already consumed.
- `synth_elab.c` gained the rest:
  - **Signedness** follows Verilog's rule -- an expression is signed
    only if every operand is -- and is carried into operand extension.
    Signed comparison flips both top bits and compares unsigned; `>>>`
    fills with the sign.
  - **Memories** are flip-flops, up to 4,096 bits. A variable-index
    read is a mux tree across the words. A variable-index write updates
    every word through a mux on `index == k`, which in a clocked block
    the enable extraction of §19.2 turns into a clock enable per word:
    a register file.
  - **`for` loops** unroll under loop-variable bindings visible to
    `const_eval`, so `a[i]` in a loop is a constant select; the scan
    for a block's targets unrolls too, since `m[i] <= 0` names a
    different word each pass.
  - **Functions** run as a small `always @(*)` with their inputs bound.
  - **Blocking assignment in a clocked block**: each target carries a
    flag, "written with `=` on this path"; a read sees the new value
    when it is set and the old one when not, and branches merge the
    flags with OR. A temporary is then a flip-flop nothing reads, which
    the dead-logic sweep removes.
  - **`*`** is shift-and-add on carry chains.

### 24.3 Checked

Every construct has a design in `examples/` checked against yosys's
synthesis of the same source, through the whole chain -- `.zl` and final
bitstream both -- by `tests/run.sh`:

| | |
|---|---|
| `hier.v` | three levels, a parameterised counter at two widths, named and positional overrides and ports, an empty port, `$clog2`, a typed `localparam` selected by parameter, `` `include``/`` `define``/`` `ifdef`` |
| `loops.v` | `for` in combinational and clocked blocks, nested, a reset loop over a memory |
| `mem.v` | an 8x8 register file, two read ports, a FIFO |
| `signed.v` | sign extension, signed and unsigned comparison of the same bits, `>>>` by a variable, signed and unsigned `*` |
| `func.v` | a CRC-16 step function with a loop, called in two places |
| `partsel.v` | `+:` and `-:`, constant and variable, read and written |
| `blocking.v` | temporaries with `=` mixed with `<=` state |

And nineteen refusals, each by file and line -- among them an
undeclared loop variable, an undefined module, `/`, `<=` in
`always @(*)`, `genvar`, a memory past the flip-flop limit, recursion,
a macro with arguments, a runaway loop, two candidate tops, a parameter
select out of range, tristate, and `x`.

### 24.4 Bugs, and what found them

- **An empty slot in a positional port list**, `(a, , c)`, did not
  parse (`hier.v`).
- **A select past a parameter's declared width** was answered, not
  refused; Verilog leaves those bits undefined, and yosys and zfpga
  picked differently (`hier.v`, as first written -- the test was wrong
  and so was the tool).
- **A mux tree on a 32-bit index** -- an `integer`, or any wide
  expression -- overflowed `base + (1 << level)` and crashed
  (`rtl/audio_spdif.v`, through `tests/rtlcheck.sh`). Only the low bits
  that can address the vector now build the tree; any higher bit set
  gives 0.
- **A variable `-:` select reaching below bit 0** was computed as a
  shift by `s - (n - 1)`, which wraps, zeroing even the bits in range.
  Padding the vector below bit 0 and shifting by `s` itself needs no
  subtraction (`partsel.v`).
- **simcheck compared state before a reset.** A register with no
  initial value powers up undefined; yosys builds a synchronous reset
  from a flip-flop's set and so powers it up at 1. With a reset input,
  comparison now starts after the first edge (`func.v`).
- `%ld` in one message, which the formatter does not support.

### 24.5 Undriven bits

Legal Verilog leaves a stub's unused output undriven (`rtl/dma.v`).
Such a bit -- nothing drives it anywhere -- now reads as 0, with one
note per signal; yosys gives `x`. A bit driven on some paths only is
still refused, as a latch.

### 24.6 This tree's RTL

`tests/rtlcheck.sh` (host, needs yosys) takes each file in `rtl/`, finds
the files of the modules it instantiates anywhere under `rtl/`, and
compares zfpga's synthesis with yosys's by simulation -- extended for
the purpose to model yosys's LUT RAM cells, which `ram_style =
"distributed"` makes it use regardless of `-nolutram`:

| | |
|---|---|
| **equivalent (15)** | arbiter_main, arbiter_vram, audio_mixer (9,575 LUTs, 2,084 flip-flops), audio_out, audio_spdif, csrs, dma, gpio (1,656 LUTs), **montmul** (7,937 LUTs, 2,560 flip-flops, 67 carry chains), mtu, rtc, socctl, spim, uart (1,615 LUTs), uart_null |
| refused: block RAM (5) | audio, cache, esp32_rxfifo, ethmac_rmii, probe |
| refused: tristate (3) | spiflashro (since replaced by `spiflash.v`, which tri-states the same way), usb_hid, usb_cdc_uart |
| refused: `generate` (1) | trng -- its ring oscillators, deliberately |
| not checkable (1) | sysctl: its BIOS RAM is `$readmemh`ed |

**No module zfpga accepts disagrees with yosys.** Block RAM inference
is what stands between zfpga and most of the rest.

### 24.7 On the device

Synthesis stays cheap: the three-level `hier.v`, with its `` `include``,
in 1.6M instructions, and the device's `.zl` is the host's
(`tests/run_dev.sh`).

## 25. `zfpga bram`

```
zfpga bram soc.cfg -f bios_seed.hex -t bios.hex -o soc_final.cfg
zfpga bram soc.bit -f bios_seed.hex -t bios.hex -o soc_new.bit
zfpga bram -g seed.hex -w 32 -d 1024 [-s 42]
```

This tree's build puts the BIOS into the SOC after place-and-route: the
SOC's RTL initialises its BIOS RAM from a random seed file, and
`ecpbram` then finds the seed in the configuration and replaces it with
the BIOS, so the BIOS changes without re-running nextpnr (`Makefile`,
target `soc`). `zfpga bram` does that job -- and, given a `.bit`, does
it on the machine: Zeitlos rebuilding its own bitstream with a new BIOS.

### 25.1 What ecpbram actually does

Read from its source (Project Trellis 1.4, `ecpbram.cpp`), it matches
**bit slices, not words**. Each bit column of the seed file, 512 words
at a time, is a 512-bit pattern; every block RAM in the design is read
as 512-bit slices in each of its six width configurations (1, 2, 4, 9,
18, 36 bits); a slice equal to a seed pattern is replaced by the same
slice of the new file. That is why the seed must be random -- its
slices must be unique, and recognisable however synthesis laid the
memory out -- and `zfpga bram` refuses a seed with a repeated slice.

### 25.2 What it does not copy

ecpbram re-serialises the whole configuration, and libtrellis writes
each `.tile_group` with its last tile twice. Harmless -- packing applies
that tile twice -- and not copied: `zfpga bram` rewrites only the
`.bram_init` data and keeps every other line as it was. So the test is
the bitstream: zfpga's output and ecpbram's pack to **the same bits**.

### 25.3 A .bit

Unpacked, patched, packed again with the options it was packed with
(§18) -- compression, clock, multiboot address, usercode -- so it boots
from the same address as before. A compressed bitstream with multiboot
at 0x040000 comes out the same as ecpbram followed by ecppack with
those options.

**A bug in two parts that each worked**: `bram` releases the unpack's
memory before packing, and the database the unpack had loaded was in
it -- while the one-per-process database cache of §20.1 still pointed
there, so the pack read freed memory. `build` never met it, loading the
database before its first mark. Now `zf_release()` tells the cache
what it frees, and a released database leaves the cache.

### 25.4 Checked

| | |
|---|---|
| `.cfg` | a ROM from a random seed, placed and routed by nextpnr: packs to the bitstream ecpbram's output packs to |
| `.bit` | compressed, multiboot at 0x040000: the same bitstream |
| `-g -s 1234` | ecpbram's seed file, byte for byte (its xorshift64* and its seed mixing) |
| a repeating seed | refused |
| on the device | a `.bit` in 181M instructions (~15 s), the host's result |

### 25.5 And git

Adding the seed fixtures, `tests/run.sh`'s new check -- that no source
file here is one git would ignore -- failed: the tree's `.gitignore`
also ignores `*.hex`. It is the check written after the first commit of
zfpga lost sixteen files to `*.config` and `*.json`, and it caught the
third pattern before any commit did. `sw/apps/zfpga/.gitignore`
re-includes all three.


## 26. `zfpga jump`

The jumploader of `docs/zboot.md` section 5 is a zfpga product: `zfpga
jump TARGET -b BOARD` builds the one-line design and packs it with the
new `pack -J`, which keeps the frames holding the boot address at a
fixed length so the machine can change the address in place
(`docs/zfpga-formats.md` section 10). Checked against ecpunpack, which
verifies every CRC; the host and the machine produce the same bytes (26
device tests); and `tests/kjump.c` runs the kernel's re-pointing code
against a simulated flash. `-J` also made `pack_write` two passes -- a
counting pass finds the offsets the header describes -- which briefly
opened the output before the options were checked; tests/run.sh's
partial-output test caught it.


## 27. The 45F, and `zfpga flash` / `run`

**The 45F** (Mozart ML1, Sergei ML1): its Trellis data is vendored --
three device files and the 23 tile types a 25F lacks, 5.3 MB -- and
`lfe5u45f.zdb` (1.67 MB) is built and goes on the card. Its nextpnr
baseline was measured, not transcribed (`ext/nextpnr-base/`). Checked:
`zfpga pack` of a nextpnr 45F design is byte-identical to ecppack's
(uncompressed, compressed, with a boot address), `zfpga unpack` to
ecpunpack's; zfpga's own place and route on the 45F gives designs
equivalent to yosys's by simulation (`loops.v`, `signed.v`); a
jumploader is 162,793 bytes; and on the machine a 45F blinky builds to
the host's bitstream, byte for byte, in 172M instructions (about 14 s)
and 3,647 KB of zfpga's 4 MB tier (tests/run_dev.sh; the host's figure
is larger, its pointers being 64-bit). Board profiles are
`mozart1.brd` and `sergei1.brd` -- 8.3 names -- and `-b mozart_ml1` /
`-b sergei_ml1` find them.

**`zfpga flash` and `zfpga run`** install a bitstream in the machine's
flash, after the core apps or at `-a ADDR`, and boot it through the
jumploader (`docs/zfpga-formats.md` section 11). The flash is reached
through the port layer: the kernel's `Z_SYS_FLASH` on the machine, a
flash image file in the test binary `zfpga-simflash`, which is how
`tests/run.sh` runs the real commands end to end. The tests were
checked against two mutations -- placement ignoring the core apps, and
no device-ID check -- which they catch.


**Placement, revised.** On a 2 MB board with a 45F, user gateware does
not fit after the core apps once they have grown; `zfpga flash` now
also uses the tail of the gateware region, after Zeitlos's own
bitstream and before the logo (`docs/zfpga-formats.md` section 11) --
where `tone.v` ran on a Mozart ML1 at `0x0A0000`. `release/lib/layout.py`
checks the copies of the flash map in `boot.c`, the logo's offset and
every DFU board's `dfu_base` among them.

---

## See also

- `docs/zcc.md` — the compiler, and the pattern this follows: host/device
  seam, differential testing, refusal over approximation
- `docs/posix.md` — the phase-plan convention, and §2's constraint survey
- `docs/toolchain.md` — the host tools this replaces, and where they come
  from
- `docs/ramdisk.md` — why the build directory is `/ram`
- `docs/zfpga-formats.md` — every file format, for editing by hand
- `docs/zfpga-test.md` — the first test on a board, step by step
- `docs/zboot.md` — multiboot, PROGRAMN, the flash layout, and both
  routes to loading what this builds
- `docs/mmod.md` — the app that already writes configuration flash
- `docs/dfu_upgrade.md` — the flash partitioning Phase 7 has to live
  inside
- `sim/README.md` — where the code should be developed before it meets a
  board
- Project Trellis: https://prjtrellis.readthedocs.io/ — bitstream format,
  tiles, general routing
- prjtrellis-db: https://github.com/YosysHQ/prjtrellis-db — the database
  `mkzdb.py` converts, CC0 licensed
