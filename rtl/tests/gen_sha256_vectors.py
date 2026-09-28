#!/usr/bin/env python3
# Zeitlos -- regenerates tests/sha256_vectors.txt for tb_sha256.v.
#
#   python3 gen_sha256_vectors.py > sha256_vectors.txt
#
# Whole messages, padded here into 64-byte blocks, and their digests
# from hashlib -- an implementation that shares nothing with the RTL.
# The empty message, "abc", the two-block NIST vector, and random
# lengths around every block boundary (55, 56, 63, 64, 65 ...).
import hashlib, random, struct
random.seed(20260927)
msgs = [b"", b"abc", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"]
for n in (1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 200, 1000):
    msgs.append(bytes(random.randrange(256) for _ in range(n)))
def pad(m):
    l = len(m) * 8
    m = m + b"\x80" + b"\x00" * ((55 - len(m)) % 64) + struct.pack(">Q", l)
    return m
print(len(msgs))
for m in msgs:
    p = pad(m)
    blocks = [p[i:i+64] for i in range(0, len(p), 64)]
    d = hashlib.sha256(m).digest()
    print(len(blocks))
    for blk in blocks:
        # as a little-endian CPU loads them: what the W register takes
        print(" ".join("%08x" % struct.unpack("<I", blk[i:i+4])[0] for i in range(0, 64, 4)))
    print(" ".join("%08x" % struct.unpack(">I", d[i:i+4])[0] for i in range(0, 32, 4)))
