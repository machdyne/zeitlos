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

The plain Lakritz build uses **55 of the 25F's 56 block RAMs**. VRAM
alone takes 40 of them. So the question for this target was never pins
or logic. It was where to find block RAM for the MAC's buffers.

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
second read port is mapped by yosys as true dual-port DP16KD, **at half
density**: a 2048×9 RAM of that shape takes 2 blocks, and one with a
write-only port takes 1. Nothing ever read `TX_BUF` back.

Measured with `synth_ecp5` on the MAC alone:

| | DP16KD | DPR16X4 |
|---|---|---|
| before | 0 | 1541 |
| 4 RX slots (ML1 boards) | 5 | 3 |
| 2 RX slots (`lakritz_katze`) | 3 | 3 |
| 2 RX slots, `ETH_RXBUF_LUTRAM` | 1 | 515 |

The ML1 boards get this for free: about 1500 LUT-RAM cells back.

### Where the other two blocks come from

Three blocks for the MAC against one free means two have to be found.
They come from the two places that give them up most cheaply, both
measured the same way:

| | before | after | block RAM |
|---|---|---|---|
| `ICACHE_KB` | 4 | 2 | 3 → 2 |
| `AUDIO_FIFO_LOG2` | 10 (1024 frames) | 9 (512 frames) | 2 → 1 |

- **The icache** halves. Fetches from SDRAM miss more often, and
  nothing else changes. `docs/icache.md` describes how to measure hit
  rates on real workloads.
- **The audio FIFO** holds 11.6ms at 44.1kHz. `rtl/sysctl.v`'s note
  found 128 frames too short for a player that loses the CPU for two or
  three 1.365ms ticks; 512 is still about three times that. Software
  reads the depth from the hardware (`z_audio_depth()`), so nothing is
  rebuilt.

That note also claimed 512 and 1024 frames cost the same two blocks.
For this FIFO they do not: one write port and one read port map to the
36-bit `PDPW16KD` mode. The note is corrected.

**Rejected: the receive buffer in LUT RAM.** `` `ETH_RXBUF_LUTRAM ``
needs only one block RAM and no trades. On the full Lakritz build it
took `TRELLIS_COMB` from 19191 to 24766 of 24288, so it did not place.
The option stays in the MAC for a board with LUTs to spare.

### The result

Full builds, `yowasp-yosys` `synth_ecp5 -abc9` and `nextpnr-ecp5` with
the default seed. The first column is the plain Lakritz build (Langkatze,
`` `SPI_ETH ``); the second is `lakritz_katze`, generated by the release
tool's own `zspec.vh` and `.lpf`.

| | `lakritz` | `lakritz_katze` |
|---|---|---|
| DP16KD | 55 / 56 | **56 / 56** |
| TRELLIS_COMB | 19191 (79%) | 19335 (79%) |
| TRELLIS_FF | 9079 | 9187 |
| TRELLIS_RAMW | 103 | 106 |
| `CLK_48` (48MHz) | 44.0 MHz **FAIL** | 52.9 MHz PASS |
| `ETH_REFCLK` (50MHz) | — | 88.1 MHz PASS |

The whole MAC costs 144 logic cells over the SPI master it replaces.

**Read the timing row with care.** The plain build missing 48MHz is
not caused by anything here. It is the untouched tree at the default
seed, and a release build of `lakritz_gpio` or `lakritz_langkatze`
would stop on it without `--allow-timing-fail` or a better seed.
`lakritz_katze` passing is one placement, not a margin. This design
sits close to 48MHz on the 25F and the seed decides which side of it a
build lands. Pin a known-good seed per board, as `ulx3s` does in the
top-level Makefile, before relying on either result.

The RMII domain has plenty of room, at 88MHz against 50.

### Two receive slots, and what software does about it

The ML1 boards buffer four received frames; this target buffers two.
A burst arriving faster than `net` drains it drops sooner, and
`rx_drop_count` (STATUS[7:4]) counts it.

Software is told rather than left to guess. The MAC reports its slot
count in STATUS[15:12], and `net_phy_select()` sizes the TCP receive
window from it: (slots − 1) × 536 bytes. That is three segments on a
four-slot MAC, as before, and one here. Advertising more than the MAC
can hold is what turns a slow link into one that drops segments; see
"The window is bounded by the NIC's receive buffer" in
`docs/networking.md`. A bitstream built before the field existed reads
zero there, and all of those had four slots, so zero means four.

### The real fix

**A denser VRAM.** It is a 9600×32 RAM with a CPU read/write port and a
video read port, which is exactly the shape yosys maps at half density.
So it takes 40 blocks where about 20 would hold it.

Fixing it means either instantiating DP16KD directly in the true
dual-port 2048×9 mode, or giving the CPU and the scanout a shared read
port. Either touches the video path of every ECP5 board, so it is left
as its own change. It would give this target four slots and its full
icache and audio FIFO back, and every other ECP5 board 20 blocks.

---

## Tests

```
$ iverilog -g2005 -o /tmp/rx rtl/tb/tb_ethmac_rmii.v    rtl/ethmac_rmii.v && vvp /tmp/rx
$ iverilog -g2005 -o /tmp/tx rtl/tb/tb_ethmac_rmii_tx.v rtl/ethmac_rmii.v && vvp /tmp/tx
```

Add `-DETH_RX_SLOTS=2` to test this target's size. Both pass at 2, 4
and 8 slots, and with `-DETH_RXBUF_LUTRAM -DETH_TXBUF_LUTRAM`.

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
