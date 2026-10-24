#!/usr/bin/env python3
#
# Every host test in the tree, one line each (docs/build.md, "Tests").
#
#   tools/run_tests.py              every host suite
#   tools/run_tests.py --rtl        and the RTL simulations (iverilog)
#   tools/run_tests.py net sheet    only suites whose name has one of these
#   tools/run_tests.py --list       what would run
#   make test                       the same as the first, from the top
#
# A suite is:
#
#   - every Makefile under sw/ with a `test:` target (sw/ext is other
#     projects' code, with their own tests);
#   - the test Makefiles no `make test` reaches (EXTRA below), and each
#     sw/common/tests/Makefile.<name>;
#   - the release tool's Python tests and tools/test_gen_abi.py.
#
# Each suite's output goes to output/tests/<name>.log. A suite that
# fails because a program or a Python module is not installed is
# reported as SKIP with its name, not FAIL -- these are host tests, and
# the host may lack dosfstools or Pillow -- unless --strict. Exit 1 if
# any suite failed.
#
import argparse
import os
import re
import subprocess
import sys
import time

sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, "output", "tests")

# Test Makefiles that no `make test` runs: (directory, target).
EXTRA = [
    ("sw/apps/backup/tests", "run"),
    ("sw/apps/update/tests", "run"),
    ("sw/apps/fs/tests", "run"),
    ("sw/apps/usb/tests", "default"),
    ("sw/apps/zlink/tests", "all"),     # about 40 s: the link runs in real time
]

# The release tool and the ABI generator: (script, directory to run in).
PYTHON = [
    ("release/lib/test_store.py", "."),
    ("release/lib/test_mkfatimg.py", "."),
    ("release/lib/selftest.py", "."),
    ("release/lib/test_incremental.py", "."),
    ("tools/test_gen_abi.py", "."),
]

# RTL simulations, with --rtl only: (directory, target).
RTL = [
    ("rtl/tests", "all"),
    ("rtl/cpu/zeitlos32/tests", "run"),
]

# What a missing program or module looks like in a log.
_MISSING = [
    re.compile(r"No module named '([\w.]+)'"),
    re.compile(r"(?:^|\s)([\w.+-]+): command not found"),
    re.compile(r"/bin/sh: \d+: ([\w.+-]+): not found"),
    re.compile(r"make(?:\[\d+\])?: ([\w.+-]+): No such file or directory"),
    re.compile(r"\b([\w.+-]+) failed \((dosfstools|mtools)\?\)"),
]
_SKIPPED = re.compile(r"\bskipp(?:ed|ing)\b", re.I)


class Suite:
    def __init__(self, name, cwd, cmd):
        self.name, self.cwd, self.cmd = name, cwd, cmd


def test_recipe(makefile):
    """The `test:` rule and its recipe, or None without one."""
    lines, rule = [], None
    with open(makefile, errors="replace") as f:
        for line in f:
            if rule is None:
                if line.startswith("test:"):
                    rule = [line]
            elif line.startswith("\t") or line.strip().startswith("#"):
                rule.append(line)
            else:
                break
    return "".join(rule) if rule is not None else None


def delegated(d):
    """A tests/ directory whose parent's `make test` runs it already."""
    parent = os.path.join(os.path.dirname(d), "Makefile")
    if os.path.basename(d) != "tests" or not os.path.exists(parent):
        return False
    r = test_recipe(parent)
    return bool(r and re.search(r"-C\s+tests\b", r))


def suites(rtl=False):
    out = []
    for dp, dn, fn in os.walk(os.path.join(ROOT, "sw")):
        rel = os.path.relpath(dp, ROOT)
        if rel == os.path.join("sw", "ext"):
            dn[:] = []
            continue
        dn.sort()
        if ("Makefile" in fn and test_recipe(os.path.join(dp, "Makefile"))
                and not delegated(dp)):
            out.append(Suite(rel, ROOT, ["make", "-C", rel, "test"]))
    for d, target in EXTRA:
        out.append(Suite(d, ROOT, ["make", "-C", d, target]))
    tests = os.path.join(ROOT, "sw", "common", "tests")
    for f in sorted(os.listdir(tests)):
        if not f.startswith("Makefile."):
            continue
        name = "sw/common/tests/" + f
        if f == "Makefile.zetta":    # run from sw/common, as it says
            out.append(Suite(name, os.path.join(ROOT, "sw", "common"),
                             ["make", "-f", "tests/" + f]))
        else:
            out.append(Suite(name, tests, ["make", "-f", f]))
    for script, cwd in PYTHON:
        out.append(Suite(script, os.path.join(ROOT, cwd),
                         [sys.executable, script]))
    if rtl:
        for d, target in RTL:
            out.append(Suite(d, ROOT, ["make", "-C", d, target]))
    return out


def missing(log):
    """The programs and modules a log says are not installed."""
    found = []
    for rx in _MISSING:
        for m in rx.finditer(log):
            name = m.group(1)
            if m.lastindex and m.lastindex > 1:
                name = m.group(2)
            if name not in found:
                found.append(name)
    return found


def run(s, timeout):
    log_name = re.sub(r"[^\w.-]+", "_", s.name) + ".log"
    log_path = os.path.join(LOGS, log_name)
    t0 = time.time()
    try:
        p = subprocess.run(s.cmd, cwd=s.cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        out, code = p.stdout.decode("utf-8", "replace"), p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace")
        out += "\n[run_tests: stopped after %d s]\n" % timeout
        code = None
    with open(log_path, "w") as f:
        f.write("$ (cd %s && %s)\n" % (os.path.relpath(s.cwd, ROOT),
                                       " ".join(s.cmd)))
        f.write(out)
    dt = time.time() - t0
    if code == 0:
        n = len(_SKIPPED.findall(out))
        need = missing(out)
        note = ""
        if need:
            note = "parts skipped: no %s" % ", ".join(need)
        elif n:
            note = "%d part(s) skipped" % n
        return "PASS", dt, note, log_path
    if code is None:
        return "FAIL", dt, "timed out", log_path
    need = missing(out)
    if need:
        return "SKIP", dt, "needs %s" % ", ".join(need), log_path
    return "FAIL", dt, "exit %d" % code, log_path


def main(argv):
    ap = argparse.ArgumentParser(description="Run every host test.")
    ap.add_argument("only", nargs="*",
                    help="run only suites whose name contains one of these")
    ap.add_argument("--rtl", action="store_true",
                    help="also run the RTL simulations (needs iverilog)")
    ap.add_argument("--list", action="store_true", help="list, do not run")
    ap.add_argument("--strict", action="store_true",
                    help="a suite skipped for a missing tool is a failure")
    ap.add_argument("--timeout", type=int, default=1200,
                    help="seconds per suite (default 1200)")
    args = ap.parse_args(argv)

    todo = [s for s in suites(args.rtl)
            if not args.only or any(o in s.name for o in args.only)]
    if args.list:
        for s in todo:
            print("%-44s %s" % (s.name, " ".join(s.cmd)))
        return 0
    if not todo:
        print("run_tests: no suite matches %s" % " ".join(args.only))
        return 1

    os.makedirs(LOGS, exist_ok=True)
    width = max(len(s.name) for s in todo)
    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    bad = []
    t0 = time.time()
    for s in todo:
        status, dt, note, log = run(s, args.timeout)
        counts[status] += 1
        if status == "FAIL" or (status == "SKIP" and args.strict):
            bad.append((s.name, log))
        print("%-4s  %-*s  %5.1fs  %s" % (status, width, s.name, dt, note),
              flush=True)

    print("\n%d passed, %d failed, %d skipped, in %.0f s; logs in %s"
          % (counts["PASS"], counts["FAIL"], counts["SKIP"],
             time.time() - t0, os.path.relpath(LOGS, ROOT)))
    for name, log in bad:
        print("  %s: %s" % (name, os.path.relpath(log, ROOT)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
