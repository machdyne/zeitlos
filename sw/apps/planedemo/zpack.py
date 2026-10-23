#!/usr/bin/env python3
#
# planedemo -- mkexec.py, then the ZEXE compressed (LZ4, as mkzar.py
# packs the core apps; every loader unpacks: docs/executables.md,
# "Compressed executables"). The film's 2.8 MB of pictures and samples
# are mostly zeros: about 1.1 MB on the card.
#
#   zpack.py IN.data OUT.bin BSS [STACK] [--driver]   (mkexec.py's arguments)

import os
import subprocess
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools")
sys.path.insert(0, TOOLS)
import mkzar  # noqa: E402

rc = subprocess.call([sys.executable, os.path.join(TOOLS, "mkexec.py")] + sys.argv[1:])
if rc:
    sys.exit(rc)
out = sys.argv[2]
data = open(out, "rb").read()
packed = mkzar.compress(data)
open(out, "wb").write(packed)
print("%s: %d bytes, %d packed" % (out, len(data), len(packed)))
