#!/usr/bin/env python3
# Regenerates fobj_vectors.txt for test_fobj.c: objects made by
# fobj_ref.py (cryptography's Ed25519, hashlib's SHA-256), each rule
# broken on purpose, and random mutations -- with fobj_ref.py's verdicts.
#
#   python3 gen_fobj_vectors.py > fobj_vectors.txt
import random, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import fobj_ref as R
random.seed(20260927)
NOW = 1790505386
keys = [Ed25519PrivateKey.from_private_bytes(bytes(random.randrange(256) for _ in range(32))) for _ in range(4)]

def rnd_payload(fmt):
    n = random.choice([0, 1, 5, 40, 200, 1000])
    if fmt == b"bytes": return bytes(random.randrange(256) for _ in range(n))
    return "".join(random.choice("abc {}[]\":,é€🙂\n") for _ in range(n)).encode()[:n + 8]

valid = []
for i in range(60):
    fmt = random.choice([b"json", b"text", b"bytes"])
    kind = random.choice([b"log", b"state"])
    valid.append(R.make(random.choice(keys), random.choice([b"timeless/forum/general", b"fed/node/0123456789abcdef", b"a", b"x.y/z_w-1"]),
        random.choice([b"bbs.post", b"fed.nodes", b"t"]), fmt, kind, random.choice([0, 1, NOW, NOW + 86400, NOW + 86401, 2**53]),
        random.choice([1, 2, 1042, 2**53]), rnd_payload(fmt), random.choice([b"list", b"a/b", b"x" * 64])))
valid.append(R.make(keys[0], b"t" * 96, b"y" * 64, b"bytes", b"log", NOW, 1, bytes(16384)))

cases = []
for b in valid: cases.append((b, NOW))
# each rule, broken
def sub(b, old, new): return b.replace(old, new, 1)
base = R.make(keys[1], b"timeless/forum/general", b"bbs.post", b"text", b"log", NOW, 7, b"hello")
st = R.make(keys[1], b"timeless/nodes", b"fed.nodes", b"json", b"state", NOW, 8, b"{}", b"list")
broken = [sub(base, b"ZFED1", b"ZFED2"), sub(base, b"topic: ", b"topic:  "), sub(base, b"topic: ", b"Topic: "),
    sub(base, b"timeless/forum/general", b"timeless//general"), sub(base, b"timeless/forum/general", b"/timeless"),
    sub(base, b"timeless/forum/general", b"timeless/"), sub(base, b"timeless/forum/general", b"Timeless"),
    sub(base, b"timeless/forum/general", b"t" * 97), sub(base, b"bbs.post", b"bbs/post"), sub(base, b"bbs.post", b"y" * 65),
    sub(base, b"format: text", b"format: html"), sub(base, b"kind: log", b"kind: other"), sub(base, b"seq: 7", b"seq: 07"),
    sub(base, b"seq: 7", b"seq: 0"), sub(base, b"seq: 7", b"seq: -7"), sub(base, b"seq: 7", b"seq: %d" % (2**53 + 1)),
    sub(base, b"time: %d" % NOW, b"time: %d" % (NOW + 86401)), sub(base, b"len: 5", b"len: 6"), sub(base, b"len: 5", b"len: 4"),
    sub(base, b"len: 5", b"len: 16385"), sub(base, b"\nlen:", b"\r\nlen:"), sub(base, b"seq: 7\n", b"seq: 7 \n"),
    sub(base, b"kind: log\n", b"kind: log\nkey: x\n"), sub(st, b"key: list\n", b""), sub(st, b"key: list", b"key: A"),
    sub(base, b"type: bbs.post\n", b""), sub(base, b"type: bbs.post\nformat: text\n", b"format: text\ntype: bbs.post\n"),
    sub(base, b"len: 5\n", b"len: 5\nextra: 1\n"), sub(base, b"\nsig: ", b"\nSig: "), sub(base, b"\nsig: ", b"\nsig:  "),
    base[:-2], base + b"x", base[:-1] + b"\r\n", sub(base, b"hello", b"he\xffo\x80"), sub(base, b"hello", b"he\x00lo"),
    sub(base, b"hello", b"hellO")]
origin = base.split(b"origin: ")[1][:64]
broken.append(sub(base, origin, origin.upper()))
sigpos = base.rfind(b"sig: ") + 5
b2 = bytearray(base); b2[sigpos] = ord("A") if b2[sigpos] > ord("9") else b2[sigpos]; broken.append(bytes(b2))
b3 = bytearray(base); b3[sigpos + 3] ^= 1 if chr(b3[sigpos + 3]) in "0123456789abcdef" else 0
if bytes(b3) != base and chr(b3[sigpos + 3]) in "0123456789abcdef": broken.append(bytes(b3))
fmt_bytes = R.make(keys[2], b"t", b"t", b"bytes", b"log", NOW, 1, b"\xff\x00\x80")
broken.append(fmt_bytes)
long_hdr = R.make(keys[2], b"t" * 96, b"y" * 64, b"text", b"state", NOW, 1, b"", b"k" * 64)
broken.append(long_hdr)
for b in broken: cases.append((b, NOW))
cases.append((sub(base, b"time: %d" % NOW, b"time: %d" % (NOW + 10**6)), 0))	# no clock: accepted
for _ in range(400):
    b = bytearray(random.choice(valid[:30] + [base, st]))
    for _ in range(random.randrange(1, 3)):
        i = random.randrange(len(b)); r = random.random()
        if r < 0.5: b[i] = random.randrange(256)
        elif r < 0.75: del b[i]
        else: b.insert(i, random.choice(b"\n :0aA/\x00"))
    cases.append((bytes(b), NOW))

print(len(cases))
ok = good = 0
for b, now in cases:
    p = R.parse(b, now)
    if p is None:
        print("0 0 0 - %d %s" % (now, b.hex()))
    else:
        oid, size, org, sig = p
        v = R.verify(oid, org, sig)
        ok += 1; good += v
        print("1 %d %d %s %d %s" % (1 if v else 0, size, oid.hex(), now, b.hex()))
print("# %d cases: %d keep the rules, %d of them signed right" % (len(cases), ok, good), file=sys.stderr)
