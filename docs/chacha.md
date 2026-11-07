# ChaCha20 and the CPU's rotate instructions

ChaCha20 encrypts everything `ssh` and `netserve`'s SSH server send
(chacha20-poly1305@openssh.com) and everything `web` sends over TLS
(TLS_CHACHA20_POLY1305_SHA256). It also keeps the random numbers coming
(`zrng`). A quarter of its operations are 32-bit rotates, and RV32IM
has none: each rotate takes three instructions, a shift each way and
an OR.

`CPU_ROT` gives picorv32 the three rotate instructions of RISC-V's Zbb
extension, `ror`, `rol` and `rori`. ChaCha20 then needs about a third
fewer instructions. Software asks the bitstream whether it has them, so
one binary runs on every board, with them or without.

## Where it is on

| | |
|---|---|
| `rtl/boards.vh` | `` `define CPU_ROT `` -- on Lakritz, off elsewhere |
| cost | a rotator beside picorv32's barrel shifter: a 64-to-32 funnel, about 100-200 LUT4 (not yet measured by PnR) |
| software sees it | FEATURES3 bit 3, `Z_FEATURE3_CPU_ROT`, `z_cpu_has_rot()` (`sw/common/zsoc.h`) |

The cores: picorv32 only. `zeitlos32` and `zeitlos32p` do not have the
instructions yet, and the feature bit stays clear when either core is
chosen, whatever `CPU_ROT` says. Software then uses shifts.

To leave it out of a board's bitstream, remove the line. Nothing else
changes, and no software is rebuilt.

## What it saves

Measured in the simulator, in instructions -- the same work on
picorv32, where an ALU instruction takes the same cycles either way
(`sw/common/tests/sim_chacha`):

| | shifts | `rori` | |
|---|---|---|---|
| one 64-byte block (20 rounds) | 1,682 | 1,051 | -38% |
| 114 bytes through monocypher | 5,934 | 4,652 | -22% |
| 1 KB of keystream | 32,265 | 22,009 | -32% |

A whole SSH or TLS record also pays for Poly1305 and the copies, so
the saving on the wire is less than the block function's. It is still
the largest single part.

## The instructions

Zbb's encodings, so that a standard assembler and disassembler name
them:

| | encoding | does |
|---|---|---|
| `ror rd, rs1, rs2` | R-type, funct7 `0110000`, funct3 `101`, OP | rotate right by `rs2 & 31` |
| `rol rd, rs1, rs2` | R-type, funct7 `0110000`, funct3 `001`, OP | rotate left by `rs2 & 31` |
| `rori rd, rs1, n` | I-type, imm[11:5] `0110000`, funct3 `101`, OP-IMM | rotate right by `n` |

In picorv32 (`rtl/cpu/picorv32/picorv32.v`, parameter `ENABLE_ROT`):
decoded beside the shifts, `rori` taking its amount from the rs2 field
as `slli` does, and computed as `{x, x} >> n`, with `rol` by `n` done as
`ror` by `32 - n`. It needs `BARREL_SHIFTER`, which every Zeitlos
build has. Without `ENABLE_ROT` the three are illegal instructions, as
before.

## The software

`z_chacha20_rounds()` (`sw/common/zrng.c`) is ChaCha20's 20 rounds. On
its first call it asks `z_cpu_has_rot()` once, and from then on uses
one of two copies of the same round code:

- with `rori`, written as `.insn i 0x13, 5, rd, rs1, 0x600 | (32 - n)`.
  The rest of the program is still compiled for rv32im, so the
  compiler cannot emit a Zbb instruction anywhere else;
- with shifts.

`zrng`'s generator uses it. So does monocypher, when it is built with
`-DZ_CHACHA_ROUNDS=z_chacha20_rounds`: its `chacha20_rounds()` is then
this function (`sw/ext/monocypher/monocypher.c`, the one change made to
it). `net`, `netserve` and `web` are built that way.

## Tests

```
make -C rtl/cpu/picorv32/tests          # iverilog: ror/rol/rori vs the base ISA,
                                        #   and an illegal instruction without ENABLE_ROT
make -C sw/common/tests/sim_chacha test # RFC 8439's vectors, with and without
```

The first runs `rot.S` on picorv32 with `ENABLE_ROT`: every amount from
0 to 63 on 2,048 values, `ror` and `rol` each against the base ISA's
shifts and OR, then `rori` at each amount ChaCha20 uses. Without
`ENABLE_ROT` the first rotate must trap. `tools/run_tests.py --rtl`
runs it.

The second runs RFC 8439's block-function and encryption vectors on the
simulator's RV32 CPU, once as a CPU without the rotates and once as one
with them (`ZSIM_ROT=1`, which also sets FEATURES3 bit 3). Both must
give the vectors' answers.

## Not yet

- **The rotates in zeitlos32 and zeitlos32p**: their shared ALU and
  decoder (`rtl/cpu/zeitlos32/`).
- **SHA-256 in software** has rotates too (`sw/common/zsha256.c`, on a
  bitstream without the SHA-256 block). It could use the same
  `.insn`s.
- **Measured on hardware**: the LUT count from PnR, and an ssh `cat` of
  a big file with the instructions and without.
