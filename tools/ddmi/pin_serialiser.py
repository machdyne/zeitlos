#!/usr/bin/env python3
"""Pin the screen modes' 378 MHz serialiser beside the HDMI pins.

    pin_serialiser.py SOC_JSON LPF DEVICE PACKAGE

On a board whose HDMI pins are in the top or bottom bank, the screen
modes' serialiser (rtl/gpu/gpu_vchar.v, X1) runs in the fabric at
378 MHz: a 2.6 ns clock, about 0.75 ns of which is a flip-flop and a
LUT. What is left is wire, so where the flip-flops are decides whether
the clock is met, and left to itself nextpnr decided differently with
every seed: 363 to 413 MHz over builds of the same RTL on Sergei ML2.

This puts every flip-flop on that clock where it has to be, by the BEL
attribute nextpnr honours, so that no seed has a say:

  each lane   its shift register, ring, set (d_x) and three output
              stages: in the tiles beside the lane's pin (rows 2..4 of
              the pin's column, then the column beside it)
  the lock    a1, a2, lk, det -- made once, in one place: the column
              half way between the outermost lanes
  the hops    h_x, det's one register on its way to lane x: half way
              between the lock and the lane

So every 378 MHz path is inside a lane (a few tiles), or one of the two
hops from the lock to a lane, flip-flop to flip-flop with no LUT, each
about a quarter of the pins' span: 7 columns on Sergei ML2, 10 on
ULX3S 85F.

A flip-flop fed by a LUT is placed with it, by the LUT, so the LUT in
front of each pinned flip-flop, where there is one, goes in the same
slice (nextpnr ignored a BEL on the flip-flop alone). A tile's slices
share their clock enable and set/reset, so a tile holds flip-flops of
one kind of control only.

The one long wire left is the character clock's words into the shift
registers, which the design gives 7.9 ns (gpu_vchar.v) and nextpnr does
not time.

The pins' columns come from SITES below: what nextpnr says of each
device ("pin 'DDMI_D0_P$tr_io' constrained to Bel 'X51/Y0/PIOA'").
`make serialiser_timing BOARD=...` places and routes the serialiser
alone, pinned, in seconds; run it after adding a board here.

A board that is not there (another package, another device) is left
unpinned, as before, and this says so. A build whose serialiser is not
what this expects -- a flip-flop missing, the four lanes' hops merged
into one by synthesis -- is stopped: pinning half of it would be worse
than none. The netlist is changed in place; running this twice does
nothing more.
"""

import json
import re
import sys

# (device, package) -> { LPF site: the column of its PIO (top edge, Y0) }
CABGA256_45 = {
    # Sergei ML2 / Mozart ML2: CK B10, D0 A9, D1 C8, D2 A11
    # Mozart ML1 / Sergei ML1: CK B13, D0 A11, D1 B12, D2 B10
    "A9": 51, "A11": 71, "B10": 53, "B12": 74, "B13": 78, "C8": 44,
}
CABGA256_25 = {
    # Mozart ML0 / Sergei ML0 (ML1's board): CK B13, D0 A11, D1 B12, D2 B10
    "A11": 53, "B10": 44, "B12": 56, "B13": 60,
}
# ULX3S: CK A17, D0 A16, D1 A14, D2 A12
ULX3S_25 = {"A17": 62, "A16": 56, "A14": 47, "A12": 40}
ULX3S_45 = {"A17": 80, "A16": 74, "A14": 56, "A12": 49}
ULX3S_85 = {"A17": 116, "A16": 110, "A14": 83, "A12": 76}
SITES = {
    ("45k", "CABGA256"): CABGA256_45,
    ("25k", "CABGA256"): CABGA256_25,
    ("12k", "CABGA256"): CABGA256_25,
    ("12k", "CABGA381"): ULX3S_25,
    ("25k", "CABGA381"): ULX3S_25,
    ("45k", "CABGA381"): ULX3S_45,
    ("85k", "CABGA381"): ULX3S_85,
}

# lane -> its pin (gpu_video.v: dvi_p[3] clock, [2] red, [1] green, [0] blue)
LANES = {"r": "DDMI_D2_P", "g": "DDMI_D1_P", "b": "DDMI_D0_P", "c": "DDMI_CK_P"}

PREFIX = "gpu_video_i.gpu_vchar_i."
# A lane's flip-flops, nearest the pin first, and how many of each there
# must be: output stages, the ring's set, the ring, the shift register.
LANE_GROUPS = [("o", 2), ("q", 2), ("p", 2), ("d", 1), ("rg", 5), ("sh", 10)]
SHARED = ["a1", "a2", "det", "lk"]
ROWS = (2, 3, 4)        # row 1, beside the I/O, holds no slices


def lpf_sites(path):
    sites = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*LOCATE\s+COMP\s+"([^"]+)"\s+SITE\s+"([^"]+)"', line)
            if m:
                sites[m.group(1)] = m.group(2)
    return sites


def control(cell):
    """What a tile's flip-flops share: their clock, clock enable and
    set/reset nets, and how those are used."""
    c = cell.get("connections", {})
    p = cell.get("parameters", {})
    return repr((c.get("CLK"), c.get("CE"), c.get("LSR"),
                 p.get("CLKMUX"), p.get("CEMUX"), p.get("LSRMUX"),
                 p.get("SRMODE"), p.get("GSR")))


class Tiles:
    """Hands out flip-flop sites: a tile to one kind of control."""

    def __init__(self):
        self.used = {}      # (x, y) -> [control, flip-flops placed]

    def put(self, sig, candidates):
        for t in candidates:
            e = self.used.get(t)
            if e is None:
                e = self.used[t] = [sig, 0]
            if e[0] != sig or e[1] >= 8:
                continue
            k = e[1]
            e[1] += 1
            return "X%d/Y%d/SLICE%s.FF%d" % (t[0], t[1], "ABCD"[k // 2], k % 2)
        return None


def column(x):
    return [(x, y) for y in ROWS]


def stop(why):
    sys.exit("pin_serialiser: %s\npin_serialiser: the serialiser (rtl/gpu/gpu_vchar.v, X1) "
             "is not what this script pins; fix one or the other" % why)


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    path, lpf, device, package = sys.argv[1:]
    table = SITES.get((device, package))
    sites = lpf_sites(lpf)

    with open(path) as f:
        net = json.load(f)
    top = net["modules"].get("sysctl")
    if top is None:
        print("pin_serialiser: no sysctl in the netlist; nothing pinned")
        return
    cells = top["cells"]

    def flops(token):
        """The flip-flops of register `token`, by name."""
        out = []
        for name, cell in cells.items():
            if cell.get("type") != "TRELLIS_FF" or not name.startswith(PREFIX):
                continue
            rest = name[len(PREFIX):]
            if rest == token or rest.startswith(token + "_TRELLIS_FF") or \
                    rest.startswith(token + "["):
                out.append(name)
        return sorted(out)

    # the X1 serialiser is there if its middle output stage is (the
    # ODDRX2F one, X2, has none, and runs at 189 MHz: nothing to pin)
    if not any(flops("p_" + lane) for lane in LANES):
        print("pin_serialiser: no 378 MHz serialiser in this build; nothing pinned")
        return

    cols = {}
    for lane, port in LANES.items():
        site = sites.get(port)
        if table is None or site is None or site not in table:
            print("pin_serialiser: %s's site %s on %s/%s is not known; nothing pinned"
                  % (port, site, device, package))
            return
        cols[lane] = table[site]

    taken = set()
    for x in cols.values():
        taken.update((x, x + 1))
    centre = (min(cols.values()) + max(cols.values())) // 2
    while centre in taken:
        centre += 1
    taken.update((centre, centre + 1))

    tiles = Tiles()
    pins = []

    def place(name, candidates):
        bel = tiles.put(control(cells[name]), candidates)
        if bel is None:
            stop("no room for %s" % name)
        pins.append((name, bel))

    # the lanes
    for lane in "rgbc":
        x = cols[lane]
        for group, count in LANE_GROUPS:
            names = flops("%s_%s" % (group, lane))
            if len(names) != count:
                stop("%s_%s has %d flip-flops, not %d" % (group, lane, len(names), count))
            for name in names:
                place(name, column(x) + column(x + 1))

    # the lock, in one place between the lanes
    for token in SHARED:
        names = flops(token)
        if len(names) != 1:
            stop("%s has %d flip-flops, not 1" % (token, len(names)))
        place(names[0], column(centre) + column(centre + 1))

    # the hops: half way from the lock to each lane, clear of the lanes
    hops = {}
    for lane in "rgbc":
        names = flops("h_" + lane)
        if len(names) != 1:
            stop("h_%s has %d flip-flops, not 1 (merged by synthesis?)" % (lane, len(names)))
        # the free column nearest the midpoint, between the two if
        # there is one (the lanes' columns and the lock's are taken)
        mid = (centre + cols[lane]) // 2
        lo, hi = sorted((centre, cols[lane]))
        between = [x for x in range(lo, hi + 1) if x not in taken]
        around = [x for x in range(lo - 6, hi + 7) if x not in taken and x > 1]
        free = sorted(between or around, key=lambda x: (abs(x - mid), x))
        if not free:
            stop("no column free for h_%s" % lane)
        hops[lane] = free[0]
        place(names[0], column(free[0]))

    # the LUT that drives a pinned flip-flop's DI, if it drives nothing
    # else, into the same slice: K0 with FF0, K1 with FF1
    driver = {}
    fanout = {}
    for cname, cell in cells.items():
        for port, bits in cell.get("connections", {}).items():
            d = cell.get("port_directions", {}).get(port)
            for b in bits:
                if isinstance(b, int):
                    if d == "output":
                        driver[b] = cname
                    else:
                        fanout[b] = fanout.get(b, 0) + 1
    luts = []
    for name, bel in pins:
        di = cells[name].get("connections", {}).get("DI", [])
        if len(di) != 1 or not isinstance(di[0], int):
            continue
        drv = driver.get(di[0])
        if drv is None or cells[drv].get("type") != "LUT4" or fanout.get(di[0], 0) != 1:
            continue
        luts.append((drv, bel.replace(".FF", ".K")))

    for name, bel in pins + luts:
        cells[name].setdefault("attributes", {})["BEL"] = bel

    with open(path, "w") as f:
        json.dump(net, f)
    print("pin_serialiser: %d flip-flops and %d LUTs pinned: lanes %s; lock X%d; hops %s"
          % (len(pins), len(luts),
             ", ".join("%s X%d" % (l, cols[l]) for l in "rgbc"), centre,
             ", ".join("%s X%d" % (l, hops[l]) for l in "rgbc")))


if __name__ == "__main__":
    main()
