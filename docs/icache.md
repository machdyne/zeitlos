# Zeitlos Instruction Cache

## Why this exists

The CPU has no cache of its own, so every instruction fetch goes out
over wishbone to main memory. Main memory is slow on every board:

| Backend | Cycles per 32-bit word | Notes |
|---------|------------------------|-------|
| `rtl/mem/qqspi.v` (PSRAM) | ~63 | full command/address/dummy sequence per word; ~40 of those are fixed overhead |
| `rtl/mem/sdram_kianv.v` | ~5-11 | `KEEP_OPEN` tracks one open row per bank; same-row hits are cheaper |
| `rtl/mem/sdram.v` | ~11 | precharge + activate on *every* access, no open-row tracking |
| `rtl/mem/sram.v` | ~1 avg | free-running ack generator, already fast — **do not cache** |
| `rtl/mem/bram.v` | 1 | not cached, no benefit |

Against picorv32's own ~4 cycles of execution per instruction, fetch
latency is the dominant term in cycles-per-instruction on the SDRAM and
PSRAM boards.

Measured with `rtl/tb/tb_soc.v`, which runs the **real** `picorv32_wb`
against the **real** cache and a memory model, executing a copy of
`k_cpu_report()`'s benchmark loop. The baseline is a build with no
cache module in the path at all, which is what `ICACHE` undefined
actually produces:

| Memory latency | No cache | With cache | Ratio |
|---|---|---|---|
| `sram.v` (~1 cyc avg) | 8.00 MIPS | 8.14 MIPS | **1.02x** |
| 2 cycles | 6.09 MIPS | 8.14 MIPS | 1.34x |
| 5 cycles (a future open-row SDRAM) | 4.41 MIPS | 8.14 MIPS | 1.85x |
| 11 cycles (SDRAM today) | 2.84 MIPS | 8.14 MIPS | **2.87x** |
| 63 cycles (QQSPI PSRAM) | 0.70 MIPS | 8.11 MIPS | **11.6x** |

With the cache, throughput is essentially independent of the backend.
That is the whole point.

Two caveats on those numbers. First, this is a *fetch-bound* benchmark:
the loop is pure register work, so nearly every bus cycle is an
instruction fetch. Real code also does loads and stores, which are not
cached and still pay full memory latency, so whole-program speedup will
be lower — see "What this does not fix".

Second, `rtl/tb/tb_cache_perf.v` reports higher ratios for the same
latencies. It measures something different and easier: its "cache off"
path still routes through `wb_icache`'s own bypass, which costs about a
cycle more per fetch than having no module at all. Trust `tb_soc.v` for
speedup claims; use `tb_cache_perf.v` only for hit rates and relative
geometry comparisons.

## Measured on hardware

Simulation above; this is a real board. `sergei_ml1` (ECP5-45F, 48MHz,
W9825G6KH-6 SDRAM via `rtl/mem/sdram_kianv.v`), 4KB / 2-word lines,
figures from `bench`:

| | CPI | MIPS | IPC |
|---|---|---|---|
| `ICACHE` undefined | 16.7 | 3.09 | 0.06 |
| **4KB, 2-word lines** | **8.33** | **5.79** | **0.12** |

**1.87x**, and better than the ~4.7 MIPS predicted from the CPI model.
Memory stall fell from 12.7 CPI to 4.33, a 2.9x reduction. Beating the
estimate suggests a hit rate above the 95% assumed, and that
`sdram_kianv.v`'s `KEEP_OPEN` row tracking helps sequential line fills
more than the flat ~11 cycles/word model credited.

Read `HITS`/`MISSES` (below) for the real rate before deciding whether
a larger cache or longer lines are worth more BRAM.

Note this is well short of the ~8.1 MIPS the fetch-bound simulation
predicts, and that gap is the point of "What this does not fix": real
code does loads and stores, and those are still uncached.

## Which boards should enable this

**SDRAM and PSRAM boards: yes.** Lakritz, Kölsch, Mozart, ULX3S,
Lebkuchen. The slower the backend, the bigger the win.

**SRAM boards (Obst): no, and it is not enabled there.** `rtl/mem/sram.v`
answers in about one cycle on average — its ack generator free-runs and
never even looks at `wb_cyc_i`/`wb_stb_i`, so a request landing on the
right phase is acknowledged almost immediately. There is essentially no
latency for a cache to hide, so the best case is break-even (1.02x
measured) while still spending EBR and LUTs. Obst also has only 1MB of
main memory, so the BRAM is better spent elsewhere.

This is worth knowing because enabling `ICACHE` on Obst with an early
version of this module made it measurably *slower* — 6.94 to 6.03 MIPS
on real hardware. The cause was a 2-cycle hit path competing with
~1-cycle memory. `FAST_HIT` (below) fixed that, but the underlying point
stands: a cache in front of fast memory buys nothing.

## Design

Direct-mapped, instruction-fetch only, physically tagged, write-through
by virtue of never caching writes at all.

A hit costs **one cycle** (`FAST_HIT=1`, the default: the tag compare
resolves and acknowledges the CPU in the same cycle rather than a cycle
later). Setting `FAST_HIT=0` keeps the cache correct but makes hits
2 cycles, which is a net loss against a fast backend. It exists only as
an escape hatch if the tag-compare-to-ack path ever threatens timing
closure; it costs ~31 LUT4 on ECP5 to have it on.

```
picorv32_wb --> wb_mtu --> wb_icache --> cs_* decode --> slaves
              (translate)  (this)
```

### It sits after the MTU, and that matters

Every app executes at virtual `0x8000_0000`, which `rtl/mtu.v` remaps
to the app's real location during context switches.

If the cache were tagged on the **virtual** address, every app's
`0x8000_0100` would collide in the tag with every other app's
`0x8000_0100`. The same line would mean different code depending on
which process is scheduled, and correctness would require a full
invalidate on every context switch — roughly 732 times a second (see
`sw/os/kernel.c`'s KTIMER handler).

Tagged **physically**, a context switch is just an MTU base register
write. The next fetch translates to a different physical address,
misses cleanly, and fills from the right place. The cache does not
need to know that context switches exist.

### Instruction fetches only

`picorv32_wb` exposes `mem_instr`, which was always present and simply
never wired up in `rtl/sysctl.v`. Using it means:

- Data traffic can never put entries in the cache.
- Stores never need to be watched or snooped.
- There is no dirty state, no write-back, no eviction path.

This is what keeps the whole design small enough to reason about.

### Cacheable region

Only main memory, tested as `(adr & 0xf000_0000) == 0x4000_0000`.

`rtl/boards.vh` makes `MEM_SRAM` / `MEM_SDRAM` / `MEM_QQSPI` mutually
exclusive and `rtl/sysctl.v` decodes all three at the same base, so one
test covers every backend with no `ifdef` inside the cache module.

Everything else bypasses. BRAM is already single-cycle, and VRAM, glyph
memory and every peripheral **must** bypass — caching a UART LSR or
blitter status read would break them.

## Coherency: you must flush after loading code

The only way a stale line can arise is code being *written as data* and
then executed. In this codebase that happens in exactly two places, and
both already call the flush:

1. `fs_load_exec()` in `sw/os/fs/fs.c` — the only app loader, five call
   sites (`sw/os/kernel.c`, `sw/os/sh.c`).
2. `load_zeitlos()` in `sw/bios/bios.c` — memcpy's the kernel from
   memory-mapped flash before jumping to it.

**If you add a third path that writes executable code, it must call
`z_icache_flush()` before jumping to it.**

The failure this prevents is worth stating plainly, because it is
intermittent and allocation-order dependent rather than reproducible:

> App A loads at base X, exits, `k_mem_free()` releases X, then app B
> loads at that same base. The cache still holds A's instructions for
> those physical addresses, so B executes A's code. Nothing about B is
> wrong; it just runs somebody else's program.

Hardware write-snooping would make this correct by construction rather
than by discipline, and is a reasonable later addition. If added, it
**must compare tags** rather than invalidate by index alone — index-only
invalidation would be cleared continuously by ordinary data writes (a
graphics workload writing tens of thousands of pixels per frame would
flush the cache many times per frame and gain nothing).

## Registers

At `0x7000_0100`, sharing nibble `0x7` with the CSRs (`0x7000_00xx`),
selected by address bit 8.

**These are answered inside `rtl/cache.v`, from the CPU address,
upstream of `wb_arbiter_main` — the cache is not a slave on the main
bus.** It cannot be: it is a bus *master*, so routing its own registers
down the bypass path would send them through the arbiter and back to
the module that was waiting to answer them. `sysctl.v` therefore has no
`cs_cache` decode; `csrs_wb` keeps the whole `0x7` nibble and absorbs
the window on builds without `ICACHE`.

These are deliberately **not** part of `rtl/csrs.v`: that block is
documented as read-only and side-effect-free, and a flush register is
neither.

| Address | Register | Description |
|---------|----------|-------------|
| 0x70000100 | CTRL | bit0 = enable, bit1 = flush (write 1, self-clearing) |
| 0x70000104 | HITS | fetch hits since last flush |
| 0x70000108 | MISSES | fetch misses since last flush |
| 0x7000010C | INFO | `{ 0x1CAC, LINE_WORDS[7:0], CACHE_KB[7:0] }` |

INFO carries a magic for the same reason `csrs.v` has one: reading an
address nothing decodes does not fault on this bus (see `sysctl.v`'s
`32'hzzzz_zzzz` default), so a known constant is the only reliable way
to tell "no cache in this bitstream" from "cache present, reporting
these numbers". Always check `z_icache_present()` first.

C-side helpers live in `sw/common/zsoc.h`.

## Shell command

```
> cache              show geometry and hit rate
> cache on           enable
> cache off          disable (forces every fetch to main memory)
> cache flush        invalidate all lines
```

Counters reset on every flush, and `fs_load_exec()` flushes on every app
load — so `cache` reports activity since the last `run`, not since boot.
That is usually what you want when measuring one app, but it explains
why the numbers can look small.

## Configuration

In `rtl/boards.vh`, per board:

```verilog
`define ICACHE
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4
`define ICACHE_FAST_HIT 1
```

`ICACHE_FAST_HIT` selects how a hit is acknowledged. 1 (the default if
a board does not define it) answers combinationally in the same cycle
the tag compare resolves — a 1-cycle hit, but it puts BRAM output ->
tag compare -> `c_ack_o` -> the CPU's `wbm_ack_i` all in one
combinational path. 0 answers from a register instead: functionally
identical, one cycle slower per hit, and that path disappears. Try 0
first if a new board is unstable with the cache enabled.

With `ICACHE` undefined the bus reverts to its previous behaviour:
9902 LUT4 / 50 DP16KD against a pristine `82a0e49` baseline of 9894 /
50, the 8-LUT difference being the named `wbm_cpu_padr` wire and the
`mem_instr` connection.

One thing that is NOT optional either way: the `0x7000_01xx` register
window must be acknowledged on every build. `bios.c` and `fs.c` write
the flush register unconditionally, and an address nothing decodes gets
no ack on this bus — `picorv32_wb` then waits forever. With `ICACHE`
the cache answers it upstream of the arbiter; without, `csrs_wb` keeps
the whole `0x7` nibble and answers for it. See `cs_csrs` in
`rtl/sysctl.v`.

`LINE_WORDS` is a parameter because the right value is backend
dependent. `sdram.v` has no burst path, so a fill costs ~11 cycles per
word regardless and short lines keep the miss penalty down. `qqspi.v`
spends ~40 of its ~63 cycles per word on fixed overhead that a burst
would amortize — so once `qqspi_wb` learns to burst, PSRAM boards will
want longer lines. Don't raise it there before that lands.

### Cost on ECP5 (Lakritz, LFE5U-25F), via `synth_ecp5`

| | Pristine | ICACHE off | +4KB | +8KB |
|---|---|---|---|---|
| DP16KD | 50 | 50 | 53 | 55 |
| LUT4 | 9894 | 9902 | 11520 | 11550 |
| TRELLIS_FF | 4574 | 4574 | 4978 | 4979 |

The 25F has **56** EBR blocks, so 8KB leaves exactly one spare. 4KB is
the safer default; compare hit counters on real workloads before
spending the extra two blocks.

### A note on the valid bits

They are stored as the top bit of each tag word, not in a separate flop
array, and a flush *walks* the lines clearing one per cycle (`S_FLUSH`).

The obvious alternative — valid bits in flops, giving a single-cycle
invalidate-all — was implemented first and measured: an indexed read
plus an indexed set across 512 flops synthesises to a wide decoder and a
wide mux, costing **2867 LUT4** for the module against **514** for the
current version. The walk costs `NUM_LINES` cycles, about 11us at 48MHz
for 512 lines, which is nothing next to the SD card read that triggers
it. Trading 11us for ~2350 LUTs is not a close call.

## Testing

```
$ iverilog -g2005 -o tb_cache rtl/tb/tb_cache.v rtl/cache.v && ./tb_cache
$ iverilog -g2005 -Ptb_cache.FAST_HIT=0 -o tb_cache0 \
      rtl/tb/tb_cache.v rtl/cache.v && ./tb_cache0
$ iverilog -g2005 -o tb_perf  rtl/tb/tb_cache_perf.v rtl/cache.v && ./tb_perf

$ iverilog -g2005 -o tb_cs \
      rtl/tb/tb_cache_sdram.v rtl/cache.v \
      rtl/mem/sdram_kianv.v rtl/tb/sdram_model.v && ./tb_cs

$ cd rtl/tb                       # cycle-accurate IPC, real picorv32
$ python3 gen_prog.py
$ iverilog -g2005 -o tb_soc tb_soc.v ../cache.v ../cpu/picorv32/picorv32.v
$ ./tb_soc                        # baseline: no cache module at all
$ iverilog -g2005 -DUSE_CACHE -DCACHE_KB=8 -DLINE_WORDS=4 \
      -o tb_soc_c tb_soc.v ../cache.v ../cpu/picorv32/picorv32.v
$ ./tb_soc_c
```

`tb_soc.v` takes `-Ptb_soc.MEM_LAT=N` to model a backend of N cycles;
`MEM_LAT=0` reproduces `sram.v`'s free-running ack exactly.

`tb_cache.v` checks every read against a reference model of what memory
actually holds, so a stale or misaddressed hit fails loudly rather than
returning plausible garbage. The slave model uses **randomized** ack
latency (1-12 cycles) — both real backends have variable timing, and a
cache that only works against a fixed-latency slave is a cache that only
works in simulation.

Twelve tests: sequential fetch, line-fill granularity, every word offset
within a line, index conflict thrashing, data reads bypassing,
uncacheable regions bypassing, the stale-line-after-code-overwrite
scenario above, runtime disable, counter accuracy, a 1500-iteration
random soak with interleaved writes, register access producing zero
memory traffic, and `CYC` continuity across multi-word fills. Passes at
2KB/2-word through 16KB/4-word, with `FAST_HIT` either way.

### What `tb_cache.v` cannot see

Its slave is synthetic: it acks on demand and has no banks, no open
rows, no CAS latency, and no opinion about `sel`. **A cache that
violates the SDRAM protocol passes it** — which is exactly what
happened with the `sel`/`we` bug, and the reason that bug survived so
long. `tb_cache.v` passed in every configuration while the hardware
would not boot.

`rtl/tb/tb_cache_sdram.v` closes that gap by wiring the cache to the
real `sdram_kianv.v` and a protocol-checking `rtl/tb/sdram_model.v`
(W9825G6KH / MT48LC16M16A2 class — the two parts are protocol-identical
at this level). It reproduced the hardware failure on its first run,
returning zeros for all 96 fetches.

The model is **incomplete**: read data currently lands one beat
misaligned and it reports a spurious ACT-on-open-bank around refresh,
so it is not yet a clean pass/fail. Finish it before trusting it as a
gate — but prefer it to `tb_cache.v` for anything touching the memory
protocol.

## Bus signalling: `sel` decides direction, not `we`

**`rtl/mem/sdram_kianv.v` decides read-vs-write from `wb_sel_i` and
never reads `wb_we_i`** — the signal appears exactly once, in the port
list. `sel == 4'b0000` means read; a nonzero `sel` names the byte lanes
to *write*. That is picorv32's native `wstrb` convention carried
straight onto the wishbone port, and it is not what a Wishbone master
written from the specification would expect.

So a line fill must drive:

```verilog
m_sel_o <= 4'b0000;   // read. NOT 4'b1111.
m_we_o  <= 1'b0;
```

This cost a full debugging session. The fill originally drove
`sel = 4'b1111` with `we = 0` — "all four byte lanes, reading" by
conventional Wishbone reading. The controller believed `sel`, took its
`WRITE_L` path on every miss, **wrote `0x00000000` over the code it was
supposed to be fetching**, and then pulsed `ready` without a read
having happened, so the cache latched stale `dout` as the fetched word.
The cache poisoned itself and the program in memory simultaneously.

Why it presented the way it did:

- BIOS ran fine — it executes from BRAM, which is never cached.
- The BIOS memory test passed — data traffic carries correct `wstrb`.
- `cache off` (CTRL = 0x2) booted — `S_BYPASS` forwards `c_sel_i`
  verbatim, so bypassed reads carry `sel = 0` and behave.
- ...but then crashed as soon as `init` started apps, because
  `fs_load_exec()` writes CTRL = 0x3, re-enabling the cache.
- `LINE_WORDS` 2 and 4 failed identically, and `FAST_HIT` was
  irrelevant: every fill was a write regardless of geometry.

**If you add another master to `wb_arbiter_main`** — the audio mixer is
next — drive `sel` this way too. Better, make whichever controller you
merge honour `we`/`mem_write` for direction, or assert on a `we`/`sel`
mismatch. A controller that silently writes in response to a
conventionally-signalled read is a trap for every future master.

## Bring-up procedure

1. Flash the new bitstream **and** the new kernel/BIOS together. The
   flush calls and the hardware must match.
2. Boot with `cache off` and confirm behaviour is identical to before.
3. `cache on`, then run apps and watch `cache` for the hit rate.
4. If anything misbehaves, `cache off` answers "is it the cache?" in one
   command instead of a re-synthesis. That is the entire reason the
   enable bit exists.

If it fails on a new board, bisect in this order — it splits the module
along the boundaries that actually separate causes:

1. `bios.c`: write CTRL = `0x2` (flush, enable=0) instead of `0x3`.
   The cache stays in the bus path but bypasses every fetch. Boots =>
   the bypass path and the module's presence are fine; the fault is in
   fill or hit. Note `fs_load_exec()` will re-enable it at the first
   `run`, so expect a later crash rather than a clean boot.
2. `ICACHE_FAST_HIT 0` — removes the combinational hit-ack path.
3. `ICACHE_LINE_WORDS 2` — if 2 works and 4 does not, the fill
   sequence is at fault; if both fail, it is not the geometry.

And check the bus signalling section above before theorising. The
`sel`/`we` mismatch survived four wrong hypotheses (arbiter deadlock,
timing margin, `mem_instr` wiring, `CYC` continuity) precisely because
every one of them was plausible and none was testable from
`tb_cache.v`, which passes in every configuration.

## What this does not fix

Data loads and stores are still uncached and still pay full memory
latency. After this lands, they are the dominant remaining term. In
rough cycles-per-instruction terms on SDRAM:

| Configuration | CPI | Source |
|---------------|-----|--------|
| No cache | 16.7 | measured, sergei_ml1 |
| With I-cache | 8.33 | measured, sergei_ml1 |
| picorv32 floor, zero-wait memory | ~4 | — |

Of the 8.33 achieved, roughly 4.33 is still memory stall, nearly all of
it data traffic. Perfect memory would be ~12 MIPS against the 5.79
measured.

The next highest-leverage change is **burst line fills**. With a
controller supporting native read bursts (see the uploaded
`mt48lc16m16a2_ctrl.v`, `ENABLE_NATIVE_READ_BURST`), a fill becomes one
command fetching 8 words in ~13 cycles instead of 8 separate ~11-cycle
reads. That makes `LINE_WORDS 8` cheap, raising the hit rate and
cutting the miss penalty together.

Burst is worth little *without* the cache: picorv32 issues one fetch and
blocks, with no prefetch buffer for a burst to fill. Modelled at a 95%
hit rate, cache alone is ~10.3 CPI and cache-plus-burst ~9.9 — burst is
a refinement of the miss path, not an alternative to caching.

Open-row tracking is already present in `sdram_kianv.v` (`KEEP_OPEN`)
and is part of why the measured result beat the estimate. A data cache
(write-through, never write-back, for DMA coherency reasons) is worth
revisiting after burst, and matters far more on PSRAM boards.
