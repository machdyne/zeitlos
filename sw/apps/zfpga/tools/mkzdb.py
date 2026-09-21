#!/usr/bin/env python3
#
# mkzdb.py -- convert a Project Trellis database into a packed .zdb
#
# Host-only. The device never reads JSON or the Trellis ASCII: 2.8MB of
# tilegrid.json per die, parsed on a 12 MIPS machine on every run, would
# be most of the run. This does it once, at build time.
#
#   mkzdb.py --db ext/prjtrellis-db --device LFE5U-25F --also LFE5U-12F \
#            --source <commit> -o lfe5u25f.zdb
#
# The bits.db parser below follows libtrellis's own rules, because the
# test for zfpga pack is byte-identity against ecppack, and every place
# this differs from libtrellis is a place that test fails for a reason
# that is not zfpga's:
#
#   - a record ends at the next line whose first non-blank character is
#     '.', not at a blank line;
#   - '#' starts a comment anywhere;
#   - a later .mux / .config / .config_enum with the same name REPLACES
#     the earlier one, and within a record a later entry with the same
#     key replaces the earlier (std::map assignment);
#   - a bit group is a SET: duplicates collapse, order is irrelevant;
#   - '-' ends a bit group (so "OPT -" is an empty group);
#   - a .config word with no default has an all-zero default;
#   - a .config_enum with no default gets NO default applied at all.
#
# The format is described in zfpga.h, and the two must agree: the
# header's version number is the check.

import argparse
import json
import os
import struct
import sys

ZDB_VERSION = 3

# Section order in the header table. zfpga.h's ZDB_S_* must match.
SECTIONS = ["STR", "VAR", "TILE", "TYPE", "MUX", "ARC", "WORD", "GRP",
            "ENUM", "OPT", "BIT", "PIO", "PIN", "BASE", "FIX"]

HDR = 256

FAMILY_ECP5 = 1


def die(msg):
    sys.stderr.write("mkzdb: " + msg + "\n")
    sys.exit(1)


# -- bits.db --------------------------------------------------------------

def parse_bit(tok, where):
    inv = False
    s = tok
    if s.startswith("!"):
        inv = True
        s = s[1:]
    if not s.startswith("F") or "B" not in s:
        die("%s: bad bit '%s'" % (where, tok))
    fr, bi = s[1:].split("B", 1)
    return (int(fr), int(bi), inv)


def parse_group(toks, where):
    """Tokens of one bit group. '-' terminates, like BitGroup::operator>>."""
    g = set()
    for t in toks:
        if t == "-":
            break
        g.add(parse_bit(t, where))
    return g


def logical_lines(path):
    """Yield token lists, one per non-empty line, comments stripped."""
    with open(path) as f:
        for n, raw in enumerate(f, 1):
            # '#' is a comment only where a token would start, as in
            # libtrellis: `>>` reads "foo#bar" as one word.
            toks = []
            for t in raw.split():
                if t.startswith("#"):
                    break
                toks.append(t)
            if toks:
                yield n, toks


def parse_bitsdb(path):
    muxes = {}      # sink -> {source: group}
    fixed = []      # (sink, source): hard connections, used for the PIO table
    words = {}      # name -> (defval_msb_first, [group, ...])
    enums = {}      # name -> (default or None, {opt: group})
    cur = None
    kind = None

    for n, toks in logical_lines(path):
        where = "%s:%d" % (path, n)
        if toks[0].startswith("."):
            d = toks[0]
            if d == ".mux":
                cur = {}
                muxes[toks[1]] = cur
                kind = "mux"
            elif d == ".config":
                name = toks[1]
                dv = toks[2] if len(toks) > 2 else None
                cur = [dv, []]
                words[name] = cur
                kind = "word"
            elif d == ".config_enum":
                name = toks[1]
                dv = toks[2] if len(toks) > 2 else None
                cur = [dv, {}]
                enums[name] = cur
                kind = "enum"
            elif d == ".fixed_conn":
                fixed.append((toks[1], toks[2]))
                cur = None
                kind = None
            else:
                die("%s: unknown directive %s" % (where, d))
            continue

        if kind == "mux":
            cur[toks[0]] = parse_group(toks[1:], where)
        elif kind == "word":
            cur[1].append(parse_group(toks, where))
        elif kind == "enum":
            cur[1][toks[0]] = parse_group(toks[1:], where)
        else:
            die("%s: data outside a record" % where)

    # Normalise words: default MSB-first string, all zero if absent.
    out_words = {}
    for name, (dv, groups) in words.items():
        if dv is None:
            dv = "0" * len(groups)
        if len(dv) != len(groups) or any(c not in "01" for c in dv):
            die("%s: word %s default '%s' does not match %d bits"
                % (path, name, dv, len(groups)))
        out_words[name] = (dv, groups)

    for name, (dv, opts) in enums.items():
        if dv is not None and dv not in opts:
            die("%s: enum %s default '%s' is not an option" % (path, name, dv))

    return muxes, out_words, {k: (v[0], v[1]) for k, v in enums.items()}, fixed


def check_group(g, where):
    """A bit listed both plain and inverted would make the result depend
    on std::set order. Refuse rather than guess."""
    seen = {}
    for (f, b, inv) in g:
        if (f, b) in seen and seen[(f, b)] != inv:
            die("%s: bit F%dB%d both plain and inverted" % (where, f, b))
        seen[(f, b)] = inv


# -- writer ---------------------------------------------------------------

class Strings:
    def __init__(self):
        self.buf = bytearray(b"\0")     # offset 0 is the empty string
        self.idx = {"": 0}

    def add(self, s):
        if s not in self.idx:
            self.idx[s] = len(self.buf)
            self.buf += s.encode("ascii") + b"\0"
        return self.idx[s]


def bkey(s):
    """std::map<string> order: bytewise. Same as C strcmp."""
    return s.encode("ascii")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True, help="prjtrellis-db root")
    ap.add_argument("--device", required=True)
    ap.add_argument("--also", action="append", default=[],
                    help="another device sharing this die (e.g. LFE5U-12F)")
    ap.add_argument("--source", default="", help="upstream commit, recorded")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--base", help="baseline .config fragment to embed")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    devices = json.load(open(os.path.join(a.db, "devices.json")))
    fam = devices["families"]["ECP5"]["devices"]
    if a.device not in fam:
        die("unknown device %s" % a.device)
    info = fam[a.device]

    variants = [(a.device, int(info["idcode"], 16))]
    for other in a.also:
        if other not in fam:
            die("unknown device %s" % other)
        oi = fam[other]
        for k in ("frames", "bits_per_frame", "pad_bits_before_frame",
                  "pad_bits_after_frame", "max_row", "max_col"):
            if oi.get(k) != info.get(k):
                die("%s differs from %s in %s; not the same die"
                    % (other, a.device, k))
        og = os.path.join(a.db, "ECP5", other, "tilegrid.json")
        if os.path.exists(og):
            mine = open(os.path.join(a.db, "ECP5", a.device,
                                     "tilegrid.json"), "rb").read()
            if open(og, "rb").read() != mine:
                die("%s tilegrid differs from %s" % (other, a.device))
        variants.append((other, int(oi["idcode"], 16)))

    grid = json.load(open(os.path.join(a.db, "ECP5", a.device,
                                       "tilegrid.json")))

    type_names = sorted(set(v["type"] for v in grid.values()), key=bkey)
    type_ix = {t: i for i, t in enumerate(type_names)}

    S = Strings()
    recs = {s: bytearray() for s in SECTIONS if s != "STR"}
    counts = {s: 0 for s in SECTIONS}
    nbits = [0]

    def emit(sec, data):
        recs[sec] += data
        counts[sec] += 1

    def emit_group(g, where):
        check_group(g, where)
        first = counts["BIT"]
        for (f, b, inv) in sorted(g):
            if f >= 128 or b >= 16:
                die("%s: bit F%dB%d out of packing range" % (where, f, b))
            emit("BIT", struct.pack("<H", (f << 4) | b | (0x8000 if inv else 0)))
        return first, counts["BIT"] - first

    for v in variants:
        emit("VAR", struct.pack("<II", S.add(v[0]), v[1]))

    # Tiles, sorted by full name. Tiles are disjoint in CRAM on every
    # ECP5 die (checked when the design was written; zfpga.h records
    # it), so this order is for lookup, not for correctness.
    for name in sorted(grid, key=bkey):
        t = grid[name]
        if t["cols"] > 255 or t["rows"] > 255:
            die("tile %s too large to pack" % name)
        emit("TILE", struct.pack("<IHHHBB", S.add(name), type_ix[t["type"]],
                                 t["start_frame"], t["start_bit"],
                                 t["cols"], t["rows"]))

    type_fixed, type_enums = {}, {}
    for tname in type_names:
        path = os.path.join(a.db, "ECP5", "tiledata", tname, "bits.db")
        if os.path.exists(path):
            muxes, words, enums, fixed = parse_bitsdb(path)
        else:
            muxes, words, enums, fixed = {}, {}, {}, []
            if a.verbose:
                print("mkzdb: note: no bits.db for %s" % tname)
        type_fixed[tname] = fixed
        type_enums[tname] = set(enums)

        mux0 = counts["MUX"]
        for sink in sorted(muxes, key=bkey):
            arcs = muxes[sink]
            arc0 = counts["ARC"]
            for src in sorted(arcs, key=bkey):
                b0, bn = emit_group(arcs[src], "%s mux %s <- %s"
                                    % (tname, sink, src))
                emit("ARC", struct.pack("<III", S.add(src), b0, bn))
            emit("MUX", struct.pack("<III", S.add(sink), arc0,
                                    counts["ARC"] - arc0))

        word0 = counts["WORD"]
        for wname in sorted(words, key=bkey):
            dv, groups = words[wname]
            g0 = counts["GRP"]
            for i, g in enumerate(groups):
                b0, bn = emit_group(g, "%s word %s[%d]" % (tname, wname, i))
                emit("GRP", struct.pack("<II", b0, bn))
            emit("WORD", struct.pack("<IIII", S.add(wname), S.add(dv),
                                     g0, counts["GRP"] - g0))

        enum0 = counts["ENUM"]
        for ename in sorted(enums, key=bkey):
            dv, opts = enums[ename]
            o0 = counts["OPT"]
            defix = -1
            for i, oname in enumerate(sorted(opts, key=bkey)):
                b0, bn = emit_group(opts[oname], "%s enum %s=%s"
                                    % (tname, ename, oname))
                emit("OPT", struct.pack("<III", S.add(oname), b0, bn))
                if oname == dv:
                    defix = i
            emit("ENUM", struct.pack("<IIIi", S.add(ename), o0,
                                     counts["OPT"] - o0, defix))

        # Fixed connections, which the router crosses without writing
        # anything: version 3 (docs/zfpga.md sec. 13.3).
        fix0 = counts["FIX"]
        for sink, src in sorted(set(fixed), key=lambda x: (bkey(x[1]), bkey(x[0]))):
            emit("FIX", struct.pack("<II", S.add(sink), S.add(src)))
        emit("TYPE", struct.pack("<IIIIIIIII", S.add(tname),
                                 mux0, counts["MUX"] - mux0,
                                 word0, counts["WORD"] - word0,
                                 enum0, counts["ENUM"] - enum0,
                                 fix0, counts["FIX"] - fix0))

    src_off = S.add(a.source)

    # -- the PIO table ----------------------------------------------------
    # One record per PIO site, with the tiles zfpga pnr has to write for
    # it. The side rules are nextpnr-ecp5 0.6's get_pio_tile() and
    # get_pic_tile() (ecp5/bitstream.cc), which Trellis does not record:
    # computed here, once, so the device looks them up rather than
    # carrying the rules.
    import re
    names = sorted(grid, key=bkey)
    tix = {n: i for i, n in enumerate(names)}
    at = {}
    for n in names:
        m = re.search(r"R(\d+)C(\d+)", n)
        at.setdefault((int(m.group(1)), int(m.group(2))), []).append(n)
    height = info["max_row"] + 1
    width = info["max_col"] + 1

    def tile_at(r, c, types):
        for n in at.get((r, c), []):
            if n.split(":")[1] in types:
                return n
        return None

    def pio_pic(r, c, L):
        if r == 0:
            if L == "A": return tile_at(0, c, {"PIOT0"}), tile_at(1, c, {"PICT0"})
            if L == "B": return tile_at(0, c + 1, {"PIOT1"}), tile_at(1, c + 1, {"PICT1"})
        elif r == height - 1:
            a_ = {"PICB0", "EFB0_PICB0", "EFB2_PICB0", "SPICB0"}
            b_ = {"PICB1", "EFB1_PICB1", "EFB3_PICB1"}
            if L == "A": return tile_at(r, c, a_), tile_at(r, c, a_)
            if L == "B": return tile_at(r, c + 1, b_), tile_at(r, c + 1, b_)
        elif c == 0 or c == width - 1:
            s_ = "L" if c == 0 else "R"
            pio = tile_at(r + 1, c, {"PIC%s1" % s_, "PIC%s1_DQS0" % s_, "PIC%s1_DQS3" % s_})
            if L in "AB":
                pic = tile_at(r, c, {"PIC%s0" % s_, "PIC%s0_DQS2" % s_})
            else:
                pic = tile_at(r + 2, c, {"PIC%s2" % s_, "PIC%s2_DQS1" % s_,
                                         "MIB_CIB_LR" if c == 0 else "MIB_CIB_LR_A"})
            return pio, pic
        return None, None

    CIB_TILES = {"CIB", "CIB_LR", "CIB_LR_S", "CIB_EFB0", "CIB_EFB1"}

    def rel(r, c, name):
        """Trellis relative wire name -> (row, col, basename)."""
        m = re.match(r"((?:[NSEW]\d+)+)_(.*)$", name)
        if not m:
            return r, c, name
        for d, n in re.findall(r"([NSEW])(\d+)", m.group(1)):
            n = int(n)
            if d == "N": r -= n
            elif d == "S": r += n
            elif d == "E": c += n
            else: c -= n
        return r, c, m.group(2)

    iodb = json.load(open(os.path.join(a.db, "ECP5", a.device, "iodb.json")))
    pio_ix = {}
    no_tie = 0
    for md in sorted(iodb["pio_metadata"], key=lambda x: (x["row"], x["col"], x["pio"])):
        r, c, L = md["row"], md["col"], md["pio"]
        pio_t, pic_t = pio_pic(r, c, L)
        if not pio_t or not pic_t:
            die("no PIO/PIC tile for R%dC%d PIO%s" % (r, c, L))
        # The tristate tie: which CIB wire drives JPADDT<L>. Found as a
        # fixed connection in the PIC tile's database, named relative to
        # the PIO's own location -- checked against nextpnr output by
        # tests/run.sh.
        # The connection is not always in the PIO's own PIC tile: a top
        # PIOB's JPADDTB is listed in PIOA's PICT0 (found when the first
        # table left every top PIOB unresolved). So the PIO's own tiles
        # are tried first, then the PIC/PIO tiles around it; the names
        # are relative to the PIO's location in every case.
        cands = [pic_t, pio_t]
        for (rr, cc) in ((r, c), (r + 1, c), (r, c + 1), (r + 1, c + 1),
                         (r + 2, c), (r - 1, c)):
            for n in at.get((rr, cc), []):
                ty_ = n.split(":")[1]
                if ("PIC" in ty_ or "PIO" in ty_) and n not in cands:
                    cands.append(n)
        tie_t, tie_e = -1, 0
        for tt in cands:
            for sink, src in type_fixed.get(tt.split(":")[1], []):
                if sink == "JPADDT" + L:
                    # relative to the LISTING tile's own location, like
                    # every Trellis name (docs/zfpga.md sec. 13.1)
                    m2 = re.search(r"R(\d+)C(\d+)", tt)
                    rr, cc, base = rel(int(m2.group(1)), int(m2.group(2)), src)
                    cib = tile_at(rr, cc, CIB_TILES)
                    en = "CIB." + base + "MUX"
                    if cib and en in type_enums.get(cib.split(":")[1], ()):
                        tie_t, tie_e = tix[cib], S.add(en)
            if tie_t >= 0:
                break
        if tie_t < 0:
            no_tie += 1
            sys.stderr.write("mkzdb: note: no tristate tie for R%dC%d PIO%s; zfpga pnr "
                             "will refuse it as an output\n" % (r, c, L))
        pio_ix[(r, c, L)] = counts["PIO"]
        emit("PIO", struct.pack("<HHBBHiiiI", r, c, ord(L), md["bank"], 0,
                                tix[pio_t], tix[pic_t], tie_t, tie_e))

    for pkg in sorted(iodb["packages"], key=bkey):
        for pin in sorted(iodb["packages"][pkg], key=bkey):
            q = iodb["packages"][pkg][pin]
            k = (q["row"], q["col"], q["pio"])
            if k not in pio_ix:
                die("pin %s/%s has no PIO" % (pkg, pin))
            emit("PIN", struct.pack("<III", S.add(pkg), S.add(pin), pio_ix[k]))

    base_text = b""
    if a.base:
        base_text = open(a.base, "rb").read()
    recs["BASE"] += base_text
    counts["BASE"] = len(base_text)
    if a.verbose and no_tie:
        print("mkzdb: note: %d PIOs have no tristate tie (outputs refused there)" % no_tie)

    # Layout: header, then sections, each 4-byte aligned.
    body = bytearray()
    table = []
    for sec in SECTIONS:
        while (HDR + len(body)) % 4:
            body += b"\0"
        off = HDR + len(body)
        if sec == "STR":
            data = bytes(S.buf)
            cnt = len(data)
        else:
            data = bytes(recs[sec])
            cnt = counts[sec]
        table.append((off, cnt))
        body += data
    total = HDR + len(body)

    hdr = bytearray()
    hdr += b"ZDB1"
    hdr += struct.pack("<III", ZDB_VERSION, total, FAMILY_ECP5)
    hdr += struct.pack("<HHBBBB", info["frames"], info["bits_per_frame"],
                       info["pad_bits_before_frame"],
                       info["pad_bits_after_frame"], len(variants), 0)
    hdr += struct.pack("<II", src_off, 0)
    for off, cnt in table:
        hdr += struct.pack("<II", off, cnt)
    assert len(hdr) <= HDR, len(hdr)
    hdr += b"\0" * (HDR - len(hdr))

    with open(a.output, "wb") as f:
        f.write(hdr)
        f.write(body)

    if a.verbose:
        print("mkzdb: %s: %d bytes" % (a.output, total))
        for sec, (off, cnt) in zip(SECTIONS, table):
            print("  %-5s %8d at %d" % (sec, cnt, off))


if __name__ == "__main__":
    main()
