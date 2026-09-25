# Zeitlos Data Cache

An optional data cache, built as one module with the instruction cache:
`rtl/cache_id.v` (`wb_cache`). A board chooses one of three builds:

| Build | Defines | Module |
|---|---|---|
| No cache | neither | CPU wired straight to the bus |
| Instruction cache | `ICACHE` | `rtl/cache.v` (`wb_icache`), see [icache.md](icache.md) |
| Instruction + data cache | `ICACHE` + `DCACHE` | `rtl/cache_id.v` (`wb_cache`) |

`DCACHE` implies `ICACHE`. The I-cache-only build is untouched by any of
this: it still builds `wb_icache`, byte for byte.

**Enabled on every cache board**, in `rtl/boards.vh` and in the release
specs: Mozart ML1, Sergei ML1 and ULX3S with the 2-entry write buffer,
Lakritz without it (`DCACHE_WBUF 0`) for headroom. See
[Boards and releases](#boards-and-releases).

Status: **running on mozart_ml1 hardware** (picorv32), see
[Measured on hardware](#measured-on-hardware). Every configuration also
passes the testbenches below with picorv32 and zeitlos32, including
against the real bus arbiter with competing masters, and the mozart_ml1
build meets timing across three placement seeds, as does every release
target (see [Boards and releases](#boards-and-releases)). zeitlos32 and
the boards other than mozart_ml1 have not run it on hardware yet.

## Summary

- **No software invalidation, anywhere.** Stores are write-through and
  update the cache; the instruction side snoops every CPU store; posted
  writes reach memory before any other bus access. The only master that
  writes main memory is the CPU, and all its stores pass through here.
- **Loading code needs no flush.** App A overwritten by app B at the
  same address runs B, because B's stores invalidated A's lines as they
  were written. The existing `z_icache_flush()` calls stay (they are
  required on I-cache-only bitstreams) and cost ~11us per app load.
- **Measured in simulation, over I-cache only: 1.19x on picorv32, 1.29x
  on zeitlos32** (with write buffer and SDRAM burst fills). Less than
  the proposal's estimate, for reasons given under
  [Performance](#performance-simulation).
- **Cost on mozart_ml1, placed and routed: +1,579 logic cells, +419
  FF, +3 DP16KD** for 4KB of data cache, a 2-entry write buffer and
  SDRAM burst reads. **Timing: 60.3 MHz against 48 MHz required** (the
  I-cache-only build reaches 55.5 MHz); the critical path is no longer
  in the cache.

## Configuration

In `rtl/boards.vh`, per board:

```verilog
`define ICACHE
`define ICACHE_KB 8
`define ICACHE_LINE_WORDS 4
`define DCACHE              // unified wb_cache instead of wb_icache
`define DCACHE_KB 4         // default 4
`define DCACHE_LINE_WORDS 4 // default 4
`define DCACHE_WBUF 2       // posted-write entries: 0, 1, 2, 4 (default 2)
`define SDRAM_BURST         // 4-word SDRAM bursts for line fills
```

`ICACHE_FAST_HIT` applies to both sides. `SDRAM_BURST` is independent
of `DCACHE` in the controller (`rtl/mem/sdram_kianv.v` `BURST`), but
only `wb_cache` issues bursts, so on its own it changes nothing. It is
ignored on boards whose main memory is not SDRAM. Lines must be a
multiple of 4 words to be burst-filled; other sides fill word by word.

To back out on a board: comment `SDRAM_BURST` (bursts off), set
`DCACHE_WBUF 0` (no write buffer), or comment `DCACHE` (the previous
`wb_icache` build exactly).

## Architecture

```
picorv32_wb / zeitlos32_wb --> wb_mtu --> wb_cache ---------> wb_arbiter_main
                             (translate)  I tags/data    one memory port:
                                          D tags/data    fills (bursts),
                                          write buffer   write-through,
                                                         bypass
```

**One module, not two.** Chaining a data cache behind the instruction
cache makes every hit in the second one pay the first one's registered
bypass, turning a 1-cycle hit into 3-4 cycles. Here both lookups run in
parallel from the incoming address, both hit in 1 cycle, and the two
sides share the memory port, bypass path, register window and flush
walker. It is also the only arrangement in which the instruction side
can see the data side's stores.

**Physically tagged, after the MTU**, for the reasons in
[icache.md](icache.md): context switches need nothing, and the kernel
touching an app's memory through its physical address hits the same
lines the app uses through `0x8000_0000`. zeitlos32 translates
internally (`MTU_ON_BUS=0`); the cache sees physical addresses either
way.

**Only main memory (`0x4xxx_xxxx`) is cached.** Everything else,
including BRAM, VRAM and every peripheral, bypasses.

> **512MB boards** (`MAIN_512MB`, [ddr3.md](ddr3.md#memory-map)) set the
> `MAIN_512` parameter here and in `rtl/cache.v`: main memory is then
> `0x4000_0000`-`0x5fff_ffff`, and the tags widen by one bit with it
> (`I_TAGB`, `D_TAGB`, `TAG_BITS` all follow the parameter). The two
> must move together -- widening the main-memory test alone would make
> `0x4000_0000` and `0x5000_0000` share lines and return each other's
> data -- which is why one parameter sets both. `tb_cache_id.v` with
> `-Ptb_cache_id.MAIN_512=1` checks it, with half its addresses moved
> into `0x5` onto the same lines.

### Data side

| | |
|---|---|
| Write policy | write-through |
| Store miss | no allocate: memory only |
| Store hit | update the line: the lookup already read the old word, so the store's byte lanes are merged in and a whole word written (no byte-enable RAM) |
| Organisation | direct-mapped, `DCACHE_KB`, `DCACHE_LINE_WORDS` |
| Hit | 1 cycle (`FAST_HIT`), same as the I side |
| Reset state | **disabled**; the kernel enables it |

### Instruction side

Identical to `wb_icache`, plus the store snoop: every store to main
memory looks up the I tag for its address (the same lookup that reads
the D tag), and a valid matching line is invalidated. The compare is on
the full tag, so data stores that merely share an index with cached
code do not disturb it. This works whether or not the I side is
enabled, so the I side can never be stale.

### Write buffer

`DCACHE_WBUF` entries. A store to main memory is acknowledged as soon
as it is buffered (and the line, if cached, updated), and drains in the
background. The rule that keeps this invisible:

> **The memory port starts nothing else while any posted store is
> outstanding.**

Every fill, every uncached read and every uncached write waits for the
buffer to empty. So "fill a buffer in main memory, then write a register
that starts the blitter (or audio mixer) reading it" is ordered with no
fence: the register write cannot reach the bus before the data.

A load that hits sees the updated line; a load that misses fills only
after the buffer drains. Stores are never posted to anything but main
memory.

### Snoop input

`snoop_stb_i`/`snoop_adr_i` invalidate both sides' line at an address,
by index. `rtl/sysctl.v` drives it from any master **other than the CPU**
completing a write to main memory. None exists today (the blitter
source port and the audio mixer are read-only, `rtl/dma.v` is a stub),
so it is constant zero; it exists so that a future writing master is
coherent without anyone remembering to make it so. A second snoop
arriving before the first is serviced turns into a full flush: slower,
never wrong.

### Why no software invalidation is needed

Main memory is written by exactly one agent, the CPU, and every CPU
store passes through `wb_cache`:

| Agent | Writes main memory? |
|---|---|
| CPU (picorv32 / zeitlos32) | yes, through `wb_cache` |
| GPU blitter, source port | no (`gpu_blit.v`: `s_we_o = 1'b0`) |
| GPU blitter destination, rasterizer | no, VRAM bus |
| Audio mixer | no (`audio_mixer.v`: `m_we_o = 1'b0`) |
| `rtl/dma.v` | stub, not instantiated |
| Ethernet, USB, SD, SPI | no, programmed I/O |

Write-through means memory is never behind the cache, so the read-only
masters always see current data; update-on-hit means the cache is never
behind memory; the I snoop means code written as data is never fetched
stale.

## Registers

At `0x7000_0100`, answered inside the cache upstream of the arbiter,
exactly as for `wb_icache` ([icache.md](icache.md)). The first four are
unchanged, so the BIOS, `fs.c` and `zar.c` work against either module.

| Address | Register | Description |
|---|---|---|
| 0x70000100 | I_CTRL | bit0 enable (reset 1), bit1 flush |
| 0x70000104 | I_HITS | |
| 0x70000108 | I_MISSES | |
| 0x7000010C | I_INFO | `{ 0x1CAC, I_LINE_WORDS, I_KB }` |
| 0x70000110 | D_CTRL | bit0 cache loads, bit1 flush, bit2 posted writes; **reset 0**. Setting bit0 from 0 also flushes D |
| 0x70000114 | D_HITS | cached loads that hit |
| 0x70000118 | D_MISSES | cached loads that filled a line |
| 0x7000011C | D_INFO | `{ 0x1DCA, D_LINE_WORDS, D_KB }` |
| 0x70000120 | LOADS | data loads from main memory (write clears) |
| 0x70000124 | STORES | stores to main memory (write clears) |
| 0x70000128 | STALL | cycles a CPU request waited, including the 1-cycle hit (write clears) |
| 0x7000012C | FEAT | bits 7:0 write buffer depth, bit 8 FAST_HIT, bit 9 BURST, bit 10 SNOOP |
| 0x70000130 | WBFULL | cycles a store waited on a full write buffer (write clears) |
| 0x70000134 | I_SNOOPS | I lines invalidated by stores (write clears) |

Hit/miss counters reset when their side is flushed.

> **Never write D_CTRL without checking `z_dcache_present()`.**
> `wb_icache` decodes only address bits [3:2], so on an I-cache-only
> bitstream `0x70000110` *is* I_CTRL, and writing 0 there silently turns
> the instruction cache off. The D magic (`0x1DCA`) is deliberately not
> the I magic, so D_INFO reads `0x1CAC` there and the check fails
> correctly. The helpers in `sw/common/zsoc.h` all check first.

C helpers (`sw/common/zsoc.h`): `z_dcache_present()`, `z_dcache_kb()`,
`z_dcache_line_words()`, `z_dcache_wbuf_depth()`, `z_cache_burst()`,
`z_dcache_set(enable, posted)`, `z_dcache_enabled()`,
`z_dcache_posted()`, `z_dcache_flush()`, `z_cache_stats_clear()`.

The kernel calls `z_dcache_set(true, true)` right after
`k_soc_report()` in `main()` (`sw/os/kernel.c`) and logs the geometry.

## Shell command

```
> cache              geometry, hit rates and traffic, both sides
> cache on|off       instruction cache
> cache don|doff     data cache (loads)
> cache wbon|wboff   posted writes
> cache flush        flush both sides
> cache clear        zero LOADS/STORES/STALL/WBFULL/I_SNOOPS
```

`don`/`doff` and `wbon`/`wboff` are independent, which is what makes
bisecting on hardware two commands rather than a re-synthesis.

## SDRAM burst reads

`rtl/mem/sdram_kianv.v` with `BURST=1` serves Wishbone B4 incrementing
read bursts: `CTI=3'b010` on a read at a 16-byte aligned address starts
a 4-word burst, the master holds STB and advances its address on each
ack, and the last beat carries `CTI=3'b111`.

The controller issues **four BL2 READs two cycles apart** to the open
row. With BL=2 each READ's data ends exactly as the next one's begins,
so the part streams eight halfwords back to back:

```
k:     0    1    2    3    4    5    6    7    8    9   10
cmd:  RD0   -   RD1   -   RD2   -   RD3   -
data:                 w0l  w0h  w1l  w1h  w2l  w2h  w3l  w3h
ack:                           w0        w1        w2       w3
```

This differs from the upstream KianV controller (the uploaded
`mt48lc16m16a2_ctrl.v`), which programs BL8 in the mode register and
terminates single reads with BST. Chained BL2 READs were chosen because
the mode register, single-read path and write path stay exactly as
they are: `BURST=1` only adds a path, `BURST=0` is bit-for-bit the
controller that was brought up on these boards, and each burst word is
captured with the same command-to-sample distance as a single read.

Line fills: 4 words in ~12 cycles in the open row, against ~32 as four
single reads. Measured at the controller (random mix of 4- and 8-word
bursts including row misses and refresh): **~22 cycles against ~46**.

`rtl/arbiter_main.v` is unchanged: `sysctl.v` forwards the cache's CTI
only while the arbiter's grant is master 0 (`wbm_cti`), and the other
masters never burst.

## Measured on hardware

mozart_ml1, picorv32, `DCACHE_KB 4`, `DCACHE_WBUF 2`, `SDRAM_BURST`.
Boot log:

```
 - icache: 8KB, 4-word lines
 - dcache: 4KB, 4-word lines, 2-entry write buffer, burst fills
 - cpu: picorv32 6.88 MIPS @ 48 MHz (0.14 IPC)
```

(6.66 MIPS before. That figure is `k_cpu_report()`'s register-only
loop, which the data cache barely touches.)

`bench`, cycles per operation (lower is better):

| | before (`ICACHE` only) | with `DCACHE` | |
|---|---|---|---|
| int | 35.03 | 35.02 | unchanged, as expected |
| mul | 29.03 | 29.02 | |
| div | 63.02 | 63.01 | |
| ld (sequential word loads) | 81.78 | **58.43** | 1.40x |
| ldr (scattered word loads) | 81.76 | **57.39** | 1.42x |
| st (sequential word stores) | 52.53 | **46.01** | 1.14x |

`cache` after running for a while:

```
dcache: 4KB, 4-word lines, 2-entry write buffer, burst fills
  D rate:   90.8%        (96,186,918 hits / 9,744,038 misses)
  loads:    105,953,178
  stores:   39,308,603
  wb full:  0 cycles
```

A 90.8% load hit rate is what the simulation assumed. `wb full: 0`: in
real use the 2-entry write buffer never filled, so stores always
completed in one cycle and a deeper buffer would buy nothing.

## Performance (simulation)

`rtl/tb/tb_cache_soc.v` runs a real CPU against the real cache, the real
SDRAM controller and `rtl/tb/sdram_model.v`, executing
`rtl/tb/cache_soc/prog.c`: code loading, register loop, insertion sort,
byte memcpy, linked-list walk, string hashing and recursion. The same
checksum is required from every configuration. Cycles for the whole
program (lower is better):

| Configuration | picorv32 | vs I | zeitlos32 | vs I |
|---|---|---|---|---|
| No cache | 1,463,222 | 0.55x | 1,207,512 | 0.51x |
| I-cache (`wb_icache`) | 799,236 | 1.00x | 618,700 | 1.00x |
| I+D, stores blocking (`DCACHE_WBUF 0`) | 723,036 | 1.11x | | |
| I+D, 2-entry write buffer | 675,010 | 1.18x | 495,262 | 1.25x |
| I+D, write buffer, `SDRAM_BURST` | **657,934** | **1.21x** | **479,080** | **1.29x** |

(The `DCACHE_WBUF 0` and zeitlos32 write-buffer rows were measured
before every miss was routed through `S_MEMREQ`, which adds one cycle
per miss; the rows measured both ways moved by 0.1%.)

The I-cache ratio here (1.83x over no cache) agrees with the 1.87x
measured on sergei_ml1 hardware, which is why these are trusted as
estimates.

### Why less than the proposal predicted

The proposal modelled 0.35 data accesses per instruction at ~12 cycles
each and concluded data traffic was nearly all of the remaining stall,
predicting ~1.6x over I-cache only. Measured on picorv32 with the cache
in place:

- **0.21** data accesses per instruction (11,104 loads + 7,243 stores
  against 88,294 fetches), not 0.35.
- Only **~20%** of cycles are the CPU waiting on the bus at all, and
  most of those are the 1-cycle fetch hit, which no cache can remove.
- The rest, a CPI of about 6, is picorv32_wb itself.

So the D-cache removes most of what was removable. Stores cost 1 cycle
each with the write buffer (it never filled in this workload); load
hits cost 1; the 606 load misses averaged ~40 cycles before bursts,
~22 after. zeitlos32 spends fewer cycles of its own per instruction,
so memory is a larger share of its time and it gains more.

Workloads whose data outgrows 4KB (large arrays, image decoding, the
browser) will miss more, gain less from the D side and more from
bursts. Read `cache` on real apps before resizing.

## Cost

Standalone `synth_ecp5` of the module (I 8KB, D 4KB, 4-word lines):

| | LUT4 | FF | DP16KD |
|---|---|---|---|
| `wb_icache` (for comparison) | 550 | 405 | 5 |
| `wb_cache`, WBUF 0 | 1327 | ~800 | 8 |
| `wb_cache`, WBUF 2, snoop (mozart_ml1) | **1869** | ~820 | 8 |

Two things were worth ~800 LUT4 between them, and both are fixed:

- yosys re-encoding `state` folded the write-buffer, snoop and hit
  conditions into its transition logic: ~430 LUT4 for nothing. `state`
  carries `fsm_encoding = "none"`.
- Fills and blocking stores could start from either `S_LOOKUP` or
  `S_MEMREQ`, which duplicated the whole memory-port launch mux: 378
  LUT4 to save one cycle per miss (~0.1% of cycles). Every fill now
  starts from `S_MEMREQ`.

What remains of the write buffer's cost (~540 LUT4 over WBUF 0) is
the ordering logic and the extra source into the memory-side
address/data registers; the entries themselves are distributed RAM,
and depth 1 costs the same as depth 2.

Full SOC, mozart_ml1, exactly the project's flow (`synth_ecp5 -abc9`,
`-DJUMPLOADER`, `nextpnr-ecp5 --45k --package CABGA256`), oss-cad-suite
2026-09-23 (yosys 0.69, nextpnr 0.11.1), default seed:

| | Logic cells | FF | DP16KD |
|---|---|---|---|
| `ICACHE` | 21,225 (48%) | 9,632 | 41 |
| `ICACHE` + `DCACHE` + `SDRAM_BURST` | 22,804 (52%) | 10,051 | 44 |

CLK_48 maximum frequency over three placement seeds (48 MHz required):

| Build | seed 1 | seed 2 | seed 3 | range |
|---|---|---|---|---|
| `ICACHE` (`rtl/cache.v` as shipped) | 55.52 | 54.90 | 52.69 | 52.7-55.5 |
| `ICACHE` + `DCACHE` + `SDRAM_BURST` | 60.31 | 59.06 | 56.60 | 56.6-60.3 |
| `ICACHE`, `rtl/cache.v` with the `!ack_r` change (not applied) | 59.64 | 58.08 | 59.48 | 58.1-59.6 |

The D-cache build is at or above the I-cache-only build on every seed,
with at least 18% margin.

### Timing

The first place-and-route of the D-cache build gave 53.9 MHz, and its
critical path ran from the D tag RAM, through the tag compare and the
combinational hit acknowledge, into `S_IDLE`'s request-accept logic and
on into a statistics counter's carry chain. None of that needed to be
one path:

- `S_IDLE` tested `!c_ack_o` to avoid re-accepting a request it was
  still acknowledging. Only the registered ack (`ack_r`) can be high
  there; the combinational hit and posted-store acks exist only in
  `S_LOOKUP`. It now tests `!ack_r`.
- STALL counts from a registered pulse, a cycle late, so the hit compare
  ends at one flop instead of a 32-bit adder.

After that: 60.3 MHz, and the critical path is `wb_arbiter_main` into
the Ethernet MAC and blitter, outside the cache.

The I-cache-only build's critical path also starts at the cache's tag
RAM (`rtl/cache.v`), and its `S_IDLE` has the same `!c_ack_o` test.
Changing that one condition in `rtl/cache.v`'s `S_IDLE`

```verilog
end else if (c_cyc_i && c_stb_i && !c_ack_o &&     // as shipped
end else if (c_cyc_i && c_stb_i && !ack_r &&       // measured
```

was measured at +3 to +7 MHz on all three seeds (table above). It is
**not applied**: `rtl/cache.v` is left byte-for-byte what is already
running on hardware. It is correct for the same reason as in
`wb_cache` (`hit_now` exists only in `S_LOOKUP`), but if applied, rerun
`tb_cache.v` (both `FAST_HIT` settings), `tb_cache_sdram.v` and
`tb_cache_soc.v -DCACHE=1` first.

`ICACHE_FAST_HIT 0` still removes the combinational hit path entirely
if a future board needs it.

## Boards and releases

Enabled on every board that has a cache, in `rtl/boards.vh` and in the
matching `release/hw/boards/*.spec` (`release/zrelease check` verifies
the two agree):

| Board | Part | Settings |
|---|---|---|
| Mozart ML1 | 45F | `DCACHE_KB 4`, `DCACHE_WBUF 2`, `SDRAM_BURST` |
| Sergei ML1 | 45F | same |
| ULX3S | 12F / 25F / 45F / 85F | same |
| Lakritz | 25F | same but **`DCACHE_WBUF 0`**, for headroom |

Lakritz is the tightest board, and with the write buffer it measured
88% full and 49.9-54.6 MHz over three seeds on the plain board build:
passing, but only 4% over 48 MHz on the worst seed. Dropping the write
buffer gives back ~540 LUT4 for about 7% of the D-cache's gain.

Every release target, built the way `zrelease build` builds it (its
own generated `zspec.vh` and merged `.lpf`, `-DZSPEC`, the board's
`JUMPLOADER` and device), oss-cad-suite 2026-09-23, one placement seed:

| Target | Logic cells | DP16KD | CLK Fmax (48 MHz required) |
|---|---|---|---|
| lakritz_gpio | 85% | 38 / 56 | 57.4 MHz |
| lakritz_katze | 88% | 43 / 56 | 54.1 MHz |
| lakritz_langkatze | 85% | 38 / 56 | 54.0 MHz |
| mozart_ml1 | 52% | 44 / 108 | 56.7 MHz |
| sergei_ml1 | 47% | 45 / 108 | 52.5 MHz |
| ulx3s_12f | 85% | 39 / 56 | 58.2 MHz |
| ulx3s_45f | 47% | 42 / 108 | 52.5 MHz |
| ulx3s_85f | not built: the ulx3s_45f design on a larger die | | |

All pass. The 25F-class targets (Lakritz, ULX3S 12F, which is the same
die) sit at 85-88%: there is room for the cache, not for much else. If
one of them runs short, the order to give things back is `DCACHE_WBUF 0`
(~540 LUT4; already the setting on Lakritz), then `-DCACHE` in the
target spec, before any of the older trade-offs those spec files
describe.

Board builds without the release layer (`make BOARD=x`, one seed),
before and after enabling:

| Board (part) | Without: logic / DP16KD / Fmax | With: logic / DP16KD / Fmax |
|---|---|---|
| mozart_ml1 (45F) | 48% / 41 / 55.5 MHz | 52% / 44 / 60.3 MHz |
| sergei_ml1 (45F) | 44% / 42 / 53.3 MHz | 48% / 45 / 54.5 MHz |
| Lakritz (25F), with write buffer | 82% / 35 / 55.2 MHz | 88% / 38 / 53.3 MHz |
| ULX3S (25F) | 79% / 39 / 56.8 MHz | 86% / 42 / 55.7 MHz |

Hardware status: mozart_ml1 has run it (see
[Measured on hardware](#measured-on-hardware)). The others are
simulation and place-and-route only; on first boot, follow
[Bring-up](#bring-up).

The release specs also brought mozart_ml1's release in line with
`rtl/boards.vh` on USB: the spec had `USB_HID` where the board file has
`USB_HOST`, which `zrelease check` reports as drift.

Obst (SRAM, no I-cache) should not enable either cache: see
[icache.md](icache.md). The GateMate boards (Kölsch, Lebkuchen) have
never been built with a cache.

The C emulator (`sim/`) needs nothing: it reads unmapped addresses as
0, so `z_dcache_present()` is false there and the kernel writes
nothing.

## Gotchas

- **The BIOS runs with the data cache off**, including its `[M]` memory
  test (which would otherwise test the cache). Only the kernel turns it
  on. A new bitstream with an old kernel behaves exactly like an
  I-cache-only build.
- **Iteration-counted delays get shorter.** Only loops whose counter
  lives in memory (`volatile` locals) change; register counters were
  already served from the I-cache. All of them in `sw/` were audited:
  the `zdialog.c` throttles and the `kernel.c` reboot waits are
  best-effort and fine shorter. `kprint_hex_digit()` was not: it wrote
  the UART without checking it had room and relied on a 500-iteration
  spin for pacing, eight times in a row from `kprint_hex32()`. It now
  waits on LSR like `kprint()`. Use `delay_ms()` or `rdcycle` for any
  new delay with a real minimum.
- **Physical aliases.** `sdram_kianv.v` uses 25 address bits, so main
  memory repeats every 32MB in the 256MB window; the cache tags on the
  full address. Nothing accesses memory through an alias. Nothing
  should.
- **A new master that writes main memory** is covered by the snoop only
  if it goes through `wb_arbiter_main` (which is the only way onto the
  main bus).

## Testing

```
# unit: reference-model checks, random latency, bursts, ordering fence,
# snoop, runtime enable/disable, Wishbone stability
iverilog -g2005 -o tb_cid rtl/tb/tb_cache_id.v rtl/cache_id.v && ./tb_cid
iverilog -g2005 -Ptb_cache_id.BURST=1 -Ptb_cache_id.WBUF=4 \
    -Ptb_cache_id.FAST=0 -o tb_cid rtl/tb/tb_cache_id.v rtl/cache_id.v

# SDRAM controller alone: singles and bursts against the protocol model
iverilog -g2005 -Ptb_sdram_burst.BURST=1 -o tb_sb \
    rtl/tb/tb_sdram_burst.v rtl/mem/sdram_kianv.v rtl/tb/sdram_model.v

# whole path with a real CPU (build the program first)
make -C rtl/tb/cache_soc PREFIX=riscv64-unknown-elf-   # or your rv32 prefix
iverilog -g2005 -DCACHE=2 -DBURST=1 [-DCPU_Z32] -o tb_soc \
    rtl/tb/tb_cache_soc.v rtl/cache_id.v rtl/cache.v \
    rtl/mem/sdram_kianv.v rtl/tb/sdram_model.v rtl/cpu/picorv32/picorv32.v \
    rtl/cpu/zeitlos32/zeitlos32.v rtl/cpu/zeitlos32/zeitlos32_muldiv.v
vvp tb_soc +prog=rtl/tb/cache_soc/prog.hex +expect=2cf1c77d
```

`tb_cache_soc.v` takes `-DCACHE=0|1|2`, `-DBURST`, `-DWBUF=n`,
`-DIKB`, `-DDKB`, `-DSNOOP=0|1`, `-DCPU_Z32`, `-DSHOW_STATS` and
`-DARB`, and reports cycles per phase plus fetch/load/store wait.
`+expect` makes a checksum mismatch a failure. With `-DARB` add
`rtl/arbiter_main.v` to the file list.

`-DARB` puts the real `wb_arbiter_main` between the cache and memory,
with `rtl/sysctl.v`'s exact `wbm_cti` and `cache_snoop_stb`
expressions, and adds two masters:

- a blitter-like reader (we=0, sel=1111, single reads) hammering the
  program's code region and checking every word against the image;
- a writer, which no real master is yet, incrementing a mailbox word
  in main memory every few hundred cycles. `prog.c`'s last phase reads
  the mailbox *through the data cache* and requires it to advance six
  times. That can only happen if the writer's stores invalidate the
  cached line, so it tests the snoop path end to end, including the
  gating in `sysctl.v`. It is not part of the checksum.

What has been run:

- `tb_cache_id.v`: 13 configurations (2-8KB, 2-8 word lines,
  `FAST_HIT` 0/1, write buffer 0/1/2/4, burst 0/1), two seeds each.
  Deliberately broken versions (no I snoop, no D update, no drain
  before bypass, wrong byte merge, snoop ignored) all fail it.
- `tb_sdram_burst.v`: `BURST` 0/1, open and closed page, CAS 2 and 3.
  With `BURST=0` the cycle counts are identical to the pre-change
  controller; a capture one cycle late fails it.
- `tb_cache_soc.v`: picorv32 in all six configurations above, zeitlos32
  in four, all with the same checksum.
- `tb_cache_soc.v -DARB` (D-cache, write buffer, bursts): picorv32 passes
  (30,324 reader checks, 0 errors; 1,859 writer stores, 1,859 snoops;
  mailbox advanced), zeitlos32 passes (21,765 checks, 0 errors; 1,391
  stores, 1,391 snoops). Contention cost the CPU ~1% of cycles. **The
  same run with `-DSNOOP=0` fails** ("MAILBOX STUCK"), which is what
  shows the test actually detects a missing snoop.
- `tb_cache_sdram.v` (`wb_icache` + controller + model): now a clean
  pass. It was not before; see below.

### The SDRAM model

`rtl/tb/sdram_model.v` was rewritten. The previous version was marked
incomplete and returned zeros. The new one models MRS (burst length,
CAS latency, write-burst mode), read bursts with wrap, READ/WRITE
truncating a read burst, BST, write bursts with DQM, auto-precharge,
and drives DQ `TAC` after the clock and holds it `TOH` past the
sampling edge. It reports ACT on an open bank, READ/WRITE on a closed
one, REF or MRS with a bank open, and commands before MRS. The
unchanged controller passes it with zero protocol errors, which is the
evidence the model is right.

## Bring-up

1. Flash the bitstream **and** the kernel together (the kernel is what
   turns the data cache on).
2. At boot the kernel logs `- icache: ...` and `- dcache: 4KB, 4-word
   lines, 2-entry write buffer, burst fills`, and the SOC feature list
   has a `cache   icache dcache` line (FEATURES2 bits 8-9, see
   [csrs.md](csrs.md)). `cache` shows both sides; run a few apps and
   check `D rate`.
3. `i-snoops` is **0 after a normal boot, and that is correct.** A
   snoop only counts when a store overwrites an instruction line that
   is currently cached, and boot loads apps into memory that has never
   executed. To see it work: run an app, quit it, then run another of
   the same size or smaller, and `cache`. `k_mem_alloc()` is first-fit,
   so the second app normally lands in the first one's freed block,
   whose code is still cached, and loading it snoops those lines. If it
   stays 0, the second app was placed elsewhere: try the same app
   twice.
4. If anything misbehaves, bisect in this order, each a shell command
   or one define:
   1. `cache doff` then `cache wboff`: data cache fully out of the way,
      bypass path still in use. Still wrong: it's the bypass/drain
      logic or the I side.
   2. `cache off`: instruction side out too.
   3. Rebuild without `SDRAM_BURST`: the controller as it was.
   4. `ICACHE_FAST_HIT 0`: removes the combinational hit path.
   5. Comment `DCACHE`: back to `wb_icache` exactly.
