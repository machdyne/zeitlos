# Katze RMII ethernet PMOD

[Katze](https://machdyne.com/product/katze-ethernet-pmod/)
([hardware](https://github.com/machdyne/katze)) is a 10/100 ethernet
PMOD built around a Microchip LAN8720A PHY. It is the first PMOD
Zeitlos supports that speaks RMII rather than SPI.

That difference decides everything else. Langkatze carries a complete
MAC — the ENC28J60 — and the FPGA only talks SPI to it. Katze carries
only the PHY, so the MAC has to be in the fabric: `rtl/ethmac_rmii.v`,
the same MAC `mozart_ml1` and `sergei_ml1` use for their on-board
LAN8720As. Software needs nothing new. `net` already picks the RMII
driver from the feature CSR on any bitstream built with `` `ETH_RMII ``.

| | |
|---|---|
| **Release target** | `lakritz_katze` |
| **Spec files** | `release/hw/pmods/katze.spec`, `release/targets/lakritz_katze.spec` |
| **MAC** | `rtl/ethmac_rmii.v`, at `0x6000_0000` |
| **Driver** | `sw/apps/net/rmii_eth.c`, chosen at runtime |

```
$ release/zrelease build v0.0.x --targets lakritz_katze
```

---

## Pinout

| PMOD pin | Katze | port | dir | |
|---|---|---|---|---|
| 1 | E_TXD0 | `ETH_TXD[0]` | out | |
| 2 | E_TXD1 | `ETH_TXD[1]` | out | |
| 3 | E_TXEN | `ETH_TX_EN` | out | |
| 4 | E_CRS_DV | `ETH_CRS_DV` | in | mode strap, pull-up |
| 7 | E_RXD0 | `ETH_RXD[0]` | in | mode strap, pull-up |
| 8 | E_RXD1 | `ETH_RXD[1]` | in | mode strap, pull-up |
| 9 | E_RST_N | `ETH_RST_N` | out | PHY reset, active low |
| 10 | CLK50 | `ETH_REFCLK` | in | 50MHz, from the PMOD |

On Lakritz, port A, that is B11 B12 B13 B14 / A11 A12 A13 A14. All
eight signal pins are used, so the port is Katze's alone. On Lakritz
that costs nothing: the console is the USB-C socket (`` `USB_CDC ``).

**The reference clock comes from the PMOD.** A 50MHz oscillator on the
module drives the PHY and pin 10 from one net. So `ETH_REFCLK` is an
input here. `` `ETH_RMII_DRIVE_REFCLK `` is Sergei's option for a PHY
that expects the FPGA to supply the clock. It must stay off for Katze,
or the FPGA and the oscillator would drive one net against each other.

`frequency.10 = 50 MHZ` in the PMOD spec emits
`FREQUENCY PORT "ETH_REFCLK" 50 MHZ;`, which is what makes nextpnr time
the MAC's 50MHz domain at all. nextpnr promotes the pin to a global
clock on its own. A14 is not a dedicated clock pin, which costs a
little insertion delay and nothing else at 50MHz.

**Pull-ups on the three mode straps only.** The LAN8720A samples
MODE[2:0] from RXD0, RXD1 and CRS_DV when its reset is released. 111
means all speeds and duplexes, auto-negotiation on. That is the only
configuration this MAC will ever set, because it has no MDIO, and
Katze does not bring MDC/MDIO to the connector anyway. The Katze LiteX
example and `boards/mozart_ml1.lpf` pull up the same three pins.

The MAC holds `ETH_RST_N` low for about 87ms after configuration, so
the straps are sampled well after the FPGA's pull-ups are live.

**No MDIO means no link status.** The MAC assumes whatever the PHY
negotiated. That is 100M full duplex against any modern switch, and it
is the same assumption the ML1 boards make.

---

## Fitting it into a 25F

The MAC needs 5 block RAMs: four receive slots and the transmit
buffer, the same as on the ML1 boards. The plain Lakritz build used
**55 of the 25F's 56**. So this target was first a block RAM problem,
and solving it turned up two things that were wasting block RAM across
the tree.

### The MAC was not using block RAM at all

Until this target existed, `rtl/ethmac_rmii.v` had **no** buffer in
block RAM. Both buffers were read into the bus data register through a
mux, and the TX engine also read `txbuf` combinationally. An ECP5 EBR
has a registered read port and nothing else, so yosys built all 80Kbit
from LUT RAM: 1541 `TRELLIS_DPR16X4` and a wide read mux per bit, for
the MAC alone. The 45F boards had room and nobody noticed.

Three changes fixed it, each with the logic unchanged:

- **RX write:** moved into its own always block with one enable and
  one address. The condition, edge and data are the same as before.
- **TX read:** now a registered prefetch one byte-time early
  (`tx_word_q`). This is safe because `tx_byte_idx` does not move during
  the four dibit cycles of the byte before it.
- **CPU reads:** the RX buffer read is registered and unconditional,
  and a registered select picks between it and the register file. Bus
  timing is unchanged.

`TX_BUF` also became write-only. A RAM with a read/write port plus a
second read port gets built as **two copies** (see "VRAM" below for
why): a 2048×9 RAM of that shape takes 2 blocks, and one with a
write-only port takes 1. Nothing ever read `TX_BUF` back.

Measured with `synth_ecp5` on the MAC alone:

| | DP16KD | DPR16X4 |
|---|---|---|
| before | 0 | 1541 |
| 4 RX slots (every board) | 5 | 3 |
| 2 RX slots | 3 | 3 |
| 2 RX slots, `ETH_RXBUF_LUTRAM` | 1 | 515 |

The ML1 boards get this for free: about 1500 LUT-RAM cells back.

### VRAM was two copies of itself

VRAM is 640×480 at 1bpp: 300Kbit, which is about 20 blocks. It took
**40**. yosys's `memory_libmap` debug output shows why:
`replicates (for ports): 2`. It was building two complete copies.

The cause is read-during-write semantics, not size.

- **What the Verilog says.** VRAM has a CPU port that reads and writes,
  and a graphics port that reads. A graphics read on the same edge as
  a CPU write to the same word returns the *old* data.
- **What the block RAM guarantees.** A DP16KD guarantees old data only
  for a read on the *same* physical port as the write
  (`READBEFOREWRITE`). yosys's ECP5 library says nothing about a read
  on the other port.
- **What yosys does about it.** To keep the Verilog's meaning, it puts
  every read on the write's port. Two readers on one port means two
  copies.

`(* no_rw_check *)` on the array tells yosys the collision result is
don't-care. The CPU's read and write then share one port, the graphics
port gets the other, and there is one copy. `rtl/mem/vram.v` carries
the full note.

What that gives up is one edge case: a graphics read on the same edge
as a CPU write to the same word returns unknown data. The ECP5 memory
guide (FPGA-TN-02204, WRITEMODE) says the *read* data may be unknown;
stored data is at risk only when two ports *write* the same address,
and VRAM has one writer. The only graphics-port reader is `gpu_video`'s
scanline refill, so the worst case is one 32-pixel word on one line
wrong for one frame. Before, it was stale for one frame. CPU-side reads
are unchanged.

Measured on the plain Lakritz build (default seed):

| | before | `no_rw_check` |
|---|---|---|
| DP16KD | 55 / 56 | **35 / 56** |
| TRELLIS_COMB | 19191 | 19503 |
| TRELLIS_FF | 9079 | 8957 |
| `CLK_48` Fmax | 44.0 MHz | 44.7 MHz |

The critical path is in the blitter in both builds. Every ECP5 board
that builds `` `MEM_VRAM `` gets the same 20 blocks back.

The same memory survey found only one other duplicated RAM in the SoC:
the PicoRV32 register file. That one is genuine, because the CPU reads
two registers and writes one every cycle.

### The result

Full builds with `yowasp-yosys` (`synth_ecp5 -abc9`) and
`nextpnr-ecp5`, default seed. `lakritz_katze` uses the release tool's
own generated `zspec.vh` and `.lpf`.

| | `lakritz`, before the VRAM fix | `lakritz`, after | `lakritz_katze` |
|---|---|---|---|
| DP16KD | 55 / 56 | 35 / 56 | **40 / 56** |
| TRELLIS_COMB | 19191 (79%) | 19503 (80%) | 19390 (79%) |
| TRELLIS_FF | 9079 | 8957 | 9078 |

Four receive slots, a 4KB icache and a 1024-frame audio FIFO: the same
as every other RMII board, with 16 blocks to spare.

**Timing:** the plain Lakritz build misses 48MHz at the default seed
both before and after the VRAM fix (44.0 and 44.7 MHz), with the
critical path in the blitter each time. That is pre-existing. Pin a
known-good seed per board, as `ulx3s` does in the top-level Makefile.
An earlier `lakritz_katze` configuration met 48MHz (52.9 MHz) with the
RMII domain at 88.1 MHz against 50, but that was one placement, not a
margin.

**Two fixes for 0.0.5.** The `lakritz_katze` build for 0.0.5 placed at
94% COMB and missed 48MHz at 47.2 MHz, again on the blitter: the
start-of-draw address `final_y * 80` had been mapped to a MULT18X18D,
so the path ran from the VRAM arbiter's ack through the DSP (3.9ns plus
the trip to its site) into the address adder. `rtl/gpu/gpu_blit.v` now
writes it as `(y << 6) + (y << 4)`. With that and the RMII change below,
the same target at the default seed placed at 92% COMB and met **57.75
MHz** on CLK_48, with ETH_REFCLK at 83.4 MHz; the critical path moved
to the icache tag compare. Still one placement -- but 9.75 MHz of it.

The RMII change is about the pins, which nextpnr does not time at all:
paths from `ETH_RXD`/`ETH_CRS_DV` show up only as `<async>`, so they
can never fail the build. They were long. The RX engine used the pins
directly, and `ETH_CRS_DV` reached the receive buffer's write port
after 8.6ns of routing -- out of a period in which the LAN8720A takes
most of the 20ns to drive the signal. On the TX side, `ETH_TXD` and
`ETH_TX_EN` were decoded from the TX state through LUTs straight onto
the pins. `rtl/ethmac_rmii.v` now puts one flop on `eth_refclk` behind
every RX pin and in front of every TX pin; RX and TX each gain one
REF_CLK of latency, uniformly, so the frame on the wire is unchanged.
The pin-to-first-flop path is 4.5ns in the build above. Both MAC
testbenches (`rtl/tb/tb_ethmac_rmii.v`, `tb_ethmac_rmii_tx.v`,
including the TX-to-RX loopback) pass with it under Verilator.

### Fitting after 0.0.5: no data cache

Since 0.0.5 the Lakritz block gained `USB_HOST` and `COLOR`, and
dropped `MONTMUL`, `MONTMUL_REGS` and `SHA256` to pay for them
(docs/boards.md, "Lakritz with game-mode colour"). That leaves the
plain board at 96-98% COMB with `SPI_ETH`, and the RMII MAC costs more
fabric than `SPI_ETH` does. `lakritz_katze` at `b663103` stopped
placing: **24709 / 24288 (101%)** on one machine, 23668 (97%) on
another with the same commit and a different toolchain build.

The target now leaves out the data cache ([dcache.md](dcache.md)).
Software already probes for it (`z_dcache_present()`), as it must on
Obst, so loads simply go to SDRAM. Colour, USB host and the audio
mixer stay. Same commit, Yosys 0.69+260 / nextpnr-ecp5 0.11.1-54,
default seed:

| | all of Lakritz | without `DCACHE` |
|---|---|---|
| TRELLIS_COMB | 23668 (97%) | **22743 (93%)** |
| DP16KD | 44 / 56 | 41 / 56 |
| `CLK_48` (48 required) | not routed | **48.94 MHz** |
| `ETH_REFCLK` (50 required) | | 100.59 MHz |
| pixel clock (25.20 required) | | 40.87 MHz |

`CLK_48` passes by 2% on that one placement: expect some seeds to
fail, and re-check `make timing` after any change to this build.

What else was measured on the same build, as COMB saved against
23668, for when this target needs more:

| Remove | Saves | Notes |
|---|---|---|
| `GPU_BLIT` | 4,717 | not possible today: wm has no software fallback |
| `AUDIO` (all of it) | 2,249 | |
| `USB_HOST` -> `USB_HID` | 2,040 | keyboard and mouse only |
| `AUDIO_MIXER` | 1,612 | sw/apps/mod mixes in software |
| `GPU_RASTER` | 1,152 | not possible today: no software fallback |
| `DCACHE` | 925 | taken |
| `COLOR` | 431 | |
| `ICACHE_KB`/`DCACHE_KB` 4 -> 2 | 96 | |

### History: the version that did not have the VRAM fix

Before VRAM was fixed, the board had one block RAM to spare and this
target had to make room for the MAC. It built two receive slots
instead of four, halved the icache (`ICACHE_KB=2`) and halved the audio
FIFO (`AUDIO_FIFO_LOG2=9`). That fitted exactly: 56 of 56. None of it
is needed now.

Two things from that version stay:

- **The MAC reports its slot count** in STATUS[15:12], and `net` sizes
  the TCP window from it: (slots − 1) × 536 bytes. Every board builds
  four today, so that is three segments everywhere. But `ETH_RX_SLOTS`
  is a per-build define, and a build with fewer slots now advertises
  less instead of promising the peer more than the MAC can hold.
- **The audio FIFO measurement.** 512 frames takes one block, not two
  as `rtl/sysctl.v` used to say, because a FIFO with one write and one
  read port maps to the 36-bit `PDPW16KD` mode. The note there is
  corrected.

**Rejected at the time: the receive buffer in LUT RAM.**
`` `ETH_RXBUF_LUTRAM `` took a full Lakritz build from 19191 to 24766
`TRELLIS_COMB` of 24288, so it did not place. The option stays in the
MAC for a board with logic to spare and no block RAM.

---

## Tests

```
$ iverilog -g2005 -o /tmp/rx rtl/tb/tb_ethmac_rmii.v    rtl/ethmac_rmii.v && vvp /tmp/rx
$ iverilog -g2005 -o /tmp/tx rtl/tb/tb_ethmac_rmii_tx.v rtl/ethmac_rmii.v && vvp /tmp/tx
```

Both follow `-DETH_RX_SLOTS=n`, and both pass at 2, 4 and 8 slots and
with `-DETH_RXBUF_LUTRAM -DETH_TXBUF_LUTRAM`.

`tb_ethmac_rmii_tx.v` is new. The TX path had no test, and a
one-cycle-early prefetch is the kind of change that goes wrong by
exactly one byte. It checks:

- preamble and SFD;
- every data byte, at frame lengths ending in each of the four byte
  lanes and at a full 1514 bytes;
- the FCS, against an independent CRC32;
- the inter-frame gap;
- a byte-lane write landing on the wire;
- a loopback that sends a frame back through the receive FIFO.

Its frame checks also pass against the MAC as it was before these
changes, so the old and new MACs are shown to behave the same. With
the prefetch address deliberately off by one, it fails.

Both files now build as plain Verilog-2005. The MAC's empty `#()`
parameter list, which iverilog rejects, is gone.

---

## Building by hand

`boards/lakritz_v0.lpf` carries the Katze constraints commented out,
next to the Langkatze block they replace. `rtl/boards.vh`'s Lakritz
block lists the define changes. The release target does all of it from
one spec and is the supported path.
