#!/usr/bin/env python3
# Two folders of the same pictures (render-chunky's software and T2
# runs): how many pixels differ in each. Fails past `limit` in any.
import os, sys
a, b = sys.argv[1], sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 400
def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    return parts[3]
bad = 0
for n in sorted(os.listdir(a)):
    if not n.endswith('.ppm'): continue
    x, y = load(os.path.join(a, n)), load(os.path.join(b, n))
    diff = sum(1 for i in range(0, len(x), 3) if x[i:i+3] != y[i:i+3])
    print('ppm_diff: %-28s %6d pixels differ' % (n, diff))
    if diff > limit: bad += 1
print('ppm_diff: %d over %d' % (bad, limit))
sys.exit(1 if bad else 0)
