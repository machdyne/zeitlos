# Zeitlos Instruction Cache

## Why this exists

The CPU has no cache of its own, so every instruction fetch goes out
over wishbone to main memory. Main memory is slow on every board:

| Backend | Cycles per 32-bit word | Notes |
|---------|------------------------|-------|
| `rtl/mem/qqspi.v` (PSRAM) | ~63 | full command/address/dummy sequence per word; ~40 of those are fixed overhead |
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
selected by address bit 8 — see `cs_cache` in `rtl/sysctl.v`.

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
```

With `ICACHE` undefined the bus reverts to its previous behaviour:
9902 LUT4 / 50 DP16KD against a pristine `82a0e49` baseline of 9894 /
50, the 8-LUT difference being the named `wbm_cpu_padr` wire and the
`mem_instr` connection.

One thing that is NOT optional either way: the `0x7000_01xx` register
window must be acknowledged on every build. `bios.c` and `fs.c` write
the flush register unconditionally, and an address nothing decodes gets
no ack on this bus — `picorv32_wb` then waits forever. Without `ICACHE`,
`csrs_wb` simply keeps the whole `0x7` nibble and answers for it. See
`cs_csrs`/`cs_cache` in `rtl/sysctl.v`.

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
$ iverilog -g2005 -o tb_perf  rtl/tb/tb_cache_perf.v rtl/cache.v && ./tb_perf

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

Ten tests: sequential fetch, line-fill granularity, every word offset
within a line, index conflict thrashing, data reads bypassing,
uncacheable regions bypassing, the stale-line-after-code-overwrite
scenario above, runtime disable, counter accuracy, and a 1500-iteration
random soak with interleaved writes. Passes at 2KB/2-word through
16KB/4-word.

## Bring-up procedure

1. Flash the new bitstream **and** the new kernel/BIOS together. The
   flush calls and the hardware must match.
2. Boot with `cache off` and confirm behaviour is identical to before.
3. `cache on`, then run apps and watch `cache` for the hit rate.
4. If anything misbehaves, `cache off` answers "is it the cache?" in one
   command instead of a re-synthesis. That is the entire reason the
   enable bit exists.

## What this does not fix

Data loads and stores are still uncached and still pay full memory
latency. After this lands, they are the dominant remaining term. In
rough cycles-per-instruction terms on SDRAM:

| Configuration | CPI |
|---------------|-----|
| Before | ~18 |
| With I-cache | ~10.5 |
| With I-cache + open-row SDRAM | ~7.5 |

The next highest-leverage change is open-row tracking in
`rtl/mem/sdram.v` — it costs no BRAM, helps loads and stores as well as
fills, and roughly halves data access cost. A data cache (write-through,
never write-back, for DMA coherency reasons) is worth revisiting after
that, and matters far more on PSRAM boards than SDRAM ones.
