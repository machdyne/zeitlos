# Boards

What the SoC actually costs on each ECP5 board, after packing and
routing.

[audio.md](audio.md) measures the audio subsystem at synthesis time.
Its LUT4 column is pre-packing, and it says so: the figures that
decide whether a board places, and whether 48 MHz is real, come out of
`make util` and `make timing`. Nothing had written those down. This
page is that table. It is the whole design, not the audio block, so
the two are not interchangeable — a board can be comfortable here and
still be out of block RAM once something else is added, or the other
way round.

## Regenerating

One board:

```
make BOARD=obst zeitlos_pico
make BOARD=obst util      # packed counts, including TRELLIS_COMB
make BOARD=obst timing    # routed fmax of each clock
```

`zeitlos_pico` is place-and-route only. It does not build the BIOS or
pack a bitstream, and it is all these numbers need.

ULX3S defaults to `DEVICE=25k`. The 85F is the same board with an
explicit device, and it is the build the seed below was chosen for:

```
make BOARD=ulx3s DEVICE=85k zeitlos_pico
```

`PNR_SEED` defaults to 10 on ULX3S and is not passed for any other
board, which then uses nextpnr's own seed. The 25k and the 85k share
that default: both rows below are seed 10.

Measured with Yosys 0.60+95 and nextpnr-ecp5 0.9-50-g1ce187ab, one run
each, at git `616e152`. An older nextpnr reports a lower fmax for the
same netlist — see [toolchain.md](toolchain.md) before treating a FAIL
as a regression.

`make timing` prints the last line nextpnr wrote for each clock. The
log contains an earlier line too, an estimate from immediately after
placement, and that estimate can fail on a design the router then
closes. The line in the table is the routed one. `ro_clk` is not
listed: those are the TRNG ring oscillators, and they are not
constraints.

## Utilisation

`TRELLIS_COMB` is the packed count, and it is the one that matters.
Above about 75% of the device the placer starts to struggle and timing
becomes seed-sensitive. The percentage in the table is nextpnr's own.

| Board | Device | COMB | FF | DP16KD | MULT18 | PLL |
|---|---|---|---|---|---|---|
| Obst | 12k | 18355 / 24288 (**75%**) | 8282 / 24288 (34%) | 32 / 56 | 11 / 28 | 2 / 2 |
| Lakritz | 25k | 19060 / 24288 (**78%**) | 8956 / 24288 (36%) | 35 / 56 | 11 / 28 | 2 / 2 |
| ULX3S | 25k | 18771 / 24288 (**77%**) | 8722 / 24288 (35%) | 39 / 56 | 11 / 28 | 2 / 2 |
| Mozart ML1 | 45k | 21033 / 43848 (47%) | 9491 / 43848 (21%) | 41 / 108 | 11 / 72 | 2 / 4 |
| Sergei ML1 | 45k | 18937 / 43848 (43%) | 8672 / 43848 (19%) | 42 / 108 | 11 / 72 | 2 / 4 |
| ULX3S | 85k | 18771 / 83640 (22%) | 8722 / 83640 (10%) | 39 / 208 | 11 / 156 | 2 / 4 |

Three of those are the same design on two densities, or the same
density with a different board:

- ULX3S 25k and 85k are one netlist. The absolute counts match
  (18771 COMB, 8722 FF, 39 DP16KD, 11 multipliers). On the 25k that is
  77% of the fabric and 39 of 56 block RAMs. On the 85k it is 22% and
  39 of 208.
- Mozart ML1 is the only board on `USB_HOST`. It is also the larger
  of the two 45k builds, 21033 COMB against Sergei's 18937. Both still
  have half the device free. The delta is the whole board difference
  (USB core, audio output, who drives the RMII clock), not a
  measurement of the USB core on its own.
- Lakritz at 35 of 56 DP16KD is the post-`no_rw_check` figure. The 55
  in the [audio.md](audio.md) table was measured before that, and the
  note there already says so.

Pre-packing LUT4, for the row [audio.md](audio.md) warns is not the
packed count: Obst 17415 (71%), Lakritz 18092 (74%), ULX3S 17794 (73%
on the 25k, 21% on the 85k), Mozart 19892 (45%), Sergei 17942 (40%).
Packing moves these by a few hundred cells either way. Quote
`TRELLIS_COMB` when the question is whether the placer has room.

## Clocks

Routed fmax, and the constraint nextpnr was checking against. Every
one of these passed.

The 48 MHz clock is not the same net on every board. `OSC48` boards
clock the design from the oscillator pin, so the name is `CLK_48`.
ULX3S is `OSC25`: 48 MHz is a PLL output, `clk48mhz`. Obst has no
126 MHz clock because its video is VGA, not DVI. Sergei drives the
RMII reference clock (`ETH_RMII_DRIVE_REFCLK`); Mozart receives it.

| Board | Clock | fmax | Constraint |
|---|---|---:|---|
| Obst | `clk25_2mhz` | 80.03 MHz | 25.20 MHz |
| | `clk12mhz` | 73.72 MHz | 12.00 MHz |
| | `CLK_48` | 56.70 MHz | 48.00 MHz |
| Lakritz | `clk126mhz` | 316.66 MHz | 126.01 MHz |
| | `clk25_2mhz` | 62.60 MHz | 25.20 MHz |
| | `clk12mhz` | 74.85 MHz | 12.00 MHz |
| | `CLK_48` | 53.88 MHz | 48.00 MHz |
| Mozart ML1 | `clk126mhz` | 295.51 MHz | 126.01 MHz |
| | `clk25_2mhz` | 64.80 MHz | 25.20 MHz |
| | `ETH_REFCLK` | 101.68 MHz | 50.00 MHz |
| | `CLK_48` | 58.98 MHz | 48.00 MHz |
| Sergei ML1 | `clk126mhz` | 243.61 MHz | 126.01 MHz |
| | `clk25_2mhz` | 66.12 MHz | 25.20 MHz |
| | `clk12mhz` | 76.75 MHz | 12.00 MHz |
| | `CLK_48` | 52.35 MHz | 48.00 MHz |
| | `ETH_REFCLK` | 103.48 MHz | 50.00 MHz |
| ULX3S 25k | `clk126mhz` | 276.78 MHz | 126.01 MHz |
| | `clk25_2mhz` | 64.18 MHz | 25.20 MHz |
| | `clk12mhz` | 79.79 MHz | 12.00 MHz |
| | `clk48mhz` | 56.19 MHz | 48.00 MHz |
| ULX3S 85k | `clk126mhz` | 257.47 MHz | 126.01 MHz |
| | `clk25_2mhz` | 59.42 MHz | 25.20 MHz |
| | `clk12mhz` | 78.76 MHz | 12.00 MHz |
| | `clk48mhz` | 53.49 MHz | 48.00 MHz |

The clocks with the least margin over 48 MHz are Sergei `CLK_48`
(52.35 MHz), ULX3S 85k `clk48mhz` (53.49) and Lakritz `CLK_48`
(53.88). Lakritz is the one the 75% rule is about: 78% COMB. Sergei
is at 43% and the 85k at 22%, so a tight 48 MHz clock is not, by
itself, evidence that the fabric is full. ULX3S pins `PNR_SEED`
because a bitstream that misses 48 MHz still programs and then
misbehaves. These two rows are that pinned seed, and both close.

## ULX3S 85k with the caches and the MPU

The tables above are at 616e152. The ULX3S 85k was measured again with
the data cache, SDRAM burst fills and the MPU in ([dcache.md](dcache.md),
[mpu.md](mpu.md)), the same flow and seed 10 (`make BOARD=ulx3s
DEVICE=85k`), Yosys 0.63+173 and nextpnr-ecp5 0.10-12-g5281b8d8, at
76f3434 (112d0ad plus two commits that do not touch the RTL). All four
clocks close.

| | 616e152 | 76f3434 |
|---|---|---|
| COMB | 18771 / 83640 (22%) | 21254 / 83640 (25%) |
| FF | 8722 / 83640 (10%) | 9627 / 83640 (11%) |
| DP16KD | 39 / 208 | 42 / 208 |
| MULT18 | 11 / 156 | 11 / 156 |
| `clk126mhz` | 257.47 MHz | 247.22 MHz |
| `clk25_2mhz` | 59.42 MHz | 65.13 MHz |
| `clk12mhz` | 78.76 MHz | 71.94 MHz |
| `clk48mhz` | 53.49 MHz | 53.15 MHz |

The 616e152 column was measured with a different Yosys and nextpnr, so
a change of a few percent in fmax between the columns is not evidence
about the design.

## Artix-7 boards (Sechzig MX1)

`sergei_mx1` and `mozart_mx1` are the Sechzig MX1 compute module --
Artix-7 XC7A35T-FTG256, 32 MB SDRAM (W9825G6KH-6), 4 MB flash
(W25Q32JV, JEDEC `EF 40 16`) -- in a Sergei or Mozart carrier: the ML1
boards with an Artix-7 module. Built with openXC7 1.x
([toolchain.md](toolchain.md)).

### Status: partial -- the BIOS runs, the SDRAM does not answer

Tested on a sergei_mx1 with openXC7 1.x (nextpnr `c68c1358`); mozart_mx1
builds but has not been on hardware.

**Working:** configuration from flash and over JTAG; the MMCMs lock; the
CPU and the BIOS run from block RAM; the console at 1 Mbaud; CSR word
63 (the BIOS reports the Zeitlos region at `0x10300000`); the video
output (the hardware cursor shows on a monitor).

**Not working: the SDRAM.** Every read returns what looks like a
floating bus -- the same few bit patterns at every address, echoing the
last value written -- and the BIOS memory test fails on its first word,
so the kernel cannot be loaded. Ruled out so far:

- **The pins.** All 39 SDRAM signals traced from the chip's own pins to
  FPGA balls on the MX1 PCB; they match the XDC, and they match LiteX's
  `machdyne_mozart_mx1.py` platform, under which (built with Vivado)
  this module's SDRAM works.
- **The bitstream's pin configuration.** From the FASM: all 54 pins of a
  build have the right direction; the SDRAM clock's ODDR is fed the
  intended values; the data pins' tristate is present and not
  inverted.
- **The controller.** `rtl/mem/sdram_kianv.v` is the controller every
  SDRAM board in the tree uses with this chip; its non-burst read path
  is unchanged from the version that passed the BIOS memory test on
  Kirsch (the same module wiring) under openXC7 0.9.
- **I/O timing**, which openXC7 does not analyse. The interface now has
  LiteDRAM `GENSDRPHY`'s structure, none of which changed the result:
  every output registered in its pin (ODDR, D1 = D2), read data captured
  in each pin's IDDR, and the SDRAM clock 90 degrees behind `sys_clk`,
  both from one MMCM (`SDRAM_CLK90`, below).

The SDRAM is therefore the open question for this board, and the prime
suspect is how openXC7 1.x builds the interface.

**`BRINGUP_LED_STATUS`** is on for these boards: the module LED shows
how far the SOC got, in hardware, independent of software -- fast blink
(~6 Hz) MMCMs not locked, solid CPU trapped, slow blink (~1 Hz) CPU
running, static CPU stalled on a bus cycle. Software cannot drive the
LED while it is set.

### What differs from the ML1 boards

- **Pins.** `boards/sergei_mx1.xdc` and `boards/mozart_mx1.xdc` are
  generated from the netlist's ports, `boards/<carrier>_ml1.lpf` and the
  ML1 and MX1 schematics (github.com/machdyne/sechzig, `pcb/ml1_v2` and
  `pcb/mx1_v1`). Banks 14, 15 and 35 are 3.3 V on the MX1; bank 34,
  used only by the experimental DS link, is 2.5 V.
- **The console** is `UART0` on the module's own `UART_TX`/`UART_RX`
  (L2/L3), bridged to USB by the carrier's RP2040, which runs the same
  dirtyJtag firmware that programs the board. The ML1 constraint files
  put `UART0` on the `XA`-`XD` link instead; those pins reach the
  RP2040 too, but its firmware does not use them. Neither carrier
  connects the module's USB device pins; Sergei uses `USBD_P` (B2) for
  S/PDIF.
- **Clocks.** `SYS_CLK48` (F5) comes from the same RP2040. With
  `SDRAM_CLK90` (set on these boards) it feeds only the MMCMs: `sys_clk`
  and the SDRAM clock, 90 degrees behind it, are both outputs of one
  MMCM, as LiteX's `sys` and `sys_ps` are. The MMCMs are held in reset
  for 1024 input clocks after configuration, so they start on a running
  clock however late the RP2040 provides it.
- **Reset.** `SYS_RST_N` goes to the Artix-7's PROGRAM_B (L9): the
  carrier's reset reconfigures the FPGA, and the gateware cannot drive
  it, so there is no `PROGRAMN_PIN` and no jumploader.
- **Flash.** The Zeitlos region is at `0x300000`, the top megabyte of
  the 4 MB part, because the bitstream is a fixed 2.09 MB
  ([boot.md](boot.md#the-flash-layout)). The write lock covers the
  bitstream (`0x220000`). The flash is reset (`66h`/`99h`) before its
  first command ([spiflash.md](spiflash.md)).
- **The BIOS** is built without its xfer receiver (`BIOS_FLAGS =
  -DBIOS_NO_XFER`), and the build refuses an MX1 BIOS with less than 512
  bytes of stack (`BIOS_MIN_STACK`). The image and the stack share the
  8 KB BIOS RAM, image growing up and stack down; every build prints
  the room between them.
- **No LUT RAM and no DSP48E1s**, and no TRNG or fast multiplier: the
  openXC7 workarounds in [toolchain.md](toolchain.md). Without the LUT
  RAM one, the CPU's register file and the console UART's FIFOs were
  built wrongly and the BIOS failed differently on every build.
- **No caches and no MONTMUL** yet. Each is one line in the board's
  `rtl/boards.vh` block, to try once the board boots.
- **The part** is `xc7a35tftg256-1` by default: most modules carry the
  -2 part, some a -1, and a -1 build meets timing on both.

**Synthesis** (`synth_xilinx`, before place-and-route): sergei_mx1: ~14,691 LUTs of 20,800 (70%), 12,616 flip-flops, 17 RAMB36 + 4 RAMB18 of 50, no DSP48E1 or LUT RAM, 6 BUFG, 47 ODDR, 20 IDDR.
mozart_mx1: ~14,671 LUTs of 20,800 (70%), 12,563 flip-flops, 17 RAMB36 + 4 RAMB18 of 50, no DSP48E1 or LUT RAM, 6 BUFG, 46 ODDR, 20 IDDR.
openXC7 1.x reports `clk48mhz` timing with a wide margin (93 MHz on a
reduced build); see [toolchain.md](toolchain.md) on how far to trust
that.

## Boards with nothing to measure

iCE40 targets in the Makefile (`riegel`, `eis`, `kolibri`, `bonbon`,
`keks`, `kuchen`, `kuchen_v0`, `brot`, `krote`, `icoboard`) have no
block in `rtl/boards.vh`. There is no configuration to synthesise.

`schoko`, `konfekt`, `minze` and `vanille` are ECP5 targets with an
LPF and the same gap: the Makefile names them, `rtl/boards.vh` does
not.

Lebkuchen and Kölsch do have a `boards.vh` block (GateMate, not ECP5).
Their flow is a separate Yosys and nextpnr under the path the Makefile
sets for those boards, not the ECP5 tools these numbers came from.
