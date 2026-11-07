#!/usr/bin/env python3
"""rx.c (sed's regular expressions) against Python's re, on random
extended expressions over a small alphabet. Exit 1 on any difference.

  python3 tests/rx_fuzz.py ./rx_fuzz [N]
"""
import random, re, subprocess, sys

random.seed(1)
A = "abc"

def atom(d):
    r = random.random()
    if d < 3 and r < 0.15: return "(" + alt(d + 1) + ")"
    if r < 0.25: return "."
    if r < 0.35: return "[" + random.choice(["ab", "^a", "a-b", "[:alpha:]", "^bc"]) + "]"
    return random.choice(A)

def piece(d):
    a = atom(d)
    q = random.random()
    if q < 0.2: a += "*"
    elif q < 0.3: a += "+"
    elif q < 0.4: a += "?"
    return a

def branch(d):
    s = "".join(piece(d) for _ in range(random.randint(1, 4)))
    if random.random() < 0.1: s = "^" + s
    if random.random() < 0.1: s = s + "$"
    return s

def alt(d):
    s = branch(d)
    while random.random() < 0.2: s += "|" + branch(d)
    return s

def pyre(p):
    return p.replace("[:alpha:]", "a-zA-Z")

n = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
cases = []
for _ in range(n):
    p = alt(0)
    s = "".join(random.choice(A + "d") for _ in range(random.randint(0, 10)))
    cases.append((p, s))
out = subprocess.run([sys.argv[1]], input="".join(f"{p}\t{s}\n" for p, s in cases),
                     capture_output=True, text=True).stdout.splitlines()
bad = 0
for (p, s), got in zip(cases, out):
    m = re.search(pyre(p), s)
    want = "none" if not m else " ".join(f"{m.start(g)},{m.end(g)}" for g in range(m.re.groups + 1)) + " "
    if got in ("error: expression too long", "error: more than nine groups", "toolong",
               "error: too many * and +", "error: too many [...] in one expression"):
        continue        # more than rx takes: refused, and said so -- as it should be
    if got != want:
        bad += 1
        if bad <= 10: print(f"DIFF {p!r} on {s!r}: rx {got!r}, re {want!r}")
print(f"rx_fuzz: {len(cases)} cases, {bad} differ")
sys.exit(1 if bad else 0)
