# Split-port cache (`wb_cache_split`)

`rtl/cache_split.v`: the instruction and data cache for the pipelined
[zeitlos32p](zeitlos32p.md), with a port for each side instead of
one.

[dcache.md](dcache.md)'s `wb_cache` (`rtl/cache_id.v`) serves one CPU
request at a time on one Wishbone port. That is right for picorv32 and
zeitlos32, which have one access in flight and take several cycles per
instruction anyway. A pipelined core wants to fetch every cycle while a
load or store is also in flight. This module gives it that, and
nothing else changes: `wb_cache` is untouched and is still what every
board builds. `rtl/sysctl.v` builds this one only with
`CPU_ZEITLOS32P`, in wb_cache's place.

Status: **in the SOC with `CPU_ZEITLOS32P`.** It is tested in
simulation and has been through out-of-context place and route. It
has not run on hardware.

## Same rules as `wb_cache`

Everything that is a rule rather than a structure is the same, so
software and the rest of the SOC cannot tell the difference:

- Physically tagged; only main memory (`0x4xxx_xxxx`, or
  `0x4000_0000`-`0x5fff_ffff` with `MAIN_512`) is cached.
- Data side **write-through, update-on-hit, no write-allocate**. Memory
  is never behind the cache, so the read-only masters (blitter source,
  mixer) always see current data. **No software invalidation.**
- Every store to main memory **snoops the instruction side** and
  invalidates a matching line.
- Posted stores **drain before any other bus access**: every fill,
  every uncached read or write. "Store a buffer, then write a register
  that starts another master reading it" needs no fence.
- `snoop_stb_i` invalidates both sides by index; a second before the
  first is serviced becomes a full flush.
- The register window at `0x7000_0100`, the same registers and meanings
  ([dcache.md](dcache.md#registers)), reached through the D port. FEAT
  (`+0x2C`) has **bit 11 set** here: software can tell the split cache
  from `wb_cache` (where it reads 0). FAST_HIT (bit 8) always reads 1.

Parameters as `wb_cache`'s: `MAIN_512`, `I_KB`, `I_LINE_WORDS`,
`D_KB`, `D_LINE_WORDS`, `WBUF_DEPTH` (0, 1, 2, 4), `BURST`, `SNOOP`,
`CFG_BASE`. There is no `FAST_HIT`: hits are always answered the cycle
after the request, from the arrays' registered outputs.
`CACHE_TAG_LUTRAM` applies as in `wb_cache`.

## The ports

| Port | Signals | Reaches |
|---|---|---|
| I | `i_req`, `i_adr` → `i_rdy`; `i_ack`, `i_dat` | fetches; main memory cached, anything else uncached |
| D | `d_req`, `d_adr`, `d_we`, `d_sel`, `d_wdat` → `d_rdy`; `d_ack`, `d_rdat` | everything: main memory cached, peripherals/VRAM/BIOS RAM uncached, the register window (`d_cfg_hit`) |

Both take **physical** addresses (after the MTU; the core translates).
The same handshake on both:

- A request is **taken** at a clock edge where `req && rdy`. Address,
  data, `we` and `sel` are sampled then and need not be held.
- It is answered by `ack` for one cycle, with the data for a read: at
  the earliest **the cycle after** it was taken (a hit, or a posted
  store), later when it needs the memory port.
- A port has one request outstanding at a time, but `rdy` is high again
  **in the cycle of its ack**: a stream of hits goes at one a clock.

```
clk      _/~\_/~\_/~\_/~\_/~\_/~\_
i_req    ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
i_adr    < A >< B >< C >< D >
i_rdy    ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾      (all hits)
i_ack    _____‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
i_dat         < A >< B >< C >
```

**The MPU's verdicts** ([rtl/mpu_split.v](zeitlos32p.md#the-mpu); tied
to 0 without one):

| Input | When | Effect |
|---|---|---|
| `i_mpu_veto` | with a fetch, when it is taken | blocked: answered 0 in LOOK, never reaches the bus |
| `d_mpu_reg` | with a load or store, when it is taken | the MPU's registers: answered the cycle after LOOK with `d_mpu_dat` |
| `d_mpu_veto` | the cycle **after** a load or store is taken | blocked: a load reads 0, a store changes nothing |

`d_mpu_veto` comes a cycle late so that the MPU can judge from a
registered copy of the request: its comparators then follow a register,
not the core's address adder. In that cycle the access is in LOOK
(cached) or about to start on the memory port (uncached); either way
it has not yet touched the arrays, the write buffer, the I snoop or
the bus, and the veto stops it there.

`m_instr_o` is high while the memory port carries an access for the I
side (a fetch's fill or an uncached fetch), for anything downstream
that needs to tell fetches from data.

## What runs at once

- I and D **look up in parallel**, each in its own arrays: a fetch hit
  and a load hit complete in the same cycle.
- **Hit under miss.** One memory port serves both sides' misses and
  uncached accesses -- D first, it is the older instruction -- and the
  write buffer. While it fills a line for one side, the other side's
  hits carry on, and stores keep posting until the buffer is full.

## Three hazards, and what is done about each

1. **Store snoop vs fetch.** A store reads the I tags through a
   **second read port of their own** (the tag array is duplicated: one
   more DP16KD, or LUT RAM with `CACHE_TAG_LUTRAM`), so snooping costs
   fetches no cycles. It invalidates the cycle after the store is taken;
   a fetch of that very line in those two cycles may still hit the old
   line. Code is loaded long before it runs, and the kernel's
   `z_icache_flush()` after loading an app (already required on
   I-cache-only bitstreams) closes even that window.
2. **Store hit vs the next D access.** A store merges its bytes into
   the word it read, and writes it back the cycle after it is taken.
   A D request taken in that cycle reads the array in the same cycle.
   For the **same word** it would get the old word. So the merged word
   is kept for one cycle. When the next access's word index matches,
   that word is used instead of what the array read: a load returns
   it, a store merges into it. Nothing waits. `d_rdy` never looks at
   the address, which keeps the core's address adder off the
   take/ready path. (An earlier version held such a request for a
   cycle instead; that put the core's whole address computation in
   front of `d_rdy`.)
3. **Stores during a fill.** The I side can be filling a line while D
   stores post. A store into the line being filled -- or about to be:
   the fill can start in the very cycle the store posts -- reaches
   memory after the fill read it, so the fill is **poisoned**: its words
   still answer the fetch that asked for them, but the line is not
   marked valid. (A D fill cannot meet a CPU store; the D port is
   waiting for it. An external snoop poisons either.)

## The arrays: written a cycle late

Everything above is written for **read-old**: a lookup reads what an
array held before this edge's write, as the simulator models it. A
DP16KD's two ports give no defined result when one writes the address
the other reads in the same cycle, so yosys, left to itself, emulates
read-old in soft logic, comparing the read address with the write
address. The read address is the take mux -- the end of tag → hit →
take -- so on the full SOC that compare was the tail of sys_clk's
critical path, on every seed (45.1-46.5 MHz on sergei_ml2).

So each array is written from registers one cycle late (`*_pwe`,
`*_pwa`, `*_pwd`), and a read that meets that late write is answered
from a register instead (`*_b`, `*_bd`). The address compare is done
for both inputs of the take mux (the incoming address and the held
one) and chosen by the take: one LUT after it, not four.
`(* no_rw_check *)` on the arrays stops yosys adding its own. The
result is read-old exactly: on the bench below every event count is
identical to before the change.

## Register window timing

Register-window accesses are acted on the cycle after they are taken,
from the registered request, and answered the cycle after that (two
cycles, not one). Statistics count a cycle late, from registered
events. Both keep the window's decode and the 32-bit counters off the
path that matters: tag RAM → hit → take → the next lookup's address.
That path is inherent to streaming hits.

The tag compare decides only the acks. `i_dat` and `d_rdat` choose
their source by registered state: a lookup in progress gives the
array's word, otherwise the registered fill or bypass word. On a miss
the array's word is presented with no ack, and nothing uses it.

## Cost and timing

Out of context, with the sergei_ml2 / mozart_ml1 settings (`I_KB 8`,
`D_KB 4`, 4-word lines, `WBUF_DEPTH 2`, `BURST`), yosys 0.33
`synth_ecp5 -abc9`, nextpnr-ecp5 0.6 `--45k --out-of-context`, three
seeds. (nextpnr 0.6 is older than the project's 0.11: compare the two
rows with each other, not with the full-SOC numbers in
[dcache.md](dcache.md).)

| | LUT4 | FF | DP16KD | Fmax, 3 seeds |
|---|---|---|---|---|
| `wb_cache` (`cache_id.v`) | 1,846 | 820 | 8 | 77.3 MHz (seed 1) |
| `wb_cache_split` (before the round-3 changes) | 1,581 | 906 | 9 | 63.7 / 66.2 / 64.0 MHz |
| `wb_cache_split` (now, synthesis only) | 1,698 | 949 | 9 | -- |

Smaller in logic -- two simple port state machines instead of one
FSM that serialises everything -- with one more DP16KD for the duplicated
I tags, and less timing margin, because a hit now feeds the next
request's address in the same cycle. Placed with zeitlos32p, core and
cache together reach 51-55 MHz out of context
([zeitlos32p.md](zeitlos32p.md#size-and-timing)).

## Testing

`rtl/tb/tb_cache_split.v`. Two independent agents drive the ports at
the same time, back to back whenever the cache allows:

- **the fetcher**: sequential runs with jumps over a "code" region, and
  now and then an uncached fetch;
- **the D agent**: in order, one access at a time, the next presented
  as soon as the last is taken (so it goes in on that one's ack, as a
  pipeline's memory stage does): loads and stores of every size, mostly
  in a data region, some into the code region, some into the very line
  the fetcher is fetching; uncached I/O; fences; register-window
  writes switching D on/off, posted writes on/off, I on/off, flushes,
  while everything runs.

Checked: every load returns exactly the last value stored; every
fetch returns a value the word held at some moment from `SNOOP_WIN` (3)
cycles before it was taken until its ack (so a stale line that
survives a store fails); at each fence, slave memory equals the
reference (write-buffer ordering); memory changed behind the cache plus
a snoop pulse is never read stale; no ack without a request; no hang;
Wishbone requests stable while waiting; FEAT bit 11.

It also counts what it exercised -- per seed-1 run: ~106,000
back-to-back fetches, ~61,000 fetch hits during data fills, ~1,500
store-snoop invalidations, ~600 poisoned fills, ~280 same-word
forwards.

**Each mechanism, broken on purpose, fails it** on every seed tried:
no poisoning; no same-cycle poisoning; no store snoop; no same-word
forwarding (132 errors on seed 1); no update on a store hit; fills
not waiting for the write buffer (that one hangs).

```
iverilog -g2005 -o tb_cs rtl/tb/tb_cache_split.v rtl/cache_split.v && vvp tb_cs
iverilog -g2005 -Ptb_cache_split.BURST=1 -Ptb_cache_split.WBUF=4 \
    -Ptb_cache_split.I_LW=8 -Ptb_cache_split.SEED=2 ...
```

Parameters: `I_KB`, `I_LW`, `D_KB`, `D_LW`, `WBUF`, `BURST`,
`MAIN_512`, `SEED`, `NOPS` (60,000 D operations by default; a run is a
minute or two). The configurations run before this was committed are in
the table below. Every one passed on both seeds (1 and 2), before
and again after the round-3 changes; the poisoned-fill counts are
from the first run.

| Configuration | I KB / words | D KB / words | Write buffer | Bursts | 512MB | Poisoned fills (seeds 1, 2) |
|---|---|---|---|---|---|---|
| default | 1 / 4 | 1 / 4 | 2 | no | no | 658, 629 |
| burst | 1 / 4 | 1 / 4 | 2 | yes | no | 630, 548 |
| wbuf0 | 1 / 4 | 1 / 4 | 0 | no | no | 483, 475 |
| wbuf1 | 1 / 4 | 1 / 4 | 1 | yes | no | 504, 492 |
| wbuf4 | 1 / 4 | 1 / 4 | 4 | yes | no | 561, 592 |
| lines8 | 1 / 8 | 1 / 8 | 2 | yes | no | 569, 509 |
| lines2 | 1 / 2 | 1 / 2 | 2 | no | no | 648, 531 |
| mixed | 2 / 8 | 1 / 4 | 2 | yes | no | 499, 480 |
| i4d2 | 4 / 4 | 2 / 4 | 1 | yes | no | 486, 406 |
| big (the boards') | 8 / 4 | 4 / 4 | 2 | yes | no | 482, 440 |
| m512 | 1 / 4 | 1 / 4 | 2 | yes | yes | 818, 770 |
| m512w0 | 1 / 4 | 1 / 4 | 0 | no | yes | 976, 912 |
