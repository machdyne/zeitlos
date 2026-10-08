# Boards

What the SoC actually costs on each ECP5 board, after packing and
routing. Konfekt, Minze, Schoko, Noir, Klinge and the ML0 boards have their own sections below
the main tables, measured later and with newer tools.

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

`ulx3s_85f_langkatze` is this board with `ESP32_LINK` left out and a
Langkatze PMOD in J1. On that target the default seed has been
measured at 45.71 MHz, which misses 48; seed 7 meets it at 56.38 MHz.
The remote desktop there is `zerdesk`, not the ESP32
([remote_desktop.md](remote_desktop.md)).

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

## Lakritz with game-mode colour

Lakritz builds `COLOR` ([color.md](color.md)) alongside `USB_HOST`,
`USB_CDC` and `AUDIO_MIXER`, and drops all of the hardware crypto to
make room: `MONTMUL`, `MONTMUL_REGS` and `SHA256`. The D-cache stays.

Measured, latest oss-cad-suite, default seed:

| | |
|---|---|
| TRELLIS_COMB | **98%** |
| `CLK_48` | 52.82 MHz (48 required) |
| `clk25_2mhz` | 51.08 MHz (25.20 required) |
| `clk126mhz` | 315.76 MHz (126.01 required) |

98% is far past the 75% line, so treat any change to this board as
needing `make timing` again. The pixel clock has the most room. Colour
moved it from about 62 to 51 MHz, still twice what it needs, which is
where the colour logic was deliberately put. `CLK_48` keeps 10% of
margin.

How the budget came out, from synthesis differences (yosys 0.69,
`synth_ecp5 -abc9`, whole SoC; LUT-equivalents count a carry cell as
two):

| Change | Approx. LUT-equivalents |
|---|---|
| `USB_HOST` instead of `USB_HID` (upstream `290bc49`: 94% to 102%) | +1,800 |
| `COLOR` | +530 to +660 |
| `MONTMUL_REGS` + `SHA256` off | about −1,900 |
| `MONTMUL` off | −900 |

For reference, other trades that were measured and not taken: no
`DCACHE` about −1,300, no `MPU` about −430, no `SPI_ETH` about −220.
Halving both caches saves about 75 and two block RAMs.

TLS and SSH work without the crypto blocks, on the software paths,
computing the same answers more slowly. A networking-first Lakritz
that does not want colour can swap the defines back in
`rtl/boards.vh`.

`release/hw/boards/lakritz.spec` matches, including `USB_HOST`, which
it had still listed as `USB_HID`; `zrelease check` is clean for every
Lakritz target.

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

## Konfekt and Schoko

Two Machdyne SDRAM boards added at `e5086e4`, both with the console on
USB CDC-ACM (the USB-C socket, [usb_cdc.md](usb_cdc.md)) and both
packed for the 256 KB DFU bootloader layout (`dfu_base = 0x040000`,
[dfu_upgrade.md](dfu_upgrade.md)). Pins are `boards/konfekt_v0.lpf` and
`boards/schoko_v1.lpf`, each checked ball by ball against the board's
schematic; the header of each file lists where the zucker and LiteX
copies were wrong.

Measured with Yosys 0.69+173 and nextpnr-ecp5 0.11.1-40 (oss-cad-suite
2026-10-01), default seed, one run each.

| Board | Device | COMB | FF | DP16KD | MULT18 | PLL | `CLK_48` | bitstream |
|---|---|---|---|---|---|---|---:|---:|
| Konfekt | 12k | 21542 / 24288 (**88%**) | 9987 (41%) | 40 / 56 | 11 / 28 | 2 / 2 | 57.00 MHz | 511,268 B |
| Schoko | 45k | 23389 / 43848 (53%) | 10742 (24%) | 39 / 108 | 9 / 72 | 2 / 4 | 52.35 MHz | 619,329 B |

Every clock passed. Konfekt `clk25_2mhz` 61.41 MHz, `clk126mhz`
388.50 MHz, `clk12mhz` 72.27 MHz; Schoko `clk25_2mhz` 60.72 MHz,
`clk126mhz` 343.64 MHz (Schoko has no `clk12mhz`: `USB_HOST` runs from
the system clock). The bitstream column is the compressed `soc.bit`,
and both fit the DFU layout's 786,432-byte gateware region
(0x040000-0x0FFFFF).

**Konfekt** is a Lakritz without the PMOD connector, on the 12F --
which the open-source tools treat as the 25F die. The block is
Lakritz's less `SPI_ETH` and **less `AUDIO_MIXER`**. The mixer went
because Lakritz itself is no longer where the table above this section
puts it: at `e5086e4` it placed at **23395 / 24288 COMB (96%)** with 40
DP16KD and routed only after more than twenty minutes of congestion
(build stopped before it finished, so there is no fmax). Konfekt at 88%
routes and closes with 9 MHz to spare, but it is past the 75% line, so
re-check `make timing` after any change, and look at `SHA256` and
`MONTMUL_REGS` next if it needs room -- both cost speed, not function,
when dropped.

**Schoko** is a 45F with SDRAM: Mozart ML1's cache settings plus the
MONTMUL register file and SHA-256, the full-speed `USB_HOST`, and both
video outputs (VGA and DDMI from one timing generator, the same
picture). Its two PMODs carry a Langkatze on A and GPIO on B in the
board's own block and `.lpf`, which is also the
`schoko_langkatze_gpio` release target. `CLK_48` at 52.35 MHz is the
lowest margin on any board here; it is one seed, and the 45F has half
its fabric free, so a failing seed is a placement problem, not a
capacity one.

Both boards have one USB host socket while `rtl/sysctl.v` always builds
two ports. The second is constrained to C3/D3 (unconnected on both
schematics) with pull-downs, so it reads as an empty port.

## Noir

Machdyne Noir: ECP5 45F with 256MB DDR3L (MT41K128M16JT-125:K) wired to
the same balls as the Sechzig ML2, so it builds as a DDR3 board
([ddr3.md](ddr3.md)): the DDR3 sources, the minimal training BIOS (1806
of 2048 words), `DDR3_ROW_BITS 14` and the 2Gb tRFC. The console is USB
CDC-ACM on the USB-C socket; the rest is a 45F desktop -- full-speed
`USB_HOST`, DDMI, sigma-delta audio with the hardware mixer, SPI
microSD, Schoko's caches and crypto. Pins are `boards/noir_v0.lpf`.

Same tools and method as Konfekt and Schoko above:

| Board | Device | COMB | FF | DP16KD | MULT18 | PLL | sys_clk | bitstream |
|---|---|---|---|---|---|---|---:|---:|
| Noir | 45k | 27716 / 43848 (63%) | 12883 (29%) | 41 / 108 | 13 / 72 | 3 / 4 | 54.23 MHz | 691,610 B |

On a DDR3 board sys_clk is divided from the DDR edge clock, so
nextpnr names it after whatever it reaches first -- here
`audio_i.mixer_i.clk`; it is the 48 MHz system clock. The DDR3 PLL's
free-running `init_clk` closes at 212 MHz, `clk25_2mhz` at 52.88 MHz and
`clk126mhz` at 341.18 MHz. The third PLL is the DDR3 one (`pll2`). The
bitstream fits the DFU layout's 786,432-byte gateware region.

**Building any DDR3 board with Yosys 0.69 needed two fixes**, both in
the tree now and both found on Noir; Mozart ML2 failed the same way,
unmodified, before them:

- `rtl/mem/ddr3_phy_ecp5.v` named the command-path ODDRX2F instance
  `cmd_oddr`, the same as the wire it drives. Inside the generate
  scope, 0.69 resolves `cmd_oddr[gi]` to the instance, leaving the
  port on a dangling interface placeholder. The instance is
  `cmd_gear` now.
- synth_ecp5's last `hierarchy -check` then still re-elaborated sysctl
  from its Verilog AST when it derived the PHY's parameterised DQSBUFM
  blackboxes, after flattening had removed the submodules, and failed
  on `gpu_cursor`. The Makefile now gives DDR3 boards a `YOSYS_PRE`
  that elaborates and round-trips the design through RTLIL first, so
  there is no AST left to re-elaborate. Non-DDR3 boards build exactly
  as before.

## Minze

Machdyne Minze: ECP5 12F, 32MB SDRAM, VGA (a 9-bit RGB333 resistor
DAC), one USB host port, microSD, one PMOD, USB-C. Pins are
`boards/minze_v1.lpf`, checked against the schematic; its header lists
where the board repo's own file differs (B1/B2 are the USB host socket,
not a UART). The block is Konfekt's on the same die, with VGA instead
of DDMI and GPIO on the PMOD instead of audio; the board block and
`.lpf` describe the `minze_gpio` release target exactly. The console is
USB CDC-ACM.

| Board | Device | COMB | FF | DP16KD | MULT18 | PLL | `CLK_48` | bitstream |
|---|---|---|---|---|---|---|---:|---:|
| Minze | 12k | 21150 / 24288 (87%) | 9664 (39%) | 38 / 56 | 9 / 28 | 2 / 2 | 57.87 MHz | 501,327 B |

Every clock passed: the pixel clock at 76.78 MHz against 25.2, the USB
12 MHz at 74.10. VGA uses only the most significant resistor of each
colour's ladder (8 colours of the board's 512), since the SOC drives
one bit per colour; the other six balls are listed in the `.lpf`. The
BIOS is 1987 of 2048 words, the jumploader 99,628 bytes, and the
bitstream fits the 256 KB DFU layout. At 87%, the same advice as
Konfekt applies: re-check timing after any change.

## Mozart ML0 and Sergei ML0

The Sechzig ML0 module is the ML1 on an LFE5U-25F: the same balls, the
same 32MB SDRAM, the same 48MHz oscillator and the same 4MB flash, so
both carriers build with their ML1 pin files unchanged
(`boards/mozart_ml1.lpf`, `boards/sergei_ml1.lpf`). The board blocks
are the ML1 ones trimmed the way Lakritz -- the same die -- is
trimmed: a 4KB instruction cache instead of 8KB, no D-cache write
buffer, and no `MONTMUL` (RSA and DH run in software). DDMI, colour,
the full-speed USB host, RMII ethernet and audio with the hardware
mixer all stay. Like the ML1 boards these are JTAG-flashed modules
(dirtyJtag on the carrier) with no DFU bootloader, and the console is
`UART0`. 25F only; a 12F ML0 would need its own images (a different
IDCODE), so there is none.

Measured with Yosys 0.69+260 and nextpnr-ecp5 0.11.1-54 (oss-cad-suite
2026-10-08), default seed, one run each:

| Board | Device | COMB | FF | DP16KD | MULT18 | PLL | `CLK_48` | bitstream |
|---|---|---|---|---|---|---|---:|---:|
| Mozart ML0 | 25k | 22406 / 24288 (**92%**) | 10391 (42%) | 43 / 56 | 9 / 28 | 2 / 2 | 51.19 MHz | 528,428 B |
| Sergei ML0 | 25k | 22654 / 24288 (**93%**) | 10444 (43%) | 43 / 56 | 9 / 28 | 2 / 2 | 50.19 MHz | 530,109 B |

Every clock passed. `ETH_REFCLK` closes at 90.1 MHz on both against 50
(an input on Mozart, where the PHY has its own oscillator; an output on
Sergei, driven from `pll0`); `clk126mhz` at 275.94 / 306.09 MHz; the
pixel clock at 44.30 / 40.56 MHz against 25.2. The BIOS is 1987 of
2048 words, the jumploaders 99,628 bytes each (zfpga profiles
`mozart0.brd` and `sergei0.brd`, aliased from `mozart_ml0` and
`sergei_ml0`), and the bitstreams fit the 960KB gateware region.

**These are the tightest boards in the tree,** level with
`lakritz_katze`. Both critical paths start in the blitter's glyph
registers (`work_glyph_w`) and are three quarters routing (14.4 of
19.5 ns on Mozart), which is congestion rather than logic depth. With
2 to 3 MHz of margin on one seed, re-check `make timing` after any
change that touches these builds, and expect some seeds to fail. If
one has to give, `COLOR` (one DP16KD and a few hundred LUT4) and
`AUDIO_MIXER` (sw/apps/mod mixes in software without it) are the
cheapest things to remove that a user would notice least.

## Klinge

Machdyne Klinge: ECP5 25F, 512MB DDR3L (MT41K256M16TW-107:P, the
Sechzig ML2's part, on the ML2's balls), two LAN8720A RMII PHYs, two
microSD slots, USB-C, and no video connector, USB host or audio. Pins
are `boards/klinge_v1.lpf`. **Only Ethernet A and microSD A are built**;
the second PHY's and slot's balls are listed, commented, at the end of
that file.

| Board | Device | COMB | FF | DP16KD | MULT18 | PLL | sys_clk | bitstream |
|---|---|---|---|---|---|---|---:|---:|
| Klinge | 25k | 20787 / 24288 (**85%**) | 9366 (38%) | 41 / 56 | 9 / 28 | 2 / 2 | 57.08 MHz | 504,447 B |

Every clock passed: sys_clk (`ddr3_clk_i.sys_clk_o`, divided from the
DDR edge clock) 57.08 MHz against 48, the RMII reference
(`ETH_REFCLK`) 98.19 MHz against 50, the DDR3 PLL's `init_clk` 211.64
MHz. The bitstream fits the DFU layout's gateware region, the minimal
DDR3 BIOS is 1806 of 2048 words, and the jumploader is 99,628 bytes.

**Two PLLs, three jobs.** A 25F has two. DDR3 takes one (`pll2`: the
edge clock and the system clock divided from it) and `pll0` makes the
50 MHz RMII reference, which leaves the FPGA on T4 for both PHYs.
`pll1` makes only video clocks, so `rtl/sysctl.v` now builds it only
with `GPU` and ties its lock high otherwise. Every board with `GPU`
builds exactly what it did before.

**Headless, with a framebuffer.** No `GPU`, but `MEM_VRAM`,
`GPU_RASTER` and `GPU_BLIT` are built: the framebuffer and the line
and blit engines run on sys_clk and need no video clock. The BIOS and
the kernel's panic screen write VRAM unconditionally (an absent VRAM
would be an unacked bus cycle and a hung CPU), and a remote desktop
over ethernet will read it. See [gpu_raster.md](gpu_raster.md) and
[gpu_blitter.md](gpu_blitter.md).

**Software.** The console is USB CDC-ACM. The release target's core
apps are `net console cron`: no `wm` and no `term`, which is a `wm`
window. With no USB host, `z_hid_init()` and the HID interrupt
handlers never see an interrupt and nothing polls the absent ports.
`net` finds the RMII MAC from the feature CSR as on Mozart and Sergei.

**Room.** 85% is past the 75% line, as Konfekt is: re-check `make
timing` after any change. It already carries `MONTMUL_REGS` and
`SHA256` -- this is the networking board -- and a second MAC for
Ethernet B (another `ethmac_rmii` with its own RX slots) is the obvious
next block to want room for. The D-cache write buffer (`DCACHE_WBUF 2`
to `0`, about 540 LUT4 on Lakritz) is the first place to find it.

A 12F Klinge builds with `make BOARD=klinge DEVICE=12k` (same die to
the open-source tools); its zfpga profile then needs `device
LFE5U-12F`.

## Boards with nothing to measure

iCE40 targets in the Makefile (`riegel`, `eis`, `kolibri`, `bonbon`,
`keks`, `kuchen`, `kuchen_v0`, `brot`, `krote`, `icoboard`) have no
block in `rtl/boards.vh`. There is no configuration to synthesise.

`vanille` is an ECP5 target with the same gap: the Makefile names it,
but there is no LPF in `boards/` and no `rtl/boards.vh` block.
(`schoko`, `konfekt` and `minze` were in this list; see above.)

Lebkuchen and Kölsch do have a `boards.vh` block (GateMate, not ECP5).
Their flow is a separate Yosys and nextpnr under the path the Makefile
sets for those boards, not the ECP5 tools these numbers came from.
