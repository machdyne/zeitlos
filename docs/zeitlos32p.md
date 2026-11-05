# zeitlos32p

The pipelined zeitlos32, at `rtl/cpu/zeitlos32p/`. The same machine
as [zeitlos32](zeitlos32.md): RV32IM, the four counter reads, FENCE as
a nop, and picorv32's custom interrupt instructions with the same
semantics, plus picorv32's IRQ timer, which zeitlos32 leaves out and
the kernel needs ([The timer](#the-timer)). The BIOS, the kernel and
every binary run on it unchanged.
The difference is how instructions are sequenced. zeitlos32p has five
stages and can finish one instruction a clock. zeitlos32 takes five
clocks per instruction at best.

It is a drop-in option for any board with a data cache (`DCACHE`).
Its size, about 5,300 LUT4 with its cache, targets the 45F/85F boards
(Mozart ML1/ML2, Sergei ML1/ML2, Noir, Schoko and the like). It is
**not** the default anywhere. A build asks for it:

```
make BOARD=sergei_ml2 EXTRA_DEFINES=-DCPU_ZEITLOS32P
```

A board block can also `` `define CPU_ZEITLOS32P ``.

## Status

- **Hardware: boots to the shell** (round 6, sergei_ml2, timing met at
  48 MHz). Rounds 4-5 stopped after `fs: driver (pid 1)`: the
  misaligned-trap bug, "Bugs found after round 3" (1).
- **Branch prediction** (round 7): a BTB in F, [below](#branch-prediction).
  Not yet on hardware.
- **Simulated.** Its own test suite, lockstep with zeitlos32, the MPU
  lockstep with zeitlos32 + wb_mpu, random stress with and without
  interrupts, the cache SOC bench with and without the MPU and the
  arbiter, the OS's LZ4 decoder.
- **The MPU** works with it: [rtl/mpu_split.v](#the-mpu).

## Performance

`rtl/tb/tb_cache_soc.v` runs the same program on each core: code
loading, sorting, byte memcpy, pointer chasing, strings and recursion.
All three use the boards' cache settings (8 KB I, 4 KB D, 4-word
lines, a 2-entry write buffer, bursts) and the SDRAM model. The
checksum is identical for every core.

| Core | Cycles | vs picorv32 |
|---|---|---|
| picorv32 | 657,910 | 1.00 |
| zeitlos32 | 479,024 | 1.37× |
| zeitlos32p, no BTB (round 6) | 175,234 | 3.75× |
| **zeitlos32p** | **140,500** | **4.68×** |
| zeitlos32p, `ARB` (arbiter, blitter-like reader, mailbox writer) | 238,596 | |

That is 85,106 instructions at 1.65 cycles each, misses included
(2.06 without the BTB). zeitlos32 takes about 5.6 cycles each.

The kernel's boot banner times a small integer loop
(`k_cpu_report()`, sw/os/kernel.c). Compiled as written (shifts and
adds, as gcc 13 and 15 do at `-O2`) that loop runs at 0.67 IPC on
zeitlos32p without the BTB and 0.92 with it. At `-Os`, which the kernel
uses, those gccs turn its `x += x << 3` into a multiply by 9, and then
it is 0.48 and 0.63. Round 3's banner (0.68) came from a kernel with
the shifts; rounds 4-6 showed 0.49 because the kernel had the
multiply, not because the core was slower (the two cores run both
loops at identical speed).

## Stages

| Stage | What happens there |
|---|---|
| **F** | `pc_f`, translated by the MTU, to the I port. A request goes out only while the fetch queue has room for its answer (queue plus in flight ≤ 3), so `i_req` depends on registers alone. |
| **D** | The instruction at the head of a 3-entry fetch queue. Register file read (LUT RAM), with a bypass from W. |
| **X** | Decode, forwarding, the ALU. Branches and jumps resolved. A load's or store's address computed and sent to the D port. Interrupts taken, the custom instructions, multiply and divide. |
| **M** | Two slots, M and S: a load or store waits there for the D port's answer; a load's byte or halfword is picked out. |
| **W** | The register write, through the register file's one write port. |

Everything an instruction does besides its register write and its
memory access happens in X, in order, and nothing behind X can cancel
it. So an instruction that leaves X has executed, or, for a load or
store, is certain to.

## What costs cycles

| Event | Cost |
|---|---|
| Taken branch or `jal`, predicted (in the BTB, counter taken) | 0 |
| Branch or `jal` mispredicted, `jalr`, `retirq` | 4 (resolved in X, the redirect out a cycle later; F, the queue and D refill) |
| Load, then an instruction using its result | 1 (forwarded from W) |
| Instruction needing a result still in S | until S moves up (only behind a waiting load/store) |
| Behind a waiting load/store | one more instruction moves into S; the next waits |
| Cache miss, uncached access, write buffer full | its own time; fetch and data wait independently |
| Multiply | 3 with `CPU_MUL_FAST`, 34 without |
| Divide | 35 |
| Custom-0 instruction (`getq`, `setq`, `retirq`, `maskirq`, `waitirq`, `timer`) | waits for M, S and W to empty, plus one cycle |

## Branch prediction

A **branch target buffer** in F (`BTB_BITS`, 6 by default: 64 entries;
0 builds the core without one). Each entry is an 8-bit tag, a 30-bit
target and a two-bit counter, for conditional branches and `jal`, in
distributed RAM read without a clock.

- **F.** When a fetch goes out and its entry matches with the counter
  at "taken", the next fetch is from the target instead of pc+4. The
  table is indexed by the register `pc_f` and feeds only `pc_f`'s next
  value, so the I port's address path is untouched. (As block RAM,
  which yosys chose unasked, its address became `pc_f`'s next value and
  the cache's hit signal reached it: 45-47 MHz. `ram_style =
  "distributed"` keeps it in LUTs.)
- **D** checks the prediction against the instruction that arrived: a
  `jal` or a branch, and pc + its offset is the target fetching went
  on at (`ex_ptok`).
- **X.** A `jal` or taken branch whose target D found right does not
  redirect. Anything else that was fetched past at a predicted target
  -- a branch not taken, a wrong target, not a branch at all, an
  instruction skipped as illegal or misaligned -- redirects as a taken
  branch does today, to the real target or to pc+4. So the table is
  never trusted: a stale entry (another program loaded at the same
  address, another MTU base, an alias) costs time only. X writes the
  outcome back a cycle after the instruction leaves: a taken one is
  entered at "weakly taken" or strengthened, a not-taken one weakened
  if it is there.

`jalr` (returns, calls through pointers) is not predicted: three
cycles, as before. A return stack is the next step.

`tests/prog/t_btb.S` trains entries and then changes what the program
does at their address: an instruction overwritten by an `addi`, a
`mul`, a load or a return, a `jal` retargeted, a branch that changes
direction, two loops sharing an index. `make nobtb` runs the
directed, local and lockstep tests on the core built without one.


## The timing decisions

This is the part of the design where most of the work went. The
stages follow from it.

The D port answers a hit from its tag RAM late in the cycle: block RAM
clock-to-out, then a 17-bit compare. The same holds for the I port.
A straightforward pipeline lets that answer gate the stall of every
stage behind it. On ECP5 that path was **41.8 MHz** out of context,
against a 48 MHz system clock. Five changes took it out, each found
from the timing report:

1. **Jumps redirect without waiting for M.** A jump redirects
   fetching as soon as its operands are ready, even if a load ahead
   of it still waits. It remembers this (`ex_started`) so it does not
   redirect twice.
2. **M is a two-slot circular buffer.** X writes the slot `q_wp`
   names, which was empty at the start of the cycle. X moves on
   whenever S is empty, a registered fact; it never looks at the D
   port's answer. The answer only clears a valid bit and moves
   `q_rp`, so it reaches a handful of flip-flops and W's registers,
   which load every cycle.
3. **Loads and stores can be sent from a slot.** The request goes
   straight from X when nothing older is still waiting to be sent. If
   the port does not take it, it goes again later from its slot, which
   keeps the physical address, the data and the byte enables.
   - Requests are in order.
   - The port has one outstanding, so every answer is M's: S is filled
     only behind a waiting M, and S's own request is taken at the
     earliest in the cycle M's is answered, when S moves up.
   - `tests/tb_zeitlos32p.v` checks this on every answer.
4. **The physical address is one adder deep.** In the `0x8` window the
   address is `mtu_base + offset`, which equals
   `va + (mtu_base − 0x8000_0000)`, and subtracting `0x8000_0000`
   flips bit 31. So the translated address is a three-input sum,
   formed carry-save next to the plain one, and the window test only
   chooses between them. Proven equal to the two-adder form with
   `yosys sat`, including a mutant that fails.
5. **The fetch queue always holds the instruction.** D reads only the
   queue's head, never the I port's answer directly, because that
   answer arrives too late to index the register file and still
   capture the read. This costs one cycle each time fetching restarts
   (taken branches mostly): 162,918 → 175,234 cycles on the bench
   above (+7.6%). The queue grew to three entries so a stream of hits
   still goes at one a clock.

Smaller changes with the same aim:

- **Forwarding** comes from `lx`, a copy of X's last result in
  registers of its own, so the forwarding selects compare registers
  directly, not the slot that `q_rp` names.
- **Fetch-queue entries** are written whether or not X redirects (a
  redirect only zeroes the count).
- **X frees early.** X's "finished" (`x_go`) does not wait for the
  address adder: a misaligned load or store is skipped under exactly
  the conditions an aligned one moves on under.
- **The cache's data muxes** pick by registered state, not by the
  hit.
- **The cache's same-word store hazard** no longer stalls. The merged
  word is forwarded for one cycle instead, so `d_rdy` never depends
  on the address ([cache_split.md](cache_split.md), hazard 2).

Found on the full SOC, in place and route on the project's nextpnr
(round 4):

6. **The cache arrays are written one cycle late, with a bypass of
   their own.** The cache's logic is written for read-old: a lookup
   reads what the array held before this edge's write. A DP16KD's two
   ports give no defined answer when one writes the address the other
   reads, so yosys emulated read-old with soft logic comparing the read
   address -- the take mux, the end of tag → hit → take -- with the
   write address: four LUTs at the end of the critical path, every
   seed (45.1-46.5 MHz). Now each array is written from registers one
   cycle late, and a read that meets that write is answered from the
   register; the address compare is done for both inputs of the take
   mux and chosen by the take, one LUT after it. `(* no_rw_check *)`
   keeps yosys from adding its own. It is read-old exactly: the cache
   bench's event counts are identical, run for run.
7. **The MPU judges a load or store the cycle after the take**, from
   its own registered copy of the request, before the access has
   touched anything ([The MPU](#the-mpu)). Judged at the take, its
   comparators followed the core's address adder: 43-45 MHz.

### The redirect a cycle after X (2026-11-05)

On sergei_ml2_console (zeitlos32p beside T2, DDR3, the network) sys_clk's
critical path was a jump's redirect: forwarding, the branch comparator's
carry, the prediction checks (`ex_ptok`, `ex_pt`), the target mux, into
`pc_f` and everything else a redirect touches -- 20.16 ns, 13.5 of it
routing, 49.6 MHz on the third seed and short of 48 on the others. X
now registers it (`jr_v`, `jr_pc`) and the redirect goes out the next
cycle; in that cycle X takes nothing from D (all of it is the old path),
so nothing wrong-path runs. A jump waiting in X (M busy) has its
redirect out already (`ex_jdone`), as before. Interrupts and skipped
instructions (`x_pfix`) still redirect at once.

Cost: a cycle a redirecting jump. The core bench's programs +1.2%
cycles (200,568 to 202,998), the SOC bench (`tb_cache_soc`, BURST) +0.2%
(140,500 to 140,770). Out of context (core + cache, no MPU, yowasp
nextpnr 0.11, three seeds) 52.73 / 54.24 / 53.65 MHz against
53.25 / 52.80 / 53.10: there the cache's tag-to-hit path is the
limit, as before; the jump's path is the one that grew in the full SOC.
All of `make -C rtl/cpu/zeitlos32p/tests all` passes.

## Interrupts and faults

These are zeitlos32's semantics. An interrupt is taken in X, at the
boundary before the instruction there:

- `q0` gets that instruction's address (the instruction did not run).
- It and everything behind it are discarded, and fetching restarts at
  `PROGADDR_IRQ`.
- Instructions ahead of it, in M, S and W, are older and finish.

As on zeitlos32, an interrupt is not taken:

- while a handler runs (`irq_active`);
- for the one instruction after `retirq` (`irq_delay`);
- once a multiply/divide or a `waitirq` has started, or a jump has
  already redirected.

Faults also behave as on zeitlos32:

- An illegal instruction, `ecall`/`ebreak`, or a misaligned load or
  store sets IRQ 1 or 2 pending and is skipped.
- A misaligned fetch becomes a marker in the fetch queue. In X it
  raises IRQ 2, with `q0` = that address.
- Any of these with the IRQ masked, or inside a handler, halts the core
  (`trap`).

**The custom instructions serialise.** A store just before `waitirq`
may be what raises the line it waits for (a device told to interrupt).
On zeitlos32 that store finished long before. Here it may still be in
M. So custom-0 instructions wait until M, S and W are empty, plus one
cycle, so that a one-cycle pulse on an unlatched line has reached
`irq_pending`, and gone again. zeitlos32's `t_irq.S` (check 630) is
exactly that case. These instructions run only at interrupt entry and
exit.

**Stores to the MTU and MPU registers serialise too.** A store to
the `0x9` nibble (the MTU's base, the MPU's registers) changes how
every later access is translated and judged. On zeitlos32 it has
finished before the next instruction starts. Here it may still be in a
slot, on its way to the bus, while X translates the next address with
the old base -- and the kernel switches `reg_mtu` and may touch the app
window right after. Round 4 missed this; it is the likeliest cause of
the hang on hardware after `fs: driver (pid 1)`, which moved with the
kernel image. Now X waits while such a store is outstanding.
`tests/prog/t_mtu.S` switches the base and, at once, stores, loads and
jumps into the window, against zeitlos32 (`make mpu`, which models the
MTU base as a register the bus writes): it fails without the wait and
matches with it.

## Bugs found after round 3, and how

The hardware run that stopped after `fs: driver (pid 1)` (rounds 3-5)
is most likely bug 1 below: it halts the core in exactly the code fs
is unpacked with, and nothing else found reproduces the stop. Not yet
confirmed on hardware. Each bug is fixed and has a test that fails
without the fix.

1. **A misaligned trap judged on a stale operand** (since round 3). A
   load or store waiting in X for the load that gives it its base
   register sees the register's old value. The trap path (IRQ 2
   masked, or inside a handler) took that old value for the address,
   found it misaligned, and halted the core. Found by running the OS's
   own LZ4 decoder, the code that unpacks every compressed executable
   -- fs first -- on the cache SOC bench (`rtl/tb/cache_soc_lz4`):
   `lw a4,12(sp); sw a5,0(a4)` with `a4` still holding `0x1dca`. Test:
   `tests/prog/t_hazard.S` (traps without the fix at every latency).
2. **Translating with the old MTU base** (round 5): see "Stores to the
   MTU and MPU registers serialise too" above. Test: `t_mtu.S`.
3. **The wrong first fault in report-only mode** (round 6; MPU only,
   not a hang). An instruction fetched outside the app's block that is
   itself a store outside it: the fetch report (`fv_ev`) and the
   store's data verdict reach `wb_mpu_split` in the same cycle, and the
   MPU recorded the store as the first fault where `wb_mpu` records the
   fetch. The fetch is always the older of the two -- X waits for every
   older access to be answered before it reports -- so it now wins.
   Test: case 9 of `t_mpu.S` (an uncached load in flight, a jump out of
   the block, a store there).
4. **X's own load or store offered only as X moves on** (round 6,
   hardening). `iss_x` is now `x_adv && x_dop`: before, it was a copy
   of most of `x_adv`'s terms without `x_fwait` and `x_serial`. No
   test found a case where the two differ in the result, but nothing
   kept them equal either; the core bench now fails at once if the D
   port takes X's request in a cycle X stays.

**The stress test that now covers this class** (`make stress`,
`tests/gen_stress.py`): random programs of loads and stores of every
size through pointer registers reloaded from memory right before use
-- having held a misaligned value until then -- branches, small loops,
calls, multiply/divide, all with every interrupt masked so that a
fault the pipeline imagines halts it. They run against zeitlos32, with
zeitlos32p on the real split cache (`tb_mpu_lockstep.v`), write for
write. Against the core before fix 1 every seed fails.

## The timer

picorv32's IRQ timer, which rtl/sysctl.v builds into picorv32
(`ENABLE_IRQ_TIMER`) and the kernel relies on: `timer(1)` is how a
process that blocks yields at once (`k_proc_block()`,
`k_fs_leave()`, sw/os/kernel.c). zeitlos32 leaves it out, so the
current kernel panics on it there with an illegal instruction, as
round 3's zeitlos32p did on hardware.

`timer rd, rs` (custom-0, funct7 `0000101`) returns the counter in `rd`
and loads it from `rs`. The counter counts down one a clock and raises
IRQ 0 as it goes from 1 to 0; 0 is off. IRQ 0 is latched, so one
raised while masked is taken on unmasking. Like the other custom-0
instructions it serialises. `zeitlos32_decode` (shared with zeitlos32)
still calls the encoding illegal; the core accepts it on its own.
`tests/prog/t_timer.S` checks all of it.

## The MPU

With `MPU` (on by default, rtl/boards.vh), a `CPU_ZEITLOS32P` build
gets [rtl/mpu_split.v](../rtl/mpu_split.v) (`wb_mpu_split`) in place of
rtl/mpu.v: the same rules, registers, fault recording and interrupt
([mpu.md](mpu.md)). `wb_mpu` itself is untouched, and is still what
every other build has.

`wb_mpu` takes a load's or store's privilege from **the most recent
fetch**. That is correct for a core with one instruction in flight.
Here fetching runs up to six instructions ahead and down paths that
are thrown away, and a thrown-away fetch outside the app's block must
not be reported or trap. So privilege belongs to the instruction:

- **Fetches** are judged in X, in program order. As an instruction
  moves from D to X the MPU's checks on its address ride with it
  (privileged region? the gate or the IRQ vector? inside the app's
  block?). X decides with the privilege of the instruction before it,
  `wb_mpu`'s rule at the fetch. A violation is reported when the
  instruction leaves X, and with ENFORCE the instruction is illegal,
  as `wb_mpu`'s blocked fetch hands the CPU `0x0000_0000`. A fetch
  that is thrown away is never judged.
- **Loads and stores** carry their instruction's privilege to the D
  port. The MPU keeps a registered copy of each one the cache takes,
  judges it in the next cycle -- before the access has reached the
  arrays, the write buffer or the bus -- and the cache drops a
  blocked one there: a load reads 0, a store changes nothing.
- **Uncached fetches** (anything but BIOS RAM and main memory) are
  blocked at the I port while the last instruction executed was
  unprivileged and the MPU enforces, so a jump into peripheral space
  cannot read a FIFO by fetching from it before X has judged it.
- **Order of reports.** A fetch violation waits in X until every older
  load or store has been taken (and so judged), so the first fault
  latched is the oldest, as on zeitlos32.

Instruction addresses are judged virtual, by `wb_mpu`'s
`XLATE_ON_BUS` rules (inside the app window: inside the block exactly
when the offset is below SIZE), which agree with judging the
translated address for any block above KTEXT, as every block the
kernel hands out is. Fault addresses and PCs are recorded physical, as
`wb_mpu` with zeitlos32 records them.

**Tested against wb_mpu.** `tests/tb_mpu_lockstep.v` builds zeitlos32 +
`wb_mpu` and zeitlos32p + `wb_mpu_split` + `wb_cache_split`, both
translating through the MTU, and runs `tests/prog/t_mpu.S` on both:
every reason (outside the block, kernel-only, masked nibble, kernel
code entered off the gate), enforcing and report-only, the grant
window, a jump into peripheral space, the first instruction after
each privilege change, a store at the block's last word followed by
falling out of it. Every register write and store must match, and the
program reads the fault registers back after each case. Accesses to
the peripheral nibbles are counted on both buses and must agree.
Matches at 4 latencies × 3 seeds.

Broken on purpose, each of these fails it: privilege taken from the
previous instruction for a load or store; fetch violations not
blocked; no I-port veto; the GATE register ignored; the grant
window's limit inclusive. Two changes it does not catch, both
defensive: a blocked fetch taking the new privilege (the IRQ that
follows always enters the handler, which is privileged, so nothing
can see it), and the fetch report not waiting for older accesses (with
this core's timing the older access has always been taken first; the
wait guarantees it).

## Which core is running

`z_soc_cpu_name()` (`sw/common/zsoc.h`) returns `"zeitlos32p"`.
FEATURES bit 23 (`Z_FEATURE_CPU_ZEITLOS32`) is set, as for zeitlos32,
and the cache's FEAT register has bit 11 (`Z_CACHE_FEAT_SPLIT`) set,
which only `rtl/cache_split.v` sets.

## Size and timing

Yosys 0.33 `synth_ecp5 -abc9`, out of context.

| | LUT4 | CCU2C | FF | Other |
|---|---|---|---|---|
| zeitlos32p, `FAST_MUL=1` | 3,918 | 400 | 1,316 | 4 MULT18X18D, 32 TRELLIS_DPR16X4 (register file) |
| zeitlos32p, `FAST_MUL=1`, `ENABLE_MPU` | 4,265 | 369 | 1,485 | |
| wb_mpu_split | 741 | 224 | 455 | |
| wb_mpu (for comparison) | 493 | 128 | 418 | |
| core + cache, as in the SOC, no MPU | 5,301 | | 2,253 | 9 DP16KD |
| core + cache + MPU, as in the SOC | 6,384 | | 2,878 | 9 DP16KD |
| core + cache + MPU + BTB (round 7) | 6,856 | | 3,059 | 9 DP16KD, 92 TRELLIS_DPR16X4 |

The core before the timing work was 3,323 LUT4: the first five
changes above cost about 600 LUT4. Without `ENABLE_MPU` its privilege
tracking is constant and synthesises away (3,918 against round 3's
3,921, the timer included).

Out-of-context place and route: nextpnr-ecp5 0.6, `--45k`, 48 MHz
target. 0.6 is older than the project's nextpnr; compare these with
each other. The full SOC on the project's nextpnr is the measure that
counts.

| | Seed 1 | Seed 2 | Seed 3 |
|---|---|---|---|
| core + cache (round 3) | 54.90 | 51.06 | 52.23 MHz |
| core + cache + MPU, MPU judging at the take | 44.55 | 43.59 | 43.33 MHz |
| core + cache + MPU (round 4) | 54.38 | 50.92 | 53.81 MHz |
| core + cache, no MPU (round 4) | 53.34 | 56.04 | 52.16 MHz |
| core + cache + MPU (round 6) | 56.33 | 54.44 | MHz |
| core + cache + MPU + BTB (round 7) | 53.63 | 57.44 | MHz |

What limits it now:

- the cache's own tag → hit → take → next-lookup path, inherent to
  answering hits back to back;
- X's decode feeding its own enables.

Round 3 on the full sergei_ml2 SOC, project nextpnr: 45.14-46.50 MHz
on four seeds, all with the path that item 6 removes.

The whole sergei_ml2 SOC, synthesis only (yosys 0.33 `-abc9`, the
Makefile's file list; no place and route yet):

| sergei_ml2 | LUT4 | CCU2C | FF | DP16KD | DPR16X4 |
|---|---|---|---|---|---|
| as released: picorv32, wb_cache, MPU | 26,748 | 2,816 | 14,688 | 69 | 118 |
| `CPU_ZEITLOS32P` (round 3): zeitlos32p, wb_cache_split, no MPU | 27,396 | 2,705 | 14,521 | 68 | 150 |
| `CPU_ZEITLOS32P` (round 4): zeitlos32p, wb_cache_split, wb_mpu_split | 28,358 | 2,937 | 15,178 | 68 | 150 |

Round 4 is +1,610 LUT4 over the released build, and +962 over round
3, which had no MPU at all; out of context, wb_mpu_split and the
core's privilege tracking come to about 1,090. `check` reports nothing new: the warnings are the same as the
released build's, line for line.

**Nothing changes on other builds.** `wb_cache`, `wb_mpu` and both
older cores are untouched, and everything new in rtl/sysctl.v is
behind `CPU_ZEITLOS32P`. The released sergei_ml2, synthesised from the
round-4 tree: 26,757 LUT4 against 26,748 before (+9, yosys mapping
noise: FF 14,688, CCU2C 2,816 and DP16KD 69, all identical).

## Testing

`rtl/cpu/zeitlos32p/tests/`. `make` runs everything.

- **Directed.** zeitlos32's own programs (`t_alu`, `t_branch`,
  `t_mem`, `t_muldiv`, `t_csr`, `t_irq`, `t_illegal`), unchanged.
  `t_timer` is this directory's own (`prog/`): zeitlos32 has no timer.
  - They run against a model of the cache's two ports, at five
    latency pairs: I/D 0/0, 3/3, 12/12, 0/12 and 12/0.
  - They also run with random latency and random not-ready cycles,
    on three seeds.
- **Random.** zeitlos32's `gen_random.py` programs against their
  golden model, at zero latency and at random latency with random
  not-ready cycles.
- **Lockstep** (`tb_lockstep.v`). zeitlos32 and zeitlos32p side by
  side, each with its own randomised memory. Every register write and
  every store must match, in order, value for value. It runs on the
  directed programs and 15 random ones.
- **Coverage.** `+cov` prints how often S was in use, how often a
  request was sent from a slot rather than from X, and how often X's
  own request was refused. With a slow D side
  (`tb_l0_12`) a random program fills S on about half its cycles.
- **The MPU lockstep** (`tb_mpu_lockstep.v`, `make mpu`): see
  [The MPU](#the-mpu).
- **Stress** (`make stress`, `gen_stress.py`): random load/store-heavy
  programs, every interrupt masked, on the MPU lockstep; see "Bugs
  found after round 3".
- **Interrupt stress** (`make irqstress`, `tb_irq_stress.v`): the same
  generator with `--irq`, IRQ 3 pulsed at random on zeitlos32p only
  (four latency / pulse-rate configurations), a handler that counts
  and returns. The two cores take interrupts at different points, so
  instead of write for write the final memories must match, all but
  the handler's counter.
- **The SOC bench.** `rtl/tb/tb_cache_soc.v` with `-DCPU_Z32P`, alone,
  with `-DMPU` (its enforcing-MPU workload and app, rtl/mpu_split.v)
  and with `-DARB` (the real arbiter, a blitter-like reader checking
  every word it reads, a mailbox writer).

```
cd rtl/cpu/zeitlos32p/tests && make             # everything
make one T=t_irq                                # one program
make lockstep                                   # zeitlos32 vs zeitlos32p
make mpu                                        # ...with their MPUs
make stress irqstress                           # random, with and without IRQs
FAST_MUL=1 make                                 # the DSP multiplier
```

The SOC bench:

```
iverilog -g2005 -DCPU_Z32P -DBURST=1 -o tb_socp rtl/tb/tb_cache_soc.v \
  rtl/cache_id.v rtl/cache.v rtl/cache_split.v rtl/mem/sdram_kianv.v \
  rtl/tb/sdram_model.v rtl/cpu/picorv32/picorv32.v \
  rtl/cpu/zeitlos32/zeitlos32.v rtl/cpu/zeitlos32/zeitlos32_muldiv.v \
  rtl/cpu/zeitlos32/zeitlos32_decode.v rtl/cpu/zeitlos32/zeitlos32_alu.v \
  rtl/cpu/zeitlos32/zeitlos32_lsu.v rtl/cpu/zeitlos32p/zeitlos32p.v \
  rtl/arbiter_main.v
vvp tb_socp +prog=rtl/tb/cache_soc/prog.hex
```

`+maxcyc=N` raises the bench's 6M-cycle limit (the LZ4 program with
`-DARB` needs more; 20M is plenty) and `+progress` prints the instruction count
every 250k cycles, to tell a slow run from a stuck one.
`rtl/tb/cache_soc_lz4/README.md` has the LZ4 program.

## Clean-room

Like zeitlos32, this is original code, written for this SOC from the
RISC-V specification and the interrupt ABI as `sw/bios/custom_ops.S`
and zeitlos32 implement it. The only code it reuses is zeitlos32's own
units.
