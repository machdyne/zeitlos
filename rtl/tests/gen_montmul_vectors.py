#!/usr/bin/env python3
# Zeitlos -- regenerates tests/montmul_vectors.txt for tb_montmul.v.
#
#   python3 gen_montmul_vectors.py > montmul_vectors.txt
#
# Montgomery products R = A*B*2^-(32n) mod N, for the three moduli the
# block actually sees. Corner cases first: carry and borrow
# propagation is where the errors live and random values rarely reach
# the edges.
import random
random.seed(20260907)
P384 = 2**384 - 2**128 - 2**96 + 2**32 - 1
P256 = 2**256 - 2**224 + 2**192 + 2**96 - 1
N384 = int("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF"
           "581A0DB248B0A77AECEC196ACCC52973", 16)
def n0inv(N):
    x = 1
    for _ in range(5): x = (x * (2 - N * x)) & 0xFFFFFFFF
    return (-x) & 0xFFFFFFFF
# ALWAYS 12 words. The block's width is now a synthesis parameter, not
# a runtime register -- that removed every dynamic comparison against
# a limb count and was a large part of a 10x LUT reduction. A narrower
# modulus is simply zero-padded, which is the same integer with a
# larger R, and Montgomery only requires R > N and gcd(R,N) = 1.
cases = []
for N in (P384, P256, N384):
    nl = 12
    R = 1 << (32 * nl)
    Rinv = pow(R, -1, N)
    vals = [(N-1, N-1), (1, 1), (0, N-1), (N-1, 1), (R % N, R % N)]
    vals += [(random.randrange(N), random.randrange(N)) for _ in range(12)]
    for a, b in vals:
        cases.append((nl, n0inv(N), a, b, N, (a * b * Rinv) % N))
print(len(cases))
for nl, ni, a, b, N, r in cases:
    def w(v): return " ".join("%08x" % ((v >> (32*i)) & 0xFFFFFFFF)
                              for i in range(nl))
    print(nl, "%08x" % ni)
    print(w(a)); print(w(b)); print(w(N)); print(w(r))
