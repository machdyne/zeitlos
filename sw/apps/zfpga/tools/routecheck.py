#!/usr/bin/env python3
#
# routecheck.py -- is a routed .config legal for the nets a .zn declares?
#
#   routecheck.py --db ext/prjtrellis-db --device LFE5U-25F \
#       --base ext/nextpnr-base/lfe5u-25f.config design.zn design.config
#
# Independent of route.c by construction: it shares only the resolver,
# which tools/resolvecheck.py proves exact against nextpnr. A router's
# output cannot be compared with nextpnr's byte for byte -- the routes
# differ -- so what is checked is what makes any routing correct:
#
#   1. no wire is driven by two different arcs (that would be a short);
#   2. no net's source is driven by an arc (its cell drives it);
#   3. following the arcs -- and the database's fixed connections, which
#      a router crosses without writing anything -- from each net's
#      source reaches every sink;
#   4. no wire DRIVEN BY AN ARC is reached by two nets. (Fixed
#      connections fan out into pins nobody uses, harmlessly; wires the
#      config drives are the ones two nets could fight over.)

import argparse, json, os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolvecheck import Resolver, tile_loc, parse_db

ap = argparse.ArgumentParser()
ap.add_argument("--db", required=True)
ap.add_argument("--device", required=True)
ap.add_argument("--base")
ap.add_argument("zn")
ap.add_argument("config")
a = ap.parse_args()

dev = json.load(open(os.path.join(a.db, "devices.json")))["families"]["ECP5"]["devices"][a.device]
pre = {"12": "25K_", "25": "25K_", "45": "45K_", "85": "85K_"}[re.search(r"-(\d+)F", a.device).group(1)]
R = Resolver(dev["max_row"], dev["max_col"], pre)

base = set()
if a.base:
    cur = None
    for line in open(a.base):
        t = line.split()
        if t[:1] == [".tile"]: cur = t[1]
        elif t[:1] == ["arc:"]: base.add((cur, t[1], t[2]))

drivers, out = defaultdict(set), defaultdict(set)
cur = None
for line in open(a.config):
    t = line.split()
    if t[:1] == [".tile"]: cur = t[1]
    elif t[:1] == ["arc:"] and (cur, t[1], t[2]) not in base:
        r, c = tile_loc(cur)
        src, dst = R(r, c, t[2]), R(r, c, t[1])
        drivers[dst].add(src)
        out[src].add(dst)

# every fixed connection on the die, resolved
grid = json.load(open(os.path.join(a.db, "ECP5", a.device, "tilegrid.json")))
fixed_out, tcache = defaultdict(set), {}
for tname, v in grid.items():
    ty = v["type"]
    if ty not in tcache:
        p = os.path.join(a.db, "ECP5", "tiledata", ty, "bits.db")
        tcache[ty] = parse_db(p)[1] if os.path.exists(p) else []
    r, c = tile_loc(tname)
    for snk, src in tcache[ty]:
        gs, gd = R(r, c, src), R(r, c, snk)
        if gs and gd:
            fixed_out[gs].add(gd)

def w(s):
    m = re.fullmatch(r"R(\d+)C(\d+)/(.*)", s)
    return (int(m.group(1)), int(m.group(2)), m.group(3))

nets, byname = [], {}
for t in (l.split() for l in open(a.zn)):
    if t[:1] == ["net"]:
        byname[t[1]] = (t[1], w(t[2]), [w(x) for x in t[3:]])
        nets.append(byname[t[1]])
    elif t[:1] == ["sinks"]:
        byname[t[1]][2].extend(w(x) for x in t[2:])

bad = 0
for d, s in drivers.items():
    if len(s) > 1:
        bad += 1; print("FAIL 1: %s driven by %d arcs: %s" % (d, len(s), sorted(s)))
owner = {}
for name, src, sinks in nets:
    if src in drivers:
        bad += 1; print("FAIL 2: net %s source %s is driven by an arc" % (name, src))
    seen, todo = {src}, [src]
    while todo:
        x = todo.pop()
        for n in list(out.get(x, ())) + list(fixed_out.get(x, ())):
            if n not in seen:
                seen.add(n); todo.append(n)
    for s in sinks:
        if s not in seen:
            bad += 1; print("FAIL 3: net %s does not reach %s" % (name, s))
    for x in seen:
        if x not in drivers:
            continue
        if x in owner:
            bad += 1; print("FAIL 4: %s reached by nets %s and %s" % (x, owner[x], name))
        owner[x] = name

arcs = sum(len(v) for v in drivers.values())
print("%s: %d nets, %d arcs, %d wires: %s" % (os.path.basename(a.config), len(nets), arcs,
      len(owner), "LEGAL" if not bad else "%d FAILURES" % bad))
sys.exit(1 if bad else 0)
