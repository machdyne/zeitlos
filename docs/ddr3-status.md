# DDR3 status

## Round 2: the first hardware scan measured nothing

```
ddr3 dd300083
wd0
 L0 r0 ffffffff        every cell 'f', every row, both lanes
 ...
ddr3 check deadbe00 deadbe00 FAILED
```

The trainer records the LAST offset that passes, so 'f' everywhere
means all sixteen offsets passed at every write delay, phase and gate.
The assembly testbench proves a real read path assembles correctly at
exactly ONE offset. The reads were not reaching the DRAM.

They were answered by the controller's block register. Training writes
leave the block in it, and every training read of that block hit it.
The data cache was switched off for the scan; the cache built into the
controller was forgotten. STATUS says the same: DATAVALID never rose,
and the final check's first real DRAM read timed out (`deadbe00`).

Fixed twice over:

  * `CTRL[3]` makes every read go to the DRAM. Proven in simulation by
    making the DRAM and the register DISAGREE -- a block is written,
    then changed behind the controller's back -- and checking which
    one answers with and without the bypass.
  * The trainer now checks its own measurement. A cell where more than
    one offset passes is impossible for a real read path, so it prints
    as `*` and is never chosen. **A diagnostic that cannot tell when it
    is measuring the wrong thing will eventually report the wrong
    thing with total confidence.**

## Memory map: spieth moved, 512MB decoded

```
0x4000_0000 - 0x5fff_ffff   main memory, DDR3 (was 0x4 only)
0xb000_0000                 sdcard   SPI device slot 0
0xb100_0000                 spieth   SPI device slot 1 (was 0x5000_0000)
```

Sixteen 16MB SPI-device slots in the 0xb region. Two things had to
change beyond the decode, and either alone would have broken a board
silently:

  * **The sdcard decoded the WHOLE 0xb region**, so spieth at
    0xb100_0000 would have had two devices answering. It now decodes
    its own slot.
  * **`rtl/spim.v` matches its register offsets against the full
    address it is given.** Given an address relative to the 256MB
    region, spieth's registers at 0xb100_0000 become word 0x0040_0000
    onward and match nothing. Each slot's device now gets its address
    relative to its slot.

`tools/hwmap/hwmap --check` reports exactly the same errors before
and after (all pre-existing upstream), and no overlap between the two
slots. For each non-DDR3 board the preprocessed design differs from
upstream only in those decode and address lines -- 5 lines on ML1,
Sergei ML1 and ULX3S, 9 on Lakritz and Obst, which have spieth.

**The OS must be rebuilt for Lakritz and Obst.** `sw/common/zeitlos.h`
moved `reg_spieth_*` and `reg_eth` to 0xb100_0000; an old OS image on
new gateware talks to an empty address.

### Why ML2's MEM is still 256

All 512MB is decoded, but the data cache treats only `0x4xxx_xxxx` as
main memory and its tags stop at address bit 27. So the upper 256MB is
UNCACHED: correct, but slow. Widening the cache's main-memory test to
0x5 WITHOUT widening its tags would make 0x4000_0000 and 0x5000_0000
share cache lines and return each other's data. Raise MEM to 512
together with a one-bit tag widening in `rtl/cache_id.v`, and check
its timing then.

### Timing

Changes on the bus path: the two 0xb decodes compare 8 address bits
instead of 4 (the file already has 8-bit decodes, e.g. `cs_probe`);
the DDR3 decode compares 3 instead of 4. Everything else is wiring.
Last full run: sys_clk 52.11MHz against 48MHz. Compare the next one.

## Build and flash

```
make BOARD=mozart_ml2 bios zeitlos_pico soc flash_soc
```

`bios` first: synthesis loads `sw/bios/bios.hex` into the BIOS memory,
and ML2's BIOS is the minimal DDR3 one.

## What the console should show

```
ZB
ddr3 dd300007                    STATUS: magic, init done, PHY ready,
                                 PLL locked
wd3                              the write delay that worked
 L0 r0 ..4.....                  lane 0: read phase x gate, the offset
 ...                             that assembled the lane, or '.'
 L1 r0 ..4.....
ddr3 tune 0443xxxx centred       settings applied; "centred" = the
                                 same gate and offset also work one
                                 read phase either side
ddr3 check a5c377c3 5a3c5a3c ok  word and single-byte stores through
                                 the normal read-modify-write path
loading zeitlos from rom ...     then boot
```

Paste whatever appears. The maps are the useful part when it does not
work: they show which lane, which phases and which gates came close.

## The minimal BIOS

DDR3 boards build `BIOS_DDR3`: logo, training, load, boot. The full
monitor is 1984 of 2048 words and the training does not fit beside it;
without it the BIOS is 1473 words. Section garbage collection drops
the unused monitor on DDR3 boards only, and `bios.lds` KEEPs `.init`
so the reset vector survives it. Every other board's BIOS is
byte-identical.

Training changes are therefore BIOS rebuilds of seconds, not gateware
builds. Once it is settled it can move into gateware and the full BIOS
return.

## Training, and why it is shaped this way

  * Writes use a training mode that skips read-modify-write's read.
    With the read path mistuned -- the state training exists to fix --
    nothing could otherwise be written, and nothing measured.
  * Refresh is held during each scan row and released while the
    pattern is rewritten, so it is never held long: DDR3 keeps data for
    64ms without refresh, and a full scan of mostly timing-out reads
    takes over a second.
  * Each lane is scored on its own bytes, twice: one lane working is
    visible while the other is not, and a stale capture that happens
    to match once does not pass.
  * The data cache is explicitly off for the scan.

## Parts

`DDR3_ROW_BITS` and `DDR3_TRFC_NS` in `rtl/boards.vh`, per part:

| Part | Size | Row bits | tRFC |
|---|---|---|---|
| MT41K64M16TW-107:J | 128MB | 13 | 110ns |
| MT41K128M16JT-125:K | 256MB | 14 | 160ns |
| MT41K256M16TW-107:P | 512MB | 15 | 260ns |

**ML2 reaches 256MB of its 512MB.** The main-memory window at
`0x4000_0000` is 256MB wide and `0x5000_0000` is spieth, so 15 row
bits do not fit the bus; `MEM` is 256 so the OS is never told about
memory it cannot reach. Setting 15 fails the build with a named error
rather than aliasing silently. Reaching all 512MB is a memory-map
decision.

## Spec note

JEDEC's DLL-on mode specifies tCK no longer than 3.3ns; this runs the
DRAM at 10.4ns (96MHz). LiteX runs this same board the same way, and
it worked there, but it is outside the letter of the specification.

## Watch in place-and-route

A partial run here estimated `sys_clk` at 43.8MHz after placement,
against 48MHz. Upstream reports ML1 at 60.3MHz with the data cache,
so if full routing also fails, suspect the new logic first: the read
assembly's 16-way history mux, or the controller's merge.

## Round 3: nothing passes -- so, first, is the strobe seen at all?

```
ddr3 dd300083
ddr3 training FAILED
```

With the bypass in place the reads reach the DRAM, and no cell passes on
either lane. "Nothing passed" says nothing about WHY, so the BIOS now
prints, before training:

  * **a gate map from BURSTDET.** Each lane's DQSBUFM sets BURSTDET when
    the read gate catches a DQS burst. That depends only on the READ
    command and the gate -- not on the data, not on whether any write
    landed -- so it answers the first question on its own: is the DRAM
    answering reads, and when? Each cell: the lanes that saw a burst
    (1, 2, 3 = both), then `v` if the read completed.
  * **on failure, a raw dump**: at the first gate where both lanes saw a
    burst, all sixteen offsets, all four words. The shape of wrong data
    says more than a pass/fail map.

## A real protocol bug, found by reading MR0 back

Auto-precharge write recovery is MR0's WR, in CLOCKS. MR0 said 6 (62.5ns);
the controller waited tWR as 15ns. It was legal only by 0.7 of a DRAM
clock of incidental state-machine overhead. Both now come from one
WR_CK, and the margin is 4.7 clocks.

The model had missed it twice over: it timed recovery from a fixed
nanosecond tWR instead of MR0, and its tRP check subtracted two unsigned
times, so an ACTIVATE arriving before recovery had even FINISHED wrapped
to a huge number and passed. Both fixed; with recovery deliberately
removed from a copy of the controller, the model now reports 16
violations where before it reported none.

MR0 was also a 17-bit concatenation into 16 bits that worked because the
extra bit was a leading zero. Now sixteen, field by field; the value,
0x0520, is unchanged. All four mode registers decode correctly against
JEDEC: CL6, CWL6, BL8, DLL on and reset, 34 ohm drive, AL0, MPR off.

## Round 4: compared against the datapath that worked

```
gate scan
 r0 0v1v1v0v0 0 0 0      reads "completing" (v) where no strobe was seen
 r1 0v1v1v2v0 0 0 0      BURSTDET sparse; the two lanes never agree
```

The vendored LiteDRAM datapath read byte-exact on this board in an
earlier session, so the PHY was compared with LiteDRAM's
`litedram/phy/ecp5ddrphy.py` line by line. Five differences, all in the
strobe path, all now matched:

| | LiteDRAM (read correctly here) | before |
|---|---|---|
| DQSBUFM delays | read MINUS 1, write MINUS 4 | defaults, 0 |
| read gate | fixed, 2 cycles, combinational | swept, 3 cycles, registered |
| capture | fixed latency after the read | on DATAVALID |
| write strobe D0..D3 | 0,1,0,1 | 1,0,1,0 -- INVERTED |
| CK and command outputs | through DELAYG | direct |

**The write strobe was inverted.** `4'b0101` is D0=1, D1=0 in Verilog;
LiteDRAM writes the same constant as `0b1010`, where D0 is bit 0. A bit
pattern read left to right and a port list read D0 first run in
opposite directions. Every write landed half a beat out, so no read
could ever have matched. Confirmed in the synthesised netlist this
time, not the source.

**DATAVALID is no longer used for capture.** It fired at gates where
BURSTDET had seen nothing, so reads "completed" empty. The design
document's rule -- "capture on DATAVALID, never count cycles" -- came
from a datapath that never worked; LiteDRAM counts cycles and does.
The assembly keeps its per-lane offset, now covering bit slip and whole
cycles from a fixed capture point; its testbench shows each lane finds
exactly one offset and that a lane arriving a cycle later simply needs
one four smaller.

Reset defaults are now the known-good configuration: gate 3 (LiteDRAM's
position for CL=6) and READCLKSEL 2 (what LiteX's own training found on
ML2, plus or minus 1).

## Round 5: training succeeds, the OS does not run

```
gate scan                 both lanes now catch the strobe together, in bands
 r0 00033000
 r5 00330000
wd4
 L0 r6 ..6.....           lane 0: two phases wide
 L0 r7 ..6.....
 L1 r0 ...8....           lane 1: r6, r7 at gate 2 AND r0 at gate 3
 L1 r6 ..8.....
 L1 r7 ..8.....
ddr3 tune 08606324 EDGE
ddr3 check a5c377c3 5a3c5a3c ok
loading zeitlos ... done.         -- and then nothing
```

The five LiteDRAM differences fixed the strobe path: BURSTDET now sees
both lanes together, training finds settings, and a word and a single
byte store correctly through the normal path.

**READCLKSEL is circular.** Stepping phase 7 to 0 moves a whole cycle and
the gate compensates by one: lane 1 works at r6 and r7 with gate 2 and
at r0 with gate 3, same offset. r7 is its centre. The old test looked at
r6 and a nonexistent r8 and called it an edge, and the pick landed on
r0. The new test wraps, with the gate adjustment, and on this exact map
chooses r7 for lane 1 (centred) and r6 for lane 0 -- whose window is two
phases wide, so it has no centre, and says so.

**Why the OS might not run: the check tested two words.** Settings at the
edge of a narrow window give occasional bit errors, and the kernel is
256KB. The BIOS now compares the loaded image against the ROM, twice,
and reports the count, the first failure and every bit that was ever
wrong. The copy itself is robust -- each block's last write carries all
four words from the merge register -- so a mismatch comes from reading
back, which is what the OS does with every instruction.

## Round 6: writes fail, and differently on every boot

With a real OS in flash, the verify now shows what the earlier clean
result hid: that was blank flash matching blank memory.

```
verify 00009aa8 bad words, bits ffffffff, first @00000000 got ffffffff
```

39,592 of 65,536 words wrong, identically in both passes, and the first
still reading all-ones -- the previous blank image, never overwritten.
WRITES are failing. And the same bitstream behaves differently per
boot: one finds nothing at any write delay, the next finds cells --
while the BURSTDET map stays identical. Strobe detection is stable;
where each beat lands is not.

**The DDR primitives were not reset by the init sequence.** The x2
registers are gearboxes between the 96MHz and 48MHz clocks, and which
beat lands in which slot depends on when they leave reset relative to
the edge clock. LiteX's ECP5 DDR3 reference board (lattice_versa_ecp5)
feeds the init sequence's reset into the system reset, which is every
DDR primitive's RST:

```python
AsyncResetSynchronizer(self.cd_sys, ~pll.locked | self.reset)
self.comb += self.crg.reset.eq(self.ddrphy.init.reset)
```

Here it reached CLKDIVF only; the primitives were released by the SoC
reset counter, at an arbitrary point relative to the edge clock. Now
all 83 share one reset net, asserted by the init sequence while the
edge clock is stopped and released on the system clock -- confirmed in
the synthesised netlist. The sequencer's timeline itself already
matched LiteDRAM's step for step.

**TUNE writes are applied under PAUSE**, as LiteDRAM holds PAUSE while
software adjusts a module's delay. The register write is acknowledged
only once the pause has ended, so the CPU cannot read into a paused
buffer. Simulated: acknowledged after 16 cycles, applied mid-pause,
never changed while running.
