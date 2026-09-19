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
