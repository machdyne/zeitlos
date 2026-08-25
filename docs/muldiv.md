# rv32im: hardware multiply and divide

## What changed

`rtl/boards.vh` now defines `CPU_MUL`, `CPU_MUL_FAST` and `CPU_DIV` in
the universal section, and `rtl/sysctl.v` wires them to picorv32's
`ENABLE_FAST_MUL` / `ENABLE_MUL` / `ENABLE_DIV`. Software builds as
`rv32im`.

Every `*`, `/` and `%` in C previously became a libgcc call --
`__mulsi3`, `__divsi3`, `__udivsi3` -- executing a shift-add or
shift-subtract loop. Those are now single instructions.

## Measured benefit

`rtl/tb/tb_soc.v` running the real picorv32, comparing one MUL against
a shift-add loop of the kind libgcc uses, counting **cycles per
multiply** rather than IPC:

| | Cycles per multiply |
|---|---|
| rv32i, software shift-add | 442.5 |
| rv32im, hardware MUL | 29.2 |
| | **15.2x** |

IPC is identical between the two (0.172 both ways) and that is exactly
why IPC is the wrong metric here: a software multiply retires ~200
instructions to do what MUL does in one, so both look equally "busy"
while one does 15x less work. Anything comparing these builds must
count work, not instructions.

The real-world gain is somewhat larger still: this benchmark inlines
the shift-add loop, whereas libgcc's `__mulsi3` is a function call with
its own prologue and return.

DIV benefits more per operation (the software routine is longer) but
appears far less often in practice.

## Cost on ECP5 (Lakritz, LFE5U-25F)

| Configuration | LUT4 | MULT18X18D | FF |
|---|---|---|---|
| icache only, no M | 11520 | 1 | 4978 |
| icache + `CPU_MUL` (sequential) | 12337 | 1 | 5457 |
| icache + `CPU_MUL_FAST` + `CPU_DIV` | 11996 | 5 | 5335 |

Note the DSP multiplier is **cheaper in LUTs than the sequential one**
(11996 vs 12337) because the multiply moves into DSP blocks. It is
faster *and* smaller, so there is no reason to prefer `CPU_MUL` alone
unless timing forces it.

## Timing: measured, and it is not the multiplier

The concern with `ENABLE_FAST_MUL` is real in principle -- picorv32
instantiates `picorv32_pcpi_fast_mul` with `EXTRA_MUL_FFS=0`, i.e. an
unpipelined 32x32 multiply. A timing failure there would not look like
a timing failure; it would look like random corruption and crashes.

So it was measured, with nextpnr-ecp5 on Lakritz:

| Build | Fmax reported |
|---|---|
| pristine (no cache, no M) | 47.06 MHz |
| + icache | 46.50 MHz |
| + icache + FAST_MUL + DIV | **47.96 MHz** |

Adding the multiplier did not reduce Fmax; the spread across all three
is within nextpnr's placement noise. **The multiplier is not the
critical path.**

The actual critical path, from the nextpnr report, is in the GPU
blitter:

```
wbm_blit0_i.work_dst_x  ->  final_x[1]  ->  rect_x_end (CCU2C carry chain)
    rtl/gpu/gpu_blit.v:86, :113
```

That is the clip-rectangle comparison in `gpu_blit.v`. If the clock
ever needs raising, that is where to look -- not the CPU.

**Caveat on absolute numbers.** These were produced with yosys 0.33 and
nextpnr 0.6, which are older than what this project builds with, and
they report the *pristine* design as failing 48 MHz -- which it plainly
does not, since it runs. Treat the comparison between rows as
meaningful and the absolute values as pessimistic. Check your own
`output/<board>/report.txt` after building.

## The failure mode this guards against

The asymmetry is what makes this worth care:

- **rv32i software on rv32im gateware**: fine. M simply goes unused.
- **rv32im software on rv32i gateware**: fatal.

In the second case every `mul` is an illegal instruction. picorv32 here
is built with `CATCH_ILLINSN` and `ENABLE_IRQ`, so that raises IRQ 1 --
and `sw/os/kernel.c` handles only KTIMER, UART and HID. The handler
returns, the same instruction executes again, and the machine spins.
No trap message, no diagnostic, just a hang somewhere that looks
unrelated to multiplication.

Three things now prevent that:

**1. One definition of ARCH.** `sw/common/arch.mk` is the single source
of truth, included by all 18 Makefiles. Previously each hardcoded
`ARCH = rv32i` separately, so a partial edit would produce a mixed
build -- some binaries rv32im, some not.

**2. The toolchain moves with it.** This is the easy one to miss:
`PREFIX` pointed at `/opt/riscv32i`, the *pure RV32I* toolchain from
picorv32's build instructions, whose libgcc and newlib are compiled for
rv32i only. Passing `-march=rv32im` to it does not produce an rv32im
build -- at best it links the rv32i libraries anyway, so libc keeps
calling `__mulsi3` and you lose the benefit exactly where a lot of it
would be. `arch.mk` derives `RISCV_PREFIX` from `ARCH`, so they cannot
drift:

```
make -C /path/to/picorv32 build-riscv32im-tools   # installs /opt/riscv32im
```

**3. A boot-time check.** `rtl/csrs.vh` exposes `CPU_MUL` (bit 20),
`CPU_DIV` (21) and `CPU_MUL_FAST` (22). GCC defines `__riscv_mul` /
`__riscv_div` when `-march` includes M, so `z_soc_check_cpu_arch()`
(`sw/common/zsoc.h`) compares what the binary was built for against
what the bitstream actually has, and the kernel prints a clear
`*** CPU MISMATCH ***` block instead of hanging. That function contains
no multiply or divide itself, so it is safe to run before the answer is
known.

The boot log now shows the CPU line first:

```
 - soc features:
     cpu     mul mul-hw div
     memory  sram vram rom glyph
     ...
     build   rv32im
```

## Toolchain age

picorv32's instructions pin riscv-gnu-toolchain rev 411d134 (2018-02-14),
roughly GCC 7. For this change that is **fine**:

- **M has been supported since the first RISC-V GCC** (upstreamed in
  GCC 7, 2017). `-march=rv32im -mabi=ilp32` works on a 2018 toolchain.
- `__riscv_mul` / `__riscv_div` / `__riscv_muldiv` have been predefined
  since the same port, so the boot check works there too. It no longer
  *depends* on that, though -- see below.

Two things that age does rule out:

- **`rv32i_zmmul` is not available.** Zmmul needs gcc/binutils 12 or
  newer; a 2018 toolchain gives "unknown ISA extension". So the
  multiply-without-divide fallback below is not an option without
  upgrading.
- **Codegen is simply older.** GCC 13 produces noticeably tighter RV32
  code than GCC 7, and smaller code means a better instruction-cache
  hit rate on top of the raw improvement. If you want another
  broad-based speedup that costs no gates, rebuilding the toolchain
  from a current riscv-gnu-toolchain is a real one -- worth measuring
  with `cache` and the boot MIPS figure before and after.

### Why the boot check does not trust the compiler

`sw/common/arch.mk` derives `Z_ARCH_HAS_MUL` / `Z_ARCH_HAS_DIV` from
`ARCH` itself and passes them as `-D` flags; GCC's own macros are only
a fallback for code not built through those Makefiles.

The reason is the failure mode, not distrust of any particular
compiler. If the check relied on `__riscv_mul` alone and some toolchain
spelled it differently, it would not fail loudly -- it would silently
evaluate to "this binary has no multiply, nothing to verify" and the
safety net would vanish with no indication. Deriving it from the build
system removes that possibility.

`__riscv_zmmul` is checked in the fallback path as well, and this is a
concrete trap rather than a hypothetical: verified on GCC 13,
`-march=rv32i_zmmul` defines `__riscv_zmmul` and does **not** define
`__riscv_mul`. A check looking only for `__riscv_mul` would conclude a
zmmul binary has no multiply while its text section is full of them.

## If DIV proves troublesome

`rv32i_zmmul` keeps multiply and drops divide entirely: set
`ARCH = rv32i_zmmul` in `arch.mk` and comment out `CPU_DIV` in
`boards.vh`. That keeps essentially all the benefit -- multiplies
vastly outnumber divides in practice -- while removing the sequential
divider and its PCPI interaction from the design.

This needs gcc/binutils 12+, so on the pinned 2018 toolchain it is not
available. Without upgrading, the options are rv32im (mul and div) or
rv32i (neither); there is no supported way to ask for one and not the
other. `arch.mk` handles `rv32i_zmmul` correctly if you do upgrade.

## Rebuild both halves

Gateware and software must be flashed together. That is not a style
preference here; it is the difference between working and hanging.
