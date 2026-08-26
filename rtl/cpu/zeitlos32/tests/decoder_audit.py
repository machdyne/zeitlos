#!/usr/bin/env python3
"""
Audit a RISC-V binary against zeitlos32's decoder.

zeitlos32 decodes funct7 STRICTLY and implements exactly six CSRs.
picorv32 is looser in places and ignores fields zeitlos32 checks. So a
binary that runs on picorv32 can contain instructions zeitlos32 calls
illegal -- and because sw/bios/boot_picorv32.S unmasks everything, an
illegal instruction does NOT light LED_R. It raises IRQ 1, the
kernel's handler sees an interrupt it has no case for, and execution
resumes with the instruction silently skipped. No message, no trap,
just wrong behaviour somewhere downstream.

This finds those instructions before they cost you a day.

  ./decoder_audit.py zeitlos.elf

Only instructions objdump itself decoded are checked, so data that
happens to live in an executable section is not reported.
  ./decoder_audit.py --raw kernel.bin 0x40000000

Anything it prints is either a gap in zeitlos32.v's is_legal or a
genuine instruction the core does not implement. Either way it needs a
decision, not a guess.
"""

import subprocess
import sys
import struct

OBJDUMP = "riscv64-unknown-elf-objdump"

# The six CSRs zeitlos32 implements (rtl/cpu/zeitlos32/zeitlos32.v).
# 0xc01 aliases 0xc00 because this SOC has no separate wall clock.
CSRS_OK = {0xc00, 0xc01, 0xc02, 0xc80, 0xc81, 0xc82}


def legal(w):
    """Mirror of is_legal in zeitlos32.v. Returns None if legal, else why."""
    op = w & 0x7f
    f3 = (w >> 12) & 7
    f7 = (w >> 25) & 0x7f
    rs1 = (w >> 15) & 0x1f
    csr = (w >> 20) & 0xfff

    if op == 0b0110111 or op == 0b0010111:          # lui / auipc
        return None
    if op == 0b1101111:                              # jal
        return None
    if op == 0b1100111:                              # jalr
        return None if f3 == 0 else "jalr with funct3=%d" % f3
    if op == 0b1100011:                              # branch
        return None if f3 not in (2, 3) else "branch with reserved funct3=%d" % f3
    if op == 0b0000011:                              # load
        return None if f3 in (0, 1, 2, 4, 5) else "load with funct3=%d" % f3
    if op == 0b0100011:                              # store
        return None if f3 in (0, 1, 2) else "store with funct3=%d" % f3
    if op == 0b0010011:                              # op-imm
        if f3 == 1:
            return None if f7 == 0 else "slli with funct7=%#x" % f7
        if f3 == 5:
            return None if f7 in (0x00, 0x20) else "srli/srai with funct7=%#x" % f7
        return None
    if op == 0b0110011:                              # op
        if f7 == 0x00:
            return None
        if f7 == 0x20:
            return None if f3 in (0, 5) else "sub/sra encoding with funct3=%d" % f3
        if f7 == 0x01:
            return None                              # RV32M
        return "op with funct7=%#x (not 0/0x20/0x01)" % f7
    if op == 0b0001111:                              # fence / fence.i
        return None
    if op == 0b1110011:                              # system
        if (w >> 21) & 0x7ff == 0 and (w >> 7) & 0x1fff == 0:
            return None                              # ecall / ebreak
        if f3 == 0b010 and rs1 == 0 and csr in CSRS_OK:
            return None                              # the counter reads
        if f3 == 0:
            return "privileged/system instruction (mret/wfi/sfence?)"
        return "CSR %#05x access (zeitlos32 has no CSR file)" % csr
    if op == 0b0001011:                              # picorv32 custom-0
        if f7 == 0x00 and f3 == 4: return None       # getq
        if f7 == 0x01 and f3 == 2: return None       # setq
        if f7 == 0x02 and f3 == 0: return None       # retirq
        if f7 == 0x03 and f3 == 6: return None       # maskirq
        if f7 == 0x04 and f3 == 4: return None       # waitirq
        return "custom-0 with funct7=%#x funct3=%d" % (f7, f3)
    if (w & 3) != 3:
        return "compressed instruction (zeitlos32 is not RVC)"
    return "unknown opcode %#04x" % op


def scan_words(words, base, note=lambda a: ""):
    bad = {}
    for i, w in enumerate(words):
        why = legal(w)
        if why:
            bad.setdefault(why, []).append((base + i * 4, w))
    return bad


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)

    if args[0] == "--raw":
        path, base = args[1], int(args[2], 0)
        data = open(path, "rb").read()
        words = struct.unpack("<%dI" % (len(data) // 4), data[:len(data) // 4 * 4])
        bad = scan_words(words, base)
        ctx = {}
    else:
        path = args[0]
        out = subprocess.run([OBJDUMP, "-d", path], capture_output=True, text=True).stdout
        bad, ctx = {}, {}
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            try:
                addr = int(parts[0].strip().rstrip(":"), 16)
                w = int(parts[1].strip(), 16)
            except ValueError:
                continue
            if len(parts[1].strip()) != 8:
                continue
            mnem = parts[2].strip()
            # Skip anything objdump itself could not decode. bios.lds
            # puts .rodata in the same CODE section as .text, so string
            # tables get disassembled as instructions -- every one of
            # them looks illegal, and none of them is ever executed.
            if mnem.startswith((".insn", ".word", ".byte", ".short", "unimp")):
                continue
            why = legal(w)
            if why:
                bad.setdefault(why, []).append((addr, w))
                ctx[addr] = mnem

    if not bad:
        print("clean -- every instruction decodes on zeitlos32")
        return 0

    total = sum(len(v) for v in bad.values())
    print("%d instruction(s) zeitlos32 would reject:\n" % total)
    for why, hits in sorted(bad.items(), key=lambda kv: -len(kv[1])):
        print("  %s  (%d)" % (why, len(hits)))
        for addr, w in hits[:6]:
            print("      %08x: %08x  %s" % (addr, w, ctx.get(addr, "")))
        if len(hits) > 6:
            print("      ... and %d more" % (len(hits) - 6))
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
