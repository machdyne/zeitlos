#!/usr/bin/env python3
"""Instructions retired per function, from the simulator's profile.

    ZSIM_PROF=prof.txt sim/zsim-headless -n 0 app.bin
    tools/simprof.py app.dasm prof.txt [n]

app.dasm is the objdump listing the app's build writes beside its .bin
(sw/common/app.mk). Functions the compiler inlined are counted in their
caller; mark one __attribute__((noinline)) to see it apart. sim/README.md.
"""
import bisect
import re
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 1
    dasm, prof = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    syms = []
    with open(dasm) as f:
        for line in f:
            m = re.match(r'^([0-9a-f]+) <([^>]+)>:', line)
            if m:
                syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    addrs = [a for a, _ in syms]
    tot, total = {}, 0
    with open(prof) as f:
        for line in f:
            a, c = line.split()
            a, c = int(a, 16), int(c)
            i = bisect.bisect_right(addrs, a) - 1
            name = syms[i][1] if i >= 0 else '?'
            tot[name] = tot.get(name, 0) + c
            total += c
    for name, c in sorted(tot.items(), key=lambda x: -x[1])[:top]:
        print(f"{c:12d} {100 * c / total:5.1f}%  {name}")
    print(f"{total:12d}        total")
    return 0


if __name__ == '__main__':
    sys.exit(main())
