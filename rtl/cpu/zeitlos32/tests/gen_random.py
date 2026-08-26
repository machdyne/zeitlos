#!/usr/bin/env python3
"""
Randomised differential test for zeitlos32.

Generates a long straight-line RV32IM program, executes it against a
golden model written here in Python, and emits an assembly file that
checks every architectural register against the model's answer.

Why this exists alongside the directed tests in prog/: a directed test
only finds bugs someone thought of. This finds the ones nobody did --
operand patterns, shift amounts and sign combinations that no hand
written test would bother with, in sequences long enough that a
register left stale by one instruction shows up in the next fifty.

What it deliberately does NOT do:

  - branches or jumps. Control flow is covered directly by
    prog/t_branch.S, and modelling it here would mean either
    generating loops that may not terminate or generating branches
    that are always taken, neither of which is worth the complexity.
  - x31 is never touched, so the epilogue has a base register to dump
    through.
  - x30 is a read-only scratch base pointer, so loads and stores have
    somewhere safe to go.

The random body is emitted as raw .word encodings rather than
mnemonics. That is on purpose: it pins the exact instruction bits the
core must decode, keeps the program counter exactly predictable (no
pseudo-instruction expanding to one word here and two words there),
and means the assembler cannot quietly "help".

Usage:
  ./gen_random.py --seed 1 --count 400 > prog/t_random1.S
"""

import argparse
import random

M32 = 0xffffffff

SCRATCH = 0x8000          # base for loads/stores, well clear of the code
DUMP = 0x9000             # where the epilogue writes the register file
BODY_ORG = 0x400          # fixed address of the random body

BASE_REG = 30             # holds SCRATCH, never written
DUMP_REG = 31             # untouched by the body, used by the epilogue


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def u32(v):
    return v & M32


# ---------------------------------------------------------------- encode

def r_type(f7, rs2, rs1, f3, rd, op):
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op


def i_type(imm, rs1, f3, rd, op):
    return ((imm & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op


def s_type(imm, rs2, rs1, f3, op):
    imm &= 0xfff
    return ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | \
           ((imm & 0x1f) << 7) | op


def u_type(imm20, rd, op):
    return ((imm20 & 0xfffff) << 12) | (rd << 7) | op


# ---------------------------------------------------------------- model

class Model:
    def __init__(self):
        self.x = [0] * 32
        self.mem = bytearray(1024)      # the scratch region only
        self.pc = BODY_ORG

    def rd(self, r):
        return 0 if r == 0 else self.x[r]

    def wr(self, r, v):
        if r != 0:
            self.x[r] = u32(v)

    def load(self, addr, size, signed):
        off = addr - SCRATCH
        assert 0 <= off < len(self.mem) - 4, "generator produced a bad address"
        v = int.from_bytes(self.mem[off:off + size], "little")
        if signed and v & (1 << (size * 8 - 1)):
            v -= 1 << (size * 8)
        return u32(v)

    def store(self, addr, size, val):
        off = addr - SCRATCH
        assert 0 <= off < len(self.mem) - 4, "generator produced a bad address"
        self.mem[off:off + size] = (val & ((1 << (size * 8)) - 1)).to_bytes(size, "little")


def alu_rr(f3, f7, a, b):
    if f7 == 0x01:                              # RV32M
        sa, sb = s32(a), s32(b)
        if f3 == 0:
            return u32(a * b)
        if f3 == 1:
            return u32((sa * sb) >> 32)
        if f3 == 2:
            return u32((sa * b) >> 32)
        if f3 == 3:
            return u32((a * b) >> 32)
        if f3 == 4:                             # div
            if b == 0:
                return M32
            if a == 0x80000000 and b == M32:
                return 0x80000000
            q = abs(sa) // abs(sb)
            return u32(-q if (sa < 0) != (sb < 0) else q)
        if f3 == 5:                             # divu
            return M32 if b == 0 else u32(a // b)
        if f3 == 6:                             # rem
            if b == 0:
                return u32(a)
            if a == 0x80000000 and b == M32:
                return 0
            r = abs(sa) % abs(sb)
            return u32(-r if sa < 0 else r)
        return u32(a) if b == 0 else u32(a % b)  # remu

    sub = (f7 == 0x20)
    if f3 == 0:
        return u32(a - b) if sub else u32(a + b)
    if f3 == 1:
        return u32(a << (b & 31))
    if f3 == 2:
        return 1 if s32(a) < s32(b) else 0
    if f3 == 3:
        return 1 if a < b else 0
    if f3 == 4:
        return a ^ b
    if f3 == 5:
        return u32(s32(a) >> (b & 31)) if sub else (a >> (b & 31))
    if f3 == 6:
        return a | b
    return a & b


# ---------------------------------------------------------------- generate

RR = [(0, 0x00), (0, 0x20), (1, 0x00), (2, 0x00), (3, 0x00),
      (4, 0x00), (5, 0x00), (5, 0x20), (6, 0x00), (7, 0x00)]
RM = [(f3, 0x01) for f3 in range(8)]
II = [0, 2, 3, 4, 6, 7]
LD = [(0, 1, True), (1, 2, True), (2, 4, True), (4, 1, False), (5, 2, False)]
ST = [(0, 1), (1, 2), (2, 4)]


def pick_rd(rnd):
    # never x0 (writes vanish and the check would be vacuous), never
    # the two harness registers
    return rnd.choice([r for r in range(1, 30)])


def pick_rs(rnd):
    return rnd.randrange(0, 30)


def generate(seed, count, use_m):
    rnd = random.Random(seed)
    m = Model()

    # seed the register file with values chosen to hit the interesting
    # cases far more often than uniform randomness would
    interesting = [0, 1, 2, 0xffffffff, 0x80000000, 0x7fffffff,
                   0xffff, 0x10000, 0xdeadbeef, 0xaaaaaaaa, 0x55555555,
                   31, 32, 33]
    init = {}
    for r in range(1, 30):
        init[r] = rnd.choice(interesting) if rnd.random() < 0.4 \
            else rnd.getrandbits(32)
        m.x[r] = init[r]
    m.x[BASE_REG] = SCRATCH

    body = []
    for _ in range(count):
        pc = m.pc
        kind = rnd.random()

        if kind < 0.34:                                     # OP
            f3, f7 = rnd.choice(RM if (use_m and rnd.random() < 0.35) else RR)
            rd, rs1, rs2 = pick_rd(rnd), pick_rs(rnd), pick_rs(rnd)
            word = r_type(f7, rs2, rs1, f3, rd, 0x33)
            m.wr(rd, alu_rr(f3, f7, m.rd(rs1), m.rd(rs2)))

        elif kind < 0.60:                                   # OP-IMM
            rd, rs1 = pick_rd(rnd), pick_rs(rnd)
            if rnd.random() < 0.3:                          # shift-immediate
                f3 = rnd.choice([1, 5])
                sh = rnd.randrange(0, 32)
                f7 = 0x20 if (f3 == 5 and rnd.random() < 0.5) else 0x00
                word = i_type((f7 << 5) | sh, rs1, f3, rd, 0x13)
                m.wr(rd, alu_rr(f3, f7, m.rd(rs1), sh))
            else:
                f3 = rnd.choice(II)
                imm = rnd.randrange(-2048, 2048)
                word = i_type(imm, rs1, f3, rd, 0x13)
                m.wr(rd, alu_rr(f3, 0x00, m.rd(rs1), u32(imm)))

        elif kind < 0.68:                                   # LUI
            rd, imm20 = pick_rd(rnd), rnd.getrandbits(20)
            word = u_type(imm20, rd, 0x37)
            m.wr(rd, u32(imm20 << 12))

        elif kind < 0.74:                                   # AUIPC
            rd, imm20 = pick_rd(rnd), rnd.getrandbits(20)
            word = u_type(imm20, rd, 0x17)
            m.wr(rd, u32(pc + (imm20 << 12)))

        elif kind < 0.87:                                   # LOAD
            f3, size, signed = rnd.choice(LD)
            rd = pick_rd(rnd)
            off = rnd.randrange(0, 256) & ~(size - 1)
            word = i_type(off, BASE_REG, f3, rd, 0x03)
            m.wr(rd, m.load(SCRATCH + off, size, signed))

        else:                                               # STORE
            f3, size = rnd.choice(ST)
            rs2 = pick_rs(rnd)
            off = rnd.randrange(0, 256) & ~(size - 1)
            word = s_type(off, rs2, BASE_REG, f3, 0x23)
            m.store(SCRATCH + off, size, m.rd(rs2))

        body.append(word)
        m.pc += 4

    return init, body, m


# ---------------------------------------------------------------- emit

def emit(seed, count, init, body, m, use_m):
    out = []
    a = out.append

    a("/*")
    a(" * zeitlos32 -- randomised differential test.")
    a(" *")
    a(" * GENERATED by tests/gen_random.py -- do not edit. Regenerate with:")
    a(f" *   ./gen_random.py --seed {seed} --count {count}"
      f"{'' if use_m else ' --no-m'} > this-file")
    a(" *")
    a(f" * {count} straight-line instructions against a golden model, then a")
    a(" * check of all 30 architectural registers the body may write, plus")
    a(" * the scratch memory it wrote through.")
    a(" *")
    a(" * A failing check id is the register number that disagreed (1-30) or")
    a(" * 100 + the word index for a memory mismatch. To see WHICH")
    a(" * instruction did it, disassemble: `make dis T=<this test>`.")
    a(" */")
    a("")
    a('#include "test.h"')
    a("")
    a(f"#define SCRATCH 0x{SCRATCH:08x}")
    a(f"#define DUMP    0x{DUMP:08x}")
    a("")
    a("\t.section .text")
    a("\t.globl _start")
    a("_start:")
    a("\tj\t_test_main")
    a("\tnop")
    a("\tnop")
    a("\tnop")
    a("\t.org 0x10")
    a("_irq_vec:")
    a("\tli\tt0, TEST_PORT")
    a("\tli\tt1, 9999")
    a("\tsw\tt1, 0(t0)")
    a(".Lirqhang:")
    a("\tj\t.Lirqhang")
    a("")
    a("\t.org 0x100")
    a("_test_main:")
    a("")
    a("\t/* clear the scratch region: the model starts it at zero */")
    a(f"\tli\tx{BASE_REG}, SCRATCH")
    a("\tli\tt0, 0")
    a(".Lclr:")
    a(f"\tadd\tt1, x{BASE_REG}, t0")
    a("\tsw\tx0, 0(t1)")
    a("\taddi\tt0, t0, 4")
    a("\tli\tt2, 512")
    a("\tblt\tt0, t2, .Lclr")
    a("")
    a("\t/* seed the register file */")
    for r in sorted(init):
        if r == BASE_REG:
            continue
        a(f"\tli\tx{r}, 0x{init[r]:08x}")
    a(f"\tli\tx{BASE_REG}, SCRATCH")
    a("")
    a("\t/* Jump over the padding .org leaves behind -- it is zero")
    a("\t * filled, and a word of zeroes is not a valid instruction. */")
    a("\tj\t.Lbody")
    a("")
    a("\t/* ---- the random body ---- */")
    a("\t/* At a fixed address so the model can predict auipc. */")
    a(f"\t.org 0x{BODY_ORG:x}")
    a(".Lbody:")
    for w in body:
        a(f"\t.word\t0x{w:08x}")
    a("")
    a("\t/* ---- dump and compare ---- */")
    a(f"\tli\tx{DUMP_REG}, DUMP")
    for r in range(1, 30):
        a(f"\tsw\tx{r}, {r * 4}(x{DUMP_REG})")
    a(f"\tsw\tx{BASE_REG}, {BASE_REG * 4}(x{DUMP_REG})")
    a("")
    for r in list(range(1, 30)) + [BASE_REG]:
        a(f"\tlw\ta0, {r * 4}(x{DUMP_REG})")
        a(f"\tCHECK_EQ {r}, a0, 0x{m.rd(r):08x}")
    a("")
    a("\t/* ---- and the memory the stores went to ---- */")
    a(f"\tli\tx{DUMP_REG}, SCRATCH")
    for i in range(64):
        want = int.from_bytes(m.mem[i * 4:i * 4 + 4], "little")
        a(f"\tlw\ta0, {i * 4}(x{DUMP_REG})")
        a(f"\tCHECK_EQ {100 + i}, a0, 0x{want:08x}")
    a("")
    a("\tTEST_PASS")
    return "\n".join(out) + "\n"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=400)
    p.add_argument("--no-m", action="store_true",
                   help="omit RV32M, for testing an ENABLE_MUL=0 build")
    args = p.parse_args()

    use_m = not args.no_m
    init, body, m = generate(args.seed, args.count, use_m)
    print(emit(args.seed, args.count, init, body, m, use_m), end="")


if __name__ == "__main__":
    main()
