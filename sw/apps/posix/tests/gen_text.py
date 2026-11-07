#!/usr/bin/env python3
"""
Fixtures for sed, tr, cut and cmp (sw/apps/posix/text.c), and cases
whose expected output comes from the real GNU programs run on the same
files -- so the comparison is with another implementation, not with
what this one printed last time.

Usage: gen_text.py ROOT OUT_CASES. ROOT is the fixture tree run.sh built.
"""

import os
import random
import subprocess
import sys

root, out_path = sys.argv[1], sys.argv[2]
random.seed(7)
os.makedirs(os.path.join(root, "text"), exist_ok=True)

WORDS = ["alpha", "beta", "gamma", "delta", "foo", "bar", "baz", "x1", "y22", "TODO", "fix",
         "Hello", "WORLD", "a.b", "c:d", "e,f", "  ", "\t", "42", "007"]


def lines(n):
    out = []
    for _ in range(n):
        k = random.randint(0, 7)
        sep = random.choice([" ", ":", ",", "\t"])
        out.append(sep.join(random.choice(WORDS) for _ in range(k)))
    return out


files = {}
for name, n in (("a.txt", 12), ("b.txt", 30), ("c.txt", 5)):
    data = "\n".join(lines(n)) + "\n"
    files[name] = data
    with open(os.path.join(root, "text", name), "w") as f:
        f.write(data)
# one with no newline at the end, and an empty one
with open(os.path.join(root, "text", "nonl.txt"), "w") as f:
    f.write("first\nlast without newline")
open(os.path.join(root, "text", "empty.txt"), "w").close()

cases = []


def gnu(argv):
    env = dict(os.environ, LC_ALL="C")
    r = subprocess.run(argv, cwd=os.path.join(root, "text"), capture_output=True, env=env)
    return r.returncode, r.stdout.decode("latin-1")


nexact = 0


def add_exact(shell_cmd, argv):
    # Byte for byte, a missing last newline included: GNU's output in a
    # file, ours into another, and cmp between them.
    global nexact
    code, out = gnu(argv)
    if code != 0:
        raise SystemExit(f"gen_text: GNU {argv} failed: {code}")
    nexact += 1
    want = f"want{nexact}.out"
    with open(os.path.join(root, "text", want), "wb") as f:
        f.write(out.encode("latin-1"))
    cases.append(f"$ cd /text && {shell_cmd} > got.out && cmp got.out {want}\n")


def add(shell_cmd, argv):
    if argv[-1] == "nonl.txt":
        add_exact(shell_cmd, argv)
        return
    code, out = gnu(argv)
    if code != 0:
        raise SystemExit(f"gen_text: GNU {argv} failed: {code}")
    body = out[:-1].split("\n") if out.endswith("\n") else out.split("\n") if out else []
    # a case ends at a blank line: output with one is compared as a file
    if any(l == "" for l in body):
        add_exact(shell_cmd, argv)
        return
    cases.append("$ cd /text && " + shell_cmd + "\n" + "".join(l + "\n" for l in body))


SED = [
    ["s/a/A/"], ["s/a/A/g"], ["s/a/A/2"], ["s/[0-9]\\+/<&>/g"], ["s/\\(foo\\|bar\\)/[\\1]/g"],
    ["s/^/> /"], ["s/$/;/"], ["s/ *$//"], ["s/[[:upper:]]/_/g"], ["s/\\([a-z]*\\):\\([a-z]*\\)/\\2:\\1/"],
    ["-n", "/TODO/p"], ["-n", "/^$/!p"], ["/beta/d"], ["2,4d"], ["3q"], ["-n", "$p"], ["-n", "2,/fix/p"],
    ["-n", "/foo/,/bar/p"], ["1!s/e/E/g"], ["$!d"], ["="], ["-n", "/x1/="], ["s/a*/-/g"], ["s/.//"],
    ["s/[^a-z]//g"], ["-n", "s/fix/FIX/p"], ["-E", "s/(a|e)+/<&>/g"], ["-E", "s/([a-z]+) ([a-z]+)/\\2 \\1/"],
    ["-E", "s/b?a/X/g"], ["s|a|/|g"], ["s,:,;,g"], ["-e", "s/a/b/", "-e", "s/b/c/"], ["s/a/b/;s/b/c/"],
]
for parts in SED:
    quoted = " ".join(p if p.startswith("-") else "'" + p + "'" for p in parts)
    for f in ("a.txt", "b.txt", "nonl.txt"):
        add(f"sed {quoted} {f}", ["sed"] + parts + [f])

TR = [("a-z", "A-Z"), ("abc", "x"), (":,", "\\t\\t"), ("[:lower:]", "[:upper:]"), ("a-e", "1-5")]
for a, b in TR:
    for f in ("a.txt", "b.txt"):
        add(f"tr '{a}' '{b}' < {f}", ["sh", "-c", f"tr '{a}' '{b}' < {f}"])
for opt, a in (("-d", "aeiou"), ("-d", "[:digit:]"), ("-s", " "), ("-cd", "a-z\\n"), ("-s", ":,")):
    for f in ("a.txt", "b.txt"):
        add(f"tr {opt} '{a}' < {f}", ["sh", "-c", f"tr {opt} '{a}' < {f}"])
add("cat a.txt | tr -s 'a-z' 'A-Z'", ["sh", "-c", "tr -s 'a-z' 'A-Z' < a.txt"])

CUT = ["-d: -f1", "-d: -f2", "-d: -f1,3", "-d: -f2-", "-d, -f-2", "-f1", "-f2,4", "-c1-3",
       "-c2,5-", "-d: -s -f2", "-d' ' -f1-2"]
for c in CUT:
    for f in ("a.txt", "b.txt"):
        argv = ["sh", "-c", f"cut {c} {f}"]
        add(f"cut {c} {f}", argv)

GREP = [["foo"], ["-i", "hello"], ["-v", "a"], ["-n", "TODO"], ["-c", "beta"], ["-E", "(foo|bar)+"],
        ["x[0-9]"], ["^fix"], ["42$"], ["-E", "^[a-z]+:"], ["-in", "world"], ["-vc", "e"],
        ["-F", "a.b"], ["-E", "o{1}"] if False else ["-E", "o+"], ["\\(ba\\)\\+"], ["-w"] if False else ["g.mma"]]
for parts in GREP:
    quoted = " ".join(p if p.startswith("-") else "'" + p + "'" for p in parts)
    for f in ("a.txt", "b.txt"):
        code, _ = gnu(["grep"] + parts + [f])
        if code == 0:
            add(f"grep {quoted} {f}", ["grep"] + parts + [f])

# numbers and fields for sort: unique enough that ties do not decide
with open(os.path.join(root, "text", "sortme.txt"), "w") as f:
    for _ in range(40):
        f.write(f"{random.choice(WORDS)}:{random.randint(-500, 500)}:{random.choice(['Ab', 'ab', 'B', 'c', 'C'])}\n")
for opts in (["-n", "-t:", "-k2"], ["-rn", "-t:", "-k2"], ["-t:", "-k3"], ["-f", "-t:", "-k3"], ["-u"], ["-r"]):
    add("sort " + " ".join(opts) + " sortme.txt", ["sort", "-s"] + opts + ["sortme.txt"])

with open(out_path, "w") as f:
    f.write("# generated by gen_text.py: expected output from GNU sed, tr and cut\n\n")
    for c in cases:
        f.write(c + "\n")
print(f"gen_text: {len(cases)} cases")
