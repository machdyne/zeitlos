#
# Zeitlos hwmap -- page layout.
#
# Page 1 is the map. Its regions are fixed, so the picture stays
# recognisable from one run to the next; what goes into them is
# derived from the model:
#
#   +---------------+--------------------------+-------------------+
#   | clocks        | masters -> main arbiter   | other buses       |
#   | interrupts    |    (folded master tree)   | + non-bus blocks  |
#   +---------------+------------+-------------+-------------------+
#   | ================ main bus, decoded on the top nibble ========= |
#   | 0x0 | 0x1 | 0x2 | ...                                   | 0xF |
#   |  one column per nibble: the tenants decoded there             |
#   +---------------------------------------------------------------+
#   | legend                              | check summary           |
#   +---------------------------------------------------------------+
#
# The remaining pages are reference tables flowed into two columns per
# page: the address map in full, interrupts, clocks, pins, the feature
# index (every define and what it controls) and the check findings.
#
# Presence is drawn, not coloured: a solid outline is always present, a
# dashed outline with a define tag is optional, a "one of" box holds
# alternatives, and a dashed box marked "bypass" is wired straight
# through when absent. Colour only encodes the category and nothing is
# lost when printed in monochrome.
#

import re

from . import cond as C
from . import model as M
from .canvas import Canvas, text_width, fit
from .hints import CATEGORIES

PAGE_W, PAGE_H = 841.89, 595.28
MARGIN = 20.0
INK = (0.12, 0.12, 0.14)
GREY = (0.42, 0.42, 0.45)
LIGHT = (0.70, 0.70, 0.72)
TAGC = (0.55, 0.13, 0.10)
BUS = (0.22, 0.27, 0.40)
WARN = (0.70, 0.18, 0.12)


def tag_of(c, parent=C.TRUE):
    return C.tag(c, parent)


def simplify_chain(conds):
    """For members of an `ifdef/`elsif/`else chain, in RTL order: drop the
    '!earlier' literals the chain implies, and call the last catch-all
    'else'."""
    out = []
    seen_pos = set()
    for c in conds:
        lits = {(n, v) for n, v in c if v or n not in seen_pos}
        seen_pos |= {n for n, v in c if v}
        out.append(C.fmt(frozenset(lits), empty="else"))
    return out


def common(cubes):
    if not cubes:
        return C.TRUE
    s = set(cubes[0])
    for c in cubes[1:]:
        s &= set(c)
    return frozenset(s)


# ---------------------------------------------------------------------
# view model
# ---------------------------------------------------------------------

class View:
    def __init__(self, model, hints, findings, groups, info):
        self.m = model
        self.h = hints
        self.findings = findings
        self.groups = groups
        self.info = info
        self.master_of = {}       # block id -> ["main m1", ...]
        self._masters(model.main_tree, "main arbiter")
        for s in model.secondary:
            self._masters(s["tree"], self.name(s["tree"]["blocks"][0])
                          if s["tree"] and s["tree"].get("blocks") else "bus")
        self.columns = self._columns()

    # -- names ---------------------------------------------------------
    def name(self, b, short=True):
        base = self.h.short(b) if short else self.h.name(b)
        same = [x for x in self.m.blocks if x.module == b.module
                and x.inst.name != b.inst.name]
        if same:
            d = re.findall(r"\d+", b.inst.name)
            others = [re.findall(r"\d+", x.inst.name) for x in same]
            if d and all(o != d for o in others):
                if b.module == "spim_wb" or not d:
                    pass
                else:
                    return "%s %s" % (base, d[-1])
        return base

    def pins_line(self, b):
        labels = []
        for g in b.pins:
            if g["kind"] == "clock":
                continue
            lab = self.h.pins(g["label"])
            if lab not in labels:
                labels.append(lab)
        return labels

    def _masters(self, f, arb_name):
        if not f:
            return
        if f["kind"] == "arbiter":
            for i in f["inputs"]:
                nd = i["node"]
                while nd:
                    for b in nd.get("blocks", []):
                        if nd["kind"] == "master":
                            self.master_of.setdefault(b.id, []).append(
                                "%s %s" % (self.h.short(f["blocks"][0]),
                                           i["port"].rstrip("_")))
                    nd = nd.get("up")

    # -- columns -------------------------------------------------------
    def _columns(self):
        m = self.m
        cols = {k: [] for k in range(16)}
        used = set()
        decs = sorted(m.decodes.values(),
                      key=lambda d: (d["windows"][0]["value"], d["line"]))
        for d in decs:
            if d["name"] in used:
                continue
            nib = d["windows"][0]["value"] >> 28
            # decodes of the same window under exclusive conditions form
            # one "one of" entry (SRAM / SDRAM / PSRAM)
            peers = [d]
            for e in decs:
                if e is d or e["name"] in used:
                    continue
                if ([(w["value"], w["mask"]) for w in e["windows"]] ==
                        [(w["value"], w["mask"]) for w in d["windows"]] and
                        all(C.exclusive(a["cond"], b["cond"])
                            for p in peers for a in p["windows"]
                            for b in e["windows"])):
                    peers.append(e)
            for p in peers:
                used.add(p["name"])
            cols[nib].append(self._entry(peers))
        for g in m.ghosts:
            nib = g["value"] >> 28
            cols[nib].append(dict(kind="ghost", name=self.h.short(g["block"]),
                                  cat=self.h.cat(g["block"]),
                                  cond=g["block"].cond, base=g["value"],
                                  note="answered in CPU path",
                                  sort=g["value"], block=g["block"]))
        for v in m.virtual:
            nib = v["value"] >> 28
            cols[nib].append(dict(kind="virtual", name="Virtual window",
                                  cat=self.h.cat(v["block"]), cond=v["block"].cond,
                                  base=v["value"], mask=v["mask"],
                                  note="%s: app address, relocated"
                                  % self.h.short(v["block"]),
                                  sort=v["value"], block=v["block"]))
        for k in cols:
            cols[k].sort(key=lambda e: (e["sort"], e.get("line", 0)))
        return cols

    def _entry(self, decs):
        m = self.m
        members = []
        for d in decs:
            ts = m.tenants.get(d["name"], [])
            resolved = []
            for t in ts:
                b = t["block"]
                via = None
                if b is not None and "arbiter" in b.roles:
                    # a tenant that is an arbiter's input: show what is
                    # behind the arbiter
                    for s in m.secondary:
                        if s["tree"] and b in s["tree"].get("blocks", []):
                            via = b
                            b = s["block"]
                            break
                resolved.append(dict(block=b, via=via, cond=t["cond"],
                                     inline=t["inline"], dec=d))
            # tenants resolving to the same block collapse into one
            merged = []
            for r in resolved:
                for x in merged:
                    if x["block"] is not None and x["block"] is r["block"]:
                        x["conds"].append(r["cond"])
                        if r["via"]:
                            x["via"].append((r["via"], r["cond"]))
                        break
                else:
                    merged.append(dict(block=r["block"], conds=[r["cond"]],
                                       via=[(r["via"], r["cond"])] if r["via"] else [],
                                       inline=r["inline"], dec=d))
            members.extend(merged)
        d0 = decs[0]
        win = d0["windows"]
        entry = dict(kind="alts" if len(members) > 1 else "single",
                     decodes=[d["name"] for d in decs],
                     windows=win, sort=win[0]["value"], line=d0["line"],
                     cover=C.absorb(sum([d["cover"] for d in decs], [])),
                     members=members,
                     absorber=any(w["excludes"] for w in win))
        mc = []
        for mem in members:
            mc.append(common(C.absorb(mem["conds"])) if mem["block"] is None
                      else mem["block"].cond)
        entry["member_tags"] = simplify_chain(mc)
        return entry


# ---------------------------------------------------------------------
# boxes
# ---------------------------------------------------------------------

class Lines:
    """A stack of text lines laid out for a width. A tag pill sits at the
    right of its line when it fits there and drops onto a line of its
    own when it does not; lines marked wrap flow onto several rows
    instead of being truncated."""

    def __init__(self):
        self.items = []

    def add(self, text, size=5.0, font="H", color=INK, right=None,
            rsize=None, rfont="HB", rcolor=TAGC, gap=0.0, wrap=False):
        self.items.append(dict(text=text, size=size, font=font, color=color,
                               right=right, rsize=rsize or size * 0.84,
                               rfont=rfont, rcolor=rcolor, gap=gap, wrap=wrap))

    def natural(self):
        w = 0
        for it in self.items:
            tw = text_width(it["text"], it["size"], it["font"])
            if it["right"]:
                tw += text_width(it["right"], it["rsize"], it["rfont"]) + 6
            w = max(w, tw)
        return w

    def rows(self, w):
        out = []
        for it in self.items:
            pw = (text_width(it["right"], it["rsize"], it["rfont"]) + 3.0
                  if it["right"] else 0)
            tw = text_width(it["text"], it["size"], it["font"]) if it["text"] else 0
            same = it["right"] and it["text"] and tw + pw + 2 <= w
            avail = w - pw - 2 if same else w
            if it["text"]:
                texts = (wrap(it["text"], it["size"], it["font"], avail)
                         if it["wrap"] else [fit(it["text"], it["size"], it["font"], avail)])
            else:
                texts = []
            first = True
            for t in texts:
                out.append(dict(kind="text", it=it, text=t,
                                pill=it["right"] if (same and first) else None,
                                pw=pw, h=it["size"] * 1.22 + (it["gap"] if first else 0),
                                gap=it["gap"] if first else 0))
                first = False
            if it["right"] and not same:
                out.append(dict(kind="pill", it=it, text="", pill=it["right"], pw=pw,
                                h=it["rsize"] * 1.3 + (it["gap"] if not texts else 0) + 0.4,
                                gap=it["gap"] if not texts else 0))
        return out

    def height(self, w):
        return sum(r["h"] for r in self.rows(w))

    def draw(self, cv, x, y, w):
        cy = y
        for r in self.rows(w):
            it = r["it"]
            cy += r["gap"]
            if r["kind"] == "text":
                if r["pill"]:
                    pill(cv, x + w - r["pw"] + 0.5, cy + 0.2, r["pw"] - 0.5,
                         it["size"] * 1.05, r["pill"], it["rsize"], it["rfont"],
                         it["rcolor"])
                cv.text(x, cy + it["size"] * 0.95, r["text"], it["size"], it["font"],
                        it["color"])
                cy += r["h"] - r["gap"]
            else:
                ph = it["rsize"] * 1.3
                pw = min(r["pw"], w)
                pill(cv, x + w - pw + 0.5, cy + 0.2, pw - 0.5, ph, r["pill"],
                     it["rsize"], it["rfont"], it["rcolor"])
                cy += r["h"] - r["gap"]
        return cy - y


def pill(cv, x, y, w, h, text, size, font, color):
    cv.rect(x, y, w, h, fill=(1, 1, 1), stroke=color, lw=0.35, r=h / 2)
    cv.text(x + w / 2, y + h / 2 + size * 0.36, text, size, font, color, "c")


def box(cv, x, y, w, h, cat, optional=False, bypass=False, ghost=False,
        lw=0.7):
    fill, stroke = CATEGORIES.get(cat, CATEGORIES["unknown"])[1:]
    if ghost:
        cv.rect(x, y, w, h, fill=(1, 1, 1), stroke=LIGHT, lw=0.5, dash=(1.2, 1.2),
                r=2)
        return
    dash = (2.2, 1.4) if (optional or bypass) else None
    cv.rect(x, y, w, h, fill=fill, stroke=stroke, lw=lw, dash=dash, r=2.2)


def wrap(text, size, font, width):
    words = text.split(" ")
    lines, cur = [], ""
    for wd in words:
        cand = (cur + " " + wd).strip()
        if text_width(cand, size, font) <= width or not cur:
            cur = cand
        else:
            lines.append(cur)
            cur = wd
    if cur:
        lines.append(cur)
    return lines


# ---------------------------------------------------------------------
# page 1
# ---------------------------------------------------------------------

class MapPage:
    def __init__(self, view):
        self.v = view
        self.m = view.m
        self.h = view.h
        self.cv = Canvas(PAGE_W, PAGE_H)
        self.unplaced = []
        self.placed = set()
        self.squeezed = []
        self.main_drop = None

    def draw(self):
        self.title()
        top = 58.0
        rx = MARGIN + 514.0
        # the panels decide how tall the top region must be: dry-run them
        real = self.cv
        self.cv = Canvas(PAGE_W, PAGE_H)
        lb = self.left_panel(MARGIN, top, 168.0)
        rb = self.right_panel(rx, top, PAGE_W - MARGIN - rx)
        ch = self.centre_height(330.0)
        self.cv = real
        self.placed = set()
        bot = max(lb, rb, top + 14 + ch) + 8
        self.top_bottom = bot
        self.left_panel(MARGIN, top, 168.0)
        self.centre_panel(MARGIN + 176.0, top, 330.0, bot - top)
        self.right_panel(rx, top, PAGE_W - MARGIN - rx)
        self.bus_and_columns(bot + 4.0, 522.0)
        self.bottom(528.0)
        for b in self.m.blocks:
            if (b.id not in self.placed and not b.primitive and
                    "clkgen" not in b.roles):
                self.unplaced.append(b)
        return self.cv

    # -- title -----------------------------------------------------------
    def title(self):
        cv, info = self.cv, self.v.info
        cv.text(MARGIN, MARGIN + 13, "Zeitlos SoC \u2013 hardware map", 15, "HB", INK)
        cv.text(MARGIN, MARGIN + 24,
                "Every optional feature of rtl/sysctl.v, each labelled with the "
                "define that includes it. Not specific to any board.",
                6.6, "H", GREY)
        right = [info.get("rev", ""), info.get("stats", "")]
        for k, s in enumerate(right):
            cv.text(PAGE_W - MARGIN, MARGIN + 9 + k * 9, s, 6.4 if k == 0 else 5.8,
                    "HB" if k == 0 else "H", INK if k == 0 else GREY, "r")
        cv.line([(MARGIN, MARGIN + 30), (PAGE_W - MARGIN, MARGIN + 30)], LIGHT, 0.5)

    def panel_title(self, x, y, text):
        self.cv.text(x, y + 6.5, text.upper(), 6.2, "HB", GREY)

    # -- left panel: clocks + interrupts -----------------------------------
    def left_panel(self, x, y, w):
        cv, m, hn = self.cv, self.m, self.h
        self.panel_title(x, y, "Clocks")
        cy = y + 12
        sysent = max(m.clocks, key=lambda e: len(e["users"])) if m.clocks else None
        col = 36.0
        for e in sorted(m.clocks, key=lambda e: (e is not sysent, -(e["mhz"] or 0))):
            users = []
            for u in e["users"]:
                b = u["block"]
                if "clkgen" in b.roles:
                    continue
                nm = self.v.name(b)
                if nm not in users:
                    users.append(nm)
            if not users:
                continue
            srcs = []
            order = sorted(e["sources"], key=lambda s: (0 if "pin" in s else 1))
            tags = simplify_chain([s["cond"] for s in order])
            for s, t in zip(order, tags):
                if "block" in s:
                    nm = hn.short(s["block"])
                elif "pin" in s:
                    nm = s["pin"] + " pin"
                else:
                    nm = s["logic"]
                srcs.append("%s [%s]" % (nm, t) if t not in ("always", "else") else nm)
            who = ("every bus block" if e is sysent else ", ".join(users))
            if e["mhz"]:
                cv.text(x, cy + 6, "%g MHz" % e["mhz"], 6.2, "HB", INK)
                cv.text(x + col, cy + 6, fit("%s: %s" % (e["net"], who), 5.2, "H",
                                             w - col), 5.2, "H", INK)
            else:
                cv.text(x, cy + 6, fit("%s: %s" % (e["net"], who), 5.8, "HB", w),
                        5.8, "HB", INK)
            cy += 7.4
            for ln in wrap("from " + ", ".join(srcs), 4.6, "H", w - col):
                cv.text(x + col, cy + 4.8, ln, 4.6, "H", GREY)
                cy += 5.7
            cy += 1.8
        cy += 6
        self.panel_title(x, cy, "Interrupts  (CPU irq[n])")
        cy += 13
        for irq in m.irqs:
            srcs = sorted(irq["sources"], key=lambda s: (
                0 if s.get("block") is not None else 1 if s.get("pin") else 2))
            names = []
            for s in srcs:
                if s.get("block") is not None:
                    names.append(self.v.name(s["block"]))
                elif s.get("pin"):
                    names.append(s["pin"] + " pin")
                elif s.get("kind") == "const":
                    names.append("tied off")
                else:
                    names.append("%s (inline logic)" % s.get("logic"))
            tags = simplify_chain([s["cond"] for s in srcs])
            parts = []
            for nm, t in zip(names, tags):
                parts.append(nm if t in ("always", "else", "") else "%s [%s]" % (nm, t))
            cv.text(x, cy + 6, "%d" % irq["n"], 6.4, "HB", INK)
            for ln in wrap(", ".join(parts), 5.2, "H", w - 12):
                cv.text(x + 12, cy + 6, ln, 5.2, "H", INK)
                cy += 6.5
            cy += 1.6
        return cy

    # -- centre panel: main-bus master tree ----------------------------------
    def centre_height(self, w):
        f = self.m.main_tree
        if not f:
            return 0.0
        _, top = self.tree(f, 0.0, 1000.0, w, dry=True)
        return 1000.0 - top

    def centre_panel(self, x, y, w, h):
        self.panel_title(x, y, "Bus masters  \u00bb  main bus")
        f = self.m.main_tree
        if not f:
            return
        self.main_drop = self.tree(f, x, y + h - 2, w)[0]

    def node_lines(self, f, context=None):
        hn = self.h
        L = Lines()
        blocks = f.get("blocks", [])
        if f["kind"] == "arbiter":
            b = blocks[0]
            L.add(hn.name(b), 6.2, "HB", right=tag_of(b.cond) or None)
            L.add("round-robin, %d masters" % len(f["inputs"]), 4.8, "H", GREY)
            if f["bypass"]:
                L.add("absent: %s wired straight through"
                      % f["inputs"][0]["port"].rstrip("_"), 4.6, "H", GREY)
        elif len(blocks) > 1:
            L.add("one of", 4.6, "HB", GREY)
            tags = simplify_chain([b.cond for b in blocks])
            for b, t in zip(blocks, tags):
                L.add(hn.name(b), 6.0, "HB", right=t)
            if context == "secondary" and f["kind"] == "master":
                L.add("direct, untranslated address", 4.6, "H", GREY)
        elif blocks:
            b = blocks[0]
            L.add(hn.name(b), 6.2, "HB", right=tag_of(b.cond) or None)
            extra = []
            if f["kind"] == "master" and f.get("iface") not in ("wbm_", None):
                lab = hn.iface(f["iface"])
                if lab:
                    extra.append(lab)
            if "tenant" in b.roles:
                slot = self.slot_of(b)
                if slot:
                    extra.append("registers at %s" % slot)
            if f["kind"] == "stage":
                extra.append("address stage")
            if f["kind"] == "bridge":
                extra.append("bridge")
            if extra:
                L.add(", ".join(extra), 4.8, "H", GREY, wrap=True)
            if f["bypass"]:
                L.add("absent: wired straight through", 4.6, "H", GREY)
        else:
            L.add(f.get("name") or f["kind"], 5.6, "H")
        return L

    def slot_of(self, b):
        for k, es in self.v.columns.items():
            for e in es:
                for mem in e.get("members", []):
                    if mem["block"] is b:
                        return "0x%X" % k
        return None

    def tree(self, f, x, bottom, w, bw=None, context=None, dry=False):
        """Draw a folded tree whose root's bottom edge is at `bottom`.
        Returns (root centre x, topmost y used)."""
        cv = Canvas(PAGE_W, PAGE_H) if dry else self.cv
        real = self.cv
        self.cv = cv
        try:
            return self._tree(f, x, bottom, w, bw, context)
        finally:
            self.cv = real

    def _tree(self, f, x, bottom, w, bw, context):
        cv = self.cv
        if f["kind"] == "arbiter":
            bw = min(w - 6, 250.0)
        bw = bw or min(w - 6, 132.0)
        L = self.node_lines(f, context)
        hgt = L.height(bw - 8) + 7
        bx = x + (w - bw) / 2
        by = bottom - hgt
        blocks = f.get("blocks", [])
        cat = self.h.cat(blocks[0]) if blocks else "unknown"
        optional = len(blocks) == 1 and bool(blocks[0].cond)
        box(cv, bx, by, bw, hgt, cat, optional=optional or f["bypass"],
            bypass=f["bypass"])
        if len(blocks) > 1:
            cv.rect(bx + 1.2, by + 1.2, bw - 2.4, hgt - 2.4,
                    stroke=CATEGORIES.get(cat, CATEGORIES["unknown"])[2], lw=0.3, r=1.6)
        L.draw(cv, bx + 4, by + 3.5, bw - 8)
        if cv is self.cv:
            for b in blocks:
                self.placed.add(b.id)
        cx = bx + bw / 2
        top = by
        gap = 14.0
        if f["kind"] == "arbiter":
            n = len(f["inputs"])
            cw = w / max(n, 1)
            for k, i in enumerate(f["inputs"]):
                if not i["node"]:
                    continue
                ix = x + k * cw
                ccx, ctop = self._tree(i["node"], ix, by - gap, cw,
                                       min(cw - 8, 124.0), context)
                top = min(top, ctop)
                tx = bx + bw * (k + 0.5) / n
                cv.line([(ccx, by - gap), (ccx, by - gap / 2), (tx, by - gap / 2),
                         (tx, by)], BUS, 1.1, arrow=True)
                cv.text(tx + 2.5, by - 2.0, i["port"].rstrip("_"), 4.6, "HB", GREY)
        elif f["kind"] in ("bridge", "stage") and f.get("up"):
            ccx, ctop = self._tree(f["up"], x, by - gap, w, bw, context)
            top = min(top, ctop)
            cv.line([(ccx, by - gap), (cx, by)], BUS, 1.1, arrow=True)
        return cx, top

    # -- right panel: other buses and non-bus blocks ---------------------------
    def right_panel(self, x, y, w):
        cv, m, hn, v = self.cv, self.m, self.h, self.v
        self.panel_title(x, y, "Other buses and sideband blocks")
        cy = y + 14
        pos = {}
        for s in m.secondary:
            tgt, tree = s["block"], s["tree"]
            _, top = self.tree(tree, x, 1000.0, w, context="secondary", dry=True)
            bottom = cy + (1000.0 - top)
            cxr, _ = self.tree(tree, x, bottom, w, context="secondary")
            L = Lines()
            L.add(hn.name(tgt), 6.4, "HB", right=tag_of(tgt.cond) or None)
            slot = self.slot_of(tgt)
            if slot:
                L.add("also the CPU's %s window" % slot, 4.8, "H", GREY)
            for lab, c in self.pin_labels(tgt):
                L.add("pins " + lab, 4.8, "H", INK, right=tag_of(c, tgt.cond) or None)
            tw = 128.0
            th = L.height(tw - 8) + 7
            tx = cxr - tw / 2
            ty = bottom + 12
            box(cv, tx, ty, tw, th, hn.cat(tgt), optional=bool(tgt.cond))
            L.draw(cv, tx + 4, ty + 3.5, tw - 8)
            cv.line([(cxr, bottom), (cxr, ty)], BUS, 1.1, arrow=True)
            self.placed.add(tgt.id)
            pos[tgt.id] = (tx, ty, tw, th)
            cy = ty + th + 22
        loose = [b for b in m.blocks if not b.ifaces and not b.primitive
                 and "clkgen" not in b.roles]
        if not loose:
            return cy
        order = []
        frontier = list(pos)
        while frontier:
            nxt = []
            for bid in frontier:
                for l in m.links:
                    for a, b in ((l["src"], l["dst"]), (l["dst"], l["src"])):
                        if a.id == bid and b in loose and b not in order:
                            order.append(b)
                            nxt.append(b.id)
            frontier = nxt
        order += [b for b in loose if b not in order]
        gapx = 30.0
        n = len(order)
        bw = min(132.0, (w - gapx * (n - 1)) / max(n, 1))
        row_x = x + (w - (n * bw + (n - 1) * gapx)) / 2
        for k, b in enumerate(order):
            L = Lines()
            L.add(hn.name(b), 6.4, "HB", right=tag_of(b.cond) or None)
            for c in b.children:
                lab = hn.child(c[0])
                if lab:
                    L.add(lab, 4.8, "H", GREY, right=tag_of(c[2], b.cond) or None)
            ins = []
            for l in m.links:
                if l["dst"] is b and l["src"].id not in pos and l["src"] not in order:
                    nm = v.name(l["src"])
                    if nm not in ins:
                        ins.append(nm)
            if ins:
                L.add("controlled by " + ", ".join(ins), 4.6, "H", GREY, wrap=True)
            for lab, c in self.pin_labels(b):
                L.add("pins " + lab, 4.8, "H", INK, right=tag_of(c, b.cond) or None,
                      gap=0.4)
            bh = L.height(bw - 8) + 7
            bx = row_x + k * (bw + gapx)
            box(cv, bx, cy, bw, bh, hn.cat(b), optional=bool(b.cond))
            L.draw(cv, bx + 4, cy + 3.5, bw - 8)
            pos[b.id] = (bx, cy, bw, bh)
            self.placed.add(b.id)
        self.sideband(pos)
        return max(p[1] + p[3] for p in pos.values())

    def pin_labels(self, b):
        """Pin groups of a block merged by display label; the tag is what
        all variants of that label have in common."""
        acc = []
        for g in b.pins:
            if g["kind"] == "clock":
                continue
            lab = self.h.pins(g["label"])
            for ent in acc:
                if ent[0] == lab:
                    ent[1].append(g["cond"])
                    break
            else:
                acc.append((lab, [g["cond"]]))
        return [(lab, common(cs)) for lab, cs in acc]

    def sideband(self, pos):
        cv, m = self.cv, self.m
        drawn = {}
        for l in m.links:
            a, b = l["src"].id, l["dst"].id
            if a not in pos or b not in pos:
                continue
            key = tuple(sorted((a, b)))
            ent = drawn.setdefault(key, dict(fwd=set(), sigs=[]))
            ent["fwd"].add(a)
            for sg in l["signals"]:
                nm = re.sub(r"_(i|o)$", "", sg.split(">")[0])
                if nm not in ent["sigs"]:
                    ent["sigs"].append(nm)
        side_slots = {}
        for (a, b), ent in sorted(drawn.items()):
            pa, pb = pos[a], pos[b]
            upper, lower = (pa, pb) if pa[1] < pb[1] - 4 else ((pb, pa) if pb[1] < pa[1] - 4 else (None, None))
            label = "/".join(ent["sigs"][:3]) + ("/..." if len(ent["sigs"]) > 3 else "")
            if upper is not None:
                ua, la = (a, b) if upper is pa else (b, a)
                x0 = upper[0] + upper[2] / 2
                y0 = upper[1] + upper[3]
                x1 = lower[0] + lower[2] / 2
                y1 = lower[1]
                ym = y1 - 9
                pts = [(x0, y0), (x0, ym), (x1, ym), (x1, y1)] if abs(x0 - x1) > 1 else [(x0, y0), (x1, y1)]
                cv.line(pts, GREY, 0.8, dash=(2.2, 1.3),
                        arrow=la in ent["fwd"] and ua in ent["fwd"] or ua in ent["fwd"],
                        arrow_start=la in ent["fwd"])
                cv.text(max(x0, x1) + 3, ym + (y0 - ym) / 2 + 2 if abs(x0 - x1) <= 1 else ym - 1.5,
                        label, 4.4, "H", GREY)
            else:
                left, right = (pa, pb) if pa[0] < pb[0] else (pb, pa)
                la, ra = (a, b) if left is pa else (b, a)
                k = side_slots.get((la, ra), 0)
                side_slots[(la, ra)] = k + 1
                yy = max(left[1], right[1]) + 12 + k * 9
                p0 = (left[0] + left[2], yy)
                p1 = (right[0], yy)
                cv.line([p0, p1], GREY, 0.8, dash=(2.2, 1.3),
                        arrow=la in ent["fwd"], arrow_start=ra in ent["fwd"])
                cv.text((p0[0] + p1[0]) / 2, yy - 1.8, fit(label, 4.2, "H", p1[0] - p0[0] + 20),
                        4.2, "H", GREY, "c")

    # -- bus bar and address columns ------------------------------------------
    def column_widths(self, total, gap):
        want = []
        for k in range(16):
            es = self.v.columns[k]
            nat = 30.0
            for e in es:
                nat = max(nat, self.entry_lines(e).natural() + 7)
            want.append(min(max(nat, 36.0), 80.0))
        avail = total - 15 * gap
        s = sum(want)
        if s <= avail:
            extra = avail - s
            return [wd + extra * wd / s for wd in want]
        # shrink the widest first, never below the floor
        floor = 34.0
        widths = list(want)
        while sum(widths) > avail + 0.01:
            over = sum(widths) - avail
            big = [i for i, wd in enumerate(widths) if wd > floor + 0.01]
            if not big:
                break
            top_w = max(widths[i] for i in big)
            second = max([widths[i] for i in big if widths[i] < top_w - 0.01] + [floor])
            cand = [i for i in big if widths[i] >= top_w - 0.01]
            cut = min(over / len(cand), top_w - second)
            for i in cand:
                widths[i] -= cut
        return widths

    def bus_and_columns(self, y0, y1):
        cv, m = self.cv, self.m
        x0, x1 = MARGIN, PAGE_W - MARGIN
        bar_h = 11.0
        cv.rect(x0, y0, x1 - x0, bar_h, fill=BUS, stroke=None, r=2)
        cv.text(x0 + 6, y0 + 7.8, "MAIN WISHBONE BUS  \u2013  %s, decoded on the top "
                "address nibble" % (m.main_bus or "?"), 6.0, "HB", (1, 1, 1))
        cv.text(x1 - 6, y0 + 7.8, "each slave sits in the column of its base address",
                5.0, "H", (0.85, 0.87, 0.93), "r")
        if self.main_drop is not None:
            cv.line([(self.main_drop, y0 - 6.5), (self.main_drop, y0)], BUS, 1.6,
                    arrow=2.6)
        gap = 3.0
        widths = self.column_widths(x1 - x0, gap)
        top = y0 + bar_h + 4
        cx = x0
        for k in range(16):
            cw = widths[k]
            cv.line([(cx + cw / 2, y0 + bar_h), (cx + cw / 2, top + 1)], BUS, 0.9)
            cv.rect(cx, top + 1, cw, 12, fill=(0.93, 0.94, 0.97), stroke=None, r=1.5)
            cv.text(cx + 3, top + 9.6, "0x%X" % k, 7.0, "HB", BUS)
            if cw > 44:
                cv.text(cx + cw - 2.5, top + 9.2, "%X000_0000" % k, 4.2, "C", GREY, "r")
            self.column(k, cx, top + 16, cw, y1 - (top + 16))
            cx += cw + gap

    def entry_lines(self, e):
        hn, v = self.h, self.v
        L = Lines()
        if e["kind"] in ("ghost", "virtual"):
            L.add(e["name"], 5.4, "HB", GREY, right=tag_of(e["cond"]) or None)
            L.add(M.hexs(e["base"]), 4.4, "C", GREY)
            L.add(e["note"], 4.4, "H", GREY, wrap=True)
            return L
        members, tags = e["members"], e["member_tags"]
        base = common(e["cover"])
        if e["kind"] == "alts":
            L.add("one of", 4.4, "HB", GREY, right=tag_of(base) or None)
            for mem, t in zip(members, tags):
                nm = v.name(mem["block"]) if mem["block"] else self.inline_name(mem)
                L.add(nm, 5.4, "HB", right=t if t != "always" else None)
        else:
            mem = members[0] if members else None
            nm = (v.name(mem["block"]) if mem and mem["block"] else
                  self.inline_name(mem) if mem else e["decodes"][0])
            L.add(nm, 5.6, "HB", right=tag_of(base) or None, wrap=True)
        for wdw in e["windows"]:
            sz = M.window_size(wdw["mask"])
            desc = M.size_str(sz) if sz else "/%X" % (wdw["mask"] & 0x0FFFFFFF)
            wt = tag_of(wdw["cond"], base) if len(e["windows"]) > 1 else ""
            L.add("%s %s" % (M.hexs(wdw["value"]), desc), 4.3, "C", INK,
                  right=wt or None, rsize=3.9)
        if e["absorber"]:
            L.add("rest of the 0x%X window" % (e["windows"][0]["value"] >> 28),
                  4.3, "H", GREY, wrap=True)
        notes = []
        for mem in members:
            b = mem["block"]
            if b is None:
                continue
            for via, c in mem["via"]:
                notes.append(("through %s" % hn.short(via), tag_of(c, via.cond | base)))
            if b.irqs:
                notes.append(("IRQ %s" % ",".join(str(i) for i in sorted(set(b.irqs))), None))
            for mo in v.master_of.get(b.id, []):
                notes.append(("master on %s" % mo, None))
            for l in self.m.links:
                if l["src"] is b and "tenant" in l["dst"].roles:
                    notes.append(("feeds %s" % v.name(l["dst"]), None))
            if e["kind"] == "single":
                for p in hn.params(b, self.m.defaults):
                    notes.append((p, None))
                labs = self.pin_labels(b)
                if len(labs) > 2:
                    notes.append(("pins " + ", ".join(lab for lab, _ in labs), None))
                else:
                    for lab, c in labs:
                        notes.append(("pins " + lab, tag_of(c, b.cond) or None))
        seen = []
        for n_, t in notes:
            if (n_, t) in seen:
                continue
            seen.append((n_, t))
            L.add(n_, 4.4, "H", GREY, right=t or None, rsize=3.8, wrap=True)
        return L

    def inline_name(self, mem):
        dn = mem["dec"]["name"] if mem else "?"
        return self.h.inline(dn)

    def column(self, k, x, y, w, h):
        cv, hn = self.cv, self.h
        es = self.v.columns[k]
        if not es:
            cv.text(x + w / 2, y + 12, "unused", 4.6, "H", LIGHT, "c")
            return
        blocks = [(e, self.entry_lines(e)) for e in es]
        gap = 3.0
        pad = 5.5
        need = sum(L.height(w - 6) + pad for _, L in blocks) + gap * (len(blocks) - 1)
        if need > h:
            for e, L in sorted(blocks, key=lambda p: -p[1].height(w - 6)):
                while need > h and len(L.items) > 2:
                    before = L.height(w - 6)
                    L.items.pop()
                    need -= before - L.height(w - 6)
                    self.squeezed.append("0x%X" % k)
        cy = y
        for e, L in blocks:
            bh = L.height(w - 6) + pad
            if e["kind"] in ("ghost", "virtual"):
                box(cv, x, cy, w, bh, e["cat"], ghost=True)
            else:
                cat = "unknown"
                for mem in e["members"]:
                    if mem["block"] is not None:
                        cat = hn.cat(mem["block"])
                        break
                    cat = hn.inline_cat(mem["dec"]["name"])
                box(cv, x, cy, w, bh, cat, optional=bool(common(e["cover"])))
                if e["kind"] == "alts":
                    cv.rect(x + 1.2, cy + 1.2, w - 2.4, bh - 2.4,
                            stroke=CATEGORIES.get(cat, CATEGORIES["unknown"])[2],
                            lw=0.3, r=1.6)
                for mem in e["members"]:
                    if mem["block"] is not None:
                        self.placed.add(mem["block"].id)
            L.draw(cv, x + 3, cy + 2.7, w - 6)
            cy += bh + gap

    # -- bottom band -------------------------------------------------------
    def bottom(self, y):
        cv, info = self.cv, self.v.info
        x = MARGIN
        cv.line([(MARGIN, y - 1), (PAGE_W - MARGIN, y - 1)], LIGHT, 0.5)
        self.panel_title(x, y + 2, "How to read")
        ly = y + 12
        cx = x
        for kind, text in (("solid", "always present"),
                           ("dashed", "optional: present when TAG is defined"),
                           ("alts", "one of: the first matching tag wins"),
                           ("bypass", "bypassable: wired straight through when absent")):
            if kind == "solid":
                box(cv, cx, ly, 16, 9, "sys")
            elif kind == "dashed":
                box(cv, cx, ly, 16, 9, "sys", optional=True)
                pill(cv, cx + 18, ly + 1.5, 16, 6, "TAG", 3.8, "HB", TAGC)
                cx += 18
            elif kind == "alts":
                box(cv, cx, ly, 16, 9, "sys")
                cv.rect(cx + 1.2, ly + 1.2, 13.6, 6.6, stroke=CATEGORIES["sys"][2],
                        lw=0.3, r=1.4)
            else:
                box(cv, cx, ly, 16, 9, "sys", bypass=True)
            cv.text(cx + 20, ly + 6.6, text, 5.4, "H", INK)
            cx += 20 + text_width(text, 5.4, "H") + 11
        ly += 12.5
        cx = x
        for style, text in (("bus", "Wishbone, initiator to target"),
                            ("side", "sideband signals"),
                            ("ghost", "register window answered off the bus")):
            if style == "bus":
                cv.line([(cx, ly + 4), (cx + 18, ly + 4)], BUS, 1.1, arrow=True)
            elif style == "side":
                cv.line([(cx, ly + 4), (cx + 18, ly + 4)], GREY, 0.8, dash=(2.2, 1.3),
                        arrow=True)
            else:
                box(cv, cx + 1, ly, 16, 9, "sys", ghost=True)
            cv.text(cx + 22, ly + 6, text, 5.4, "H", INK)
            cx += 22 + text_width(text, 5.4, "H") + 11
        ly += 12.5
        cx = x
        for key, (label, fill, stroke) in CATEGORIES.items():
            if key == "unknown":
                continue
            cv.rect(cx, ly, 9, 8, fill=fill, stroke=stroke, lw=0.5, r=1.5)
            cv.text(cx + 11, ly + 6, label, 5.2, "H", INK)
            cx += 11 + text_width(label, 5.2, "H") + 8
        # check summary
        sx = PAGE_W - MARGIN - 300
        cv.line([(sx - 10, y + 2), (sx - 10, y + 44)], LIGHT, 0.5)
        cnt = {"E": 0, "W": 0, "N": 0}
        for g in self.v.groups:
            cnt[g["severity"]] += 1
        self.panel_title(sx, y + 2, "hwmap check")
        cv.text(sx + 52, y + 8.5,
                "%s: %d break the build or the bus, %d leave signals undriven, "
                "%d notes (last page)" % (
                    plural(len(self.v.groups), "define combination"), cnt["E"],
                    cnt["W"], cnt["N"]), 4.9, "H", GREY)
        sy = y + 17.5
        shown = 0
        errs = [g for g in self.v.groups if g["severity"] == "E"]
        for g in errs[:4]:
            it = g["items"][0]
            cv.text(sx, sy, fit(C.fmt(g["when"]), 5.0, "HB", 92), 5.0, "HB", WARN)
            cv.text(sx + 95, sy, fit(it["msg"], 5.0, "H", 205), 5.0, "H", INK)
            sy += 6.3
            shown += 1
        if len(errs) > shown:
            cv.text(sx + 95, sy, "and %d more" % (len(errs) - shown), 5.0, "H", GREY)
        cv.text(MARGIN, PAGE_H - 11,
                "Generated by tools/hwmap from %s. Re-run `make hwmap` after changing "
                "the RTL; see docs/hwmap.md." % info.get("files", "rtl/"),
                5.0, "H", GREY)
        cv.text(PAGE_W - MARGIN, PAGE_H - 11, "page 1", 5.0, "H", GREY, "r")


def plural(n, word):
    return "%d %s%s" % (n, word, "" if n == 1 else "s")


# ---------------------------------------------------------------------
# reference pages
# ---------------------------------------------------------------------

class Flow:
    """Two-column text flow across as many pages as needed."""

    def __init__(self, title, first_page_no):
        self.pages = []
        self.title = title
        self.page_no = first_page_no
        self.col_w = (PAGE_W - 2 * MARGIN - 18) / 2
        self.top = MARGIN + 36
        self.bottom = PAGE_H - MARGIN - 12
        self._new_page()

    def _new_page(self):
        cv = Canvas(PAGE_W, PAGE_H)
        cv.text(MARGIN, MARGIN + 13, self.title, 12, "HB", INK)
        cv.line([(MARGIN, MARGIN + 22), (PAGE_W - MARGIN, MARGIN + 22)], LIGHT, 0.5)
        cv.text(PAGE_W - MARGIN, PAGE_H - 12, "page %d" % (self.page_no + len(self.pages)),
                5.0, "H", GREY, "r")
        self.pages.append(cv)
        self.cv = cv
        self.col = 0
        self.y = self.top

    def x(self):
        return MARGIN + self.col * (self.col_w + 18)

    def need(self, h):
        if self.y + h > self.bottom:
            if self.col == 0:
                self.col = 1
                self.y = self.top
            else:
                self._new_page()

    def heading(self, text, note=None):
        self.need(40)
        if self.y > self.top:
            self.y += 8
        self.cv.text(self.x(), self.y + 8, text, 8.4, "HB", BUS)
        self.y += 11
        if note:
            for ln in wrap(note, 5.4, "H", self.col_w):
                self.cv.text(self.x(), self.y + 5.5, ln, 5.4, "H", GREY)
                self.y += 6.8
        self.y += 2

    def table(self, cols, rows, size=5.4):
        """cols: [(title, width fraction, font)]; rows: [[text | (text, font,
        color)]]. Cells wrap; a header repeats after a column break."""
        widths = [f * self.col_w for _, f, _ in cols]

        def header():
            cx = self.x()
            for (t, _, _), wd in zip(cols, widths):
                self.cv.text(cx, self.y + 5.6, t, size * 0.92, "HB", GREY)
                cx += wd
            self.y += 7.2
            self.cv.line([(self.x(), self.y - 0.8), (self.x() + self.col_w, self.y - 0.8)],
                         LIGHT, 0.4)
        self.need(20)
        header()
        for row in rows:
            cells = []
            hmax = 1
            for (t, _, font), wd, cell in zip(cols, widths, row):
                if isinstance(cell, tuple):
                    text, cf, color = cell
                else:
                    text, cf, color = cell, font, INK
                lines = wrap(text or "", size, cf, wd - 4) if text else [""]
                cells.append((lines, cf, color))
                hmax = max(hmax, len(lines))
            h = hmax * size * 1.22 + 1.6
            col_before, page_before = self.col, len(self.pages)
            self.need(h)
            if self.col != col_before or len(self.pages) != page_before:
                header()
            cx = self.x()
            for (lines, cf, color), wd in zip(cells, widths):
                for k, ln in enumerate(lines):
                    self.cv.text(cx, self.y + size + k * size * 1.22, ln, size, cf, color)
                cx += wd
            self.y += h


def reference_pages(view, first_page_no=2):
    m, hn, v = view.m, view.h, view
    fl = Flow("Zeitlos SoC \u2013 hardware map reference", first_page_no)

    # address map ---------------------------------------------------------
    fl.heading("Address map",
               "Every decoded window on the main bus, the slaves that answer it and "
               "the condition under which each is present. Line numbers are in "
               "rtl/sysctl.v.")
    rows = []
    for k in range(16):
        for e in v.columns[k]:
            if e["kind"] in ("ghost", "virtual"):
                rows.append([("%s" % M.hexs(e["base"]), "C", GREY),
                             (e.get("mask") and "/%X" % e["mask"] or "", "C", GREY),
                             ("%s \u2013 %s" % (e["name"], e["note"]), "H", GREY),
                             (C.fmt(e["cond"]), "H", GREY), ""])
                continue
            for wdw in e["windows"]:
                sz = M.window_size(wdw["mask"])
                names = []
                for mem, t in zip(e["members"], e["member_tags"]):
                    nm = hn.name(mem["block"]) if mem["block"] else view_inline(mem)
                    if mem["block"] is not None:
                        nm += " (%s)" % mem["block"].module
                    if len(e["members"]) > 1:
                        nm += " [%s]" % t
                    for via, c in mem["via"]:
                        nm += " via %s [%s]" % (hn.name(via), C.fmt(c))
                    names.append(nm)
                rows.append([(M.hexs(wdw["value"]), "C", INK),
                             ((M.size_str(sz) if sz else "/%X" % wdw["mask"]), "C", INK),
                             "; ".join(names) + (" (rest of window)" if wdw["excludes"] else ""),
                             C.fmt(wdw["cond"]), "%d" % wdw["line"]])
    fl.table([("Base", 0.15, "C"), ("Size/mask", 0.12, "C"), ("Slave", 0.43, "H"),
              ("Present when", 0.22, "H"), ("Line", 0.08, "H")], rows)

    # interrupts ------------------------------------------------------------
    fl.heading("Interrupts")
    rows = []
    for irq in m.irqs:
        srcs = []
        for s in irq["sources"]:
            if s.get("block") is not None:
                nm = "%s (%s.%s)" % (hn.name(s["block"]), s["block"].inst.name, s["port"])
            elif s.get("pin"):
                nm = "pin %s" % s["pin"]
            else:
                nm = "%s (%s)" % (s.get("logic"), s.get("kind"))
            srcs.append("%s [%s]" % (nm, C.fmt(s["cond"])))
        rows.append(["irq[%d]" % irq["n"], "; ".join(srcs), C.fmt(irq["cond"]),
                     "%d" % irq["line"]])
    fl.table([("IRQ", 0.1, "HB"), ("Source [present when]", 0.6, "H"),
              ("Assigned when", 0.22, "H"), ("Line", 0.08, "H")], rows)

    # clocks ------------------------------------------------------------------
    fl.heading("Clocks")
    rows = []
    for e in m.clocks:
        users = []
        for u in e["users"]:
            if "clkgen" in u["block"].roles:
                continue
            nm = v.name(u["block"])
            if nm not in users:
                users.append(nm)
        srcs = []
        for s in e["sources"]:
            nm = (hn.name(s["block"]) + " ." + s["port"]) if "block" in s else (
                "pin " + s["pin"] if "pin" in s else s["logic"])
            srcs.append("%s [%s]" % (nm, C.fmt(s["cond"])))
        rows.append([("%g MHz" % e["mhz"]) if e["mhz"] else "?", e["net"],
                     "; ".join(srcs), ", ".join(users)])
    fl.table([("Freq", 0.1, "HB"), ("Net", 0.14, "C"), ("Source [present when]", 0.42, "H"),
              ("Used by", 0.34, "H")], rows)

    # pins ----------------------------------------------------------------------
    fl.heading("External interfaces",
               "Top-level ports of sysctl, grouped by the block they connect to.")
    rows = []
    for g in m.pin_groups:
        who = (hn.name(g["block"]) + " (" + g["block"].inst.name + ")") if g["block"] else (
            "clock input" if g["kind"] == "clock" else
            "unused" if g["kind"] == "unused" else "board glue")
        rows.append([hn.pins(g["label"]), ", ".join(p.name for p in g["ports"]), who,
                     C.fmt(g["cond"])])
    fl.table([("Interface", 0.16, "HB"), ("Ports", 0.36, "C"), ("Block", 0.26, "H"),
              ("Present when", 0.22, "H")], rows, size=5.0)

    # blocks ------------------------------------------------------------------------
    fl.heading("Blocks", "Every module instantiated in sysctl, with its role and "
               "documentation.")
    rows = []
    for b in m.blocks:
        roles = ", ".join(sorted(b.roles)) or ("primitive" if b.primitive else "")
        rows.append([hn.name(b), (b.inst.name, "C", INK), b.module, roles,
                     C.fmt(b.cond), hn.doc(b) or "", "%d" % b.line])
    fl.table([("Block", 0.17, "HB"), ("Instance", 0.17, "C"), ("Module", 0.15, "H"),
              ("Role", 0.14, "H"), ("Present when", 0.17, "H"), ("Doc", 0.14, "H"),
              ("Line", 0.06, "H")], rows, size=4.8)

    # feature index -----------------------------------------------------------------
    fl.heading("Feature index",
               "Every define that sysctl.v's structure depends on and what it "
               "controls. 'selects' means each branch of an `ifdef/`else gives a "
               "different version; 'without it' lists what exists only when the "
               "define is absent. Value defines show their built-in default.")
    rows = []

    def tidy(lst):
        # "localparam X" says less than "X bit N"; keep the specific one
        bits = {u.split(" bit ")[0] for u in lst if " bit " in u}
        return [u for u in lst if not (u.startswith("localparam ") and
                                       u[len("localparam "):] in bits)]
    for name in sorted(m.features):
        ent = m.features[name]
        both = [u for u in ent["with"] if u in ent["without"]]
        w = tidy([u for u in ent["with"] if u not in both])
        wo = tidy([u for u in ent["without"] if u not in both])
        parts = []
        if w:
            parts.append("; ".join(w))
        if both:
            parts.append("selects: " + "; ".join(both))
        if wo:
            parts.append("without it: " + "; ".join(wo))
        dv = m.defaults.get(name)
        rows.append([(name, "HB", INK), dv[0] if dv else "", ". ".join(parts)])
    fl.table([("Define", 0.24, "HB"), ("Default", 0.12, "C"), ("Controls", 0.64, "H")],
             rows, size=5.0)

    # findings ------------------------------------------------------------------------
    fl.heading("hwmap check: combinations that would not work",
               "Each entry names a combination of defines under which the RTL is "
               "inconsistent. hwmap never evaluates the board blocks in "
               "rtl/boards.vh, so it cannot say whether a board uses one; most "
               "are traps for the next board or feature. E = would not build, or hangs the bus; "
               "W = a signal left undriven; N = note.")
    rows = []
    for g in v.groups:
        first = True
        for it in g["items"]:
            rows.append([(C.fmt(g["when"]) if first else "", "HB", WARN),
                         it["severity"], it["msg"],
                         "%s:%s" % (it["file"], it["line"]) if it["file"] else ""])
            first = False
    if not rows:
        rows.append(["", "", "no findings", ""])
    fl.table([("When", 0.25, "HB"), ("", 0.04, "HB"), ("Finding", 0.53, "H"),
              ("Where", 0.18, "C")], rows, size=5.0)
    return fl.pages


def view_inline(mem):
    return "%s (inline logic)" % re.sub(r"^cs_", "", mem["dec"]["name"])
