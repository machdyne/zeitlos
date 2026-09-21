#!/usr/bin/env python3
#
# resolvecheck.py -- Phase 4, step 1: the relative-name resolver, and
# the proof that it is right.
#
#   resolvecheck.py --db ext/prjtrellis-db --device LFE5U-25F \
#       d_routed.json d.config [more pairs...]
#
# Trellis names wires relative to the tile that lists them. This turns
# a (tile, relative name) into a global wire exactly as libtrellis's
# RoutingGraph::globalise_net_ecp5() does (docs/zfpga.md sec. 13):
#
#   - a tile's location is the R<row>C<col> in its NAME;
#   - "25K_", "45K_", "85K_" prefixes apply only to that die;
#   - at column 69 and beyond, PCSA in a name is read as PCSB;
#   - G_, L_, R_ are globals: G_ ones (other than VPTX, HPBX, HPRX) at
#     (0,0), the rest at the tile's own location;
#   - otherwise an optional [NS]n then an optional [EW]n, then '_',
#     offset the location: N is up (row -), S down, W left, E right;
#   - a result outside the array is not a wire.
#
# The proof: nextpnr's routed JSON records, per net, every pip it used,
# as global wires. Three checks, all of which must hold exactly:
#
#   1. every arc in nextpnr's .config (bar the baseline) resolves to a
#      pip nextpnr used;
#   2. every configurable pip nextpnr used is some resolved arc of the
#      database -- so the database's names, resolved, span nextpnr's;
#   3. every FIXED pip nextpnr used is a resolved fixed connection,
#      except LUT permutation pseudo-pips (A1 -> B1_SLICE), which are
#      nextpnr's own invention and are counted separately.

import argparse
import json
import os
import re
import sys
from collections import defaultdict

LOCAL_RE = re.compile(r"^([NS]\d+)?([EW]\d+)?_(.*)$")


class Resolver:
    def __init__(self, max_row, max_col, prefix):
        self.max_row, self.max_col, self.prefix = max_row, max_col, prefix

    def __call__(self, row, col, name):
        if name[:4] in ("25K_", "45K_", "85K_"):
            if name[:4] != self.prefix:
                return None
            name = name[4:]
        if col >= 69:
            i = name.find("PCSA")
            if i >= 0:
                name = name[:i + 3] + "B" + name[i + 4:]
        if name.startswith(("G_", "L_", "R_")):
            if name.startswith("G_") and not any(k in name for k in ("VPTX", "HPBX", "HPRX")):
                return (0, 0, name)
            return (row, col, name)
        m = LOCAL_RE.match(name)
        if m:
            for g in (m.group(1), m.group(2)):
                if not g:
                    continue
                n = int(g[1:])
                if g[0] == "N": row -= n
                elif g[0] == "S": row += n
                elif g[0] == "W": col -= n
                else: col += n
            name = m.group(3)
        if row < 0 or row > self.max_row or col < 0 or col > self.max_col:
            return None
        return (row, col, name)


def tile_loc(tname):
    m = re.search(r"R(\d+)C(\d+)", tname)
    return int(m.group(1)), int(m.group(2))


def parse_db(path):
    muxes, fixed = defaultdict(set), []
    kind = None
    for line in open(path):
        t = [x for x in line.split()]
        t = t[:next((i for i, x in enumerate(t) if x.startswith("#")), len(t))]
        if not t:
            continue
        if t[0].startswith("."):
            kind = t[0]
            if kind == ".mux":
                sink = t[1]
            elif kind == ".fixed_conn":
                fixed.append((t[1], t[2]))
            continue
        if kind == ".mux":
            muxes[sink].add(t[0])
    return muxes, fixed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True)
    ap.add_argument("--device", required=True)
    ap.add_argument("--base", help="baseline .config: its arcs are exempt from check 1")
    ap.add_argument("pairs", nargs="+", help="routed.json config pairs")
    a = ap.parse_args()
    if len(a.pairs) % 2:
        sys.exit("give routed.json and .config in pairs")

    dev = json.load(open(os.path.join(a.db, "devices.json")))["families"]["ECP5"]["devices"][a.device]
    grid = json.load(open(os.path.join(a.db, "ECP5", a.device, "tilegrid.json")))
    prefix = {"12": "25K_", "25": "25K_", "45": "45K_", "85": "85K_"}[re.search(r"-(\d+)F", a.device).group(1)]
    R = Resolver(dev["max_row"], dev["max_col"], prefix)

    types = {}
    for t in set(v["type"] for v in grid.values()):
        p = os.path.join(a.db, "ECP5", "tiledata", t, "bits.db")
        types[t] = parse_db(p) if os.path.exists(p) else (defaultdict(set), [])

    # Every configurable arc and fixed connection on the die, resolved.
    arcs, fixed = set(), set()
    for tname, v in grid.items():
        r, c = tile_loc(tname)
        muxes, fx = types[v["type"]]
        for sink, srcs in muxes.items():
            gs = R(r, c, sink)
            if gs is None:
                continue
            for s in srcs:
                gsrc = R(r, c, s)
                if gsrc is not None:
                    arcs.add((gsrc, gs))
        for sink, src in fx:
            gs, gsrc = R(r, c, sink), R(r, c, src)
            if gs is not None and gsrc is not None:
                fixed.add((gsrc, gs))
    print("database, resolved: %d configurable arcs, %d fixed connections" % (len(arcs), len(fixed)))

    base = set()
    if a.base:
        cur = None
        for line in open(a.base):
            t = line.split()
            if t[:1] == [".tile"]: cur = t[1]
            elif t[:1] == ["arc:"]: base.add((cur, t[1], t[2]))

    bad = 0
    for jpath, cpath in zip(a.pairs[0::2], a.pairs[1::2]):
        # nextpnr's pips as global (src, dst)
        used = set()
        mod = json.load(open(jpath))["modules"]
        for m in mod.values():
            for net in m.get("netnames", {}).values():
                rt = net.get("attributes", {}).get("ROUTING", "").split(";")
                for i in range(0, len(rt) - 2, 3):
                    pip = rt[i + 1]
                    if not pip:
                        continue
                    pm = re.fullmatch(r"X(\d+)/Y(\d+)/(-?\d+)_(-?\d+)_(.*)->(-?\d+)_(-?\d+)_(.*)", pip)
                    x, y = int(pm.group(1)), int(pm.group(2))
                    src = (y + int(pm.group(4)), x + int(pm.group(3)), pm.group(5))
                    dst = (y + int(pm.group(7)), x + int(pm.group(6)), pm.group(8))
                    used.add((src, dst))

        cfg, cur = [], None
        for line in open(cpath):
            t = line.split()
            if t[:1] == [".tile"]: cur = t[1]
            elif t[:1] == ["arc:"] and (cur, t[1], t[2]) not in base:
                cfg.append((cur, t[1], t[2]))

        n1 = n2 = n3 = perm = 0
        for tname, snk, src in cfg:                             # check 1
            r, c = tile_loc(tname)
            g = (R(r, c, src), R(r, c, snk))
            if g in used:
                n1 += 1
            else:
                bad += 1
                print("  1 FAIL %s: arc %s <- %s resolves to %s, not a pip nextpnr used" % (tname, snk, src, g))
        for p in used:                                          # checks 2, 3
            if p in arcs:
                n2 += 1
            elif p in fixed:
                n3 += 1
            elif re.fullmatch(r"[ABCD]\d", p[0][2]) and re.fullmatch(r"[ABCD]\d_SLICE", p[1][2]) \
                    and p[0][:2] == p[1][:2]:
                perm += 1
            else:
                bad += 1
                print("  2/3 FAIL: nextpnr pip %s -> %s is no resolved arc or fixed connection" % p)
        print("%s: %d config arcs all nextpnr's pips; %d of nextpnr's pips are database arcs, "
              "%d fixed connections, %d LUT-permutation pseudo-pips"
              % (os.path.basename(cpath), n1, n2, n3, perm))

    print("FAILED: %d" % bad if bad else "resolver: exact on every arc and every pip")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
