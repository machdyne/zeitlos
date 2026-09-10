#
# Zeitlos hwmap -- from nets to a system model.
#
# Everything here is derived from the conventions rtl/sysctl.v already
# follows (docs/hwmap.md lists them). None of it comes from a list of
# known peripherals: lib/hints.py only supplies display names.
#
#   interfaces   ports grouped by prefix: <p>cyc_i + <p>ack_o is a
#                Wishbone TARGET, <p>cyc_o + <p>ack_i an INITIATOR
#   decodes      wires of the form  (net & MASK) == VALUE  or
#                net[hi:lo] == VALUE, optionally && !other_decode ...
#   buses        the address net those decodes compare
#   tenants      the data/ack mux terms  ({N{cs}} & dat)  and  (cs & ack);
#                the ack signal is traced back to whoever drives it
#   masters      traced back from a bus (or a target) through arbiters
#                (several target ports, one initiator), bridges (one of
#                each), and address stages (an instance whose non-bus
#                output feeds an address, e.g. the MTU)
#   interrupts   assignments to  <irq-net>[N]  where <irq-net> is wired
#                to an instance port called irq
#

import re

from . import cond as C
from . import vparse as V
from . import nets as NETS

SUFFIX = re.compile(r"^(.*?)(adr|dat|we|sel|stb|cyc|ack)_(i|o)$")
CLKNET = re.compile(r"clk", re.I)
FREQ = re.compile(r"(\d+)(?:_(\d+))?\s*mhz", re.I)


def merge_terms(terms):
    """[(terminal, cond)] -> [(terminal, [cubes])] with equal terminals
    merged and their conditions minimised."""
    acc = {}
    order = []
    for t, c in terms:
        k = tkey(t)
        if k not in acc:
            acc[k] = (t, [])
            order.append(k)
        acc[k][1].append(c)
    return [(acc[k][0], C.absorb(acc[k][1])) for k in order]


def tkey(t):
    if t[0] == "inst":
        return ("inst", t[1].name, t[1].line, t[2])
    return t


def cover_fmt(cubes):
    if not cubes:
        return "never"
    if any(len(c) == 0 for c in cubes):
        return "always"
    return " | ".join(C.fmt(c) for c in cubes)


def one_cube(cubes):
    """Best single cube for a cover (for tags): the common literals."""
    if not cubes:
        return C.TRUE
    common = set(cubes[0])
    for c in cubes[1:]:
        common &= c
    return frozenset(common)


def freq_of(name):
    m = FREQ.search(name)
    if m:
        v = float(m.group(1) + ("." + m.group(2) if m.group(2) else ""))
        return v
    m = re.match(r"^CLK_?(\d+)$", name, re.I)
    if m:
        return float(m.group(1))
    return None


def hexs(v):
    return "%04X_%04X" % ((v >> 16) & 0xFFFF, v & 0xFFFF)


def window_size(mask):
    """Bytes covered by a mask that is contiguous from bit 31, else None."""
    inv = (~mask) & 0xFFFFFFFF
    if inv & (inv + 1) == 0:
        return inv + 1
    return None


def size_str(n):
    if n is None:
        return ""
    for unit, sh in (("G", 30), ("M", 20), ("K", 10)):
        if n >= (1 << sh) and n % (1 << sh) == 0:
            return "%d%s" % (n >> sh, unit)
    return "%d" % n


# ---------------------------------------------------------------------
# expression shapes
# ---------------------------------------------------------------------

def strip_parens(toks):
    while (len(toks) >= 2 and toks[0].text == "(" and toks[-1].text == ")"):
        depth = 0
        ok = True
        for k, t in enumerate(toks):
            if t.kind == "op" and t.text in "([{":
                depth += 1
            elif t.kind == "op" and t.text in ")]}":
                depth -= 1
                if depth == 0 and k != len(toks) - 1:
                    ok = False
                    break
        if not ok:
            break
        toks = toks[1:-1]
    return toks


def split_top(toks, op):
    parts, cur, depth = [], [], 0
    for t in toks:
        if t.kind == "op":
            if t.text in "([{":
                depth += 1
            elif t.text in ")]}":
                depth -= 1
            elif t.text == op and depth == 0:
                parts.append(cur)
                cur = []
                continue
        cur.append(t)
    parts.append(cur)
    return parts


def parse_compare(toks):
    """(net & M) == V | net[h:l] == V | net == V  ->  (net, mask, value)."""
    toks = strip_parens(toks)
    parts = split_top(toks, "==")
    if len(parts) != 2:
        return None
    lhs, rhs = strip_parens(parts[0]), strip_parens(parts[1])
    if len(rhs) != 1 or rhs[0].kind != "num":
        return None
    val = V.parse_number(rhs[0].text)
    if val is None:
        return None
    ands = split_top(lhs, "&")
    if len(ands) == 2:
        a, b = strip_parens(ands[0]), strip_parens(ands[1])
        if len(b) == 1 and b[0].kind == "num" and len(a) == 1 and a[0].kind == "id":
            mask = V.parse_number(b[0].text)
            if mask is None:
                return None
            return (a[0].text, mask & 0xFFFFFFFF, val & 0xFFFFFFFF)
        return None
    if len(lhs) == 1 and lhs[0].kind == "id":
        return (lhs[0].text, 0xFFFFFFFF, val)
    if (len(lhs) == 6 and lhs[0].kind == "id" and lhs[1].text == "[" and
            lhs[3].text == ":" and lhs[5].text == "]"):
        hi, lo = V.parse_number(lhs[2].text), V.parse_number(lhs[4].text)
        if hi is None or lo is None:
            return None
        mask = ((1 << (hi + 1)) - 1) & ~((1 << lo) - 1)
        return (lhs[0].text, mask & 0xFFFFFFFF, (val << lo) & 0xFFFFFFFF)
    return None


# ---------------------------------------------------------------------
# the model
# ---------------------------------------------------------------------

class Block:
    def __init__(self, inst, bid, design):
        self.inst = inst
        self.id = bid
        self.module = inst.module
        self.cond = inst.cond
        self.file, self.line = inst.file, inst.line
        self.primitive = design.is_primitive(inst.module)
        self.ifaces = {}          # prefix -> dict(role, ports)
        self.children = []        # (module, name, cond) inside the module
        self.params = {}          # name -> (text, cond)
        self.roles = set()        # tenant master arbiter bridge stage clkgen video
        self.pins = []
        self.irqs = []
        self.alt_group = None
        self.placed = False


class Model:
    def __init__(self, design, hints):
        self.d = design
        self.h = hints
        self.n = NETS.Nets(design)
        self.blocks = []
        self.by_inst = {}
        self.problems = []        # dicts: kind, severity, msg, file, line, when
        self.decodes = {}         # name -> dict
        self.buses = []
        self.tenants = {}         # decode -> [dict]
        self.trees = {}
        self.secondary = []
        self.irqs = []
        self.clocks = []
        self.pin_groups = []
        self.links = []
        self.alt_groups = []
        self.defaults = {}
        self.features = {}
        self._blocks()
        self._decodes()
        self._muxes()
        self._classify()
        self._alternatives()
        self._trees()
        self._irqs()
        self._clocks()
        self._pins()
        self._links()
        self._windows()
        self._defines()

    # -----------------------------------------------------------------
    def problem(self, kind, severity, msg, file=None, line=None, when=None):
        self.problems.append(dict(kind=kind, severity=severity, msg=msg,
                                  file=file, line=line,
                                  when=C.fmt(when) if when is not None else None))

    def block_of(self, inst):
        return self.by_inst.get((inst.name, inst.line))

    # -----------------------------------------------------------------
    def _blocks(self):
        design_modules = self.d.modules
        names = {}
        for inst in self.d.top.instances:
            names.setdefault(inst.name, []).append(inst)
        for inst in self.d.top.instances:
            bid = inst.name
            if len(names[inst.name]) > 1:
                bid = "%s:%s" % (inst.name, inst.module)
            b = Block(inst, bid, self.d)
            sub = self.d.modules.get(inst.module)
            if sub is not None:
                pnames = [p.name for p in sub.ports]
                groups = {}
                for p in sub.ports:
                    mm = SUFFIX.match(p.name)
                    if mm:
                        groups.setdefault(mm.group(1), set()).add(
                            mm.group(2) + "_" + mm.group(3))
                for pre, sfx in groups.items():
                    role = None
                    if "cyc_i" in sfx and "ack_o" in sfx:
                        role = "target"
                    elif "cyc_o" in sfx and "ack_i" in sfx:
                        role = "initiator"
                    if role is None:
                        continue
                    ports = [x for x in pnames if
                             (pre and x.startswith(pre)) or
                             (not pre and SUFFIX.match(x) and
                              SUFFIX.match(x).group(1) == "")]
                    b.ifaces[pre] = dict(role=role, ports=ports)
                for ci in sub.instances:
                    if ci.module in design_modules:
                        b.children.append((ci.module, ci.name, ci.cond))
            for pc in inst.params:
                b.params[str(pc.name)] = (V.expr_text(pc.expr), pc.cond)
            self.blocks.append(b)
            self.by_inst[(inst.name, inst.line)] = b

    def iface_of(self, b, port):
        for pre, i in b.ifaces.items():
            if port in i["ports"]:
                return pre
        return None

    # -----------------------------------------------------------------
    def _decodes(self):
        m = self.d.top
        for a in m.assigns:
            if a.kind not in ("wire", "assign"):
                continue
            tg = NETS.lhs_targets(a.lhs)
            if len(tg) != 1 or tg[0][1] is not None:
                continue
            rhs = strip_parens(a.rhs)
            terms = split_top(rhs, "&&")
            cmp_ = None
            excl = []
            ok = True
            for t in terms:
                t = strip_parens(t)
                if t and t[0].text == "!" and len(t) == 2 and t[1].kind == "id":
                    excl.append((t[1].text, C.conj(a.cond, t[0].cond)))
                    continue
                pc = parse_compare(t)
                if pc is None or cmp_ is not None:
                    ok = False
                    break
                cmp_ = pc
            if not ok or cmp_ is None:
                continue
            net, mask, val = cmp_
            if mask & 0xF0000000 != 0xF0000000:
                continue            # not an address-space decode
            name = tg[0][0]
            dec = self.decodes.setdefault(name, dict(name=name, net=net,
                                                     windows=[], line=a.line,
                                                     file=a.file))
            dec["windows"].append(dict(mask=mask, value=val, cond=a.cond,
                                       line=a.line, excludes=excl))
        # exclusions must name other decodes
        for dec in self.decodes.values():
            for w in dec["windows"]:
                w["excludes"] = [(x, c) for x, c in w["excludes"]
                                 if x in self.decodes]
            dec["cover"] = C.absorb([w["cond"] for w in dec["windows"]])
        nets = {}
        for dec in self.decodes.values():
            nets.setdefault(dec["net"], []).append(dec["name"])
        for net, ds in sorted(nets.items(), key=lambda x: -len(x[1])):
            self.buses.append(dict(net=net, decodes=sorted(ds)))
        self.main_bus = self.buses[0]["net"] if self.buses else None

    # -----------------------------------------------------------------
    def _muxes(self):
        m = self.d.top
        self.mux_terms = {}       # decode -> [dict(kind, cond, sig, line)]
        for a in m.assigns:
            if a.kind not in ("wire", "assign"):
                continue
            terms = split_top(strip_parens(a.rhs), "|")
            if len(terms) < 3:
                continue
            found = []
            for t in terms:
                t = strip_parens(t)
                if not t:
                    continue
                parts = split_top(t, "&")
                if len(parts) != 2:
                    continue
                dec = sig = None
                rep = False
                for p in parts:
                    ids = [x.text for x in V.expr_idents(p)]
                    ds = [x for x in ids if x in self.decodes]
                    if ds and dec is None:
                        dec = ds[0]
                        rep = any(x.text == "{" for x in p)
                    elif ids:
                        sig = ids[0]
                if dec and sig:
                    found.append((dec, sig, rep, C.conj(a.cond, *[x.cond for x in t]),
                                  t[0].line))
            if len(found) < 3:
                continue
            for dec, sig, rep, cnd, line in found:
                self.mux_terms.setdefault(dec, []).append(
                    dict(kind="dat" if rep else "ack", sig=sig, cond=cnd,
                         line=line, net=V.expr_text(a.lhs)))
        for name, dec in self.decodes.items():
            tenants = []
            for term in self.mux_terms.get(name, []):
                if term["kind"] != "ack":
                    continue
                for t, cover in merge_terms(self.n.back(term["sig"], term["cond"])):
                    for c in cover:
                        if t[0] == "inst":
                            b = self.block_of(t[1])
                            pre = self.iface_of(b, t[2])
                            tenants.append(dict(block=b, iface=pre, cond=c,
                                                inline=None, line=term["line"]))
                            b.roles.add("tenant")
                        else:
                            tenants.append(dict(block=None, iface=None, cond=c,
                                                inline=name, line=term["line"]))
            self.tenants[name] = tenants

    # -----------------------------------------------------------------
    def _addr_port(self, b, pre):
        for p in b.ifaces[pre]["ports"]:
            if p == pre + "adr_i" or p == pre + "adr_o":
                return p
        return None

    def _on_bus(self, b, pre):
        """Does this target interface's address come from a decoded bus?"""
        port = self._addr_port(b, pre)
        if port is None:
            return False
        for ident, c in self.n.conn_idents(b.inst, port):
            for t, _ in self.n.back(ident, c, stop=self._is_bus_net):
                if t[0] == "stop":
                    return True
            if self._is_bus_net(ident):
                return True
        return False

    def _is_bus_net(self, name):
        return any(name == bus["net"] for bus in self.buses)

    def up(self, ident, ctx, depth=0):
        """Alternatives [(cond, node)] for whatever drives an address net."""
        if depth > 10:
            return []
        out = []
        for t, cover in merge_terms(self.n.back(ident, ctx, stop=self._is_bus_net)):
            for c in cover:
                out.append((c, self._node(t, c, depth)))
        return [(c, nd) for c, nd in out if nd is not None]

    def _classify(self):
        """Arbiters and bridges, from interface shape alone: an instance
        with an initiator and N target interfaces whose addresses do not
        come from a decoded bus is an arbiter (N >= 2) or a bridge (N == 1)."""
        self.nonbus_targets = {}
        for b in self.blocks:
            tg = [p for p, i in sorted(b.ifaces.items()) if i["role"] == "target"
                  and not self._on_bus(b, p)]
            ini = [p for p, i in b.ifaces.items() if i["role"] == "initiator"]
            self.nonbus_targets[b.id] = tg
            if ini and len(tg) >= 2:
                b.roles.add("arbiter")
            elif ini and len(tg) == 1:
                b.roles.add("bridge")

    def _node(self, t, c, depth):
        if t[0] == "stop":
            return dict(kind="bus", net=t[1])
        if t[0] == "pin":
            return dict(kind="pin", name=t[1])
        if t[0] != "inst":
            return dict(kind="logic", name=t[1])
        b = self.block_of(t[1])
        pre = self.iface_of(b, t[2])
        if pre is not None and b.ifaces[pre]["role"] == "initiator":
            ins = self.nonbus_targets.get(b.id, [])
            if len(ins) >= 2:
                b.roles.add("arbiter")
                inputs = []
                for p in ins:
                    ap = self._addr_port(b, p)
                    alts = []
                    for ident, ic in self.n.conn_idents(b.inst, ap):
                        cc = C.overlap(c, ic)
                        if cc is not None:
                            alts += self.up(ident, cc, depth + 1)
                    inputs.append(dict(port=p, alts=alts))
                return dict(kind="arbiter", block=b, iface=pre, inputs=inputs)
            if len(ins) == 1:
                b.roles.add("bridge")
                ap = self._addr_port(b, ins[0])
                alts = []
                for ident, ic in self.n.conn_idents(b.inst, ap):
                    cc = C.overlap(c, ic)
                    if cc is not None:
                        alts += self.up(ident, cc, depth + 1)
                return dict(kind="bridge", block=b, iface=pre, port=ins[0],
                            alts=alts)
            b.roles.add("master")
            return dict(kind="master", block=b, iface=pre)
        if pre is None:
            # an address stage: follow its non-bus address inputs
            b.roles.add("stage")
            alts = []
            sub = self.d.modules.get(b.module)
            if sub is not None:
                for p in sub.ports:
                    if (p.dir == "input" and self.iface_of(b, p.name) is None
                            and re.search(r"adr|addr", p.name)):
                        for ident, ic in self.n.conn_idents(b.inst, p.name):
                            cc = C.overlap(c, ic)
                            if cc is not None:
                                alts += self.up(ident, cc, depth + 1)
            return dict(kind="stage", block=b, port=t[2], alts=alts)
        return dict(kind="logic", name="%s.%s" % (b.id, t[2]))

    def _trees(self):
        if self.main_bus:
            self.trees[self.main_bus] = self.up(self.main_bus, C.TRUE)
        # targets that are not reached through a decode: point-to-point
        # buses behind an arbiter or a bridge (the VRAM bus)
        for b in self.blocks:
            for pre, i in sorted(b.ifaces.items()):
                if i["role"] != "target":
                    continue
                if "arbiter" in b.roles or "bridge" in b.roles:
                    continue
                if self._on_bus(b, pre):
                    continue
                ap = self._addr_port(b, pre)
                alts = []
                for ident, ic in self.n.conn_idents(b.inst, ap):
                    alts += self.up(ident, ic)
                # only a target behind an arbiter or bridge is a bus of its
                # own; one addressed straight from a master is a decoded
                # tenant that happens to take the raw CPU address (the MTU)
                if any(nd["kind"] in ("arbiter", "bridge") for _, nd in alts):
                    b.roles.add("p2p")
                    self.secondary.append(dict(block=b, iface=pre, alts=alts,
                                               tree=self.fold(alts)))
        if self.main_bus:
            self.main_tree = self.fold(self.trees[self.main_bus])
        else:
            self.main_tree = None

    # -----------------------------------------------------------------
    # folding master trees for display
    #
    # A trace yields one alternative per condition, so an optional
    # arbiter shows up as "arbiter(inputs...) when GPU_BLIT" next to
    # "whatever is on input m0, when !GPU_BLIT". Folding recognises that
    # second alternative as the arbiter's own input and marks the arbiter
    # BYPASSABLE instead -- which is what the RTL's `else branch of pure
    # wiring means.
    # -----------------------------------------------------------------
    def sig(self, nd):
        k = nd["kind"]
        if k in ("master", "bridge", "stage", "arbiter"):
            b = nd["block"]
            key = b.alt_group or b.inst.name
            if k == "arbiter":
                return (k, key, tuple((i["port"], self.alts_sig(i["alts"]))
                                      for i in nd["inputs"]))
            if k in ("bridge", "stage"):
                return (k, key, self.alts_sig(nd["alts"]))
            return (k, key, nd.get("iface"))
        return (k, nd.get("name") or nd.get("net"))

    def alts_sig(self, alts):
        return tuple(sorted({repr(self.sig(n)) for _, n in alts}))

    def fold(self, alts):
        groups = []
        for c, nd in alts:
            sg = repr(self.sig(nd))
            for g in groups:
                if g["sig"] == sg:
                    g["items"].append((c, nd))
                    break
            else:
                groups.append(dict(sig=sg, items=[(c, nd)]))
        if not groups:
            return None
        if len(groups) == 1:
            return self._make(groups[0])

        def child_sigs(nd):
            if nd["kind"] == "arbiter":
                return {repr(self.sig(n)) for i in nd["inputs"] for _, n in i["alts"]}
            if nd["kind"] in ("bridge", "stage"):
                return {repr(self.sig(n)) for _, n in nd["alts"]}
            return set()
        for g in sorted(groups, key=lambda g: -len(g["sig"])):
            cs = child_sigs(g["items"][0][1])
            if all(h["sig"] in cs for h in groups if h is not g):
                f = self._make(g)
                f["bypass"] = True
                return f
        return dict(kind="alts", options=[self._make(g) for g in groups],
                    bypass=False)

    def _make(self, g):
        nd0 = g["items"][0][1]
        f = dict(kind=nd0["kind"], cover=C.absorb([c for c, _ in g["items"]]),
                 bypass=False, iface=nd0.get("iface"), port=nd0.get("port"),
                 name=nd0.get("name") or nd0.get("net"))
        blocks = []
        for _, nd in g["items"]:
            b = nd.get("block")
            if b is not None and b not in blocks:
                blocks.append(b)
        f["blocks"] = sorted(blocks, key=lambda b: b.line)
        if nd0["kind"] == "arbiter":
            ins = []
            for i0 in nd0["inputs"]:
                alts = []
                for _, nd in g["items"]:
                    for i in nd["inputs"]:
                        if i["port"] == i0["port"]:
                            alts += i["alts"]
                ins.append(dict(port=i0["port"], node=self.fold(alts)))
            f["inputs"] = ins
        elif nd0["kind"] in ("bridge", "stage"):
            alts = []
            for _, nd in g["items"]:
                alts += nd["alts"]
            f["up"] = self.fold(alts)
        return f

    # -----------------------------------------------------------------
    def _windows(self):
        """Register windows answered off the bus (a *BASE* parameter that
        lands inside a decoded window, e.g. the icache's CFG_BASE) and
        virtual windows declared by a module (the MTU's translate window)."""
        self.ghosts = []
        self.virtual = []
        for b in self.blocks:
            if "tenant" in b.roles:
                continue
            for pname, (txt, pc) in b.params.items():
                if "BASE" not in pname.upper():
                    continue
                v = V.parse_number(txt.replace(" ", ""))
                if v is None or v < 0x1000:
                    continue
                for dec in self.decodes.values():
                    for w in dec["windows"]:
                        if (v & w["mask"]) == w["value"]:
                            self.ghosts.append(dict(block=b, value=v, param=pname,
                                                    cond=C.conj(b.cond, pc),
                                                    decode=dec["name"]))
                            break
                    else:
                        continue
                    break
        for b in self.blocks:
            spec = self.h.mod(b.module).get("window")
            sub = self.d.modules.get(b.module)
            if not spec or sub is None:
                continue
            a = sub.params.get(spec[0])
            mk = sub.params.get(spec[1])
            if not a or not mk:
                continue
            av = V.parse_number(V.expr_text(a[0]).replace(" ", ""))
            mv = V.parse_number(V.expr_text(mk[0]).replace(" ", ""))
            if av is None or mv is None:
                continue
            self.virtual.append(dict(block=b, value=av, mask=mv, cond=b.cond))

    # -----------------------------------------------------------------
    def _irqs(self):
        m = self.d.top
        irq_nets = set()
        for inst in m.instances:
            for c in inst.conns:
                if isinstance(c.name, str) and c.name == "irq":
                    for t in V.expr_idents(c.expr):
                        irq_nets.add(t.text)
        for a in m.assigns:
            for name, slc in NETS.lhs_targets(a.lhs):
                if name not in irq_nets or not isinstance(slc, tuple):
                    continue
                if slc[0] != slc[1]:
                    continue
                srcs = []
                for t, cover in merge_terms(
                        sum([self.n.back(s, C.conj(a.cond, sc))
                             for s, sc in [(x.text, x.cond)
                                           for x in V.expr_idents(a.rhs)]], [])):
                    for c in cover:
                        if t[0] == "inst":
                            b = self.block_of(t[1])
                            srcs.append(dict(block=b, port=t[2], cond=c))
                            b.irqs.append(slc[0])
                        elif t[0] == "pin":
                            srcs.append(dict(pin=t[1], cond=c))
                        else:
                            srcs.append(dict(logic=t[1], kind=t[0], cond=c))
                sig = V.expr_text(a.rhs)
                self.irqs.append(dict(n=slc[0], net=name, signal=sig,
                                      cond=a.cond, line=a.line, file=a.file,
                                      sources=srcs))
        self.irqs.sort(key=lambda x: x["n"])

    # -----------------------------------------------------------------
    def _clocks(self):
        """A clock is whatever reaches an INPUT port whose name says clk
        on a module with RTL here (vendor primitives are not users)."""
        m = self.d.top
        users = {}
        for inst in m.instances:
            b = self.block_of(inst)
            if b.primitive:
                continue
            for cn in inst.conns:
                if not isinstance(cn.name, str) or not CLKNET.search(cn.name):
                    continue
                if self.d.port_dir(inst.module, cn.name) != "input":
                    continue
                for t in V.expr_idents(cn.expr):
                    ctx = C.conj(inst.cond, cn.cond, t.cond)
                    for src, c in self._canon_near(t.text, ctx):
                        ent = users.setdefault(src, dict(net=src, users=[]))
                        ent["users"].append(dict(block=b, port=cn.name, cond=c))
        for net, ent in users.items():
            srcs = []
            for t, cover in merge_terms(self.n.back(net, C.TRUE)):
                for c in cover:
                    if t[0] == "inst":
                        gb = self.block_of(t[1])
                        gb.roles.add("clkgen")
                        srcs.append(dict(block=gb, port=t[2], cond=c))
                    elif t[0] == "pin":
                        srcs.append(dict(pin=t[1], cond=c))
                    else:
                        srcs.append(dict(logic=t[1], cond=c))
            ent["sources"] = srcs
            ent["mhz"] = freq_of(net)
            if ent["mhz"] is None:
                fs = {freq_of(x.get("pin") or "") for x in srcs} - {None}
                if len(fs) == 1:
                    ent["mhz"] = fs.pop()
            ent["user_cover"] = C.absorb([u["cond"] for u in ent["users"]])
            self.clocks.append(ent)
        self.clocks.sort(key=lambda e: (-(e["mhz"] or 0), e["net"]))

    def _canon_near(self, ident, ctx, depth=0):
        """Resolve clock aliases (wbm_clk = sys_clk = clk48mhz) to the first
        net with a frequency in its name, or to where aliasing stops. A net
        assigned from alternative aliases resolves to each of them."""
        if depth > 8 or freq_of(ident):
            return [(ident, ctx)]
        drv = self.n.drivers.get(ident, [])
        if drv and all(x.kind in ("wire", "assign") and len(x.srcs) == 1
                       for x in drv):
            out = []
            for x in drv:
                nc = C.overlap(ctx, x.cond)
                if nc is not None:
                    out += self._canon_near(x.srcs[0][0], nc, depth + 1)
            return out
        return [(ident, ctx)]

    # -----------------------------------------------------------------
    def _pins(self):
        """Top-level ports, grouped by the block they belong to. A port
        belongs to a block if it is wired to one of its ports directly,
        or through ONE assign (the tri-state pattern GPIO uses). Anything
        further away is board-level glue and is listed as such."""
        m = self.d.top
        groups = {}
        clock_pins = {s.get("pin") for e in self.clocks for s in e["sources"]}
        for p in m.ports:
            hits = []
            for rd in self.n.readers.get(p.name, []):
                if rd.kind == "inst":
                    hits.append((rd.inst, rd.cond))
            for drv in self.n.drivers.get(p.name, []):
                if drv.kind == "inst":
                    hits.append((drv.inst, drv.cond))
            if not hits:
                for drv in self.n.drivers.get(p.name, []):
                    if drv.kind not in ("assign", "wire"):
                        continue
                    for s_, sc in drv.srcs:
                        for d2 in self.n.drivers.get(s_, []):
                            if d2.kind == "inst":
                                hits.append((d2.inst, C.conj(drv.cond, d2.cond)))
                for rd in self.n.readers.get(p.name, []):
                    if rd.kind != "assign":
                        continue
                    for tgt, _ in NETS.lhs_targets(rd.assign.lhs):
                        for r2 in self.n.readers.get(tgt, []):
                            if r2.kind == "inst":
                                hits.append((r2.inst, C.conj(rd.cond, r2.cond)))
            bs = []
            for inst, c in hits:
                b = self.block_of(inst)
                if b.primitive or "clkgen" in b.roles or b in bs:
                    continue
                bs.append(b)
            if p.name in clock_pins:
                kind = "clock"
            elif not self.n.readers.get(p.name) and p.dir == "input":
                kind = "unused"
            else:
                kind = "io"
            if not bs:
                bs = [None]
            for b in bs:
                gk = (b.id if b else None, C.fmt(p.cond), kind)
                g = groups.setdefault(gk, dict(block=b, cond=p.cond, ports=[],
                                               kind=kind))
                g["ports"].append(p)
        for g in groups.values():
            g["label"] = pin_label([x.name for x in g["ports"]])
            if g["block"] is not None:
                g["block"].pins.append(g)
            self.pin_groups.append(g)
        self.pin_groups.sort(key=lambda g: (g["block"].id if g["block"] else "~",
                                            g["label"]))

    # -----------------------------------------------------------------
    def _links(self):
        """Sideband: non-bus, non-clock outputs of one block reaching
        non-bus inputs of another."""
        m = self.d.top
        irq_nets = {i["net"] for i in self.irqs}
        acc = {}
        for inst in m.instances:
            a = self.block_of(inst)
            if a.primitive:
                continue
            for cn in inst.conns:
                if not isinstance(cn.name, str):
                    continue
                if self.d.port_dir(inst.module, cn.name) != "output":
                    continue
                if self.iface_of(a, cn.name) is not None:
                    continue
                if CLKNET.search(cn.name) or re.search(r"rst|reset|irq|int_o$|"
                                                        r"^locked$", cn.name):
                    continue
                for t in V.expr_idents(cn.expr):
                    ctx = C.conj(inst.cond, cn.cond, t.cond)
                    for tt, c in self.n.forward(t.text, ctx,
                                                stop=lambda x: x in irq_nets):
                        if tt[0] != "inst":
                            continue
                        b = self.block_of(tt[1])
                        if b is a or b.primitive:
                            continue
                        if self.iface_of(b, tt[2]) is not None:
                            continue
                        if a.module and "arbiter" in a.roles:
                            continue
                        k = (a.id, b.id)
                        ent = acc.setdefault(k, dict(src=a, dst=b, signals=[],
                                                     conds=[]))
                        sig = "%s>%s" % (cn.name, tt[2])
                        if sig not in ent["signals"]:
                            ent["signals"].append(sig)
                        ent["conds"].append(c)
        for ent in acc.values():
            ent["cover"] = C.absorb(ent["conds"])
            del ent["conds"]
            self.links.append(ent)
        self.links.sort(key=lambda e: (e["src"].id, e["dst"].id))

    # -----------------------------------------------------------------
    def _alternatives(self):
        """Blocks that fill the same role under mutually exclusive
        conditions: same instance name, or the same tenant slot."""
        groups = []
        byname = {}
        for b in self.blocks:
            byname.setdefault(b.inst.name, []).append(b)
        for name, bs in byname.items():
            if len(bs) > 1 and all(C.exclusive(x.cond, y.cond)
                                   for i, x in enumerate(bs)
                                   for y in bs[i + 1:]):
                groups.append(dict(kind="instance", key=name, blocks=bs))
        # tenants of one decode (or of decodes sharing a window) that
        # exclude each other
        for name, ts in self.tenants.items():
            bs = []
            for t in ts:
                if t["block"] is not None and t["block"] not in bs:
                    bs.append(t["block"])
            if len(bs) > 1:
                groups.append(dict(kind="slot", key=name, blocks=bs))
        # the same instance output driven from alternatives (e.g. uart0,
        # whose three candidates have two different instance names)
        merged = []
        for g in groups:
            for h in merged:
                if set(id(x) for x in g["blocks"]) & set(id(x) for x in h["blocks"]):
                    for x in g["blocks"]:
                        if x not in h["blocks"]:
                            h["blocks"].append(x)
                    break
            else:
                merged.append(dict(g))
        for i, g in enumerate(merged):
            g["id"] = "alt%d" % i
            for b in g["blocks"]:
                b.alt_group = g["id"]
        self.alt_groups = merged

    # -----------------------------------------------------------------
    def _defines(self):
        """Feature index: every define and what depends on it. A use under
        !X is recorded as such ('without X: ...')."""
        for s in self.d.top_unit.defines:
            if s.kind == "default":
                self.defaults.setdefault(s.name, (s.value, s.cond, s.file, s.line))
        idx = {}

        def note(c, what):
            for n, v in c:
                ent = idx.setdefault(n, {"with": [], "without": []})
                lst = ent["with" if v else "without"]
                if what not in lst:
                    lst.append(what)

        def own(c, parent):
            return frozenset(c) - frozenset(parent)
        top = self.d.top
        for b in self.blocks:
            note(b.cond, "block " + self.h.name(b))
            for pc in b.inst.params:
                note(own(pc.cond, b.cond), "%s.%s" % (b.inst.name, pc.name))
            sub = self.d.modules.get(b.module)
            if sub is not None:
                inner = set()
                for x in sub.decls + sub.assigns + sub.instances + sub.ports:
                    for lit in x.cond:
                        inner.add(lit)
                for lit in sorted(inner):
                    note(frozenset([lit]), "inside %s" % b.module)
        for dec in self.decodes.values():
            for w in dec["windows"]:
                note(w["cond"], "decode " + dec["name"])
        for g in self.pin_groups:
            note(g["cond"], "pins " + self.h.pins(g["label"]))
        for irq in self.irqs:
            note(irq["cond"], "IRQ %d" % irq["n"])
        def inner_lits(toks, base):
            lits = set()
            for t in toks:
                lits |= set(t.cond) - set(base)
            return lits
        for d in top.decls + self.d.top_unit.global_decls:
            if d.kind in ("localparam", "parameter"):
                note(d.cond, "localparam " + d.name)
                if d.init:
                    for lit in inner_lits(d.init, d.cond):
                        note(frozenset([lit]), "localparam " + d.name)
                    self._feature_bits(d, note)
        for a in top.assigns:
            lits = inner_lits(a.rhs, a.cond)
            if lits:
                tgt = ", ".join(sorted({x for x, _ in NETS.lhs_targets(a.lhs)}))
                for lit in lits:
                    note(frozenset([lit]), "value of " + tgt)
        macros = {}
        for inst in top.instances:
            for pc in inst.params:
                for t in pc.expr:
                    if t.kind == "macro":
                        macros.setdefault(t.text[1:], []).append(
                            "%s.%s" % (inst.name, pc.name))
        for a in top.assigns:
            for t in a.rhs:
                if t.kind == "macro":
                    macros.setdefault(t.text[1:], []).append(
                        "expression at line %d" % a.line)
        for name, uses in macros.items():
            ent = idx.setdefault(name, {"with": [], "without": []})
            for u in uses:
                if "value " + u not in ent["with"]:
                    ent["with"].append("value " + u)
        # bookkeeping defines (include guards) are not features
        for s in self.d.top_unit.defines:
            if s.kind == "guard" and s.name in idx:
                del idx[s.name]
        self.features = idx

    def _feature_bits(self, d, note):
        """localparam X = (32'h1 << N) | ... with each term under its own
        `ifdef: a feature bitmap software can read back."""
        for term in split_top(d.init, "|"):
            term = strip_parens(term)
            parts = split_top(term, "<<")
            if len(parts) != 2 or not term:
                continue
            n = strip_parens(parts[1])
            if len(n) == 1 and n[0].kind == "num":
                bit = V.parse_number(n[0].text)
                c = frozenset(term[0].cond) - frozenset(d.cond)
                if bit is not None and c:
                    note(c, "%s bit %d" % (d.name, bit))


def pin_label(names):
    """UART0_TX, UART0_RX -> UART0_*;  SRAM_D, SRAM1_CE -> SRAM_*."""
    if len(names) == 1:
        return names[0]
    pre = names[0]
    for n in names[1:]:
        k = 0
        while k < min(len(pre), len(n)) and pre[k] == n[k]:
            k += 1
        pre = pre[:k]
    if "_" in pre:
        pre = pre[:pre.rindex("_")]
    else:
        pre = re.sub(r"[0-9]+$", "", pre)
    return (pre or names[0]) + "_*"
