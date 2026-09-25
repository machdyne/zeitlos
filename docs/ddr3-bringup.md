# DDR3 bring-up

How DDR3 came to work on Mozart ML2, and what each wrong turn taught.
For how it works now, see [ddr3.md](ddr3.md).

## Summary

Two attempts. The **first** built a controller that was correct -- it
round-tripped data byte-exact on hardware when driven through a vendored
LiteDRAM datapath -- and an ECP5 PHY that never read more than three of
four words of a burst. A long hunt through tuning parameters followed;
much of what it measured turned out to be the diagnostic measuring
itself. LiteDRAM was briefly vendored whole, then reverted.

The **second** kept that controller, rebuilt everything around it, and
reached a booting OS in eight hardware rounds. What finally made the
strobe path work was comparing the PHY line by line with LiteDRAM's ECP5
PHY -- the one datapath that had read correctly on this board.

## First attempt, briefly

- The controller was right from early on. The PHY stalled at three of
  four words, always one byte lane, and which lane depended on the read
  phase.
- **Four controls turned out to be connected to nothing** -- a capture
  delay ignored in one mode, a write tap renamed away, a per-lane gate
  and a per-lane read phase each wired to the other lane's value. Each
  produced a clean, uniform sweep map, read every time as "this axis
  does not matter".
- **A 4-bit sweep drove a 2-bit field**, so maps printed four columns
  four times and looked like sixteen measurements.
- **Refresh landed in the middle of scans**, disturbing a different
  random subset of points each run, so identical bitstreams gave
  different maps. Holding refresh during training fixed repeatability.
- Masked writes never worked at any alignment; unmasked ones did.

## Second attempt

### Recovered, and verified before anything new

The working tree had been lost; the controller, the DRAM model and the
testbenches were recovered from session transcripts. The controller was
kept deliberately at its first, simulation-proven version: of the later
changes, only some were genuine fixes, and the rest were workarounds
for PHY faults that a new PHY should not inherit.

### Controller bugs found in simulation

- **A function in a continuous assignment re-evaluates only when its
  arguments change.** The read-modify-write merge read the store data
  from the module, not as arguments, and froze on the first store's
  data. Synthesis does not share the fault -- simulation and hardware
  would have disagreed.
- **A request was accepted twice.** A read answered from the block
  register acknowledged without leaving idle; on the next cycle the
  master still held the request and the miss path accepted it again.
  That spurious read later acknowledged the master's next transaction,
  a store, which was never performed.
- **Only a quarter of every row was reachable.** The address map had
  five block bits where a 10-bit column needs seven. Every address was
  distinct -- nothing aliased -- and three quarters of the memory was
  silently never used.
- **tRFC was the 1Gb value** on a 4Gb part.
- **Write recovery.** MR0 programmed WR = 6 clocks while the controller
  waited 15ns; it was legal only by 0.7 of a DRAM clock of incidental
  state-machine overhead. One value now sets both.
- **MR0 was a 17-bit concatenation into 16 bits**, correct only because
  the extra bit was a leading zero.

### Test doubles that were wrong

- **The read and write paths each had a one-beat error, in opposite
  directions, and they cancelled.** Every pattern whose halves matched
  passed through both. Fixing the write exposed the read.
- The strobe's preamble edge, high impedance to 0, counted as a beat.
- One loop index was shared by concurrent always blocks.
- **The model's tRFC check compared refresh to refresh only**, so an
  ordinary command issued while still refreshing passed.
- **Its tRP check subtracted unsigned times**, so an ACTIVATE before
  auto-precharge had even started wrapped to a huge number and passed.
- It timed write recovery from a fixed nanosecond tWR, not from MR0.

Each fix to the model was shown to fail a deliberately broken
controller.

### Hardware, round by round

**1. Every setting passed.** The map was 'f' everywhere: all sixteen
offsets at every phase, gate and write delay. A real read path
assembles a lane at exactly one offset. The training writes had left
the block in the controller's block register, which then answered every
training read. The data cache had been switched off; the cache built
into the controller had been forgotten. Fixed with a bypass -- proven by
making the DRAM and the register disagree -- and the trainer now marks
any cell where two offsets pass, so it catches itself.

**2. Nothing passed.** Real now, but "nothing passed" says nothing
about why. Added a gate scan from BURSTDET -- whether each lane's DQS
buffer caught the strobe, which depends on neither data nor writes.

**3. The strobe was caught sporadically, and never on both lanes in the
same cell.** Reads "completed" where no strobe had been seen. Compared
with LiteDRAM's ECP5 PHY, five differences, all in the strobe path:

| | LiteDRAM | here, before |
|---|---|---|
| DQS delays | read MINUS 1, write MINUS 4 | defaults |
| read gate | fixed, 2 cycles, combinational | swept, 3 cycles, registered |
| capture | fixed latency | on DATAVALID |
| write strobe D0..D3 | 0,1,0,1 | 1,0,1,0 |
| CK, commands | through DELAYG | direct |

**The write strobe was inverted**: `4'b0101` is D0 = 1 in Verilog, while
LiteDRAM writes the same constant as `0b1010`, D0 being bit 0. A bit
pattern read left to right and a port list read D0 first run in
opposite directions. Every write landed half a beat out.

**4. Both lanes now caught the strobe, in bands, and training found
settings.** The check passed; the OS did not run. The chosen settings
were on the edge of a narrow window -- and **READCLKSEL turned out to be
circular**: phase 7 to 0 wraps a cycle and the gate compensates by one.
A centre test that stopped at phase 7 called a centre an edge.

**5. The kernel verified clean -- against blank flash.** No OS had been
flashed; blank memory matched blank flash. With a real image, 60% of
words were wrong, identically on both passes: writes were failing. And
each boot behaved differently while the strobe map did not.

**6. The DDR primitives were not reset by the init sequence.** The x2
registers are gearboxes whose beat alignment depends on when they leave
reset relative to the edge clock. LiteX resets them from the init
sequence while the edge clock is stopped; here the SoC reset released
them at an arbitrary moment. Also: READCLKSEL changes are now applied
under PAUSE, as LiteDRAM does.

**7. Zeitlos boots.** Centred on both lanes, kernel verified clean,
256MB reported by the OS, timing 55MHz against 48MHz. (512MB followed;
see below.)

### Making room: where spieth went

The ENC28J60 SPI Ethernet sat at `0x5000_0000`, in the way of 512MB of
main memory. It first moved to `0xb100_0000`, as the second of sixteen
16MB slots in the sdcard's region. That needed two changes beyond the
decode, each of which alone would have broken a board silently: the
sdcard decoded the whole `0xb` region, and `rtl/spim.v` compares its
register numbers against the full address it is given, so each device
must be handed an address relative to its own slot.

It then turned out that the MPU grants apps access per 256MB nibble,
that `0xB` is kernel-only so apps cannot drive the sdcard -- and that
the ENC28J60 driver is an app. The two protection domains had merged.
It moved again, to `0x6100_0000`, beside the RMII MAC: both Ethernet
drivers are apps, nibble 6 is in the default app mask, and the sdcard
keeps `0xB` to itself. The slot scheme came along; the sdcard went back
to exactly its original decode.

**When a device moves, check everything that is keyed on its address,
not just the decode**: register offsets inside the module, the driver's
headers, and access rules such as the MPU's.

### All 512MB

Main memory at `0x4`-`0x5` needed four things to agree, not one: both
caches (whose tags must widen by a bit with the main-memory test, or
`0x4` and `0x5` share lines), the MPU (whose store rules only covered
nibble 4 -- with the default mask any app could have written anywhere
above `0x5000_0000`), the cache snoop, and the DDR3 decode itself. One
define, `MAIN_512MB`, sets a parameter on each, from one place in
`sysctl.v`. Each failure mode has a test that catches it, shown to fail
against the naive change.

Two findings on the way:

- **Overriding a parameter with its default still changes the build.**
  Passing `MAIN_512(0)` to every board made yosys build separately named
  module copies. It is now passed only when the define is set, and every
  other board's `sysctl.v` preprocesses to the same text as before.
- **ABC's mapping moves with any change to its input text.** With the
  three module files edited, ML1 -- logic proven identical, same
  register count -- mapped its combinational logic a few tenths of a
  percent differently, and swapping any one file back gave yet another
  mapping. Deterministic, but chaotic; "logic unchanged" and "netlist
  unchanged" are different claims.

And one the docs nearly got wrong: a smaller DDR3 part still decoding
`0x5` would alias it onto the lower memory while the MPU let apps store
there. The decode now follows the define too.

On hardware: the OS reports `main memory: 512MB` and boots to the shell,
with sys_clk at 54.1MHz against 48MHz, about 1MHz less than before. The
wider tag compare sits on the cache hit path and may account for it, but a
shift that size is also within what mapping differences alone produce
(see above), so it is not attributed.
(A first attempt printed `ddr3 00000000 init timeout` -- ML1's bitstream
had been flashed, which has no DDR3 registers, so STATUS read zero. It is
now in the troubleshooting table in [ddr3.md](ddr3.md#troubleshooting).)

## Lessons

**Before believing a measurement, show it can fail.** Four controls
reached no hardware; a sweep wider than its field aliased; a tRFC check
compared the rule against itself; a tRP check wrapped; a verify compared
blank to blank. Each passed or mapped cleanly. The cheapest check is to
break the thing on purpose and watch the measurement notice.

**A test finds faults only along the axes its pattern varies on.**
Symmetric data hid a beat shift, twice, and two opposite shifts
cancelled completely.

**A diagnostic must be able to succeed partially, and must not rewrite
what it measures.** Score each lane on its own bytes; never let a scan
change the configuration it is scanning; read the raw data when a map
says nothing.

**Know what else could be answering.** A data cache, a controller's
block register, stale memory from the last run, a blank flash image.

**Compare with the thing that works, before sweeping.** The strobe path
was fixed by reading LiteDRAM's source for twenty minutes, after weeks of
sweeping parameters in a structure that differed from it. And
**copying a value does not copy the mechanism**: LiteX's CWL 5 belongs
to a controller that issues writes on the second phase; this one
cannot use it.

**Rules drawn from a design that never worked are suspect.** "Capture
on DATAVALID, never count cycles" came from the first PHY. LiteDRAM
counts cycles, and works.

**Two things that must agree, kept in two places, eventually will not.**
DQ and DM share one output path; MR0 and the controller share one WR;
the read buffer and the merge register are one register.

**A read must complete even when nothing answers.** A hung load
leaves nothing on the console; a timeout leaves a marker and a sticky
bit.

**Plain Verilog traps** worth remembering: a function reading module
signals in a continuous assignment; a 3-bit index plus 3'd2; an unsized
expression in a concatenation; subtracting unsigned times; `4'b0101`
versus D0.
