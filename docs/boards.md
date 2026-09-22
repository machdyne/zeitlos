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
