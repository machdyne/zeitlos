# zeitlos32p

The pipelined zeitlos32: five stages, one instruction a clock at
best, with the same RV32IM, counters and picorv32 interrupt ABI, so
nothing in software changes. **Not the default.** A build asks for it:

```
make BOARD=sergei_ml2 EXTRA_DEFINES=-DCPU_ZEITLOS32P
```

It needs `DCACHE` and `ARBITER`. With `MPU` it gets `rtl/mpu_split.v`
(`ENABLE_MPU`), the MPU with privilege tracked per instruction.

Full write-up: [`docs/zeitlos32p.md`](../../../docs/zeitlos32p.md).

```
zeitlos32p.v           the core (F, D, X, M/S, W)
tests/                 make && go: directed, random, lockstep with
                       zeitlos32, the MPU lockstep (tb_mpu_lockstep.v)
tests/prog/            t_timer.S, t_mpu.S: this core's own programs
```

Built from zeitlos32's units (`../zeitlos32/zeitlos32_decode.v`,
`zeitlos32_alu.v`, `zeitlos32_lsu.v`, `zeitlos32_muldiv.v`). It talks
to `rtl/cache_split.v`'s instruction and data ports rather than to a
Wishbone bus.

## Quick start

```
cd tests && make
```

Needs `iverilog`, `cpp`, `python3` and RISC-V binutils, as zeitlos32's
suite. The programs are zeitlos32's own (`../zeitlos32/tests/prog`),
built by that directory's Makefile.
