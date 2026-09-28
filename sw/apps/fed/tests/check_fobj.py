#!/usr/bin/env python3
# The other direction: objects fobj.c made (/tmp/fobj_c_made.txt), checked
# by the independent reference (fobj_ref.py). Every one must keep the rules,
# have the id fobj.c reported, and carry a signature that checks.
import sys
import fobj_ref as R
bad = n = 0
for line in open("/tmp/fobj_c_made.txt"):
    idh, hx = line.split()
    b = bytes.fromhex(hx); n += 1
    p = R.parse(b, 0)
    if p is None or p[0].hex() != idh or p[1] != len(b) or not R.verify(p[0], p[2], p[3]):
        bad += 1
print("check_fobj.py: %d objects made by fobj.c, %d refused by the reference" % (n, bad))
sys.exit(1 if bad or n == 0 else 0)
