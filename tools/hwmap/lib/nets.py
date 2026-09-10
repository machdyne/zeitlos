#
# Zeitlos hwmap -- the top module as a graph of nets.
#
# Every identifier in rtl/sysctl.v gets a list of the things that drive
# it (instance output ports, assign statements, procedural assignments,
# top-level input ports) and the things that read it, each carrying its
# presence condition. Traces walk that graph and AND the conditions of
# every step together, so an answer like "wbm_adr is driven by the main
# arbiter, or -- when GPU_BLIT is absent -- straight from the cache" falls
# out of the walk rather than being written down anywhere.
#

from . import cond as C
from . import vparse as V


class Driver:
    __slots__ = ("kind", "inst", "port", "cond", "file", "line", "srcs",
                 "block", "slice", "assign", "pin")

    def __init__(self, kind, cond, file, line, inst=None, port=None,
                 srcs=(), block=None, slc=None, assign=None, pin=None):
        self.kind = kind          # inst | assign | wire | proc | pin
        self.inst, self.port, self.cond = inst, port, cond
        self.file, self.line = file, line
        self.srcs = list(srcs)    # [(ident, cond)]
        self.block, self.slice, self.assign, self.pin = block, slc, assign, pin


class Reader:
    __slots__ = ("kind", "inst", "port", "cond", "line", "assign")

    def __init__(self, kind, cond, line, inst=None, port=None, assign=None):
        self.kind, self.cond, self.line = kind, cond, line
        self.inst, self.port, self.assign = inst, port, assign


def lhs_targets(lhs):
    """[(name, slice)] for an assignment's left-hand side. slice is
    (hi, lo) for constant bit/part selects, None for the whole net."""
    out = []
    k = 0
    while k < len(lhs):
        t = lhs[k]
        if t.kind == "id" and t.text not in V.KEYWORDS:
            slc = None
            if k + 1 < len(lhs) and lhs[k + 1].text == "[":
                j = k + 2
                inner = []
                depth = 1
                while j < len(lhs) and depth:
                    if lhs[j].text == "[":
                        depth += 1
                    elif lhs[j].text == "]":
                        depth -= 1
                        if depth == 0:
                            break
                    inner.append(lhs[j])
                    j += 1
                nums = [V.parse_number(x.text) for x in inner
                        if x.kind == "num"]
                if len(inner) == 1 and nums and nums[0] is not None:
                    slc = (nums[0], nums[0])
                elif (len(inner) == 3 and inner[1].text == ":" and
                      len(nums) == 2 and None not in nums):
                    slc = (max(nums), min(nums))
                else:
                    slc = "?"
                k = j
            out.append((t.text, slc))
        k += 1
    return out


class Nets:
    def __init__(self, design):
        self.d = design
        m = design.top
        self.m = m
        self.drivers = {}
        self.readers = {}
        self.decls = {}
        self.ports = {p.name: p for p in m.ports}
        self.inst_by_key = {}
        self._build()

    def key(self, inst):
        return "%s@%d" % (inst.name, inst.line)

    def _add_driver(self, name, drv):
        self.drivers.setdefault(name, []).append(drv)

    def _add_reader(self, name, rd):
        self.readers.setdefault(name, []).append(rd)

    def _build(self):
        m = self.m
        for d in m.decls + self.d.top_unit.global_decls:
            self.decls.setdefault(d.name, []).append(d)
        for p in m.ports:
            if p.dir in ("input", "inout"):
                self._add_driver(p.name, Driver("pin", p.cond, p.file, p.line,
                                                pin=p.name))
            if p.dir in ("output", "inout"):
                self._add_reader(p.name, Reader("pin", p.cond, p.line))
        for a in m.assigns:
            srcs = [(t.text, C.conj(a.cond, t.cond))
                    for t in V.expr_idents(a.rhs)]
            for name, slc in lhs_targets(a.lhs):
                self._add_driver(name, Driver(a.kind, a.cond, a.file, a.line,
                                              srcs=srcs, block=a.block,
                                              slc=slc, assign=a))
            for s, c in srcs:
                self._add_reader(s, Reader("assign", c, a.line, assign=a))
        for inst in m.instances:
            self.inst_by_key[self.key(inst)] = inst
            for c in inst.conns:
                cc = C.conj(inst.cond, c.cond)
                dirn = self.d.port_dir(inst.module, c.name)
                ids = V.expr_idents(c.expr)
                if dirn in ("output", "inout"):
                    # an output connects to an lvalue: names, selects of
                    # names, or a concatenation of those
                    for nm, slc in lhs_targets(c.expr):
                        tc = cc
                        for t in ids:
                            if t.text == nm:
                                tc = C.conj(cc, t.cond)
                                break
                        self._add_driver(nm, Driver("inst", tc, inst.file,
                                                    c.line, inst=inst,
                                                    port=c.name, slc=slc))
                for t in ids:
                    tc = C.conj(cc, t.cond)
                    if dirn in ("input", "inout", None):
                        self._add_reader(t.text, Reader("inst", tc, c.line,
                                                        inst=inst,
                                                        port=c.name))

    # -----------------------------------------------------------------
    # traces
    # -----------------------------------------------------------------

    def back(self, name, ctx=C.TRUE, stop=None, depth=0, seen=None):
        """Walk drivers backwards from a net.

        Returns [(terminal, cond)] where terminal is one of
            ("inst", inst, port)
            ("pin", name)
            ("const", name)       every source is a literal
            ("undriven", name)
            ("stop", name)        stop(name) returned True
        and cond is the AND of every step's condition with ctx.
        """
        seen = set() if seen is None else seen
        out = []
        if not C.is_sat(ctx):
            return out
        if stop is not None and depth > 0 and stop(name):
            return [(("stop", name), ctx)]
        if (name, ctx) in seen or depth > 24:
            return out
        seen = seen | {(name, ctx)}
        drvs = self.drivers.get(name, [])
        if not drvs:
            return [(("undriven", name), ctx)]
        for drv in drvs:
            c = C.overlap(ctx, drv.cond)
            if c is None:
                continue
            if drv.kind == "inst":
                out.append((("inst", drv.inst, drv.port), c))
            elif drv.kind == "pin":
                out.append((("pin", drv.pin), c))
            else:
                srcs = [(s, sc) for s, sc in drv.srcs if s != name]
                if not srcs:
                    # a continuous assign of a literal is a tie-off; a
                    # register loaded with literals is logic (a timer, a
                    # state bit) whose behaviour lives in its always block
                    out.append((("logic" if drv.kind == "proc" else "const", name), c))
                    continue
                for s, sc in srcs:
                    if s in self.decls or s in self.drivers or s in self.ports:
                        cc = C.overlap(c, sc)
                        if cc is None:
                            continue
                        out.extend(self.back(s, cc, stop, depth + 1, seen))
        return out

    def forward(self, name, ctx=C.TRUE, depth=0, seen=None, stop=None):
        """Walk readers forwards: [(("inst", inst, port) | ("pin", name) |
        ("stop", name), cond)]."""
        seen = set() if seen is None else seen
        out = []
        if (name, ctx) in seen or depth > 12:
            return out
        seen = seen | {(name, ctx)}
        if stop is not None and depth > 0 and stop(name):
            return [(("stop", name), ctx)]
        for rd in self.readers.get(name, []):
            c = C.overlap(ctx, rd.cond)
            if c is None:
                continue
            if rd.kind == "inst":
                out.append((("inst", rd.inst, rd.port), c))
            elif rd.kind == "pin":
                out.append((("pin", name), c))
            else:
                a = rd.assign
                ac = C.overlap(c, a.cond)
                if ac is None:
                    continue
                for tgt, _ in lhs_targets(a.lhs):
                    if tgt == name:
                        continue
                    out.extend(self.forward(tgt, ac, depth + 1, seen, stop))
        return out

    def conn(self, inst, port):
        for c in inst.conns:
            if c.name == port:
                return c
        return None

    def conn_idents(self, inst, port):
        c = self.conn(inst, port)
        if c is None:
            return []
        return [(t.text, C.conj(inst.cond, c.cond, t.cond))
                for t in V.expr_idents(c.expr)]

    def decl_cover(self, name):
        cs = [d.cond for d in self.decls.get(name, [])]
        cs += [p.cond for p in self.m.ports if p.name == name]
        return cs
