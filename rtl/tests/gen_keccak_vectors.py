#!/usr/bin/env python3
# Regenerates keccak_vectors.txt for tb_keccak.v: Keccak-f[1600] on
# random states, by a reference written here from FIPS 202 -- which
# first has to reproduce hashlib's SHA3-256, SHA3-512, SHAKE128 and
# SHAKE256 on random messages, or this script stops.
#
#   python3 gen_keccak_vectors.py > keccak_vectors.txt
#
# Each line: the 50 words of a state (word k = bits 32k+31..32k, lane i
# = words 2i and 2i+1), then the 50 words after the permutation, in hex.
import hashlib, random, sys
random.seed(20260927)
RC = [0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
      0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
      0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
      0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
      0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
      0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008]
# rotation offsets r[x][y], FIPS 202 Table 2, as computed by its rule
R = [[0] * 5 for _ in range(5)]
x, y = 1, 0
for t in range(24):
    R[x][y] = ((t + 1) * (t + 2) // 2) % 64
    x, y = y, (2 * x + 3 * y) % 5
M = (1 << 64) - 1
def rotl(v, n): return ((v << n) | (v >> (64 - n))) & M if n else v
def f1600(A):                                   # A[x + 5y]
    A = list(A)
    for rnd in range(24):
        C = [A[x] ^ A[x+5] ^ A[x+10] ^ A[x+15] ^ A[x+20] for x in range(5)]
        D = [C[(x-1) % 5] ^ rotl(C[(x+1) % 5], 1) for x in range(5)]
        B = [0] * 25
        for x in range(5):
            for y in range(5):
                B[y + 5 * ((2*x + 3*y) % 5)] = rotl(A[x + 5*y] ^ D[x], R[x][y])
        A = [B[x + 5*y] ^ (~B[(x+1) % 5 + 5*y] & B[(x+2) % 5 + 5*y]) for y in range(5) for x in range(5)]
        A = [A[i] for i in range(25)]
        # the list comprehension above ran y outer, x inner: index x + 5y
        A[0] ^= RC[rnd]
    return A
def sponge(msg, rate, pad, outlen):
    A = [0] * 25
    m = bytearray(msg) + bytes([pad])
    while len(m) % rate: m.append(0)
    m[-1] |= 0x80
    for off in range(0, len(m), rate):
        for i in range(rate // 8):
            A[i] ^= int.from_bytes(m[off + 8*i: off + 8*i + 8], "little")
        A = f1600(A)
    out = b""
    while True:
        out += b"".join(A[i].to_bytes(8, "little") for i in range(rate // 8))
        if len(out) >= outlen: return out[:outlen]
        A = f1600(A)
for n in [0, 1, 71, 72, 135, 136, 137, 167, 168, 169, 300, 1000]:
    msg = bytes(random.randrange(256) for _ in range(n))
    assert sponge(msg, 136, 0x06, 32) == hashlib.sha3_256(msg).digest(), "SHA3-256"
    assert sponge(msg, 72, 0x06, 64) == hashlib.sha3_512(msg).digest(), "SHA3-512"
    assert sponge(msg, 168, 0x1F, 500) == hashlib.shake_128(msg).digest(500), "SHAKE128"
    assert sponge(msg, 136, 0x1F, 300) == hashlib.shake_256(msg).digest(300), "SHAKE256"
print("# the reference reproduces hashlib's SHA3-256/512 and SHAKE128/256", file=sys.stderr)
def words(A): return " ".join("%08x" % ((A[i // 2] >> (32 * (i % 2))) & 0xFFFFFFFF) for i in range(50))
states = [[0] * 25, [M] * 25, [1] + [0] * 24] + [[random.getrandbits(64) for _ in range(25)] for _ in range(21)]
for s in states:
    print(words(s), words(f1600(s)))
