# DMA

`rtl/dma.v`, behind `DMA`: one or two channels that move 32-bit words
between main memory and the SD card, the ENC28J60 or the RMII MAC's
buffers. The CPU stops moving those bytes itself. The main bus also
stops being held while each word is on the wire, as a stalled SPI
`DATA` access holds it ([sdcard.md](sdcard.md), "What stalling costs").

It is the shape [sdcard.md](sdcard.md) called C, "one shared DMA
engine with peripheral request lines", cut down to what two SPI masters
and one MAC need.

## Where it is built

| target | `DMA` | channels | channel 0 | channel 1 |
|---|---|---|---|---|
| `lakritz_langkatze` | on | 1 (`DMA_CHANNELS=1`) | SD card and ENC28J60 (SPI), a job at a time | - |
| `sergei_ml1`, `sergei_ml2` | on | 2 | SD card (SPI) | RMII MAC buffers |
| `mozart_ml1`, `mozart_ml2` | on | 2 | SD card (SPI) | RMII MAC buffers |
| `lakritz_katze`, `lakritz_gpio` and the rest | off | - | - | - |

A board without a network gets one channel; `DMA_CHANNELS=1` asks for
one anyway. A single channel serves both SPI masters, and CTRL.PORT
picks the master job by job. The kernel sets PORT, so drivers ask for
the channel by what it is for. If the card and the network meet, the
later one is refused and does its transfer with the CPU. `DMA` needs `GPU_BLIT`,
because there is no main-bus arbiter without the blitter (the build
stops with ``DMA_NEEDS_GPU_BLIT`` otherwise). It is set per target in
`release/targets/*.spec`; `lakritz_katze` stays off because it is the
fullest Lakritz build. "Cost" below has the measured numbers.

## How a word moves

Every word is one bus read and one bus write, through one data
register: read at SRC, write at DST. Either address can stay put (a
device's data register) or step a word at a time (memory, the MAC's
buffers). What paces it is the only difference between the jobs:

| PACE | for | what happens a word |
|---|---|---|
| 0 | the RMII MAC; memory to memory | read, write, as fast as the bus allows |
| 1 | SPI in: an SD sector read, an ENC28J60 `RBM` | the channel starts a 32-bit receive (all ones out); when it is done it reads `DATA`, starts the next receive at once, and writes the word to memory while that one is on the wire |
| 2 | SPI out: an SD sector write, an ENC28J60 `WBM` | a word is read from memory only while the SPI master is idle, then written to `DATA`, which starts the transfer |

Words are in `spim.v`'s 32-bit order: byte 0 of memory goes first on
the wire. A job is whole words only. The driver sends any odd bytes at
either end itself, and starts the job at a word-aligned buffer (it
already does both for the 32-bit path).

The SPI master and the DMA share two wires: a start pulse for a
receive, and the master's BUSY. The data itself goes over the bus. The
channel touches `DATA` only while BUSY is clear, because a `DATA`
access while busy stalls the whole bus, and one master must not do
that to the others. The CPU keeps chip select, the clock divider and
the protocol (commands, the 0xFE token, CRC, the data response). The
channel moves the payload only.

On the bus the DMA is one Wishbone master. It shares port 2 of
`rtl/arbiter_main.v` with whatever already has that port: the audio
mixer, or `rtl/arbiter_gfx.v` on `VRAM_SDRAM`. The sharing goes through
`rtl/arbiter_dma.v`, a two-way round robin with the same grant-per-cyc
policy. `arbiter_main.v` itself is unchanged. The DMA drops cyc after
every read and every write, so the CPU, the blitter and the mixer get
in between, and two busy channels take turns a word at a time.

### Coherence

The DMA's writes to main memory reach the caches' snoop input
(`sysctl.v`, `cache_snoop_stb`; [dcache.md](dcache.md), "Snoop
input"), which invalidates the matching lines. The data cache is
write-through, and its write buffer starts nothing else while a store
is posted ([dcache.md](dcache.md), "Write buffer"). So a buffer the
CPU filled is in memory before the register write that starts a
channel. No flushes are needed in either direction.

## Registers

At `0x7b00_0000`. Word `4c + n` belongs to channel c.

| word | name | |
|---|---|---|
| 4c+0 | SRC | byte address of the first word read (bits 1:0 ignored) |
| 4c+1 | DST | byte address of the first word written |
| 4c+2 | - | reserved |
| 4c+3 | CTRL | write: bit 0 GO (0 stops), bit 1 SRC fixed, bit 2 DST fixed, bits 4:3 PACE, bit 5 PORT (one-channel builds: 0 the SD card's master, 1 the ENC28J60's), bits 31:16 words (1-65535). Read: bit 0 BUSY, bits 31:16 words left |
| 14 | INFO | channels built (1 or 2) |
| 15 | MAGIC | `"DMA1"`, `0x444d4131` |

BUSY covers three things: the words left, a bus cycle under way, and an
SPI transfer the channel started. Once it is clear, nothing of the last
job can land late. Start a channel only while its BUSY is clear. A stop
is for giving up, and BUSY then clears by itself.

**Stores here are kernel-only** (`rtl/mpu.v`, `mpu_split.v`: nibble
`0x7b` is in the fixed kernel-only set). A channel writes physical
memory past every check the MPU makes, so a process that could program
one could write anywhere. Loads are open, so a driver can wait on BUSY
without a syscall.

## Software

```c
#include "zeitlos.h"

if (z_dma_channels() > Z_DMA_CH_SD &&
    z_dma_start(Z_DMA_CH_SD, Z_DMA_SPI_IN, buf, 0, 128) == Z_OK)
	z_dma_wait(Z_DMA_CH_SD);        // a load of BUSY, no syscall
```

`z_dma_channels()` reads MAGIC and INFO, which are plain loads, and
returns 0 on a bitstream without DMA (the CSRs answer that address
there). `z_dma_start()` is `Z_SYS_DMA` (`sw/common/zproc.h`,
`z_dma_args_t`). The kernel (`k_dma()`, `sw/os/kernel.c`) does the
following:

- refuses anyone but a driver (ZEXE driver flag) and the kernel itself,
  and refuses a channel that is busy;
- translates an address in the app's window (`0x8xxx_xxxx`) to
  physical, because the DMA does not go through the MTU;
- fills in the registers from the mode and `dev` (`Z_DMA_DEV_*` in
  `zeitlos.h`):

| mode | `dev` | PACE |
|---|---|---|
| `Z_DMA_SPI_IN` | `Z_DMA_DEV_SD` `0xb000_0000` or `Z_DMA_DEV_ENC` `0x6100_0000` (`DATA`, fixed; PORT from which) | 1 |
| `Z_DMA_SPI_OUT` | the same, as DST | 2 |
| `Z_DMA_DEV_IN` | `Z_DMA_DEV_RMII_RX` `0x6000_0100`, stepping | 0 |
| `Z_DMA_DEV_OUT` | `Z_DMA_DEV_RMII_TX` `0x6000_0a00`, stepping | 0 |

`dev` must be the SD card's `DATA` or in nibble 6, where a driver may
store anyway. Only a job that writes memory (the `_IN` modes) has its
buffer checked as the caller's. One that only reads memory needs it
aligned and nothing more, because a driver may load from anywhere, and
`fs` sends a client's buffer straight from where it lies (through the
grant window). A transfer always ends by itself, so there is no stop call. The kernel
image is 12 bytes under its budget with this in it
([kernel.md](kernel.md)). For that reason `op`, an unknown `mode` and
`ch` are not checked: none of them can reach past the buffer and `dev`
checks.

A kernel from before `Z_SYS_DMA` refuses the call. The drivers take
that, like any refusal, as "do this one with the CPU".

### The drivers

- **`fs`** (`sw/apps/fs/fatfs/sdmm.c`, channel 0): the aligned middle of
  `rcvr_mmc()` and `xmit_mmc()` of 64 bytes or more, which means every
  sector payload. It sets the master's 32-bit mode first (pace 2
  relies on it). After a send it waits for the last word to leave the
  wire before anything else touches the master. `sd_stat.dma_bytes`
  counts what went this way.
- **`net`, ENC28J60** (`sw/apps/net/enc28j60.c`, channel 1): the aligned
  middle of the buffer reads (`RBM`) and writes (`WBM`). After a write
  it waits for the last word to leave the wire, because chip select
  goes up straight after.
- **`net`, RMII** (`sw/apps/net/rmii_eth.c`, channel 1): the whole words
  of a frame, MAC buffer to the caller's buffer (receive) or back
  (send), when the buffer is word-aligned. The odd bytes at the end
  are copied by hand, so nothing past the frame's length is written.

## Testing

- **RTL:** `make -C rtl/tests dma` (`rtl/tb/tb_dma.v`) runs `dma.v` on a
  small copy of the main bus. The bus has a CPU, the DMA beside a
  reader standing in for the mixer (through `arbiter_dma.v`), two real
  `spim.v` masters with an SPI device each, and a memory that answers
  after a random wait. It checks every mode on both channels, both
  channels and the mixer at once, a stop part way, one-word jobs and
  the CPU's own transfers afterwards. It also checks that the DMA
  never touches a busy master's `DATA`, and a one-channel build. Then
  it times a sector.
- **The whole system:** `sim/sys/zsys --dma` models channel 0. It does
  each job the moment it starts and holds BUSY up as long as the wire
  would, so the kernel's checks, the translation and `fs`'s paths all
  run for real. Two checks pass with it:
  - `cinema` streams a film off the card;
  - `sw/apps/dmatest` writes a file, reads it back and compares, at
    sizes and alignments that go through every path. `run dmatest PATH
    KB` picks the file and size. `run dmatest crc PATH` reads a file and
    prints its CRC-32, to compare with the host's.

  (`dmatest` is not on any card: `make -C sw/apps/dmatest`, copy
  `dmatest.bin` to `/apps/dmatest`, `run dmatest`.) The simulator's own
  SPI is instant, so its timings say nothing about the speed-up.
- **On the board**, run these with and without `DMA`:
  - `run dmatest` (bytes match, cycles per byte);
  - `cinema`'s UART lines (`read` per frame);
  - `sdbench`.

## Measured

`tb_dma`, at DIV=0, with memory waits of 1-8 cycles a word and the
mixer reading beside it:

| job | cycles | wire time | |
|---|---:|---:|---|
| a sector in (512 bytes) | 9,617 | 8,192 | 18.8 a byte |
| a sector out | 11,416 | 8,192 | 22.3 a byte |
| 1,500 bytes memory to memory | 13,423 | - | the testbench's slow memory; the MAC's block RAM answers at once |

The CPU's own 32-bit path at DIV=1 measured 848 KB/s end to end
([sdcard.md](sdcard.md)), which is about 57 cycles a byte including
FatFs and the protocol. The DMA gives two things: the payload at wire
speed, and the bus free while it moves. The bus is not held for the
128 cycles of each word, as a stalled `DATA` read holds it. How much
of that a film or a download shows is for the hardware to say.

### On Sergei ML2: no speed-up yet, and why to look elsewhere

`dmatest`, 256 KB, the same card and build with and without `DMA`:

| | write | read |
|---|---:|---:|
| without DMA | 229 cycles a byte | 297 cycles a byte |
| with DMA (2 channels) | 220 | 302 |

All bytes matched both times, so the DMA works. It saves nothing,
though, because the wire is not where the time goes. At DIV=1 a byte
on the wire is 16 cycles and the 32-bit CPU path is about 57 cycles a
byte. A read here costs 300. The rest is above the card: the trip
from the app to `fs` and back, `fs` itself, and FatFs.

To show which part, `fs` now keeps timing counters (`Z_FSOP_STATS`,
[sdcard.md](sdcard.md), "Where a read goes"). `dmatest` prints them
under each phase:

    dmatest: reading, where the time went:
      all of it (here)                 10727525 cycles    40 a byte
      calls to fs, and back             5177713 cycles    19 a byte
      in fs                             5549812 cycles    21 a byte
        of which FatFs and copies        257594 cycles     0 a byte
        of which the card               5292218 cycles    20 a byte
          waiting for its 0xFE           103319 cycles     0 a byte
          the bytes moving              4667071 cycles    17 a byte
      256 calls to fs, 512 card reads (512 sectors, 1 a call), ...

That is zsys, whose card is fast. On Sergei ML2, with DMA, 256 KB:

| reading, cycles a byte | |
|---|---:|
| all of it | 210 |
| the trip to `fs` and back | 134 |
| in `fs`: FatFs and copies | 17 |
| the card: commands | 18 |
| the card: waiting for 0xFE | 7 |
| the card: the bytes | 33 |

(Writing: 156 in all, 114 of it the card.) The trip to `fs` and back
is the biggest part in `dmatest`, because stdio asks for 1 KB at a
time. Each trip costs about 137,000 cycles, nearly 3 ms. `cinema` asks
for a whole record at once, and there the card is the biggest part.
These numbers are not comparable with the 297/302 above: those also
counted filling and checking the buffer.

The DMA's share is the 33 cycles a byte of payload, against roughly
40-55 for the CPU's 32-bit path. The wire at DIV=0 is 16, and
`tb_dma` reached 18.8. The other half is not yet explained: memory
shared with the display, or the gap between words. The saving is real but small next to everything else, and `fs`
spins on BUSY while the DMA works, so the CPU gets nothing back. An
interrupt at the end of a job (below, "Not done") would return that
time only if another process has work. `cinema` waits for its own
read, so it would also need to ask for the next record before decoding
the current frame. `cinema` prints a shorter form every five seconds
([cinema_app.md](cinema_app.md), "What it reports").

### Off and on, on one build

`run dmatest dma off` has `fs` stop using the DMA for the card until it
next starts (`dma on`: again; `Z_FS_STATS_DMA_OFF`, `sd_dma_allow()`),
so one bitstream measures both: play a film each way and compare
`cinema`'s `fs:` lines. On Sergei ML2 the open question is what the DMA
costs around itself. Its payload moves at 33-35 cycles a byte where
the wire is 16, which fits slow single-word writes to DDR3 (the
controller writes a word as a read-modify-write of a burst,
[ddr3.md](ddr3.md)); and a reply from `fs` takes 142k cycles to reach
`cinema` against 55k for the request, which would fit caches flushed
by the DMA's writes ([dcache.md](dcache.md), snoop collisions).
Neither is established.

Measured that way on Sergei ML2 (The General, 8 KB reads): with the
DMA the card took 334k cycles a read, without it 365k -- the DMA saves
about 8% of the card's time, and is kept. The reply took 146k cycles
to come back either way, so the DMA is not why; that is still open.

## Cost

Yosys 0.69 and nextpnr-ecp5 0.11.1 (YoWASP), default seed, through each
target's generated `zspec.vh` and `.lpf`. The engine on its own
(`synth_ecp5 -abc9 -top dma_wb`):

| | LUT4 | CCU2C | FF |
|---|---:|---:|---:|
| `dma_wb`, two channels | 283 | 31 | 222 |
| `dma_wb`, one channel | 146 | 23 | 137 |
| `wb_arbiter2` | 82 | 0 | 2 |

Whole SoC, packed (`TRELLIS_COMB`):

| target | without `DMA` | with `DMA` | | `CLK_48` / main clock | pixel clock |
|---|---|---|---|---|---|
| `lakritz_langkatze`, one channel | 23,070 (94%) | 23,454 (**96%**) | +384 | 56.21 → 58.08 MHz | 50.10 → 47.36 MHz (37.80 needed) |
| `lakritz_langkatze`, two channels | 23,070 | 23,622 (97%) | +552 | not routed to the end | |
| `sergei_ml2`, two channels | 27,737 (63%) | 28,191 (64%) | +454 | 53.81 → 51.63 MHz | 50.25 → 51.35 MHz |

Both built targets close timing at 48 MHz, at one seed each. Lakritz
gets one channel: the second costs ~170 more LUTs there, and is only
needed when the card and the network want the DMA at the same moment.
Then the later job goes the CPU's way, as it would without DMA.
`sergei_ml1` and the Mozarts were not built separately. They are the
same 45k part with about the same use, so their cost should be about
the same.

The first design, with a data register per channel and a 32-bit
transmit and receive bus into each SPI master, was 710 LUT4 for the
engine alone. Two changes took it to 283:
- the shared data register, so the bus write data is that register
  with no mux in front of it;
- the data going over the bus.

## Not done

- **Waiting without spinning.** The drivers spin on BUSY (a load each
  time round). An interrupt at the end of a job would let `fs` sleep or
  serve another request, which matters most for big reads. It needs
  one IRQ line and the kernel's `z_irq_bind()`.
- **Overlap in pace 2.** A sector out reads the next word only after the
  master is idle: about 12 cycles a word lost against the wire. A
  second data register would hide it, at 32 FF and a mux.
- **Snoop collisions.** A second snoop arriving before the first is
  serviced flushes the caches ([dcache.md](dcache.md)). An RMII copy
  writes a word every few cycles. Whether that costs more than the
  copy saves needs measuring on Sergei or Mozart (`dmatest` cannot
  see it; a download can).
