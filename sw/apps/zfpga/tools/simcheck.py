#!/usr/bin/env python3
#
# simcheck.py -- does a .config compute what the Verilog says?
#
#   simcheck.py --db ext/prjtrellis-db --device LFE5U-25F \
#       --package CABGA256 d.json d.lpf d.config [--cycles 2000]
#   simcheck.py ... d.zl - d.config
#
# The reference is yosys's netlist (with the .lpf for pins), or a .zl
# logical netlist, which names its own pins (give "-" for the .lpf).
#
# The functional test for Phases 3-5 without a board (docs/zfpga.md sec.
# 15.4). It shares no code with place.c or pnr.c:
#
#   1. EXTRACT the circuit from the .config alone: every LUT (its INIT
#      word, and which inputs are tied to 1), every flip-flop (its
#      settings), and every connection -- found by tracing each input pin
#      backwards through the configured arcs and the database's fixed
#      connections, resolved by the rules tools/resolvecheck.py proves
#      exact against nextpnr, until a LUT output, a flip-flop output or an
#      input pad is reached;
#   2. SIMULATE that circuit and yosys's netlist of the same design side
#      by side, cycle by cycle, from the same random inputs, and compare
#      every output port every cycle.
#
# Both are simulated by the same code with the same semantics, so a
# difference is a difference in the circuit, not in the model. The model
# is one clock domain: flip-flops load on each cycle's edge; an
# asynchronous reset also acts within the cycle.

import argparse, json, os, random, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolvecheck import Resolver, tile_loc, parse_db

ap = argparse.ArgumentParser()
ap.add_argument("--db", required=True)
ap.add_argument("--device", required=True)
ap.add_argument("--package", required=True)
ap.add_argument("--cycles", type=int, default=2000)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("json")
ap.add_argument("lpf")
ap.add_argument("config")
a = ap.parse_args()


def die(m):
    print("simcheck: " + m)
    sys.exit(1)


# -- a netlist both sides share: nets are opaque keys ----------------------
# luts:  (out, [in a..d], init)      input 'C1'/'C0' for constants
# ffs:   dict(d, q, clk, ce, lsr, cemux, lsrmux, regset, srmode)

class Circuit:
    def __init__(self):
        self.luts, self.ffs, self.inputs, self.outputs = [], [], {}, {}
        self.ccu2s = []     # dict(ins=[8], cin, cout, s0, s1, init0, init1, inj0, inj1)


def ccu2_half(init, inj, ins, cin, val):
    """yosys's CCU2C model (techlibs/ecp5/cells_sim.v), one half: a LUT4 on
    A-D and a LUT2 on A-B from the same INIT; with INJECT1=YES the carry
    in is ignored by the sum and the LUT2 does not generate."""
    a, b, c, d = (val.get(x, 0) for x in ins)
    l4 = (init >> (a | b << 1 | c << 2 | d << 3)) & 1
    l2 = (init >> (a | b << 1)) & 1
    g = 0 if inj == "YES" else 1
    return l4 ^ (cin & g), ((1 - l4) & l2 & g) | (l4 & cin)


def simulate(ck, rng_inputs, cycles):
    clks = {f["clk"] for f in ck.ffs}
    if len(clks) > 1:
        die("more than one clock: %s" % clks)
    clk = next(iter(clks)) if clks else None
    val = {"C0": 0, "C1": 1}
    for f in ck.ffs:
        val[f["q"]] = 1 if f["regset"] == "SET" else 0
    trace = []
    for cyc in range(cycles):
        for port, v in rng_inputs[cyc].items():
            if port in ck.inputs:
                val[ck.inputs[port]] = v
        for _ in range(len(ck.luts) + len(ck.ffs) + 2 * len(ck.ccu2s) + 2):     # settle
            changed = False
            for f in ck.ffs:                                  # async reset
                if f["srmode"] == "ASYNC" and f["lsr"] is not None and \
                        val.get(f["lsr"], 0) == (0 if f["lsrmux"] == "INV" else 1):
                    rv = 1 if f["regset"] == "SET" else 0
                    if val[f["q"]] != rv:
                        val[f["q"]] = rv; changed = True
            for out, ins, init in ck.luts:
                idx = sum(val.get(x, 0) << i for i, x in enumerate(ins))
                v = (init >> idx) & 1
                if val.get(out) != v:
                    val[out] = v; changed = True
            for cc in ck.ccu2s:
                s0, c0 = ccu2_half(cc["init0"], cc["inj0"], cc["ins"][:4], val.get(cc["cin"], 0), val)
                s1, c1 = ccu2_half(cc["init1"], cc["inj1"], cc["ins"][4:], c0, val)
                for k, v in ((cc["s0"], s0), (cc["s1"], s1), (cc["cout"], c1)):
                    if k is not None and val.get(k) != v:
                        val[k] = v; changed = True
            if not changed:
                break
        else:
            die("combinational loop")
        trace.append(tuple(val.get(ck.outputs[p], 0) for p in sorted(ck.outputs)))
        nxt = {}
        for f in ck.ffs:                                      # clock edge
            rv = 1 if f["regset"] == "SET" else 0
            en = 1 if f["cemux"] == "1" else val.get(f["ce"], 0) ^ (f["cemux"] == "INV")
            rst = f["lsr"] is not None and val.get(f["lsr"], 0) == (0 if f["lsrmux"] == "INV" else 1)
            nxt[f["q"]] = rv if rst else (val.get(f["d"], 0) if en else val[f["q"]])
        val.update(nxt)
    return trace, clk


# -- the reference: yosys's netlist ----------------------------------------

def func_init(f):
    """func= to INIT, independently of place.c: a b c d 0 1, ~ ! & ^ |,
    parentheses; & binds tighter than ^, which binds tighter than |."""
    def ev(v):
        pos = [0]
        def peek(): return f[pos[0]] if pos[0] < len(f) else ""
        def prim():
            ch = peek(); pos[0] += 1
            if ch in "~!": return 1 - prim()
            if ch == "(":
                r = orr()
                assert peek() == ")", "missing ) in " + f
                pos[0] += 1
                return r
            if ch in "abcd": return (v >> "abcd".index(ch)) & 1
            if ch in "01": return int(ch)
            raise SystemExit("simcheck: func=%s: bad '%s'" % (f, ch))
        def andd():
            r = prim()
            while peek() == "&": pos[0] += 1; r &= prim()
            return r
        def xor():
            r = andd()
            while peek() == "^": pos[0] += 1; r ^= andd()
            return r
        def orr():
            r = xor()
            while peek() == "|": pos[0] += 1; r |= xor()
            return r
        r = orr()
        assert pos[0] == len(f), "trailing text in func=" + f
        return r
    return sum(ev(v) << v for v in range(16))


def parse_zl(path):
    """A .zl as a Circuit, and its port -> pin map."""
    ck, pins = Circuit(), {}
    def zn(v):
        return "C1" if v == "1" else "C0" if v in ("0", "-") else "z" + v
    for t in (l.split() for l in open(path)):
        if not t or t[0].startswith("#"):
            continue
        o = dict(x.split("=", 1) for x in t[2:] if "=" in x)
        if t[0] in ("input", "output"):
            pins[t[1]] = t[2]
            o = dict(x.split("=", 1) for x in t[3:] if "=" in x)
            (ck.inputs if t[0] == "input" else ck.outputs)[t[1]] = zn(o["net"])
        elif t[0] == "lut":
            init = int(o["init"], 16) if "init" in o else func_init(o["func"])
            ck.luts.append((zn(o.get("z", "-")), [zn(o.get(k, "0")) for k in "abcd"], init))
        elif t[0] == "ccu2":
            nz = lambda k: None if o.get(k, "-") == "-" else zn(o[k])
            ck.ccu2s.append(dict(ins=[zn(o.get(k, "0")) for k in
                                      ("a0", "b0", "c0", "d0", "a1", "b1", "c1", "d1")],
                                 cin=zn(o.get("cin", "0")), cout=nz("cout"), s0=nz("s0"), s1=nz("s1"),
                                 init0=int(o["init0"], 16), init1=int(o["init1"], 16),
                                 inj0=o.get("inject0", "YES"), inj1=o.get("inject1", "YES")))
        elif t[0] == "ff":
            ce, lsr = o.get("ce", "1"), o.get("lsr", "0")
            ck.ffs.append(dict(d=zn(o["d"]), q=zn(o["q"]), clk=zn(o["clk"]),
                               ce=None if ce == "1" else zn(ce), lsr=None if lsr in ("0", "-") else zn(lsr),
                               cemux="1" if ce == "1" else "CE", lsrmux=o.get("lsrmux", "LSR"),
                               regset=o.get("regset", "RESET"), srmode=o.get("srmode", "LSR_OVER_CE")))
    return ck, pins


site = {}
if a.json.endswith(".zl"):
    ref, site = parse_zl(a.json)
    top = {"ports": {}, "cells": {}}
else:
    ref = Circuit()
    mod = json.load(open(a.json))["modules"]
    top = ([m for m in mod.values() if m.get("attributes", {}).get("top")] or list(mod.values()))[0]
bn = lambda b: "C1" if b == "1" else "C0" if b in ("0", "x", "z") else "n%d" % b
for pn, p in top["ports"].items():
    for i, b in enumerate(p["bits"]):
        name = pn if len(p["bits"]) == 1 else "%s[%d]" % (pn, i)
        (ref.inputs if p["direction"] == "input" else ref.outputs)[name] = bn(b)
for c in top["cells"].values():
    cn, pa = c["connections"], {k: str(v).rstrip() for k, v in c["parameters"].items()}
    g = lambda k: bn(cn[k][0]) if k in cn else None
    if c["type"] == "LUT4":
        ref.luts.append((g("Z"), [g(k) or "C0" for k in "ABCD"], int(pa["INIT"], 2)))
    elif c["type"] == "TRELLIS_FF":
        ce = g("CE")
        ref.ffs.append(dict(d=g("DI"), q=g("Q"), clk=g("CLK"), ce=ce, lsr=g("LSR") if g("LSR") not in (None, "C0") else None,
                            cemux="1" if ce in (None, "C1") else pa.get("CEMUX", "CE"),
                            lsrmux=pa.get("LSRMUX", "LSR"), regset=pa.get("REGSET", "RESET"),
                            srmode=pa.get("SRMODE", "LSR_OVER_CE")))
    elif c["type"] == "CCU2C":
        ref.ccu2s.append(dict(ins=[g(k) or "C0" for k in ("A0", "B0", "C0", "D0", "A1", "B1", "C1", "D1")],
                              cin=g("CIN") or "C0", cout=g("COUT"), s0=g("S0"), s1=g("S1"),
                              init0=int(pa["INIT0"], 2), init1=int(pa["INIT1"], 2),
                              inj0=pa.get("INJECT1_0", "YES"), inj1=pa.get("INJECT1_1", "YES")))
    else:
        die("reference cell type %s not modelled" % c["type"])

# -- the extraction: the circuit the .config builds -----------------------

dev = json.load(open(os.path.join(a.db, "devices.json")))["families"]["ECP5"]["devices"][a.device]
pre = {"12": "25K_", "25": "25K_", "45": "45K_", "85": "85K_"}[re.search(r"-(\d+)F", a.device).group(1)]
R = Resolver(dev["max_row"], dev["max_col"], pre)
grid = json.load(open(os.path.join(a.db, "ECP5", a.device, "tilegrid.json")))
iodb = json.load(open(os.path.join(a.db, "ECP5", a.device, "iodb.json")))["packages"][a.package]

tiles = defaultdict(lambda: {"arcs": [], "words": {}, "enums": {}})
cur = None
for line in open(a.config):
    t = line.split()
    if t[:1] == [".tile"]: cur = t[1]
    elif t[:1] == ["arc:"]: tiles[cur]["arcs"].append((t[1], t[2]))
    elif t[:1] == ["word:"]: tiles[cur]["words"][t[1]] = t[2]
    elif t[:1] == ["enum:"]: tiles[cur]["enums"][t[1]] = t[2]

arc_drv, fix_drv = {}, defaultdict(set)
for tn, v in tiles.items():
    r, c = tile_loc(tn)
    for snk, src in v["arcs"]:
        d, s = R(r, c, snk), R(r, c, src)
        if d in arc_drv and arc_drv[d] != s:
            die("%s driven by two arcs" % (d,))
        arc_drv[d] = s
tcache = {}
for tn, v in grid.items():
    ty = v["type"]
    if ty not in tcache:
        p = os.path.join(a.db, "ECP5", "tiledata", ty, "bits.db")
        tcache[ty] = parse_db(p)[1] if os.path.exists(p) else []
    r, c = tile_loc(tn)
    for snk, src in tcache[ty]:
        # CIB tiles' *_CIBTEST wires are Lattice test paths, never a real
        # driver; counting them makes every CIB input look doubly driven
        # and stops the trace (found tracing an input pad on sides.v).
        if src.endswith("_CIBTEST"):
            continue
        d, s = R(r, c, snk), R(r, c, src)
        if d and s:
            fix_drv[d].add(s)

SRC_RE = re.compile(r"^(F\d_SLICE|Q\d_SLICE|JPADDI[A-D]_PIO|FCO[ABC]?_SLICE)$")
FCI = ["FCI_SLICE", "FCIB_SLICE", "FCIC_SLICE", "FCID_SLICE"]
FCO = ["FCOA_SLICE", "FCOB_SLICE", "FCOC_SLICE", "FCO_SLICE"]


def trace(w):
    """Back from a sink wire to the source pin that drives it."""
    seen = set()
    while w is not None:
        if SRC_RE.match(w[2]):
            return "%d/%d/%s" % w
        if w in seen:
            die("routing loop at %s" % (w,))
        seen.add(w)
        if w in arc_drv:
            w = arc_drv[w]
        elif len(fix_drv.get(w, ())) == 1:
            w = next(iter(fix_drv[w]))
        else:
            return None
    return None


ext = Circuit()
for tn, v in (tiles.items() if not a.config.endswith(".zl") else []):
    if not tn.endswith(":PLC2"):
        continue
    r, c = tile_loc(tn)
    for s in range(4):
        S = "SLICE" + "ABCD"[s]
        ccu2 = v["enums"].get(S + ".MODE") == "CCU2"
        if ccu2:
            ins = []
            for lc in range(2):
                for P in "ABCD":
                    if v["enums"].get("%s.%s%dMUX" % (S, P, lc)) == "1":
                        ins.append("C1")
                    else:
                        ins.append(trace((r, c, "%s%d" % (P, 2 * s + lc))) or "C0")
            init = [int(v["words"].get("%s.K%d.INIT" % (S, lc), "1" * 16), 2) for lc in range(2)]
            # the carry in, followed back through the fixed chain to
            # whichever slice's carry out drives it; nothing is 0
            ext.ccu2s.append(dict(ins=ins, cin=trace((r, c, FCI[s])) or "C0",
                                  cout="%d/%d/%s" % (r, c, FCO[s]),
                                  s0="%d/%d/F%d_SLICE" % (r, c, 2 * s),
                                  s1="%d/%d/F%d_SLICE" % (r, c, 2 * s + 1),
                                  init0=init[0], init1=init[1],
                                  inj0=v["enums"].get(S + ".CCU2.INJECT1_0", "YES"),
                                  inj1=v["enums"].get(S + ".CCU2.INJECT1_1", "YES")))
        for lc in range(2):
            k = 2 * s + lc
            w = v["words"].get("%s.K%d.INIT" % (S, lc))
            if w is not None and v["enums"].get(S + ".MODE", "LOGIC") not in ("LOGIC", "CCU2"):
                die("%s %s: mode %s not modelled" % (tn, S, v["enums"][S + ".MODE"]))
            if w is not None and not ccu2:
                ins = []
                for P in "ABCD":
                    if v["enums"].get("%s.%s%dMUX" % (S, P, lc)) == "1":
                        ins.append("C1")
                    else:
                        ins.append(trace((r, c, "%s%d" % (P, k))) or "C0")
                ext.luts.append(("%d/%d/F%d_SLICE" % (r, c, k), ins, int(w, 2)))
            sd = v["enums"].get("%s.REG%d.SD" % (S, lc))
            if sd is None:
                continue
            d = "%d/%d/F%d_SLICE" % (r, c, k) if sd == "1" else trace((r, c, "M%d_SLICE" % k))
            muxclk = dict(v["arcs"]).get("MUXCLK%d" % s)
            muxlsr = dict(v["arcs"]).get("MUXLSR%d" % s)
            cemux = v["enums"].get(S + ".CEMUX", "1")
            lsr = trace((r, c, "LSR%d_SLICE" % s)) if muxlsr else None
            lw = muxlsr[-1] if muxlsr else "0"
            ext.ffs.append(dict(d=d, q="%d/%d/Q%d_SLICE" % (r, c, k),
                                clk=trace((r, c, "CLK%d_SLICE" % s)),
                                ce=trace((r, c, "CE%d_SLICE" % s)) if cemux != "1" else None,
                                lsr=lsr, cemux=cemux,
                                lsrmux=v["enums"].get("LSR%s.LSRMUX" % lw, "LSR"),
                                regset=v["enums"].get("%s.REG%d.REGSET" % (S, lc), "RESET"),
                                srmode=v["enums"].get("LSR%s.SRMODE" % lw, "LSR_OVER_CE")))

for stmt in (open(a.lpf).read().split(";") if a.lpf != "-" else []):
    m = re.match(r'\s*LOCATE\s+COMP\s+"([^"]+)"\s+SITE\s+"([^"]+)"', stmt)
    if m:
        site[m.group(1)] = m.group(2)
for port in (list(ref.inputs) + list(ref.outputs)) if not a.config.endswith(".zl") else []:
    q = iodb[site[port]]
    if port in ref.inputs:
        ext.inputs[port] = "%d/%d/JPADDI%s_PIO" % (q["row"], q["col"], q["pio"])
    else:
        ext.outputs[port] = trace((q["row"], q["col"], "PADDO%s_PIO" % q["pio"])) or "UNDRIVEN"

if a.config.endswith(".zl"):
    # netlist against netlist: synthesis checked against yosys, by port name
    ext, _ = parse_zl(a.config)
    missing = set(ref.inputs) ^ set(ext.inputs) | set(ref.outputs) ^ set(ext.outputs)
    if missing:
        die("ports differ: %s" % sorted(missing))

# -- compare --------------------------------------------------------------

random.seed(a.seed)
stim = [{p: random.randint(0, 1) for p in ref.inputs} for _ in range(a.cycles)]
# reset-like inputs asserted rarely, so state gets somewhere; a name
# ending in n (rst_n, resetn) is active low
for p in ref.inputs:
    if re.search(r"rst|reset", p, re.I):
        act = 0 if re.search(r"n$", p, re.I) else 1
        for cyc in stim:
            cyc[p] = act if random.random() < 0.02 else 1 - act
        if stim:
            stim[0][p] = act
t_ref, ck_ref = simulate(ref, stim, a.cycles)
t_ext, ck_ext = simulate(ext, stim, a.cycles)
# the clock port drives no logic in either model; it must be the same pin
outs = sorted(ref.outputs)
for cyc, (x, y) in enumerate(zip(t_ref, t_ext)):
    if x != y:
        bad = [outs[i] for i in range(len(outs)) if x[i] != y[i]]
        die("cycle %d: outputs %s differ (reference %s, extracted %s)" % (
            cyc, bad, [x[outs.index(b)] for b in bad], [y[outs.index(b)] for b in bad]))
toggles = sum(1 for i in range(1, len(t_ref)) if t_ref[i] != t_ref[i - 1])
print("%s: %d LUTs, %d carry slices, %d flip-flops extracted; %d cycles, outputs changed on %d; EQUIVALENT"
      % (os.path.basename(a.config), len(ext.luts), len(ext.ccu2s), len(ext.ffs), a.cycles, toggles))
