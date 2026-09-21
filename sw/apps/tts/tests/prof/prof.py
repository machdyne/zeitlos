#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# Instructions and memory accesses per audio sample, for synth.c as
# the target runs it: compiled for rv32im and executed in an emulator
# (pip install unicorn), with a driver rendering 40 frames of a voiced
# vowel and 40 of a fricative.
#
#     cd sw/apps/tts && make prof
#
# On the board an instruction costs ~16 cycles, so instructions per
# sample x 11025 x 16 / 48MHz is roughly the share of the CPU speech
# takes. That model reproduced the first measurement on hardware (202
# instructions: 74% estimated, ~80% measured), which is what makes it
# worth running before sending a change to a board.

import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TTS = os.path.abspath(os.path.join(HERE, "..", ".."))
CROSS = os.environ.get("CROSS", "riscv-none-elf-")

try:
    from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_CODE, \
        UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_INTR
    from unicorn.riscv_const import UC_RISCV_REG_SP
except ImportError:
    sys.exit("prof: needs `pip install unicorn`")


def run(nv, nf, opt):
    elf, binf = "/tmp/synth_prof.elf", "/tmp/synth_prof.bin"
    subprocess.check_call([CROSS + "gcc", "-std=gnu99", opt, "-march=rv32im", "-mabi=ilp32",
                           "-nostdlib", "-ffreestanding", "-DNV=%d" % nv, "-DNF=%d" % nf,
                           "-I" + TTS, "-I" + os.path.join(TTS, "..", "..", "common"),
                           "-T", os.path.join(HERE, "link.ld"), "-o", elf,
                           os.path.join(HERE, "drv.c"), os.path.join(TTS, "synth.c"), "-lgcc"],
                          stderr=subprocess.DEVNULL)
    subprocess.check_call([CROSS + "objcopy", "-O", "binary", elf, binf])
    code = open(binf, "rb").read()
    mu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    mu.mem_map(0x10000, 0x100000)
    mu.mem_write(0x10000, code)
    mu.mem_map(0x800000, 0x10000)
    mu.reg_write(UC_RISCV_REG_SP, 0x80fff0)
    c = [0, 0, 0]

    def ins(u, a, s, d): c[0] += 1
    def rd(u, t, a, s, v, d): c[1] += 1
    def wr(u, t, a, s, v, d): c[2] += 1
    mu.hook_add(UC_HOOK_CODE, ins)
    mu.hook_add(UC_HOOK_MEM_READ, rd)
    mu.hook_add(UC_HOOK_MEM_WRITE, wr)
    mu.hook_add(UC_HOOK_INTR, lambda u, i, d: u.emu_stop())
    try:
        mu.emu_start(0x10000, 0x10000 + len(code), count=50_000_000)
    except Exception:
        pass
    n = (nv + nf) * 55
    return c[0] / n, c[1] / n, c[2] / n


if __name__ == "__main__":
    for label, nv, nf in (("vowel", 40, 0), ("fricative", 0, 40)):
        i, r, w = run(nv, nf, "-O2")
        print("%-9s  %5.0f instructions/sample  %3.0f loads  %3.0f stores  ~%2.0f%% of the CPU"
              % (label, i, r, w, i * 11025 * 16 / 48e6 * 100))
