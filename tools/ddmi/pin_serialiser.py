#!/usr/bin/env python3
"""Pin the screen modes' 378 MHz serialiser beside the HDMI pins.

    pin_serialiser.py SOC_JSON LPF DEVICE PACKAGE

On a board whose HDMI pins are in the top or bottom bank, the screen
modes' serialiser (rtl/gpu/gpu_vchar.v, X1) runs in the fabric at
378 MHz: a 2.6 ns clock. Left to itself, nextpnr put each lane's
shift register 35 to 65 columns from its pin (on Sergei ML2, around
X6 with the pins at X44..X71), and bclk passed or failed by a few
per cent with the seed: 363 to 413 MHz over builds of the same RTL.

This puts every 378 MHz flip-flop of a lane -- its shift register,
its ring, its lock bit and its three output stages -- in the three
tiles beside that lane's pin (rows 2..4 of its column, then the
column beside it; a tile for each kind of control signal, which a
tile's slices share), by
the BEL attribute nextpnr honours. The LUT in front of each of them,
where there is one, goes in the same slice: nextpnr places a LUT and
the flip-flop it feeds as one, by the LUT, and ignored a BEL on the
flip-flop alone. The one long hop left is from the character clock's words
into the shift register, which the design gives 7.9 ns or more and
nextpnr does not time (docs/screen_modes.md, "The serialiser's place").

The pins' columns come from SITES below, measured from nextpnr's own
placement on these boards. A site that is not there (another board,
another device) pins nothing: the build goes on unpinned, as before,
and this says so. The netlist is changed in place; running it twice
does nothing more.
"""

import json
import re
import sys

# (device, package) -> { LPF site: the column of its PIO (top edge, Y0) }
SITES = {
    ("45k", "CABGA256"): {
        # Sergei ML2 / Mozart ML2: CK B10, D0 A9, D1 C8, D2 A11
        # Mozart ML1 / Sergei ML1: CK B13, D0 A11, D1 B12, D2 B10
        "A9": 51, "A11": 71, "B10": 53, "B12": 74, "B13": 78, "C8": 44,
    },
    ("85k", "CABGA381"): {
        # ULX3S 85F: CK A17, D0 A16, D1 A14, D2 A12
        "A17": 116, "A16": 110, "A14": 83, "A12": 76,
    },
}

# lane -> its pin (gpu_video.v: dvi_p[3] clock, [2] red, [1] green, [0] blue)
LANES = {"r": "DDMI_D2_P", "g": "DDMI_D1_P", "b": "DDMI_D0_P", "c": "DDMI_CK_P"}

PREFIX = "gpu_video_i.gpu_vchar_i."
# the X1 serialiser's flip-flops, per lane: shift register, ring, lock,
# output stages. Grouped so that the two flip-flops of a slice share
# their clock enable and set/reset, as a slice requires.
GROUPS = ["o", "q", "p", "det", "lk", "rg", "sh"]


def lpf_sites(path):
    sites = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*LOCATE\s+COMP\s+"([^"]+)"\s+SITE\s+"([^"]+)"', line)
            if m:
                sites[m.group(1)] = m.group(2)
    return sites


# The tiles a lane's flip-flops go in: rows 2..4 of the pin's column
# (row 1, beside the I/O, holds no slices), then the column beside it.
# On every known layout that column is no other lane's.
def tiles(x):
    return [(x, 2), (x, 3), (x, 4), (x + 1, 2), (x + 1, 3), (x + 1, 4)]


def control(cell):
    """What a slice's two flip-flops must share: their clock, clock
    enable and set/reset nets, and how those are used."""
    c = cell.get("connections", {})
    p = cell.get("parameters", {})
    return (tuple(c.get("CLK", [])), tuple(c.get("CE", [])), tuple(c.get("LSR", [])),
            p.get("CLKMUX"), p.get("CEMUX"), p.get("LSRMUX"), p.get("SRMODE"), p.get("GSR"))


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

    ffs = {}
    for name, cell in cells.items():
        if cell.get("type") != "TRELLIS_FF" or not name.startswith(PREFIX):
            continue
        rest = name[len(PREFIX):]
        for lane in LANES:
            for g in GROUPS:
                tok = g + "_" + lane
                if rest == tok or rest.startswith(tok + "_") or rest.startswith(tok + "["):
                    ffs.setdefault(lane, {}).setdefault(g, []).append(name)
    if not ffs:
        print("pin_serialiser: no X1 serialiser in this build; nothing pinned")
        return

    cols = {}
    for lane, port in LANES.items():
        site = sites.get(port)
        if table is None or site is None or site not in table:
            print("pin_serialiser: %s's site %s on %s/%s is not known; nothing pinned"
                  % (port, site, device, package))
            return
        cols[lane] = table[site]

    # A tile's four slices share their control signals, so each
    # control signature (clock, enable, set/reset and their modes) has
    # rows of its own: the plain flip-flops -- output stages, lock bit,
    # shift register, in that order -- from the row beside the pin, then
    # the ring's.
    pins = []
    for lane, groups in sorted(ffs.items()):
        bysig = {}
        order = []
        for g in GROUPS:
            for name in sorted(groups.get(g, [])):
                sig = control(cells[name])
                if sig not in bysig:
                    bysig[sig] = []
                    order.append(sig)
                bysig[sig].append(name)
        rows = tiles(cols[lane])
        for sig in order:
            names = bysig[sig]
            k = 0
            while k < len(names):
                if not rows:
                    print("pin_serialiser: lane %s needs more than %d tiles; nothing pinned"
                          % (lane, len(tiles(0))))
                    return
                x, y = rows.pop(0)
                for s_i, sl in enumerate("ABCD"):
                    for ff in (0, 1):
                        if k < len(names):
                            pins.append((names[k], "X%d/Y%d/SLICE%s.FF%d" % (x, y, sl, ff)))
                            k += 1

    # A flip-flop fed by a LUT is packed with it, and the pair goes
    # where the LUT goes: a BEL on the flip-flop alone was ignored for
    # those (the shift register and ring stayed 15 to 60 columns away).
    # So the LUT that drives a pinned flip-flop's DI, if it drives
    # nothing else, is pinned to the same slice: K0 with FF0, K1 with FF1.
    driver = {}
    fanout = {}
    for cname, cell in cells.items():
        for port, bits in cell.get("connections", {}).items():
            d = cell.get("port_directions", {}).get(port)
            for b in bits:
                if isinstance(b, int):
                    if d == "output":
                        driver[b] = (cname, port)
                    else:
                        fanout[b] = fanout.get(b, 0) + 1
    luts = []
    for name, bel in pins:
        di = cells[name].get("connections", {}).get("DI", [])
        if len(di) != 1 or not isinstance(di[0], int):
            continue
        drv = driver.get(di[0])
        if not drv or cells[drv[0]].get("type") != "LUT4" or fanout.get(di[0], 0) != 1:
            continue
        luts.append((drv[0], bel.replace(".FF", ".K")))

    for name, bel in pins + luts:
        cells[name].setdefault("attributes", {})["BEL"] = bel
    with open(path, "w") as f:
        json.dump(net, f)
    print("pin_serialiser: %d flip-flops and %d LUTs pinned beside the HDMI pins (columns %s)"
          % (len(pins), len(luts), ", ".join("%s X%d" % (l, cols[l]) for l in "rgbc")))


if __name__ == "__main__":
    main()
