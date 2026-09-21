#!/usr/bin/env python3
# rtldeps.py FILE RTLFILES... -- FILE, and every file defining a module it
# names, transitively: what both yosys and zfpga must be given to
# synthesise FILE's module (tests/rtlcheck.sh).
import re, sys
f, files = sys.argv[1], sys.argv[2:]
defs = {}
for g in files:
    for m in re.findall(r"^\s*module\s+(\w+)", open(g).read(), re.M):
        defs.setdefault(m, g)
need, todo = [f], [f]
while todo:
    # block comments across lines, then line comments within them: one
    # pattern with re.S let `//.*` swallow the rest of the file
    text = re.sub(r"/\*.*?\*/", "", open(todo.pop()).read(), flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    for m, g in defs.items():
        if g not in need and re.search(r"\b%s\b" % m, text):
            need.append(g)
            todo.append(g)
print(" ".join(need))
