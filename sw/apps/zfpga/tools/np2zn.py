#!/usr/bin/env python3
#
# np2zn.py -- a nextpnr-ecp5 placed-and-routed design, as a zfpga .zn
#
#   nextpnr-ecp5 ... --textcfg d.config --write d.json
#   np2zn.py d.json d.config -o d.zn
#
# Host-only, for tests/run.sh. The point of the exercise (docs/zfpga.md
# sec. 12.5) is to check `zfpga pnr` against nextpnr, so what this takes
# from nextpnr's .config is limited to PHYSICAL facts -- things a router
# decides and a .zn states:
#
#   - every arc, verbatim;
#   - which LUT input pins are used: those with an arc into them;
#   - which tile wire a flip-flop's clock and reset arrive on: the source
#     of its slice's MUXCLKn / MUXLSRn arc;
#   - each LUT's INIT in physical pin order, because nextpnr permutes it
#     for the pins its router chose and that permutation is a router's
#     business (Phase 4), not a translation.
#
# Everything else -- modes, carry injection, unused-pin muxes, every
# flip-flop setting and the clock/reset comparisons, IO types in two
# tiles, tristate ties, bank voltages, the baseline -- comes from the
# netlist and has to be worked out by zfpga pnr, and that is what the
# test checks.

import argparse
import json
import os
import re
import sys


def die(msg):
    sys.stderr.write("np2zn: " + msg + "\n")
    sys.exit(1)


def read_config(path):
    device, part, tiles = None, None, {}
    cur = None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == ".device":
            device = t[1]
        elif t[0] == ".comment" and len(t) > 2 and t[1] == "Part:":
            part = t[2]
        elif t[0] == ".tile":
            cur = tiles.setdefault(t[1], {"arcs": [], "words": {}})
        elif t[0] == "arc:":
            cur["arcs"].append((t[1], t[2]))
        elif t[0] == "word:":
            cur["words"][t[1]] = t[2]
    return device, part, tiles


def val(v):
    """A nextpnr JSON parameter/attribute value as nextpnr's
    str_or_default() would see it: strings lose yosys's trailing
    space; 32-digit bit strings are integers."""
    v = str(v)
    if re.fullmatch(r"[01]{32}", v):
        return str(int(v, 2))
    return v.rstrip(" ")


def net_of(cell, port):
    bits = cell.get("connections", {}).get(port)
    if not bits or not isinstance(bits[0], int):
        return None             # unconnected, or a constant
    return "n%d" % bits[0]


def bel_xy(bel):
    m = re.fullmatch(r"X(\d+)/Y(\d+)/(.*)", bel)
    if not m:
        die("bad bel %s" % bel)
    return int(m.group(1)), int(m.group(2)), m.group(3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("config")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--route", metavar="DB",
                    help="Phase 4: declare nets by terminal instead of giving arcs "
                         "(needs the prjtrellis-db root, for the resolver)")
    a = ap.parse_args()

    device, part, tiles = read_config(a.config)
    if not device or not part or not part.startswith(device + "-"):
        die("config has no usable .device / Part comment")
    rest = part[len(device) + 1:]
    speed, package = rest[0], rest[1:]

    mod = json.load(open(a.json))["modules"]
    top = [m for m in mod.values() if m.get("attributes", {}).get("top")] or list(mod.values())
    cells = top[0]["cells"]

    out = ["# converted by tools/np2zn.py from %s and %s" % (a.json, a.config),
           "device %s" % device, "package %s" % package, "speed %s" % speed, ""]

    # -- Phase 4: nets by their terminals ------------------------------
    # A net's configurable pips, as global wires; its source is the one
    # wire they leave from that none of them drives, its sinks the wires
    # they reach that none of them leaves. The clock buffer is dropped
    # and its two nets merged: router v1 does not use global wires.
    terminals, dcc_merge = {}, {}
    if a.route:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from resolvecheck import Resolver, tile_loc
        dev = json.load(open(os.path.join(a.route, "devices.json")))["families"]["ECP5"]["devices"][device]
        pre = {"12": "25K_", "25": "25K_", "45": "45K_", "85": "85K_"}[re.search(r"-(\d+)F", device).group(1)]
        R = Resolver(dev["max_row"], dev["max_col"], pre)
        isglob = lambda w: w[2].startswith(("G_", "L_", "R_"))
        perm = lambda p: (re.fullmatch(r"[ABCD]\d", p[0][2]) and
                          re.fullmatch(r"[ABCD]\d_SLICE", p[1][2]) and p[0][:2] == p[1][:2])
        netpips = {}
        for nn, v in top[0]["netnames"].items():
            if not v.get("bits") or not isinstance(v["bits"][0], int):
                continue
            rt = v.get("attributes", {}).get("ROUTING", "").split(";")
            pips = set()
            for i in range(0, len(rt) - 2, 3):
                pm = re.fullmatch(r"X(\d+)/Y(\d+)/(-?\d+)_(-?\d+)_(.*)->(-?\d+)_(-?\d+)_(.*)", rt[i + 1] or "")
                if not pm:
                    continue
                x, y = int(pm.group(1)), int(pm.group(2))
                p = ((y + int(pm.group(4)), x + int(pm.group(3)), pm.group(5)),
                     (y + int(pm.group(7)), x + int(pm.group(6)), pm.group(8)))
                if not perm(p):
                    pips.add(p)
            if pips:
                netpips["n%d" % v["bits"][0]] = pips

        def ends(pips):
            return ({p[0] for p in pips} - {p[1] for p in pips},
                    {p[1] for p in pips} - {p[0] for p in pips})

        for n, pips in netpips.items():
            terminals[n] = ends(pips)
        for c in cells.values():
            if c["type"] == "DCCA":
                i, o = net_of(c, "CLKI"), net_of(c, "CLKO")
                dcc_merge[o] = i
        for o, i in dcc_merge.items():
            # the input net's source; the output net's sinks; no globals
            si, _ = ends(netpips.pop(i, set()))
            _, ko = ends({p for p in netpips.pop(o, set()) if not (isglob(p[0]) or isglob(p[1]))})
            terminals.pop(i, None)
            terminals[o] = (si, ko)
        for n, (srcs, snks) in terminals.items():
            if len(srcs) != 1:
                die("net %s has %d sources after merging: %s" % (n, len(srcs), sorted(srcs)))
            for w in srcs | snks:
                if w[2].startswith(("G_", "L_", "R_")):
                    die("net %s has a global terminal %s; router v1 has no globals" % (n, w))

    def arcs_in(tile):
        return tiles.get(tile, {"arcs": []})["arcs"]

    for name in sorted(cells):
        c = cells[name]
        ty = c["type"]
        p = {k: val(v) for k, v in c.get("parameters", {}).items()}
        at = {k: val(v) for k, v in c.get("attributes", {}).items()}
        bel = at.get("NEXTPNR_BEL")
        if not bel:
            die("cell %s is not placed" % name)
        x, y, b = bel_xy(bel)
        site = "R%dC%d" % (y, x)
        tile = site + ":PLC2"

        if ty == "TRELLIS_COMB":
            m = re.fullmatch(r"SLICE([A-D])\.K([01])", b)
            s, lc = "ABCD".index(m.group(1)), int(m.group(2))
            mode = p.get("MODE", "LOGIC")
            if mode == "RAMW_BLOCK":
                continue
            word = tiles.get(tile, {"words": {}})["words"].get("SLICE%s.K%d.INIT" % ("ABCD"[s], lc))
            if word is None:
                die("%s: no INIT word in %s" % (name, tile))
            n = s * 2 + lc
            sinks = {snk for snk, _ in arcs_in(tile)}
            pins = "".join(P for P in "ABCD" if "%s%d" % (P, n) in sinks) or "-"
            line = "comb %s SLICE%s %d mode=%s init=0x%04X pins=%s" % (
                site, "ABCD"[s], lc, mode, int(word, 2), pins)
            if mode == "CCU2":
                line += " inject=%s" % p.get("CCU2_INJECT1", "YES")
            out.append(line)

        elif ty == "TRELLIS_FF":
            m = re.fullmatch(r"SLICE([A-D])\.FF([01])", b)
            s, lc = "ABCD".index(m.group(1)), int(m.group(2))
            src = dict(arcs_in(tile))

            def wire(port, mux, base):
                net = net_of(c, port)
                if not net:
                    return "-"
                if a.route:
                    return "?@" + net
                w = src.get("%s%d" % (mux, s))
                if w not in (base + "0", base + "1"):
                    die("%s: %s net has no %s%d arc in %s" % (name, port, mux, s, tile))
                return "%s@%s" % (w, net)

            line = "ff %s SLICE%s %d clk=%s lsr=%s" % (
                site, "ABCD"[s], lc, wire("CLK", "MUXCLK", "CLK"), wire("LSR", "MUXLSR", "LSR"))
            for k in ("GSR", "SD", "REGSET", "LSRMODE", "CEMUX", "CLKMUX", "SRMODE", "LSRMUX"):
                if k in p:
                    line += " %s=%s" % (k.lower(), p[k])
            out.append(line)

        elif ty == "TRELLIS_IO":
            if not re.fullmatch(r"PIO[A-D]", b):
                die("bad IO bel %s" % bel)
            line = "io R%dC%d.%s dir=%s type=%s" % (y, x, b[3], p.get("DIR", "INPUT"),
                                                  at.get("IO_TYPE", "LVCMOS33"))
            for k in ("HYSTERESIS", "SLEWRATE", "PULLMODE", "CLAMP", "DRIVE", "OPENDRAIN"):
                if k in at:
                    line += " %s=%s" % (k.lower(), at[k])
            for k in ("DATAMUX_ODDR", "DATAMUX_OREG", "DATAMUX_MDDR", "TRIMUX_TSREG"):
                if k in p:
                    line += " %s=%s" % (k.lower(), p[k])
            for k in ("DIFFRESISTOR", "TERMINATION"):
                if k in at:
                    die("%s: %s is not a Phase 3 IO option" % (name, k))
            if net_of(c, "T") or net_of(c, "IOLTO"):
                line += " tristate"
            out.append(line)

        elif ty == "DCCA":
            if net_of(c, "CE"):
                die("%s: DCCA with a clock enable is not Phase 3" % name)
            if not a.route:
                out.append("dcc %s %s" % (site, b))

        else:
            die("%s: cell type %s is not a Phase 3 cell" % (name, ty))

    out.append("")
    if a.route:
        ws = lambda w: "R%dC%d/%s" % w
        for n in sorted(terminals):
            srcs, snks = terminals[n]
            snks = sorted(snks)
            out.append("net %s %s %s" % (n, ws(next(iter(srcs))), " ".join(ws(w) for w in snks[:16])))
            for i in range(16, len(snks), 16):
                out.append("sinks %s %s" % (n, " ".join(ws(w) for w in snks[i:i + 16])))
    else:
        for t in sorted(tiles):
            for snk, src in tiles[t]["arcs"]:
                out.append("arc %s %s %s" % (t, snk, src))
    out.append("")
    open(a.output, "w").write("\n".join(out))


if __name__ == "__main__":
    main()
