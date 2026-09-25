# DDR3 main memory

Zeitlos runs from DDR3 on **Mozart ML2** (LFE5U-45F, MT41K256M16TW-107
DDR3L). The controller, PHY, clocking and BIOS training are this
project's own; the PHY's strobe path follows LiteDRAM's ECP5 PHY, the one
datapath known to work on this board. How it got here, and what each
wrong turn taught, is in [ddr3-bringup.md](ddr3-bringup.md).

```
ddr3 dd300083
...
ddr3 tune 08800334 L0:C L1:C
ddr3 check a5c377c3 5a3c5a3c ok
loading zeitlos from rom to main memory ... done.
verify 00000000 bad words, bits 00000000
verify 00000000 bad words, bits 00000000

ZEITLOS
 ...
 - main memory: 256MB
```

sys_clk meets timing at 55MHz against the 48MHz it runs at.

## Contents

- [Build and flash](#build-and-flash)
- [Boards and parts](#boards-and-parts)
- [Memory map](#memory-map)
- [Architecture](#architecture)
- [Registers](#registers)
- [Training, in the BIOS](#training-in-the-bios)
- [Performance](#performance)
- [Verification](#verification)
- [Limitations and open work](#limitations-and-open-work)

## Build and flash

```
make BOARD=mozart_ml2 bios zeitlos_pico soc flash_soc
```

`bios` first: synthesis loads `sw/bios/bios.hex` into the BIOS memory.
A change to the BIOS alone -- which is where training lives -- does not
need synthesis or place-and-route; `soc` patches the new BIOS into the
existing placed design with `ecpbram`:

```
make BOARD=mozart_ml2 bios soc flash_soc
```

The OS image and apps are flashed separately, as on every board. If the
OS region of flash is blank the BIOS still copies it and the verify
still passes -- blank memory matches blank flash -- so a machine that
trains, verifies clean and then does nothing may simply have no OS.

## Boards and parts

A DDR3 board needs, in `rtl/boards.vh`:

```
`define MEM_DDR3
`define DDR3_ROW_BITS 15      // from the part, below
`define DDR3_TRFC_NS 260      // from the part, below
`define MEM 256               // what the OS may use; see Memory map
```

and in the Makefile's board block, the DDR3 sources -- appended there
rather than listed for every board, because they instantiate ECP5 DDR
primitives no other family should read:

```
RTL_PICO += rtl/clk/pll2.v rtl/mem/ddr3.v rtl/mem/ddr3_ctrl.v \
            rtl/mem/ddr3_phy_ecp5.v rtl/mem/ddr3_rdasm.v \
            rtl/mem/ddr3_clk.v rtl/mem/ddr3_phy_init.v
```

`sw/bios/bios.c` and `sw/bios/Makefile` each have a one-line board list
for the minimal DDR3 BIOS ([Training](#training-in-the-bios)); add the
board to both.

The part decides two values, from its datasheet. All three candidates
are x16 with 8 banks and 10 column bits:

| Part | Density | Size | `DDR3_ROW_BITS` | `DDR3_TRFC_NS` |
|---|---|---|---|---|
| MT41K64M16TW-107:J | 1Gb | 128MB | 13 | 110 |
| MT41K128M16JT-125:K | 2Gb | 256MB | 14 | 160 |
| MT41K256M16TW-107:P | 4Gb | 512MB | 15 | 260 |

**tRFC must match the density.** The 1Gb value on a 4Gb part issues
commands to a DRAM that is still refreshing, 128,000 times a second, and
the corruption is intermittent and looks like a tuning problem.
More than 15 row bits does not fit the bus and fails the build with a
named error rather than aliasing.

## Memory map

```
0x4000_0000 - 0x5fff_ffff   main memory, DDR3 (512MB decoded)
0x6000_0000                 ethmac   RMII MAC            Ethernet slot 0
0x6100_0000                 spieth   ENC28J60 over SPI   Ethernet slot 1
0x7000_0700                 DDR3 registers
```

**512MB is decoded, 256MB is used.** The data cache treats only
`0x4xxx_xxxx` as main memory and its tags stop at address bit 27, so the
upper half would be uncached: correct, but slow. `MEM` stays 256 until
the cache's tags are widened by one bit together with its main-memory
test -- widening the test alone would make `0x4000_0000` and
`0x5000_0000` share cache lines ([dcache.md](dcache.md)). On a smaller
part the upper addresses alias the lower ones, which is harmless because
`MEM` tells the OS how much there really is.

**The `0x6` region holds the Ethernet controllers, one 16MB slot
each.** spieth moved there from `0x5000_0000` to make room, beside the
RMII MAC. Each decodes its own slot and receives its address relative
to that slot: both `rtl/spim.v` and `rtl/ethmac_rmii.v` compare their
register numbers against the whole address they are given, so an
address relative to the 256MB region would reach no register in any
slot but the first. For the MAC, in slot 0, the two are identical across
its whole window, which ends below `0x1200`.

Both drivers are apps, and nibble 6 is in the MPU's default app mask,
so the two share one app-accessible region while the sdcard keeps the
kernel-only nibble `0xB` to itself ([mpu.md](mpu.md)).

**Rebuild the OS on boards with spieth** (Lakritz, Obst): its registers
moved in `sw/common/zeitlos.h`, and an old OS image on new gateware
talks to an empty address.

## Architecture

| File | What |
|---|---|
| `rtl/mem/ddr3.v` | subsystem top: memory port, registers, controller, PHY |
| `rtl/mem/ddr3_ctrl.v` | controller: initialisation, refresh, commands, read-modify-write |
| `rtl/mem/ddr3_phy_ecp5.v` | ECP5 PHY: command, write and read paths |
| `rtl/mem/ddr3_rdasm.v` | read burst assembly, simulatable, per-lane offset |
| `rtl/mem/ddr3_clk.v` | PLL, edge clock, system clock, DLL sequencer |
| `rtl/mem/ddr3_phy_init.v` | DDRDLL and the stop/reset/update sequence |
| `rtl/clk/pll2.v` | 48MHz in, 96MHz edge clock and a free-running 48MHz |

### Clocks and reset

The DRAM runs at 96MHz (the edge clock); the SoC at 48MHz. On a DDR3
board **the system clock is divided from the edge clock** (`CLKDIVF`),
so the PHY's two halves are phase-locked by construction. Stopping the
edge clock therefore stops the system clock, which is why the DLL
sequencer runs on pll2's second, free-running output.

The sequence -- freeze the DLL, stop the edge clock, reset, release,
restart, update, pause and release the DQS buffers -- is LiteDRAM's,
step for step. **Its reset is every DDR primitive's reset**, asserted
while the edge clock is stopped and released on the system clock, as
LiteX's ECP5 boards do. The x2 input and output registers are gearboxes
between the two clocks, and which beat lands in which slot depends on
when they leave reset relative to the edge clock. Released by the SoC
reset instead, the same bitstream trained differently on every boot.

### Controller

Closed-page: every access is ACTIVATE then READ or WRITE with
auto-precharge. One command per system cycle, on the first of its two
DRAM clocks -- which makes CL and CWL both even: **CL 6, CWL 6**. (LiteX
runs this board at CWL 5, issuing writes on the second phase; that value
belongs to that mechanism.)

Mode registers, decoded against JEDEC: `MR0 0x0520` (BL8, CL6, DLL
reset, WR 6), `MR1 0x0002` (DLL on, 34 ohm, AL 0, no RTT), `MR2 0x0008`
(CWL 6), `MR3 0x0000`. Every timing is the larger of its time and
clock-count forms -- tRTP, tWTR, tMRD, tCCD and tMOD are clock-count
minimums whose nanosecond forms round to nothing at this clock.
**Auto-precharge write recovery is MR0's WR in clocks**, not tWR in
nanoseconds; both the mode register and the controller's timing come
from one `WR_CK`.

**Stores are read-modify-write; DM is never used.** The data cache is
write-through, so stores arrive as 32-bit words while a DRAM access is
16 bytes. The controller reads the block, merges the word under its byte
enables, and writes all sixteen bytes back. DM is tied inactive. Masked
writes never worked in the first bring-up; unmasked ones did, and this
leaves one mechanism to get right instead of two.

**One block register** serves reads and writes. A burst read keeps all
sixteen bytes, so the three words after the first of a cache line fill
come from the register; a store whose block is held skips its read.
Every write updates it rather than invalidating it; it is dropped only
at reset and when a read times out.

**Reads always complete.** A read the PHY never answers times out after
1023 cycles, returns `0xdeadbe00` and sets a sticky status bit, rather
than hanging the CPU on a load with nothing on the console. With the
fixed-latency capture below, the PHY always answers, so this should not
fire in normal operation; it stays as a guard.

### PHY

The strobe path matches LiteDRAM's `ecp5ddrphy.py`:

| | |
|---|---|
| DQS delays | `DQS_LI_DEL` MINUS 1 (read), `DQS_LO_DEL` MINUS 4 (write) |
| read gate | READ pulse two cycles wide, combinational, at the read enable delayed 4 and 5 cycles for CL 6 |
| read capture | a FIXED latency after the read -- never DATAVALID |
| write strobe | output register toggles constantly, D0..D3 = 0,1,0,1; preamble and postamble shaped by the tristate alone |
| CK and commands | through a `DELAYG` each |

**Write path.** One beat mux drives all eighteen output bits -- sixteen
DQ and two DM -- through the same registers, so DQ and DM cannot be
misaligned against each other. The write delay (TUNE) places the two
data cycles after the WRITE command.

**Read path.** Each DQ input passes `DELAYG` (`DQS_ALIGNED_X2`) into an
`IDDRX2DQA`. Assembly is `ddr3_rdasm.v`: every beat enters a history,
and at the fixed capture point each lane takes eight beats at its own
offset. One four-bit number per lane covers the bit slip within a cycle
and whole cycles either side, so the two lanes -- separate strobes, pads
and FIFOs -- can arrive at different times.

Every read control is per lane: gate, READCLKSEL, offset. Every tuning
field is used at its full width.

## Registers

At `0x7000_0700`, word offsets:

| Word | Name | Bits |
|---|---|---|
| 0 | STATUS (ro) | `[31:16]` 0xdd30 &middot; `[7]` PLL locked &middot; `[6]` read timeout, sticky &middot; `[5:4]` BURSTDET per lane, cleared by each read &middot; `[3:2]` DATAVALID per lane, live &middot; `[1]` PHY ready &middot; `[0]` init done |
| 1 | TUNE (rw) | `[2:0]` write delay &middot; `[6:4]` `[10:8]` read gate, lanes 0 and 1 &middot; `[14:12]` `[18:16]` READCLKSEL &middot; `[23:20]` `[27:24]` read offset |
| 2 | CTRL (rw) | `[0]` hold refresh &middot; `[1]` clear status (self-clearing) &middot; `[2]` training writes &middot; `[3]` reads bypass the block register |

TUNE resets to `0x08822333`: write delay 3, gate 3, READCLKSEL 2,
offset 8. These are starting points; training replaces them. On
hardware it has always settled on **write delay 4**, which is also
LiteDRAM's position.

**A TUNE write is applied under PAUSE.** The DQS buffers are paused for
seven cycles either side of the change, as LiteDRAM holds PAUSE while
software adjusts a delay, and the write is not acknowledged until the
pause has ended -- so the CPU cannot issue a read into a paused buffer.

**The training modes.** *Training writes* skip read-modify-write's read:
merging into whatever the block register holds and writing four words
of a block in sequence leaves it exactly right, whatever the read path
is doing -- without this, a mistuned read path could never be written
to, and so never measured. *Reads bypass* sends every read to the DRAM:
otherwise the register answers training reads and every setting
"passes". *Hold refresh* defers refresh -- the counter keeps running and
the refresh is issued when released.

## Training, in the BIOS

DDR3 boards build a **minimal BIOS**: copy the logo, train, load the OS,
verify it, boot. The full monitor uses 1984 of the BIOS's 2048 words and
training does not fit beside it; the DDR3 BIOS is about 1800. Section
garbage collection drops the unused monitor on DDR3 boards only, and
`bios.lds` keeps `.init`, which holds the reset vector. Every other
board's BIOS is byte-identical. Training changes are BIOS rebuilds of
seconds; once settled it can move into gateware and the full BIOS
return.

The data cache is off for the whole of it, and so is the controller's
block register (*reads bypass*).

1. **Gate scan.** One read per read phase and gate, reporting BURSTDET:
   which lanes' DQS buffers caught the strobe. It depends only on the
   READ command and the gate, not on data, so it says whether the DRAM
   answers at all.
2. **Scan.** For each write delay: per read phase, rewrite a 16-byte
   pattern with every byte distinct (with refresh running, so it is
   never held longer than a row of the scan, far inside DDR3's 64ms
   retention); then per gate and offset, read it twice and score each
   lane on its own bytes. A cell passes when both reads agree and match.
   **A cell where more than one offset passes is impossible for a real
   read path** and is marked `*`: the trainer checks that its
   measurement reaches the DRAM.
3. **Pick**, per lane, the cell whose offset also works one read phase
   either side. READCLKSEL is circular -- phase 7 to 0 wraps a cycle and
   the gate compensates by one -- and the test knows it. `C` is centred,
   `e` an edge.
4. **Check** a word and a single byte through the normal path.
5. **Verify** the loaded kernel against the ROM, twice: the count, the
   first failure and every bit ever wrong (`00ff00ff` is lane 0,
   `ff00ff00` lane 1). The copy itself is robust, so a mismatch comes
   from reading back; if the passes disagree it is read noise.

```
gate scan: burstdet, 1=L0 2=L1 3=both, row=rdclksel col=gate
 r0 00033000
 ...
wd4
 L0 r0 ...8....       offset 8 works at gate 3, read phase 0
 ...
ddr3 tune 08800334 L0:C L1:C
```

On failure it dumps the raw words read at the first gate where both
lanes saw a burst, every offset: the shape of wrong data says more than
a pass/fail map.

## Performance

`bench` on Mozart ML2, picorv32 at 48MHz, 4KB data cache with a 2-entry
write buffer:

```
  int    35.03 cyc    5.00 insn   add/shift/xor
  mul    29.02 cyc    4.00 insn   hardware
  div    63.02 cyc    4.00 insn   hardware
  ld     59.37 cyc    7.00 insn   sequential word loads
  ldr    57.62 cyc    7.00 insn   scattered word loads
  st     46.02 cyc    6.00 insn   sequential word stores
```

In simulation a random read costs 10 controller cycles, and the three
words after the first of a line fill come from the block register at
one cycle each. Stores pay for read-modify-write -- a read and a write -- unless
their block is held; the write buffer hides that from the CPU until it
fills.

## Verification

| | |
|---|---|
| `rtl/tb/tb_ddr3.v` | controller against `ddr3_model.v`, a DRAM model that fails the run on any protocol violation: 34 checks |
| `rtl/tb/tb_ddr3_rdasm.v` | assembly: every misalignment has exactly one offset per lane; every offset selects a different window |
| `rtl/tb/tb_ddr3_reg.v` | register block: a TUNE write is applied mid-pause and acknowledged after it |
| `rtl/tb/check_ddr3_style.sh` | plain Verilog, no local declarations |

See [rtl/tb/README-ddr3.md](../rtl/tb/README-ddr3.md) for how to run
them and what each proves. `make hwmap` reads the DDR3 sources too
(they are appended in ML2's board block) and its golden model is
current.

## Limitations and open work

- **256 of 512MB used.** Widen the data and instruction cache tags by a
  bit, include `0x5` in the cache's main-memory test, raise `MEM` to
  512. See [Memory map](#memory-map).
- **The read windows are narrow**: two or three READCLKSEL phases per
  lane, which is why the BIOS trains on every boot and picks the centre.
- **Training is in the BIOS**, so DDR3 boards lose the monitor. Moving
  it into gateware brings the full BIOS back.
- **Below JEDEC's DLL-on clock.** DLL-on mode specifies tCK no longer
  than 3.3ns; this runs the DRAM at 10.4ns. LiteX runs this board the
  same way.
- **With `PROBE`, every `0x7000_0x00` register tenant -- DDR3's among
  them -- overlaps `0x7F00_0000`** (`hwmap --check`). Pre-existing for
  the others; ML2 does not enable PROBE.
