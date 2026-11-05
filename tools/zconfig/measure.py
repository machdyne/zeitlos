#!/usr/bin/env python3
#
# tools/zconfig/measure.py -- what a target really costs, by synthesis.
#
#   measure.py <target> [--modules] [--yosys yosys] [--nextpnr nextpnr-ecp5]
#   measure.py --spec path/to/custom.spec ...
#
# Default: synthesises the target as a release build does (synth_ecp5
# -abc9, flattened) and runs nextpnr-ecp5's packer on it, which is
# where TRELLIS_COMB -- the figure the LUT budget is in -- is counted.
# Prints the totals against the device. 10-25 minutes for a whole SoC.
#
# --modules: synthesises with -noflatten and prints each of sysctl's
# blocks (its instances) with an estimate of its LUTs (LUT4 + 2 a carry
# cell + 2 a distributed RAM cell) and block RAMs. Blocks are not
# optimised across their edges in this mode, so the sum is higher than
# a release build's by roughly 10-20%; the numbers are for comparing
# blocks, and for the catalog (tools/zconfig/catalog.cfg). glyph_mem
# is mapped to LUT RAM in this mode and to block RAM in a real build.
#
# ECP5 only. The include file a target needs (rtl/zspec.vh) is written
# into a directory of its own, so two measurements can run at once.
#
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))
import spec as zspec   # noqa: E402
import gen as zgen     # noqa: E402

# device sizes: LUT4 (= TRELLIS_COMB slots) and DP16KD
# (the 12F is the 25F's die, with its resources)
DEVICES = {
    "12k": (24288, 56), "25k": (24288, 56), "45k": (43848, 108),
    "85k": (83640, 208),
}


def make_value(target, var):
    common = ["BOARD=" + target.board, "EXTRA_DEFINES=-DZSPEC"] + target.make_vars
    out = subprocess.run(
        ["make", "-s", "--no-print-directory", "-C", ROOT] + common +
        ["--eval", "zc-print: ; @echo $(%s)" % var, "zc-print"],
        capture_output=True, text=True)
    lines = out.stdout.strip().splitlines()
    return lines[-1].strip() if lines else ""


def load(args):
    if args.spec:
        # a spec outside release/targets: copy it in under a scratch name
        name = "_zconfig_measure_%d" % os.getpid()
        dst = os.path.join(ROOT, "release", "targets", name + ".spec")
        shutil.copy(args.spec, dst)
        try:
            return zspec.load_target(ROOT, name)
        finally:
            os.remove(dst)
    return zspec.load_target(ROOT, args.target)


def prerequisites(target):
    """The hex files synthesis reads ($readmemh), built if missing."""
    want = [w for w in make_value(target, "SOC_SYNTH_INPUTS").split()
            if w.endswith(".hex")]
    missing = [w for w in want if not os.path.exists(os.path.join(ROOT, w))]
    if missing:
        subprocess.run(["make", "-s", "-C", ROOT, "BOARD=" + target.board,
                        "EXTRA_DEFINES=-DZSPEC"] + target.make_vars + missing,
                       check=True)


def comb_of(cells):
    return (cells.get("LUT4", 0) + 2 * cells.get("CCU2C", 0) +
            2 * cells.get("TRELLIS_DPR16X4", 0))


def parse_modules(stat_json):
    """yosys `stat -json`: {module: {cell type: n}}."""
    with open(stat_json) as f:
        data = json.load(f)
    mods = {}
    for name, m in data.get("modules", {}).items():
        mods[name.lstrip("\\")] = m.get("num_cells_by_type", {})
    return mods


def instances(netlist_json):
    """sysctl's instances and every module's submodule instances."""
    with open(netlist_json) as f:
        data = json.load(f)
    def real(name):
        m = data["modules"].get(name)
        if m is None:
            return False
        a = m.get("attributes", {})
        return not ("blackbox" in a or "whitebox" in a or "lib_whitebox" in a)
    tree = {}
    for mname, m in data["modules"].items():
        subs = []
        for cname, c in m.get("cells", {}).items():
            if real(c["type"]):
                subs.append((cname, c["type"]))
        tree[mname] = subs
    return tree


def total(mod, tree, mods, memo):
    if mod in memo:
        return memo[mod]
    c = dict(mods.get(mod, {}))
    for _, sub in tree.get(mod, []):
        for k, v in total(sub, tree, mods, memo).items():
            c[k] = c.get(k, 0) + v
    memo[mod] = c
    return c


def modules_table(work):
    mods = parse_modules(os.path.join(work, "stat.json"))
    tree = instances(os.path.join(work, "soc.json"))
    memo = {}
    rows = []
    for inst, typ in tree.get("sysctl", []):
        c = total(typ, tree, mods, memo)
        rows.append((comb_of(c), c.get("DP16KD", 0), c.get("MULT18X18D", 0),
             inst, re.sub(r"^\$paramod\$?[0-9a-f]*\\?", "", typ)))
    own = mods.get("sysctl", {})
    rows.append((comb_of(own), own.get("DP16KD", 0), own.get("MULT18X18D", 0),
             "(sysctl itself)", "glue, decode, muxes"))
    rows.sort(reverse=True)
    print("%-28s %-28s %7s %5s %4s" % ("instance", "module", "LUT~", "BRAM", "DSP"))
    for comb, bram, dsp, inst, typ in rows:
        print("%-28s %-28s %7d %5d %4d" % (inst, typ[:28], comb, bram, dsp))
    allc = total("sysctl", tree, mods, memo)
    print("%-57s %7d %5d %4d" % ("all (not optimised across blocks)",
                     comb_of(allc), allc.get("DP16KD", 0),
                     allc.get("MULT18X18D", 0)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", nargs="?")
    ap.add_argument("--spec")
    ap.add_argument("--modules", action="store_true")
    ap.add_argument("--yosys", default=os.environ.get("YOSYS", "yosys"))
    ap.add_argument("--nextpnr", default=os.environ.get("NEXTPNR", "nextpnr-ecp5"))
    ap.add_argument("--from", dest="from_dir",
                    help="a --keep directory of an earlier --modules run: its table again")
    ap.add_argument("--keep", help="a directory (inside the tree) to leave the netlist and logs in")
    args = ap.parse_args()
    if args.from_dir:
        modules_table(args.from_dir)
        return
    if not args.target and not args.spec:
        ap.error("a target, or --spec")

    t = load(args)
    if t.family != "ecp5":
        sys.exit("measure.py: ECP5 only (%s is %s)" % (t.name, t.family))
    if os.path.exists(os.path.join(ROOT, "rtl", "zspec.vh")):
        sys.exit("measure.py: rtl/zspec.vh is in the tree (a release build "
                 "running, or one left behind): it would be read instead of "
                 "this target's")
    prerequisites(t)
    device = make_value(t, "DEVICE") or "25k"
    package = make_value(t, "PACKAGE")
    files = make_value(t, "RTL_PICO").split()

    # under the tree: YoWASP's yosys sees only the directory it runs in
    if args.keep:
        work = os.path.abspath(args.keep)
    else:
        os.makedirs(os.path.join(ROOT, "output"), exist_ok=True)
        work = tempfile.mkdtemp(prefix="zconfig-", dir=os.path.join(ROOT, "output"))
    os.makedirs(work, exist_ok=True)
    inc = os.path.join(work, "inc")
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, "zspec.vh"), "w") as f:
        f.write(zgen.zspec_vh(t, "measure", "measure"))

    board_uc = "BOARD_" + t.board.upper()
    # the Makefile's YOSYS_PRE (the DDR3 boards): elaborate, write the
    # design out and read it back, so yosys does not re-elaborate sysctl
    # when a late blackbox appears (the Makefile explains)
    pre = ""
    if make_value(t, "YOSYS_PRE"):
        il = os.path.join(work, "elab.il")
        pre = ("hierarchy -top sysctl; write_rtlil %s; design -reset; read_rtlil %s; "
               % (il, il))
    synth = pre + "synth_ecp5 -abc9 -top sysctl"
    if args.modules:
        synth += " -noflatten"
    script = ("read -define ZSPEC %s ECP5; read -incdir %s; read_verilog %s; "
              "%s; delete t:$print t:$scopeinfo; proc; tee -q -o %s/stat.json stat -json; "
              "write_json %s/soc.json"
              % (board_uc, inc, " ".join(files), synth, work, work))
    print("measure: %s (%s, %s), synthesising -- this takes a while" %
          (t.name, t.board, device), file=sys.stderr)
    r = subprocess.run([args.yosys, "-q", "-l", os.path.join(work, "synth.log"),
                        "-p", script], cwd=ROOT)
    if r.returncode:
        sys.exit("measure: yosys failed; see %s/synth.log" % work)

    if args.modules:
        modules_table(work)
        return


    log = os.path.join(work, "pack.log")
    cmd = [args.nextpnr, "--" + device, "--json", os.path.join(work, "soc.json"),
           "--pack-only", "-l", log, "--ignore-loops"]
    if t.lpf:
        # the board's pins: the DDR3 PHY's DQSBUFM will not pack without them
        cmd += ["--lpf", os.path.join(ROOT, "boards", t.lpf), "--lpf-allow-unconstrained"]
    if package:
        cmd += ["--package", package]
    subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    got = {}
    try:
        with open(log) as f:
            for line in f:
                m = re.search(r"\s(TRELLIS_COMB|TRELLIS_FF|DP16KD|MULT18X18D):\s+(\d+)/\s*(\d+)", line)
                if m:
                    got[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    except OSError:
        pass
    if "TRELLIS_COMB" not in got:
        sys.exit("measure: the packer did not report; see %s" % log)
    for k in ("TRELLIS_COMB", "TRELLIS_FF", "DP16KD", "MULT18X18D"):
        if k in got:
            n, of = got[k]
            print("%-13s %6d / %6d  %3d%%" % (k, n, of, 100 * n // max(of, 1)))


if __name__ == "__main__":
    main()
