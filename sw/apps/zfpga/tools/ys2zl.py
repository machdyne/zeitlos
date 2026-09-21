#!/usr/bin/env python3
#
# ys2zl.py -- a yosys synth_ecp5 netlist and an .lpf, as a zfpga .zl
#
#   yosys -p "synth_ecp5 -noccu2 -nowidelut -nodsp -nobram -nolutram \
#             -top top -json d.json" d.v
#   ys2zl.py d.json d.lpf --device LFE5U-25F --package CABGA256 -o d.zl
#
# Host-only, for tests and until `zfpga synth` exists (Phase 6), which is
# meant to write .zl itself. A .zl is a LOGICAL netlist: cells with no
# sites, connected by named nets (docs/zfpga.md sec. 15.1):
#
#   device LFE5U-25F
#   package CABGA256
#   input  clk A7 net=n2 type=LVCMOS33
#   output led A2 net=n9
#   lut  $abc$123 init=0x00FF a=0 b=0 c=0 d=n7 z=n19
#   ff   $auto$45 d=n16 q=n10 clk=n2 ce=n4 lsr=n3 regset=RESET srmode=ASYNC
#   ccu2 $alu$7 init0=0x96AA init1=0x96AA inject0=NO inject1=NO \
#        a0=1 b0=n9 c0=0 d0=1 a1=0 b1=n10 c1=0 d1=1 cin=0 cout=n11 s0=n12 s1=n13
#
# A LUT input may be a net, 0 or 1. With -noccu2 dropped from the flags,
# carry chains arrive as ccu2 cells (yosys's CCU2C). Anything but LUT4,
# TRELLIS_FF and CCU2C is refused by name.

import argparse
import json
import re
import sys


def die(msg):
    sys.stderr.write("ys2zl: " + msg + "\n")
    sys.exit(1)


def read_lpf(path):
    """LOCATE COMP "x" SITE "y"; and IOBUF PORT "x" K=V ...;"""
    site, attrs = {}, {}
    text = open(path).read()
    for stmt in text.split(";"):
        t = stmt.strip()
        m = re.match(r'LOCATE\s+COMP\s+"([^"]+)"\s+SITE\s+"([^"]+)"', t)
        if m:
            site[m.group(1)] = m.group(2)
            continue
        m = re.match(r'IOBUF\s+PORT\s+"([^"]+)"\s+(.*)', t, re.S)
        if m:
            attrs[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split())
    return site, attrs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("lpf")
    ap.add_argument("--device", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    mods = json.load(open(a.json))["modules"]
    top = [m for m in mods.values() if m.get("attributes", {}).get("top")] or list(mods.values())
    m = top[0]
    site, attrs = read_lpf(a.lpf)

    def net(b):
        if b == "0" or b == "1":
            return b
        if b == "x" or b == "z":
            return "0"
        return "n%d" % b

    out = ["# converted by tools/ys2zl.py from %s and %s" % (a.json, a.lpf),
           "device %s" % a.device, "package %s" % a.package, ""]

    for pname in sorted(m["ports"]):
        p = m["ports"][pname]
        d = p["direction"]
        if d == "inout":
            die("port %s is inout: tristate IO is not in the Phase 5 v1 subset" % pname)
        for i, b in enumerate(p["bits"]):
            name = pname if len(p["bits"]) == 1 else "%s[%d]" % (pname, i)
            if name not in site:
                die("port %s has no LOCATE in %s" % (name, a.lpf))
            opts = " ".join("%s=%s" % (k.lower(), v) for k, v in
                            sorted(attrs.get(name, {}).items()))
            out.append("%s %s %s net=%s %s" % ("input" if d == "input" else "output",
                                              name, site[name], net(b), opts))
    out.append("")

    for cname in sorted(m["cells"]):
        c = m["cells"][cname]
        cn = c["connections"]
        one = lambda k: net(cn[k][0]) if k in cn and cn[k] else "0"
        if c["type"] == "LUT4":
            init = int(c["parameters"]["INIT"], 2)
            out.append("lut %s init=0x%04X a=%s b=%s c=%s d=%s z=%s" % (
                cname.replace(" ", "_"), init, one("A"), one("B"), one("C"), one("D"), one("Z")))
        elif c["type"] == "TRELLIS_FF":
            p = {k: str(v).rstrip() for k, v in c["parameters"].items()}
            line = "ff %s d=%s q=%s clk=%s" % (cname.replace(" ", "_"), one("DI"), one("Q"), one("CLK"))
            if "CE" in cn:
                line += " ce=%s" % one("CE")
            if "LSR" in cn:
                line += " lsr=%s" % one("LSR")
            for k in ("GSR", "REGSET", "SRMODE", "LSRMODE", "CEMUX", "CLKMUX", "LSRMUX"):
                if k in p:
                    line += " %s=%s" % (k.lower(), p[k])
            out.append(line)
        elif c["type"] == "CCU2C":
            p = {k: str(v).rstrip() for k, v in c["parameters"].items()}
            line = "ccu2 %s init0=0x%04X init1=0x%04X inject0=%s inject1=%s" % (
                cname.replace(" ", "_"), int(p["INIT0"], 2), int(p["INIT1"], 2),
                p.get("INJECT1_0", "YES"), p.get("INJECT1_1", "YES"))
            for k in ("A0", "B0", "C0", "D0", "A1", "B1", "C1", "D1", "CIN"):
                line += " %s=%s" % (k.lower(), one(k))
            for k in ("COUT", "S0", "S1"):
                line += " %s=%s" % (k.lower(), one(k) if k in cn and cn[k] else "-")
            out.append(line)
        else:
            die("cell %s is %s: not in the Phase 5 subset (LUT4, TRELLIS_FF, CCU2C)" % (cname, c["type"]))
    out.append("")
    open(a.output, "w").write("\n".join(out))


if __name__ == "__main__":
    main()
