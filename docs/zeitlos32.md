# zeitlos32

An experimental in-house RV32IM core, living at `rtl/cpu/zeitlos32/`,
intended eventually to replace PicoRV32 as the Zeitlos CPU.

It is **not** the default. Nothing changes until you uncomment one
line in `rtl/boards.vh`:

```verilog
`define CPU_ZEITLOS32
```

Everything else about the SOC — the memory map, the BIOS, the kernel,
`custom_ops.S`, the wishbone fabric — is untouched either way.

## Status

Based on `e02af33`. Passes its own test suite (RV32IM, the counter
CSRs, and the picorv32 interrupt ABI) in simulation, at three fixed
memory latencies plus three randomised-latency profiles, in both
multiplier configurations. Synthesises clean and meets 60MHz on ECP5.

Zeitlos can run on Zeitlos32, but we plan to continue using PicoRV32 for development until the CPU is more proven.

### On the main-bus arbiter

`rtl/arbiter_main.v` puts the blitter's source reads on the same bus
as the CPU, so a CPU access now takes however long the arbiter takes
to get round to it, and the CPU can lose the bus between transactions.
zeitlos32 satisfies what that arbiter requires: it holds address,
data, `sel`, `we` and `cyc` stable from the cycle it asserts `stb`
until the cycle it sees `ack`, and drops `cyc` on `ack` — so a
transaction is never torn, and the grant is free to rotate between
them. It has no internal timeout and no assumption that consecutive
accesses take the same time.

The test suite covers this directly: as well as the three fixed
latencies, everything runs three times with the wait-state count
varying per access. Constant latency can hide an off-by-one that only
bites when two consecutive accesses differ.

## Why

PicoRV32 works and is not the problem. The reasons to have our own:

- **It's ours.** A bug in the CPU currently means reading someone
  else's 2000-line core; the whole of zeitlos32 is a single FSM with
  nine states and no pipeline.
- **It implements what this SOC uses, and nothing else.** No PCPI, no
  compressed ISA, no IRQ timer, no trace port, no two-cycle-ALU size
  options. Those are all real features and none of them are used here.
- **It's a place to try things.** Being able to A/B two cores against
  an otherwise identical bitstream turns "is it my scheduler or my
  core?" from an expensive question into a one-line experiment.

The reason *not* to switch yet is that PicoRV32 has years of use
behind it and this has a few days.

## Design

A multicycle FSM. Not a pipeline: no hazards, no forwarding, no
stalls. Every instruction walks the same path.

| state | what happens |
| --- | --- |
| `ST_FETCH` | decide whether to take an interrupt; if not, issue the fetch |
| `ST_FWAIT` | wait for ack, latch the instruction |
| `ST_RS` | read `rs1`/`rs2` out of the register file |
| `ST_EX` | ALU, branch decision, address calculation — most instructions retire here |
| `ST_LWAIT` | load: wait for ack, extract and extend, retire |
| `ST_SWAIT` | store: wait for ack, retire |
| `ST_MULDIV` | wait for the sequential multiplier/divider |
| `ST_WAITIRQ` | `waitirq` stalled with nothing pending |
| `ST_TRAP` | halted |

That comes to 5 cycles for an ALU instruction against a slave that
acks in one cycle, and 8 for a load.

Every wishbone output is registered, as in `picorv32_wb`. In
`rtl/sysctl.v` the CPU address feeds `rtl/mtu.v` and then a large
chip-select mux combinationally, so an unregistered address would drag
the entire decode fabric into the core's own timing path. That costs
a cycle of bus turnaround per access and is worth paying — see
`docs/muldiv.md` for how little margin this design has historically
had.

### Performance

Measured in simulation, same test programs, same memory model, both
cores configured as `rtl/sysctl.v` configures them today (barrel
shifter, MUL+DIV, IRQs on). Cycle counts, lower is better:

| test | wait states | zeitlos32 | picorv32 | |
| --- | --- | --- | --- | --- |
| `t_alu` | 0 | 906 | 1286 | −30% |
| `t_branch` | 0 | 570 | 844 | −32% |
| `t_mem` | 0 | 1590 | 2482 | −36% |
| `t_muldiv` | 0 | 2650 | 3628 | −27% |
| `t_alu` | 3 | 1452 | 1940 | −25% |
| `t_mem` | 3 | 2686 | 3798 | −29% |
| `t_alu` | 12 | 3090 | 3902 | −21% |
| `t_mem` | 12 | 5970 | 7750 | −23% |

Roughly 20–35% fewer cycles, narrowing as memory gets slower — which
is what you would expect, since at 12 wait states both cores are
mostly waiting for the bus. The interesting consequence is the
opposite of the usual one: on the SDRAM and PSRAM boards the CPU is
not the bottleneck and this advantage largely disappears, so the
argument for zeitlos32 is size and legibility rather than speed.

### Size and timing

Yosys 0.33 + nextpnr-ecp5, `--25k --package CABGA381`, each core
synthesised standalone with its ports registered to I/O so the
measurement is of the core's own logic. Fmax is the worst of three
placement seeds.

| | LUT4 | FF | LUTRAM | DSP | Fmax |
| --- | --- | --- | --- | --- | --- |
| zeitlos32, `FAST_MUL=0` | 3213 | 845 | 32 | — | ~79 MHz |
| zeitlos32, `FAST_MUL=1` | 3176 | 781 | 32 | 4 | ~69 MHz |
| picorv32 (as `sysctl.v` builds it) | 2780 | 1179 | — | 4 | ~80 MHz |

Read that honestly: **zeitlos32 is about 15% larger than picorv32 in
LUTs**, not smaller. It uses fewer flip-flops and no block RAM
(picorv32 puts its register file in two DP16KD; this one uses
distributed RAM), which is a better trade on a part where BRAM is
scarce and a worse one on a part where LUTs are. The case for it is
cycle count and legibility, not area.

On timing, the sequential-multiplier build is at parity with picorv32
and the DSP build costs about 13%. Both clear 60MHz with margin, and
the historical critical path for the whole SOC was `gpu_blit.v`'s
clip-rect carry chain rather than the CPU (`docs/muldiv.md`), so
neither should be what limits the design. This has not been checked
against the full `sysctl.v` place-and-route.

### Multiply and divide

`rtl/cpu/zeitlos32/zeitlos32_muldiv.v`. Both operations work on
magnitudes and fix the sign afterwards, rather than the more compact
trick of sign-extending and letting the arithmetic sort itself out.
The magnitude version is directly checkable against the definition of
the instruction, which matters more than the three conditional
negations it costs.

| | `FAST_MUL=0` | `FAST_MUL=1` |
| --- | --- | --- |
| multiply | 34 cycles | 3 cycles |
| divide | 35 cycles | 35 cycles |

The DSP operands are registered before the multiply, which is what
makes it three cycles rather than two. Feeding the DSP array straight
from the register file put the read, the sign-extension mux, four
cascaded `MULT18X18D` and the partial-product adder tree into one
combinational path — about 11ns, most of a 60MHz cycle, in an
instruction that takes several cycles anyway.

Divide is always sequential. Divides are far rarer than multiplies in
this codebase and a fast divider is a much worse area and timing trade
than a fast multiplier.

`FAST_MUL` follows `` `CPU_MUL_FAST `` in `rtl/boards.vh` and maps onto
ECP5 `MULT18X18D` blocks. **Do not enable it on GateMate**: the
top-level `Makefile` passes `-nomult` to `synth_gatemate` for
Lebkuchen and Kölsch, so the multiplier lands in LUTs — large, and
squarely on the critical path.

## Which core is in this bitstream?

The two cores are drop-in compatible, so the same kernel binary runs on
either and nothing in a running system would otherwise say which one is
present. `rtl/csrs.vh` therefore exposes bit 23 of the FEATURES word,
set when `` `CPU_ZEITLOS32 `` is defined:

```c
printf("cpu: %s\n", z_soc_cpu_name());     /* sw/common/zsoc.h */
```

Clear means picorv32 — which is also what every bitstream built before
this bit existed reports, so "clear" and "predates the bit" agree
rather than conflicting. `z_soc_cpu_name()` returns `"unknown"` when
`rtl/csrs.v` is absent entirely, because there the bit is not merely
clear, it is unreadable, and answering "picorv32" would be a guess
dressed as a fact.

The core also appears in `k_soc_report()`'s feature list as `zeitlos32`
in the CPU group.

## Compatibility with PicoRV32

The core reproduces PicoRV32's interrupt ABI deliberately, because
`sw/bios/boot_picorv32.S` and `sw/bios/custom_ops.S` are a fixed
interface. `getq`, `setq`, `retirq`, `maskirq` and `waitirq` use the
same encodings and have the same semantics, including the parts that
look like bugs:

- **`waitirq` reports the pending set without clearing it.** The BIOS
  reset path is `waitirq` immediately followed by
  `maskirq(zero, zero)`, and the interrupt it just waited for is meant
  to be delivered the instant the mask opens. Clearing would swallow
  the first timer tick. Only delivery clears a pending bit.
- **`irq_mask` resets to all ones**, so nothing can fire before the
  BIOS opens it.
- **`irq_pending` accumulates from the `irq` input regardless of the
  mask**, which is what lets the reset-vector `waitirq` return at all.
- **Exactly one instruction always executes after `retirq`** before
  another interrupt can be taken. Without that, a level-triggered
  source still asserted on return would livelock the handler.
- **`LATCHED_IRQ`** is passed through from `rtl/sysctl.v` unchanged
  (`0xffffffef` — everything latched except IRQ 4, the 16550, which is
  level).

The test suite is cross-run against PicoRV32 itself, and all of it
passes on both cores except for one deliberate difference:

- **FENCE is accepted as a nop.** PicoRV32 has no FENCE decode at all
  and faults on it. Accepting an instruction PicoRV32 rejects cannot
  break anything that runs on PicoRV32 today, and it means a future
  compiler emitting `fence` for rv32im needs no core change.

There is one more difference, in fault handling, which no software in
this tree can observe:

- On an illegal instruction, `ecall`/`ebreak`, or a misaligned access,
  zeitlos32 retires the faulting instruction **without executing it**
  and raises IRQ 1 (illegal/ebreak) or IRQ 2 (misaligned) at the next
  boundary — so `q0` points at the *next* instruction. PicoRV32 leaves
  the faulting instruction's own address in `q0`. `rtl/sysctl.v` wires
  neither IRQ line to anything, so nothing depends on either choice.
  If the relevant line is masked, or the core is already in a handler,
  it halts and asserts `trap` instead (shown on `LED_R`).

Counter CSRs (`rdcycle`, `rdcycleh`, `rdinstret`, `rdinstreth`) are
implemented because `sw/os/kernel.c` and `sw/os/sh.c` read them
directly. They are the only CSRs; there is no CSR file.

## Testing

```
cd rtl/cpu/zeitlos32/tests
make
```

Needs `iverilog`, `cpp`, and RISC-V binutils. It does **not** need the
toolchain `sw/common/arch.mk` pins — only the assembler and linker are
used, no C compiler, no libc, no libgcc — so the core can be tested on
a machine that cannot build Zeitlos itself.

`make` runs two things:

**Directed tests** (`tests/prog/`), at 0, 3, and 12 wait states, then
three more passes with the latency randomised per access in 0..8. The
fixed profiles model BRAM, SRAM and SDRAM; the random ones model
sharing the main bus with the blitter through `rtl/arbiter_main.v`. A
bus handshake bug that only appears when ack is slow, or only when two
consecutive accesses differ, is exactly the kind that otherwise
reaches the board.

| file | covers |
| --- | --- |
| `t_alu.S` | arithmetic, logic, shifts, comparisons, both operand forms |
| `t_branch.S` | every branch in both directions, `jal`/`jalr`, nested calls |
| `t_mem.S` | all widths at all alignments, byte-lane steering, neighbours preserved |
| `t_muldiv.S` | RV32M including the two specified divide special cases |
| `t_csr.S` | the four counter reads |
| `t_irq.S` | the whole picorv32 interrupt ABI |
| `t_illegal.S` | illegal instructions, `ecall`/`ebreak`, misaligned access |

**Randomised differential tests** (`tests/gen_random.py`). Each seed
generates a long straight-line RV32IM program, runs it against a
golden model written in Python, and emits a program that checks all 30
architectural registers and 64 words of scratch memory against the
model's answer. Directed tests only find bugs someone thought of; this
finds the operand patterns, shift amounts and sign combinations nobody
would write by hand.

Useful invocations:

```
make T=t_muldiv one      # one test, verbose
make T=t_muldiv wave     # ...dumping tb_zeitlos32.vcd
make T=t_muldiv dis      # disassemble it
make FAST_MUL=1          # the DSP multiplier build
make random SEEDS="1 2 3" COUNT=2000
```

A failing directed check reports its id; the ids are namespaced per
file (100s for `t_alu`, 200s for `t_branch`, and so on) so the number
alone identifies the check. A failing random test leaves its generated
`.S` in `prog/` for disassembly.

### Four bugs found so far

Worth recording, because both are the kind that survive casual
testing:

1. **`FAST_MUL` double sign correction.** The 33×33 signed multiply
   already produces a correctly signed product, but the sign-fix mux
   was still negating it. This is wrong *only* when the two operands
   have different signs, so it passes every test written with positive
   numbers. Found by `t_muldiv` check 414 and by 8 of 15 random seeds.
2. **`waitirq` clearing the pending set.** Caught by cross-running the
   suite against PicoRV32 and finding that PicoRV32 disagreed. See the
   compatibility section above for why this one mattered.
3. **Two drivers on the multiplier's `result`.** It was assigned by
   the combinational result mux *and* in the reset branch of the
   sequential block. Legal Verilog, simulates correctly (last write in
   source order wins), and yosys resolves it by tying the net to the
   reset constant — a core that passes every test in this directory
   and returns zero from every multiply and divide on real hardware.
   Simulation cannot find this one; only synthesis can. Caught by the
   "multiple conflicting drivers" warnings on the first real build.
4. **`ENABLE_MUL` / `ENABLE_DIV` not reaching the datapath.** The
   parameters gated whether `zeitlos32.v` would *issue* a multiply or
   divide, but `zeitlos32_muldiv.v` built its full datapath
   regardless, so `ENABLE_DIV=0` saved 28 LUTs instead of 715. A
   parameter that appears to work and does nothing.

Three of those four are invisible to simulation, which is worth
remembering: this suite is necessary and it is not sufficient. Run
`yosys` and read the warnings.

## A software bug this core exposed

Worth recording, because it is the shape of thing a faster CPU finds
and because nothing about it is in the RTL.

`sw/os/uart.c`'s `k_uart_putc()` sent a character directly to the
16550 only when the software ring happened to be empty *and* THRE was
set; otherwise it queued, and draining was left entirely to
`z_uart_irq()`. Two consequences:

1. `z_uart_irq()` is only reachable once `reg_kernel` (`0x0000000c`)
   is set, which `kernel.c` does *after* `k_soc_report()`. Every
   `printf()` before that queued into a ring nothing emptied.
2. The "ring was empty" test is a **one-way latch**. `_write()` calls
   `k_uart_putc()` once per character with a handful of instructions
   in between, so a single 40-byte string outruns a 1 Mbaud UART (480
   CPU cycles per character). After that `fifo_was_empty` is false
   forever and no character is ever sent directly again, however idle
   the CPU becomes.

`_write()`'s `while (k_uart_tx_full()) ;` then spins forever. No trap,
no `LED_R`, output simply stops part-way through a long run.

picorv32 survived on timing alone — slow enough, most of the time, to
stay on the right side of the burst rate. zeitlos32 is 20-35% fewer
cycles for the same work and crosses it. The measured symptom was a
ring already 21 characters behind on entry with `tx_tail` frozen at 1.

The fix is a `tx_pump()` that pushes whatever the UART will accept
whenever it is called, used by `k_uart_putc()`, `k_uart_tx_full()` and
the ISR alike, so forward progress never depends on the interrupt
being reachable. `tx_head`/`tx_tail` were also `uint8_t` against a
512-entry ring, so the usable depth was silently 256.

## Debugging a failure on hardware

The important thing to know first: **an illegal instruction does not
light LED_R.** `sw/bios/boot_picorv32.S` unmasks everything, so a
decode failure raises IRQ 1 rather than asserting `trap`. The kernel's
handler has no case for IRQ 1, so it returns — and because zeitlos32
retires the faulting instruction without executing it, execution
resumes with that instruction *silently skipped*. No message, no trap,
just wrong behaviour somewhere downstream.

So, in order:

1. **`tests/decoder_audit.py <elf>`** — scans a binary for anything
   zeitlos32's `is_legal` would reject. zeitlos32 decodes `funct7`
   strictly where picorv32 ignores it, and implements six CSRs where
   picorv32 also implements six but the kernel may want more. A binary
   that runs on picorv32 can contain instructions this core refuses.
2. **Instrument the interrupt handler.** In the kernel's IRQ path,
   report any `irqs & 0x6` (illegal/ebreak, misaligned) together with
   `regs[0]` — that is `q0`, the saved PC. On zeitlos32 it points at
   the instruction *after* the fault. That names the offending
   instruction directly.
3. **Check whether the bus is stuck.** zeitlos32 has no bus timeout:
   a store to an address no slave acks parks it in `ST_SWAIT`
   forever. `rtl/cpu/zeitlos32/tests/tb_bios.v` hit exactly this when
   its model was missing the LED register.
4. **`tests/tb_bios.v`** boots the real BIOS against the real 16550 in
   simulation. Build `sw/bios/bios.hex` first — `bios_seed.hex` in the
   tree is an `ecpbram` placeholder, not a usable image.
5. **`sw/os/z32check.c`** — an in-kernel self-test that runs entirely
   on `kprint()`, so it works at a point where `printf()` does not.
   Add it to the kernel sources and call `z32_check()` from `main()`
   immediately after `z_uart_init()`. Each test prints its name
   *before* it runs, so a hang leaves the guilty test on the console
   and a wrong answer prints expected vs actual. It covers ALU,
   shifts, RV32M (including divide-by-zero and signed overflow),
   sub-word loads and stores, byte-copy loops, deep recursion,
   indirect calls and jump tables, varargs, `_sbrk`, and a
   read-modify-write walk over main memory.

   `z32_check_soc()` covers what `k_soc_report()` touches — both kinds
   of "CSR", the counter instructions and `rtl/csrs.v`'s memory-mapped
   registers — and `z32_check_printf()` walks up to `printf()` in
   stages, printing "back" after each one returns.

   **Make stdout unbuffered before trusting any boot log.** If newlib
   fully buffers stdout, `printf()` output only appears when the buffer
   fills, so the log stops at whatever call happened to fill it rather
   than at whatever failed. `setvbuf(stdout, NULL, _IONBF, 0)` at the
   top of `main()` fixes that, and is worth doing before drawing any
   conclusion of the form "it dies right after X".

   Note that this whole suite **passes on zeitlos32 in simulation**.
   If it also passes on hardware, the fault is narrower than "the core
   is broken" and the next suspects are timing and board-specific
   behaviour rather than the ISA.

### Slave handshakes this core has been tested against

`tests/tb_zeitlos32.v` now models four, because the SOC's slaves do not
agree with each other and the differences are exactly where a bus bug
lives:

| model | matches | notes |
| --- | --- | --- |
| `MEM_LAT=N` | `rtl/mem/bram.v` | registered ack, one cycle after stb |
| `MEM_RANDLAT=1` | contention via `rtl/arbiter_main.v` | latency varies per access |
| `MEM_SRAM=1` | `rtl/mem/sram.v` | ack free-runs on a 3-cycle counter, ignoring stb and cyc |
| `MEM_COMB=1` | `rtl/csrs.v`, `rtl/socctl.v`, `rtl/mtu.v` | combinational same-cycle ack AND data |
| `MEM_MIXED=1` | Obst: MMIO combinational, RAM free-running | both, alternating, in one instruction stream |

`MEM_MIXED` exists because every other model applies one slave timing
to the whole address space, while the real machine constantly switches
between them — `k_soc_report()`'s feature loop reads two `csrs.v`
registers (same-cycle ack) and fetches its instructions from `sram.v`
(free-running ack) on every iteration. The transition between two very
different handshakes was untested until this was added.

The suite passes against all five. `MEM_COMB` is the fastest handshake
in the machine — it takes the core to 4 CPI instead of 5, since the bus
turnaround disappears entirely.

One consequence worth carrying around: **an address with no slave never
acks, and this core has no bus timeout.** It waits in `ST_LWAIT` or
`ST_SWAIT` forever. `rtl/sysctl.v`'s ack mux ends in `1'b0`, so this is
reachable — on a board without `ICACHE`, `cs_csrs` is deliberately
widened to absorb the `0x7000_01xx` window for exactly this reason.

### A note on rtl/mem/sram.v

Obst has no instruction cache, so every fetch goes to SRAM. That
controller's ack state machine does not look at `wb_stb_i` or
`wb_cyc_i` at all — it free-runs on a three-cycle loop, so which cycle
a request completes in depends on the phase between that counter and
the CPU's access pattern. zeitlos32's handshake survives it (there is
a `MEM_SRAM=1` model of exactly this behaviour in
`tests/tb_zeitlos32.v`, and the suite passes against it), but a core
with different timing than picorv32 meets different phases, and the
resulting access time on an asynchronous external SRAM is something
only hardware can settle.

## Not done yet

- Hardware. Everything above is simulation and place-and-route; no
  bitstream has ever been loaded. This is the biggest gap.
- Place-and-route of the full `sysctl.v` with `CPU_ZEITLOS32` set, to
  see whether the CPU becomes the critical path in context.
- Booting the actual BIOS in simulation (`rtl/tb/tb_soc.v`).
- Running with the blitter actually contending for the main bus. The
  randomised-latency model approximates it; `rtl/tb/tb_arbiter_main.v`
  and `rtl/gpu/bench/tb_memblit.v` are the real thing.
- Hardware bring-up on any board.
- `riscv-tests` / `riscv-formal`. The core has no RVFI port; adding one
  would let `riscv-formal` prove ISA conformance rather than sample it.

Ideas noted in the source, not acted on:

- Merging `ST_RS` into the fetch-ack cycle would give 4 CPI instead of
  5, at the cost of a path from bus data through the register file
  address. Worth measuring once there are timing numbers to measure
  against.
- The multiplier and divider each have their own adder. Sharing would
  save perhaps 40 LUTs and cost a good deal of clarity.
- **Control flow in the randomised tests.** `gen_random.py` generates
  straight-line code only: no branches, no jumps, no `jalr`. That was
  a deliberate simplification and it is the largest remaining hole in
  the suite, because compiled C is dense with exactly what it omits --
  jump tables, computed calls, long branch displacements. This is the
  first thing to fix.
- Closing the remaining ~400 LUT gap to picorv32. The decode wires and
  the twelve-way opcode case are the obvious places to look; the
  register file is already distributed RAM.
- A second pipeline stage inside the DSP multiply would recover the
  ~13% of Fmax `FAST_MUL=1` currently costs.
