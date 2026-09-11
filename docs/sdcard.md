# The SD card, and where its time goes

## Status

**Measured, diagnosed, fixed, and measured again.**

The investigation found that the SPI clock was not the problem, the
card was not the problem, and FatFs was barely the problem: essentially
all of the cost was the CPU executing about twenty-three instructions
per byte. `spim.v` version 1 addresses that directly and multi-block
reads went from **274 KB/s to 848 KB/s** -- see "What was built".

The sections below are in the order the work happened, because the
reasoning is worth as much as the result.

## What the measurement says

`sdbench` on a 48MHz board, `SD_POLL_TIGHT=1`:

| layer | | KB/s | cyc/byte |
|---|---|---|---|
| 0 | raw byte exchange, DIV=1 (12MHz SCLK) | 246 | 190.2 |
| 0 | raw byte exchange, DIV=0 (24MHz SCLK) | 317 | 147.7 |
| 1 | `sd_disk_read()`, one sector per CMD17 | 251 | 186.2 |
| 2 | `sd_disk_read()`, 16 sectors per CMD18 | 274 | 170.8 |
| 3 | `f_read()` through FatFs, 8KB chunks | 239 | 195.4 |

Theoretical wire time at DIV=1 is **32 cycles per byte**. Layer 0 costs
**190**.

Read the layers against each other and the answer falls out:

- **Layer 1 is no worse than layer 0** (251 vs 246). Per-command
  overhead -- select, `wait_ready`, six command bytes, response poll,
  CMD12 -- is nearly free next to the per-byte cost.
- **Layer 2 beats layer 1 by only 9%.** Multi-block streaming barely
  helps, for the same reason.
- **Layer 3 is within 13% of layer 2.** FatFs's cluster walking and
  window buffer are not the problem either.
- **DIV=0 buys 29%, not 100%.** Halving the wire time from 32 cycles to
  16 moved the total from 190 to 148, which is what you would expect if
  ~150 cycles of every byte is not wire time at all.

So: **there is nothing left to find in software above `spi_xchg()`.**
Layer 0 is a two-instruction loop over the register interface and it is
already the floor.

### The old 19 KB/s figure was badly stale

`docs/ramdisk.md` recorded ~19 KB/s. The real figure is 239-274 KB/s --
**about thirteen times better**. That measurement predated the gateware
SPI master, and everything downstream that was designed around it was
designed around a number that no longer existed. Reading the 35KB libz
blob costs about 128ms, not 1.8 seconds.

## What was built, and what it measured

**`spim.v` version 1**, simulated in `rtl/tb/tb_spim.v`:

```
iverilog -o /tmp/tb_spim rtl/tb/tb_spim.v rtl/spim.v && /tmp/tb_spim
```

Two additions, neither needing a bus master, an arbiter port, or a bit
of block RAM. About seventy flip-flops and a handful of LUTs.

1. **A DATA access stalls while busy.** The ack is withheld until the
   shift register is free, so `write DATA; read DATA` is a complete
   exchange and the driver polls nothing. The testbench measures the
   stall rather than assuming it: a second DATA write waits 30 cycles
   at DIV=1, against the 32 a byte takes.
2. **32-bit transfers**, selected by CTRL bit 1. One bus access moves
   four bytes.

`MAGIC` is now `"SPI1"`. That is the part that matters for shipping: a
driver that skips the BUSY poll against an **old** bitstream reads
DATA mid-transfer and gets a byte half old and half new -- corruption
with no error anywhere. `sdmm.c` and `enc28j60.c` each read the magic
once at init and pick their path from it, so an old bitstream with new
software behaves exactly as before. `sdbench` prints which gateware it
measured.

Byte order was the detail most likely to go wrong silently, so the
testbench asserts it directly: writing `0x44332211` must put `0x11` on
the wire first, and a slave sending `0xDEADBEEF` must read back as
`0xEFBEADDE`, so that `*(uint32_t *)buf` needs no software swapping.
`rcvr_mmc()`/`xmit_mmc()` use the wide path for the aligned middle of a
buffer only -- FatFs makes no alignment promise and a misaligned access
here does not fault, it moves the wrong bytes.

### Measured on hardware

| layer | before | after | |
|---|---|---|---|
| 1, single-block CMD17 | 251 KB/s | **645 KB/s** | 2.6x |
| 2, multi-block CMD18 | 274 KB/s | **848 KB/s** | **3.1x** |
| 3, `f_read()` via FatFs | 239 KB/s | **604 KB/s** | 2.5x |

Multi-block went from 170.8 cycles per byte to **55.2**. Against 32
cycles of wire time at DIV=1 that is about 60% of the wire, up from
20%.

For the compiler, which prompted the investigation: reading the 35KB
libz blob now costs about 58ms, down from 128ms -- and from the 1.8
seconds the stale 19 KB/s figure implied.

### Two things the numbers said that the design did not predict

**Stalling is worth far less on its own than expected.** Layer 0, which
measures the 8-bit path, went from 190 to 164 cycles per byte -- 14%.
Obvious in hindsight and the prediction was sloppy: *stalling does not
make the wire faster, it replaces a spin loop with a bus stall.* The
saving is the loop instructions, not the wait. Essentially all of the
3x came from the 32-bit transfers.

**DIV=0 stopped helping, which is a clue rather than a
disappointment.** It was worth 29% before (246 to 317 KB/s) and is now
worth nothing on the 8-bit path (285 vs 283). At ~165 cycles per byte
with the wire costing 16 to 32 of them, that path is bound by the
wishbone round trip and the CPU's load/store cost; the clock disappears
inside the stall.

The **wide** path is different. A 32-bit transfer at DIV=1 is 128
cycles of wire inside a 221-cycle total -- **58% wire**. So DIV=0
should now be worth something substantial on layers 1-3 where it is
worth nothing on layer 0. Estimated ~1.2 MB/s.

**Now set to 0** (`Z_SPISD_DIV_FAST`, `sw/common/zeitlos.h`), and
unmeasured on hardware.

SD cards are specified to 25MHz in default speed mode, so 24MHz is
inside spec rather than an overclock. What it is sensitive to is the
BOARD -- trace length, socket, and the card itself. The failure mode is
not subtle but it is easy to misattribute: CRC failures, `f_read`
returning short, a filesystem that mounts and then does not. **If a
card that worked at DIV=1 misbehaves, put this back to 1 before
suspecting anything else.**

SD card only. `Z_SPIETH_DIV` stays at 1: the ENC28J60 is specified to
20MHz.

### A correction to `sdbench` itself

The first run after the change reported layer 0 at 285 KB/s underneath
a layer 2 of 848 KB/s, which reads as though the upper layers beat
their own floor. They did not: layer 0 was measuring the **narrow**
path while layers 1-3 had moved to the wide one. Two different paths,
not one measurement taken twice.

`sdbench` now reports both 8-bit and 32-bit at each divider, so layer 0
is a floor again. Worth recording because the misleading version was
not wrong about anything it measured -- it had stopped measuring the
thing its own name promised.

## It applies to ethernet too

`spim.v` is instantiated once per device -- the SD card and the
ENC28J60 are the same module -- so the gateware change lands on both.
The driver has to opt in, and `sw/apps/net/enc28j60.c` now does: the
same magic check, the same dropped BUSY poll, and the same 32-bit path
in `eth_read_buffer()`/`eth_write_buffer()`.

Only the buffer paths. A control-register access is two or three bytes
and gains nothing from a wide transfer; RBM and WBM move whole frames,
up to 1500 bytes, and are where the time is.

**One asymmetry that matters:** `Z_SPISD_DIV_FAST` can go to 0 (24MHz)
for the card. `Z_SPIETH_DIV` **cannot** -- the ENC28J60's SPI maximum
is 20MHz. Same gateware module, same divider field, so "raise the SPI
clock" is exactly the kind of change that gets applied to both, and on
the ethernet side it would be out of spec with intermittent corruption
rather than a clean failure as the symptom. The note is in
`zeitlos.h` beside the constant, not only here.

Whether it helps as much is **unmeasured**. There is no `sdbench`
equivalent for the ethernet path and there should be before anyone
claims a number -- which is the whole lesson of this document.

### Still to check

Stalling holds the main bus for a whole transfer (`rtl/arbiter_main.v`
holds a grant until the master drops `cyc`): 32 cycles for a byte at
DIV=1, 128 for a word. Card **initialisation** runs at DIV=59, where a
byte is ~960 cycles -- longer than the audio mixer's sample period.
Initialisation happens at boot before anything is playing, but an
`sdbench` run with audio active would be worth doing before calling
this finished.

## Where the 190 cycles go, and what would fix it

At the measured ~5.8 MIPS on an SDRAM board with the instruction cache
on (`docs/icache.md`), 190 cycles is roughly **23 instructions**.
`spi_xchg()` is a store to DATA, a poll loop over STATUS, and a load
from DATA -- with the call, that is about twenty instructions plus two
or three poll iterations. The arithmetic closes.

It follows that the fix is in **`rtl/spim.v`**, and that no
restructuring of `sdmm.c` can substitute for it. Ranked by ratio per
unit of work:

**1. Stall the bus instead of making software poll.** Today
`wb_ack_o` is asserted immediately on every access and a write during a
transfer is ignored, so software has no choice but to poll. If a read
of DATA (and a write to it while busy) withheld the ack until the
shift register was free, `spi_xchg()` would collapse to one store and
one load -- no loop, no branch, no call. Perhaps 2-3x, for a small,
local gateware change.

Worth being explicit about the cost: stalling holds the wishbone bus
for the duration of the byte, which blocks the blitter's source reads
(`rtl/arbiter_main.v`). In practice the CPU is already spinning in a
poll loop and holding the bus nearly as hard, so this is closer to
honest accounting than to a regression -- but it should be measured
with the blitter busy, not only idle.

**2. Widen the data path.** A small FIFO with a 32-bit register face
means one bus access per four bytes rather than per byte, multiplying
whatever (1) achieves by roughly four.

**3. DMA.** Give the master a length and a memory address and let it
stream. That reaches the wire limit -- about 1.4 MB/s at DIV=1 and 2.9
MB/s at DIV=0 -- and reduces the software cost per sector to a handful
of instructions.

See "If more is wanted later" below for what that would involve. In
the event (2) made it unnecessary at this clock, which was the
prediction: 32 of the 190 cycles were the wire, so anything that got
the software cost near zero landed in the same place. DMA's distinct
payoff is the CPU, not the throughput.

### On adapting a FatFs hardware-SPI sample

`sdmm.c` is indeed ChaN's *bit-banging* sample, and his hardware ports
do specialise the bulk-transfer entry points. But `rcvr_mmc(buff, bc)`
already **is** that entry point here -- it is called once per 512-byte
sector, not once per byte -- and what those ports specialise it into is
a DMA or FIFO transfer, which is item (3) above and needs the gateware
to have one.

Layer 0 settles it: a bare `while (n--) spi_xchg(0xFF);` with no card,
no command and no filesystem in the way already costs 190 cycles per
byte. Any rewrite of `sdmm.c` is still making that call.

The one software change still worth making is unrelated to structure:
`Z_SPISD_DIV_FAST` from 1 to 0 is a free 29%, subject to signal
integrity on the board in question -- see "Trying DIV=0" below.

### The polling change, in retrospect

`token-polls` came back at 4872 for 512 sectors -- about 9.5 per
sector, out of 514 bytes moved. The `dly_us(100)` that used to sit in
that loop was costing roughly 100us per sector against a ~1.8ms sector,
so removing it was worth about 5%. Real, correct, and nowhere near the
main term. The prediction in this document was that it would account
for "perhaps 100-200us per sector against an observed excess of about
25ms"; the excess turned out not to exist, but the estimate of the fix
itself held.

## If more is wanted later: DMA, and whether it should be shared

The stall and the wide transfer got the SD path from 246 to 848 KB/s.
The wire limit at DIV=1 is about 1.4 MB/s, so there is roughly another
1.6x available before the gateware clock is the constraint -- and
`Z_SPISD_DIV_FAST` at 0 is the cheap part of it.

Beyond that the options are a bus master, and this section is what was
worked out before the cheap changes made it unnecessary for now. Kept
because the reasoning survives even though the urgency did not.

### Three shapes, cheapest first

#### A. A wider shift register -- DONE, see above

This section originally said "a small FIFO". **That was wrong**, and
the correction matters because BRAM is the scarcest thing on the
smallest board.

A FIFO is what deeper buffering needs, and deeper buffering is not
where the win is. The win is **one bus access per four bytes instead
of one per byte**, and that needs a wider shift register, not storage.

`spim.v` today holds:

```
reg [7:0] shift_tx;   reg [7:0] shift_rx;
reg [7:0] data_rx;    reg [3:0] bit_cnt;
```

Widen `shift_tx`/`shift_rx`/`data_rx` to 32 bits and `bit_cnt` to 6,
with a width field in CTRL selecting 8 or 32 bits per transfer:

| | cost |
|---|---|
| flip-flops | ~72 more (three 8-bit registers become 32-bit, counter grows 2 bits) |
| LUTs | a handful, for the wider mux and the width select |
| **BRAM / EBR** | **none** |
| DSP | none |

That is the same order as `MONTMUL`, whose own note in
`rtl/boards.vh` says "Costs a handful of DSP slices and ~50 words of
distributed LUT RAM. **NO BRAM.**" This is smaller than that and needs
neither.

**Confirming the headroom, if it is ever in doubt:** `nextpnr-ecp5`
prints its EBR usage in the build log (`Info: Used BRAM: n/m`). But
for this change the question does not arise -- there is nothing to
put in a block.

Combined with the ack stall below, the software cost per 32-bit word
is about the same as per byte is today, so the per-byte cost falls by
roughly four, to somewhere near the 32 cycles of wire time. **That is
wire-limited at DIV=1 with no bus master, no arbiter change and no
BRAM** -- which is the reason to try it before anything in B or C.

Three details that are easy to get wrong and expensive to debug:

- **Byte order.** SPI is MSB-first within a byte, and RISC-V is
  little-endian, so a `uint32_t` loaded from a buffer has the byte
  that must go out FIRST in bits 7:0. The gateware should shift byte
  lanes in little-endian order, MSB-first within each lane, so that
  `*(uint32_t *)buf` works directly. Making software byte-swap instead
  gives back part of what the change just bought.
- **Alignment.** `rcvr_mmc()`'s buffer is not guaranteed 4-aligned.
  Head and tail bytes stay in 8-bit mode; only the aligned middle uses
  the wide path. A 512-byte sector is divisible by four, so in
  practice this is a couple of bytes at each end or none at all.
- **Backward compatibility.** The width field defaults to 8, so an
  unmodified `sdmm.c` and every other current user of `spim.v` behave
  exactly as now. The change is additive, which matters for something
  that has to build on every board.

#### The ack stall -- DONE, see above

**Withhold `wb_ack_o` on a DATA read (and on a write while busy) until
the shift register is free.** Today the ack is immediate and a write
during a transfer is ignored, so software has no choice but to poll.
Stalling instead collapses `spi_xchg()` to one store and one load --
no loop, no branch, no call. Perhaps 2-3x for a handful of lines, no
new state at all.

The cost of stalling is that it holds the bus for the duration of a
byte, blocking the blitter's source reads. In practice the CPU is
already spinning in a poll loop and holding the bus nearly as hard, so
this is closer to honest accounting than to a regression -- but it
should be measured with the blitter busy, not idle.

#### B. `spim.v` becomes its own bus master

Give it a physical address, a length and a direction; it streams to or
from memory and raises a done flag.

- Wire-limited: 1.4 MB/s at DIV=1, 2.9 at DIV=0.
- Per-sector software cost drops from ~12,000 instructions to about
  ten.
- **Costs one arbiter port per peripheral that wants it**, and
  `arbiter_main.v` is a fixed three-port design with a two-bit
  `rotate` -- adding a fourth master is a real edit to a module every
  bitstream depends on.

#### C. One shared DMA engine with peripheral request lines

A single master, N channels, peripherals expose a "ready for a word"
handshake.

- **One arbiter port for every peripheral**, which is the argument for
  it.
- One implementation of the wishbone-master state machine instead of
  three.
- More gateware than B for a single client, less for three.

### Would anything else use it

The realistic client list is short: the SD card and the ENC28J60, and
**both are SPI**. The blitter and the audio mixer are already bus
masters with their own arbiter ports; SPI flash is read once at boot;
the UART is far too slow to matter.

That is not a general-purpose DMA case. It is an argument for a shared
SPI-DMA at most, and a fully general engine sized for a client list of
two is gateware spent on symmetry.

### Three hazards a bus master brings that a FIFO does not

- **Physical addresses only.** `rtl/gpu/gpu_blit.v` already states the
  rule: it "does not go through the MTU, so a virtual 0x8000_0000 app
  address means nothing". Harmless for the SD path, whose buffers are
  kernel-side and physical already -- but it means a future
  `Z_SYS_FS_READ_DMA` taking an app's own buffer would be a bug with no
  diagnostic.
- **A fourth arbiter master.** `rtl/arbiter_main.v` is deliberately
  three ports with a two-bit rotate, and its own header explains it was
  written separately from `arbiter_vram.v` rather than generalised
  because "generalising it would put a working, shipped path at risk to
  save a file". The same caution applies to widening it.
- **Instruction cache coherency, which would be NEW.** The blitter only
  ever READS main memory. An engine that writes it can leave stale
  lines in the instruction cache -- and the most important thing the SD
  path writes is executables (`fs_load_exec()`). Load an app over a
  cached region, jump to it, and the CPU runs the previous occupant,
  intermittently, depending on what was loaded before. Check whether
  `docs/icache.md`'s cache has a flush control at all before starting;
  if it does not, that is part of the work.

## `sdbench`

A kernel shell command (`sw/os/fs/sdbench.c`), because the raw layer is
not reachable from an app: `spi_xchg()` is static, and an app poking the
SPI registers directly would race whatever FatFs is doing —
`docs/filesystem.md` covers why that is fatal rather than merely
untidy.

```
> sdbench            # layers 0-2, no filesystem needed
> sdbench wm         # also layer 3, through FatFs
> sdbench /ram/x     # layer 3 with the card taken out, as a floor
```

### The layers

| | what it adds | what a bad result means |
|---|---|---|
| **0** | bus + gateware only, CS deasserted | the CPU's bus access is the limit; a faster SPI clock will not help |
| **1** | the card, one CMD17 per sector | per-command overhead |
| **2** | CMD18 streaming | the card's own streaming rate |
| **3** | FatFs | cluster walking and the window buffer |

Layer 0 runs at the current divider, at DIV=1 (12MHz) and at DIV=0
(24MHz), and prints the theoretical wire cost alongside. **If DIV=0
measures no faster than DIV=1, hypothesis 2 is confirmed** and the
answer is not a faster clock.

### Reading it

Each line reports KB/s, cycles per byte, and elapsed milliseconds. Under
each of layers 1–3 is a counter line:

```
   cmds 512  sect 512  ready-polls 1088  token-polls 3204
```

Those poll counts are the useful part. Each is one byte clocked at the
current divider, so multiplying by layer 0's measured cycles-per-byte
turns them into a cycle count that can be **subtracted** from the total.
What is left over is neither polls nor payload, and is therefore
software.

- `token-polls` far above one per sector: the card is slow to answer,
  and the tight poll is doing its job of not making it worse.
- `token-polls` ≈ one per sector but the total still large: the time is
  in the payload transfer or in software, not in waiting.
- Any `TIMEOUTS` line at all: something is genuinely wrong with the
  card, and every other figure on the page is suspect.

### Run it with nothing else running

`rdcycle` counts **wall** cycles, so every figure is inflated by
whatever share of the CPU other processes took. Same caveat `sh_bench()`
carries, and it matters more here because the runs are longer. Either
measure before `init`, or `ps` and `kill` first.

## Trying DIV=0

`Z_SPISD_DIV_FAST` is 1 (12MHz). DIV=0 gives 24MHz, which is within the
SD specification for SPI mode and which layer 0 already exercises.

Two things to check before changing the default:

- **Layer 0 first.** If the bus is the limit (hypothesis 2), 24MHz buys
  nothing and adds signal-integrity risk for free.
- **Signal integrity is board and card dependent.** 24MHz on a
  microSD socket at the end of a PMOD ribbon is not the same
  proposition as 24MHz on a board with the socket next to the FPGA. The
  failure mode is intermittent CRC/data errors, which surface as
  corrupted files rather than as anything announcing itself as a clock
  problem.

So: raise it per board if at all, after measuring, and not as a global
default.

## Why this matters more than it did

An on-device C compiler is an I/O amplifier — headers in, binary out,
and for self-hosting, its own source read back. `docs/posix.md`'s Phase
0 puts this first for that reason. The mitigation that already works is
`/ram` (`docs/ramdisk.md`), which is thirty times faster and is where a
build directory belongs; but a compiler still has to read its input from
somewhere and write its output somewhere durable.

## See also

- `rtl/spim.v` — the gateware SPI master, its register map and divider
- `sw/os/fs/fatfs/sdmm.c` — the driver, and the comment above
  `wait_ready()` on the polling change
- `sw/os/fs/sdbench.c` / `sdbench.h` — the benchmark and its counters
- `docs/filesystem.md` — FatFs non-reentrancy, and why this is a kernel
  command
- `docs/ramdisk.md` — the 19 KB/s figure, and `/ram`
- `docs/posix.md` — why this is Phase 0
