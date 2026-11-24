# zeitlos32ps

A small pipelined RV32IM core at `rtl/cpu/zeitlos32ps/`, sized for the
ECP5 25F boards (Lakritz, Konfekt, Mozart ML0, Sergei ML0). It is the
same machine as [zeitlos32](zeitlos32.md) and [zeitlos32p](zeitlos32p.md):
RV32IM, the four counter reads, FENCE as a nop, picorv32's custom
interrupt instructions and IRQ timer with the same semantics, and (with
`CPU_ROT`) Zbb's ror, rol and rori. The BIOS, the kernel and every
binary run on it unchanged.

It is the release default on most ECP5 targets (`+CPU_ZEITLOS32PS` in
`release/targets/*.spec`, including Lakritz, Konfekt, Klinge, Minze,
Mozart, Sergei, and ULX3S 12F / 45F / 85F Langkatze). Standalone
`make BOARD=x` still builds picorv32 unless asked:

```
make BOARD=lakritz EXTRA_DEFINES=-DCPU_ZEITLOS32PS
```

It needs `DCACHE` and `ARBITER`, and works with or without `MPU`.

## The design

- **One memory port.** Fetches, loads and stores share one request port
  to `rtl/cache_one.v`, a unified cache of `ICACHE_KB + DCACHE_KB`
  (cache_split's data side, with fetches as reads). One access is
  outstanding at a time; a hit is answered the cycle after it is taken,
  and the next request can be taken in the answer's cycle.
- **Four stages: F, D, X, W.** F fetches into a three-entry queue, D
  reads the register file (LUT RAM) from the queue's head, X executes
  (one instruction at a time; loads and stores wait for their answer;
  multiply and divide use `zeitlos32_muldiv_lean`), W writes the register
  file and forwards its result to D and X.
- **Redirects** (taken branches, jumps, interrupts, retirq) are decided
  in X and cost three cycles; nothing is predicted.
- **Faults** are zeitlos32's: an illegal instruction raises IRQ 1 and is
  skipped, a misaligned access IRQ 2.
- **The MPU** is `rtl/mpu_one.v`: wb_mpu's rules and registers, with
  privilege per instruction as zeitlos32p has it. Every access on the
  port is judged from one registered copy of the request: a load or
  store's verdict reaches the cache the cycle after the take; a fetch's
  properties (privileged region, gate, own block) come back with its
  answer, travel with the instruction, and are decided in X in program
  order. The core keeps each instruction's physical fetch address, so
  fault addresses and PCs are recorded physical, as wb_mpu records
  them, without translators in the MPU.
- **The cache's statistics counters** are in a block RAM
  (`STAT_BRAM`), updated in turn by one adder; a read of one waits
  2-9 cycles for its next update.

## Performance

`rtl/tb/tb_cache_soc.v` at 25F settings (`-DBURST=1 -DIKB=4 -DDKB=4`),
the same program on each core. picorv32, zeitlos32 and zeitlos32p are
measured without a write buffer, as the 25F boards ship
(`DCACHE_WBUF 0`); zeitlos32ps with the 2-entry buffer it is built with
(`CACHE_ONE_WBUF`), which is inside its size below:

| core | cycles | vs zeitlos32 |
|---|---:|---:|
| picorv32 | 713,562 | 0.75x |
| zeitlos32 | 533,714 | 1x |
| **zeitlos32ps** | **195,056** | **2.74x** |
| zeitlos32p (45F/85F) | 188,150 | 2.84x |

With the bus contended by a second master (`-DARB`): zeitlos32ps
267,150, zeitlos32 601,156 (2.25x). The write buffer (2 entries) is
needed for that: without it zeitlos32ps drops to 1.78x.

## Size and timing

Full default builds (MPU, `CPU_ROT` where the board has it), nextpnr
48 MHz. LUT4 cells are nextpnr's TRELLIS_COMB:

| board | picorv32: cells / BRAM / Fmax | zeitlos32ps: cells / BRAM / Fmax |
|---|---|---|
| Lakritz | 23,163 / 52 / 57.5 MHz | 22,843 / 50 / 51.6 MHz |
| Konfekt | 21,023 / 43 / 57.3 MHz | 20,765 / 41 / 56.6 MHz |
| Mozart ML0 | 21,706 / 56 / 56.0 MHz | 21,438 / 54 / 52.0 MHz |
| Sergei ML0 | 21,766 / 56 / 55.9 MHz | 21,502 / 54 / 50.1 MHz |
| Klinge (DDR3) | 20,146 / 42 / 59.6 MHz | 19,892 / 40 / 56.1 MHz |
| ULX3S (25F) | 24,360: does not fit | 22,927 / 44 / 48.1 MHz |
| ULX3S 12F (release) | 24,243 / 42 / 62.9 MHz | 22,987 / 41 / 48.7–50.6 MHz (4/4 seeds) |
| ULX3S 45F (release) | 23,989 / 89 / 57.2 MHz | 23,568 / 88 / 48.1–50.6 MHz (4/4 seeds) |
| ULX3S 85F (release) | 23,989 / 89 / 54.3 MHz | 23,568 / 88 / 48.6 MHz (1/4 seeds; stays on picorv32) |
| ULX3S 85F Langkatze | 24,160 / 85 / 58.5 MHz | 23,841 / 84 / 48.7–51.1 MHz (5/6 seeds) |
| Minze | 21,430 / 42 / 58.2 MHz | 21,406 / 40 / 57.4 MHz |

(The ML0 rows were placed without `tools/ddmi/pin_serialiser.py`; see
its fallback for the output stage's flip-flops.)

Three placement seeds (default, 2, 3), 48 MHz clock: Lakritz 53.7 /
54.2 / 52.8 MHz, Konfekt 56.6 / 52.4 / 54.0, Mozart ML0 52.0 / 52.6 /
53.6, Sergei ML0 (pinned) 55.0 / 54.0 / 54.0 -- all pass.

The 48 MHz margin is thinner than picorv32's (about 57 MHz). The critical path is the cache's
tag RAM (about 5.8 ns clock-to-output) through the hit compare and the
answer into the core's X-stage enables.

## Which core is running

`z_soc_cpu_name()` returns `"zeitlos32ps"`: FEATURES bit 23 is set, as
for zeitlos32, and the cache's FEAT register has bit 12
(`Z_CACHE_FEAT_ONE`) set, which only `rtl/cache_one.v` sets.

## Testing

In `rtl/cpu/zeitlos32ps/tests`:

- `make run` -- zeitlos32's directed tests at memory latencies 0, 1, 3
  and 12 and with random latency and busy cycles;
- `make random` -- generated programs, self-checking;
- `make lockstep` -- every register write compared with zeitlos32's;
- `make rot` -- `prog/t_rot.S` and every directed test with the rotates;
- `make mpu` -- zeitlos32ps + mpu_one + cache_one against zeitlos32 +
  wb_mpu on zeitlos32p's MPU programs: register writes, stores, blocked
  accesses and fault registers must match.

`rtl/tb/tb_cache_one_stat.v` checks the statistics registers;
`rtl/tb/tb_cache_soc.v -DCPU_Z32PS` runs the SoC benchmark.

## Clean-room

Original code, written for this SOC from the RISC-V specification and
the interrupt ABI as `sw/bios/custom_ops.S` and zeitlos32 implement it.
It reuses zeitlos32's decoder and load/store units.
