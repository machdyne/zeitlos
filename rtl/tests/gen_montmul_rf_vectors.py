#!/usr/bin/env python3
# Zeitlos -- regenerates tests/montmul_rf_vectors.txt for tb_montmul_rf.v.
#
#   python3 gen_montmul_rf_vectors.py > montmul_rf_vectors.txt
#
# Random programs for montmul's register file: sixteen registers loaded
# with values below N (0, 1 and N-1 among them), eighty random MUL, ADD
# and SUB commands -- destinations that are also sources included --
# and every register's value afterwards, from this model of the block:
#
#   MUL  rd = ra * rb * R^-1 mod N,  R = 2^(32 * 12)
#   ADD  rd = ra + rb mod N
#   SUB  rd = ra - rb mod N
import random
random.seed(20260927)
P384 = 2**384 - 2**128 - 2**96 + 2**32 - 1
P256 = 2**256 - 2**224 + 2**192 + 2**96 - 1
N384 = int("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF"
           "581A0DB248B0A77AECEC196ACCC52973", 16)
P25519 = 2**255 - 19
NL = 12
R = 1 << (32 * NL)
def n0inv(N):
    x = 1
    for _ in range(5): x = (x * (2 - N * x)) & 0xFFFFFFFF
    return (-x) & 0xFFFFFFFF
def w(v): return " ".join("%08x" % ((v >> (32*i)) & 0xFFFFFFFF) for i in range(NL))
out = []
for N in (P384, P256, N384, P25519):
    Rinv = pow(R, -1, N)
    regs = [0, 1, N - 1, N - 2, (N + 1) // 2] + [random.randrange(N) for _ in range(11)]
    init = list(regs)
    cmds = []
    for _ in range(80):
        op = random.choice((1, 1, 2, 3))
        rd, ra, rb = random.randrange(16), random.randrange(16), random.randrange(16)
        a, b = regs[ra], regs[rb]
        if op == 1: r = a * b * Rinv % N
        elif op == 2: r = (a + b) % N
        else: r = (a - b) % N
        regs[rd] = r
        cmds.append((op << 12) | (rd << 8) | (ra << 4) | rb)
    out.append((N, init, cmds, regs))
print(len(out))
for N, init, cmds, regs in out:
    print("%08x" % n0inv(N)); print(w(N))
    for v in init: print(w(v))
    print(len(cmds)); print(" ".join("%04x" % c for c in cmds))
    for v in regs: print(w(v))
